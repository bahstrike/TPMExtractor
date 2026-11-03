// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): new entry point signature with Options;
// public bounds-checked LAB directory parser (stored or sequential layout) and a
// case-insensitive entry lookup.

#pragma once

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

// LAB archive layout (little-endian):
//   header      16 bytes: "LABN", u32 version (0x10000), u32 entry count, u32 name-table size
//   entries     16 bytes each: u32 name offset (relative to the start of the name table),
//               u32 data offset (absolute), u32 length, char[4] type (a fourCC stored
//               byte-reversed: "TRPS" is SPRT)
//   name table  NUL-terminated names, `name-table size` bytes
//   data
// On the retail disc (big.lab, LOCALIZE.LAB) names and data are stored in entry order with no
// gaps, so every stored offset equals the position a sequential read finds.

const uint32_t kLabVersion = 0x10000;
const size_t kLabHeaderSize = 16;
const size_t kLabEntrySize = 16;
const uint32_t kLabMaxEntries = 1000000;                // sanity limit for the entry count
const uint32_t kLabMaxNameTableSize = 64u * 1024 * 1024;  // sanity limit for the name table

// Where the names and the data of the entries are read from.
enum class LabLayout {
    Stored,      // at the stored name and data offsets (the format; default mode)
    Sequential,  // one after another: names from the start of the name table, data from just
                 // after the last name, in entry order. What the original tool read; --legacy.
};

struct LabEntry
{
    std::string name;               // from the name table ("" when `problem` says why not)
    uint32_t name_offset = 0;       // as stored
    uint32_t data_offset = 0;       // as stored
    uint32_t length = 0;            // as stored
    char type[4] = {0, 0, 0, 0};    // as stored (byte-reversed fourCC)
    uint64_t read_offset = 0;       // where the data is read from, per the LabLayout
    std::string problem;            // why the entry cannot be extracted; "" if it can
    uint64_t problem_offset = 0;    // file offset the problem refers to
};

struct LabDirectory
{
    uint32_t version = 0;
    uint32_t name_table_size = 0;
    uint64_t file_size = 0;
    LabLayout layout = LabLayout::Stored;
    std::vector<LabEntry> entries;
    // Entries whose stored name or data offset differs from the sequential layout (0 on the
    // retail disc). When non-zero, the two layouts extract different files.
    unsigned layout_mismatches = 0;
};

// The fourCC of `type` in reading order ("SPRT"), non-printable bytes as '?'.
std::string lab_type_name(const char type[4]);

// Parses a LAB directory (header, entry table, name table) from `data`, which holds at least
// the first 16 + 16 * count + name-table-size bytes of a file of `file_size` bytes (the whole
// file is fine). `name` is used in messages. Every count, offset and length is checked against
// the limits above and the file size. Problems with single entries (a bad name offset, an
// unterminated or empty name, data past the end of the file) are recorded in that entry's
// `problem`; problems that make the whole directory unusable return false with *err =
// "<name> @0x<offset>: <reason>".
bool parse_lab_directory(const uint8_t* data, size_t size, uint64_t file_size, const std::string& name,
                         LabLayout layout, LabDirectory& out, std::string* err);

// Reads only the directory part of the LAB file at `path` and parses it (see above).
bool read_lab_directory(const std::string& path, LabLayout layout, LabDirectory& out, std::string* err);

// Index of the first entry whose name equals `name` ignoring ASCII case, or -1.
int find_lab_entry(const LabDirectory& dir, const std::string& name);

// Extracts every entry of the LAB archive `lab_path` into `out_dir` (which must exist), one file
// per entry named as in the archive's name table; names go through platform::safe_output_path
// (a name that differs only in case from an existing file reuses it, as on NTFS; a later entry
// with the same name overwrites an earlier one, with a warning). Data is copied in chunks, never
// loaded whole. Default mode reads the stored offsets; --legacy reads sequentially, as the
// original did (identical on the retail disc; a warning says when they differ).
// An entry that cannot be extracted is reported with file and offset and skipped; a malformed
// directory stops the archive. Returns true only if every entry was written.
bool extract_lab(const std::string& lab_path, const std::string& out_dir, const Options& opts);

} // namespace tpmx
