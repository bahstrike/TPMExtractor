// TPMExtractor fork - fuzz target: an InstallShield v3 (.Z) archive through extract_z() (the
// unshieldv3 table of contents and the blast decompressor).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is the archive. After
// every run nothing may be written outside the output folder (fuzz/sandbox.h): archive paths
// may create folders below it, and every path extract_z() reports as written must lie inside
// it, without "." or ".." components.

#include "fuzz/sandbox.h"
#include "log.h"
#include "options.h"
#include "zextract.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    static tpmx_fuzz::Sandbox sb("z");
    const std::string in = sb.put("x.z", data, size);
    tpmx::Options o;
    std::vector<std::string> written;
    (void)tpmx::extract_z(in, sb.out(), o, &written);
    (void)sb.check();
    for (const std::string& w : written) {
        std::string rel;
        if (!sb.inside_out(w, rel))
            tpmx_fuzz::violation("a reported output path outside the output folder", w);
    }
    sb.clean_out();
    return 0;
}
