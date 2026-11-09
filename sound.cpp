// TPMExtractor fork - TPM's voice files (INDYWV) and music files (MCMP with an iMUS map), decoded
// to WAV, and the music file's map as text.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "sound.h"

#include "binreader.h"
#include "log.h"
#include "platform.h"
#include "textout.h"
#include "vima.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace tpmx {

namespace {

const char kWvMagic[] = "INDYWV";
const size_t kWvMagicBytes = 6;
const size_t kWvSyncField = 0x16;
const size_t kWvSyncStart = 0x1A;
const char kMcmpMagic[] = "MCMP";
const size_t kMcmpEntryBytes = 9;
const uint32_t kImusFirstEvent = 16;   // the map's events start here in the stream
const uint32_t kImusHeaderExtra = 24;  // "iMUS" size "MAP " size + "DATA" size around the map

uint16_t be16(BinReader& r)
{
    const uint8_t* p = r.span(2);
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t be32_at(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t be32(BinReader& r) { return be32_at(r.span(4)); }

void put_u16(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    put_u16(v, x & 0xFFFF);
    put_u16(v, x >> 16);
}

void put_tag(std::vector<uint8_t>& v, const char* tag) { v.insert(v.end(), tag, tag + 4); }

std::string channels_text(int channels) { return channels == 1 ? "mono" : channels == 2 ? "stereo" : "?"; }

// The decoded stream holds `tag` at `pos` of `data` (inside the reader's bytes).
bool tag_at(const BinReader& r, size_t pos, const char* tag)
{
    BinReader probe = r;
    if (pos > r.size())
        return false;
    probe.seek(pos);
    return probe.peek_tag(tag);
}

} // namespace

// ------------------------------------------------------------------------------------ WAV

std::vector<uint8_t> wav_header(uint32_t rate, int channels, uint32_t data_bytes)
{
    const uint32_t align = (uint32_t)channels * 2;
    std::vector<uint8_t> h;
    h.reserve(kWavHeaderBytes);
    put_tag(h, "RIFF");
    put_u32(h, data_bytes + 36);
    put_tag(h, "WAVE");
    put_tag(h, "fmt ");
    put_u32(h, 16);
    put_u16(h, 1);  // PCM
    put_u16(h, (uint32_t)channels);
    put_u32(h, rate);
    put_u32(h, rate * align);
    put_u16(h, align);
    put_u16(h, 16);
    put_tag(h, "data");
    put_u32(h, data_bytes);
    return h;
}

bool write_wav(const std::string& path, uint32_t rate, int channels, const std::vector<int16_t>& samples,
               std::string* err)
{
    const uint64_t data_bytes = (uint64_t)samples.size() * 2;
    if (data_bytes > 0xFFFFFFFFull - 36) {
        if (err)
            *err = path + ": too much audio for a WAV file";
        return false;
    }
    const std::vector<uint8_t> head = wav_header(rate, channels, (uint32_t)data_bytes);
    FILE* f = platform::open_write(path);
    if (!f) {
        if (err)
            *err = path + ": cannot create file: " + platform::errno_text();
        return false;
    }
    bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size();
    std::vector<uint8_t> buf;
    const size_t chunk = 32768;
    for (size_t i = 0; ok && i < samples.size(); i += chunk) {
        const size_t n = samples.size() - i < chunk ? samples.size() - i : chunk;
        buf.resize(n * 2);
        for (size_t k = 0; k < n; k++) {
            const uint16_t v = (uint16_t)samples[i + k];
            buf[2 * k] = (uint8_t)(v & 0xFF);
            buf[2 * k + 1] = (uint8_t)(v >> 8);
        }
        ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    }
    if (std::fclose(f) != 0)
        ok = false;
    if (!ok) {
        if (err)
            *err = path + ": write error";
        platform::remove_file(path);
    }
    return ok;
}

std::string sound_output_name(const std::string& file_name, const char* suffix)
{
    const size_t dot = file_name.rfind('.');
    return (dot == std::string::npos ? file_name : file_name.substr(0, dot)) + suffix;
}

std::string sound_printable(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '\\') {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------- voice

WvSound wv_decode(const uint8_t* data, size_t size, const std::string& name, uint64_t base, const std::string& what)
{
    BinReader r(data, size, name, base);
    if (!r.peek_tag(kWvMagic))
        r.fail("not an INDYWV voice file (no \"INDYWV\" signature)");
    r.skip(kWvMagicBytes);
    WvSound s;
    s.rate = r.u32();
    const uint32_t bits = r.u32();
    const uint32_t channels = r.u32();
    const uint32_t data_size = r.u32();
    const uint32_t sync = r.u32();
    if (s.rate == 0 || s.rate > kMaxSampleRate)
        r.fail_at(6, format("sample rate %u is outside 1..%u", s.rate, kMaxSampleRate));
    if (bits != 16)
        r.fail_at(0xA, format("%u bits per sample: the voice codec writes 16", bits));
    if (channels != 1 && channels != 2)
        r.fail_at(0xE, format("%u channels (1 or 2 expected)", channels));
    s.channels = (int)channels;
    if (sync > r.remaining())
        r.fail_at(kWvSyncField, format("the sync block (%u bytes) runs past the end of the file (%llu bytes)", sync,
                                       (unsigned long long)size));
    r.skip(sync);
    const size_t count_at = r.pos();
    s.pcm_bytes = r.u32();
    if ((uint64_t)kWvSyncStart + sync + data_size != (uint64_t)size)
        log::warn_at(name, base + 0x12,
                     "%sdata size %u does not match the %llu bytes after the header (the game does not use it)\n",
                     what.c_str(), data_size, (unsigned long long)(size - kWvSyncStart - sync));
    if (sync)
        log::verbose("%s @0x%llX: %sthe %u-byte sync block is not written to the WAV\n", name.c_str(),
                     (unsigned long long)(base + kWvSyncStart), what.c_str(), sync);
    const VimaHeader h = vima_read_header(r, VimaPredictor::Quirk);
    if (h.channels != s.channels)
        r.fail_at((size_t)(h.offset - base), format("the coded stream is %s, the header says %s",
                                                    channels_text(h.channels).c_str(),
                                                    channels_text(s.channels).c_str()));
    s.wvsm = h.wvsm;
    const uint64_t frame_bytes = 2 * (uint64_t)s.channels;
    if (s.pcm_bytes % frame_bytes)
        log::warn_at(name, base + count_at,
                     "%sthe PCM size %u is not a whole number of %s frames: the last %u byte(s) are not written\n",
                     what.c_str(), s.pcm_bytes, channels_text(s.channels).c_str(),
                     (unsigned)(s.pcm_bytes % frame_bytes));
    const uint64_t asked = vima_samples_requested(h, s.pcm_bytes);
    const uint64_t room = vima_max_samples(h, r.remaining());
    if (asked > room)
        r.fail_at(count_at, format("the PCM size %u asks for %llu samples; the %llu bytes of codes hold at most %llu",
                                   s.pcm_bytes, (unsigned long long)asked, (unsigned long long)r.remaining(),
                                   (unsigned long long)room));
    const VimaResult res = vima_decode_body(r, h, s.pcm_bytes, s.samples);
    s.escapes = res.escapes;
    s.clamps = res.clamps;
    return s;
}

bool wv_write_wav(const uint8_t* data, size_t size, const std::string& name, uint64_t base, const std::string& what,
                  const std::string& out_path)
{
    WvSound s;
    try {
        s = wv_decode(data, size, name, base, what);
    } catch (const ParseError& e) {
        log::error_at(e.file(), e.offset(), "%s%s\n", what.c_str(), e.message().c_str());
        return false;
    }
    std::string why;
    if (!write_wav(out_path, s.rate, s.channels, s.samples, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    log::verbose("%s: %s%s %u Hz, %llu frames (%s, %u escapes, %u clamps)\n", out_path.c_str(), what.c_str(),
                 channels_text(s.channels).c_str(), s.rate,
                 (unsigned long long)(s.samples.size() / (size_t)s.channels), s.wvsm ? "WVSM" : "ADPCM", s.escapes,
                 s.clamps);
    return true;
}

namespace {

// A sound input read whole: refuses files larger than kMaxSoundFileSize.
bool read_sound_file(const std::string& path, std::vector<uint8_t>& data)
{
    uint64_t size = 0;
    if (!platform::file_size(path, size)) {
        log::error("%s: cannot open: not a readable file\n", path.c_str());
        return false;
    }
    if (size > kMaxSoundFileSize) {
        log::error("%s: %llu bytes, more than the %llu bytes a sound file may have here\n", path.c_str(),
                   (unsigned long long)size, (unsigned long long)kMaxSoundFileSize);
        return false;
    }
    std::string why;
    if (!platform::read_file(path, data, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    return true;
}

} // namespace

bool process_wv(const std::string& wv_path, const std::string& file_name, const std::string& out_dir,
                const Options& opts)
{
    (void)opts;
    std::vector<uint8_t> data;
    if (!read_sound_file(wv_path, data))
        return false;
    std::string out, why;
    if (!platform::safe_output_path(out_dir, sound_output_name(file_name, ".wav"), out, &why)) {
        log::error("%s: %s\n", wv_path.c_str(), why.c_str());
        return false;
    }
    return wv_write_wav(data.data(), data.size(), wv_path, 0, std::string(), out);
}

// ---------------------------------------------------------------------------------- music

namespace {

struct McmpBlock
{
    uint64_t entry_offset = 0;   // the block's 9-byte entry
    bool vima = false;
    uint32_t decoded = 0;
    uint32_t stored = 0;
    uint64_t stored_offset = 0;  // file offset of the stored bytes
};

const ImusEvent* find_event(const std::vector<ImusEvent>& events, const char* tag, uint32_t pos)
{
    for (const ImusEvent& e : events)
        if (e.tag == tag && e.pos == pos)
            return &e;
    return nullptr;
}

// Reads the map events of the iMUS header in `hr` (the raw header bytes, file offsets kept),
// from kImusFirstEvent to `map_end`, walking each event as the game does.
std::vector<ImusEvent> read_events(BinReader& hr, size_t map_end)
{
    std::vector<ImusEvent> events;
    hr.seek(kImusFirstEvent);
    while (hr.pos() < map_end) {
        const size_t at = hr.pos();
        if (map_end - at < 8)
            hr.fail_at(at, format("an event header (8 bytes) runs past the end of the map (%llu bytes left)",
                                  (unsigned long long)(map_end - at)));
        ImusEvent e;
        e.offset = hr.file_offset();
        e.tag = hr.fixed_string(4);
        const uint32_t size = be32(hr);
        const size_t body = hr.pos();
        const size_t left = map_end - body;
        int words_needed = 0;
        if (e.tag == "TEXT") {
            if (left < 5)
                hr.fail_at(at, "a TEXT event runs past the end of the map");
            e.pos = be32(hr);
            const uint8_t* p = hr.span(0);
            const void* nul = std::memchr(p, 0, map_end - hr.pos());
            if (!nul)
                hr.fail_at(at, "the text of a TEXT event is not terminated inside the map");
            const size_t len = (size_t)(static_cast<const uint8_t*>(nul) - p);
            e.text.assign(reinterpret_cast<const char*>(p), len);
            hr.skip(len + 1);  // walked by the text's NUL, as the game does, not by the size field
            events.push_back(e);
            continue;
        }
        if (e.tag == "FRMT")
            words_needed = 5;
        else if (e.tag == "REGN")
            words_needed = 2;
        else if (e.tag == "JUMP")
            words_needed = 4;
        else if (e.tag == "STOP" || e.tag == "SYNC")
            words_needed = 1;
        else
            hr.fail_at(at, format("unknown map event \"%s\"", sound_printable(e.tag).c_str()));
        const uint64_t words = size >> 2;
        if (words < (uint64_t)words_needed)
            hr.fail_at(at, format("a %u-byte %s event, at least %d bytes expected", size, e.tag.c_str(),
                                  4 * words_needed));
        if (words * 4 > left)
            hr.fail_at(at, format("the %s event (%u bytes) runs past the end of the map", e.tag.c_str(), size));
        e.pos = be32(hr);
        for (int k = 1; k < words_needed; k++)
            e.values[k - 1] = be32(hr);
        if (e.tag == "SYNC")
            e.data_bytes = (uint32_t)(words * 4 - 4);
        hr.seek(body + (size_t)(words * 4));  // the game converts size / 4 words
        events.push_back(e);
    }
    return events;
}

// The event checks (see sound.h); the first event is FRMT at the start of the audio.
void check_events(BinReader& hr, ImcTrack& t, const std::string& name)
{
    const std::vector<ImusEvent>& ev = t.events;
    const uint64_t base = hr.file_offset() - hr.pos();  // file offset of the header's first byte
    auto fail = [&](const ImusEvent& e, const std::string& msg) {
        hr.fail_at((size_t)(e.offset - base), msg);
    };
    if (ev.empty() || ev[0].tag != "FRMT" || ev[0].pos != t.header_bytes)
        hr.fail_at(kImusFirstEvent, format("the first map event is not FRMT at the start of the audio (byte %u)",
                                           t.header_bytes));
    const ImusEvent& f = ev[0];
    if (f.values[1] != 16)
        fail(f, format("FRMT: %u bits per sample, 16 expected", f.values[1]));
    if (f.values[2] == 0 || f.values[2] > kMaxSampleRate)
        fail(f, format("FRMT: sample rate %u is outside 1..%u", f.values[2], kMaxSampleRate));
    if (f.values[3] != 1 && f.values[3] != 2)
        fail(f, format("FRMT: %u channels, 1 or 2 expected", f.values[3]));
    t.rate = f.values[2];
    t.channels = (int)f.values[3];
    t.frame_bytes = 2 * t.channels;
    if (f.values[0] == 0)
        log::warn_at(name, f.offset,
                     "FRMT: byte-order flag 0, which makes the game play the samples byte-swapped; written as "
                     "decoded\n");
    const uint64_t start = t.header_bytes;
    const uint64_t end = start + (uint64_t)t.data_bytes;
    const uint32_t fb = (uint32_t)t.frame_bytes;
    uint32_t prev = 0;
    for (size_t i = 0; i < ev.size(); i++) {
        const ImusEvent& e = ev[i];
        if (e.pos < start || e.pos > end)
            fail(e, format("%s at byte %u of the stream, outside the audio (bytes %llu..%llu)", e.tag.c_str(), e.pos,
                           (unsigned long long)start, (unsigned long long)end));
        if (i && e.pos < prev)
            fail(e, format("%s at byte %u comes before the previous event (byte %u)", e.tag.c_str(), e.pos, prev));
        if ((e.pos - start) % fb)
            fail(e, format("%s at byte %u is not on a %u-byte frame boundary of the audio", e.tag.c_str(), e.pos, fb));
        prev = e.pos;
        if (e.tag == "FRMT" && i && (e.values[1] != f.values[1] || e.values[2] != f.values[2] ||
                                     e.values[3] != f.values[3]))
            fail(e, "a second FRMT with another format");
        if (e.tag == "REGN" && (e.values[0] % fb || e.pos + (uint64_t)e.values[0] > end))
            fail(e, format("REGN of %u bytes at byte %u: not whole frames, or past the end of the audio", e.values[0],
                           e.pos));
        if (e.tag == "JUMP" && (e.values[0] < start || e.values[0] >= end || (e.values[0] - start) % fb))
            fail(e, format("JUMP to byte %u: outside the audio or not on a frame boundary", e.values[0]));
    }
    bool has_lp = false;
    for (const ImusEvent& e : ev)
        if (e.tag == "TEXT" && e.text == "lp")
            has_lp = true;
    for (const ImusEvent& e : ev) {
        if (e.tag != "JUMP")
            continue;
        if (!find_event(ev, "REGN", e.values[0]))
            fail(e, format("JUMP to byte %u, where no region starts", e.values[0]));
        if (has_lp) {
            bool on_lp = false;
            for (const ImusEvent& m : ev)
                if (m.tag == "TEXT" && m.text == "lp" && m.pos == e.values[0])
                    on_lp = true;
            if (!on_lp)
                fail(e, format("JUMP to byte %u, but the loop point (lp) is elsewhere", e.values[0]));
        }
    }
}

} // namespace

ImcTrack imc_decode(const uint8_t* data, size_t size, const std::string& name)
{
    BinReader r(data, size, name);
    if (!r.peek_tag(kMcmpMagic))
        r.fail("not an MCMP music file (no \"MCMP\" signature)");
    r.skip(4);
    const uint32_t n = be16(r);
    if (n == 0)
        r.fail_at(4, "the file has no blocks");
    if ((uint64_t)6 + (uint64_t)n * kMcmpEntryBytes + 2 > size)
        r.fail_at(4, format("the table of %u blocks runs past the end of the file (%llu bytes)", n,
                            (unsigned long long)size));
    const size_t names_at = 6 + (size_t)n * kMcmpEntryBytes;
    r.seek(names_at);
    const uint32_t names_size = be16(r);
    if ((uint64_t)names_at + 2 + names_size > size)
        r.fail_at(names_at, format("the codec name table (%u bytes) runs past the end of the file", names_size));
    std::vector<std::string> codec_names;
    {
        const char* p = reinterpret_cast<const char*>(r.span(names_size));
        size_t i = 0;
        while (i < names_size) {
            const void* nul = std::memchr(p + i, 0, names_size - i);
            if (!nul)
                r.fail_at(names_at + 2 + i, "a codec name is not terminated inside the name table");
            const size_t len = (size_t)(static_cast<const char*>(nul) - (p + i));
            codec_names.push_back(std::string(p + i, len));
            i += len + 1;
        }
    }
    const uint64_t data0 = (uint64_t)names_at + 2 + names_size;
    std::vector<McmpBlock> blocks(n);
    uint64_t stored_at = data0;
    r.seek(6);
    for (uint32_t i = 0; i < n; i++) {
        McmpBlock& b = blocks[i];
        b.entry_offset = r.file_offset();
        const uint8_t codec = r.u8();
        b.decoded = be32(r);
        b.stored = be32(r);
        if (codec >= codec_names.size())
            r.fail_at((size_t)b.entry_offset, format("block %u: codec %u, the name table has %u name(s)", i, codec,
                                                     (unsigned)codec_names.size()));
        const std::string& cn = codec_names[codec];
        if (cn == "VIMA")
            b.vima = true;
        else if (cn != "NULL" && cn != "NONE")
            r.fail_at((size_t)b.entry_offset,
                      format("block %u: codec \"%s\" (VIMA, NULL or NONE expected)", i, sound_printable(cn).c_str()));
        b.stored_offset = stored_at;
        if (stored_at + b.stored > size)
            r.fail_at((size_t)b.entry_offset,
                      format("block %u (%u bytes stored at 0x%llX) runs past the end of the file (%llu bytes)", i,
                             b.stored, (unsigned long long)stored_at, (unsigned long long)size));
        stored_at += b.stored;
    }
    if (stored_at != size)
        r.fail_at((size_t)stored_at, format("%llu bytes after the last block (the block sizes add up to %llu bytes)",
                                            (unsigned long long)(size - stored_at), (unsigned long long)stored_at));
    uint32_t header_bytes = 0;
    uint64_t audio_bytes = 0;
    bool seen_vima = false;
    for (uint32_t i = 0; i < n; i++) {
        const McmpBlock& b = blocks[i];
        if (b.vima) {
            seen_vima = true;
            audio_bytes += b.decoded;
            continue;
        }
        if (seen_vima)
            r.fail_at((size_t)b.entry_offset, format("block %u is stored raw after VIMA blocks", i));
        if (b.decoded != b.stored)
            r.fail_at((size_t)b.entry_offset, format("block %u is stored raw (%u bytes) but decodes to %u bytes", i,
                                                     b.stored, b.decoded));
        header_bytes += b.stored;  // stored blocks are back to back: the header is contiguous
    }
    if (header_bytes == 0)
        r.fail_at(6, "the first block does not hold the iMUS header (it is not stored raw)");

    // The iMUS header: the raw blocks, which lie one after another from data0.
    ImcTrack t;
    t.blocks = n;
    BinReader hr(data + data0, header_bytes, name, data0);
    if (!hr.peek_tag("iMUS"))
        hr.fail("no \"iMUS\" header in the raw blocks");
    if (header_bytes < kImusFirstEvent)
        hr.fail_at(header_bytes, format("the iMUS header ends after %u bytes, before its map", header_bytes));
    if (!tag_at(hr, 8, "MAP "))
        hr.fail_at(8, "no \"MAP \" after the iMUS header");
    hr.seek(12);
    const uint32_t map_size = be32(hr);
    if (map_size > kImusMaxMapBytes)
        hr.fail_at(12, format("a map of %u bytes; the game takes at most %u", map_size, kImusMaxMapBytes));
    if ((uint64_t)map_size + kImusHeaderExtra > header_bytes)
        hr.fail_at(12, format("the map (%u bytes) and the DATA chunk run past the %u bytes of the raw blocks", map_size,
                              header_bytes));
    const size_t data_at = kImusFirstEvent + (size_t)map_size;
    if (!tag_at(hr, data_at, "DATA"))
        hr.fail_at(data_at, "no \"DATA\" chunk after the map");
    hr.seek(data_at + 4);
    t.data_bytes = be32(hr);
    t.header_bytes = map_size + kImusHeaderExtra;
    if (t.header_bytes != header_bytes)
        hr.fail_at(data_at, format("the audio starts at byte %u of the stream, but the raw blocks hold %u bytes",
                                   t.header_bytes, header_bytes));
    t.events = read_events(hr, data_at);
    check_events(hr, t, name);
    if (t.data_bytes % (uint32_t)t.frame_bytes)
        hr.fail_at(data_at + 4, format("DATA: %u bytes, not a whole number of %d-byte frames", t.data_bytes,
                                       t.frame_bytes));
    if (audio_bytes != t.data_bytes)
        hr.fail_at(data_at + 4, format("DATA: %u bytes, but the VIMA blocks decode to %llu", t.data_bytes,
                                       (unsigned long long)audio_bytes));

    // The audio, block by block (every VIMA block starts with its own stream header). Nothing is
    // reserved up front: a block's decoded size is trusted only after its capacity check.
    std::vector<int16_t> part;
    for (uint32_t i = 0; i < n; i++) {
        const McmpBlock& b = blocks[i];
        if (!b.vima)
            continue;
        BinReader br(data + b.stored_offset, b.stored, name, b.stored_offset);
        const VimaHeader h = vima_read_header(br, VimaPredictor::Clean);
        if (h.channels != t.channels)
            br.fail_at(0, format("block %u is %s, the FRMT says %s", i, channels_text(h.channels).c_str(),
                                 channels_text(t.channels).c_str()));
        if (b.decoded % (uint32_t)t.frame_bytes)
            r.fail_at((size_t)b.entry_offset, format("block %u decodes to %u bytes, not a whole number of %d-byte "
                                                     "frames", i, b.decoded, t.frame_bytes));
        const uint64_t asked = vima_samples_requested(h, b.decoded);
        const uint64_t room = vima_max_samples(h, br.remaining());
        if (asked > room)
            r.fail_at((size_t)b.entry_offset,
                      format("block %u: %llu bytes of codes cannot hold the %llu samples of its %u decoded bytes", i,
                             (unsigned long long)br.remaining(), (unsigned long long)asked, b.decoded));
        const VimaResult res = vima_decode_body(br, h, b.decoded, part);
        t.samples.insert(t.samples.end(), part.begin(), part.end());
        t.escapes += res.escapes;
        t.clamps += res.clamps;
    }
    return t;
}

std::string imc_map_text(const ImcTrack& t, const std::string& file_label)
{
    const uint32_t fb = (uint32_t)t.frame_bytes;
    auto frame = [&](uint32_t pos) { return (pos - t.header_bytes) / fb; };
    std::string s = format("# iMUSE map of %s, written by TPMExtractor (fork)\n", sound_printable(file_label).c_str());
    s += "# Positions are frames of the audio: (byte position in the decoded stream - header bytes) / frame bytes.\n";
    s += format("format\tframes=%u\trate=%u\tchannels=%d\tbits=16\tframe_bytes=%d\theader_bytes=%u\n",
                t.data_bytes / fb, t.rate, t.channels, t.frame_bytes, t.header_bytes);
    s += "# event\tframe\tvalues\n";
    for (const ImusEvent& e : t.events) {
        s += format("%s\t%u", e.tag.c_str(), frame(e.pos));
        if (e.tag == "FRMT")
            s += format("\tflag=%u\tbits=%u\trate=%u\tchannels=%u", e.values[0], e.values[1], e.values[2], e.values[3]);
        else if (e.tag == "TEXT")
            s += "\ttext=\"" + sound_printable(e.text) + "\"";
        else if (e.tag == "REGN")
            s += format("\tframes=%u", e.values[0] / fb);
        else if (e.tag == "JUMP")
            s += format("\tdest=%u\thook=%u\tfade_ms=%u", frame(e.values[0]), e.values[1], e.values[2]);
        else if (e.tag == "SYNC")
            s += format("\tbytes=%u", e.data_bytes);
        s += "\n";
    }
    return s;
}

bool process_imc(const std::string& imc_path, const std::string& file_name, const std::string& out_dir,
                 const Options& opts)
{
    (void)opts;
    std::vector<uint8_t> data;
    if (!read_sound_file(imc_path, data))
        return false;
    std::string wav_path, map_path, why;
    if (!platform::safe_output_path(out_dir, sound_output_name(file_name, ".wav"), wav_path, &why) ||
        !platform::safe_output_path(out_dir, sound_output_name(file_name, ".imap.txt"), map_path, &why)) {
        log::error("%s: %s\n", imc_path.c_str(), why.c_str());
        return false;
    }
    ImcTrack t;
    try {
        t = imc_decode(data.data(), data.size(), imc_path);
    } catch (const ParseError& e) {
        log::error("%s\n", e.what());
        return false;
    }
    if (!write_wav(wav_path, t.rate, t.channels, t.samples, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    TextWriter w;
    if (!w.open(map_path, &why)) {
        log::error("%s\n", why.c_str());
        platform::remove_file(wav_path);
        return false;
    }
    w.write(imc_map_text(t, file_name));
    if (!w.close(&why)) {
        log::error("%s\n", why.c_str());
        platform::remove_file(map_path);
        platform::remove_file(wav_path);
        return false;
    }
    log::verbose("%s: %s %u Hz, %llu frames in %u blocks, %zu map events (%u escapes, %u clamps)\n",
                 imc_path.c_str(), channels_text(t.channels).c_str(), t.rate,
                 (unsigned long long)(t.samples.size() / (size_t)t.channels), t.blocks, t.events.size(), t.escapes,
                 t.clamps);
    return true;
}

} // namespace tpmx
