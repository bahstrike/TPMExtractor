// TPMExtractor fork - TPM level geometry (.B3D): decoder and OBJ/MTL export.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Strike's notes left the level geometry undecoded. The format below was recovered for the fork
// by static analysis of the game's level loader and renderer (the executable was read, never
// run) and checked on every retail level; the fork's research notes hold the evidence. Every
// rule here is stated for the decompressed image (b3d_load_image), and every offset in a message
// is an offset into that image (the bytes --keep-decompressed writes as <NAME>.dec).
//
// The geometry (game part of the section chain, "B3D FILE" to "B3D_END1"):
//   B3D_GMAP  grid_w x grid_h cells, row-major (cell = x + y * grid_w). A cell is the byte 0xFF
//             (empty) or a 16-byte block header {u8 count (1..254); u8 flags; ...} followed by
//             count 64-byte entries. An entry is a polygon, or a LINK entry (first u16 0xFFFF)
//             naming polygons homed in other cells that also cover this one. Every polygon is
//             stored once; its index is its position in this walk, LINK entries skipped.
//   B3D_GVRT  vertices {f32 x, y, z}: Z up, 1 unit = 1 grid cell = 1 BAF unit, JK's axes.
//   B3D_MAT   40-byte UV mappings {f32 uv[4][2]; i32 texture; u32 flags} onto B3D_TEX pages.
//   B3D_TEX   textures (the <i>.bmp files process_b3d writes); B3D_PALS their palettes.
//   B3D_AMAP  moving objects (doors, lifts): which polygons move with which object.
// Polygon entry fields used here (offsets into the 64-byte entry):
//   +0x00 u16 v[4] (v[3] unused on triangles)   +0x10 f32 normal[3]   +0x20 i16 front MAT
//   +0x22 u16 back MAT   +0x24 u8 texture bound when +0x2B is 0   +0x25 u8 vertex count (3, 4)
//   +0x26 u8 uv_mode (bits 0-2 corner symmetry; bit 7 triangle, bits 4-5 its corner subset)
//   +0x27 u8 facing class (1 +X, 2 -X, 3 +Y, 4 -Y, 5 +Z, 6 -Z)   +0x28 u8 object flags (0x04
//   moved by an AMAP node, 0x10 hidden)   +0x29 u8 AMAP node   +0x2A u8 AMAP object
//   +0x2B u8 blend (0 opaque; otherwise drawn blended with TEX[MAT.texture])   +0x2C u16 render
//   flags (0x1 texture animation, 0x2 two-sided, 0x100 LINK target)   +0x30 u16 link id
//   +0x3C u16 collision class   +0x3E u16 surface class.
//
// Vertices are stored clockwise seen from the side the stored normal points to; JK faces and
// surfaces, and OBJ, want counter-clockwise. The export reorients every polygon around its
// stored normal (the side the game draws), keeping vertex 0 first.

#pragma once

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

// ------------------------------------------------------------------ format constants

const uint32_t kB3dGeomMinVersion = 278;        // 0x116: older levels pack the GMAP bit fields
                                                // differently (the loader repacks them); refused
const uint32_t kB3dGmapBlockHeaderSize = 16;
const uint32_t kB3dGmapEntrySize = 64;
const uint32_t kB3dGmapFirstRead = kB3dGmapBlockHeaderSize + kB3dGmapEntrySize;  // header +0x03C
const uint8_t kB3dGmapEmptyCell = 0xFF;
const uint16_t kB3dLinkMarker = 0xFFFF;
const int kB3dLinkMaxRecords = 5;
const uint32_t kB3dVertexSize = 12;
const uint32_t kB3dMappingSize = 40;
const int64_t kB3dGeomMaxTexels = 65536;        // the game's fixed 64 KiB texture load buffer
const uint32_t kB3dAmapMinVersion = 4;          // the game reads B3D_AMAP only from this version
const uint32_t kB3dAmapRecordHeaderSize = 0x84;
const uint32_t kB3dAmapNodeSize = 156;

