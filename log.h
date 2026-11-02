// TPMExtractor fork - console logging with verbosity levels and error counting.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// info/verbose/debug go to stdout (where the original printed its progress); warnings and
// errors go to stderr and are counted, so main() can choose the exit code. Messages are
// formatted with tpmx::format (textout.h). Callers end lines with '\n' themselves.

#pragma once

#include "textout.h"

#include <cstdint>
#include <string>

namespace tpmx {
namespace log {

// -1 quiet (errors and warnings only), 0 normal, 1 verbose, 2 debug.
void set_verbosity(int level);
int verbosity();

void info(const char* fmt, ...) TPMX_PRINTF(1, 2);     // level >= 0
void verbose(const char* fmt, ...) TPMX_PRINTF(1, 2);  // level >= 1
void debug(const char* fmt, ...) TPMX_PRINTF(1, 2);    // level >= 2
void warn(const char* fmt, ...) TPMX_PRINTF(1, 2);     // "warning: ..." on stderr, counted
void error(const char* fmt, ...) TPMX_PRINTF(1, 2);    // "error: ..." on stderr, counted

// "error: <file> @0x<offset>: <message>" - the form every parse error should take.
void error_at(const std::string& file, uint64_t offset, const char* fmt, ...) TPMX_PRINTF(3, 4);
void warn_at(const std::string& file, uint64_t offset, const char* fmt, ...) TPMX_PRINTF(3, 4);

unsigned error_count();
unsigned warning_count();
void reset_counts();

} // namespace log
} // namespace tpmx
