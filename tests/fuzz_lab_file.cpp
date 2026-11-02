// TPMExtractor fork - fuzz target: a LAB archive through extract_lab(), in default mode (stored
// offsets) and with --legacy (sequential read).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is the LAB file
// (fuzz_lab covers the in-memory directory parser; this target covers the extraction: entry
// names becoming file names, chunked copies, overwrites). After every run nothing may be
// written outside the output folder (fuzz/sandbox.h), and the output folder may hold plain
// files only: LAB entry names never create folders.

#include "fuzz/sandbox.h"
#include "lab.h"
#include "log.h"
#include "options.h"

#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    static tpmx_fuzz::Sandbox sb("lab_file");
    const std::string in = sb.put("x.lab", data, size);
    for (int run = 0; run < 2; run++) {
        tpmx::Options o;
        if (run == 1)
            tpmx::apply_legacy(o);
        (void)tpmx::extract_lab(in, sb.out(), o);
        for (const tpmx_fuzz::OutEntry& e : sb.check())
            if (e.is_dir)
                tpmx_fuzz::violation("a folder in the LAB output", e.path);
        sb.clean_out();
    }
    return 0;
}