// Header fields used only by the geometry (body offsets, as in b3d.h).
const size_t kB3dHdrGmapFirstRead = 0x03C;      // must be kB3dGmapFirstRead (80)
const size_t kB3dHdrGmapEntrySize = 0x044;      // must be kB3dGmapEntrySize (64)
const size_t kB3dHdrVertexSize = 0x04C;         // must be kB3dVertexSize (12)
const size_t kB3dHdrMappingSize = 0x05C;        // must be kB3dMappingSize (40)

// Polygon flag bits.
const uint8_t kB3dObjMoving = 0x04;             // +0x28: moved by AMAP node +0x29 of object +0x2A
const uint8_t kB3dObjHidden = 0x10;             // +0x28: hidden/inactive (also AMAP-bound)
const uint16_t kB3dRenderTexAnim = 0x0001;      // +0x2C: texture animation mat_front..mat_back
const uint16_t kB3dRenderTwoSided = 0x0002;
const uint16_t kB3dRenderLinkTarget = 0x0100;
const uint8_t kB3dBlockLinkTarget = 0x04;       // block flags: holds LINK targets
const uint8_t kB3dUvTriangle = 0x80;            // uv_mode: triangle corner subset in bits 4-5

// The game's UV corner tables (its .data tables, read from the executable): uv_mode bits 0-2
// pick one of the 8 symmetries of the mapping's 4 corners; for a triangle (bit 7) bits 4-5
// pick which 3 corners are used.
extern const int kB3dUvPerm[8][4];
extern const int kB3dUvTri[4][3];

// ------------------------------------------------------------------ decoded geometry

struct B3dVertex
{
    float x = 0, y = 0, z = 0;
};

struct B3dMapping  // a B3D_MAT record: UV corners on a texture page, not a JK material
{
    float uv[4][2] = {};  // normalised (u, v) of corners 0..3; v = 0 is the texture's top row
    int32_t tex = 0;      // B3D_TEX index (bound only when the polygon is blended)
    uint32_t flags = 0;
};

struct B3dTextureInfo
{
    int32_t width = 0, height = 0, palette = 0;
    uint64_t offset = 0;  // image offset of the 16-byte texture header
};

struct B3dBlock  // a non-empty cell
{
    uint64_t offset = 0;      // image offset of the 16-byte block header
    uint64_t cell = 0;        // x + y * grid_w
    uint8_t count = 0;        // entries (polygons and LINK entries)
    uint8_t flags = 0;        // 0x02 AMAP-bound polygons, 0x04 LINK targets, 0x08 LINK entries
    uint32_t first_poly = 0;  // its polygons are polys[first_poly, first_poly + num_polys)
    uint32_t num_polys = 0;
};

struct B3dPoly
{
    uint64_t offset = 0;      // image offset of the 64-byte entry
    uint32_t block = 0;       // index into B3dGeometry::blocks
    uint16_t v[4] = {};       // GVRT indices (the first nverts are used)
    uint8_t nverts = 0;       // 3 or 4
    float normal[3] = {};     // as stored (may be non-finite; see b3d_poly_normal)
    int16_t mat_front = 0;    // B3D_MAT index of the side the normal points to
    uint16_t mat_back = 0;    // B3D_MAT index of the back side (two-sided polygons)
    uint8_t tex_bucket = 0;   // B3D_TEX index bound when blend == 0
    uint8_t uv_mode = 0;
    uint8_t facing = 0;
    uint8_t obj_flags = 0;
    uint8_t amap_node = 0;
    uint8_t amap_index = 0;
    uint8_t blend = 0;
    uint16_t render = 0;
    uint16_t link_id = 0;
    uint16_t coll = 0;
    uint16_t surf = 0;
};

struct B3dLinkRecord
{
    int16_t cell_x = 0, cell_y = 0;  // the home cell of the target polygon
    uint32_t id = 0;                 // the target's link id
};

struct B3dLink
{
    uint64_t offset = 0;  // image offset of the 64-byte entry
    uint32_t block = 0;   // index into B3dGeometry::blocks
    uint8_t count = 0;    // records used (1..5)
    B3dLinkRecord rec[kB3dLinkMaxRecords];
};

