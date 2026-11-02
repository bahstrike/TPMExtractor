// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): new entry point signature with Options;
// bounds-checked section walk, named header fields and level loading for later decoders (see
// the end of this file).

#pragma once

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

// Processes one level file (the original b3dexperiment()):
//   - creates <level_dump_root>/<NAME>/ (NAME = the file name of b3d_path as given, up to its
//     first '.'; the file is opened through platform::find_ci, so callers pass the canonical
//     upper-case name and get the original's directory names on any file system),
//   - decompresses the level in memory,
//   - writes every B3D_TEX texture as <level_dump_root>/<NAME>/<i>.bmp (opts.bmp), coloured
//     with its B3D_PALS palette (no colour key),
//   - writes the decompressed image as <level_dump_root>/<NAME>/<NAME>.dec if
//     opts.keep_decompressed.
// Default mode walks the section chain (offsets and sizes checked against the file), reads the
// palettes and textures only from inside their sections, accepts an image that is not
// compressed (such as a kept .dec), and warns about anything unusual. --legacy reads the level as
// the original did: it searches the image for the first "B3D_PALS" and "B3D_TEX" bytes, reads
// from there, and writes and then deletes <NAME>.dec; both modes write the same files from every
// retail level.
// Returns false, after logging the reason with file and offset, if the level could not be
// processed completely. Textures that could be read are still written.
//
// Level files embed no other files: on the retail disc DFLT.CMP, DFLT.MAT and the BMP, TXT and
// SPR files are entries of big.lab, which the disc run writes to <dump>/biglab/ (DFLT.CMP there
// is a retail-format colormap that --palette accepts).
bool process_b3d(const std::string& b3d_path, const std::string& level_dump_root, const Options& opts);

// ------------------------------------------------------------------ level file structure
//
// A decompressed level is a chain of sections: char tag[8] ("B3D" + '_' or ' ' + a 4-character
// name padded with spaces); uint32 size (little-endian); the body. The game reads the chain from
// "B3D FILE" (a 2,200-byte header) to "B3D_END1" and skips tags it does not know; editor-only
// sections follow, up to "B3D_END2" at the end of the file.

const size_t kB3dSectionHeaderSize = 12;
const uint32_t kB3dHeaderBodySize = 2200;   // the "B3D FILE" body
const uint32_t kB3dVersion = 293;           // header version of every retail level; the game rejects newer ones
const size_t kB3dPaletteSize = 768;         // one B3D_PALS palette: 256 x (r, g, b)
const size_t kB3dTextureHeaderSize = 16;    // int32 width, height, palette index, reserved (0);
                                            // width*height 8-bit indices follow
const int kB3dGameMaxTextures = 64;         // the game keeps at most this many level textures
const int64_t kB3dGameMaxTexels = 65536;    // and loads textures of at most this many texels
const size_t kB3dMaxSections = 4096;        // sanity limit for the walk (retail levels have 32 to 34)
const int kB3dMaxTextures = 1024;           // sanity limit for textures written per level (retail: 8 to 16)
const int64_t kB3dMaxTextureTexels = (int64_t)4096 * 4096;  // sanity limit for one texture's size

// Header fields used by this file and by decoders: uint32 values at these offsets into the
// "B3D FILE" body (file offset = kB3dSectionHeaderSize + body offset).
const size_t kB3dHdrVersion = 0x000;          // kB3dVersion on the retail disc
const size_t kB3dHdrNumPalettes = 0x020;      // B3D_PALS holds this many 768-byte palettes
const size_t kB3dHdrGridW = 0x028;            // B3D_GMAP and B3D TMAP grid: cells in X
const size_t kB3dHdrGridH = 0x02C;            //   ... and in Y
const size_t kB3dHdrGmapBlocks = 0x038;       // non-empty B3D_GMAP cells
const size_t kB3dHdrGmapEntries = 0x040;      // 64-byte B3D_GMAP entries (polygons and links)
const size_t kB3dHdrNumVertices = 0x048;      // 12-byte B3D_GVRT records
const size_t kB3dHdrNumMappings = 0x058;      // 40-byte B3D_MAT records
const size_t kB3dHdrNumTextures = 0x060;      // B3D_TEX records
const size_t kB3dHdrNumLights1 = 0x068;       // B3D_LIT1 records
const size_t kB3dHdrNumLights2 = 0x070;       // B3D_LIT2 records (no B3D_LIT2 section when 0)
const size_t kB3dHdrNumMovers = 0x0B8;        // B3D_AMAP records
const size_t kB3dHdrMoverVersion = 0x0BC;     // the game reads B3D_AMAP only when this is >= 4
const size_t kB3dHdrCompressed = 0x31C;       // file offset 0x328 (see b3d_decompress.h)
const size_t kB3dHdrEditorLightSize = 0x84C;  // B3D_TLIT record size

// The uint32 header field at `body_offset` of a level image, or 0 if the image is too short.
uint32_t b3d_header_u32(const uint8_t* image, size_t size, size_t body_offset);

struct B3dSection
{
    std::string tag;          // the 8 tag bytes as stored, e.g. "B3D_TEX " (padding kept)
    uint64_t offset = 0;      // file offset of the 12-byte section header
    uint32_t size_field = 0;  // the size the header stores
    uint64_t extent = 0;      // the bytes the body really occupies (see b3d_walk_sections)

    uint64_t body() const { return offset + kB3dSectionHeaderSize; }
    uint64_t end() const { return body() + extent; }
};

struct B3dWalk
{
    std::vector<B3dSection> sections;  // in file order
    bool game_part_complete = false;   // the chain is intact from "B3D FILE" through "B3D_END1"
    bool complete = false;             // ... and on to the exact end of the file
    uint64_t stop_offset = 0;          // where the walk stopped (the file size when complete)
    std::string problem;               // why the walk stopped early ("" when complete)

