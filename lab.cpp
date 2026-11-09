// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): rewritten on BinReader with bounds
// checks on the header, entry table, name table and data ranges; stored (default) or
// sequential (--legacy, the original's read) layout; safe output names; chunked copies; errors
// with file and offset instead of crashes; voice files (*.wv) also decoded to WAV (--sound).

#include "lab.h"

#include "binreader.h"
#include "log.h"
#include "platform.h"
#include "sound.h"
#include "textout.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace tpmx {

namespace {

const char kLabMagic[] = "LABN";
const size_t kCopyChunkSize = 64 * 1024;
// Entries that cannot be extracted are listed one by one up to this count, then only counted.
const unsigned kMaxListedEntryErrors = 20;

struct LabHeader
{
    uint32_t version = 0;
    uint32_t count = 0;
    uint32_t name_table_size = 0;
};

uint64_t entry_record_offset(size_t index) { return kLabHeaderSize + (uint64_t)index * kLabEntrySize; }

// Reads the 16-byte header and checks the counts against the limits and the file size.
LabHeader parse_header(BinReader& r, uint64_t file_size)
{
    if (!r.peek_tag(kLabMagic))
        r.fail("not a LAB archive (no \"LABN\" signature)");
    r.skip(4);
    LabHeader h;
    h.version = r.u32();
    const size_t count_at = r.pos();
    h.count = r.u32();
    const size_t names_at = r.pos();
    h.name_table_size = r.u32();
    if (h.count > kLabMaxEntries)
        r.fail_at(count_at, format("entry count %u exceeds the limit %u", h.count, kLabMaxEntries));
    const uint64_t table_at = entry_record_offset(h.count);
    if (table_at > file_size)
        r.fail_at(count_at, format("%u entries need %llu bytes, the file has %llu", h.count,
                                   (unsigned long long)table_at, (unsigned long long)file_size));
    if (h.name_table_size > kLabMaxNameTableSize)
        r.fail_at(names_at, format("name-table size %u exceeds the limit %u", h.name_table_size, kLabMaxNameTableSize));
    if (table_at + h.name_table_size > file_size)
        r.fail_at(names_at, format("name table (%u bytes at 0x%llX) runs past the end of the file (%llu bytes)",
                                   h.name_table_size, (unsigned long long)table_at, (unsigned long long)file_size));
    return h;
}

// Control and non-ASCII bytes as \xNN, for messages (names come from the archive).
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

std::string entry_label(size_t index, const LabEntry& e)
{
    if (e.name.empty())
        return format("entry %zu", index);
    return format("entry %zu \"%s\"", index, printable(e.name).c_str());
}

std::string upper_ascii(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    return u;
}

// fseek takes a long: 2 GiB on Windows and 32-bit systems, far beyond any LAB on the disc.
bool seek_to(FILE* f, uint64_t offset)
{
    return offset <= (uint64_t)LONG_MAX && std::fseek(f, (long)offset, SEEK_SET) == 0;
}

bool read_at(FILE* f, uint64_t offset, size_t size, std::vector<uint8_t>& out)
{
    out.resize(size);
    return seek_to(f, offset) && (size == 0 || std::fread(out.data(), 1, size, f) == size);
}

// Copies `length` bytes at `offset` of `in` to a new file `out_path`. A partly written file is
// removed again.
bool copy_range(FILE* in, uint64_t offset, uint64_t length, const std::string& out_path, std::vector<uint8_t>& buf,
                std::string& why)
{
    if (!seek_to(in, offset)) {
        why = format("cannot seek to offset 0x%llX", (unsigned long long)offset);
        return false;
    }
    FILE* out = platform::open_write(out_path);
    if (!out) {
        why = out_path + ": cannot create file: " + platform::errno_text();
        return false;
    }
    bool read_ok = true, write_ok = true;
    while (length > 0) {
        const size_t n = (size_t)std::min<uint64_t>(length, buf.size());
        if (std::fread(buf.data(), 1, n, in) != n) {
            read_ok = false;
            break;
        }
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            write_ok = false;
            break;
        }
        length -= n;
    }
    if (std::fclose(out) != 0)
        write_ok = false;
    if (read_ok && write_ok)
        return true;
    why = read_ok ? out_path + ": write error" : std::string("read error (the archive is shorter than its directory says)");
    platform::remove_file(out_path);
    return false;
}

