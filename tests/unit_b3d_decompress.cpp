// TPMExtractor fork - unit tests for the level (.B3D) LZSS decoder.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Without arguments: known-answer checks of tpmx::lzss_decode and tpmx::b3d_decompress on
// hand-assembled bit streams (literals, copies, overlapping copies, copies from the unwritten
// part of the ring, ring wrap-around, the end marker at every bit phase, truncated tokens with
// and without the original decoder's quirks, the output limit) and a file round trip through
// b3d_decompress_file in TPMX_TEST_TMP. Exit 0 = pass, 1 = fail.
//
// With "--decompress IN OUT": runs b3d_decompress_file(IN, OUT) and exits 0 if it succeeded,
// 1 if not (OUT is still written for a truncated stream, as the original wrote it). The test
// runner (tests/run_tests.py) uses this for its differential against an independent decoder
// and, when available, against a build of the original decoder.

#include "b3d_decompress.h"
#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tpmx;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what, int line)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

typedef std::vector<uint8_t> Bytes;

// Bits written most significant first, packed into bytes (the level stream's order).
class Bits
{
public:
    void put(uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; i--) {
            cur_ = (uint8_t)((cur_ << 1) | ((value >> i) & 1));
            if (++n_ == 8) {
                out_.push_back(cur_);
                cur_ = 0;
                n_ = 0;
            }
        }
    }
    void literal(uint8_t c) { put(1, 1); put(c, 8); }
    void copy(uint32_t offset, uint32_t length)  // copies length bytes (2..17) from ring[offset]
    {
        put(0, 1);
        put(offset, 12);
        put(length - 2, 4);
    }
    void end() { put(0, 13); }
    size_t bit_count() const { return out_.size() * 8 + (size_t)n_; }
    Bytes bytes() const  // the last partial byte is padded with zero bits
    {
        Bytes b = out_;
        if (n_)
            b.push_back((uint8_t)(cur_ << (8 - n_)));
        return b;
    }

private:
    Bytes out_;
    uint8_t cur_ = 0;
    int n_ = 0;
};

Bytes decode(const Bytes& in, LzssResult& r, bool quirks, size_t max_output = (size_t)1 << 24)
{
    Bytes out;
    r = lzss_decode(in.data(), in.size(), out, max_output, quirks);
    return out;
}

Bytes str(const char* s) { return Bytes(s, s + std::strlen(s)); }

