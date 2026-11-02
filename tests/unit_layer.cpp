// TPMExtractor fork - unit tests of the foundation layer: platform (paths, safe output names,
// case-insensitive resolution, directories, NTFS enumeration order, files), the BMP writer
// (byte-identical to a replica of the original's code), BinReader, TextWriter and format(),
// option parsing and the --legacy profile, palettes and the legacy colormap, the silent log level.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Generic data only. File checks run in a new folder under TPMX_TEST_TMP (skipped when it is
// not set). Exit 0 = pass, 1 = fail.
#include "binreader.h"
#include "bmp.h"
#include "image.h"
#include "jkout.h"
#include "log.h"
#include "options.h"
#include "platform.h"
#include "textout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
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

static std::string slurp(const std::string& p)
{
    std::vector<uint8_t> d;
    platform::read_file(p, d);
    return std::string(d.begin(), d.end());
}

// ---------------------------------------------------------------- original BMP code, replicated
#pragma pack(push, 2)
struct OrigFileHeader
{
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1, bfReserved2;
    uint32_t bfOffBits;
};
#pragma pack(pop)
struct OrigInfoHeader
{
    uint32_t biSize;
    int32_t biWidth, biHeight;
    uint16_t biPlanes, biBitCount;
    uint32_t biCompression, biSizeImage;
    int32_t biXPelsPerMeter, biYPelsPerMeter;
    uint32_t biClrUsed, biClrImportant;
};
static_assert(sizeof(OrigFileHeader) == 14 && sizeof(OrigInfoHeader) == 40, "SDK layouts");

// What b3d.cpp / baf.cpp wrote (ZeroMemory, field order, then B,G,R per pixel, no padding).
static std::string orig_bmp(int w, int h, const uint8_t* idx, const Palette& pal, bool trans)
{
    OrigFileHeader fh;
    OrigInfoHeader ih;
    std::memset(&fh, 0, sizeof fh);
    std::memset(&ih, 0, sizeof ih);
    ih.biBitCount = 24;
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biSize = sizeof(OrigInfoHeader);
    ih.biSizeImage = (uint32_t)(w * h * 3);
    fh.bfType = ('M' << 8) | 'B';
    fh.bfOffBits = sizeof(OrigFileHeader) + sizeof(OrigInfoHeader);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    std::string out((const char*)&fh, sizeof fh);
    out.append((const char*)&ih, sizeof ih);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t pi = idx[x + y * w];
            ColorRef clr = (trans && pi == 0) ? rgb(255, 0, 255) : rgb(pal.c[pi].r, pal.c[pi].g, pal.c[pi].b);
            char r = (char)red(clr), g = (char)green(clr), b = (char)blue(clr);
            out += b;
            out += g;
            out += r;
        }
    return out;
}

static void test_bmp(const std::string& dir)
{
    std::mt19937 rng(7);
    Palette pal;
    for (int i = 0; i < 256; i++)
        pal.c[i] = Rgb{(uint8_t)rng(), (uint8_t)rng(), (uint8_t)rng()};
    const int dims[][2] = {{1, 1}, {2, 3}, {3, 2}, {4, 4}, {5, 7}, {16, 32}, {32, 16}, {64, 64}, {256, 256}, {256, 1}};
    for (auto& d : dims) {
        for (int trans = 0; trans < 2; trans++) {
            std::vector<uint8_t> idx((size_t)d[0] * d[1]);
            for (auto& v : idx)
                v = (uint8_t)(rng() % 8 == 0 ? 0 : rng());
            const std::string path = platform::path_join(dir, "box.bmp");
            CHECK(bmp24_write_indexed(path, d[0], d[1], idx.data(), pal, trans != 0, BmpRows::Unpadded, nullptr));
            CHECK(slurp(path) == orig_bmp(d[0], d[1], idx.data(), pal, trans != 0));
            // Padded equals Unpadded exactly when width*3 is a multiple of 4
            std::vector<uint8_t> a, b;
            std::vector<Rgb> px(idx.size());
            indexed_to_rgb(idx.data(), idx.size(), pal, trans != 0, kMagenta, px.data());
            CHECK(bmp24_encode(d[0], d[1], px.data(), BmpRows::Unpadded, a, nullptr));
            CHECK(bmp24_encode(d[0], d[1], px.data(), BmpRows::Padded, b, nullptr));
            CHECK(((d[0] * 3) % 4 == 0) == (a == b));
            CHECK(b.size() == 54 + (size_t)((d[0] * 3 + 3) & ~3) * d[1]);
        }
    }
    std::vector<uint8_t> out;
    std::string err;
    Rgb px[1];
    CHECK(!bmp24_encode(0, 1, px, BmpRows::Padded, out, &err) && !err.empty());
    CHECK(!bmp24_encode(1, kBmpMaxDimension + 1, px, BmpRows::Padded, out, &err));
}

