// TPMExtractor fork - fuzz target: the voice-file reader and the VIMA decoder (wv_decode).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is a voice file, read as if
// it were a LAB entry at file offset 0x1000. A decoded file must hold whole frames of 1 or 2
// channels, no more samples than its codes can hold, and give a 44-byte WAV header; a refused one
// must name an offset inside the entry. (Voice files inside LAB archives, whose names become file
// names, go through fuzz_lab_file.)

#include "binreader.h"
#include "log.h"
#include "sound.h"

#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    const uint64_t base = 0x1000;
    try {
        const tpmx::WvSound s = tpmx::wv_decode(data, size, "fuzz.wv", base, "entry 0: ");
        if ((s.channels != 1 && s.channels != 2) || s.samples.size() % (size_t)s.channels ||
            s.samples.size() > 2 * (uint64_t)size)
            std::abort();
        if (tpmx::wav_header(s.rate, s.channels, (uint32_t)(2 * s.samples.size())).size() != tpmx::kWavHeaderBytes)
            std::abort();
    } catch (const tpmx::ParseError& e) {
        if (e.offset() < base || e.offset() > base + size)
            std::abort();
    }
    return 0;
}
