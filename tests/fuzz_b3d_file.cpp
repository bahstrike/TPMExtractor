// TPMExtractor fork - fuzz target: a level file through process_b3d(), in default mode, with
// --keep-decompressed and with --legacy.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is the level file
// LVL.B3D (fuzz_b3d covers the in-memory section walk and decoder, fuzz_b3d_geom the geometry
// decoder; this target covers the whole level run: loading, texture output, the geometry files,
// the .dec file and its removal). After every run nothing may be written outside the output
// folder (fuzz/sandbox.h), and the output folder may hold only LVL/, <n>.bmp files in it,
// LVL/LVL.obj and LVL/LVL.mtl in default mode, and LVL/LVL.dec with --keep-decompressed.

#include "b3d.h"
#include "fuzz/sandbox.h"
#include "log.h"
#include "options.h"

#include <cstdint>
#include <string>

namespace {

const size_t kMaxInput = 4u << 20;  // larger inputs only make the target slower

bool is_bmp_name(const std::string& s)
{
    size_t digits = 0;
    while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9')
        digits++;
    return digits > 0 && s.compare(digits, std::string::npos, ".bmp") == 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    static tpmx_fuzz::Sandbox sb("b3d_file");
    if (size > kMaxInput)
        return 0;
    const std::string in = sb.put("LVL.B3D", data, size);
    for (int run = 0; run < 3; run++) {
        tpmx::Options o;
        if (run == 1)
            o.keep_decompressed = true;
        if (run == 2)
            tpmx::apply_legacy(o);
        (void)tpmx::process_b3d(in, sb.out(), o);
        for (const tpmx_fuzz::OutEntry& e : sb.check()) {
            if (e.path == "LVL" && e.is_dir)
                continue;
            if (e.path == "LVL/LVL.dec" && !e.is_dir) {
                if (!o.keep_decompressed)
                    tpmx_fuzz::violation("LVL.dec left behind without --keep-decompressed", e.path);
                continue;
            }
            if (!e.is_dir && e.path.compare(0, 4, "LVL/") == 0 && is_bmp_name(e.path.substr(4)))
                continue;
            if (!e.is_dir && (e.path == "LVL/LVL.obj" || e.path == "LVL/LVL.mtl")) {
                if (o.legacy)
                    tpmx_fuzz::violation("geometry written with --legacy", e.path);
                continue;
            }
            tpmx_fuzz::violation("unexpected level output", e.path);
        }
        sb.clean_out();
    }
    return 0;
}