static void test_platform(const std::string& dir)
{
    using namespace platform;
    CHECK(path_join("a", "b") == "a/b");
    CHECK(path_join("a/", "b") == "a/b");
    CHECK(path_join("", "b") == "b");
    CHECK(path_join("H:", "x") == "H:/x");
    CHECK(base_name("a/b/c.txt") == "c.txt");
    CHECK(dir_name("a/b/c.txt") == "a/b");
    CHECK(dir_name("/x") == "/");
    CHECK(dir_name("x") == "");
    CHECK(iequals_ascii("Box.MAT", "box.mat") && !iequals_ascii("box", "boxx"));
    CHECK(glob_match_ci("*.baf", "box.BAF"));
    CHECK(!glob_match_ci("*.baf", "box.bafx"));
    CHECK(glob_match_ci("?ox.*", "Box.mat"));
    CHECK(glob_match_ci("*", "anything"));
    CHECK(ntfs_less("a", "B") && ntfs_less("B", "c") && ntfs_less("z", "_x") && ntfs_less("ab", "abc"));

    std::string out, why;
    const char* bad[] = {"", "/abs", "\\abs", "C:x", "..", ".", "a/b", "a\\b", "con", "CON.txt", "lpt1", "Com9.x",
                         "x.", "x ", "a:b", "a*b", "a?b", "a<b", "a|b", "a\"b", ("a\x01" "b")};
    for (const char* b : bad) {
        why.clear();
        const bool ok = safe_output_path(dir, b, out, &why);
        CHECK(!ok && !why.empty());
        if (ok)
            std::fprintf(stderr, "  accepted unsafe name [%s]\n", b);
    }
    const char* bad_sub[] = {"a/../b", "a//b", "/a/b", "a/./b", "a/b/"};
    for (const char* b : bad_sub)
        CHECK(!safe_output_path(dir, b, out, &why, true));
    const char* good[] = {"box.mat", "node_a16.mat", "mat0.bmp", "REMAIN.HEX", "a b.txt", "x.y.z", "comx", "con1"};
    for (const char* g : good) {
        CHECK(safe_output_path(dir, g, out, &why));
        CHECK(out == path_join(dir, g));
    }
    CHECK(safe_output_path(dir, "sub\\leaf.bin", out, &why, true) && out == path_join(dir, "sub/leaf.bin"));

    // directories
    const std::string d1 = path_join(dir, "Lvl");
    CHECK(make_dir(d1) == MkdirResult::Created);
    CHECK(make_dir(d1 + "/") == MkdirResult::AlreadyExists);
    CHECK(make_dir(path_join(dir, "missing/child")) == MkdirResult::ParentMissing);
    CHECK(make_dirs(path_join(dir, "p/q/r")) && is_dir(path_join(dir, "p/q/r")));
    // case-insensitive reuse, as on NTFS (on Windows the file system itself does this and the
    // functions only join)
    CHECK(write_file(path_join(d1, "BOX.MAT"), "x", 1));
#ifndef _WIN32
    CHECK(safe_output_path(d1, "box.mat", out, &why) && out == path_join(d1, "BOX.MAT"));
    CHECK(resolve_ci(dir, "lvl/box.mat") == path_join(dir, "Lvl/BOX.MAT"));
    CHECK(resolve_ci(dir, "LVL\\new.txt") == path_join(dir, "Lvl/new.txt"));
    CHECK(find_ci(path_join(d1, "Box.Mat")) == path_join(d1, "BOX.MAT"));
#endif
    CHECK(find_ci(path_join(d1, "none.mat")) == path_join(d1, "none.mat"));
    CHECK(is_file(path_join(d1, "BOX.MAT")) && !is_dir(path_join(d1, "BOX.MAT")));
    uint64_t sz = 0;
    CHECK(file_size(path_join(d1, "BOX.MAT"), sz) && sz == 1);
    CHECK(!remove_file(d1));
    CHECK(remove_file(path_join(d1, "BOX.MAT")) && !exists(path_join(d1, "BOX.MAT")));
    CHECK(!remove_file(path_join(d1, "BOX.MAT"), &why) && !why.empty());

    // enumeration order and filtering
    const std::string d2 = path_join(dir, "lab");
    CHECK(make_dir(d2) == MkdirResult::Created);
    for (const char* n : {"b.baf", "A.BAF", "c.txt", "_z.baf", "node_a.baf"})
        CHECK(write_file(path_join(d2, n), "", 0));
    CHECK(make_dir(path_join(d2, "d.baf")) == MkdirResult::Created);
    auto list = list_dir(d2, "*.baf");
    std::string order;
    for (auto& e : list)
        order += e.name + (e.is_dir ? "/" : "") + " ";
    CHECK(order == "A.BAF b.baf d.baf/ node_a.baf _z.baf ");
    CHECK(list_dir(d2, "*.*").size() == 6);
    CHECK(list_dir(path_join(dir, "nope"), "*", &why).empty() && !why.empty());

    std::vector<uint8_t> big(200000);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = (uint8_t)(i * 31);
    CHECK(write_file(path_join(dir, "big.bin"), big));
    std::vector<uint8_t> back;
    CHECK(read_file(path_join(dir, "big.bin"), back) && back == big);
    CHECK(!read_file(path_join(dir, "nope.bin"), back, &why) && !why.empty());
}

