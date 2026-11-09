// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): portable I/O and BMP output through
// the platform layer, Options, and errors with file and offset instead of crashes; the level is
// decompressed in memory and parsed with bounds checks (default mode walks the section chain,
// --legacy keeps the original's tag search and loop limit); section-walk API; default mode also
// exports the level geometry as OBJ/MTL (--geometry, b3d_geom.cpp).

#include "b3d.h"

#include "b3d_decompress.h"
#include "b3d_geom.h"
#include "binreader.h"
#include "bmp.h"
#include "image.h"
#include "log.h"
#include "platform.h"
#include "textout.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace tpmx {

namespace {

const char kTagFile[] = "B3D FILE";
const char kTagPals[] = "B3D_PALS";
const char kTagTex[] = "B3D_TEX ";
const char kTagEnd1[] = "B3D_END1";
const char kTagTmap[] = "B3D TMAP";
const char kTagTlit[] = "B3D_TLIT";

// What the original's searchfile() looked for (B3D_TEX without its padding space).
const char kLegacySearchPals[] = "B3D_PALS";
const char kLegacySearchTex[] = "B3D_TEX";

// The tags the game's level loader knows. It reads up to B3D_END1 and skips any other tag
// (it also skips B3D_APRT and B3D_SNAM, which occur in no retail level).
const char* const kGameTags[] = {
    "B3D FILE", "B3D_PALS", "B3D_GMAP", "B3D_GVRT", "B3D_LVRT", "B3D_MAT ", "B3D_TEX ", "B3D_APRT",
    "B3D_LIT1", "B3D_LIT2", "B3D_END1", "B3D_ADEF", "B3D_AMAP", "B3D_ENMY", "B3D_AI  ", "B3D_ANAM",
    "B3D_SNAM", "B3D_SCAL", "B3D_SPLA", "B3D_EPLA", "B3D_SREF", "B3D_DLG ",
};

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// a * b, saturating at UINT64_MAX.
uint64_t mul_sat(uint64_t a, uint64_t b) { return (a != 0 && b > UINT64_MAX / a) ? UINT64_MAX : a * b; }

// "B3D", then '_' or ' ', then 4 characters from A-Z, 0-9 and space.
bool looks_like_tag(const uint8_t* t)
{
    if (t[0] != 'B' || t[1] != '3' || t[2] != 'D' || (t[3] != '_' && t[3] != ' '))
        return false;
    for (int i = 4; i < 8; i++) {
        const uint8_t c = t[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '))
            return false;
    }
    return true;
}

// Up to 8 bytes for a message, other than printable ASCII as \xNN.
std::string printable_bytes(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7F && p[i] != '"' && p[i] != '\\')
            s += (char)p[i];
        else
            s += format("\\x%02X", p[i]);
    }
    return s;
}

bool is_game_tag(const std::string& tag)
{
    for (const char* t : kGameTags)
        if (tag == t)
            return true;
    return false;
}

// Length of the "B3D TMAP" cell stream at data[body]: `cells` cells, each the byte 0xFF or a
// 12-byte cell header (first byte n) plus n 20-byte references. False unless it ends within
// `limit` bytes. Every cell takes at least one byte, which bounds the loop.
bool tmap_stream_length(const uint8_t* data, uint64_t body, uint64_t limit, uint64_t cells, uint64_t& len)
{
    uint64_t p = 0;
    for (uint64_t i = 0; i < cells; i++) {
        if (p >= limit)
            return false;
        const uint8_t n = data[body + p];
        p += n == 0xFF ? 1 : 12 + 20 * (uint64_t)n;
    }
    if (p > limit)
        return false;
    len = p;
    return true;
}

// ---------------------------------------------------------------------------------- output

struct LevelFiles
{
    std::string dumpdir;  // <root>/<NAME>
    std::string decfile;  // <root>/<NAME>/<NAME>.dec
    bool dec_written = false;
};