    // The first section with exactly this 8-byte tag (e.g. "B3D_TEX "), or nullptr.
    const B3dSection* find(const char* tag8) const;
};

// Walks the section chain of a decompressed level image. The first section must be
// "B3D FILE" with a 2,200-byte body. Every section header and body must fit in the image; the
// walk stops at the first one that does not, at a missing tag, or after kB3dMaxSections.
// The game part ends at "B3D_END1". After it, two editor sections store a size that is not
// their length, and their extent is computed instead (both rules hold for every retail level):
//   "B3D TMAP"  the size overcounts the cell stream (one byte per non-empty cell); the extent is
//               the stream itself: kB3dHdrGridW x kB3dHdrGridH cells, each the byte 0xFF
//               (empty) or a 12-byte cell header whose first byte is a count n, followed by
//               n 20-byte references;
//   "B3D_TLIT"  the size counts only the first group of light records; the extent is
//               (kB3dHdrNumLights1 + kB3dHdrNumLights2) records of kB3dHdrEditorLightSize bytes.
B3dWalk b3d_walk_sections(const uint8_t* data, size_t size);

struct B3dImageInfo
{
    uint64_t file_size = 0;       // bytes read from the file
    bool was_compressed = false;  // the file's "compressed" flag was set and its data was decoded
    bool truncated = false;       // the compressed data ended without its end marker
    uint64_t trailing_bytes = 0;  // input bytes after the end marker, at the end of the file (0 on the retail disc)
};

// Reads a level file and returns its decompressed image, the bytes --keep-decompressed writes as
// <NAME>.dec. A file whose 32-bit "compressed" flag (offset 0x328) is 0, such as a kept .dec,
// is returned as it is. Returns false and sets *err on failure; when info->truncated, `image`
// still holds everything that could be decoded.
bool b3d_load_image(const std::string& path, std::vector<uint8_t>& image, B3dImageInfo* info, std::string* err);

// ------------------------------------------------------------------ level geometry (hook)
//
// process_b3d() writes the textures only; the level geometry is not decoded yet. A decoder goes
// in a file of its own and starts from the calls process_b3d() makes in default mode:
//
//     std::vector<uint8_t> image;  B3dImageInfo info;  std::string err;
//     if (!b3d_load_image(path, image, &info, &err)) ...;            // decompressed image
//     const B3dWalk walk = b3d_walk_sections(image.data(), image.size());
//     if (!walk.game_part_complete) ...;                              // broken chain
//     const B3dSection* gmap = walk.find("B3D_GMAP");                 // bounds already checked
//     BinReader r(image.data() + gmap->body(), (size_t)gmap->extent, path, gmap->body());
//     const uint32_t grid_w = b3d_header_u32(image.data(), image.size(), kB3dHdrGridW);
//
// Its call belongs in b3d.cpp at the place marked "geometry hook", in default mode only and
// behind a new option, so that the texture output and --legacy stay as they are.
//
// What a decoder reads (little-endian; every rule below holds on all retail levels):
//   B3D_GMAP   grid_w x grid_h cells, row-major (cell = x + y * grid_w), 1 unit per cell, Z up.
//              A cell is the byte 0xFF (empty) or a 16-byte block {u8 count; u8 flags; u8; u8;
//              f32 z_min, z_max; 4 bytes 0} followed by count 64-byte entries; the stream ends
//              exactly at the section end, with kB3dHdrGmapBlocks blocks and kB3dHdrGmapEntries
//              entries. An entry is a polygon or, when its first u16 is 0xFFFF, a link
//              {u16 0xFFFF; u8 n (1..5); u8 0; 5 x {i16 cell_x, cell_y; u32 unused; u32 id}}
//              whose first n records name polygons stored in other cells that also cover this
//              cell: the polygon in cell (cell_x, cell_y) whose render flags have 0x100 and
//              whose link id equals id (the other records are zero).
//              Polygon: +0x00 u16 v[4] (B3D_GVRT indices; v[3] = 0 for a triangle), +0x10 f32
//              normal[3], +0x20 i16 front and +0x22 u16 back B3D_MAT index, +0x25 u8 vertex
//              count (3 or 4), +0x26 u8 uv_mode, +0x2C u16 render flags (0x1 texture animation
//              from the front to the back mapping, 0x2 two-sided, 0x100 link target), +0x30 u16
//              link id. Vertices run clockwise seen from the side the normal points to (a JK
//              surface runs counter-clockwise); in under 1% of polygons they run the other way,
//              and the stored normal is the one the game uses. A few normals are NaN.
//   B3D_GVRT   kB3dHdrNumVertices x {f32 x, y, z}.
//   B3D_MAT    kB3dHdrNumMappings x 40 bytes {f32 uv[4][2]; i32 B3D_TEX index; u32 flags}: atlas
//              corners in 0..1 (some tile beyond 1). uv_mode bits 0-2 choose one of the 8
//              symmetries of the square for the corner order; on triangles (bit 7) bits 4-5
//              choose which 3 of the 4 corners are used.
//   B3D_TEX, B3D_PALS   the textures and palettes process_b3d() writes.
//   B3D_AMAP   moving objects (doors, lifts): polygons whose byte +0x28 has 0x04 or 0x10 move
//              with node +0x29 of object +0x2A.
// The editor sections after B3D_END1 (B3D TMAP, B3D TLIB, B3D_TPLY) hold a tile-based copy of
// the same polygons, usable as a cross-check; the game itself never reads them.

} // namespace tpmx