static void test_binreader()
{
    const uint8_t data[] = {1, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0, 0, 0x80, 0x3F, 'b', 'o', 'x', 0, 'z', 'z', 9};
    BinReader r(data, sizeof data, "node_a.bin", 0x100);
    CHECK(r.u8() == 1);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0x12345678);
    CHECK(r.f32() == 1.0f);
    CHECK(r.file_offset() == 0x100 + 11);
    CHECK(r.peek_tag("box"));
    CHECK(r.fixed_string(6) == "box");
    bool threw = false;
    try {
        r.u32();
    } catch (const ParseError& e) {
        threw = true;
        CHECK(e.file() == "node_a.bin");
        CHECK(e.offset() == 0x100 + 17);
        CHECK(std::string(e.what()).find("node_a.bin @0x111:") == 0);
    }
    CHECK(threw);
    CHECK(r.pos() == 17);  // a failed read does not advance
    BinReader r2(data, sizeof data, "x", 0);
    r2.skip(3);
    BinReader s = r2.sub(4);
    CHECK(s.file_offset() == 3 && s.size() == 4 && r2.pos() == 7);
    threw = false;
    try {
        s.skip(5);
    } catch (const ParseError& e) {
        threw = e.offset() == 3;
    }
    CHECK(threw);
    // count(): limit and bytes-left checks
    const uint8_t cnt[] = {5, 0, 0, 0, 1, 2, 3, 4, 5};
    BinReader c1(cnt, sizeof cnt, "c");
    CHECK(c1.count("items", 10, 1) == 5);
    BinReader c2(cnt, sizeof cnt, "c");
    threw = false;
    try {
        c2.count("items", 4);
    } catch (const ParseError&) {
        threw = true;
    }
    CHECK(threw);
    BinReader c3(cnt, sizeof cnt, "c");
    threw = false;
    try {
        c3.count("items", 10, 2);  // 5*2 > 5 bytes left
    } catch (const ParseError& e) {
        threw = e.offset() == 0;
    }
    CHECK(threw);
    BinReader empty(nullptr, 99, "e");
    CHECK(empty.size() == 0 && empty.at_end());
}

