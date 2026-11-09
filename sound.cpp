// TPMExtractor fork - TPM's voice files (INDYWV), decoded to WAV.
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

} // namespace tpmx