// One directory level, as the original's CreateDirectory; an existing directory is fine.
bool ensure_dir(const std::string& dir)
{
    std::string why;
    const platform::MkdirResult r = platform::make_dir(dir, &why);
    if (r == platform::MkdirResult::Created || platform::is_dir(dir))
        return true;
    log::error("%s\n", why.empty() ? (dir + ": exists and is not a directory").c_str() : why.c_str());
    return false;
}

// The level's palettes, stored back to back in the image; copied out one at a time.
struct PaletteTable
{
    const uint8_t* base = nullptr;
    size_t count = 0;

    Palette get(size_t i) const
    {
        Palette p;
        std::memcpy(&p, base + i * kB3dPaletteSize, sizeof p);
        return p;
    }
};

// <dumpdir>/<index>.bmp from 8-bit indices and their palette (no colour key).
bool write_texture(const std::string& dumpdir, int index, int32_t width, int32_t height, const uint8_t* pixels,
                   const Palette& pal, const Options& opts)
{
    if (!opts.bmp)
        return true;
    std::string bmpfile, why;
    if (!platform::safe_output_path(dumpdir, format("%d.bmp", index), bmpfile, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    log::verbose("dumping: %s...\n", bmpfile.c_str());
    if (!bmp24_write_indexed(bmpfile, width, height, pixels, pal, false,
                             opts.legacy ? BmpRows::Unpadded : BmpRows::Padded, &why)) {
        log::error("%s\n", why.c_str());
        return false;
    }
    return true;
}

struct TextureCount
{
    int read = 0;     // texture records whose header and pixels were present
    int written = 0;  // of those, the ones that were valid and written (valid, with --no-bmp)
};

// Reads texture records from r and writes them as <index>.bmp. Default mode reads to the end of
// r, which covers the B3D_TEX body, or until max_count records (when max_count >= 0). --legacy
// reads while the position is before legacy_end, with r covering the whole image: the original
// stopped at the TEX tag's offset plus the size field, 12 bytes short of the section end (on the
// retail disc the last texture still ends exactly at the section end, so nothing is lost).
// A record with an unusable size ends the loop (the next record cannot be located); a texture
// that is too large or names a missing palette is skipped and the loop goes on.
bool read_textures(BinReader& r, int64_t legacy_end, int max_count, const PaletteTable& pals, const std::string& name,
                   const std::string& dumpdir, const Options& opts, TextureCount& count)
{
    bool ok = true;
    bool warned_size = false;
    for (int textry = 0;; textry++) {
        if (opts.legacy ? legacy_end - (int64_t)r.pos() <= 0 : (r.at_end() || (max_count >= 0 && textry >= max_count)))
            break;
        const uint64_t at = r.file_offset();
        if (textry >= kB3dMaxTextures) {
            log::error_at(name, at, "more than %d textures; the rest are skipped\n", kB3dMaxTextures);
            return false;
        }
        log::verbose("dumping %d\n", textry);
        if (!r.can_read(kB3dTextureHeaderSize)) {
            log::error_at(name, at, "texture %d: header truncated (%zu bytes left)\n", textry, r.remaining());
            return false;
        }
        const int32_t width = r.i32();
        const int32_t height = r.i32();
        const int32_t palIndex = r.i32();
        const int32_t reserved = r.i32();
        log::verbose("texdump %dx%d, %d, %d\n", width, height, palIndex, reserved);
        if (reserved != 0)  // the original printed "WHSFDFSDFAT" here and went on
            log::warn_at(name, at + 12, "texture %d: reserved field is %d, expected 0\n", textry, reserved);
        if (width < 1 || height < 1) {
            log::error_at(name, at, "texture %d: invalid size %dx%d; the textures after it cannot be located\n",
                          textry, width, height);
            return false;
        }
        const uint64_t texels = (uint64_t)width * (uint64_t)height;
        if (texels > r.remaining()) {
            log::error_at(name, at + kB3dTextureHeaderSize, "texture %d: %dx%d needs %llu bytes of pixels, %zu left\n",
                          textry, width, height, (unsigned long long)texels, r.remaining());
            return false;
        }
        const uint8_t* pixels = r.span((size_t)texels);
        count.read++;
        if (width > kBmpMaxDimension || height > kBmpMaxDimension || (int64_t)texels > kB3dMaxTextureTexels) {
            log::error_at(name, at,
                          "texture %d: %dx%d is larger than this tool writes (%d per side, %lld texels); skipped\n",
                          textry, width, height, kBmpMaxDimension, (long long)kB3dMaxTextureTexels);
            ok = false;
            continue;
        }
        if (palIndex < 0 || (uint64_t)palIndex >= pals.count) {
            log::error_at(name, at + 8, "texture %d: palette %d, but the level has %zu palette(s); skipped\n", textry,
                          palIndex, pals.count);
            ok = false;
            continue;
        }
        if (!opts.legacy && !warned_size && (int64_t)texels > kB3dGameMaxTexels) {
            log::warn_at(name, at, "texture %d: %dx%d is larger than the game loads (%lld texels)\n", textry, width,
                         height, (long long)kB3dGameMaxTexels);
            warned_size = true;
        }
        if (write_texture(dumpdir, textry, width, height, pixels, pals.get((size_t)palIndex), opts))
            count.written++;
        else
            ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------------- --legacy

// Offset of the first occurrence of `needle` in the image, or -1 (the original's searchfile()).
int64_t search_bytes(const std::vector<uint8_t>& d, size_t from, const char* needle)
{
    const size_t n = std::strlen(needle);
    if (from > d.size())
        return -1;
    const auto it = std::search(d.begin() + (std::ptrdiff_t)from, d.end(), needle, needle + n);
    return it == d.end() ? -1 : (int64_t)(it - d.begin());
}

// The original's parse of the decompressed image, with every read bounds-checked.
bool legacy_textures(const std::vector<uint8_t>& dec, const std::string& name, const std::string& dumpdir,
                     const Options& opts)
{
    const int64_t palsOffset = search_bytes(dec, 0, kLegacySearchPals);
    const int64_t texOffset = search_bytes(dec, 0, kLegacySearchTex);
    log::info("%s:  PALS:%lld  TEX:%lld\n", name.c_str(), (long long)palsOffset, (long long)texOffset);

    // "B3D FILE", 2200, 293 (the original's B3DHEADER magic0, magic1, magic2)
    if (dec.size() < kB3dRawHeaderSize || std::memcmp(dec.data(), kTagFile, 8) != 0 ||
        le32(dec.data() + 8) != kB3dHeaderBodySize ||
        le32(dec.data() + kB3dSectionHeaderSize + kB3dHdrVersion) != kB3dVersion) {
        log::error_at(name, 0, "header value check bad\n");
        return false;
    }
    if (palsOffset < 0 || texOffset < 0) {
        log::error_at(name, 0, "B3D_PALS or B3D_TEX section not found\n");
        return false;
    }

    BinReader r(dec, name);

    // The palette count is the B3D_PALS size field / 768; the palettes follow it.
    r.seek((size_t)palsOffset + 8);
    if (!r.can_read(4)) {
        log::error_at(name, r.file_offset(), "bad B3D_PALS size (truncated)\n");
        return false;
    }
    const int32_t value = r.i32();
    if (value < 0) {
        log::error_at(name, (uint64_t)palsOffset + 8, "bad B3D_PALS size %d\n", value);
        return false;
    }
    PaletteTable pals;
    pals.count = (size_t)value / kB3dPaletteSize;
    log::verbose("loading %zu palettes...\n", pals.count);
    const size_t whole = r.remaining() / kB3dPaletteSize;
    if (pals.count > whole) {
        log::error_at(name, r.file_offset() + whole * kB3dPaletteSize, "palette %zu truncated\n", whole);
        return false;
    }
    pals.base = dec.data() + r.pos();

    // B3D_TEX: the 12-byte section header, then texture records up to the original's limit.
    r.seek((size_t)texOffset);
    if (!r.can_read(kB3dSectionHeaderSize)) {
        log::error_at(name, (uint64_t)texOffset, "B3D_TEX header truncated\n");
        return false;
    }
    r.skip(8);
    const int32_t someSize = r.i32();
    const int64_t texEnd = texOffset + (int64_t)someSize;
    TextureCount count;
    const bool ok = read_textures(r, texEnd, -1, pals, name, dumpdir, opts, count);
    if (count.read == 0)  // the original said nothing here
        log::warn_at(name, (uint64_t)texOffset + 8, "B3D_TEX: no textures read (size field %d)\n", someSize);
    return ok;
}

// ---------------------------------------------------------------------------------- default

// Tag, header size and version of the "B3D FILE" section.
bool check_header(const std::vector<uint8_t>& d, const std::string& name)
{
    if (d.size() < kB3dSectionHeaderSize + kB3dHeaderBodySize || std::memcmp(d.data(), kTagFile, 8) != 0 ||
        le32(d.data() + 8) != kB3dHeaderBodySize) {
        log::error_at(name, 0, "not a level file: it does not start with a %u-byte \"B3D FILE\" section\n",
                      kB3dHeaderBodySize);
        return false;
    }
    const uint32_t version = le32(d.data() + kB3dSectionHeaderSize + kB3dHdrVersion);
    if (version > kB3dVersion) {
        log::error_at(name, kB3dSectionHeaderSize + kB3dHdrVersion,
                      "header version %u is newer than the retail format (%u), which the game rejects too\n", version,
                      kB3dVersion);
        return false;
    }
    if (version < kB3dVersion)
        log::warn_at(name, kB3dSectionHeaderSize + kB3dHdrVersion,
                     "header version %u is older than the retail format (%u); palettes and textures are read "
                     "with the retail layout\n",
                     version, kB3dVersion);
    return true;
}

// The first section header with this tag at or after `from`. A body that runs past the end of
// the image is clipped there (clipped = true), so a cut-off section can still be read in part.
bool find_section_by_tag(const std::vector<uint8_t>& d, uint64_t from, const char* tag8, B3dSection& s, bool& clipped)
{
    const int64_t at = search_bytes(d, (size_t)from, tag8);
    if (at < 0 || (uint64_t)at + kB3dSectionHeaderSize > d.size())
        return false;
    s.tag.assign(tag8, 8);
    s.offset = (uint64_t)at;
    s.size_field = le32(d.data() + at + 8);
    const uint64_t avail = d.size() - s.body();
    clipped = s.size_field > avail;
    s.extent = clipped ? avail : s.size_field;
    return true;
}

// header_ok: false when the image is not a level the tool reads at all (already reported).
bool default_textures(const std::vector<uint8_t>& dec, const std::string& name, const std::string& dumpdir,
                      const Options& opts, TextureCount& count, size_t& num_palettes, bool& header_ok)
{
    num_palettes = 0;
    header_ok = check_header(dec, name);
    if (!header_ok)
        return false;
    const uint8_t* hdr = dec.data() + kB3dSectionHeaderSize;
    bool ok = true;

    const B3dWalk walk = b3d_walk_sections(dec.data(), dec.size());
    bool game_part = true;
    for (const B3dSection& s : walk.sections) {
        log::debug("%s: section \"%s\" at 0x%llX, size %u, extent %llu\n", name.c_str(), s.tag.c_str(),
                   (unsigned long long)s.offset, s.size_field, (unsigned long long)s.extent);
        if (game_part && !is_game_tag(s.tag))
            log::warn_at(name, s.offset, "unknown section \"%s\" (%u bytes) skipped, as the game skips it\n",
                         s.tag.c_str(), s.size_field);
        if (s.tag == kTagEnd1)
            game_part = false;
    }
    if (!walk.game_part_complete) {
        log::error_at(name, walk.stop_offset, "broken section chain: %s\n", walk.problem.c_str());
        ok = false;
    } else if (!walk.complete) {
        log::warn_at(name, walk.stop_offset, "after B3D_END1 (editor data the game does not read): %s\n",
                     walk.problem.c_str());
    }

    const B3dSection* pals = walk.find(kTagPals);
    const B3dSection* tex = walk.find(kTagTex);
    B3dSection found_pals, found_tex;
    bool pals_clipped = false, tex_clipped = false;
    if (!walk.game_part_complete) {
        // The chain broke before them: look for the tags themselves from the break on. A section
        // cut off by the end of the image is read as far as it goes.
        auto salvage = [&](const char* tag, const char* label, B3dSection& found, bool& clipped) -> const B3dSection* {
            if (!find_section_by_tag(dec, walk.stop_offset, tag, found, clipped))
                return nullptr;
            if (found.offset != walk.stop_offset)
                log::warn_at(name, found.offset, "%s found by a tag search\n", label);
            if (clipped)
                log::error_at(name, found.offset, "%s is cut off: %llu of its %u bytes are present\n", label,
                              (unsigned long long)found.extent, found.size_field);
            return &found;
        };
        if (!pals)
            pals = salvage(kTagPals, "B3D_PALS", found_pals, pals_clipped);
        if (!tex)
            tex = salvage(kTagTex, "B3D_TEX", found_tex, tex_clipped);
    }
    if (!pals || !tex) {
        log::error_at(name, walk.stop_offset, "no %s section\n", !pals ? "B3D_PALS" : "B3D_TEX");
        return false;
    }

    const uint32_t header_palettes = le32(hdr + kB3dHdrNumPalettes);
    const uint32_t header_textures = le32(hdr + kB3dHdrNumTextures);
    PaletteTable table;
    table.base = dec.data() + pals->body();
    table.count = (size_t)(pals->extent / kB3dPaletteSize);
    if (pals_clipped && header_palettes < table.count)
        table.count = header_palettes;  // the count the game reads
    num_palettes = table.count;
    if (!pals_clipped && pals->extent % kB3dPaletteSize)
        log::warn_at(name, pals->offset + 8,
                     "B3D_PALS size %u is not a multiple of %zu; the last %llu bytes are ignored\n", pals->size_field,
                     kB3dPaletteSize, (unsigned long long)(pals->extent % kB3dPaletteSize));
    if (header_palettes != table.count)
        log::warn_at(name, kB3dSectionHeaderSize + kB3dHdrNumPalettes,
                     "the header counts %u palettes, B3D_PALS holds %zu\n", header_palettes, table.count);
    log::verbose("loading %zu palettes...\n", table.count);

    // A cut-off B3D_TEX is read up to the header's texture count, the number the game reads.
    const int max_textures = tex_clipped ? (int)std::min<uint32_t>(header_textures, (uint32_t)kB3dMaxTextures) : -1;
    BinReader r(dec.data() + tex->body(), (size_t)tex->extent, name, tex->body());
    if (!read_textures(r, -1, max_textures, table, name, dumpdir, opts, count))
        ok = false;
    if (pals_clipped || tex_clipped)
        ok = false;
    if (header_textures != (uint32_t)count.read)
        log::warn_at(name, kB3dSectionHeaderSize + kB3dHdrNumTextures,
                     "the header counts %u textures, B3D_TEX holds %d\n", header_textures, count.read);
    if (count.read > kB3dGameMaxTextures)
        log::warn_at(name, tex->offset, "%d textures; the game loads at most %d\n", count.read, kB3dGameMaxTextures);
    return ok;
}

// ---------------------------------------------------------------------------------- the level

bool run_level(const std::string& szLevelFile, const std::string& szDumpDir, const Options& opts, LevelFiles& files)
{
    // level name: the file name up to its first '.'
    const std::string filename = platform::base_name(szLevelFile);
    const std::string levelname = filename.substr(0, filename.find('.'));

    std::string why;
    if (!platform::safe_output_path(szDumpDir, levelname, files.dumpdir, &why)) {
        log::error("%s: %s\n", szLevelFile.c_str(), why.c_str());
        return false;
    }
    if (!ensure_dir(files.dumpdir))
        return false;
    if (!platform::safe_output_path(files.dumpdir, levelname + ".dec", files.decfile, &why)) {
        log::error("%s: %s\n", szLevelFile.c_str(), why.c_str());
        return false;
    }

    // the level name above comes from the path as given; the file itself may differ in case
    const std::string input = platform::find_ci(szLevelFile);
    std::vector<uint8_t> image;

    if (opts.legacy) {
        // As the original: decompress whatever the flag says, write <NAME>.dec, read the level
        // from its bytes (here kept in memory), and delete it afterwards (process_b3d).
        bool ok = true;
        {
            std::vector<uint8_t> raw;
            if (!b3d_read_file(input, raw, &why)) {
                log::error("%s\n", why.c_str());
                return false;
            }
            LzssResult lzss;
            if (!b3d_decompress(raw.data(), raw.size(), input, image, true, &lzss, &why)) {
                log::error("%s\n", why.c_str());
                if (image.empty() || lzss.end == LzssEnd::OutputLimit)
                    return false;
                ok = false;  // truncated: the original went on with what it had decoded
            }
        }
        if (!platform::write_file(files.decfile, image, &why)) {
            log::error("%s\n", why.c_str());
            return false;
        }
        files.dec_written = true;
        return legacy_textures(image, files.decfile, files.dumpdir, opts) && ok;
    }

    bool ok = true;
    B3dImageInfo info;
    if (!b3d_load_image(input, image, &info, &why)) {
        log::error("%s\n", why.c_str());
        if (!info.truncated)
            return false;
        ok = false;  // go on with what could be decoded
    }
    if (info.trailing_bytes)
        log::warn_at(input, info.file_size - info.trailing_bytes,
                     "%llu bytes after the end of the compressed data were ignored\n",
                     (unsigned long long)info.trailing_bytes);
    if (!info.was_compressed)
        log::verbose("%s: not compressed; read as it is\n", input.c_str());
    if (opts.keep_decompressed) {
        if (platform::write_file(files.decfile, image, &why)) {
            files.dec_written = true;
        } else {
            log::error("%s\n", why.c_str());
            ok = false;
        }
    }

    // Offsets in messages are offsets in the decompressed image (the .dec).
    const std::string name = info.was_compressed ? input + " (decompressed)" : input;
    TextureCount count;
    size_t num_palettes = 0;
    bool header_ok = false;
    if (!default_textures(image, name, files.dumpdir, opts, count, num_palettes, header_ok))
        ok = false;
    log::info("%s: %d texture(s), %zu palette(s)\n", input.c_str(), count.written, num_palettes);

    // Level geometry (b3d_geom.h): <NAME>.obj and <NAME>.mtl next to the textures. Only default
    // mode reaches this point, so --legacy output cannot change, and the texture output above
    // does not depend on it. An image that is not a level at all was reported above.
    if (opts.geometry == GeometryMode::Obj && header_ok) {
        if (info.truncated) {
            log::error_at(name, image.size(), "level geometry not exported: the compressed data ends without its "
                                              "end marker\n");
            ok = false;
        } else if (!b3d_export_geometry(image, name, files.dumpdir, levelname, opts)) {
            ok = false;
        }
    }
    return ok;
}

} // namespace

// ------------------------------------------------------------------------------- public API

uint32_t b3d_header_u32(const uint8_t* image, size_t size, size_t body_offset)
{
    if (!image || body_offset > kB3dHeaderBodySize - 4 || size < kB3dSectionHeaderSize + body_offset + 4)
        return 0;
    return le32(image + kB3dSectionHeaderSize + body_offset);
}

const B3dSection* B3dWalk::find(const char* tag8) const
{
    for (const B3dSection& s : sections)
        if (s.tag == tag8)
            return &s;
    return nullptr;
}

B3dWalk b3d_walk_sections(const uint8_t* data, size_t size)
{
    B3dWalk w;
    if (!data)
        size = 0;
    const uint8_t* hdr = nullptr;  // the header body, once the first section has passed its check
    uint64_t pos = 0;
    bool after_end1 = false;
    for (;;) {
        if (pos == size) {
            if (after_end1)
                w.complete = true;
            else
                w.problem = w.sections.empty() ? "the image is empty" : "the image ends before B3D_END1";
            break;
        }
        const uint64_t left = size - pos;
        if (left < kB3dSectionHeaderSize) {
            w.problem = format("%llu byte(s) left, too few for a section header", (unsigned long long)left);
            break;
        }
        if (w.sections.size() >= kB3dMaxSections) {
            w.problem = format("more than %zu sections", kB3dMaxSections);
            break;
        }
        const uint8_t* t = data + pos;
        if (!looks_like_tag(t)) {
            w.problem = format("no section tag (found \"%s\")", printable_bytes(t, 8).c_str());
            break;
        }
        B3dSection s;
        s.tag.assign((const char*)t, 8);
        s.offset = pos;
        s.size_field = le32(t + 8);
        s.extent = s.size_field;
        if (w.sections.empty() && (s.tag != kTagFile || s.size_field != kB3dHeaderBodySize)) {
            w.problem = format("the image starts with \"%s\" (size %u), not a %u-byte \"B3D FILE\" section",
                               s.tag.c_str(), s.size_field, kB3dHeaderBodySize);
            break;
        }
        const uint64_t avail = left - kB3dSectionHeaderSize;
        if (after_end1 && hdr && s.tag == kTagTmap) {
            const uint64_t cells = (uint64_t)le32(hdr + kB3dHdrGridW) * le32(hdr + kB3dHdrGridH);
            uint64_t len = 0;
            if (!tmap_stream_length(data, s.body(), std::min<uint64_t>(s.size_field, avail), cells, len)) {
                w.problem = format("the \"B3D TMAP\" cell stream (%llu cells) does not end inside its section",
                                   (unsigned long long)cells);
                break;
            }
            s.extent = len;
        } else if (after_end1 && hdr && s.tag == kTagTlit) {
            const uint64_t records = (uint64_t)le32(hdr + kB3dHdrNumLights1) + le32(hdr + kB3dHdrNumLights2);
            const uint64_t extent = mul_sat(records, le32(hdr + kB3dHdrEditorLightSize));
            if (extent > s.extent)
                s.extent = extent;
        }
        if (s.extent > avail) {
            w.problem = format("section \"%s\" needs %llu bytes, %llu left", s.tag.c_str(),
                               (unsigned long long)s.extent, (unsigned long long)avail);
            break;
        }
        w.sections.push_back(s);
        if (!hdr)
            hdr = data + kB3dSectionHeaderSize;
        pos += kB3dSectionHeaderSize + s.extent;
        if (!after_end1 && s.tag == kTagEnd1) {
            after_end1 = true;
            w.game_part_complete = true;
        }
    }
    w.stop_offset = pos;
    return w;
}

bool b3d_load_image(const std::string& path, std::vector<uint8_t>& image, B3dImageInfo* info, std::string* err)
{
    B3dImageInfo local;
    B3dImageInfo& inf = info ? *info : local;
    inf = B3dImageInfo();
    image.clear();

    std::vector<uint8_t> raw;
    if (!b3d_read_file(path, raw, err))
        return false;
    inf.file_size = raw.size();
    if (raw.size() >= kB3dRawHeaderSize && le32(raw.data() + kB3dCompressedFlagOffset) == 0) {
        image.swap(raw);  // not compressed (a kept .dec, or a level saved without compression)
        return true;
    }
    inf.was_compressed = true;
    LzssResult r;
    const bool ok = b3d_decompress(raw.data(), raw.size(), path, image, false, &r, err);
    if (ok)
        inf.trailing_bytes = raw.size() - kB3dRawHeaderSize - r.consumed;
    inf.truncated = r.end == LzssEnd::InputEnd && !image.empty();
    if (!ok && !inf.truncated)
        image.clear();
    return ok;
}

bool process_b3d(const std::string& szLevelFile, const std::string& szDumpDir, const Options& opts)
{
    LevelFiles files;
    bool ok = false;
    try {
        ok = run_level(szLevelFile, szDumpDir, opts, files);
    } catch (const std::exception& e) {  // ParseError, or out of memory: skip this level only
        log::error("%s: %s\n", szLevelFile.c_str(), e.what());
        ok = false;
    }
    if (files.dec_written && !opts.keep_decompressed) {
        // lool we done?
        std::string why;
        if (!platform::remove_file(files.decfile, &why))
            log::warn("%s\n", why.c_str());
    }
    return ok;
}

} // namespace tpmx
