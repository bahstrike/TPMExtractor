// TPMExtractor fork - shared image and palette types.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tpmx {

// One palette entry, 8 bits per channel, stored r, g, b (3 bytes, no padding).
struct Rgb
{
    uint8_t r = 0, g = 0, b = 0;
};
static_assert(sizeof(Rgb) == 3, "Rgb must be 3 packed bytes");

inline bool operator==(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(const Rgb& a, const Rgb& b) { return !(a == b); }

// 256-entry palette. The layout is 768 contiguous bytes (r,g,b per entry), the same as the
// palette block of a JK CMP, a TPM BAF colormap and a B3D_PALS entry.
struct Palette
{
    Rgb c[256];

    // The "fake grayscale" ramp the original tool uses for BAFs without a colormap: entry i = (i,i,i).
    static Palette grayscale();
};
static_assert(sizeof(Palette) == 768, "Palette must be 768 packed bytes");

// Win32-compatible packed colour (0x00BBGGRR, the COLORREF layout) so the original
// code's RGB()/GetRValue() arithmetic ports without behaviour changes.
typedef uint32_t ColorRef;

// Same conversions as the Windows SDK RGB() macro: each argument is truncated to a byte first,
// so a signed char -1 becomes 255 exactly as with (BYTE)(r).
inline ColorRef rgb(int r, int g, int b)
{
    return (ColorRef)(uint8_t)r | ((ColorRef)(uint8_t)g << 8) | ((ColorRef)(uint8_t)b << 16);
}
inline uint8_t red(ColorRef c) { return (uint8_t)(c & 0xFFu); }
inline uint8_t green(ColorRef c) { return (uint8_t)((c >> 8) & 0xFFu); }
inline uint8_t blue(ColorRef c) { return (uint8_t)((c >> 16) & 0xFFu); }

// Loads a 256-colour palette from a file:
//   - a JK/TPM CMP ("CMP " magic, palette at offset 64, file at least 832 bytes), or
//   - a raw 768-byte RGB palette (.pal).
// Returns false and sets *err (if given) on any other input.
bool load_palette_file(const std::string& path, Palette& out, std::string* err);

} // namespace tpmx
