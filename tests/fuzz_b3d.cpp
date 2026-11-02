// TPMExtractor fork - fuzz target: the level (.B3D) section walk and the LZSS decoder.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is used as a
// decompressed level image (section walk) and as a level file (decompression with and without
// the original decoder's quirks).

#include "b3d.h"
#include "b3d_decompress.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool quiet = (tpmx::log::set_verbosity(-1), true);
    (void)quiet;
    const tpmx::B3dWalk walk = tpmx::b3d_walk_sections(data, size);
    for (const tpmx::B3dSection& s : walk.sections)
        if (s.end() > size)
            std::abort();  // a walked section must lie inside the image
    if (walk.complete && walk.stop_offset != size)
        std::abort();
    for (int quirks = 0; quirks < 2; quirks++) {
        std::vector<uint8_t> out;
        tpmx::LzssResult r;
        std::string err;
        tpmx::b3d_decompress(data, size, "fuzz.b3d", out, quirks != 0, &r, &err);
        if (r.end == tpmx::LzssEnd::Marker && r.consumed + tpmx::kB3dRawHeaderSize > size)
            std::abort();  // the end marker lies inside the input
    }
    std::vector<uint8_t> out;
    const tpmx::LzssResult r = tpmx::lzss_decode(data, size, out, 1 << 16, true);
    if (out.size() > (1 << 16) || r.produced != out.size())
        std::abort();
    return 0;
}