void test_literals_and_copies()
{
    // "AB", then a copy of 2 bytes from ring position 1 (where "A" was written): "ABAB".
    Bits b;
    b.literal('A');
    b.literal('B');
    b.copy(1, 2);
    b.end();
    LzssResult r;
    const Bytes out = decode(b.bytes(), r, false);
    CHECK(out == str("ABAB"));
    CHECK(r.end == LzssEnd::Marker);
    CHECK(r.produced == 4);
    CHECK(r.consumed == b.bytes().size());

    // An overlapping copy: one literal at ring position 1, then 17 bytes from position 1 while
    // they are being written (a run of 18 equal bytes).
    Bits c;
    c.literal('a');
    c.copy(1, 17);
    c.literal('z');
    c.end();
    const Bytes run = decode(c.bytes(), r, false);
    CHECK(run.size() == 19);
    bool all_a = run.size() == 19 && run[18] == 'z';
    for (size_t i = 0; i + 1 < run.size(); i++)
        all_a = all_a && run[i] == 'a';
    CHECK(all_a);

    // Copies from ring positions that were never written give zeros (the ring starts zeroed);
    // ring position 0 is never written by the first 4,095 bytes.
    Bits z;
    z.literal(7);
    z.copy(4000, 5);
    z.copy(2, 3);  // positions 2.. hold the zeros just copied
    z.end();
    const Bytes zeros = decode(z.bytes(), r, false);
    CHECK(zeros == (Bytes{7, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(r.end == LzssEnd::Marker);
}

void test_ring_wrap()
{
    // 4,100 literals wrap the 4,096-byte ring (write position starts at 1): byte k of the
    // output sits at ring[(k + 1) & 0xFFF], so ring[5] now holds output byte 4,100 - 1 - ...;
    // copy 4 bytes from ring position 2 and compare with what was written there last.
    Bits b;
    Bytes expect;
    for (int k = 0; k < 4100; k++) {
        const uint8_t c = (uint8_t)((k * 37 + 11) & 0xFF);
        b.literal(c);
        expect.push_back(c);
    }
    // ring[p] holds the last output byte k with (k + 1) % 4096 == p
    Bytes ring(4096, 0);
    for (size_t k = 0; k < expect.size(); k++)
        ring[(k + 1) & 0xFFF] = expect[k];
    b.copy(2, 4);
    for (int i = 0; i < 4; i++) {
        const uint8_t c = ring[(2 + i) & 0xFFF];
        expect.push_back(c);
        ring[(expect.size()) & 0xFFF] = c;
    }
    b.end();
    LzssResult r;
    const Bytes out = decode(b.bytes(), r, false);
    CHECK(out == expect);
    CHECK(r.end == LzssEnd::Marker);
}

void test_end_marker_phases()
{
    for (int lits = 0; lits < 9; lits++) {
        Bits b;
        Bytes expect;
        for (int k = 0; k < lits; k++) {
            b.literal((uint8_t)(0xA0 + k));
            expect.push_back((uint8_t)(0xA0 + k));
        }
        b.end();
        const Bytes in = b.bytes();
        LzssResult r;
        const Bytes out = decode(in, r, false);
        CHECK(out == expect);
        CHECK(r.end == LzssEnd::Marker);
        CHECK(r.consumed == (b.bit_count() + 7) / 8);
        // bytes after the end marker are not read
        Bytes padded = in;
        padded.insert(padded.end(), {0xFF, 0xFF, 0x12});
        const Bytes out2 = decode(padded, r, false);
        CHECK(out2 == expect);
        CHECK(r.consumed == in.size());
    }
}

void test_truncation()
{
    // The input ends inside a literal: the original decoder wrote one 0 byte for it.
    {
        Bits b;
        b.literal('Q');
        b.put(1, 1);  // the flag of a second literal ...
        b.put(5, 3);  // ... and 3 of its 8 bits; zero padding fills the byte
        Bytes in = b.bytes();
        in.resize(2);  // flag+'Q' = 9 bits, then 4 more bits: 2 bytes, the last one partial
        LzssResult r;
        const Bytes plain = decode(in, r, false);
        CHECK(r.end == LzssEnd::InputEnd);
        CHECK(plain == str("Q"));
        const Bytes quirky = decode(in, r, true);
        CHECK(r.end == LzssEnd::InputEnd);
        CHECK(quirky == (Bytes{'Q', 0}));
    }
    // The input ends after a copy's 12-bit offset, before its 4-bit length: the original
    // copied 2 bytes.
    {
        Bits b;
        b.literal('x');
        b.literal('y');
        b.put(0, 1);
        b.put(1, 12);  // offset 1, length missing: 9 + 9 + 13 = 31 bits, padded to 4 bytes
        const Bytes in = b.bytes();
        CHECK(in.size() == 4);
        LzssResult r;
        const Bytes plain = decode(in, r, false);
        CHECK(r.end == LzssEnd::InputEnd);
        CHECK(plain == str("xy"));
        const Bytes quirky = decode(in, r, true);
        CHECK(r.end == LzssEnd::InputEnd);
        CHECK(quirky == str("xyxy"));
    }
    // Empty input.
    {
        LzssResult r;
        const Bytes out = decode(Bytes(), r, true);
        CHECK(out.empty());
        CHECK(r.end == LzssEnd::InputEnd);
        CHECK(r.produced == 0);
    }
}

void test_output_limit()
{
    Bits b;
    b.literal('m');
    for (int i = 0; i < 20; i++)
        b.copy(1, 17);
    b.end();
    LzssResult r;
    const Bytes out = decode(b.bytes(), r, false, 100);
    CHECK(r.end == LzssEnd::OutputLimit);
    CHECK(out.size() == 100);
    CHECK(r.produced == 100);
}

Bytes level_file(const Bytes& stream, uint8_t flag)
{
    Bytes f(kB3dRawHeaderSize, 0);
    for (size_t i = 0; i < f.size(); i++)
        f[i] = (uint8_t)(i * 13 + 5);
    f[kB3dCompressedFlagOffset] = flag;
    f.insert(f.end(), stream.begin(), stream.end());
    return f;
}

void test_b3d_decompress()
{
    Bits b;
    b.literal('L');
    b.literal('V');
    b.copy(1, 3);
    b.end();
    const Bytes file = level_file(b.bytes(), 1);
    Bytes out;
    LzssResult r;
    std::string err;
    CHECK(b3d_decompress(file.data(), file.size(), "level", out, true, &r, &err));
    Bytes expect(file.begin(), file.begin() + (long)kB3dRawHeaderSize);
    expect[kB3dCompressedFlagOffset] = 0;
    const Bytes tail = str("LVLVL");
    expect.insert(expect.end(), tail.begin(), tail.end());
    CHECK(out == expect);
    CHECK(r.end == LzssEnd::Marker);

    // shorter than the raw header: refused, nothing written
    out.assign(3, 9);
    err.clear();
    CHECK(!b3d_decompress(file.data(), kB3dRawHeaderSize - 1, "short", out, true, &r, &err));
    CHECK(!err.empty());

    // a raw header without any stream: the stream is "truncated" at once
    Bytes head_only(file.begin(), file.begin() + (long)kB3dRawHeaderSize);
    out.clear();
    err.clear();
    const bool ok = b3d_decompress(head_only.data(), head_only.size(), "head", out, true, &r, &err);
    CHECK(!ok);
    CHECK(r.end == LzssEnd::InputEnd);
    CHECK(out.size() == kB3dRawHeaderSize && out[kB3dCompressedFlagOffset] == 0);
}

void test_file_round_trip()
{
    const char* tmp = std::getenv("TPMX_TEST_TMP");
    if (!tmp || !*tmp)
        return;
    const std::string dir = platform::path_join(tmp, "unit_b3d_decompress");
    if (!platform::make_dirs(dir)) {
        CHECK(!"cannot create the test directory");
        return;
    }
    Bits b;
    for (int i = 0; i < 300; i++)
        b.literal((uint8_t)(i * 7));
    b.copy(10, 17);
    b.end();
    const Bytes file = level_file(b.bytes(), 1);
    const std::string in = platform::path_join(dir, "round.b3d");
    const std::string out = platform::path_join(dir, "round.dec");
    CHECK(platform::write_file(in, file));
    std::string err;
    CHECK(b3d_decompress_file(in, out, &err));
    Bytes got;
    CHECK(platform::read_file(out, got));
    Bytes mem;
    LzssResult r;
    CHECK(b3d_decompress(file.data(), file.size(), "round", mem, true, &r, &err));
    CHECK(got == mem);

    // a missing input reports an error and does not crash
    err.clear();
    CHECK(!b3d_decompress_file(platform::path_join(dir, "missing.b3d"), out, &err));
    CHECK(!err.empty());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::strcmp(argv[1], "--decompress") == 0) {
        std::string err;
        const bool ok = b3d_decompress_file(argv[2], argv[3], &err);
        if (!ok)
            std::fprintf(stderr, "%s\n", err.c_str());
        return ok ? 0 : 1;
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: unit_b3d_decompress [--decompress IN OUT]\n");
        return 2;
    }
    test_literals_and_copies();
    test_ring_wrap();
    test_end_marker_phases();
    test_truncation();
    test_output_limit();
    test_b3d_decompress();
    test_file_round_trip();
    std::printf("unit_b3d_decompress: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
