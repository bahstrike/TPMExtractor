// TPMExtractor fork - fuzz target: the music-file reader (MCMP container, iMUS map, VIMA blocks)
// and the map text (imc_decode, imc_map_text).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is a music file. A decoded
// file must hold exactly the DATA size of whole frames and give a map text that starts with its
// comment and format lines; a refused one must name an offset inside the file.

#include "binreader.h"
#include "log.h"
#include "sound.h"

#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    try {
        const tpmx::ImcTrack t = tpmx::imc_decode(data, size, "fuzz.imc");
        if ((t.channels != 1 && t.channels != 2) || t.samples.size() * 2 != t.data_bytes ||
            t.samples.size() % (size_t)t.channels || t.events.empty() || t.events[0].tag != "FRMT")
            std::abort();
        const std::string text = tpmx::imc_map_text(t, "fuzz.imc");
        if (text.compare(0, 2, "# ") != 0 || text.find("\nformat\tframes=") == std::string::npos)
            std::abort();
    } catch (const tpmx::ParseError& e) {
        if (e.offset() > size)
            std::abort();
    }
    return 0;
}
