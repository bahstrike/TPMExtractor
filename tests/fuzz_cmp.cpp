// TPMExtractor fork - fuzz target: the colormap (CMP) reader and writer.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). A CMP that parses must encode,
// and the encoded bytes must parse back to the same tables.

#include "jkout.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    tpmx::jk::Cmp cmp;
    std::string err;
    if (!tpmx::jk::parse_cmp(data, size, "fuzz.cmp", cmp, &err))
        return 0;
    std::vector<uint8_t> out;
    if (!tpmx::jk::encode_cmp(cmp, out, &err))
        return 0;  // a header the writer refuses (for example unknown flag bits)
    tpmx::jk::Cmp back;
    if (!tpmx::jk::parse_cmp(out.data(), out.size(), "fuzz.cmp", back, &err))
        std::abort();
    if (back.light != cmp.light || back.transparency != cmp.transparency ||
        std::memcmp(&back.palette, &cmp.palette, sizeof cmp.palette) != 0)
        std::abort();
    return 0;
}
