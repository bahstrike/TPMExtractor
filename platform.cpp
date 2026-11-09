// TPMExtractor fork - portability layer: paths, directories, files, console.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// The original code (and the legacy paths of the fork) fwrite()s little-endian structs.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "TPMExtractor writes little-endian structures directly; big-endian hosts are not supported");
#endif

namespace tpmx {
namespace platform {

namespace {

inline unsigned char upper_ascii(unsigned char c) { return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c; }

// '/' and '\\' separate components of names that come from game data, on every OS.
inline bool is_data_separator(char c) { return c == '/' || c == '\\'; }

std::string strip_trailing_separators(const std::string& p)
{
    size_t n = p.size();
    while (n > 1 && is_separator(p[n - 1]))
        --n;
    return p.substr(0, n);
}

// Makes a name printable for error messages (control and non-ASCII bytes as \xNN).
std::string printable(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '\\') {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

bool is_reserved_device_name(const std::string& comp)
{
    std::string base = comp.substr(0, comp.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    if (base.size() != 3 && base.size() != 4)
        return false;
    std::string up;
    for (char ch : base)
        up += (char)upper_ascii((unsigned char)ch);
    if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL")
        return true;
    if (up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) && up[3] >= '1' &&
        up[3] <= '9')
        return true;
    return false;
}

#ifndef _WIN32
// Bytewise-smallest entry of `dir` equal to `name` ignoring ASCII case; "" if none.
std::string find_ci_entry(const std::string& dir, const std::string& name)
{
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d)
        return std::string();
    std::string best;
    bool found = false;
    while (dirent* e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        if (iequals_ascii(e->d_name, name)) {
            std::string n = e->d_name;
            if (!found || n < best) {
                best = n;
                found = true;
            }
        }
    }
    closedir(d);
    return best;
}
#endif

} // namespace

// ------------------------------------------------------------------ paths

bool is_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

std::string path_join(const std::string& a, const std::string& b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    if (is_separator(a.back()))
        return a + b;
    return a + "/" + b;
}

std::string base_name(const std::string& path)
{
    size_t i = path.size();
    while (i > 0 && !is_separator(path[i - 1]))
        --i;
    return path.substr(i);
}

std::string dir_name(const std::string& path)
{
    size_t i = path.size();
    while (i > 0 && !is_separator(path[i - 1]))
        --i;
    while (i > 0 && is_separator(path[i - 1]))
        --i;
    if (i == 0 && !path.empty() && is_separator(path[0]))
        return path.substr(0, 1);
    return path.substr(0, i);
}

bool iequals_ascii(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (upper_ascii((unsigned char)a[i]) != upper_ascii((unsigned char)b[i]))
            return false;
    return true;
}

std::string resolve_ci(const std::string& base, const std::string& rel)
{
    std::string out = base;
    size_t i = 0;
    while (i < rel.size()) {
        size_t j = i;
        while (j < rel.size() && !is_data_separator(rel[j]))
            ++j;
        std::string comp = rel.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".")
            continue;
        std::string cand = path_join(out, comp);
#ifndef _WIN32
        if (comp != "..") {
            struct stat st;
            if (lstat(cand.c_str(), &st) != 0 && errno == ENOENT) {
                std::string match = find_ci_entry(out, comp);
                if (!match.empty())
                    cand = path_join(out, match);
            }
        }
#endif
        out = cand;
    }
    return out;
}

std::string find_ci(const std::string& path)
{
#ifdef _WIN32
    return path;
#else
    if (path.empty() || exists(path))
        return path;
    const std::string base = base_name(path);
    if (base.empty() || base == "." || base == "..")
        return path;
    const std::string dir = path.substr(0, path.size() - base.size());  // keeps its separator
    const std::string match = find_ci_entry(dir.empty() ? std::string(".") : dir, base);
    return match.empty() ? path : dir + match;
#endif
}

bool safe_output_path(const std::string& root, const std::string& name, std::string& out, std::string* why,
                      bool allow_subdirs)
{
    auto fail = [&](const char* reason) {
        if (why)
            *why = "unsafe output name \"" + printable(name) + "\": " + reason;
        return false;
    };
    if (name.empty())
        return fail("empty name");
    if (is_data_separator(name[0]))
        return fail("absolute path");
    if (name.size() >= 2 && name[1] == ':')
        return fail("drive-qualified path");

    std::vector<std::string> comps;
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < name.size() && !is_data_separator(name[j]))
            ++j;
        comps.push_back(name.substr(i, j - i));
        if (j >= name.size())
            break;
        i = j + 1;
    }
    if (!allow_subdirs && comps.size() != 1)
        return fail("contains a path separator");

    std::string rel;
    for (const std::string& c : comps) {
        if (c.empty())
            return fail("empty path component");
        if (c == "." || c == "..")
            return fail("'.' or '..' component");
        if (c.size() > 255)
            return fail("component longer than 255 bytes");
        for (unsigned char ch : c) {
            if (ch < 0x20)
                return fail("control character");
            if (std::strchr("<>:\"|?*", ch))
                return fail("character not allowed in Windows file names");
        }
        if (c.back() == '.' || c.back() == ' ')
            return fail("component ends in '.' or ' '");
        if (is_reserved_device_name(c))
            return fail("Windows device name");
        rel = rel.empty() ? c : rel + "/" + c;
    }
    out = resolve_ci(root, rel);
    return true;
}

// ---------------------------------------------------------------- queries

#ifdef _WIN32

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool is_dir(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_file(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool file_size(const std::string& path, uint64_t& out)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d))
        return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    out = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
}

#else

bool exists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 || lstat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool file_size(const std::string& path, uint64_t& out)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    out = (uint64_t)st.st_size;
    return true;
}

