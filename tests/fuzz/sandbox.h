// TPMExtractor fork - a scratch folder for the file-level fuzz targets, with containment checks.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Test code only (tests/fuzz_*.cpp). Each fuzz process gets one folder of its own:
//
//     <base>/tpmx-fuzz-<target>-<random>/
//         in/<input files>      what the target writes for the tool to read
//         w1/w2/out/            the output folder the tool is given
//
// <base> is TPMX_FUZZ_TMP if set, else the system temporary folder (TMPDIR on POSIX). A RAM
// file system such as /dev/shm makes the targets much faster.
//
// check() aborts (so libFuzzer and the replay driver report the input) when the run left
// anything outside out/: a stray entry in the root, in/, w1/ or w2/ (an output name with up to
// three ".." levels lands there), a changed input, or something in out/ that is neither a plain
// file nor a folder (a symlink, say). It returns what out/ holds, for the target's own rules.
// clean_out() empties out/; the destructor removes the whole folder.

#pragma once

#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace tpmx_fuzz {

namespace fs = std::filesystem;

[[noreturn]] inline void violation(const char* what, const std::string& detail)
{
    std::fprintf(stderr, "fuzz check failed: %s: %s\n", what, detail.c_str());
    std::fflush(stderr);
    std::abort();
}

struct OutEntry
{
    std::string path;     // relative to out/, '/'-separated
    bool is_dir = false;  // else a regular file (anything else fails the check)
};

class Sandbox
{
public:
    explicit Sandbox(const char* target)
    {
        std::error_code ec;
        fs::path base;
        const char* env = std::getenv("TPMX_FUZZ_TMP");
        if (env && *env)
            base = env;
        else
            base = fs::temp_directory_path(ec);
        if (ec || base.empty())
            violation("no temporary folder", ec.message());
        std::random_device rd;
        for (int i = 0; i < 1000 && root_.empty(); i++) {
            const fs::path r = base / ("tpmx-fuzz-" + std::string(target) + "-" + std::to_string(rd()) + "-" +
                                       std::to_string(i));
            if (fs::create_directory(r, ec))
                root_ = r;
        }
        if (root_.empty())
            violation("cannot create a sandbox folder in", base.string());
        in_ = root_ / "in";
        out_ = root_ / "w1" / "w2" / "out";
        fs::create_directories(in_, ec);
        if (!ec)
            fs::create_directories(out_, ec);
        if (ec)
            violation("cannot create the sandbox folders", root_.string() + ": " + ec.message());
        out_str_ = out_.string();
    }

    ~Sandbox()
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    // The output folder, as a string for the tool's entry points.
    const std::string& out() const { return out_str_; }

    // Writes in/<name> (replacing it) and returns its path.
    std::string put(const std::string& name, const uint8_t* data, size_t size)
    {
        const std::string path = (in_ / name).string();
        std::string err;
        if (!tpmx::platform::write_file(path, data, size, &err))
            violation("cannot write the input", err);
        inputs_[name] = size;
        return path;
    }

    // Checks containment (see the top of this file) and returns everything below out/.
    std::vector<OutEntry> check() const
    {
        expect_only(root_, {"in", "w1"});
        expect_only(root_ / "w1", {"w2"});
        expect_only(root_ / "w1" / "w2", {"out"});
        std::set<std::string> names;
        for (const auto& kv : inputs_)
            names.insert(kv.first);
        expect_only(in_, names);
        for (const auto& kv : inputs_) {
            std::error_code ec;
            const fs::path p = in_ / kv.first;
            const fs::file_status st = fs::symlink_status(p, ec);
            bool same = !ec && fs::is_regular_file(st);
            if (same) {
                const uintmax_t size = fs::file_size(p, ec);
                same = !ec && size == (uintmax_t)kv.second;
            }
            if (!same)
                violation("an input file was changed", p.string());
        }

        std::vector<OutEntry> out;
        std::error_code ec;
        fs::recursive_directory_iterator it(out_, fs::directory_options::none, ec), end;
        if (ec)
            violation("cannot list the output folder", out_.string() + ": " + ec.message());
        for (; it != end; it.increment(ec)) {
            if (ec)
                violation("cannot list the output folder", out_.string() + ": " + ec.message());
            const fs::file_status st = it->symlink_status(ec);
            if (ec)
                violation("cannot stat", it->path().string());
            OutEntry e;
            e.path = it->path().lexically_relative(out_).generic_string();
            if (fs::is_directory(st))
                e.is_dir = true;
            else if (!fs::is_regular_file(st))
                violation("something other than a file or a folder in the output", it->path().string());
            out.push_back(e);
        }
        return out;
    }

    // True if `path` (as the tool reports it) names something inside out/; sets `rel` to the
    // part after out/ with '/' separators.
    bool inside_out(const std::string& path, std::string& rel) const
    {
        std::string p = path, o = out_str_;
        for (std::string* s : {&p, &o})
            for (char& c : *s)
                if (c == '\\')
                    c = '/';
        if (p.size() <= o.size() + 1 || p.compare(0, o.size(), o) != 0 || p[o.size()] != '/')
            return false;
        rel = p.substr(o.size() + 1);
        size_t start = 0;
        while (start <= rel.size()) {  // no empty, "." or ".." component
            const size_t slash = rel.find('/', start);
            const std::string c = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (c.empty() || c == "." || c == "..")
                return false;
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        return true;
    }

    // Removes everything below out/.
    void clean_out() const
    {
        std::error_code ec;
        for (fs::directory_iterator it(out_, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code rm;
            fs::remove_all(it->path(), rm);
            if (rm)
                violation("cannot clean the output folder", it->path().string() + ": " + rm.message());
        }
        if (ec)
            violation("cannot list the output folder", out_.string() + ": " + ec.message());
    }

private:
    static void expect_only(const fs::path& dir, const std::set<std::string>& names)
    {
        std::error_code ec;
        std::set<std::string> seen;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (!names.count(n))
                violation("written outside the output folder", it->path().string());
            seen.insert(n);
        }
        if (ec)
            violation("cannot list", dir.string() + ": " + ec.message());
        if (seen != names)
            violation("a sandbox entry was removed in", dir.string());
    }

    fs::path root_, in_, out_;
    std::string out_str_;
    std::map<std::string, size_t> inputs_;  // in/ file name -> size written
};

} // namespace tpmx_fuzz
