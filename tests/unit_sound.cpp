// TPMExtractor fork - unit tests of the sound code: the VIMA tables (against closed forms computed
// here), the bit reader and the decoder on hand-made streams (escapes, the rounding term, the
// index changes, clamping, stereo channel order, the two header predictors, WVSM blocks, read-ahead
// at the end of the data), the error offsets of voice files, the WAV header bytes and output names.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Generic data only. Exit 0 = pass, 1 = fail.
#include "binreader.h"
#include "log.h"
#include "sound.h"
#include "vima.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tpmx;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_fail;                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

// MSB-first bits, as the codes are stored.
struct Bits
{
    std::vector<uint8_t> out;
    int n = 0;
    void put(uint32_t v, int count)
    {
        for (int i = count - 1; i >= 0; i--) {
            if (n % 8 == 0)
                out.push_back(0);
            if ((v >> i) & 1)
                out.back() |= (uint8_t)(0x80 >> (n % 8));
            n++;
        }
    }
};

// The offset of the ParseError `fn` throws, or -1 when it throws none.
template <typename F> static long long error_offset(F fn)
{
    try {
        fn();
    } catch (const ParseError& e) {
        return (long long)e.offset();
    }
    return -1;
}

// ----------------------------------------------------------------------------------- tables

static void test_tables()
{
    // the IMA ADPCM step table: 7 ... 32767, each step about 1.1 times the previous one
    CHECK(kVimaStep[0] == 7 && kVimaStep[44] == 494 && kVimaStep[88] == 32767);
    for (int i = 1; i < kVimaSteps; i++)
        CHECK(kVimaStep[i] > kVimaStep[i - 1] && kVimaStep[i] * 10 <= kVimaStep[i - 1] * 12 + 10);
    for (int i = 0; i < kVimaSteps; i++)
        CHECK(kVimaWidth[i] == (i <= 44 ? 4 : i <= 58 ? 5 : i <= 73 ? 6 : 7));
    // index changes: the lower half of the magnitudes -1, the escape (all ones) +6 (+4 at width 2)
    for (int w = 2; w <= 7; w++) {
        const int n = 1 << (w - 1);
        for (int m = 0; m < n / 2; m++)
            CHECK(vima_index_adjust(w, m) == -1);
        CHECK(vima_index_adjust(w, n - 1) == (w == 2 ? 4 : 6));
        for (int m = n / 2; m + 1 < n; m++)
            CHECK(vima_index_adjust(w, m) >= 1 && vima_index_adjust(w, m) <= vima_index_adjust(w, m + 1));
    }
    CHECK(vima_index_adjust(1, 0) == 0 && vima_index_adjust(4, 8) == 0);
    // the delta table against its definition, and the delta against the closed form
    //   d = sum over the w-1 magnitude bits, MSB first, of step >> k, plus step >> (w-1) for m != 0
    for (int idx = 0; idx < kVimaSteps; idx++) {
        for (int i6 = 0; i6 < 64; i6++) {
            int acc = 0;
            for (int k = 0; k < 6; k++)
                if (i6 & (0x20 >> k))
                    acc += kVimaStep[idx] >> k;
            CHECK(vima_delta_table(idx, i6) == acc);
        }
        const int w = kVimaWidth[idx];
        for (int m = 0; m < (1 << (w - 1)) - 1; m++) {
            int d = 0;
            for (int k = 0; k < w - 1; k++)
                if ((m >> (w - 2 - k)) & 1)
                    d += kVimaStep[idx] >> k;
            if (m)
                d += kVimaStep[idx] >> (w - 1);
            CHECK(vima_delta(idx, w, m) == d);
        }
    }
    CHECK(vima_delta_table(88, 63) == 64506);  // the largest entry: read back unsigned
    CHECK(vima_delta(0, 4, 0) == 0 && vima_delta(0, 4, 1) == 1 + 0 && vima_delta(0, 4, 4) == 7 + 0);
}

// ------------------------------------------------------------------------------ predictors