#endif

// ------------------------------------------------------------ directories

MkdirResult make_dir(const std::string& path0, std::string* err)
{
    const std::string path = strip_trailing_separators(path0);
    if (path.empty()) {
        if (err)
            *err = "cannot create a directory with an empty name";
        return MkdirResult::ParentMissing;
    }
#ifdef _WIN32
    if (CreateDirectoryA(path.c_str(), nullptr))
        return MkdirResult::Created;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS)
        return MkdirResult::AlreadyExists;
    if (err)
        *err = path + ": cannot create directory (Win32 error " + std::to_string((unsigned long)e) + ")";
    return e == ERROR_PATH_NOT_FOUND ? MkdirResult::ParentMissing : MkdirResult::Failed;
#else
    if (mkdir(path.c_str(), 0777) == 0)
        return MkdirResult::Created;
    int e = errno;
    if (e == EEXIST)
        return MkdirResult::AlreadyExists;
    if (err)
        *err = path + ": cannot create directory: " + std::strerror(e);
    return (e == ENOENT || e == ENOTDIR) ? MkdirResult::ParentMissing : MkdirResult::Failed;
#endif
}

bool make_dirs(const std::string& path0, std::string* err)
{
    const std::string path = strip_trailing_separators(path0);
    if (path.empty() || is_dir(path))
        return true;
    const std::string parent = dir_name(path);
    if (!parent.empty() && parent != path && !is_dir(parent)) {
        if (!make_dirs(parent, err))
            return false;
    }
    std::string why;
    MkdirResult r = make_dir(path, &why);
    if (r == MkdirResult::Created || (r == MkdirResult::AlreadyExists && is_dir(path)))
        return true;
    if (err)
        *err = why.empty() ? path + ": exists and is not a directory" : why;
    return false;
}

bool remove_file(const std::string& path, std::string* err)
{
#ifdef _WIN32
    if (is_dir(path)) {
        if (err)
            *err = path + ": is a directory";
        return false;
    }
    if (DeleteFileA(path.c_str()))
        return true;
    if (err)
        *err = path + ": cannot delete (Win32 error " + std::to_string((unsigned long)GetLastError()) + ")";
    return false;
#else
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        if (err)
            *err = path + ": " + errno_text();
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (err)
            *err = path + ": is a directory";
        return false;
    }
    if (unlink(path.c_str()) == 0)
        return true;
    if (err)
        *err = path + ": " + errno_text();
    return false;
#endif
}

bool glob_match_ci(const char* p, const char* s)
{
    const char* star = nullptr;
    const char* resume = nullptr;
    while (*s) {
        if (*p == '?' || (*p && *p != '*' && upper_ascii((unsigned char)*p) == upper_ascii((unsigned char)*s))) {
            ++p;
            ++s;
        } else if (*p == '*') {
            star = p++;
            resume = s;
        } else if (star) {
            p = star + 1;
            s = ++resume;
        } else {
            return false;
        }
    }
    while (*p == '*')
        ++p;
    return *p == 0;
}

bool ntfs_less(const std::string& a, const std::string& b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = upper_ascii((unsigned char)a[i]), y = upper_ascii((unsigned char)b[i]);
        if (x != y)
            return x < y;
    }
    if (a.size() != b.size())
        return a.size() < b.size();
    return a < b;
}

std::vector<DirEntry> list_dir(const std::string& dir, const std::string& pattern0, std::string* err)
{
    std::vector<DirEntry> out;
    const std::string pattern = pattern0 == "*.*" ? std::string("*") : pattern0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path_join(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && err)
            *err = dir + ": cannot list directory (Win32 error " + std::to_string((unsigned long)e) + ")";
        return out;
    }
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0)
            continue;
        if (!glob_match_ci(pattern.c_str(), fd.cFileName))
            continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d) {
        if (err)
            *err = dir + ": cannot list directory: " + errno_text();
        return out;
    }
    while (dirent* de = readdir(d)) {
        if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0)
            continue;
        if (!glob_match_ci(pattern.c_str(), de->d_name))
            continue;
        DirEntry e;
        e.name = de->d_name;
        e.is_dir = is_dir(path_join(dir, e.name));
        out.push_back(e);
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return ntfs_less(a.name, b.name); });
    return out;
}

// --------------------------------------------------------------- file I/O

FILE* open_read(const std::string& path) { return std::fopen(path.c_str(), "rb"); }

FILE* open_write(const std::string& path) { return std::fopen(path.c_str(), "wb"); }

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string* err)
{
    out.clear();
    FILE* f = open_read(path);
    if (!f) {
        if (err)
            *err = path + ": " + errno_text();
        return false;
    }
    uint64_t size = 0;
    if (file_size(path, size) && size < ((uint64_t)1 << 31))
        out.reserve((size_t)size);
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        if (err)
            *err = path + ": read error";
        return false;
    }
    return true;
}

bool write_file(const std::string& path, const void* data, size_t size, std::string* err)
{
    FILE* f = open_write(path);
    if (!f) {
        if (err)
            *err = path + ": cannot create file: " + errno_text();
        return false;
    }
    bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok && err)
        *err = path + ": write error";
    return ok;
}

std::string errno_text() { return std::strerror(errno); }

// ---------------------------------------------------------------- console

bool owns_console()
{
#ifdef _WIN32
    DWORD pids[2];
    return GetConsoleProcessList(pids, 2) == 1;
#else
    return false;
#endif
}

void wait_key()
{
#ifdef _WIN32
    while (!_getch())
        Sleep(10);
#else
    int c;
    while ((c = std::getchar()) != EOF && c != '\n') {
    }
#endif
}

} // namespace platform
} // namespace tpmx