struct B3dAmapNode
{
    float pivot[3] = {};
    uint32_t capacity = 0;  // the number of polygons bound to this node
    int32_t parent = -1;
};

struct B3dAmapObject
{
    uint64_t offset = 0;  // image offset of the record
    uint32_t size = 0, type = 0, adef_index = 0;
    std::vector<B3dAmapNode> nodes;
};

struct B3dGeomIssue
{
    uint64_t offset = 0;  // image offset
    std::string message;
};

struct B3dGeometry
{
    uint32_t version = 0;
    uint32_t grid_w = 0, grid_h = 0;
    std::vector<B3dVertex> vertices;
    std::vector<B3dMapping> mappings;
    std::vector<B3dTextureInfo> textures;
    std::vector<B3dBlock> blocks;         // in cell order
    std::vector<B3dPoly> polys;           // in GMAP order: the vector index is the polygon index
    std::vector<B3dLink> links;           // in GMAP order
    std::vector<B3dAmapObject> amap;      // empty when B3D_AMAP is not read
    bool amap_read = false;               // B3D_AMAP present with version >= 4 and objects > 0
    std::vector<B3dGeomIssue> warnings;   // unusual but readable data (see b3d_decode_geometry)
};

// Decodes the geometry of a decompressed level image. Reads the game part of the section chain
// only (the editor sections after B3D_END1 are never read, as in the game) and validates it:
// the header fields and record sizes, every section size against the header's counts, the
// B3D_TEX entries (each at most kB3dGeomMaxTexels, filling the section exactly), the B3D_GMAP
// cell stream (blocks of 1..254 entries ending exactly at the section end, the header's block
// and entry counts, 3 or 4 vertices per polygon, vertex indices < the vertex count, 1..5 LINK
// records), finite vertex coordinates, and B3D_AMAP when the game reads it.
// Throws ParseError (`name`, image offset, message) on the first problem; a level that fails
// has no geometry. Warnings (a header version between kB3dGeomMinVersion and the retail
// version) are collected in B3dGeometry::warnings.
// Polygons whose mapping or texture index is out of range are kept; the export makes them
// untextured (the game skips them).
B3dGeometry b3d_decode_geometry(const uint8_t* image, size_t size, const std::string& name);

// LINK entries are validated, not needed for the export: each record must name a block with
// flag 0x04 holding a polygon with render flag 0x100 and that link id (the first such polygon is
// the target), and link ids must be unique level-wide (the game's draw pass assumes both, and
// refuses levels that break the first). Problems are warnings: the geometry is still exported.
struct B3dLinkCheck
{
    uint64_t records = 0;          // LINK records
    uint64_t unresolved = 0;       // records with no target
    uint64_t shared_ids = 0;       // LINK targets whose link id an earlier target already uses
    std::vector<B3dGeomIssue> issues;  // the first problem of each kind, with its offset
};
B3dLinkCheck b3d_check_links(const B3dGeometry& g);

// ------------------------------------------------------------------ polygon helpers

// The UVs of a polygon's stored vertices from a mapping, as the game computes them: vertex k
// gets m.uv[perm[k]] (quads), or m.uv[perm[tri[k]]] (triangles with uv_mode bit 7), with perm =
// kB3dUvPerm[uv_mode & 7] and tri = kB3dUvTri[(uv_mode >> 4) & 3]. The game computes only 3
// UVs when bit 7 is set, so a quad with bit 7 has no defined 4th UV: returns false for it.
bool b3d_poly_uvs(const B3dMapping& m, uint8_t uv_mode, int nverts, float out[4][2]);

// Newell's normal of the stored vertex order (unnormalised, in double precision from the
// float32 positions). For TPM's clockwise polygons it points away from the stored normal.
void b3d_newell(const B3dGeometry& g, const B3dPoly& p, double out[3]);