static void test_textwriter(const std::string& dir)
{
    const std::string p = platform::path_join(dir, "node_a.3do.txt");
    {
        TextWriter w;
        CHECK(w.open(p));
        w.printf("a\nb\r\nc %d %f\n", 7, 0.5);
        std::string big(100000, 'x');
        big[50000] = '\n';
        w.write(big);
        CHECK(w.close());
    }
    std::string got = slurp(p);
    const std::string head = "a\r\nb\r\r\nc 7 0.500000\r\n";
    CHECK(got.compare(0, head.size(), head) == 0);
    CHECK(got.size() == head.size() + 100000 + 1);  // the '\n' inside `big` became "\r\n"
    CHECK(got.substr(head.size() + 50000, 3) == "\r\nx");
    {
        TextWriter w;
        w.discard();
        w.printf("dropped\n");
        CHECK(w.close());
        CHECK(!platform::exists(platform::path_join(dir, "dropped")));
    }
    {
        TextWriter w;
        std::string err;
        CHECK(!w.open(platform::path_join(dir, "no/such/dir/x.txt"), &err) && !err.empty());
        w.printf("x\n");
        CHECK(!w.close());
    }
    CHECK(format("%s|%5.2f|%-4d|%04X|%%", "box", 3.14159, 7, 0xAB) == "box| 3.14|7   |00AB|%");
    CHECK(format_fixed(0.1f, 6) == "0.100000");
    CHECK(format_fixed(-0.0000004, 6) == "-0.000000");
    CHECK(format_fixed(2.5, 0) == "2" && format_fixed(3.5, 0) == "4");
}

// Verbosity -2 (the fuzz targets' level) prints no warning or error but still counts them.
// Runs last: stderr goes to a file from here on, so failures are also reported on stdout.
static void test_log_silent(const std::string& dir)
{
    const std::string path = platform::path_join(dir, "stderr.txt");
    const int saved = log::verbosity();
    log::reset_counts();
    std::fflush(stderr);
    const bool redirected = std::freopen(path.c_str(), "w", stderr) != nullptr;
    CHECK(redirected);
    if (!redirected)
        return;
    log::set_verbosity(-2);
    log::warn("silent %d\n", 1);
    log::error("silent\n");
    log::warn_at("file", 4, "silent\n");
    log::error_at("file", 8, "silent\n");
    std::fflush(stderr);
    const std::string silent = slurp(path);
    const unsigned warnings = log::warning_count(), errors = log::error_count();
    log::set_verbosity(-1);
    log::warn("shown\n");
    std::fflush(stderr);
    const std::string shown = slurp(path);
    log::set_verbosity(saved);
    log::reset_counts();
    const int failed_before = g_fail;
    CHECK(silent.empty());
    CHECK(warnings == 2 && errors == 2);
    CHECK(shown.find("warning: shown") != std::string::npos);
    if (g_fail != failed_before)
        std::printf("unit_layer: log level -2 check failed (stderr: \"%s\")\n", shown.c_str());
}

