// TPMExtractor fork - TPM level geometry (.B3D): decoder and OBJ/MTL export.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "b3d_geom.h"

#include "b3d.h"
#include "binreader.h"
#include "log.h"
#include "platform.h"
#include "textout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace tpmx {

const int kB3dUvPerm[8][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1},
                              {3, 0, 1, 2}, {0, 3, 2, 1}, {2, 1, 0, 3}, {1, 2, 3, 0}};
const int kB3dUvTri[4][3] = {{0, 1, 3}, {0, 1, 2}, {1, 2, 3}, {0, 2, 3}};

namespace {

const char kTagPals[] = "B3D_PALS";
const char kTagGmap[] = "B3D_GMAP";
const char kTagGvrt[] = "B3D_GVRT";
const char kTagMat[] = "B3D_MAT ";
const char kTagTex[] = "B3D_TEX ";
const char kTagAmap[] = "B3D_AMAP";
const char kTagEnd1[] = "B3D_END1";

// Thresholds of the orientation rule and the normal fallback (fork decisions; no retail polygon
// lies near any of them).
const double kNormalMinLength = 1e-6;    // a shorter stored normal is not used
const double kNewellMinLength = 1e-12;   // a polygon with a shorter Newell normal has no area
const double kKeepMinNewell = 1e-9;      // ... for the orientation rule
const double kKeepMinCosine = 1e-6;      // N.n must exceed this times |N| |n| to keep the order

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

float lef32(const uint8_t* p)
{
    const uint32_t bits = le32(p);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

[[noreturn]] void fail(const std::string& name, uint64_t offset, const std::string& message)
{
    throw ParseError(name, offset, message);
}

// File offset of a header field (body offset + the 12-byte section header).
uint64_t header_field_offset(size_t body_offset) { return kB3dSectionHeaderSize + body_offset; }

double length3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// The effective normal and whether the stored order is kept, from one Newell computation.
void normal_and_order(const B3dGeometry& g, const B3dPoly& p, double n[3], bool* keep)
{
    double N[3];
    b3d_newell(g, p, N);
    const double newell_len = length3(N);
    const double s[3] = {p.normal[0], p.normal[1], p.normal[2]};
    if (std::isfinite(s[0]) && std::isfinite(s[1]) && std::isfinite(s[2]) && length3(s) >= kNormalMinLength) {
        n[0] = s[0];
        n[1] = s[1];
        n[2] = s[2];
    } else if (newell_len > kNewellMinLength) {
        n[0] = -N[0] / newell_len;
        n[1] = -N[1] / newell_len;
        n[2] = -N[2] / newell_len;
    } else if (p.facing >= 1 && p.facing <= 6) {
        // facing class: 1 +X, 2 -X, 3 +Y, 4 -Y, 5 +Z, 6 -Z
        n[0] = n[1] = n[2] = 0.0;
        n[(p.facing - 1) / 2] = (p.facing & 1) ? 1.0 : -1.0;
    } else {
        n[0] = 0.0;
        n[1] = 0.0;
        n[2] = 1.0;
    }
    if (keep) {
        const double dot = N[0] * n[0] + N[1] * n[1] + N[2] * n[2];
        *keep = newell_len >= kKeepMinNewell && dot > kKeepMinCosine * newell_len * length3(n);
    }
}

// The stored vertex position of output corner j: the stored order, or reversed with v0 first.
int output_corner(bool keep, int j, int nverts) { return (keep || j == 0) ? j : nverts - j; }

// The block of cell (x, y), or nullptr (blocks are in cell order).
const B3dBlock* find_block(const B3dGeometry& g, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || (uint64_t)x >= g.grid_w || (uint64_t)y >= g.grid_h)
        return nullptr;
    const uint64_t cell = (uint64_t)x + (uint64_t)y * g.grid_w;
    const auto it = std::lower_bound(g.blocks.begin(), g.blocks.end(), cell,
                                     [](const B3dBlock& b, uint64_t c) { return b.cell < c; });
    return (it != g.blocks.end() && it->cell == cell) ? &*it : nullptr;
}

std::string cell_text(const B3dGeometry& g, uint64_t cell)
{
    const uint64_t w = g.grid_w ? g.grid_w : 1;
    return format("(%llu,%llu)", (unsigned long long)(cell % w), (unsigned long long)(cell / w));
}

void append_number(std::string& out, double v)
{
    const std::string s = format_fixed(v, 6);
    out += s == "-0.000000" ? "0.000000" : s;
}

bool write_text_file(const std::string& path, const std::string& text, std::string* err)
{
    TextWriter w;
    if (!w.open(path, err))
        return false;
    w.write(text);
    return w.close(err);
}

} // namespace

