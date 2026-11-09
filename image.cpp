// TPMExtractor fork - shared image and palette types.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "image.h"

#include "platform.h"

#include <cstring>
#include <vector>

namespace tpmx {

Palette Palette::grayscale()
{
    Palette p;
    for (int i = 0; i < 256; i++)
        p.c[i] = Rgb{(uint8_t)i, (uint8_t)i, (uint8_t)i};
    return p;
}

bool load_palette_file(const std::string& path, Palette& out, std::string* err)
{
    std::vector<uint8_t> data;
    std::string why;
    if (!platform::read_file(path, data, &why)) {
        if (err)
            *err = why;
        return false;
    }
    const size_t kCmpPaletteOffset = 64;
    if (data.size() >= 4 && std::memcmp(data.data(), "CMP ", 4) == 0) {
        if (data.size() < kCmpPaletteOffset + sizeof(Palette)) {
            if (err)
                *err = path + ": CMP file too short for a palette (" + std::to_string(data.size()) + " bytes)";
            return false;
        }
        std::memcpy(&out, data.data() + kCmpPaletteOffset, sizeof(Palette));
        return true;
    }
    if (data.size() == sizeof(Palette)) {
        std::memcpy(&out, data.data(), sizeof(Palette));
        return true;
    }
    if (err)
        *err = path + ": not a CMP file and not a raw 768-byte palette (" + std::to_string(data.size()) + " bytes)";
    return false;
}

} // namespace tpmx