static void test_predictors()
{
    // the voice decoder's quirk: a second byte of 0x80 or more makes the high byte 0xFF
    CHECK(vima_header_predictor(0x12, 0x9A, VimaPredictor::Quirk) == -102);
    CHECK(vima_header_predictor(0x12, 0x9A, VimaPredictor::Clean) == 0x129A);
    CHECK(vima_header_predictor(0x7F, 0xFF, VimaPredictor::Quirk) == -1);
    CHECK(vima_header_predictor(0x7F, 0xFF, VimaPredictor::Clean) == 32767);
    CHECK(vima_header_predictor(0x80, 0x00, VimaPredictor::Quirk) == -32768);
    CHECK(vima_header_predictor(0x80, 0x00, VimaPredictor::Clean) == -32768);
    CHECK(vima_header_predictor(0x11, 0x11, VimaPredictor::Quirk) == 0x1111);
}

// A mono stream with the given header bytes and codes, decoded for `pcm` output bytes.
static std::vector<int16_t> decode(const std::vector<uint8_t>& stream, uint32_t pcm, VimaPredictor p,
                                   VimaResult* res = nullptr, VimaHeader* hdr = nullptr)
{
    BinReader r(stream, "stream");
    const VimaHeader h = vima_read_header(r, p);
    std::vector<int16_t> out;
    const VimaResult v = vima_decode_body(r, h, pcm, out);
    if (res)
        *res = v;
    if (hdr)
        *hdr = h;
    return out;
}

static void test_decoder()
{
    // index 0 (4-bit codes), predictor 0: an escape to 0x1234 (+6 -> index 6), then +m1 (index 5),
    // then -m4 (index 6), then m0 (no rounding term; index 5)
    {
        Bits b;
        b.put(0x7, 4);
        b.put(0x1234, 16);
        b.put(0x1, 4);
        b.put(0x8 | 0x4, 4);
        b.put(0x0, 4);
        std::vector<uint8_t> s = {0x00, 0x00, 0x00};
        s.insert(s.end(), b.out.begin(), b.out.end());
        VimaResult res;
        const std::vector<int16_t> out = decode(s, 8, VimaPredictor::Clean, &res);
        const int d1 = (kVimaStep[6] >> 2) + (kVimaStep[6] >> 3);  // magnitude 1 at index 6
        const int d2 = kVimaStep[5] + (kVimaStep[5] >> 3);        // magnitude 4 at index 5
        CHECK(out.size() == 4);
        CHECK(out[0] == 0x1234 && out[1] == 0x1234 + d1 && out[2] == 0x1234 + d1 - d2 && out[3] == out[2]);
        CHECK(res.escapes == 1 && res.clamps == 0 && res.code_bits == 32 && res.coded_bytes == 4);
    }
    // clamping at both ends: start at 32767 (index 88, 7-bit codes) and add; start at -32768 and subtract
    {
        Bits b;
        b.put(0x05, 7);
        std::vector<uint8_t> s = {88, 0x7F, 0xFF};
        s.insert(s.end(), b.out.begin(), b.out.end());
        VimaResult res;
        std::vector<int16_t> out = decode(s, 2, VimaPredictor::Clean, &res);
        CHECK(out.size() == 1 && out[0] == 32767 && res.clamps == 1);
        Bits c;
        c.put(0x40 | 0x05, 7);
        s = {88, 0x80, 0x00};
        s.insert(s.end(), c.out.begin(), c.out.end());
        out = decode(s, 2, VimaPredictor::Clean, &res);
        CHECK(out.size() == 1 && out[0] == -32768 && res.clamps == 1);
        // the same stream read with the voice decoder's predictor starts elsewhere (0x7FFF -> -1)
        Bits d;
        d.put(0x00, 7);
        s = {88, 0x7F, 0xFF};
        s.insert(s.end(), d.out.begin(), d.out.end());
        CHECK(decode(s, 2, VimaPredictor::Quirk)[0] == -1);
    }
    // the escape ignores its sign bit; stereo streams code all of channel 0, then channel 1, and
    // interleave the output; channel 1's index byte is not complemented
    {
        Bits b;
        b.put(0xF, 4);       // channel 0 sample 0: escape with the sign bit set
        b.put(0x0100, 16);
        b.put(0x0, 4);       // channel 0 sample 1: m0 at index 6
        b.put(0x2, 4);       // channel 1 sample 0: +m2 at index 3
        b.put(0xA, 4);       // channel 1 sample 1: -m2 at index 2
        std::vector<uint8_t> s = {0xFF, 0x00, 0x00, 3, 0x00, 0x10};  // 0xFF: stereo, index ~0xFF = 0
        s.insert(s.end(), b.out.begin(), b.out.end());
        VimaHeader h;
        const std::vector<int16_t> out = decode(s, 8, VimaPredictor::Clean, nullptr, &h);
        CHECK(h.channels == 2 && h.index[0] == 0 && h.index[1] == 3 && h.predictor[1] == 16 && h.size == 6);
        const int d3 = (kVimaStep[3] >> 1) + (kVimaStep[3] >> 3);
        const int d2 = (kVimaStep[2] >> 1) + (kVimaStep[2] >> 3);
        CHECK(out.size() == 4 && out[0] == 0x100 && out[2] == 0x100 && out[1] == 16 + d3 && out[3] == 16 + d3 - d2);
    }
    // codes that end on a byte boundary: the reader looks two bytes ahead, which is not an error;
    // one bit more than the data holds is, at the end of the data
    {
        std::vector<uint8_t> s = {0, 0, 0, 0x01, 0x23};  // four 4-bit codes, 16 bits exactly
        CHECK(error_offset([&] { decode(s, 8, VimaPredictor::Clean); }) == -1);
        CHECK(error_offset([&] { decode(s, 10, VimaPredictor::Clean); }) == 5);
        std::vector<uint8_t> e = {0, 0, 0, 0x70};  // an escape whose 16 bits are missing
        CHECK(error_offset([&] { decode(e, 2, VimaPredictor::Clean); }) == 4);
    }
    // header errors: a short header, a step index outside 0..88 (at its byte, also channel 1's)
    {
        std::vector<uint8_t> s = {0, 0};
        CHECK(error_offset([&] { decode(s, 0, VimaPredictor::Clean); }) == 2);
        s = {89, 0, 0, 0};
        CHECK(error_offset([&] { decode(s, 2, VimaPredictor::Clean); }) == 0);
        s = {0xFF, 0, 0, 0x80, 0, 0, 0};
        CHECK(error_offset([&] { decode(s, 4, VimaPredictor::Clean); }) == 3);
        s = {0, 0, 0};
        CHECK(error_offset([&] { decode(s, 0, VimaPredictor::Clean); }) == -1);  // no samples, no codes
    }
}