static void test_options()
{
    auto parse = [](std::vector<const char*> a, ParsedArgs& out, std::string& err) {
        a.insert(a.begin(), "tpmextractor");
        return parse_args((int)a.size(), a.data(), out, err);
    };
    ParsedArgs p;
    std::string err;
    CHECK(parse({"disc", "dump"}, p, err));
    const Options& o = p.opts;
    CHECK(!o.legacy && o.scale == 0.2f && o.draw_scale && o.angles == AngleMode::Raw);
    CHECK(o.insert_offset.mode == InsertOffsetMode::Auto && o.radius.mode == RadiusMode::Animated);
    CHECK(o.mat8 && o.mat16 && o.bmp && o.palette.empty() && o.debug_dumps && !o.keep_decompressed);
    CHECK(!o.pause && o.verbose == 0 && p.positional.size() == 2 && !p.pause_explicit);

    CHECK(parse({"--legacy", "disc", "dump"}, p, err));
    CHECK(p.opts.legacy && p.opts.scale == 0.2f && !p.opts.draw_scale && p.opts.angles == AngleMode::Raw);
    CHECK(p.opts.insert_offset.mode == InsertOffsetMode::Fixed && p.opts.insert_offset.x == 0.0f &&
          p.opts.insert_offset.y == 0.0f && p.opts.insert_offset.z == 0.1f);
    CHECK(p.opts.radius.mode == RadiusMode::Fixed && p.opts.radius.value == 0.1f);
    CHECK(!p.opts.mat8 && p.opts.mat16 && p.opts.bmp && p.opts.debug_dumps && !p.opts.keep_decompressed);

    CHECK(parse({"disc", "--legacy", "-v", "--no-pause", "dump"}, p, err));
    CHECK(p.opts.legacy && p.opts.verbose == 1 && p.pause_explicit && !p.opts.pause);
    for (const char* opt : {"--scale=1", "--mat8", "--no-bmp", "--radius=2", "--insert-offset=floor", "--angles=raw",
                            "--palette=x.cmp", "--no-debug-dumps", "--keep-decompressed", "--no-mat16",
                            "--draw-scale", "--no-draw-scale", "--radius=animated"}) {
        CHECK(!parse({"--legacy", opt, "disc", "dump"}, p, err) && err.find("--legacy") != std::string::npos);
    }
    CHECK(parse({"--scale", "0.5", "a", "b"}, p, err) && p.opts.scale == 0.5f);
    CHECK(parse({"--scale=1e-1", "a", "b"}, p, err) && p.opts.scale == 0.1f);
    CHECK(!parse({"--scale", "0", "a", "b"}, p, err));
    CHECK(!parse({"--scale", "abc", "a", "b"}, p, err));
    CHECK(!parse({"--scale"}, p, err));
    CHECK(parse({"--insert-offset", "1,2.5,-3"}, p, err) && p.opts.insert_offset.mode == InsertOffsetMode::Fixed &&
          p.opts.insert_offset.y == 2.5f && p.opts.insert_offset.z == -3.0f);
    CHECK(parse({"--insert-offset=node0"}, p, err) && p.opts.insert_offset.mode == InsertOffsetMode::Node0);
    CHECK(parse({"--insert-offset=floor"}, p, err) && p.opts.insert_offset.mode == InsertOffsetMode::Floor);
    CHECK(parse({"--insert-offset", "auto"}, p, err) && p.opts.insert_offset.mode == InsertOffsetMode::Auto);
    CHECK(describe(Options()).find("insert-offset=auto") != std::string::npos);
    CHECK(!parse({"--legacy", "--insert-offset=auto", "a", "b"}, p, err));
    CHECK(parse({"--insert-offset=zero"}, p, err) && p.opts.insert_offset.mode == InsertOffsetMode::Fixed &&
          p.opts.insert_offset.z == 0.0f);
    CHECK(parse({"--insert-offset=legacy"}, p, err) && p.opts.insert_offset.z == 0.1f);
    CHECK(!parse({"--insert-offset=1,2"}, p, err));
    CHECK(parse({"--radius=stored"}, p, err) && p.opts.radius.mode == RadiusMode::Stored);
    CHECK(parse({"--radius", "2.5"}, p, err) && p.opts.radius.mode == RadiusMode::Fixed && p.opts.radius.value == 2.5f);
    CHECK(!parse({"--radius=-1"}, p, err));
    CHECK(parse({"--radius", "animated"}, p, err) && p.opts.radius.mode == RadiusMode::Animated);
    CHECK(parse({"--radius=computed"}, p, err) && p.opts.radius.mode == RadiusMode::Computed);
    CHECK(parse({"--no-draw-scale"}, p, err) && !p.opts.draw_scale);
    CHECK(parse({"--no-draw-scale", "--draw-scale"}, p, err) && p.opts.draw_scale);
    CHECK(!parse({"--draw-scale=1"}, p, err));
    CHECK(parse({"--angles=rad2deg"}, p, err) && p.opts.angles == AngleMode::RadToDeg);
    CHECK(!parse({"--angles=deg"}, p, err));
    CHECK(parse({"--no-mat8", "--no-mat16", "--no-bmp", "--no-debug-dumps"}, p, err) && !p.opts.mat8 &&
          !p.opts.mat16 && !p.opts.bmp && !p.opts.debug_dumps);
    CHECK(parse({"-vv"}, p, err) && p.opts.verbose == 2);
    CHECK(parse({"-q"}, p, err) && p.opts.verbose == -1);
    CHECK(parse({"--", "-disc", "--dump"}, p, err) && p.positional.size() == 2 && p.positional[0] == "-disc");
    CHECK(!parse({"--frobnicate"}, p, err) && err.find("--frobnicate") != std::string::npos);
    CHECK(!parse({"--legacy=1"}, p, err));
    CHECK(parse({"--help"}, p, err) && p.help);
    CHECK(parse({"--version"}, p, err) && p.version);
    CHECK(!describe(Options()).empty() && usage_text("x").find("--legacy") != std::string::npos);
    CHECK(usage_text("x").find("--[no-]draw-scale") != std::string::npos &&
          usage_text("x").find("animated") != std::string::npos);
    Options legacy_o;
    apply_legacy(legacy_o);
    CHECK(describe(legacy_o).find("draw-scale=off") != std::string::npos);
}

