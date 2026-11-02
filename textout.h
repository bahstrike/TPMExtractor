// TPMExtractor fork - deterministic printf-style formatting and CRLF text files.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Why this exists: the original tool wrote every .3do/.key/.txt with MSVC fprintf in text
// mode. To get the same bytes on every OS and C runtime (glibc, MSVCRT under MinGW, the
// UCRT, macOS libc), the fork never formats output with the C library:
//   - format()/vformat() implement printf syntax themselves; %f is the exact decimal
//     expansion rounded half-to-even, as glibc and the UCRT on Windows 10 2004+ print it
//     (the original's output, as reproduced by --legacy, assumes that runtime);
//   - TextWriter writes every '\n' as "\r\n", as MSVC text mode did.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>

#if defined(__clang__)
#define TPMX_PRINTF(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#elif defined(__GNUC__)
#define TPMX_PRINTF(fmt_index, first_arg) __attribute__((format(gnu_printf, fmt_index, first_arg)))
#else
#define TPMX_PRINTF(fmt_index, first_arg)
#endif

namespace tpmx {

// printf-compatible formatting that gives the same bytes on every platform.
// Supported: flags "-+ #0", width and precision (numbers or '*'), length modifiers
// hh h l ll z j t, conversions d i u o x X c s p f F and %%.
//   - %f/%F: exact, round-half-to-even (see format_fixed).
//   - %p: "0x" followed by lower-case hex digits ("(nil)" for a null pointer, as glibc).
//   - %s with a null pointer prints "(null)".
//   - %n is not supported: it writes nothing and prints nothing.
//   - %e %E %g %G %a %A and %L... fall back to the C library and are NOT deterministic
//     across platforms; never use them for output files.
std::string format(const char* fmt, ...) TPMX_PRINTF(1, 2);
std::string vformat(const char* fmt, va_list ap);
void vformat_append(std::string& out, const char* fmt, va_list ap);

// The text %.<precision>f gives for `v` (no width, no flags):
//   - finite values: the exact binary value rounded to `precision` decimals, ties to even
//     (correct rounding, same as glibc and the Windows 10 2004+ UCRT); the sign is kept
//     for negative values that round to zero ("-0.000000"), as both runtimes do;
//   - infinities: "inf" / "-inf";
//   - NaN: "nan" / "-nan" by sign, except the x86 "indefinite" NaN (bits
//     0xFFF8000000000000, what float->double promotion of the default quiet NaN gives),
//     which prints "-nan(ind)" like the UCRT.
// `upper` gives the %F spellings (INF, NAN, -NAN(IND)). precision < 0 means 6; values above
// 100 are clamped to 100.
std::string format_fixed(double v, int precision, bool upper = false);

// A text output file with MSVC text-mode semantics on every platform: each '\n' byte
// written becomes "\r\n" on disk (so "\r\n" in the data becomes "\r\r\n", exactly as the
// original fopen(path, "w") produced). Output is buffered; errors are sticky and reported
// by close().
class TextWriter
{
public:
    TextWriter() = default;
    ~TextWriter();  // closes; use close() to see errors
    TextWriter(const TextWriter&) = delete;
    TextWriter& operator=(const TextWriter&) = delete;

    // Creates or truncates `path`. False (and *err) if the file cannot be created.
    bool open(const std::string& path, std::string* err = nullptr);
    // Makes this writer a sink: until the next open(), writes are dropped without error.
    // Lets a parser keep its print statements when an output (e.g. a debug dump) is off.
    void discard();
    bool is_open() const { return f_ != nullptr; }
    bool is_discarding() const { return discard_; }
    const std::string& path() const { return path_; }

    void printf(const char* fmt, ...) TPMX_PRINTF(2, 3);
    void vprintf(const char* fmt, va_list ap);
    void write(const char* data, size_t n);
    void write(const std::string& s) { write(s.data(), s.size()); }

    // Flushes and closes. False (and *err) if open() failed, or if any write or the close
    // failed (writes to a writer that is not open count as failed writes). The error state
    // is cleared afterwards, so a second close() returns true.
    bool close(std::string* err = nullptr);

private:
    void flush_buffer();

    FILE* f_ = nullptr;
    std::string path_;
    std::string buf_;
    bool failed_ = false;
    bool discard_ = false;
};

} // namespace tpmx