// ------------------------------------------------------------------------------- decoding

B3dGeometry b3d_decode_geometry(const uint8_t* image, size_t size, const std::string& name)
{
    if (!image)
        size = 0;
    B3dGeometry g;

    // ---- the "B3D FILE" section: header fields
    if (size < kB3dSectionHeaderSize + kB3dHeaderBodySize || std::memcmp(image, "B3D FILE", 8) != 0 ||
        le32(image + 8) != kB3dHeaderBodySize)
        fail(name, 0, format("not a level file: it does not start with a %u-byte \"B3D FILE\" section",
                             kB3dHeaderBodySize));
    const uint8_t* hdr = image + kB3dSectionHeaderSize;
    auto field = [hdr](size_t body_offset) { return le32(hdr + body_offset); };

    g.version = field(kB3dHdrVersion);
    if (g.version > kB3dVersion)
        fail(name, header_field_offset(kB3dHdrVersion),
             format("header version %u is newer than the retail format (%u), which the game rejects too", g.version,
                    kB3dVersion));
    if (g.version < kB3dGeomMinVersion)
        fail(name, header_field_offset(kB3dHdrVersion),
             format("header version %u: levels older than version %u store their polygon records in another "
                    "layout, which is not supported",
                    g.version, kB3dGeomMinVersion));
    if (g.version < kB3dVersion)
        g.warnings.push_back({header_field_offset(kB3dHdrVersion),
                              format("header version %u is older than the retail format (%u); the geometry is read "
                                     "with the retail layout, which is untested for this version",
                                     g.version, kB3dVersion)});
    if (field(kB3dHdrGmapFirstRead) != kB3dGmapFirstRead)
        fail(name, header_field_offset(kB3dHdrGmapFirstRead),
             format("unsupported B3D_GMAP first-read size %u (the game's is %u)", field(kB3dHdrGmapFirstRead),
                    kB3dGmapFirstRead));
    if (field(kB3dHdrGmapEntrySize) != kB3dGmapEntrySize)
        fail(name, header_field_offset(kB3dHdrGmapEntrySize),
             format("unsupported B3D_GMAP entry size %u (the game's is %u)", field(kB3dHdrGmapEntrySize),
                    kB3dGmapEntrySize));
    if (field(kB3dHdrVertexSize) != kB3dVertexSize)
        fail(name, header_field_offset(kB3dHdrVertexSize),
             format("unsupported B3D_GVRT record size %u (the game's is %u)", field(kB3dHdrVertexSize), kB3dVertexSize));
    if (field(kB3dHdrMappingSize) != kB3dMappingSize)
        fail(name, header_field_offset(kB3dHdrMappingSize),
             format("unsupported B3D_MAT record size %u (the game's is %u)", field(kB3dHdrMappingSize),
                    kB3dMappingSize));
    g.grid_w = field(kB3dHdrGridW);
    g.grid_h = field(kB3dHdrGridH);
    const uint32_t num_palettes = field(kB3dHdrNumPalettes);
    const uint32_t num_vertices = field(kB3dHdrNumVertices);
    const uint32_t num_mappings = field(kB3dHdrNumMappings);
    const uint32_t num_textures = field(kB3dHdrNumTextures);
    const uint32_t num_movers = field(kB3dHdrNumMovers);
    const uint32_t mover_version = field(kB3dHdrMoverVersion);

    // ---- the game part of the section chain ("B3D FILE" .. "B3D_END1"); the first section with
    // each tag counts, as in the game's loader. Nothing after B3D_END1 is read.
    const B3dWalk walk = b3d_walk_sections(image, size);
    if (!walk.game_part_complete)
        fail(name, walk.stop_offset, "the section chain is broken before B3D_END1: " + walk.problem);
    const B3dSection *pals = nullptr, *gmap = nullptr, *gvrt = nullptr, *mat = nullptr, *tex = nullptr,
                     *amap = nullptr, *end1 = nullptr;
    for (const B3dSection& s : walk.sections) {
        const B3dSection** slot = s.tag == kTagPals   ? &pals
                                  : s.tag == kTagGmap ? &gmap
                                  : s.tag == kTagGvrt ? &gvrt
                                  : s.tag == kTagMat  ? &mat
                                  : s.tag == kTagTex  ? &tex
                                  : s.tag == kTagAmap ? &amap
                                                      : nullptr;
        if (slot && !*slot)
            *slot = &s;
        if (s.tag == kTagEnd1) {
            end1 = &s;
            break;
        }
    }
    const uint64_t end1_offset = end1 ? end1->offset : walk.stop_offset;
    const struct
    {
        const B3dSection* s;
        const char* tag;
    } required[] = {{pals, "B3D_PALS"}, {gmap, "B3D_GMAP"}, {gvrt, "B3D_GVRT"}, {mat, "B3D_MAT"}, {tex, "B3D_TEX"}};
    for (const auto& r : required)
        if (!r.s)
            fail(name, end1_offset, format("no %s section before B3D_END1", r.tag));

    // ---- section sizes against the header's counts (game-part sections: extent == size field)
    auto check_size = [&](const B3dSection* s, uint64_t need, const char* what, uint32_t count) {
        if (s->size_field != need)
            fail(name, s->offset,
                 format("%s is %u bytes; the header's %u %s need %llu", s->tag.c_str(), s->size_field, count, what,
                        (unsigned long long)need));
    };
    check_size(pals, (uint64_t)num_palettes * kB3dPaletteSize, "palette(s)", num_palettes);
    check_size(gvrt, (uint64_t)num_vertices * kB3dVertexSize, "vertices", num_vertices);
    check_size(mat, (uint64_t)num_mappings * kB3dMappingSize, "mapping record(s)", num_mappings);

    // ---- B3D_GVRT
    {
        BinReader r(image + gvrt->body(), (size_t)gvrt->extent, name, gvrt->body());
        g.vertices.resize(num_vertices);
        for (uint32_t i = 0; i < num_vertices; i++) {
            const size_t at = r.pos();
            B3dVertex& v = g.vertices[i];
            v.x = r.f32();
            v.y = r.f32();
            v.z = r.f32();
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                r.fail_at(at, format("vertex %u has a coordinate that is not a finite number", i));
        }
    }

    // ---- B3D_MAT (UV mappings)
    {
        BinReader r(image + mat->body(), (size_t)mat->extent, name, mat->body());
        g.mappings.resize(num_mappings);
        for (B3dMapping& m : g.mappings) {
            for (int c = 0; c < 4; c++) {
                m.uv[c][0] = r.f32();
                m.uv[c][1] = r.f32();
            }
            m.tex = r.i32();
            m.flags = r.u32();
        }
    }

    // ---- B3D_TEX: the header's number of textures, filling the section exactly
    {
        BinReader r(image + tex->body(), (size_t)tex->extent, name, tex->body());
        g.textures.reserve((size_t)std::min<uint64_t>(num_textures, tex->extent / (kB3dTextureHeaderSize + 1)));
        for (uint32_t i = 0; i < num_textures; i++) {
            const size_t at = r.pos();
            if (!r.can_read(kB3dTextureHeaderSize))
                r.fail_at(at, format("texture %u of %u: its header runs past the end of B3D_TEX", i, num_textures));
            B3dTextureInfo t;
            t.offset = r.file_offset();
            t.width = r.i32();
            t.height = r.i32();
            t.palette = r.i32();
            r.skip(4);  // reserved
            const int64_t texels = (int64_t)t.width * t.height;
            if (t.width <= 0 || t.height <= 0 || texels > kB3dGeomMaxTexels)
                r.fail_at(at, format("texture %u has the size %dx%d; the game loads textures of 1 to %lld texels", i,
                                     t.width, t.height, (long long)kB3dGeomMaxTexels));
            if (!r.can_read((size_t)texels))
                r.fail_at(at, format("texture %u: its %dx%d pixels run past the end of B3D_TEX", i, t.width, t.height));
            r.skip((size_t)texels);
            g.textures.push_back(t);
        }
        if (!r.at_end())
            r.fail_at(r.pos(), format("B3D_TEX holds %zu bytes after its %u texture(s)", r.remaining(), num_textures));
    }

    // ---- B3D_GMAP: grid_w x grid_h cells, row-major. Every cell takes at least one byte, so the
    // loop ends with the stream whatever the grid size.
    {
        BinReader r(image + gmap->body(), (size_t)gmap->extent, name, gmap->body());
        const uint64_t cells = (uint64_t)g.grid_w * g.grid_h;
        uint64_t entries = 0;
        for (uint64_t cell = 0; cell < cells; cell++) {
            const size_t at = r.pos();
            if (r.at_end())
                r.fail_at(at, format("B3D_GMAP ends before cell %s; the grid has %ux%u cells", cell_text(g, cell).c_str(),
                                     g.grid_w, g.grid_h));
            const uint8_t count = r.u8();
            if (count == kB3dGmapEmptyCell)
                continue;
            if (count == 0)
                r.fail_at(at, format("cell %s: a block with 0 entries", cell_text(g, cell).c_str()));
            r.seek(at);
            if (!r.can_read(kB3dGmapBlockHeaderSize + (size_t)kB3dGmapEntrySize * count))
                r.fail_at(at, format("cell %s: its block of %u entries runs past the end of B3D_GMAP",
                                     cell_text(g, cell).c_str(), count));
            B3dBlock b;
            b.offset = r.file_offset();
            b.cell = cell;
            b.count = count;
            b.flags = r.span(kB3dGmapBlockHeaderSize)[1];
            b.first_poly = (uint32_t)g.polys.size();
            const uint32_t block_index = (uint32_t)g.blocks.size();
            for (unsigned j = 0; j < count; j++) {
                const uint64_t eo = r.file_offset();
                const uint8_t* e = r.span(kB3dGmapEntrySize);
                if (le16(e) == kB3dLinkMarker) {
                    B3dLink link;
                    link.offset = eo;
                    link.block = block_index;
                    link.count = e[2];
                    if (link.count < 1 || link.count > kB3dLinkMaxRecords)
                        fail(name, eo + 2, format("LINK entry with %u records (1 to %d)", link.count, kB3dLinkMaxRecords));
                    for (int k = 0; k < link.count; k++) {
                        const uint8_t* q = e + 4 + 12 * k;
                        link.rec[k].cell_x = (int16_t)le16(q);
                        link.rec[k].cell_y = (int16_t)le16(q + 2);
                        link.rec[k].id = le32(q + 8);
                    }
                    g.links.push_back(link);
                    continue;
                }
                B3dPoly p;
                p.offset = eo;
                p.block = block_index;
                p.nverts = e[0x25];
                if (p.nverts != 3 && p.nverts != 4)
                    fail(name, eo + 0x25, format("polygon %zu has %u vertices (3 or 4)", g.polys.size(), p.nverts));
                for (int k = 0; k < 4; k++)
                    p.v[k] = le16(e + 2 * k);
                for (int k = 0; k < p.nverts; k++)
                    if (p.v[k] >= num_vertices)
                        fail(name, eo + 2 * k,
                             format("polygon %zu: vertex index %u, but the level has %u vertices", g.polys.size(),
                                    p.v[k], num_vertices));
                for (int k = 0; k < 3; k++)
                    p.normal[k] = lef32(e + 0x10 + 4 * k);
                p.mat_front = (int16_t)le16(e + 0x20);
                p.mat_back = le16(e + 0x22);
                p.tex_bucket = e[0x24];
                p.uv_mode = e[0x26];
                p.facing = e[0x27];
                p.obj_flags = e[0x28];
                p.amap_node = e[0x29];
                p.amap_index = e[0x2A];
                p.blend = e[0x2B];
                p.render = le16(e + 0x2C);
                p.link_id = le16(e + 0x30);
                p.coll = le16(e + 0x3C);
                p.surf = le16(e + 0x3E);
                g.polys.push_back(p);
            }
            b.num_polys = (uint32_t)(g.polys.size() - b.first_poly);
            g.blocks.push_back(b);
            entries += count;
        }
        if (!r.at_end())
            r.fail_at(r.pos(), format("B3D_GMAP holds %zu bytes after its last cell", r.remaining()));
        if (g.blocks.size() != field(kB3dHdrGmapBlocks))
            fail(name, header_field_offset(kB3dHdrGmapBlocks),
                 format("B3D_GMAP holds %zu blocks, the header says %u", g.blocks.size(), field(kB3dHdrGmapBlocks)));
        if (entries != field(kB3dHdrGmapEntries))
            fail(name, header_field_offset(kB3dHdrGmapEntries),
                 format("B3D_GMAP holds %llu entries, the header says %u", (unsigned long long)entries,
                        field(kB3dHdrGmapEntries)));
    }

    // ---- B3D_AMAP (read by the game from version 4 on): a size table, then the records. Only
    // the part that binds polygons to objects and nodes is decoded.
    if (amap && mover_version >= kB3dAmapMinVersion && num_movers > 0) {
        g.amap_read = true;
        if ((uint64_t)num_movers * 4 > amap->extent)
            fail(name, amap->offset,
                 format("B3D_AMAP is %u bytes, too small for the size table of the header's %u moving objects",
                        amap->size_field, num_movers));
        BinReader r(image + amap->body(), (size_t)amap->extent, name, amap->body());
        std::vector<uint32_t> sizes(num_movers);
        for (uint32_t& s : sizes)
            s = r.u32();
        g.amap.reserve(num_movers);
        for (uint32_t i = 0; i < num_movers; i++) {
            const size_t at = r.pos();
            const uint32_t sz = sizes[i];
            if (sz < kB3dAmapRecordHeaderSize || sz > r.remaining())
                r.fail_at(at, format("moving object %u: its %u bytes do not fit (at least %u needed, %zu left in "
                                     "B3D_AMAP)",
                                     i, sz, kB3dAmapRecordHeaderSize, r.remaining()));
            B3dAmapObject o;
            o.offset = r.file_offset();
            o.size = sz;
            const uint8_t* rec = r.span(sz);
            o.type = le32(rec + 0x04);
            o.adef_index = le32(rec + 0x18);
            const uint32_t nodes = le32(rec + 0x24);
            if (kB3dAmapRecordHeaderSize + (uint64_t)kB3dAmapNodeSize * nodes > sz)
                r.fail_at(at, format("moving object %u: its %u nodes do not fit in its %u bytes", i, nodes, sz));
            o.nodes.resize(nodes);
            for (uint32_t k = 0; k < nodes; k++) {
                const uint8_t* q = rec + kB3dAmapRecordHeaderSize + (size_t)kB3dAmapNodeSize * k;
                B3dAmapNode& n = o.nodes[k];
                for (int c = 0; c < 3; c++)
                    n.pivot[c] = lef32(q + 0x08 + 4 * c);
                n.capacity = le32(q + 0x4C);
                n.parent = (int32_t)le32(q + 0x54);
            }
            g.amap.push_back(std::move(o));
        }
        if (!r.at_end())
            r.fail_at(r.pos(), format("B3D_AMAP holds %zu bytes after its last moving object", r.remaining()));
    }
    return g;
}

