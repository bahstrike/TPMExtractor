// TPMExtractor fork - fuzz target: a BAF actor file through process_baf(), the BAF parsers and
// every BAF writer, in --legacy and in default mode.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). Byte 0 of the input chooses the
// run; the rest is the BAF file:
//   bits 0-1  0: default mode; 1: --legacy; 2: default mode with the variants in bits 2-7;
//             3: the variants plus --palette, a generated retail CMP (the palette of
//             colormap-less BAFs and the one every 8-bit MAT is remapped into)
//   bit 2  --radius animated          bit 5  --no-mat8
//   bit 3  --insert-offset node0      bit 6  --no-mat16 --no-bmp --no-debug-dumps
//   bit 4  --no-draw-scale            bit 7  --radius stored --angles rad2deg --scale 0.5
// After every run the output folder may hold plain files only, and nothing may be written
// outside it (fuzz/sandbox.h). Default mode builds a full CMP for every BAF with a colormap,
// so this target runs at tens of inputs per second under ASan; give it minutes, not seconds.

#include "baf.h"
#include "fuzz/sandbox.h"
#include "image.h"
#include "jkout.h"
#include "log.h"
#include "options.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

struct Target
{
    std::string path;
    tpmx::Palette palette;
};

// A generated palette (no data from anywhere) written as a retail CMP with a few fullbright
// colours, so that the glow exclusion of --palette runs too.
Target make_target(tpmx_fuzz::Sandbox& sb)
{
    Target t;
    for (int i = 0; i < 256; i++)
        t.palette.c[i] = tpmx::Rgb{(uint8_t)i, (uint8_t)(255 - i), (uint8_t)(i * 7)};
    tpmx::jk::CmpBuildOptions o;
    for (int i = 248; i < 256; i++)
        o.fullbright.set((size_t)i);
    tpmx::jk::Cmp cmp;
    std::vector<uint8_t> bytes;
    std::string err;
    if (!tpmx::jk::build_cmp(t.palette, o, cmp, &err) || !tpmx::jk::encode_cmp(cmp, bytes, &err))
        tpmx_fuzz::violation("cannot build the --palette CMP", err);
    t.path = sb.put("target.cmp", bytes.data(), bytes.size());
    if (!tpmx::load_palette_file(t.path, t.palette, &err))
        tpmx_fuzz::violation("cannot load the --palette CMP", err);
    return t;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    static tpmx_fuzz::Sandbox sb("baf");
    static const Target target = make_target(sb);
    if (size < 1)
        return 0;
    const uint8_t mode = data[0];
    const std::string in = sb.put("x.baf", data + 1, size - 1);

    tpmx::Options o;
    const tpmx::Palette* fallback = nullptr;
    if ((mode & 3) == 1) {
        tpmx::apply_legacy(o);
    } else if ((mode & 3) >= 2) {
        if (mode & 0x04)
            o.radius.mode = tpmx::RadiusMode::Animated;
        if (mode & 0x08)
            o.insert_offset.mode = tpmx::InsertOffsetMode::Node0;
        if (mode & 0x10)
            o.draw_scale = false;
        if (mode & 0x20)
            o.mat8 = false;
        if (mode & 0x40) {
            o.mat16 = false;
            o.bmp = false;
            o.debug_dumps = false;
        }
        if (mode & 0x80) {
            o.radius.mode = tpmx::RadiusMode::Stored;
            o.angles = tpmx::AngleMode::RadToDeg;
            o.scale = 0.5f;
        }
        if ((mode & 3) == 3) {
            o.palette = target.path;
            fallback = &target.palette;
        }
    }
    (void)tpmx::process_baf(in, sb.out(), o, fallback);

    for (const tpmx_fuzz::OutEntry& e : sb.check())
        if (e.is_dir)
            tpmx_fuzz::violation("a folder in the BAF output", e.path);
    sb.clean_out();
    return 0;
}
