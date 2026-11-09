// TPMExtractor fork - unit tests for jkout (retail 8-bit MAT writer, CMP builder, nearest colour).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Synthetic fixtures only: generated palettes (gray ramp, seeded pseudo-random, duplicates) and
// generated checker/gradient textures. Exit 0 = pass, 1 = fail. Uses TPMX_TEST_TMP for the
// file round trips when it is set (make check and ctest set it).
//
// MATs are checked with an independent reader that follows the read order of the retail loader
// (OpenJKDF2 src/Engine/rdMaterial.c rdMaterial_LoadEntry_Common), CMPs at fixed offsets
// (src/types.h rdColormapHeader; src/Engine/rdColormap.c rdColormap_LoadEntry). build_cmp's
// tables are compared with a plain-scan reference implementation of its documented rules, and
// every light table is checked for monotonic brightness.

#include "jkout.h"
#include "platform.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tpmx;
using namespace tpmx::jk;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what, int line, const std::string& detail = std::string())
{
    g_checks++;
    if (!ok) {
        g_failures++;
        std::fprintf(stderr, "FAIL line %d: %s%s%s\n", line, what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_MSG(cond, msg) check((cond), #cond, __LINE__, (msg))

uint32_t rd32(const std::vector<uint8_t>& d, size_t at)
{
    return (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8) | ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24);
}

// ------------------------------------------------------------- fixtures

uint32_t g_lcg = 1;
uint32_t lcg()
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg >> 8;
}

Palette gray_ramp() { return Palette::grayscale(); }

Palette random_palette(uint32_t seed)
{
    g_lcg = seed;
    Palette p;
    for (int i = 0; i < 256; i++)
        p.c[i] = Rgb{(uint8_t)(lcg() & 0xFF), (uint8_t)(lcg() & 0xFF), (uint8_t)(lcg() & 0xFF)};
    return p;
}

// A palette with deliberate duplicates (every colour appears twice) and a second black at 128.
Palette duplicate_palette()
{
    Palette p;
    for (int i = 0; i < 128; i++) {
        const Rgb c{(uint8_t)(i * 2), (uint8_t)(255 - i * 2), (uint8_t)((i * 37) & 0xFF)};
        p.c[i] = c;
        p.c[i + 128] = c;
    }
    p.c[0] = Rgb{0, 0, 0};
    p.c[128] = Rgb{0, 0, 0};
    return p;
}

IndexSet all_indices()
{
    IndexSet s;
    s.set();
    return s;
}

// width x height checker of two indices, with index 0 holes where (x + 2y) % 5 == 0 if `holes`.
Mat8Texture checker(int width, int height, uint8_t a, uint8_t b, bool holes)
{
    Mat8Texture t;
    t.width = width;
    t.height = height;
    t.masked = holes;
    std::vector<uint8_t> px((size_t)width * (size_t)height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            px[(size_t)y * (size_t)width + (size_t)x] =
                (holes && (x + 2 * y) % 5 == 0) ? 0 : (((x / 2 + y / 2) & 1) ? a : b);
    t.mips.push_back(px);
    return t;
}

// ------------------------------------------------------------- independent MAT reader

struct MatTexinfo
{
    uint32_t texture_type = 0, solid_color = 0, tail[4] = {0, 0, 0, 0};
    bool has_ext = false;
    uint32_t ext[4] = {0, 0, 0, 0};
};
struct MatTextureRec
{
    uint32_t width = 0, height = 0, alpha_en = 0, unk_0c = 0, key = 0, mips = 0;
    std::vector<std::vector<uint8_t>> levels;
};
struct ParsedMat
{
    uint32_t revision = 0, type = 0, num_texinfo = 0, num_textures = 0, format[14] = {};
    std::vector<MatTexinfo> cels;
    std::vector<MatTextureRec> textures;
    size_t consumed = 0;
};

// Reads in the order of rdMaterial_LoadEntry_Common: 76-byte header (magic, revision, type,
// num_texinfo, num_textures, rdTexFormat), per texinfo 24 bytes plus a 16-byte ext header when
// texture_type & 8, per texture a 24-byte header then num_mipmaps levels of
// (w>>n)*(h>>n)*bpp/8 bytes, then 768 palette bytes when type & 1.
bool read_mat_like_retail(const std::vector<uint8_t>& d, ParsedMat& p, std::string& why)
{
    size_t at = 0;
    auto need = [&](size_t n) {
        if (n > d.size() - at) {
            why = "short at " + std::to_string(at);
            return false;
        }
        return true;
    };
    auto u32 = [&]() {
        const uint32_t v = rd32(d, at);
        at += 4;
        return v;
    };
    if (!need(76))
        return false;
    if (std::memcmp(d.data(), "MAT ", 4) != 0) {
        why = "magic";
        return false;
    }
    at = 4;
    p.revision = u32();
    p.type = u32();
    p.num_texinfo = u32();
    p.num_textures = u32();
    for (uint32_t& f : p.format)
        f = u32();
    if (p.num_texinfo > 16 || p.num_textures > 16) {
        why = "counts";
        return false;
    }
    for (uint32_t k = 0; k < p.num_texinfo; k++) {
        MatTexinfo ti;
        if (!need(24))
            return false;
        ti.texture_type = u32();
        ti.solid_color = u32();
        for (uint32_t& t : ti.tail)
            t = u32();
        if (ti.texture_type & 8) {
            if (!need(16))
                return false;
            ti.has_ext = true;
            for (uint32_t& e : ti.ext)
                e = u32();
        }
        p.cels.push_back(ti);
    }
    const uint32_t bytes_per_texel = p.format[1] / 8;
    for (uint32_t k = 0; k < p.num_textures; k++) {
        MatTextureRec t;
        if (!need(24))
            return false;
        t.width = u32();
        t.height = u32();
        t.alpha_en = u32();
        t.unk_0c = u32();
        t.key = u32();
        t.mips = u32();
        if (t.mips > 4) {
            why = "mips";
            return false;
        }
        for (uint32_t m = 0; m < t.mips; m++) {
            const size_t n = (size_t)(t.width >> m) * (size_t)(t.height >> m) * bytes_per_texel;
            if (n == 0 || !need(n)) {
                why = "level size";
                return false;
            }
            t.levels.emplace_back(d.begin() + (std::ptrdiff_t)at, d.begin() + (std::ptrdiff_t)(at + n));
            at += n;
        }
        p.textures.push_back(t);
    }
    if (p.type & 1) {
        if (!need(768))
            return false;
        at += 768;
    }
    p.consumed = at;
    return true;
}

std::string tmp_dir()
{
    const char* t = std::getenv("TPMX_TEST_TMP");
    return t && *t ? std::string(t) : std::string();
}

// ------------------------------------------------------------- nearest colour

void test_nearest()
{
    const Palette dup = duplicate_palette();
    const IndexSet all = all_indices();
    // exact match, ties to the lowest index (entries i and i+128 are equal)
    CHECK(nearest_index(dup, dup.c[40], all) == 40);
    CHECK(nearest_index(dup, dup.c[168], all) == 40);
    CHECK(nearest_index(dup, dup.c[168], all, ColorMetric::Redmean) == 40);
    IndexSet upper;
    for (int i = 128; i < 256; i++)
        upper.set((size_t)i);
    CHECK(nearest_index(dup, dup.c[40], upper) == 168);
    CHECK(nearest_index(dup, Rgb{1, 2, 3}, IndexSet()) == 0);  // empty set
    const Palette gray = gray_ramp();
    CHECK(nearest_index(gray, Rgb{100, 101, 102}, all) == 101);
    CHECK(nearest_index(gray, Rgb{100, 101, 102}, all, ColorMetric::Redmean) == 101);

    // remap_table uses the pruned matcher: it must equal a plain scan for every entry.
    for (int metric = 0; metric < 2; metric++) {
        const ColorMetric m = metric ? ColorMetric::Redmean : ColorMetric::Euclidean;
        for (uint32_t seed = 1; seed <= 6; seed++) {
            const Palette src = random_palette(seed * 7919u), dst = random_palette(seed * 104729u);
            IndexSet allowed = all;
            for (int i = 0; i < 256; i += 3 + (int)seed)
                allowed.reset((size_t)i);
            IndexSet scan = allowed;
            scan.reset(0);
            const std::array<uint8_t, 256> t = remap_table(src, dst, allowed, false, m);
            int same = 0;
            for (int i = 0; i < 256; i++)
                same += t[(size_t)i] == nearest_index(dst, src.c[i], scan, m);
            CHECK_MSG(same == 256,
                      "matcher vs scan, seed " + std::to_string(seed) + " metric " + std::to_string(metric));
        }
    }

    // index 0 is never a target except for the key of a masked texture
    const Palette src = random_palette(77), dst = random_palette(78);
    const std::array<uint8_t, 256> masked = remap_table(src, dst, all, true);
    const std::array<uint8_t, 256> opaque = remap_table(src, dst, all, false);
    CHECK(masked[0] == 0);
    int zeros_masked = 0, zeros_opaque = 0;
    for (int i = 1; i < 256; i++)
        zeros_masked += masked[(size_t)i] == 0;
    for (int i = 0; i < 256; i++)
        zeros_opaque += opaque[(size_t)i] == 0;
    CHECK(zeros_masked == 0);
    CHECK(zeros_opaque == 0);
    // identical palettes: identity, except that duplicates go to the lower copy
    const std::array<uint8_t, 256> self = remap_table(dup, dup, all, true);
    CHECK(self[0] == 0 && self[40] == 40 && self[168] == 40 && self[128] != 0);
    // nothing allowed but 0: everything maps to 0
    IndexSet only0;
    only0.set(0);
    const std::array<uint8_t, 256> none = remap_table(src, dst, only0, false);
    CHECK(none[17] == 0 && none[255] == 0);
}

// ------------------------------------------------------------- mips

void test_mips()
{
    const Palette gray = gray_ramp();
    const IndexSet all = all_indices();
    std::string err;

    // a uniform texture keeps its index at every level
    Mat8Texture u;
    u.width = 16;
    u.height = 16;
    u.mips.push_back(std::vector<uint8_t>(256, 77));
    CHECK(generate_mips(u, 4, gray, all, &err));
    CHECK(u.mips.size() == 4);
    CHECK(u.mips[1].size() == 64 && u.mips[2].size() == 16 && u.mips[3].size() == 4);
    bool all77 = true;
    for (const std::vector<uint8_t>& level : u.mips)
        for (uint8_t v : level)
            all77 = all77 && v == 77;
    CHECK(all77);

    // box filter: a 2x2 of gray 10, 20, 30, 41 averages to round(25.25) = 25
    Mat8Texture b;
    b.width = 2;
    b.height = 2;
    b.mips.push_back({10, 20, 30, 41});
    CHECK(generate_mips(b, 2, gray, all, &err));
    CHECK(b.mips.size() == 2 && b.mips[1].size() == 1 && b.mips[1][0] == 25);

    // masked: three or four holes stay a hole; two holes average the two opaque texels
    Mat8Texture m;
    m.width = 4;
    m.height = 2;
    m.masked = true;
    m.mips.push_back({0, 0, 0, 50, 0, 100, 60, 0});  // blocks: {0,0,0,100}, {0,50,60,0}
    CHECK(generate_mips(m, 4, gray, all, &err));
    CHECK(m.mips.size() == 2);  // 4x2 -> 2x1 -> (1x0 impossible)
    CHECK(m.mips[1][0] == 0);   // three holes
    CHECK(m.mips[1][1] == 55);  // (50 + 60) / 2

    // opaque texels never become index 0, even when the average is index 0's colour
    Mat8Texture z;
    z.width = 2;
    z.height = 2;
    z.mips.push_back({0, 0, 0, 0});
    CHECK(generate_mips(z, 2, gray, all, &err));
    CHECK(z.mips[1][0] == 1);

    // existing levels are kept, extra ones dropped
    Mat8Texture k = checker(8, 8, 9, 200, false);
    k.mips.push_back(std::vector<uint8_t>(16, 123));
    CHECK(generate_mips(k, 3, gray, all, &err));
    CHECK(k.mips.size() == 3 && k.mips[1][0] == 123 && k.mips[1][15] == 123);
    CHECK(generate_mips(k, 1, gray, all, &err));
    CHECK(k.mips.size() == 1);

    // errors leave the texture unchanged
    Mat8Texture e = checker(8, 8, 9, 200, false);
    CHECK(!generate_mips(e, 0, gray, all, &err) && !err.empty());
    CHECK(!generate_mips(e, 5, gray, all, &err));
    IndexSet only0;
    only0.set(0);
    err.clear();
    CHECK(!generate_mips(e, 2, gray, only0, &err) && !err.empty() && e.mips.size() == 1);
    Mat8Texture np = e;
    np.width = 12;
    CHECK(!generate_mips(np, 2, gray, all, &err));
    Mat8Texture big = e;
    big.width = 512;
    CHECK(!generate_mips(big, 2, gray, all, &err));
    Mat8Texture bad = e;
    bad.mips[0].pop_back();
    CHECK(!generate_mips(bad, 2, gray, all, &err) && bad.mips.size() == 1 && bad.mips[0].size() == 63);
    Mat8Texture none;
    none.width = 4;
    none.height = 4;
    CHECK(!generate_mips(none, 2, gray, all, &err));

    // representative colour: mean of the opaque texels, never 0
    Mat8Texture r;
    r.width = 2;
    r.height = 2;
    r.masked = true;
    r.mips.push_back({0, 100, 200, 0});
    CHECK(representative_index(r, gray, all) == 150);
    r.mips[0] = {0, 0, 0, 0};
    CHECK(representative_index(r, gray, all) == 0);
}

// ------------------------------------------------------------- MAT

void test_mat8()
{
    const Palette gray = gray_ramp();
    const IndexSet all = all_indices();
    std::string err;

    // one masked 8x8 texture, full mip chain
    Mat8 one;
    one.textures.push_back(checker(8, 8, 40, 90, true));
    CHECK(generate_mips(one.textures[0], 4, gray, all, &err));
    one.cels.push_back(Mat8Cel{0, 65});
    std::vector<uint8_t> d;
    CHECK(encode_mat8(one, d, &err));
    const size_t expect =
        kMatHeaderSize + kMatTexinfoSize + kMatTexinfoExtSize + kMatTextureHeaderSize + 64 + 16 + 4 + 1;
    CHECK_MSG(d.size() == expect, std::to_string(d.size()));
    CHECK(std::memcmp(d.data(), "MAT ", 4) == 0);
    CHECK(rd32(d, 0x04) == 0x32);
    CHECK(rd32(d, 0x08) == 2);
    CHECK(rd32(d, 0x0C) == 1 && rd32(d, 0x10) == 1);
    CHECK(rd32(d, 0x14) == 0 && rd32(d, 0x18) == 8);
    bool zero_format = true;
    for (size_t at = 0x1C; at < 0x4C; at += 4)
        zero_format = zero_format && rd32(d, at) == 0;
    CHECK(zero_format);
    ParsedMat p;
    std::string why;
    CHECK_MSG(read_mat_like_retail(d, p, why), why);
    CHECK(p.consumed == d.size());
    CHECK(p.cels.size() == 1 && p.cels[0].texture_type == 8 && p.cels[0].solid_color == 65);
    CHECK(p.cels[0].tail[0] == 0x3F800000 && p.cels[0].tail[3] == 0x3F800000);
    CHECK(p.cels[0].has_ext && p.cels[0].ext[0] == 0 && p.cels[0].ext[2] == 0 && p.cels[0].ext[3] == 0);
    CHECK(p.textures.size() == 1);
    const MatTextureRec& tr = p.textures[0];
    CHECK(tr.width == 8 && tr.height == 8 && tr.alpha_en == 1 && tr.unk_0c == 0 && tr.key == 0 && tr.mips == 4);
    CHECK(tr.levels.size() == 4 && tr.levels[0] == one.textures[0].mips[0] && tr.levels[3] == one.textures[0].mips[3]);

    // several textures of different sizes, cels pointing at them out of order
    Mat8 multi;
    multi.textures.push_back(checker(16, 16, 3, 4, false));
    multi.textures.push_back(checker(32, 8, 5, 6, false));
    multi.textures.push_back(checker(1, 1, 7, 7, false));
    CHECK(generate_mips(multi.textures[0], 2, gray, all, &err));
    multi.cels = {Mat8Cel{2, 11}, Mat8Cel{0, 12}, Mat8Cel{1, 13}, Mat8Cel{0, 14}};
    CHECK(encode_mat8(multi, d, &err));
    CHECK(read_mat_like_retail(d, p = ParsedMat(), why));
    CHECK(p.consumed == d.size());
    CHECK(p.type == 2 && p.num_texinfo == 4 && p.num_textures == 3);
    CHECK(p.cels[0].ext[3] == 2 && p.cels[1].ext[3] == 0 && p.cels[2].ext[3] == 1 && p.cels[3].ext[3] == 0);
    CHECK(p.cels[0].solid_color == 11 && p.cels[3].solid_color == 14);
    CHECK(p.textures[0].mips == 2 && p.textures[1].width == 32 && p.textures[1].height == 8);
    CHECK(p.textures[1].alpha_en == 0 && p.textures[2].mips == 1 && p.textures[2].width == 1);
    CHECK(d.size() == kMatHeaderSize + 4 * (kMatTexinfoSize + kMatTexinfoExtSize) + 3 * kMatTextureHeaderSize +
                          (256 + 64) + 256 + 1);

    // empty cel list: one cel per texture, solid colour 0
    Mat8 dflt;
    dflt.textures = {checker(4, 4, 1, 2, false), checker(2, 2, 3, 4, false)};
    CHECK(encode_mat8(dflt, d, &err));
    CHECK(read_mat_like_retail(d, p = ParsedMat(), why) && p.consumed == d.size());
    CHECK(p.num_texinfo == 2 && p.cels[0].ext[3] == 0 && p.cels[1].ext[3] == 1 && p.cels[1].solid_color == 0);

    // solid-colour MAT: type 0, no textures, texinfo type 0 without ext header
    Mat8 solid;
    solid.cels.push_back(Mat8Cel{0, 5});
    CHECK(encode_mat8(solid, d, &err));
    CHECK(d.size() == kMatHeaderSize + kMatTexinfoSize);
    CHECK(read_mat_like_retail(d, p = ParsedMat(), why) && p.consumed == d.size());
    CHECK(p.type == 0 && p.num_textures == 0 && p.cels[0].texture_type == 0 && !p.cels[0].has_ext &&
          p.cels[0].solid_color == 5);

    // limits: 16 cels pass, 17 fail; bad sizes and mip chains fail and leave `out` empty
    Mat8 cels16;
    cels16.textures.push_back(checker(2, 2, 1, 2, false));
    cels16.cels.assign(16, Mat8Cel{0, 1});
    CHECK(encode_mat8(cels16, d, &err));
    cels16.cels.push_back(Mat8Cel{0, 1});
    d.assign(3, 1);
    CHECK(!encode_mat8(cels16, d, &err) && d.empty() && !err.empty());
    Mat8 nope;
    CHECK(!encode_mat8(nope, d, &err));
    struct Bad
    {
        const char* what;
        Mat8Texture t;
    };
    std::vector<Bad> bad;
    Mat8Texture t = checker(12, 8, 1, 2, false);
    bad.push_back(Bad{"width 12", t});
    t = checker(512, 2, 1, 2, false);
    bad.push_back(Bad{"width 512", t});
    t = checker(8, 8, 1, 2, false);
    t.mips.clear();
    bad.push_back(Bad{"no mips", t});
    t = checker(8, 8, 1, 2, false);
    t.mips[0].pop_back();
    bad.push_back(Bad{"short mip 0", t});
    t = checker(2, 2, 1, 2, false);
    t.mips.push_back({1});
    t.mips.push_back({1});
    bad.push_back(Bad{"2x2 with 3 levels", t});
    t = checker(64, 64, 1, 2, false);
    for (int k = 1; k <= 4; k++)
        t.mips.push_back(std::vector<uint8_t>((size_t)(64 >> k) * (size_t)(64 >> k), 1));
    bad.push_back(Bad{"5 levels", t});
    t = checker(8, 8, 1, 2, false);
    t.mips.push_back(std::vector<uint8_t>(15, 1));
    bad.push_back(Bad{"wrong mip 1 size", t});
    for (const Bad& b : bad) {
        Mat8 m;
        m.textures.push_back(b.t);
        err.clear();
        CHECK_MSG(!encode_mat8(m, d, &err) && d.empty() && !err.empty(), b.what);
    }
    Mat8 badcel;
    badcel.textures.push_back(checker(2, 2, 1, 2, false));
    badcel.cels.push_back(Mat8Cel{1, 0});
    CHECK(!encode_mat8(badcel, d, &err));
    badcel.cels[0].texture = -1;
    CHECK(!encode_mat8(badcel, d, &err));

    // write_mat8 writes exactly the encoded bytes
    const std::string dir = tmp_dir();
    if (!dir.empty()) {
        const std::string path = platform::path_join(dir, "mat0.mat");
        CHECK(write_mat8(path, multi, &err));
        std::vector<uint8_t> file, enc;
        CHECK(platform::read_file(path, file) && encode_mat8(multi, enc, &err) && file == enc);
        CHECK(!write_mat8(path, nope, &err) && err.find(path) != std::string::npos);
    }
}

// ------------------------------------------------------------- CMP

// The original tool's colormap: Strike's tCMP_Data written by CCMP::saveColormap after
// newColormap() (all zero, "CMP ", version 20, transparency 0) and setColor() for 256 entries.
std::vector<uint8_t> original_tool_cmp(const Palette& pal)
{
    std::vector<uint8_t> d(64 + 768 + 16384, 0);
    std::memcpy(d.data(), "CMP ", 4);
    d[4] = 20;
    std::memcpy(d.data() + 64, &pal, 768);
    return d;
}

void check_cmp_header(const std::vector<uint8_t>& d, uint32_t version, uint32_t flags, uint32_t tint_bits, int line)
{
    check(d.size() >= 64 && std::memcmp(d.data(), "CMP ", 4) == 0, "CMP magic", line);
    check(rd32(d, 4) == version, "CMP version", line);
    check(rd32(d, 8) == flags, "CMP flags", line);
    check(rd32(d, 12) == tint_bits && rd32(d, 16) == tint_bits && rd32(d, 20) == tint_bits, "CMP tint", line);
    bool zero = true;
    for (size_t at = 24; at < 64; at++)
        zero = zero && d[at] == 0;
    check(zero, "CMP reserved bytes are zero", line);
}

void check_light_table(const Cmp& c, const CmpBuildOptions& o, const char* name)
{
    const std::string n(name);
    CHECK_MSG(c.light.size() == kCmpLightTableSize, n);
    bool row63 = true, identity = true, no_zero = true, no_reserved = true;
    for (int i = 0; i < 256; i++) {
        row63 = row63 && c.light[(size_t)63 * 256 + (size_t)i] == i;
        const bool id_col = o.fullbright.test((size_t)i) || (o.key_index0 && i == 0);
        for (int level = 0; level < 64; level++) {
            const uint8_t v = c.light[(size_t)level * 256 + (size_t)i];
            if (id_col)
                identity = identity && v == i;
            if (o.key_index0 && i != 0)
                no_zero = no_zero && v != 0;
            if (v != i)
                no_reserved = no_reserved && !o.reserved.test(v);
        }
    }
    CHECK_MSG(row63, n + ": row 63 is the identity");
    CHECK_MSG(identity, n + ": fullbright and key columns are the identity");
    CHECK_MSG(no_zero, n + ": index 0 is never a light result");
    CHECK_MSG(no_reserved, n + ": reserved indices are never a result for another index");
    // monotonicity: no column gets brighter as the light level falls
    CHECK_MSG(light_monotonic_violations(c) == 0, n + ": monotonic, " + std::to_string(light_monotonic_violations(c)));
}

// The rules build_cmp documents, implemented with nothing but the plain-scan nearest_index: the
// accelerated search inside build_cmp must give exactly the same tables.
Cmp reference_cmp(const Palette& pal, const CmpBuildOptions& o)
{
    IndexSet results;
    results.set();
    results &= ~o.reserved;
    if (o.key_index0)
        results.reset(0);
    IndexSet identity = o.fullbright;
    if (o.key_index0)
        identity.set(0);
    Cmp c;
    c.palette = pal;
    c.light.assign(kCmpLightTableSize, 0);
    for (int i = 0; i < 256; i++) {
        for (int level = 0; level < 64; level++) {
            uint8_t v = (uint8_t)i;
            if (level < 63 && !identity.test((size_t)i))
                v = nearest_index(pal,
                                  Rgb{(uint8_t)((pal.c[i].r * level + 31) / 63),
                                      (uint8_t)((pal.c[i].g * level + 31) / 63),
                                      (uint8_t)((pal.c[i].b * level + 31) / 63)},
                                  results, o.metric);
            c.light[(size_t)level * 256 + (size_t)i] = v;
        }
        if (o.monotonic_light && !identity.test((size_t)i))
            for (int level = 62; level >= 0; level--) {
                uint8_t& e = c.light[(size_t)level * 256 + (size_t)i];
                const uint8_t above = c.light[(size_t)(level + 1) * 256 + (size_t)i];
                auto lum = [&pal](uint8_t k) { return 299 * pal.c[k].r + 587 * pal.c[k].g + 114 * pal.c[k].b; };
                if (lum(e) > lum(above))
                    e = above;
            }
    }
    if (o.transparency_table) {
        const int a = o.transparency_alpha, b = 256 - a;
        c.transparency.assign(kCmpTransparencyTableSize, 0);
        for (int s = 0; s < 256; s++)
            for (int d = 0; d < 256; d++)
                c.transparency[(size_t)s * 256 + (size_t)d] =
                    (s == d && results.test((size_t)s))
                        ? (uint8_t)s
                        : nearest_index(pal,
                                        Rgb{(uint8_t)((a * pal.c[s].r + b * pal.c[d].r + 128) >> 8),
                                            (uint8_t)((a * pal.c[s].g + b * pal.c[d].g + 128) >> 8),
                                            (uint8_t)((a * pal.c[s].b + b * pal.c[d].b + 128) >> 8)},
                                        results, o.metric);
    }
    return c;
}

void test_cmp_reference()
{
    std::string err;
    struct Case
    {
        Palette pal;
        CmpBuildOptions o;
        const char* name;
    };
    std::vector<Case> cases;
    for (uint32_t seed = 500; seed < 504; seed++)
        for (int metric = 0; metric < 2; metric++) {
            Case c{random_palette(seed), CmpBuildOptions(), "random"};
            c.o.metric = metric ? ColorMetric::Redmean : ColorMetric::Euclidean;
            c.o.transparency_alpha = seed == 501 ? 128 : (seed == 502 ? 0 : (seed == 503 ? 256 : 90));
            if (seed == 502) {
                for (int i = 16; i < 48; i++)
                    c.o.fullbright.set((size_t)i);
                c.o.reserved.set(7);
                c.o.reserved.set(255);
            }
            if (seed == 503) {
                c.o.key_index0 = false;
                c.o.monotonic_light = false;
            }
            cases.push_back(c);
        }
    cases.push_back(Case{duplicate_palette(), CmpBuildOptions(), "duplicates"});
    cases.push_back(Case{gray_ramp(), CmpBuildOptions(), "gray"});
    Palette clustered;  // every entry inside one small RGB box: worst case for the cell bounds
    for (int i = 0; i < 256; i++)
        clustered.c[i] = Rgb{(uint8_t)(100 + (i & 7)), (uint8_t)(120 + ((i >> 3) & 7)), (uint8_t)(140 + (i >> 6))};
    cases.push_back(Case{clustered, CmpBuildOptions(), "clustered"});
    for (const Case& k : cases) {
        Cmp got;
        CHECK(build_cmp(k.pal, k.o, got, &err));
        const Cmp want = reference_cmp(k.pal, k.o);
        CHECK_MSG(got.light == want.light, std::string(k.name) + " light table equals the reference");
        CHECK_MSG(got.transparency == want.transparency,
                  std::string(k.name) + " transparency table equals the reference");
    }
}

void test_cmp()
{
    std::string err;
    const Palette gray = gray_ramp();

    // legacy colormap: byte-identical to the original tool's file
    for (uint32_t seed : {3u, 4u}) {
        const Palette pal = random_palette(seed);
        std::vector<uint8_t> d;
        CHECK(encode_cmp(legacy_cmp(pal), d, &err));
        CHECK(d.size() == kCmpSizeNoTransparency);
        CHECK(d == original_tool_cmp(pal));
        check_cmp_header(d, 20, 0, 0, __LINE__);
    }

    // retail colormap, default options (documented defaults)
    const Palette pal = random_palette(11);
    Cmp c;
    CmpBuildOptions o;
    CHECK(o.transparency_table && o.key_index0 && o.monotonic_light && o.fullbright.none() && o.reserved.none());
    CHECK(o.transparency_alpha == kTransparencyAlphaDefault && kTransparencyAlphaDefault == 90);
    CHECK(o.metric == ColorMetric::Redmean);
    CHECK(kCmpSizeWithTransparency == 64 + 768 + 16384 + 65536 && kCmpSizeNoTransparency == 64 + 768 + 16384);
    CHECK(build_cmp(pal, o, c, &err));
    CHECK(c.version == 30 && c.flags == 1 && c.tint[0] == 1.0f && c.tint[2] == 1.0f);
    std::vector<uint8_t> d;
    CHECK(encode_cmp(c, d, &err));
    CHECK(d.size() == kCmpSizeWithTransparency);
    check_cmp_header(d, 30, 1, 0x3F800000u, __LINE__);
    CHECK(std::memcmp(d.data() + 64, &pal, 768) == 0);
    CHECK(std::memcmp(d.data() + 0x340, c.light.data(), kCmpLightTableSize) == 0);
    CHECK(std::memcmp(d.data() + 0x4340, c.transparency.data(), kCmpTransparencyTableSize) == 0);
    check_light_table(c, o, "random 11");

    // light rows on a gray ramp: exactly max(1, round(i * L / 63)) (index 0 is never a result)
    Cmp g;
    CHECK(build_cmp(gray, o, g, &err));
    check_light_table(g, o, "gray");
    bool exact = true;
    for (int level = 0; level < 63; level++)
        for (int i = 1; i < 256; i++) {
            int want = (i * level + 31) / 63;
            if (want < 1)
                want = 1;
            exact = exact && g.light[(size_t)level * 256 + (size_t)i] == want;
        }
    CHECK(exact);
    // brightness falls with the level: row 0 maps every column to the darkest result (index 1)
    CHECK(g.light[200] == 1 && g.light[(size_t)32 * 256 + 200] == 102);

    // transparency on a gray ramp: (a*s + (256-a)*d + 128) >> 8, never 0, diagonal identity
    bool trans_exact = true, trans_no_zero = true;
    for (int s = 0; s < 256; s++)
        for (int t = 0; t < 256; t++) {
            const uint8_t v = g.transparency[(size_t)s * 256 + (size_t)t];
            int want = (kTransparencyAlphaDefault * s + (256 - kTransparencyAlphaDefault) * t + 128) >> 8;
            if (want < 1)
                want = 1;
            trans_exact = trans_exact && v == want;
            trans_no_zero = trans_no_zero && v != 0;
        }
    CHECK(trans_exact && trans_no_zero);
    CmpBuildOptions half = o;
    half.transparency_alpha = 128;
    Cmp h;
    CHECK(build_cmp(gray, half, h, &err));
    CHECK(h.transparency[(size_t)10 * 256 + 31] == 21 && h.transparency[(size_t)31 * 256 + 10] == 21);

    // monotonicity over many palettes, and evidence that the rule matters
    int raw_violations = 0;
    for (uint32_t seed = 100; seed < 120; seed++) {
        const Palette rp = random_palette(seed);
        Cmp mc;
        CHECK(build_cmp(rp, o, mc, &err));
        check_light_table(mc, o, "random");
        CmpBuildOptions raw = o;
        raw.monotonic_light = false;
        raw.transparency_table = false;
        Cmp rc;
        CHECK(build_cmp(rp, raw, rc, &err));
        raw_violations += light_monotonic_violations(rc);
        CmpBuildOptions eu = o;
        eu.metric = ColorMetric::Euclidean;
        Cmp euc;
        CHECK(build_cmp(rp, eu, euc, &err));
        check_light_table(euc, eu, "random euclidean");
    }
    CHECK_MSG(raw_violations > 0, "nearest colour alone is not monotonic on these palettes");

    // duplicates: identity row 63 and diagonal despite lower duplicates
    const Palette dup = duplicate_palette();
    Cmp dc;
    CHECK(build_cmp(dup, o, dc, &err));
    check_light_table(dc, o, "duplicates");
    CHECK(dc.transparency[(size_t)168 * 256 + 168] == 168 && dc.light[(size_t)63 * 256 + 168] == 168);
    CHECK(dc.light[(size_t)0 * 256 + 77] == 128);  // darkest target: the non-zero black

    // fullbright and reserved sets
    CmpBuildOptions fr = o;
    fr.fullbright.set(3);
    fr.fullbright.set(200);
    fr.reserved.set(5);
    fr.reserved.set(250);
    Cmp fc;
    CHECK(build_cmp(pal, fr, fc, &err));
    check_light_table(fc, fr, "fullbright/reserved");
    const IndexSet fb = fullbright_indices(fc);
    CHECK(fb.test(0) && fb.test(3) && fb.test(200));
    bool trans_no_reserved = true;
    for (uint8_t v : fc.transparency)
        trans_no_reserved = trans_no_reserved && v != 5 && v != 250;
    CHECK(trans_no_reserved);
    const IndexSet glow = glow_indices(fc);
    CHECK(!glow.test(0) && glow.test(3) == (std::max(pal.c[3].r, std::max(pal.c[3].g, pal.c[3].b)) > 8));

    // without the transparency table: flags 0, 17,216 bytes
    CmpBuildOptions nt = o;
    nt.transparency_table = false;
    Cmp n;
    CHECK(build_cmp(pal, nt, n, &err) && n.flags == 0 && n.transparency.empty());
    CHECK(encode_cmp(n, d, &err) && d.size() == kCmpSizeNoTransparency);
    check_cmp_header(d, 30, 0, 0x3F800000u, __LINE__);

    // option errors
    CmpBuildOptions bad = o;
    bad.transparency_alpha = 257;
    CHECK(!build_cmp(pal, bad, n, &err) && !err.empty());
    bad = o;
    bad.reserved.set();
    CHECK(!build_cmp(pal, bad, n, &err));

    // parse round trips
    Cmp back;
    CHECK(encode_cmp(c, d, &err) && parse_cmp(d.data(), d.size(), "cmp0.cmp", back, &err));
    CHECK(back.version == 30 && back.flags == 1 && back.tint[1] == 1.0f && back.light == c.light &&
          back.transparency == c.transparency && std::memcmp(&back.palette, &pal, 768) == 0);
    std::vector<uint8_t> again;
    CHECK(encode_cmp(back, again, &err) && again == d);
    std::vector<uint8_t> trailing = d;
    trailing.push_back(7);
    CHECK(parse_cmp(trailing.data(), trailing.size(), "cmp0.cmp", back, &err));  // ignored, as the engine
    CHECK(encode_cmp(legacy_cmp(pal), d, &err) && parse_cmp(d.data(), d.size(), "cmp1.cmp", back, &err));
    CHECK(back.version == 20 && back.flags == 0 && back.tint[0] == 0.0f && back.transparency.empty());

    // parse errors carry the name and offset
    CHECK(encode_cmp(c, d, &err));
    std::vector<uint8_t> bad_magic = d;
    bad_magic[0] = 'X';
    err.clear();
    CHECK(!parse_cmp(bad_magic.data(), bad_magic.size(), "cmp2.cmp", back, &err));
    CHECK_MSG(err.find("cmp2.cmp @0x0:") != std::string::npos, err);
    CHECK(!parse_cmp(d.data(), 40, "cmp3.cmp", back, &err) && err.find("cmp3.cmp") != std::string::npos);
    CHECK(!parse_cmp(d.data(), kCmpSizeNoTransparency, "cmp4.cmp", back, &err));  // flags 1, table missing
    CHECK_MSG(err.find("truncated") != std::string::npos, err);
    std::vector<uint8_t> rgb16 = d;
    rgb16[8] = 5;
    CHECK(!parse_cmp(rgb16.data(), rgb16.size(), "cmp5.cmp", back, &err));
    CHECK_MSG(err.find("cmp5.cmp @0x8:") != std::string::npos, err);
    CHECK(!parse_cmp(nullptr, 0, "cmp6.cmp", back, &err));

    // encode errors
    Cmp e = c;
    e.flags = 5;
    CHECK(!encode_cmp(e, d, &err) && d.empty());
    e = c;
    e.light.pop_back();
    CHECK(!encode_cmp(e, d, &err));
    e = c;
    e.transparency.clear();
    CHECK(!encode_cmp(e, d, &err));
    e = c;
    e.flags = 0;
    CHECK(!encode_cmp(e, d, &err));
    e.flags = 0x101;  // unknown bits other than bit 2 are written as given
    CHECK(encode_cmp(e, d, &err) && rd32(d, 8) == 0x101);

    // helpers on a malformed Cmp
    Cmp empty;
    empty.light.clear();
    CHECK(fullbright_indices(empty).none() && light_monotonic_violations(empty) == -1);

    // file round trip
    const std::string dir = tmp_dir();
    if (!dir.empty()) {
        const std::string path = platform::path_join(dir, "cmp0.cmp");
        CHECK(write_cmp(path, c, &err));
        Cmp f;
        CHECK(read_cmp(path, f, &err) && f.light == c.light && f.transparency == c.transparency);
        uint64_t size = 0;
        CHECK(platform::file_size(path, size) && size == kCmpSizeWithTransparency);
        CHECK(!read_cmp(platform::path_join(dir, "missing.cmp"), f, &err) && !err.empty());
        Cmp w = c;
        w.flags = 4;
        CHECK(!write_cmp(path, w, &err) && err.find(path) != std::string::npos);
    }
}

} // namespace

int main()
{
    test_nearest();
    test_mips();
    test_mat8();
    test_cmp();
    test_cmp_reference();
    std::printf("unit_jkout: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