B3dLinkCheck b3d_check_links(const B3dGeometry& g)
{
    B3dLinkCheck c;
    // link ids must be unique level-wide (the draw pass keeps one "drawn" flag per id)
    std::vector<int64_t> first_with_id(65536, -1);
    for (size_t i = 0; i < g.polys.size(); i++) {
        const B3dPoly& p = g.polys[i];
        if (!(p.render & kB3dRenderLinkTarget))
            continue;
        int64_t& first = first_with_id[p.link_id];
        if (first < 0) {
            first = (int64_t)i;
            continue;
        }
        if (c.shared_ids++ == 0) {
            const B3dPoly& q = g.polys[(size_t)first];
            c.issues.push_back({p.offset, format("LINK targets %lld in cell %s and %zu in cell %s share the link id %u, "
                                                 "which the game's draw pass expects to be unique",
                                                 (long long)first, cell_text(g, g.blocks[q.block].cell).c_str(), i,
                                                 cell_text(g, g.blocks[p.block].cell).c_str(), p.link_id)});
        }
    }
    // every record names the first polygon with that link id in a LINK-target block
    for (const B3dLink& link : g.links) {
        for (int k = 0; k < link.count; k++) {
            c.records++;
            const B3dLinkRecord& rec = link.rec[k];
            const B3dBlock* b = find_block(g, rec.cell_x, rec.cell_y);
            bool found = false;
            if (b && (b->flags & kB3dBlockLinkTarget)) {
                for (uint32_t j = 0; j < b->num_polys && !found; j++) {
                    const B3dPoly& p = g.polys[b->first_poly + j];
                    found = (p.render & kB3dRenderLinkTarget) && (uint32_t)p.link_id == rec.id;
                }
            }
            if (!found && c.unresolved++ == 0)
                c.issues.push_back(
                    {link.offset + 4 + 12 * (uint64_t)k,
                     format("the LINK record in cell %s names cell (%d,%d) and link id %u, which hold no such target "
                            "(the game refuses such a level); the geometry is exported all the same",
                            cell_text(g, g.blocks[link.block].cell).c_str(), rec.cell_x, rec.cell_y, rec.id)});
        }
    }
    if (c.unresolved > 1)
        c.issues.push_back({c.issues.back().offset, format("%llu of the %llu LINK records name no target",
                                                           (unsigned long long)c.unresolved,
                                                           (unsigned long long)c.records)});
    return c;
}

