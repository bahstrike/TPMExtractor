// TPMExtractor fork - InstallShield v3 (.Z) extraction glue around unshieldv3.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Replaces unshieldv3's extract() (unshieldv3/unshieldv3.cpp, still built into the static
// library, no longer called). The table of contents comes from ISArchiveV3; the decompression
// is done here instead of through ISArchiveV3::decompress(), which reads the whole compressed
// file into memory, collects the whole output in memory (about 200 MB for BIG.Z), and hands
// blast() the same input buffer again whenever it asks for more, so a truncated stream decodes
// garbage instead of failing. Here blast() gets the file's bytes once, in chunks, and the output
// goes to disk as it is produced, capped at the size the table of contents records.

#include "zextract.h"

#include "log.h"
#include "platform.h"
#include "textout.h"
#include "unshieldv3/ISArchiveV3.h"

extern "C" {
#include "unshieldv3/blast.h"
}

#include <algorithm>
#include <climits>
#include <cstdio>
#include <exception>
#include <map>

namespace tpmx {

namespace {

const size_t kChunkSize = 64 * 1024;
const uint8_t kAttribUncompressed = ISArchiveV3::File::UNCOMPRESSED;  // 0x10: stored as is

// Control and non-ASCII bytes as \xNN, for messages (names come from the archive).
std::string printable(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c >= 0x7F || c == '"') {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

std::string upper_ascii(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c == '\\')
            c = '/';
    return u;
}

// fseek takes a long: 2 GiB on Windows and 32-bit systems. InstallShield v3 offsets are 32-bit
// and BIG.Z on the retail disc is about 81 MB.
bool seek_to(FILE* f, uint64_t offset)
{
    return offset <= (uint64_t)LONG_MAX && std::fseek(f, (long)offset, SEEK_SET) == 0;
}

// blast() input: the file's compressed bytes, chunk by chunk, each byte exactly once. Returning
// 0 at the end makes blast() stop with "out of input" (2) instead of reading further.
struct BlastInput
{
    FILE* f = nullptr;
    uint64_t remaining = 0;  // compressed bytes not handed to blast() yet
    bool read_error = false;
    std::vector<unsigned char> buf;
};

unsigned blast_input(void* how, unsigned char** buf)
{
    BlastInput* in = static_cast<BlastInput*>(how);
    if (in->remaining == 0 || in->read_error)
        return 0;
    const size_t want = (size_t)std::min<uint64_t>(in->remaining, in->buf.size());
    const size_t got = std::fread(in->buf.data(), 1, want, in->f);
    if (got != want)
        in->read_error = true;
    in->remaining -= got;
    *buf = in->buf.data();
    return (unsigned)got;
}

// blast() output: straight to the file, refusing to go past `limit` bytes.
struct BlastOutput
{
    FILE* f = nullptr;
    uint64_t written = 0;
    uint64_t limit = 0;
    bool too_large = false;
    bool write_error = false;
};

int blast_output(void* how, unsigned char* buf, unsigned len)
{
    BlastOutput* out = static_cast<BlastOutput*>(how);
    if (len > out->limit - out->written) {
        out->too_large = true;
        return 1;
    }
    if (std::fwrite(buf, 1, len, out->f) != len) {
        out->write_error = true;
        return 1;
    }
    out->written += len;
    return 0;
}

std::string blast_error_text(int ret)
{
    switch (ret) {
    case 2: return "the compressed data ends before the end of the stream";
    case -1: return "invalid compressed data (literal flag not 0 or 1)";
    case -2: return "invalid compressed data (dictionary size not 4, 5 or 6)";
    case -3: return "invalid compressed data (distance too far back)";
    default: return format("decompression failed (blast error %d)", ret);
    }
}

// Writes the archive file `file` to `dest`. On failure sets `why` and removes `dest`.
bool extract_one(FILE* archive, const ISArchiveV3::File& file, const std::string& dest, uint64_t& written,
                 std::string& why)
{
    written = 0;
    if (!seek_to(archive, file.offset)) {
        why = format("cannot seek to offset 0x%X", (unsigned)file.offset);
        return false;
    }
    FILE* out = platform::open_write(dest);
    if (!out) {
        why = dest + ": cannot create file: " + platform::errno_text();
        return false;
    }

    bool ok = true;
    if (file.attrib & kAttribUncompressed) {
        std::vector<unsigned char> buf(kChunkSize);
        uint64_t left = file.compressed_size;
        while (ok && left > 0) {
            const size_t n = (size_t)std::min<uint64_t>(left, buf.size());
            if (std::fread(buf.data(), 1, n, archive) != n) {
                why = "read error";
                ok = false;
            } else if (std::fwrite(buf.data(), 1, n, out) != n) {
                why = dest + ": write error";
                ok = false;
            } else {
                left -= n;
                written += n;
            }
        }
        if (ok && file.uncompressed_size != file.compressed_size)
            log::warn("%s: stored file of %u bytes, but the table of contents says %u bytes uncompressed\n",
                      printable(file.full_path).c_str(), (unsigned)file.compressed_size,
                      (unsigned)file.uncompressed_size);
    } else {
        BlastInput in;
        in.f = archive;
        in.remaining = file.compressed_size;
        in.buf.resize(kChunkSize);
        BlastOutput o;
        o.f = out;
        o.limit = file.uncompressed_size;
        unsigned unused = 0;
        const int ret = blast(blast_input, &in, blast_output, &o, &unused, nullptr);
        written = o.written;
        if (ret != 0) {
            ok = false;
            if (o.too_large)
                why = format("decompresses to more than the %u bytes the table of contents records",
                             (unsigned)file.uncompressed_size);
            else if (o.write_error)
                why = dest + ": write error";
            else if (in.read_error)
                why = "read error";
            else
                why = blast_error_text(ret);
        } else if (o.written != file.uncompressed_size) {
            ok = false;
            why = format("decompressed to %llu bytes, but the table of contents records %u",
                         (unsigned long long)o.written, (unsigned)file.uncompressed_size);
        } else if (unused + in.remaining) {
            log::debug("%s: %llu compressed bytes after the end of the stream ignored\n",
                       printable(file.full_path).c_str(), (unsigned long long)(unused + in.remaining));
        }
    }

    if (std::fclose(out) != 0 && ok) {
        why = dest + ": write error";
        ok = false;
    }
    if (!ok)
        platform::remove_file(dest);
    return ok;
}

} // namespace

bool extract_z(const std::string& z_path, const std::string& out_dir, const Options& opts,
               std::vector<std::string>* written)
{
    (void)opts;  // both modes extract the same bytes
    if (!platform::is_dir(out_dir)) {
        log::error("%s: output directory not found: %s\n", z_path.c_str(), out_dir.c_str());
        return false;
    }
    uint64_t archive_size = 0;
    if (!platform::file_size(z_path, archive_size)) {
        log::error("%s: cannot open: not a readable file\n", z_path.c_str());
        return false;
    }

    std::vector<ISArchiveV3::File> files;
    ISArchiveV3::Header header = ISArchiveV3::Header();
    try {
        ISArchiveV3 archive(z_path);
        files = archive.files();
        header = archive.header();
    } catch (const std::exception& e) {
        log::error("%s: %s\n", z_path.c_str(), e.what());
        return false;
    }
    if (header.is_multivolume)
        log::warn("%s: part of a multi-volume archive; only files stored completely in it can be extracted\n",
                  z_path.c_str());
    if (files.empty())
        log::warn("%s: the archive contains no files\n", z_path.c_str());
    log::debug("%s: %u directories, %zu files, table of contents at 0x%X\n", z_path.c_str(),
               (unsigned)header.dir_count, files.size(), (unsigned)header.toc_address);

    FILE* in = platform::open_read(z_path);
    if (!in) {
        log::error("%s: cannot open: %s\n", z_path.c_str(), platform::errno_text().c_str());
        return false;
    }
    std::map<std::string, size_t> seen;  // normalised archive path -> first file with it
    unsigned failed = 0;
    for (size_t i = 0; i < files.size(); i++) {
        const ISArchiveV3::File& file = files[i];
        const std::string shown = printable(file.full_path);
        log::info("%s\n", shown.c_str());
        log::info("      Compressed size: %10u\n", (unsigned)file.compressed_size);

        std::string dest, why;
        uint64_t size = 0;
        bool ok = false;
        if (file.is_split) {
            why = "the file is split across volumes, which is not supported";
        } else if ((uint64_t)file.offset + file.compressed_size > archive_size) {
            why = format("data (%u bytes at 0x%X) runs past the end of the archive (%llu bytes)",
                         (unsigned)file.compressed_size, (unsigned)file.offset, (unsigned long long)archive_size);
        } else if (platform::safe_output_path(out_dir, file.full_path, dest, &why, true)) {
            const std::string parent = platform::dir_name(dest);
            if (parent.empty() || platform::make_dirs(parent, &why)) {
                const auto ins = seen.insert(std::make_pair(upper_ascii(file.full_path), i));
                if (!ins.second)
                    log::warn("%s: \"%s\" is in the archive more than once; the later copy overwrites the earlier\n",
                              z_path.c_str(), shown.c_str());
                ok = extract_one(in, file, dest, size, why);
            }
        }
        if (!ok) {
            log::error_at(z_path, file.offset, "%s: %s\n", shown.c_str(), why.c_str());
            failed++;
            continue;
        }
        log::info("    Uncompressed size: %10llu\n", (unsigned long long)size);
        if (written)
            written->push_back(dest);
    }
    std::fclose(in);
    return failed == 0;
}

} // namespace tpmx
