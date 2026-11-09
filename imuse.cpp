// TPMExtractor fork - the music tables of TPM's IMUSE.DLL, read from the file as data.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "imuse.h"

#include "binreader.h"
#include "log.h"
#include "platform.h"
#include "sound.h"
#include "textout.h"

#include <cstring>

namespace tpmx {

namespace {

const uint32_t kScnInitializedData = 0x40;  // IMAGE_SCN_CNT_INITIALIZED_DATA
const unsigned kMaxSections = 96;           // the PE format's limit
const uint32_t kMaxTransition = 12;         // the engine handles transitions 0..12
const uint32_t kMaxSmallField = 0xFFFF;     // attribute, hook and fade are small numbers

struct Section
{
    uint64_t raw = 0, raw_end = 0;  // the section's bytes in the file (clipped to the file)
    uint32_t address = 0;           // its relative virtual address
    uint32_t characteristics = 0;
};

uint32_t u32_at(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A fixed-size string field: printable ASCII up to a NUL inside the field, zeros after it.
bool field_string(const uint8_t* p, size_t n, bool allow_empty, std::string* out)
{
    const void* nul = std::memchr(p, 0, n);
    if (!nul)
        return false;
    const size_t len = (size_t)(static_cast<const uint8_t*>(nul) - p);
    if (len == 0 && !allow_empty)
        return false;
    for (size_t i = 0; i < len; i++)
        if (p[i] < 0x20 || p[i] > 0x7E)
            return false;
    for (size_t i = len; i < n; i++)
        if (p[i])
            return false;
    if (out)
        out->assign(reinterpret_cast<const char*>(p), len);
    return true;
}

// A state or sequence record (76 bytes at p).
bool music_record(const uint8_t* p, ImuseRecord* out)
{
    ImuseRecord r;
    if (!field_string(p, 32, true, &r.key) || !field_string(p + 40, 24, false, &r.symbol))
        return false;
    r.transition = u32_at(p + 32);
    r.id = u32_at(p + 36);
    r.attribute = u32_at(p + 64);
    r.hook = u32_at(p + 68);
    r.fade = u32_at(p + 72);
    if (r.transition > kMaxTransition || r.attribute > kMaxSmallField || r.hook > kMaxSmallField ||
        r.fade > kMaxSmallField)
        return false;
    if (out)
        *out = r;
    return true;
}

// The first record of a table: empty key, transition 0, the given id, a symbol, zeros after it.
bool null_record(const uint8_t* p, uint32_t id)
{
    ImuseRecord r;
    for (int i = 0; i < 32; i++)
        if (p[i])
            return false;
    return music_record(p, &r) && r.transition == 0 && r.id == id && r.attribute == 0 && r.hook == 0 && r.fade == 0;
}

// A file record (64 bytes at p) with a key and a file name.
bool file_record(const uint8_t* p, ImuseFile* out)
{
    ImuseFile f;
    if (!field_string(p, 32, false, &f.key) || !field_string(p + 32, 32, false, &f.file))
        return false;
    if (out)
        *out = f;
    return true;
}

std::vector<Section> pe_sections(BinReader& r, uint64_t& image_base)
{
    if (!r.peek_tag("MZ"))
        r.fail("not a Windows DLL (no \"MZ\" header)");
    r.seek(0x3C);
    const uint32_t pe = r.u32();
    if ((uint64_t)pe + 24 > r.size())
        r.fail_at(0x3C, format("the PE header offset 0x%X points past the end of the file", pe));
    r.seek(pe);
    const uint8_t* sig = r.span(4);
    if (sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0)
        r.fail_at(pe, "no \"PE\" signature where the MZ header points");
    r.skip(2);  // machine
    const size_t nsec_at = r.pos();
    const uint16_t nsec = r.u16();
    r.skip(12);
    const uint16_t opt_size = r.u16();
    r.skip(2);  // characteristics
    const size_t opt = r.pos();
    const uint16_t magic = r.u16();
    if (magic == 0x10B) {
        r.seek(opt + 28);
        image_base = r.u32();
    } else if (magic == 0x20B) {
        r.seek(opt + 24);
        image_base = r.u64();
    } else {
        r.fail_at(opt, format("unknown optional header (magic 0x%X)", magic));
    }
    if (nsec == 0 || nsec > kMaxSections)
        r.fail_at(nsec_at, format("%u sections", nsec));
    const uint64_t table = (uint64_t)opt + opt_size;
    if (table + (uint64_t)nsec * 40 > r.size())
        r.fail_at(nsec_at, format("the table of %u sections runs past the end of the file", nsec));
    std::vector<Section> out;
    for (unsigned i = 0; i < nsec; i++) {
        r.seek((size_t)(table + (uint64_t)i * 40 + 12));
        Section s;
        s.address = r.u32();
        const uint32_t raw_size = r.u32();
        const uint32_t raw_ptr = r.u32();
        r.skip(12);
        s.characteristics = r.u32();
        s.raw = raw_ptr < r.size() ? raw_ptr : r.size();
        const uint64_t end = (uint64_t)raw_ptr + raw_size;
        s.raw_end = end < r.size() ? end : r.size();
        out.push_back(s);
    }
    return out;
}

struct Found
{
    uint64_t offset = 0;
    const Section* section = nullptr;
};

// The one record position, 4-byte aligned in an initialized-data section, where null_record(id)
// holds and a valid record follows. `d` is the file r reads.
Found find_null_record(const BinReader& r, const uint8_t* d, const std::vector<Section>& sections, uint32_t id,
                       const char* what)
{
    Found found;
    unsigned count = 0;
    uint64_t second = 0;
    for (const Section& s : sections) {
        if (!(s.characteristics & kScnInitializedData) || s.raw_end < s.raw + 2 * kImuseRecordBytes)
            continue;
        for (uint64_t o = (s.raw + 3) & ~(uint64_t)3; o + 2 * kImuseRecordBytes <= s.raw_end; o += 4) {
            if (!null_record(d + o, id) || !music_record(d + o + kImuseRecordBytes, nullptr))
                continue;
            if (count++ == 0) {
                found.offset = o;
                found.section = &s;
            } else if (count == 2) {
                second = o;
            }
        }
    }
    if (count == 0)
        r.fail_at(0, format("no %s table: no record with an empty key and id %u was found", what, id));
    if (count > 1)
        r.fail_at((size_t)second, format("%u candidates for the %s table (records with an empty key and id %u)", count,
                                         what, id));
    return found;
}

uint64_t address_of(uint64_t image_base, const Section& s, uint64_t offset)
{
    return image_base + s.address + (offset - s.raw);
}

} // namespace

ImuseTables imuse_read_tables(const uint8_t* data, size_t size, const std::string& name)
{
    BinReader r(data, size, name);
    uint64_t image_base = 0;
    const std::vector<Section> sections = pe_sections(r, image_base);
    r.seek(0);
    const Found st = find_null_record(r, data, sections, kImuseNullState, "state");
    const Found sq = find_null_record(r, data, sections, kImuseNullSequence, "sequence");
    if (sq.section != st.section || sq.offset < st.offset + kImuseRecordBytes)
        r.fail_at((size_t)sq.offset, "the sequence table does not follow the state table");
    const uint8_t* d = data;
    const uint64_t end = st.section->raw_end;
    ImuseTables t;
    t.state_offset = st.offset;
    t.sequence_offset = sq.offset;

    // states: records up to the sequence table, then fewer than 76 zero bytes
    uint64_t o = st.offset;
    ImuseRecord rec;
    while (o + kImuseRecordBytes <= sq.offset && music_record(d + o, &rec)) {
        t.states.push_back(rec);
        o += kImuseRecordBytes;
    }
    if (sq.offset - o >= kImuseRecordBytes)
        r.fail_at((size_t)o, format("state %u is not a valid record", (unsigned)t.states.size()));
    for (uint64_t k = o; k < sq.offset; k++)
        if (d[k])
            r.fail_at((size_t)k, "the bytes between the state and sequence tables are not zero");

    // sequences: records up to the first file record
    o = sq.offset;
    ImuseFile file;
    for (;;) {
        if (o + kImuseFileRecordBytes <= end && file_record(d + o, nullptr))
            break;
        if (o + kImuseRecordBytes > end || !music_record(d + o, &rec))
            r.fail_at((size_t)o, format("sequence %u is not a valid record, nor the start of the file table",
                                        (unsigned)t.sequences.size()));
        t.sequences.push_back(rec);
        o += kImuseRecordBytes;
    }
    t.file_offset = o;

    // files: up to the first empty key
    for (;;) {
        if (o + kImuseFileRecordBytes > end)
            r.fail_at((size_t)o, "the file table is not ended by an empty record inside its section");
        if (d[o] == 0)
            break;
        if (!file_record(d + o, &file))
            r.fail_at((size_t)o, format("file record %u is not valid", (unsigned)t.files.size()));
        t.files.push_back(file);
        o += kImuseFileRecordBytes;
    }

    const struct
    {
        size_t got;
        unsigned want;
        uint64_t at;
        const char* what;
    } counts[] = {{t.states.size(), kImuseStates, t.state_offset, "state"},
                  {t.sequences.size(), kImuseSequences, t.sequence_offset, "sequence"},
                  {t.files.size(), kImuseFiles, t.file_offset, "file"}};
    for (const auto& c : counts)
        if (c.got != c.want)
            r.fail_at((size_t)c.at, format("%zu %s records; the game's DLL has %u (another build of the DLL?)", c.got,
                                           c.what, c.want));
    t.state_address = address_of(image_base, *st.section, t.state_offset);
    t.sequence_address = address_of(image_base, *st.section, t.sequence_offset);
    t.file_address = address_of(image_base, *st.section, t.file_offset);
    return t;
}

int imuse_find_file(const ImuseTables& t, const std::string& key)
{
    if (key.empty())
        return -1;
    for (size_t i = 0; i < t.files.size(); i++)
        if (t.files[i].key == key)
            return (int)i;
    return -1;
}

std::string imuse_tables_text(const ImuseTables& t, const std::string& file_label)
{
    unsigned unmatched = 0;
    for (const std::vector<ImuseRecord>* table : {&t.states, &t.sequences})
        for (const ImuseRecord& r : *table)
            if (!r.key.empty() && imuse_find_file(t, r.key) < 0)
                unmatched++;
    std::string s = format("# iMUSE music tables of %s, written by TPMExtractor (fork)\n",
                           sound_printable(file_label).c_str());
    s += format("# states: %zu records at file offset 0x%llX (address 0x%llX)\n", t.states.size(),
                (unsigned long long)t.state_offset, (unsigned long long)t.state_address);
    s += format("# sequences: %zu records at file offset 0x%llX (address 0x%llX)\n", t.sequences.size(),
                (unsigned long long)t.sequence_offset, (unsigned long long)t.sequence_address);
    s += format("# files: %zu records at file offset 0x%llX (address 0x%llX)\n", t.files.size(),
                (unsigned long long)t.file_offset, (unsigned long long)t.file_address);
    s += "# transition: 0 none, 3 crossfade into the file, 4 crossfade, then back to the state at the file's _end "
         "marker\n";
    s += "# file: the file record whose key equals the record's key byte for byte, as the engine looks keys up; - for "
         "none\n";
    s += format("# records with a key that matches no file key byte for byte: %u\n", unmatched);
    s += "# table\tindex\tid\ttransition\tattribute\thook\tfade\tkey\tsymbol\tfile\n";
    const struct
    {
        const char* name;
        const std::vector<ImuseRecord>* records;
    } tables[] = {{"state", &t.states}, {"sequence", &t.sequences}};
    for (const auto& tb : tables) {
        for (size_t i = 0; i < tb.records->size(); i++) {
            const ImuseRecord& r = (*tb.records)[i];
            const int f = imuse_find_file(t, r.key);
            s += format("%s\t%zu\t%u\t%u\t%u\t%u\t%u\t\"%s\"\t\"%s\"\t%s\n", tb.name, i, r.id, r.transition,
                        r.attribute, r.hook, r.fade, sound_printable(r.key).c_str(), sound_printable(r.symbol).c_str(),
                        f < 0 ? "-" : ("\"" + sound_printable(t.files[(size_t)f].file) + "\"").c_str());
        }
    }
    s += "# table\tindex\tkey\tfile\n";
    for (size_t i = 0; i < t.files.size(); i++)
        s += format("file\t%zu\t\"%s\"\t\"%s\"\n", i, sound_printable(t.files[i].key).c_str(),
                    sound_printable(t.files[i].file).c_str());
    return s;
}

bool process_imuse(const std::string& dll_path, const std::string& out_dir, const Options& opts)
{
    (void)opts;
    uint64_t size = 0;
    if (!platform::file_size(dll_path, size)) {
        log::error("%s: cannot open: not a readable file\n", dll_path.c_str());
        return false;
    }
    if (size > kMaxDllSize) {
        log::error("%s: %llu bytes, more than the %llu bytes a DLL may have here\n", dll_path.c_str(),
                   (unsigned long long)size, (unsigned long long)kMaxDllSize);
        return false;
    }
    std::vector<uint8_t> data;
    std::string why;
    if (!platform::read_file(dll_path, data, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    ImuseTables t;
    try {
        t = imuse_read_tables(data.data(), data.size(), dll_path);
    } catch (const ParseError& e) {
        log::error("%s\n", e.what());
        return false;
    }
    std::string out;
    if (!platform::safe_output_path(out_dir, "imuse_tables.txt", out, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    TextWriter w;
    if (!w.open(out, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    w.write(imuse_tables_text(t, platform::base_name(dll_path)));
    if (!w.close(&why)) {
        log::error("%s\n", why.c_str());
        platform::remove_file(out);
        return false;
    }
    log::info("%s: %zu states, %zu sequences, %zu files -> %s\n", dll_path.c_str(), t.states.size(),
              t.sequences.size(), t.files.size(), out.c_str());
    return true;
}

} // namespace tpmx