// ---------------------------------------------------------------------------------- helpers

bool b3d_poly_uvs(const B3dMapping& m, uint8_t uv_mode, int nverts, float out[4][2])
{
    const int* perm = kB3dUvPerm[uv_mode & 7];
    if (uv_mode & kB3dUvTriangle) {
        if (nverts > 3)
            return false;  // the game computes 3 UVs; a 4th corner has none
        const int* tri = kB3dUvTri[(uv_mode >> 4) & 3];
        for (int k = 0; k < 3; k++) {
            out[k][0] = m.uv[perm[tri[k]]][0];
            out[k][1] = m.uv[perm[tri[k]]][1];
        }
        out[3][0] = out[3][1] = 0.0f;
        return true;
    }
    for (int k = 0; k < 4; k++) {
        out[k][0] = m.uv[perm[k]][0];
        out[k][1] = m.uv[perm[k]][1];
    }
    return true;
}

void b3d_newell(const B3dGeometry& g, const B3dPoly& p, double out[3])
{
    // Double precision from the float32 positions, summed in vertex order: the same bits on
    // every platform (no contraction; correctly rounded IEEE operations).
    double nx = 0.0, ny = 0.0, nz = 0.0;
    const int n = p.nverts;
    for (int i = 0; i < n; i++) {
        const B3dVertex& a = g.vertices[p.v[i]];
        const B3dVertex& b = g.vertices[p.v[(i + 1) % n]];
        const double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z;
        nx += (ay - by) * (az + bz);
        ny += (az - bz) * (ax + bx);
        nz += (ax - bx) * (ay + by);
    }
    out[0] = nx;
    out[1] = ny;
    out[2] = nz;
}

