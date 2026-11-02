// TPMExtractor fork - fuzz target: the LAB directory parser, in both layouts.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). Input byte 0 picks the layout;
// byte 1 adds to the file size (a LAB longer than the bytes given); the rest is the file.

#include "lab.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool quiet = (tpmx::log::set_verbosity(-1), true);
    (void)quiet;
    if (size < 2)
        return 0;
    const tpmx::LabLayout layout = (data[0] & 1) ? tpmx::LabLayout::Sequential : tpmx::LabLayout::Stored;
    const uint64_t file_size = (uint64_t)(size - 2) + (uint64_t)data[1] * 977;
    tpmx::LabDirectory dir;
    std::string err;
    if (tpmx::parse_lab_directory(data + 2, size - 2, file_size, "fuzz.lab", layout, dir, &err)) {
        for (const tpmx::LabEntry& e : dir.entries)
            if (e.problem.empty() && e.read_offset + e.length > dir.file_size)
                std::abort();  // an entry accepted for extraction must lie inside the file
        (void)tpmx::find_lab_entry(dir, "box.mat");
    }
    return 0;
}