static void test_wvsm()
{
    // the signature, then one block for 8 output bytes: u16, shifts (2 for even, 1 for odd samples),
    // 5 << 2, -1 << 1, an escaped 0x1234, 127 << 1
    std::vector<uint8_t> s = {0xFF, 0x11, 0x11, 0x64, 0x22, 0x22, 'W', 'V', 'S', 'M', 8, 0, 0x21,
                              0x05, 0xFF, 0x80, 0x12, 0x34, 0x7F};
    VimaHeader h;
    VimaResult res;
    std::vector<int16_t> out = decode(s, 8, VimaPredictor::Quirk, &res, &h);
    CHECK(h.wvsm && h.channels == 2 && h.size == 10);
    CHECK(out.size() == 4 && out[0] == 20 && out[1] == -2 && out[2] == 0x1234 && out[3] == 254);
    CHECK(res.escapes == 1 && res.coded_bytes == 9);
    // 6 output bytes: one whole frame kept, although the block's loop reads two pairs
    out = decode(s, 6, VimaPredictor::Quirk);
    CHECK(out.size() == 2 && out[0] == 20 && out[1] == -2);
    // a missing byte is an error at the end of the data
    std::vector<uint8_t> cut(s.begin(), s.end() - 1);
    CHECK(error_offset([&] { decode(cut, 8, VimaPredictor::Quirk); }) == (long long)cut.size());
    // with channel 1's index 100 but no "WVSM" it is an ADPCM header with a bad index
    std::vector<uint8_t> no = {0xFF, 0x11, 0x11, 0x64, 0x22, 0x22, 'W', 'V', 'S', 'X'};
    CHECK(error_offset([&] { decode(no, 4, VimaPredictor::Quirk); }) == 3);
}