void b3d_poly_normal(const B3dGeometry& g, const B3dPoly& p, double out[3]) { normal_and_order(g, p, out, nullptr); }

bool b3d_poly_keeps_order(const B3dGeometry& g, const B3dPoly& p)
{
    double n[3];
    bool keep = false;
    normal_and_order(g, p, n, &keep);
    return keep;
}

int b3d_front_texture(const B3dGeometry& g, const B3dPoly& p, GeometryTextureRule rule)
{
    if (p.mat_front < 0 || (size_t)p.mat_front >= g.mappings.size())
        return -1;  // the game skips a polygon whose mapping does not exist
    const int64_t t = (rule == GeometryTextureRule::Game && p.blend == 0) ? (int64_t)p.tex_bucket
                                                                           : (int64_t)g.mappings[(size_t)p.mat_front].tex;
    return (t >= 0 && (uint64_t)t < g.textures.size()) ? (int)t : -1;
}

std::string b3d_obj_number(double v)
{
    std::string s;
    append_number(s, v);
    return s;
}

// ------------------------------------------------------------------------------------ export

void b3d_obj_text(const B3dGeometry& g, const std::string& level_name, float scale, GeometryTextureRule rule,
                  std::string& obj, std::string& mtl, B3dObjStats* stats)
{
    B3dObjStats st;
    const double S = (double)scale;  // positions: the exact double product of two float32 values
    const GeometryTextureRule other =
        rule == GeometryTextureRule::Game ? GeometryTextureRule::Mat : GeometryTextureRule::Game;
    const uint64_t movers = g.amap.size();
    const uint64_t hidden_rank = movers + 1;

    struct Face
    {
        uint32_t poly;
        uint64_t rank;  // 0 static, 1 + k amap<k>, movers + 1 hidden
        int tex;        // -1: untextured
        bool keep;
        double n[3];
        float uv[4][2];
    };
    std::vector<Face> faces(g.polys.size());
    for (size_t i = 0; i < g.polys.size(); i++) {
        const B3dPoly& p = g.polys[i];
        Face& f = faces[i];
        f.poly = (uint32_t)i;
        if (p.obj_flags & kB3dObjHidden)
            f.rank = hidden_rank;
        else if ((p.obj_flags & kB3dObjMoving) && p.amap_index < movers)
            f.rank = 1 + (uint64_t)p.amap_index;
        else
            f.rank = 0;
        normal_and_order(g, p, f.n, &f.keep);
        f.tex = b3d_front_texture(g, p, rule);
        if (b3d_front_texture(g, p, other) != f.tex)
            st.rule_differs++;
        const char* why = nullptr;
        if (f.tex < 0) {
            why = (p.mat_front < 0 || (size_t)p.mat_front >= g.mappings.size())
                      ? "its front mapping index is out of range"
                      : "the texture it binds is out of range";
        } else if (!b3d_poly_uvs(g.mappings[(size_t)p.mat_front], p.uv_mode, p.nverts, f.uv)) {
            why = "a quad with the triangle UV flag (uv_mode bit 7), whose 4th UV the game leaves undefined";
        } else {
            for (int k = 0; k < p.nverts && !why; k++)
                if (!std::isfinite(f.uv[k][0]) || !std::isfinite(f.uv[k][1]))
                    why = "its mapping has a UV that is not a finite number";
        }
        if (why) {
            f.tex = -1;
            if (st.untextured++ == 0) {
                st.first_untextured = i;
                st.untextured_reason = why;
            }
        }
        if (f.keep)
            st.kept_order++;
        if (p.nverts == 3)
            st.triangles++;
        else
            st.quads++;
        if (f.rank == hidden_rank)
            st.hidden++;
        else if (f.rank)
            st.moving++;
    }
    // categories in rank order, polygon index order within each
    std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.rank < b.rank; });
    st.vertices = g.vertices.size();
    st.faces = faces.size();

    obj.clear();
    obj.reserve(g.vertices.size() * 34 + faces.size() * 140 + 256);
    obj += "# TPM level geometry: " + level_name + " (scale ";
    append_number(obj, S);
    obj += "; Z up; +X right, +Y forward)\n";
    obj += "mtllib " + level_name + ".mtl\n";
    for (const B3dVertex& v : g.vertices) {
        obj += "v ";
        append_number(obj, (double)v.x * S);
        obj += ' ';
        append_number(obj, (double)v.y * S);
        obj += ' ';
        append_number(obj, (double)v.z * S);
        obj += '\n';
    }
    // texture coordinates: every corner of every textured face, in face output order
    std::vector<uint64_t> first_vt(faces.size(), 0);
    uint64_t vt = 0;
    for (size_t fi = 0; fi < faces.size(); fi++) {
        const Face& f = faces[fi];
        if (f.tex < 0)
            continue;
        first_vt[fi] = vt + 1;
        const int nv = g.polys[f.poly].nverts;
        for (int j = 0; j < nv; j++) {
            const int k = output_corner(f.keep, j, nv);
            obj += "vt ";
            append_number(obj, (double)f.uv[k][0]);
            obj += ' ';
            append_number(obj, 1.0 - (double)f.uv[k][1]);  // texture row 0 at the top of the OBJ's v
            obj += '\n';
            vt++;
        }
    }
    st.texcoords = vt;
    for (const Face& f : faces) {
        obj += "vn ";
        append_number(obj, f.n[0]);
        obj += ' ';
        append_number(obj, f.n[1]);
        obj += ' ';
        append_number(obj, f.n[2]);
        obj += '\n';
    }
    std::vector<bool> used(g.textures.size(), false);
    bool cat_started = false;
    uint64_t cat = 0;
    int cur_mat = -2;
    for (size_t fi = 0; fi < faces.size(); fi++) {
        const Face& f = faces[fi];
        const B3dPoly& p = g.polys[f.poly];
        if (!cat_started || f.rank != cat) {
            const std::string name = f.rank == 0             ? std::string("static")
                                     : f.rank == hidden_rank ? std::string("hidden")
                                                             : format("amap%llu", (unsigned long long)(f.rank - 1));
            obj += "o " + name + "\ng " + name + '\n';
            if (f.rank != 0 && f.rank != hidden_rank)
                st.moving_objects++;
            cat_started = true;
            cat = f.rank;
            cur_mat = -2;
        }
        if (f.tex != cur_mat) {
            obj += f.tex < 0 ? std::string("usemtl untextured\n") : format("usemtl tex%d\n", f.tex);
            cur_mat = f.tex;
        }
        if (f.tex >= 0)
            used[(size_t)f.tex] = true;
        obj += 'f';
        for (int j = 0; j < p.nverts; j++) {
            const int k = output_corner(f.keep, j, p.nverts);
            if (f.tex >= 0)
                obj += format(" %u/%llu/%llu", p.v[k] + 1u, (unsigned long long)(first_vt[fi] + (uint64_t)j),
                              (unsigned long long)(fi + 1));
            else
                obj += format(" %u//%llu", p.v[k] + 1u, (unsigned long long)(fi + 1));
        }
        obj += '\n';
    }
    st.materials_used = (uint64_t)std::count(used.begin(), used.end(), true) + (st.untextured ? 1 : 0);

    mtl = "# TPM level materials: " + level_name + "\n";
    for (size_t i = 0; i < g.textures.size(); i++)
        mtl += format("\nnewmtl tex%zu\nKa 1.000000 1.000000 1.000000\nKd 1.000000 1.000000 1.000000\n"
                      "Ks 0.000000 0.000000 0.000000\nd 1.000000\nillum 1\nmap_Kd %zu.bmp\n",
                      i, i);
    if (st.untextured)
        mtl += "\nnewmtl untextured\nKa 1.000000 1.000000 1.000000\nKd 0.500000 0.500000 0.500000\n"
               "Ks 0.000000 0.000000 0.000000\nd 1.000000\nillum 1\n";
    if (stats)
        *stats = st;
}