// The effective normal: the stored normal when it is finite and not near zero (|n| >= 1e-6);
// otherwise the reversed, normalised Newell normal when the polygon has area (|N| > 1e-12);
// otherwise the axis of the facing class (1..6); otherwise (0, 0, 1). The retail disc has 12
// polygons with a non-finite stored normal, all of zero area and facing class 6: (0, 0, -1).
void b3d_poly_normal(const B3dGeometry& g, const B3dPoly& p, double out[3]);

// True when the stored order already runs counter-clockwise about the effective normal n:
// |N| >= 1e-9 and N.n > 1e-6 |N| |n|, with N the Newell normal. Otherwise the export reverses
// the order, keeping vertex 0 first: (v0, v2, v1) or (v0, v3, v2, v1).
bool b3d_poly_keeps_order(const B3dGeometry& g, const B3dPoly& p);

// The B3D_TEX index bound when the front side is drawn, or -1:
//   Game  the texture the game draws: tex_bucket for an opaque polygon (blend == 0), the front
//         mapping's texture for a blended one;
//   Mat   always the front mapping's texture (where the two differ, the game's choice looks
//         like a glitch in some cases; 88 polygons on the retail disc).
// -1 when the front mapping index or the texture index is out of range.
int b3d_front_texture(const B3dGeometry& g, const B3dPoly& p, GeometryTextureRule rule);

// ------------------------------------------------------------------ OBJ/MTL export

// A number as the OBJ/MTL files write it: the exact value rounded to 6 decimals, ties to even,
// never "-0.000000" (format_fixed, so every platform writes the same bytes).
std::string b3d_obj_number(double v);

struct B3dObjStats
{
    uint64_t vertices = 0;
    uint64_t faces = 0;               // one per polygon
    uint64_t triangles = 0, quads = 0;
    uint64_t texcoords = 0;           // vt lines
    uint64_t kept_order = 0;          // faces written in their stored order (already CCW)
    uint64_t untextured = 0;          // faces with material "untextured"
    uint64_t rule_differs = 0;        // faces whose texture differs under the other texture rule
    uint64_t moving = 0, hidden = 0;  // faces in amap<k> and in hidden
    uint64_t moving_objects = 0;      // amap<k> categories written
    uint64_t materials_used = 0;
    uint64_t first_untextured = 0;    // polygon index of the first untextured face
    std::string untextured_reason;    // why that face is untextured
};

// The OBJ and MTL text for a decoded level, with '\n' line ends (b3d_export_geometry writes them
// as CRLF, as the tool writes all its text files):
//   - "v X Y Z" per GVRT vertex, in order: position x scale, as the exact double product of the
//     float32 values (no welding, no axis change; OBJ vertex number = GVRT index + 1);
//   - "vt U V" per corner of every textured face, in face output order: U = u, V = 1 - v;
//   - "vn" per face: the effective normal;
//   - faces grouped by category, each started by "o <c>" and "g <c>": "static", then "amap<k>"
//     (object flag 0x04 with object k < the AMAP objects read) in ascending k, then "hidden"
//     (object flag 0x10); polygon index order within a category; "usemtl tex<i>" (or
//     "untextured") at each category start and material change; "f v/vt/vn ..." in
//     counter-clockwise order (b3d_poly_keeps_order), untextured faces as "f v//vn";
//   - the MTL: one material tex<i> per B3D_TEX entry, with "map_Kd <i>.bmp" (the files
//     process_b3d writes next to it), and "untextured" when a face uses it.
// `level_name` is <NAME> (the OBJ names <NAME>.mtl). `scale` is used as the float it is.
void b3d_obj_text(const B3dGeometry& g, const std::string& level_name, float scale, GeometryTextureRule rule,
                  std::string& obj, std::string& mtl, B3dObjStats* stats);

// For process_b3d (default mode, --geometry obj): decodes the image, logs the warnings and LINK
// problems, and writes <dumpdir>/<level_name>.obj and .mtl (CRLF). `name` is the file name used
// in messages. Returns false, after logging the error with file and offset, if the geometry was
// refused or could not be written; nothing is written then.
bool b3d_export_geometry(const std::vector<uint8_t>& image, const std::string& name, const std::string& dumpdir,
                         const std::string& level_name, const Options& opts);

} // namespace tpmx