// ------------------------------------------------------------------------------- WAV, names

static void test_wav()
{
    const uint8_t mono[44] = {'R', 'I', 'F', 'F', 136, 0, 0, 0,   'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                              16,  0,   0,   0,   1,   0, 1, 0,   0x22, 0x56, 0, 0, 0x44, 0xAC, 0, 0,
                              2,   0,   16,  0,   'd', 'a', 't', 'a', 100, 0, 0, 0};
    const std::vector<uint8_t> h = wav_header(22050, 1, 100);
    CHECK(h.size() == kWavHeaderBytes && std::memcmp(h.data(), mono, 44) == 0);
    const std::vector<uint8_t> st = wav_header(11025, 2, 0x10000);
    CHECK(st.size() == 44 && st[22] == 2 && st[24] == 0x11 && st[25] == 0x2B && st[28] == 0x44 && st[29] == 0xAC &&
          st[32] == 4 && st[40] == 0 && st[42] == 1 && st[4] == 36 && st[6] == 1);
    CHECK(sound_output_name("LINE1.WV", ".wav") == "LINE1.wav");
    CHECK(sound_output_name("a.b.imc", ".imap.txt") == "a.b.imap.txt");
    CHECK(sound_output_name("noext", ".wav") == "noext.wav");
}

// -------------------------------------------------------------------------------- voice file

static std::vector<uint8_t> voice_file(const std::vector<uint8_t>& stream, uint32_t pcm, uint32_t channels = 1)
{
    std::vector<uint8_t> f = {'I', 'N', 'D', 'Y', 'W', 'V'};
    const uint32_t fields[5] = {22050, 16, channels, (uint32_t)(4 + stream.size()), 0};
    for (uint32_t v : fields)
        for (int k = 0; k < 4; k++)
            f.push_back((uint8_t)(v >> (8 * k)));
    for (int k = 0; k < 4; k++)
        f.push_back((uint8_t)(pcm >> (8 * k)));
    f.insert(f.end(), stream.begin(), stream.end());
    return f;
}

static void test_voice()
{
    const std::vector<uint8_t> stream = {0x00, 0x00, 0x00, 0x01, 0x23};
    std::vector<uint8_t> f = voice_file(stream, 8);
    const WvSound s = wv_decode(f.data(), f.size(), "v.wv", 0, "");
    CHECK(s.rate == 22050 && s.channels == 1 && s.samples.size() == 4 && !s.wvsm);
    // errors name absolute offsets: inside a LAB the file starts at its entry's offset
    const uint64_t base = 0x1000;
    std::vector<uint8_t> bad = f;
    bad[2] = 'X';
    CHECK(error_offset([&] { wv_decode(bad.data(), bad.size(), "x.lab", base, "entry 0: "); }) == 0x1000);
    bad = f;
    bad[0xA] = 8;  // 8 bits per sample
    CHECK(error_offset([&] { wv_decode(bad.data(), bad.size(), "x.lab", base, ""); }) == 0x100A);
    bad = f;
    bad[0x1A] = 0xFF;  // 255 PCM bytes: 127 samples, more than 2 bytes of codes hold
    CHECK(error_offset([&] { wv_decode(bad.data(), bad.size(), "x.lab", base, ""); }) == 0x101A);
    bad = f;
    bad[0xE] = 2;  // a mono stream in a stereo file: at the stream header
    CHECK(error_offset([&] { wv_decode(bad.data(), bad.size(), "x.lab", base, ""); }) == 0x101E);
    std::vector<uint8_t> cut(f.begin(), f.begin() + 0x18);
    CHECK(error_offset([&] { wv_decode(cut.data(), cut.size(), "x.lab", base, ""); }) == 0x1016);
}

int main()
{
    log::set_verbosity(-2);
    test_tables();
    test_predictors();
    test_decoder();
    test_wvsm();
    test_wav();
    test_voice();
    std::printf("unit_sound: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