bool b3d_export_geometry(const std::vector<uint8_t>& image, const std::string& name, const std::string& dumpdir,
                         const std::string& level_name, const Options& opts)
{
    B3dGeometry g;
    try {
        g = b3d_decode_geometry(image.data(), image.size(), name);
    } catch (const ParseError& e) {
        log::error_at(e.file(), e.offset(), "level geometry not exported: %s\n", e.message().c_str());
        return false;
    }
    for (const B3dGeomIssue& w : g.warnings)
        log::warn_at(name, w.offset, "level geometry: %s\n", w.message.c_str());
    const B3dLinkCheck links = b3d_check_links(g);
    for (const B3dGeomIssue& w : links.issues)
        log::warn_at(name, w.offset, "level geometry: %s\n", w.message.c_str());

    std::string obj, mtl;
    B3dObjStats st;
    b3d_obj_text(g, level_name, opts.scale, opts.geometry_texture_rule, obj, mtl, &st);
    if (st.untextured)
        log::warn_at(name, g.polys[(size_t)st.first_untextured].offset,
                     "level geometry: %llu polygon(s) exported with the material \"untextured\"; the first, polygon "
                     "%llu: %s\n",
                     (unsigned long long)st.untextured, (unsigned long long)st.first_untextured,
                     st.untextured_reason.c_str());

    std::string obj_path, mtl_path, why;
    if (!platform::safe_output_path(dumpdir, level_name + ".obj", obj_path, &why) ||
        !platform::safe_output_path(dumpdir, level_name + ".mtl", mtl_path, &why)) {
        log::error("%s: level geometry not exported: %s\n", name.c_str(), why.c_str());
        return false;
    }
    log::verbose("dumping: %s...\n", obj_path.c_str());
    if (!write_text_file(obj_path, obj, &why) || !write_text_file(mtl_path, mtl, &why)) {
        log::error("%s\n", why.c_str());
        std::string ignored;
        platform::remove_file(obj_path, &ignored);  // no OBJ without its MTL
        platform::remove_file(mtl_path, &ignored);
        return false;
    }
    log::info("%s: %llu vertices, %llu faces (%llu triangles, %llu quads); %llu moving in %llu object(s), %llu "
              "hidden; %llu in their stored order, %llu untextured, %llu textured otherwise under "
              "--geometry-texture-rule %s; %llu unresolved LINK record(s)\n",
              obj_path.c_str(), (unsigned long long)st.vertices, (unsigned long long)st.faces,
              (unsigned long long)st.triangles, (unsigned long long)st.quads, (unsigned long long)st.moving,
              (unsigned long long)st.moving_objects, (unsigned long long)st.hidden, (unsigned long long)st.kept_order,
              (unsigned long long)st.untextured, (unsigned long long)st.rule_differs,
              opts.geometry_texture_rule == GeometryTextureRule::Game ? "mat" : "game",
              (unsigned long long)links.unresolved);
    return true;
}

} // namespace tpmx