static void test_image_jk(const std::string& dir)
{
    Palette g = Palette::grayscale();
    CHECK(g.c[0] == (Rgb{0, 0, 0}) && g.c[200] == (Rgb{200, 200, 200}));
    CHECK(rgb(-1, 0, 128) == 0x008000FFu && red(0x00112233u) == 0x33 && blue(0x00112233u) == 0x11);
    // CMP-format and raw palettes
    std::vector<uint8_t> cmp(64 + 768 + 16384, 0);
    std::memcpy(cmp.data(), "CMP ", 4);
    for (int i = 0; i < 768; i++)
        cmp[64 + i] = (uint8_t)(i * 7);
    CHECK(platform::write_file(platform::path_join(dir, "mat0.cmp"), cmp));
    Palette p;
    std::string err;
    CHECK(load_palette_file(platform::path_join(dir, "mat0.cmp"), p, &err) && p.c[1].r == 21);
    std::vector<uint8_t> raw(cmp.begin() + 64, cmp.begin() + 64 + 768);
    CHECK(platform::write_file(platform::path_join(dir, "mat0.pal"), raw));
    CHECK(load_palette_file(platform::path_join(dir, "mat0.pal"), p, &err) && p.c[1].r == 21);
    CHECK(platform::write_file(platform::path_join(dir, "bad.pal"), raw.data(), 700));
    CHECK(!load_palette_file(platform::path_join(dir, "bad.pal"), p, &err) && !err.empty());
    jk::Cmp lc = jk::legacy_cmp(g);
    CHECK(lc.version == 20 && lc.flags == 0 && lc.light.size() == 16384 && lc.transparency.empty() &&
          lc.palette.c[9] == g.c[9] && lc.tint[0] == 0.0f);
}

// A new, empty folder under TPMX_TEST_TMP (which ctest may keep between runs): the first
// "<name>_<n>" that does not exist yet.
static std::string fresh_folder(const char* name)
{
    const char* tmp = std::getenv("TPMX_TEST_TMP");
    if (!tmp || !*tmp)
        return std::string();
    if (!platform::make_dirs(tmp))
        return std::string();
    for (int n = 0; n < 100000; n++) {
        const std::string d = platform::path_join(tmp, std::string(name) + "_" + std::to_string(n));
        if (platform::make_dir(d) == platform::MkdirResult::Created)
            return d;
    }
    return std::string();
}

int main()
{
    test_binreader();
    test_options();
    const std::string dir = fresh_folder("unit_layer");
    if (dir.empty()) {
        std::printf("unit_layer: TPMX_TEST_TMP not set or not writable: file checks skipped\n");
    } else {
        const std::string pd = platform::path_join(dir, "plat");
        CHECK(platform::make_dir(pd) == platform::MkdirResult::Created);
        test_platform(pd);
        test_bmp(dir);
        test_textwriter(dir);
        test_image_jk(dir);
        test_log_silent(dir);
    }
    std::printf("unit_layer: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
