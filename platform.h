// TPMExtractor fork - portability layer: paths, directories, files, console.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Replaces every Win32 call of the original tool (CreateDirectory, DeleteFile,
// FindFirstFile/FindNextFile, _getch/Sleep). Paths are narrow strings everywhere: on
// Windows they go to the ANSI ("A") APIs and fopen, exactly like the original
// MultiByte build; on POSIX they are passed through unchanged.
//
// '/' is used as the separator in every path this module builds; Windows accepts it.
// '\\' is also a separator on Windows, and in names that come from game data on every OS
// (see safe_output_path).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tpmx {
namespace platform {

// ------------------------------------------------------------------ paths

// True for '/' everywhere, and for '\\' on Windows.
bool is_separator(char c);

// a + "/" + b, without doubling a separator that `a` already ends with. Returns b if a is
// empty. "H:" + "x" gives "H:/x" (never the drive-relative "H:x").
std::string path_join(const std::string& a, const std::string& b);

// Final component of `path` (everything after the last separator).
std::string base_name(const std::string& path);

// Everything before the final component, without trailing separators ("" if there is none).
std::string dir_name(const std::string& path);

// ASCII-only case-insensitive comparison (bytes >= 0x80 must match exactly).
bool iequals_ascii(const std::string& a, const std::string& b);

// Case-insensitive path resolution (ASCII, as NTFS does for ASCII names).
// `base` is used verbatim. Every component of `rel` ('/' or '\\' separated; empty and "."
// components are dropped) that does not exist with exactly that spelling is replaced by an
// existing directory entry that matches it ignoring ASCII case (the bytewise-smallest if
// several do). A component that does not exist in any spelling is kept as written, so a new
// file gets exactly the requested name.
// On Windows the file system already ignores case: the result is path_join(base, rel) with
// the separators of `rel` normalised to '/'.
std::string resolve_ci(const std::string& base, const std::string& rel);

// For input files whose name is fixed by the format (e.g. "GAMEDATA/LEVEL/ASSAULT.B3D"):
// returns `path` if it exists as written; otherwise `path` with its final component replaced
// by a directory entry that matches it ignoring ASCII case (as resolve_ci); otherwise `path`
// unchanged. The directory part is used as written. Lets callers keep deriving output names
// from the canonical spelling, as the original did on Windows, while opening the real file.
std::string find_ci(const std::string& path);

// Builds the output path for a file or directory name that comes from game data
// (LAB entry names, MAT/model/key names, InstallShield paths, ...) and checks it is safe:
//   - rejected: empty names; absolute names (leading '/' or '\\'); drive-qualified names
//     ("C:..."); empty, "." and ".." components; control characters (< 0x20); any of
//     < > : " | ? * ; components ending in '.' or ' ' (Win32 strips them); Windows device
//     names (CON, PRN, AUX, NUL, COM1-COM9, LPT1-LPT9, also with an extension);
//     components longer than 255 bytes;
//   - '/' and '\\' are separators in `name` on every OS; a name with a separator is
//     rejected unless allow_subdirs is true (the caller must then create the parents).
// The checks are the same on every OS, so Linux and Windows accept the same names.
// The result is resolve_ci(root, name): a name that differs only in case from an existing
// entry reuses (overwrites) it, as the original did on NTFS.
// On failure returns false, leaves `out` unchanged and sets *why (if given).
bool safe_output_path(const std::string& root, const std::string& name, std::string& out,
                      std::string* why, bool allow_subdirs = false);

// ---------------------------------------------------------------- queries

bool exists(const std::string& path);   // anything (file, directory, ...)
bool is_dir(const std::string& path);   // follows symlinks
bool is_file(const std::string& path);  // regular file, follows symlinks
bool file_size(const std::string& path, uint64_t& out);

// ------------------------------------------------------------ directories

enum class MkdirResult {
    Created,
    AlreadyExists,  // something (file or directory) with that name exists
    ParentMissing,  // a parent component does not exist (Win32 ERROR_PATH_NOT_FOUND)
    Failed,         // anything else (permissions, invalid name, ...)
};

// Creates one directory level, with Win32 CreateDirectory semantics (see MkdirResult).
// Trailing separators are ignored.
MkdirResult make_dir(const std::string& path, std::string* err = nullptr);

// Creates `path` and any missing parents (mkdir -p). True if it is a directory afterwards.
bool make_dirs(const std::string& path, std::string* err = nullptr);

// Deletes a file (never a directory). False if it is missing, a directory, or not deletable.
bool remove_file(const std::string& path, std::string* err = nullptr);

struct DirEntry
{
    std::string name;
    bool is_dir = false;
};

// Lists the entries of `dir` whose names match the glob `pattern`, without "." and "..".
// Glob: '*' and '?', ASCII case-insensitive; the Win32 special case "*.*" means "*".
// Sorted in NTFS order (see ntfs_less), the order the original FindFirstFile loop saw.
// Not emulated: Windows 8.3 short-name matches (on Windows "*.baf" also matches
// "x.bafx" through its short name X~1.BAF).
// Returns an empty list and sets *err if the directory cannot be read.
std::vector<DirEntry> list_dir(const std::string& dir, const std::string& pattern,
                               std::string* err = nullptr);

// The glob used by list_dir.
bool glob_match_ci(const char* pattern, const char* name);

// NTFS directory order for ASCII names: compare after ASCII upper-casing, unsigned bytes;
// a name that is a prefix of another sorts first; names equal ignoring case fall back to
// plain byte order (only possible on case-sensitive file systems).
bool ntfs_less(const std::string& a, const std::string& b);

// --------------------------------------------------------------- file I/O

// fopen in binary mode ("rb" / "wb"). nullptr on failure.
FILE* open_read(const std::string& path);
FILE* open_write(const std::string& path);

// Whole-file helpers. On failure they return false and set *err to "<path>: <reason>".
bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string* err = nullptr);
bool write_file(const std::string& path, const void* data, size_t size, std::string* err = nullptr);
inline bool write_file(const std::string& path, const std::vector<uint8_t>& data,
                       std::string* err = nullptr)
{
    return write_file(path, data.data(), data.size(), err);
}

// Text for the current errno (portable strerror).
std::string errno_text();

// ---------------------------------------------------------------- console

// True on Windows when this process is the only one attached to its console, i.e. it was
// started from Explorer and its window closes when it exits. Always false elsewhere.
bool owns_console();

// "Press any key": one _getch() on Windows; elsewhere it waits for Enter (or EOF).
void wait_key();

} // namespace platform
} // namespace tpmx
