// TPMExtractor fork - the VIMA ADPCM codec of TPM's voice files and music blocks.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Written from a description of the game's two decoders made by static analysis; both were
// checked bit for bit against an independent decoder on every voice file and music track of a
// retail copy. The tables are the ones both decoders hold (the step table is IMA ADPCM's).

#include "vima.h"

#include "textout.h"

#include <array>
#include <cstring>

namespace tpmx {

const int16_t kVimaStep[kVimaSteps] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// Indices 0..44 code 4 bits, 45..58 5 bits, 59..73 6 bits, 74..88 7 bits (sign bit included).
const uint8_t kVimaWidth[kVimaSteps] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

namespace {

// Index changes per code magnitude, for code widths 2..7 (only 4..7 occur through kVimaWidth).
const int8_t kAdjust2[] = {-1, 4};
const int8_t kAdjust3[] = {-1, -1, 2, 6};
const int8_t kAdjust4[] = {-1, -1, -1, -1, 1, 2, 4, 6};
const int8_t kAdjust5[] = {-1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 2, 2, 4, 5, 6};
const int8_t kAdjust6[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                           1,  1,  1,  1,  1,  2,  2,  2,  2,  4,  4,  4,  5,  5,  6,  6};
const int8_t kAdjust7[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                           -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                           1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,
                           2,  2,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6};
const int8_t* const kAdjust[8] = {nullptr, nullptr, kAdjust2, kAdjust3, kAdjust4, kAdjust5, kAdjust6, kAdjust7};

static_assert(sizeof kAdjust2 == 2 && sizeof kAdjust3 == 4 && sizeof kAdjust4 == 8 && sizeof kAdjust5 == 16 &&
                  sizeof kAdjust6 == 32 && sizeof kAdjust7 == 64,
              "one entry per code magnitude");

struct Tables
{
    uint16_t delta[kVimaSteps * 64];  // vima_delta_table
    int32_t step_delta[kVimaSteps][64];  // vima_delta per (index, magnitude)
    uint8_t next_index[kVimaSteps][64];  // index after a non-escape code, clamped to 0..88
};

void fill_tables(Tables& t)
{
    std::memset(&t, 0, sizeof t);
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < kVimaSteps; j++) {
            int s = kVimaStep[j];
            int acc = 0;
            for (int m = 0x20; m; m >>= 1) {
                if (i & m)
                    acc += s;
                s >>= 1;  // the steps are positive: an arithmetic and a logical shift agree
            }
            t.delta[j * 64 + i] = (uint16_t)(acc & 0xFFFF);
        }
    }
    for (int idx = 0; idx < kVimaSteps; idx++) {
        const int w = kVimaWidth[idx];
        const int count = 1 << (w - 1);
        for (int m = 0; m < count; m++) {
            int d = t.delta[idx * 64 + (m << (7 - w))];
            if (m)
                d += kVimaStep[idx] >> (w - 1);
            t.step_delta[idx][m] = d;
            int next = idx + kAdjust[w][m];
            t.next_index[idx][m] = (uint8_t)(next <= 0 ? 0 : (next >= kVimaMaxIndex ? kVimaMaxIndex : next));
        }
    }
}

// Built once, on first use (the game's decoders build their delta table the same way).
const Tables& tables()
{
    static Tables t;
    static const bool filled = (fill_tables(t), true);
    (void)filled;
    return t;
}

// A 16-bit value as the signed sample it is.
inline int16_t to_s16(uint32_t v)
{
    v &= 0xFFFFu;
    return (int16_t)(v >= 0x8000u ? (int32_t)v - 0x10000 : (int32_t)v);
}

// The game's 16-bit MSB-first window: `used` bits of its top are consumed; one byte is shifted
// in whenever more than 7 are. Bytes past the end of the data read as zero (the decoders look up
// to two bytes ahead of the bits they use).
class Window
{
public:
    Window(const uint8_t* data, size_t size) : data_(data), size_(size)
    {
        win_ = (uint32_t)next() << 8;
        win_ |= next();
    }

    // The next `w` bits (w <= 7).
    uint32_t take(int w)
    {
        used_ += w;
        const uint32_t code = (win_ >> (16 - used_)) & ((1u << w) - 1);
        if (used_ > 7) {
            used_ -= 8;
            win_ = ((win_ << 8) | next()) & 0xFFFFu;
        }
        return code;
    }

