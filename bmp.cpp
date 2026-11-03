// TPMExtractor fork - 24-bit BMP writer (byte-identical to the original tool's output).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "bmp.h"

#include "platform.h"

namespace tpmx {

namespace {

void put_u16(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    put_u16(v, x & 0xFFFF);
    put_u16(v, x >> 16);
}

} // namespace

bool bmp24_encode(int width, int height, const Rgb* pixels, BmpRows rows, std::vector<uint8_t>& out, std::string* err)
{
    out.clear();
    if (width < 1 || height < 1 || width > kBmpMaxDimension || height > kBmpMaxDimension || !pixels) {
        if (err)
            *err = "invalid BMP dimensions " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    const uint32_t raw_row = (uint32_t)width * 3u;
    const uint32_t row_bytes = rows == BmpRows::Padded ? (raw_row + 3u) & ~3u : raw_row;
    const uint32_t image_size = row_bytes * (uint32_t)height;  // <= 16384*3*16384 < 2^32
    const uint32_t header_size = 14 + 40;

    out.reserve(header_size + image_size);
    // BITMAPFILEHEADER (14 bytes, packed)
    put_u16(out, 0x4D42);  // 'B','M'
    put_u32(out, header_size + image_size);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u32(out, header_size);
    // BITMAPINFOHEADER (40 bytes)
    put_u32(out, 40);
    put_u32(out, (uint32_t)width);
    put_u32(out, (uint32_t)(-(int64_t)height));  // negative: top-down rows
    put_u16(out, 1);   // planes
    put_u16(out, 24);  // bits per pixel
    put_u32(out, 0);   // BI_RGB
    put_u32(out, image_size);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0);

    for (int y = 0; y < height; y++) {
        const Rgb* row = pixels + (size_t)y * (size_t)width;
        for (int x = 0; x < width; x++) {
            out.push_back(row[x].b);
            out.push_back(row[x].g);
            out.push_back(row[x].r);
        }
        for (uint32_t pad = raw_row; pad < row_bytes; pad++)
            out.push_back(0);
    }
    return true;
}

bool bmp24_write(const std::string& path, int width, int height, const Rgb* pixels, BmpRows rows, std::string* err)
{
    std::vector<uint8_t> data;
    if (!bmp24_encode(width, height, pixels, rows, data, err)) {
        if (err)
            *err = path + ": " + *err;
        return false;
    }
    return platform::write_file(path, data, err);
}

void indexed_to_rgb(const uint8_t* indices, size_t count, const Palette& pal, bool key0, Rgb key_color, Rgb* out)
{
    for (size_t i = 0; i < count; i++) {
        const uint8_t pi = indices[i];
        out[i] = (key0 && pi == 0) ? key_color : pal.c[pi];
    }
}

bool bmp24_write_indexed(const std::string& path, int width, int height, const uint8_t* indices, const Palette& pal,
                         bool key0, BmpRows rows, std::string* err)
{
    if (width < 1 || height < 1 || width > kBmpMaxDimension || height > kBmpMaxDimension || !indices) {
        if (err)
            *err = path + ": invalid BMP dimensions " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    std::vector<Rgb> rgb((size_t)width * (size_t)height);
    indexed_to_rgb(indices, rgb.size(), pal, key0, kMagenta, rgb.data());
    return bmp24_write(path, width, height, rgb.data(), rows, err);
}

} // namespace tpmx
