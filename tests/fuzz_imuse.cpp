// TPMExtractor fork - fuzz target: the music-table reader of IMUSE.DLL (PE headers read as data,
// tables found by their structure) and its text (imuse_read_tables, imuse_tables_text).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is a DLL. Tables that are
// read must have the retail record counts and every resolved file must be one of the file table's;
// a refused input must name an offset inside the file.

#include "binreader.h"
#include "imuse.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    try {
        const tpmx::ImuseTables t = tpmx::imuse_read_tables(data, size, "fuzz.dll");
        if (t.states.size() != tpmx::kImuseStates || t.sequences.size() != tpmx::kImuseSequences ||
            t.files.size() != tpmx::kImuseFiles || t.state_offset >= t.sequence_offset ||
            t.sequence_offset >= t.file_offset || t.file_offset >= size)
            std::abort();
        for (const tpmx::ImuseRecord& r : t.states) {
            const int f = tpmx::imuse_find_file(t, r.key);
            if (f >= 0 && t.files[(size_t)f].key != r.key)
                std::abort();
        }
        if (tpmx::imuse_tables_text(t, "fuzz.dll").find("\nstate\t0\t") == std::string::npos)
            std::abort();
    } catch (const tpmx::ParseError& e) {
        if (e.offset() > size)
            std::abort();
    }
    return 0;
}