void set_problem(LabEntry& e, uint64_t offset, const std::string& problem)
{
    if (e.problem.empty()) {
        e.problem = problem;
        e.problem_offset = offset;
    }
}

// A voice file by its name: "<something>.wv" in any letter case.
bool is_voice_name(const std::string& name)
{
    return name.size() > 3 && platform::iequals_ascii(name.substr(name.size() - 3), ".wv");
}

enum class VoiceResult { Decoded, Skipped, Failed };

// Decodes the voice file of entry `index` (already extracted) to <name>.wav in `out_dir`.
// `entry_names` holds every entry name in upper case; `decoded` the upper-cased WAV names
// written so far. Skipped (with a warning) when the archive has an entry of the WAV's name.
VoiceResult decode_voice_entry(FILE* in, const std::string& lab_path, size_t index, const LabEntry& e,
                               const std::string& out_dir, const std::set<std::string>& entry_names,
                               std::map<std::string, size_t>& decoded, std::vector<uint8_t>& buf)
{
    const std::string what = entry_label(index, e) + ": ";
    const std::string wav = sound_output_name(e.name, ".wav");
    const std::string up = upper_ascii(wav);
    const uint64_t record = entry_record_offset(index);
    if (entry_names.count(up)) {
        log::warn_at(lab_path, record, "%snot decoded: the archive has an entry named \"%s\"\n", what.c_str(),
                     printable(wav).c_str());
        return VoiceResult::Skipped;
    }
    std::string out, why;
    if (!platform::safe_output_path(out_dir, wav, out, &why)) {
        log::error_at(lab_path, record, "%s%s\n", what.c_str(), why.c_str());
        return VoiceResult::Failed;
    }
    const auto ins = decoded.insert(std::make_pair(up, index));
    if (!ins.second)
        log::warn_at(lab_path, record, "%sits decoded \"%s\" overwrites the one of entry %zu\n", what.c_str(),
                     printable(wav).c_str(), ins.first->second);
    if (e.length > kMaxSoundFileSize) {
        log::error_at(lab_path, e.read_offset, "%s%u bytes, more than the %llu bytes a voice file may have here\n",
                      what.c_str(), e.length, (unsigned long long)kMaxSoundFileSize);
        return VoiceResult::Failed;
    }
    if (!read_at(in, e.read_offset, e.length, buf)) {
        log::error_at(lab_path, e.read_offset, "%sread error\n", what.c_str());
        return VoiceResult::Failed;
    }
    return wv_write_wav(buf.data(), buf.size(), lab_path, e.read_offset, what, out) ? VoiceResult::Decoded
                                                                                  : VoiceResult::Failed;
}

} // namespace

