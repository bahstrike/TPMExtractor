// TPMExtractor fork - bounds-checked little-endian reader over a memory buffer.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "binreader.h"

#include "textout.h"

#include <cstring>

namespace tpmx {

ParseError::ParseError(const std::string& file, uint64_t offset, const std::string& message)
    : std::runtime_error(format("%s @0x%llX: %s", file.c_str(), (unsigned long long)offset, message.c_str())),
      file_(file), offset_(offset), message_(message)
{
}

BinReader::BinReader(const uint8_t* data, size_t size, std::string name, uint64_t base_offset)
    : data_(data), size_(data ? size : 0), name_(std::move(name)), base_(base_offset)
{
}

BinReader::BinReader(const std::vector<uint8_t>& data, std::string name, uint64_t base_offset)
    : data_(data.data()), size_(data.size()), name_(std::move(name)), base_(base_offset)
{
}

void BinReader::fail(const std::string& message) const { throw ParseError(name_, base_ + pos_, message); }

void BinReader::fail_at(size_t pos, const std::string& message) const { throw ParseError(name_, base_ + pos, message); }

void BinReader::need(size_t n, const char* what) const
{
    if (n > size_ - pos_)
        fail(format("unexpected end of data reading %s (%llu bytes needed, %llu left)", what, (unsigned long long)n,
                    (unsigned long long)(size_ - pos_)));
}

void BinReader::seek(size_t pos)
{
    if (pos > size_)
        fail(format("seek to %llu beyond end of data (%llu bytes)", (unsigned long long)pos,
                    (unsigned long long)size_));
    pos_ = pos;
}

void BinReader::skip(size_t n)
{
    need(n, "skipped bytes");
    pos_ += n;
}

uint8_t BinReader::u8()
{
    need(1, "u8");
    return data_[pos_++];
}

uint16_t BinReader::u16()
{
    need(2, "u16");
    uint16_t v = (uint16_t)(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
}

uint32_t BinReader::u32()
{
    need(4, "u32");
    uint32_t v = (uint32_t)data_[pos_] | ((uint32_t)data_[pos_ + 1] << 8) | ((uint32_t)data_[pos_ + 2] << 16) |
                 ((uint32_t)data_[pos_ + 3] << 24);
    pos_ += 4;
    return v;
}

uint64_t BinReader::u64()
{
    need(8, "u64");
    uint64_t lo = u32();
    uint64_t hi = u32();
    return lo | (hi << 32);
}

float BinReader::f32()
{
    need(4, "f32");
    uint32_t bits = u32();
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void BinReader::bytes(void* dst, size_t n)
{
    need(n, "bytes");
    if (n)
        std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
}

std::vector<uint8_t> BinReader::bytes(size_t n)
{
    need(n, "bytes");
    std::vector<uint8_t> v(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return v;
}

const uint8_t* BinReader::span(size_t n)
{
    need(n, "bytes");
    const uint8_t* p = data_ + pos_;
    pos_ += n;
    return p;
}

std::string BinReader::fixed_string(size_t n)
{
    need(n, "fixed-size string");
    const char* p = reinterpret_cast<const char*>(data_ + pos_);
    size_t len = 0;
    while (len < n && p[len])
        ++len;
    pos_ += n;
    return std::string(p, len);
}

bool BinReader::peek_tag(const char* tag) const
{
    const size_t n = std::strlen(tag);
    return n <= size_ - pos_ && std::memcmp(data_ + pos_, tag, n) == 0;
}

BinReader BinReader::sub(size_t n)
{
    need(n, "sub-block");
    BinReader r(data_ + pos_, n, name_, base_ + pos_);
    pos_ += n;
    return r;
}

uint32_t BinReader::count(const char* what, uint32_t max_count, size_t min_element_size)
{
    const size_t at = pos_;
    const uint32_t c = u32();
    if (c > max_count)
        fail_at(at, format("%s count %u exceeds the limit %u", what, c, max_count));
    if (min_element_size && (uint64_t)c * min_element_size > (uint64_t)(size_ - pos_))
        fail_at(at, format("%s count %u needs at least %llu bytes, %llu left", what, c,
                           (unsigned long long)((uint64_t)c * min_element_size), (unsigned long long)(size_ - pos_)));
    return c;
}

} // namespace tpmx
