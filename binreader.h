// TPMExtractor fork - bounds-checked little-endian reader over a memory buffer.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// For the phase-2 parser rewrites. Every read is checked against the buffer end; a failed
// read throws ParseError, which carries the file name and the absolute file offset of the
// failing read, so callers can log it and move on to the next file:
//
//     try { BinReader r(data, path); ... }
//     catch (const ParseError& e) { log::error("%s\n", e.what()); }  // next file
//
// Values are decoded byte by byte (little-endian), so the reader works on any host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace tpmx {

class ParseError : public std::runtime_error
{
public:
    // what() is "<file> @0x<offset>: <message>".
    ParseError(const std::string& file, uint64_t offset, const std::string& message);
    const std::string& file() const { return file_; }
    uint64_t offset() const { return offset_; }
    const std::string& message() const { return message_; }

private:
    std::string file_;
    uint64_t offset_;
    std::string message_;
};

class BinReader
{
public:
    // Reads `size` bytes at `data`, which must outlive the reader. `name` is the file name
    // used in errors; `base_offset` is the file offset of data[0] (non-zero for sub-readers
    // and for buffers that hold only part of a file).
    BinReader(const uint8_t* data, size_t size, std::string name, uint64_t base_offset = 0);
    BinReader(const std::vector<uint8_t>& data, std::string name, uint64_t base_offset = 0);

    const std::string& name() const { return name_; }
    size_t size() const { return size_; }
    size_t pos() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }
    bool at_end() const { return pos_ == size_; }
    uint64_t file_offset() const { return base_ + pos_; }  // absolute offset of pos()
    bool can_read(size_t n) const { return n <= size_ - pos_; }

    void seek(size_t pos);  // pos <= size(), else ParseError
    void skip(size_t n);

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int8_t i8() { return (int8_t)u8(); }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    float f32();  // IEEE-754 single from its little-endian bits

    void bytes(void* dst, size_t n);
    std::vector<uint8_t> bytes(size_t n);
    // Pointer to the next n bytes (inside the buffer), then advances.
    const uint8_t* span(size_t n);
    // Reads exactly n bytes and returns them up to the first NUL (a fixed-size name field).
    std::string fixed_string(size_t n);
    // True if the next n bytes equal `tag` (n = strlen(tag)); does not advance.
    bool peek_tag(const char* tag) const;

    // A reader over the next n bytes (positions restart at 0, offsets stay absolute); advances.
    BinReader sub(size_t n);

    // Element count read from the file, checked against a sanity limit and against the bytes
    // left (count * min_element_size <= remaining()). Throws ParseError naming `what`.
    uint32_t count(const char* what, uint32_t max_count, size_t min_element_size = 1);

    // Throws ParseError at the current position.
    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] void fail_at(size_t pos, const std::string& message) const;

private:
    void need(size_t n, const char* what) const;

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    std::string name_;
    uint64_t base_;
};

} // namespace tpmx