    // The next 16 bits (an escaped sample), as the game reads them.
    uint32_t take16()
    {
        const uint32_t hi = (win_ << used_) & 0xFF00u;
        win_ = ((win_ << 8) | next()) & 0xFFFFu;
        const uint32_t lo = (win_ >> (8 - used_)) & 0xFFu;
        win_ = ((win_ << 8) | next()) & 0xFFFFu;
        return hi | lo;
    }

private:
    uint8_t next()
    {
        const uint8_t b = pos_ < size_ ? data_[pos_] : 0;
        pos_++;
        return b;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t win_ = 0;
    int used_ = 0;
};

VimaResult decode_adpcm(BinReader& r, const VimaHeader& h, uint64_t frames, std::vector<int16_t>& out)
{
    const Tables& t = tables();
    const size_t avail = r.remaining();
    const uint64_t body_offset = r.file_offset();
    const uint64_t avail_bits = (uint64_t)avail * 8;
    const uint8_t* data = r.span(0);  // a pointer to r's position (no bytes taken yet)
    const int nch = h.channels;
    out.assign((size_t)(frames * (uint64_t)nch), 0);

    VimaResult res;
    Window win(data, avail);
    uint64_t bits = 0;
    bool short_data = false;
    uint64_t short_sample = 0;  // the first sample (counted over all channels) the data cannot hold
    int short_channel = 0;
    uint64_t sample_count = 0;
    for (int c = 0; c < nch; c++) {
        int index = h.index[c];
        int pred = h.predictor[c];
        size_t o = (size_t)c;
        for (uint64_t k = 0; k < frames; k++, sample_count++) {
            const int w = kVimaWidth[index];
            const uint32_t sign = 1u << (w - 1);
            const uint32_t mask = sign - 1;
            const uint32_t code = win.take(w);
            bits += (uint64_t)w;
            const uint32_t magnitude = code & mask;
            if (magnitude == mask) {
                pred = to_s16(win.take16());  // the code's sign bit is ignored
                bits += 16;
                res.escapes++;
                index = t.next_index[index][magnitude];  // +6 for every width
            } else {
                const int d = t.step_delta[index][magnitude];
                if (code & sign) {
                    pred -= d;
                    if (pred < -32768) {
                        pred = -32768;
                        res.clamps++;
                    }
                } else {
                    pred += d;
                    if (pred > 32767) {
                        pred = 32767;
                        res.clamps++;
                    }
                }
                index = t.next_index[index][magnitude];
            }
            if (!short_data && bits > avail_bits) {
                short_data = true;
                short_sample = sample_count;
                short_channel = c;
            }
            out[o] = (int16_t)pred;
            o += (size_t)nch;
        }
    }
    res.code_bits = bits;
    res.coded_bytes = (size_t)((bits + 7) / 8);
    if (short_data)
        throw ParseError(r.name(), body_offset + avail,
                         format("the coded data ends inside sample %llu of %llu (channel %d): the codes need %llu "
                                "bits, the data has %llu",
                                (unsigned long long)(short_sample - (uint64_t)short_channel * frames),
                                (unsigned long long)frames, short_channel, (unsigned long long)bits,
                                (unsigned long long)avail_bits));
    r.skip(res.coded_bytes);
    return res;
}

VimaResult decode_wvsm(BinReader& r, uint64_t pcm_bytes, uint64_t frames, std::vector<int16_t>& out)
{
    // As the game: blocks of 0x1000 output bytes; per block a u16 (not used), a byte of two
    // shifts (high nibble for even samples, low nibble for odd ones), then per sample one byte
    // c: 0x80 escapes a big-endian 16-bit sample, anything else is (signed) c << shift. The loop
    // writes two samples per pass, so a block of an odd sample count writes one more.
    const size_t avail = r.remaining();
    const uint64_t body_offset = r.file_offset();
    const uint8_t* data = r.span(0);
    const size_t keep = (size_t)(frames * 2);
    out.assign(keep, 0);
    VimaResult res;
    size_t p = 0;
    auto byte = [&]() -> uint8_t {
        if (p >= avail)
            throw ParseError(r.name(), body_offset + avail, "the WVSM data ends before the samples asked for");
        return data[p++];
    };
    uint64_t left = pcm_bytes;
    size_t block_start = 0;  // output sample index of the block
    while (left) {
        const uint64_t n = left > kVimaWvsmBlockBytes ? kVimaWvsmBlockBytes : left;
        left -= n;
        const uint64_t half = (n & ~(uint64_t)1) >> 1;
        byte();
        byte();
        const uint8_t shifts = byte();
        const int shift[2] = {shifts >> 4, shifts & 15};
        size_t o = block_start;
        for (uint64_t i = 0; i < half; i += 2) {
            for (int s = 0; s < 2; s++, o++) {
                const uint8_t c = byte();
                int16_t v;
                if (c == 0x80) {
                    const uint32_t b0 = byte();
                    const uint32_t b1 = byte();
                    v = to_s16((b0 << 8) + b1);
                    res.escapes++;
                } else {
                    v = to_s16((uint32_t)((int32_t)(int8_t)c * (int32_t)(1 << shift[s])));
                }
                if (o < keep)
                    out[o] = v;
            }
        }
        block_start += kVimaWvsmBlockBytes / 2;
    }
    res.coded_bytes = p;
    r.skip(p);
    return res;
}

} // namespace