std::string lab_type_name(const char type[4])
{
    std::string s;
    for (int i = 3; i >= 0; --i) {
        const unsigned char c = (unsigned char)type[i];
        s += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    return s;
}

bool parse_lab_directory(const uint8_t* data, size_t size, uint64_t file_size, const std::string& name,
                         LabLayout layout, LabDirectory& out, std::string* err)
{
    out = LabDirectory();
    out.file_size = file_size;
    out.layout = layout;
    try {
        BinReader r(data, size, name);
        const LabHeader h = parse_header(r, file_size);
        out.version = h.version;
        out.name_table_size = h.name_table_size;
        out.entries.resize(h.count);
        for (LabEntry& e : out.entries) {
            e.name_offset = r.u32();
            e.data_offset = r.u32();
            e.length = r.u32();
            r.bytes(e.type, 4);
        }
        const uint64_t table_at = r.file_offset();
        const size_t table_size = h.name_table_size;
        const char* table = reinterpret_cast<const char*>(r.span(table_size));

        // The sequential layout: name i starts after the NUL of name i-1; the data of entry 0
        // starts right after the last name and every entry's data follows the previous one.
        std::vector<uint32_t> seq_name(h.count);
        size_t pos = 0;
        uint32_t seq_names = 0;  // names the sequential walk found
        for (; seq_names < h.count; seq_names++) {
            const void* nul = pos < table_size ? std::memchr(table + pos, 0, table_size - pos) : nullptr;
            if (!nul)
                break;
            seq_name[seq_names] = (uint32_t)pos;
            pos = (size_t)(static_cast<const char*>(nul) - table) + 1;
        }
        if (layout == LabLayout::Sequential && seq_names < h.count)
            r.fail_at((size_t)(table_at + pos),
                      format("name %u of %u is not terminated inside the name table (%u bytes at 0x%llX)", seq_names,
                             h.count, h.name_table_size, (unsigned long long)table_at));

        uint64_t seq_data = table_at + pos;
        for (uint32_t i = 0; i < h.count; i++) {
            LabEntry& e = out.entries[i];
            const uint64_t record_at = entry_record_offset(i);
            if (i >= seq_names || e.name_offset != seq_name[i] || e.data_offset != seq_data)
                out.layout_mismatches++;

            const size_t name_at = layout == LabLayout::Sequential ? seq_name[i] : e.name_offset;
            if (name_at >= table_size) {
                set_problem(e, record_at,
                            format("name offset %u is outside the name table (%u bytes)", e.name_offset, h.name_table_size));
            } else {
                const void* nul = std::memchr(table + name_at, 0, table_size - name_at);
                if (!nul) {
                    set_problem(e, table_at + name_at, "name is not terminated inside the name table");
                } else {
                    e.name.assign(table + name_at, static_cast<const char*>(nul));
                    if (e.name.empty())
                        set_problem(e, table_at + name_at, "empty name");
                }
            }

            e.read_offset = layout == LabLayout::Sequential ? seq_data : e.data_offset;
            if (e.read_offset + e.length > file_size)
                set_problem(e, record_at,
                            format("data (%u bytes at 0x%llX) runs past the end of the file (%llu bytes)", e.length,
                                   (unsigned long long)e.read_offset, (unsigned long long)file_size));
            seq_data += e.length;
        }
    } catch (const ParseError& e) {
        if (err)
            *err = e.what();
        out.entries.clear();
        return false;
    }
    return true;
}

bool read_lab_directory(const std::string& path, LabLayout layout, LabDirectory& out, std::string* err)
{
    out = LabDirectory();
    uint64_t file_size = 0;
    if (!platform::file_size(path, file_size)) {
        if (err)
            *err = path + ": cannot open: not a readable file";
        return false;
    }
    FILE* f = platform::open_read(path);
    if (!f) {
        if (err)
            *err = path + ": cannot open: " + platform::errno_text();
        return false;
    }
    bool ok = false;
    try {
        // The header says how large the directory is; then read exactly that much.
        std::vector<uint8_t> buf;
        if (!read_at(f, 0, (size_t)std::min<uint64_t>(file_size, kLabHeaderSize), buf))
            throw ParseError(path, 0, "read error");
        BinReader r(buf, path);
        const LabHeader h = parse_header(r, file_size);
        const uint64_t dir_size = entry_record_offset(h.count) + h.name_table_size;
        if (!read_at(f, 0, (size_t)dir_size, buf))
            throw ParseError(path, 0, "read error");
        ok = parse_lab_directory(buf.data(), buf.size(), file_size, path, layout, out, err);
    } catch (const ParseError& e) {
        if (err)
            *err = e.what();
    }
    std::fclose(f);
    return ok;
}

int find_lab_entry(const LabDirectory& dir, const std::string& name)
{
    for (size_t i = 0; i < dir.entries.size(); i++)
        if (!dir.entries[i].name.empty() && platform::iequals_ascii(dir.entries[i].name, name))
            return (int)i;
    return -1;
}

bool extract_lab(const std::string& lab_path, const std::string& out_dir, const Options& opts)
{
    if (!platform::is_dir(out_dir)) {
        log::error("%s: output directory not found: %s\n", lab_path.c_str(), out_dir.c_str());
        return false;
    }
    const LabLayout layout = opts.legacy ? LabLayout::Sequential : LabLayout::Stored;
    LabDirectory dir;
    std::string why;
    if (!read_lab_directory(lab_path, layout, dir, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    const unsigned count = (unsigned)dir.entries.size();
    if (count == 0)
        log::warn("%s: the archive contains no entries\n", lab_path.c_str());
    if (dir.version != kLabVersion)
        log::warn_at(lab_path, 4, "LAB version 0x%X (expected 0x%X), reading it anyway\n", dir.version, kLabVersion);
    if (dir.layout_mismatches) {
        if (layout == LabLayout::Stored)
            log::warn("%s: %u of %u entries are not where a sequential read finds them; extracting from the "
                      "stored offsets (the original tool, and --legacy, read sequentially)\n",
                      lab_path.c_str(), dir.layout_mismatches, count);
        else
            log::warn("%s: %u of %u entries are stored elsewhere than a sequential read finds them; --legacy "
                      "reads sequentially as the original did (default mode uses the stored offsets)\n",
                      lab_path.c_str(), dir.layout_mismatches, count);
    }
    log::debug("%s: LAB version 0x%X, %u entries, name table %u bytes, %s layout\n", lab_path.c_str(), dir.version,
               count, dir.name_table_size, layout == LabLayout::Stored ? "stored" : "sequential");
    for (size_t i = 0; i < dir.entries.size(); i++) {
        const LabEntry& e = dir.entries[i];
        log::debug("  %5zu  %s  0x%08llX  %10u  %s\n", i, lab_type_name(e.type).c_str(),
                   (unsigned long long)e.read_offset, e.length, printable(e.name).c_str());
    }

    FILE* in = platform::open_read(lab_path);
    if (!in) {
        log::error("%s: cannot open: %s\n", lab_path.c_str(), platform::errno_text().c_str());
        return false;
    }
    std::vector<uint8_t> buf(kCopyChunkSize);
    std::map<std::string, size_t> seen;  // upper-cased name -> first entry with that name
    unsigned failed = 0;
    // Voice files are also decoded to WAV (not with --legacy, which pins --sound none).
    const bool decode = opts.sound == SoundMode::Wav;
    std::set<std::string> entry_names;
    std::map<std::string, size_t> decoded;
    std::vector<uint8_t> voice;
    unsigned decode_failed = 0, decoded_count = 0, decode_skipped = 0;
    if (decode)
        for (const LabEntry& e : dir.entries)
            if (!e.name.empty())
                entry_names.insert(upper_ascii(e.name));
    for (size_t i = 0; i < dir.entries.size(); i++) {
        const LabEntry& e = dir.entries[i];
        std::string problem = e.problem;
        uint64_t problem_at = e.problem_offset;
        std::string outfile;
        if (problem.empty() && !platform::safe_output_path(out_dir, e.name, outfile, &why)) {
            problem = why;
            problem_at = entry_record_offset(i);
        }
        if (problem.empty()) {
            const auto ins = seen.insert(std::make_pair(upper_ascii(e.name), i));
            if (!ins.second)
                log::warn_at(lab_path, entry_record_offset(i), "%s has the same name as entry %zu and overwrites it\n",
                             entry_label(i, e).c_str(), ins.first->second);
            log::verbose("Extracting %s\n", outfile.c_str());
            if (!copy_range(in, e.read_offset, e.length, outfile, buf, problem))
                problem_at = e.read_offset;
            if (problem.empty() && decode && is_voice_name(e.name)) {
                switch (decode_voice_entry(in, lab_path, i, e, out_dir, entry_names, decoded, voice)) {
                case VoiceResult::Decoded: decoded_count++; break;
                case VoiceResult::Skipped: decode_skipped++; break;
                case VoiceResult::Failed: decode_failed++; break;
                }
            }
        }
        if (!problem.empty()) {
            if (failed < kMaxListedEntryErrors)
                log::error_at(lab_path, problem_at, "%s: %s\n", entry_label(i, e).c_str(), problem.c_str());
            failed++;
        }
    }
    std::fclose(in);
    if (failed > kMaxListedEntryErrors)
        log::error("%s: %u more entries could not be extracted\n", lab_path.c_str(), failed - kMaxListedEntryErrors);
    if (decoded_count || decode_skipped || decode_failed)
        log::info("%s: %u voice file(s) decoded to WAV%s%s\n", lab_path.c_str(), decoded_count,
                  decode_skipped ? format(", %u not decoded (see the warnings)", decode_skipped).c_str() : "",
                  decode_failed ? format(", %u could not be decoded", decode_failed).c_str() : "");
    return failed == 0 && decode_failed == 0;
}

} // namespace tpmx
