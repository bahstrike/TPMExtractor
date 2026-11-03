// TPMExtractor fork - console logging with verbosity levels and error counting.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "log.h"

#include <cstdarg>
#include <cstdio>

namespace tpmx {
namespace log {

namespace {

int g_level = 0;
unsigned g_errors = 0;
unsigned g_warnings = 0;

const int kSilent = -2;  // at or below this level warnings and errors are counted, not printed

void emit(FILE* stream, const std::string& prefix, const char* fmt, va_list ap)
{
    std::string s = prefix;
    vformat_append(s, fmt, ap);
    std::fwrite(s.data(), 1, s.size(), stream);
    if (stream == stderr)
        std::fflush(stderr);
}

std::string location(const std::string& file, uint64_t offset)
{
    return format("%s @0x%llX: ", file.c_str(), (unsigned long long)offset);
}

} // namespace

void set_verbosity(int level) { g_level = level; }
int verbosity() { return g_level; }

void info(const char* fmt, ...)
{
    if (g_level < 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    emit(stdout, std::string(), fmt, ap);
    va_end(ap);
}

void verbose(const char* fmt, ...)
{
    if (g_level < 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    emit(stdout, std::string(), fmt, ap);
    va_end(ap);
}

void debug(const char* fmt, ...)
{
    if (g_level < 2)
        return;
    va_list ap;
    va_start(ap, fmt);
    emit(stdout, std::string(), fmt, ap);
    va_end(ap);
}

void warn(const char* fmt, ...)
{
    ++g_warnings;
    if (g_level <= kSilent)
        return;
    std::fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    emit(stderr, "warning: ", fmt, ap);
    va_end(ap);
}

void error(const char* fmt, ...)
{
    ++g_errors;
    if (g_level <= kSilent)
        return;
    std::fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    emit(stderr, "error: ", fmt, ap);
    va_end(ap);
}

void error_at(const std::string& file, uint64_t offset, const char* fmt, ...)
{
    ++g_errors;
    if (g_level <= kSilent)
        return;
    std::fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    emit(stderr, "error: " + location(file, offset), fmt, ap);
    va_end(ap);
}

void warn_at(const std::string& file, uint64_t offset, const char* fmt, ...)
{
    ++g_warnings;
    if (g_level <= kSilent)
        return;
    std::fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    emit(stderr, "warning: " + location(file, offset), fmt, ap);
    va_end(ap);
}

unsigned error_count() { return g_errors; }
unsigned warning_count() { return g_warnings; }

void reset_counts()
{
    g_errors = 0;
    g_warnings = 0;
}

} // namespace log
} // namespace tpmx