int vima_index_adjust(int width, int magnitude)
{
    if (width < 2 || width > 7 || magnitude < 0 || magnitude >= (1 << (width - 1)))
        return 0;
    return kAdjust[width][magnitude];
}

uint16_t vima_delta_table(int index, int i6)
{
    if (index < 0 || index >= kVimaSteps || i6 < 0 || i6 >= 64)
        return 0;
    return tables().delta[index * 64 + i6];
}

int vima_delta(int index, int width, int magnitude)
{
    if (index < 0 || index >= kVimaSteps || width < 2 || width > 7 || magnitude < 0 ||
        magnitude >= (1 << (width - 1)))
        return 0;
    int d = tables().delta[index * 64 + (magnitude << (7 - width))];
    if (magnitude)
        d += kVimaStep[index] >> (width - 1);
    return d;
}

int vima_header_predictor(uint8_t b0, uint8_t b1, VimaPredictor p)
{
    uint32_t v = ((uint32_t)b0 << 8) | b1;
    if (p == VimaPredictor::Quirk && b1 >= 0x80)
        v |= 0xFF00u;  // (b0 << 8) | (signed char)b1
    return to_s16(v);
}

VimaHeader vima_read_header(BinReader& r, VimaPredictor p)
{
    VimaHeader h;
    h.offset = r.file_offset();
    const size_t start = r.pos();
    const uint8_t b = r.u8();
    h.channels = vima_stream_channels(b);
    const int index0 = h.channels == 2 ? (int)(uint8_t)~b : (int)b;
    uint8_t b0 = r.u8();
    uint8_t b1 = r.u8();
    h.index[0] = index0;
    h.predictor[0] = vima_header_predictor(b0, b1, p);
    uint8_t raw_index1 = 0;
    if (h.channels == 2) {
        raw_index1 = r.u8();
        b0 = r.u8();
        b1 = r.u8();
        h.index[1] = (int)(int8_t)raw_index1;  // read back sign-extended by the game
        h.predictor[1] = vima_header_predictor(b0, b1, p);
        if (h.predictor[0] == 0x1111 && raw_index1 == 0x64 && h.predictor[1] == 0x2222 && r.peek_tag("WVSM")) {
            r.skip(4);
            h.wvsm = true;
        }
    }
    h.size = r.pos() - start;
    if (!h.wvsm) {
        for (int c = 0; c < h.channels; c++) {
            if (h.index[c] < 0 || h.index[c] > kVimaMaxIndex)
                r.fail_at(c == 0 ? start : start + 3,
                          format("step index %d of channel %d is outside 0..%d", h.index[c], c, kVimaMaxIndex));
        }
    }
    return h;
}

uint64_t vima_frames(const VimaHeader& h, uint64_t pcm_bytes) { return pcm_bytes / (2 * (uint64_t)h.channels); }

uint64_t vima_samples_requested(const VimaHeader& h, uint64_t pcm_bytes)
{
    return h.wvsm ? pcm_bytes / 2 : vima_frames(h, pcm_bytes) * (uint64_t)h.channels;
}

uint64_t vima_max_samples(const VimaHeader& h, uint64_t coded_bytes) { return h.wvsm ? coded_bytes : coded_bytes * 2; }

VimaResult vima_decode_body(BinReader& r, const VimaHeader& h, uint64_t pcm_bytes, std::vector<int16_t>& out)
{
    const uint64_t frames = vima_frames(h, pcm_bytes);
    if (vima_samples_requested(h, pcm_bytes) > vima_max_samples(h, r.remaining()))
        r.fail_at(r.size(), format("%llu bytes of codes cannot hold the %llu samples asked for",
                                   (unsigned long long)r.remaining(),
                                   (unsigned long long)vima_samples_requested(h, pcm_bytes)));
    return h.wvsm ? decode_wvsm(r, pcm_bytes, frames, out) : decode_adpcm(r, h, frames, out);
}

} // namespace tpmx
