// TPMExtractor fork - 24-bit BMP writer (byte-identical to the original tool's output).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// The original wrote a 14-byte BITMAPFILEHEADER and a 40-byte BITMAPINFOHEADER with
// fwrite, then the pixels one byte at a time (B, G, R), top row first (negative height),
// with NO row padding. bmp24_encode reproduces those bytes exactly with BmpRows::Unpadded.

#pragma once

#include "image.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

enum class BmpRows {
    // The original layout: rows of exactly width*3 bytes. Malformed for readers when
    // width*3 is not a multiple of 4 (every texture on the retail disc has width*3 % 4 == 0,
    // so there the two layouts are identical). Used by --legacy.
    Unpadded,
    // Standard BMP: each row padded with zero bytes to a multiple of 4. Default mode.
    Padded,
};

// The magenta the original writes for transparent texels (index 0 of a transparent MAT).
const Rgb kMagenta = Rgb{255, 0, 255};

// Largest width or height accepted (TPM textures are at most 256).
const int kBmpMaxDimension = 16384;

// Encodes a 24-bit, uncompressed, top-down BMP:
//   file header: "BM", bfSize = 54 + biSizeImage, reserved 0, bfOffBits = 54;
//   info header: biSize 40, biWidth = width, biHeight = -height, biPlanes 1, biBitCount 24,
//                biCompression 0, biSizeImage = row_bytes * height, the rest 0;
//   then height rows of width pixels (B, G, R), plus padding with BmpRows::Padded.
// width and height must be in 1..kBmpMaxDimension; otherwise returns false and sets *err.
bool bmp24_encode(int width, int height, const Rgb* pixels, BmpRows rows, std::vector<uint8_t>& out,
                  std::string* err = nullptr);

// bmp24_encode + platform::write_file.
bool bmp24_write(const std::string& path, int width, int height, const Rgb* pixels, BmpRows rows,
                 std::string* err = nullptr);

// Expands `count` palette indices to colours. With key0, index 0 becomes key_color (the
// original's transparent texels); otherwise every index goes through the palette.
void indexed_to_rgb(const uint8_t* indices, size_t count, const Palette& pal, bool key0, Rgb key_color, Rgb* out);

// indexed_to_rgb (key colour kMagenta) + bmp24_write.
bool bmp24_write_indexed(const std::string& path, int width, int height, const uint8_t* indices, const Palette& pal,
                         bool key0, BmpRows rows, std::string* err = nullptr);

} // namespace tpmx
