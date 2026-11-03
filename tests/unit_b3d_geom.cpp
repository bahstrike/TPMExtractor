// TPMExtractor fork - unit tests of the level geometry decoder and OBJ/MTL writer (b3d_geom.h).
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// tests/run_tests.py compares the tool's OBJ/MTL files with an independent model on synthetic
// levels; this program checks the pieces directly, on a small level built here (abstract shapes,
// generated palette and pixels):
//   - the OBJ number format: exact, 6 decimals, ties to even, never "-0.000000";
//   - the UV corner tables and triangle subsets, and the quad with the triangle flag;
//   - the effective normal (stored, Newell fallback, facing class, +Z) and the orientation rule;
//   - the two texture rules and out-of-range indices;
//   - decoding the level, its OBJ/MTL text, LINK checks, and the error offsets of damaged copies.
// Exit 0 = pass, 1 = fail.

#include "b3d.h"
#include "b3d_geom.h"
#include "binreader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace tpmx;

namespace {

int g_checks = 0, g_fail = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

void expect_str(const std::string& got, const std::string& want, const std::string& what)
{
    check(got == want, what + ": got \"" + got + "\", want \"" + want + "\"");
}

// ------------------------------------------------------------------ a level built in memory

struct Bytes
{
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u16(uint32_t v)
    {
        u8(v & 0xFF);
        u8(v >> 8);
    }
    void u32(uint32_t v)
    {
        u16(v & 0xFFFF);
        u16(v >> 16);
    }
    void f32(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        u32(u);
    }
    void raw(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
    void put32(size_t off, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
            b[off + i] = (uint8_t)(v >> (8 * i));
    }
};

void section(Bytes& out, const char* tag, const std::vector<uint8_t>& body)
{
    for (int i = 0; i < 8; i++)
        out.u8((uint8_t)tag[i]);
    out.u32((uint32_t)body.size());
    out.raw(body);
}

// A 64-byte polygon entry.
std::vector<uint8_t> poly_entry(std::vector<uint16_t> v, float nx, float ny, float nz, int16_t mat_front,
                                uint8_t tex_bucket, uint8_t uv_mode, uint8_t facing, uint8_t obj = 0,
                                uint8_t obj_index = 0, uint8_t blend = 0, uint16_t render = 0, uint16_t link_id = 0)
{
    Bytes e;
    const size_t n = v.size();
    v.resize(4, 0);
    for (uint16_t x : v)
        e.u16(x);
    for (int i = 0; i < 8; i++)
        e.u8(0);
    e.f32(nx);
    e.f32(ny);
    e.f32(nz);
    e.u32(0);                   // +0x1C
    e.u16((uint16_t)mat_front); // +0x20
    e.u16((uint16_t)mat_front); // +0x22 back
    e.u8(tex_bucket);           // +0x24
    e.u8((uint8_t)n);           // +0x25
    e.u8(uv_mode);
    e.u8(facing);
    e.u8(obj);                  // +0x28
    e.u8(0);
    e.u8(obj_index);
    e.u8(blend);                // +0x2B
    e.u16(render);              // +0x2C
    e.u16(0);
    e.u16(link_id);             // +0x30
    while (e.b.size() < 64)
        e.u8(0);
    return e.b;
}

// Layout of the level (offsets checked below):
//   grid 3 x 1: cell 0 holds polygons 0 (clockwise floor quad) and 1 (counter-clockwise triangle,
//   texture-rule difference), cell 1 a LINK entry to polygon 2, cell 2 polygon 2 (a LINK target,
//   moving with object 0) and polygon 3 (hidden, non-finite normal, zero area, facing -Z).
struct Level
{
    std::vector<uint8_t> image;
    size_t gmap = 0, gvrt = 0, tex = 0, end1 = 0;  // section header offsets
    size_t poly[4] = {};                           // entry offsets
    size_t link = 0;
};

Level build_level()
{
    Level L;
    Bytes gmap;
    // cell 0: block of 2
    gmap.u8(2);
    gmap.u8(0);
    gmap.u16(0);
    gmap.f32(0.0f);
    gmap.f32(1.0f);
    gmap.u32(0);
    const size_t p0 = gmap.b.size();
    gmap.raw(poly_entry({0, 1, 2, 3}, 0, 0, 1, 0, 0, 0x00, 5));             // clockwise seen from +Z
    const size_t p1 = gmap.b.size();
    gmap.raw(poly_entry({0, 3, 4}, 0, 0, 1, 1, 0, 0x80 | (1 << 4) | 5, 5));  // counter-clockwise; TRI 1, PERM 5
    // cell 1: a LINK entry naming polygon 2 in cell (2,0) with id 7
    gmap.u8(1);
    gmap.u8(0x08);
    gmap.u16(0);
    gmap.f32(0.0f);
    gmap.f32(1.0f);
    gmap.u32(0);
    const size_t lk = gmap.b.size();
    {
        Bytes e;
        e.u16(0xFFFF);
        e.u8(1);
        e.u8(0);
        e.u16(2);
        e.u16(0);
        e.u32(0);
        e.u32(7);
        while (e.b.size() < 64)
            e.u8(0);
        gmap.raw(e.b);
    }
    // cell 2: polygons 2 and 3
    gmap.u8(2);
    gmap.u8(0x04 | 0x02);
    gmap.u16(0);
    gmap.f32(0.0f);
    gmap.f32(1.0f);
    gmap.u32(0);
    const size_t p2 = gmap.b.size();
    gmap.raw(poly_entry({5, 6, 7, 8}, 0, 0, 1, 1, 0, 0x00, 5, kB3dObjMoving, 0, 0, kB3dRenderLinkTarget, 7));
    const size_t p3 = gmap.b.size();
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    gmap.raw(poly_entry({5, 5, 6}, -inf, nan, nan, 0, 0, 0x81, 6, kB3dObjHidden));

    Bytes gvrt;
    const float v[9][3] = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {-1, 0.5f, 0},
                           {2, 0, 0.25f}, {2, 1, 0.25f}, {3.5f, 1, 0.25f}, {3.5f, 0, 0.25f}};
    for (const auto& p : v)
        for (float c : p)
            gvrt.f32(c);
    Bytes mat;
    const float uv[2][8] = {{0, 0, 1, 0, 1, 1, 0, 1}, {0.25f, 0.5f, 0.75f, 0.5f, 0.75f, 1, 0.25f, 1}};
    for (int m = 0; m < 2; m++) {
        for (float c : uv[m])
            mat.f32(c);
        mat.u32((uint32_t)m);  // page m
        mat.u32(0);
    }
    Bytes tex;
    for (int t = 0; t < 2; t++) {
        tex.u32(2);
        tex.u32(2);
        tex.u32(0);
        tex.u32(0);
        for (int i = 0; i < 4; i++)
            tex.u8((uint8_t)(t * 64 + i * 16));
    }
    Bytes pals;
    for (int i = 0; i < 768; i++)
        pals.u8((uint8_t)(i * 7));

    Bytes head;
    head.b.assign(kB3dHeaderBodySize, 0);
    head.put32(kB3dHdrVersion, kB3dVersion);
    head.put32(kB3dHdrNumPalettes, 1);
    head.put32(kB3dHdrGridW, 3);
    head.put32(kB3dHdrGridH, 1);
    head.put32(kB3dHdrGmapBlocks, 3);
    head.put32(kB3dHdrGmapFirstRead, 80);
    head.put32(kB3dHdrGmapEntries, 5);
    head.put32(kB3dHdrGmapEntrySize, 64);
    head.put32(kB3dHdrNumVertices, 9);
    head.put32(kB3dHdrVertexSize, 12);
    head.put32(kB3dHdrNumMappings, 2);
    head.put32(kB3dHdrMappingSize, 40);
    head.put32(kB3dHdrNumTextures, 2);
    head.put32(kB3dHdrNumMovers, 1);
    head.put32(kB3dHdrMoverVersion, 4);
    Bytes amap;  // one moving object without nodes
    amap.u32(0x84);
    for (int i = 0; i < 0x84; i++)
        amap.u8(0);

    Bytes img;
    section(img, "B3D FILE", head.b);
    section(img, "B3D_PALS", pals.b);
    L.gmap = img.b.size();
    section(img, "B3D_GMAP", gmap.b);
    L.gvrt = img.b.size();
    section(img, "B3D_GVRT", gvrt.b);
    section(img, "B3D_MAT ", mat.b);
    L.tex = img.b.size();
    section(img, "B3D_TEX ", tex.b);
    section(img, "B3D_AMAP", amap.b);
    L.end1 = img.b.size();
    section(img, "B3D_END1", {});
    section(img, "B3D_GMAP", {});  // after B3D_END1: never read
    L.image = img.b;
    const size_t gbody = L.gmap + kB3dSectionHeaderSize;
    L.poly[0] = gbody + p0;
    L.poly[1] = gbody + p1;
    L.poly[2] = gbody + p2;
    L.poly[3] = gbody + p3;
    L.link = gbody + lk;
    return L;
}

// The offset of the ParseError decoding `image` throws, or -1 when it decodes.
long long error_offset(const std::vector<uint8_t>& image, std::string* message = nullptr)
{
    try {
        (void)b3d_decode_geometry(image.data(), image.size(), "unit.b3d");
    } catch (const ParseError& e) {
        if (message)
            *message = e.message();
        return (long long)e.offset();
    }
    return -1;
}

void put16(std::vector<uint8_t>& b, size_t off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

void put32(std::vector<uint8_t>& b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[off + i] = (uint8_t)(v >> (8 * i));
}

// ------------------------------------------------------------------ the tests

void test_numbers()
{
    expect_str(b3d_obj_number(0.0), "0.000000", "zero");
    expect_str(b3d_obj_number(-0.0), "0.000000", "negative zero");
    expect_str(b3d_obj_number(-1e-9), "0.000000", "a negative value that rounds to zero");
    expect_str(b3d_obj_number(-0.25), "-0.250000", "negative");
    expect_str(b3d_obj_number(1.0 / 128), "0.007812", "a tie rounds to even (down)");
    expect_str(b3d_obj_number(3.0 / 128), "0.023438", "a tie rounds to even (up)");
    expect_str(b3d_obj_number(-1.0 / 128), "-0.007812", "a negative tie");
    expect_str(b3d_obj_number((double)0.2f), "0.200000", "0.2f");
    expect_str(b3d_obj_number((double)0.05f * (double)0.2f), "0.010000", "a product of floats");
    expect_str(b3d_obj_number(123456.0), "123456.000000", "large");
}

void test_uvs()
{
    B3dMapping m;
    for (int c = 0; c < 4; c++) {
        m.uv[c][0] = (float)c;
        m.uv[c][1] = (float)(10 + c);
    }
    float out[4][2];
    // each UV_PERM row is a permutation, and quads take uv[perm[k]]
    for (int row = 0; row < 8; row++) {
        check(b3d_poly_uvs(m, (uint8_t)row, 4, out), "quad UVs");
        int seen = 0;
        for (int k = 0; k < 4; k++) {
            check(out[k][0] == (float)kB3dUvPerm[row][k] && out[k][1] == 10.0f + (float)kB3dUvPerm[row][k],
                  "quad corner from the permutation");
            seen |= 1 << kB3dUvPerm[row][k];
        }
        check(seen == 15, "UV_PERM row is a permutation");
    }
    // the spec's worked example: uv_mode 5 gives corners (c0, c3, c2, c1)
    check(b3d_poly_uvs(m, 5, 4, out) && out[0][0] == 0 && out[1][0] == 3 && out[2][0] == 2 && out[3][0] == 1,
          "uv_mode 5");
    // triangles: perm[tri[k]]
    for (int tri = 0; tri < 4; tri++)
        for (int row = 0; row < 8; row++) {
            const uint8_t mode = (uint8_t)(0x80 | (tri << 4) | row);
            check(b3d_poly_uvs(m, mode, 3, out), "triangle UVs");
            for (int k = 0; k < 3; k++)
                check(out[k][0] == (float)kB3dUvPerm[row][kB3dUvTri[tri][k]], "triangle corner from the subset");
        }
    // a triangle without the flag takes the quad rule; a quad with it has no 4th UV
    check(b3d_poly_uvs(m, 0x02, 3, out) && out[2][0] == (float)kB3dUvPerm[2][2], "triangle with the quad rule");
    check(!b3d_poly_uvs(m, 0x80, 4, out), "a quad with the triangle flag is refused");
}

void test_level()
{
    const Level L = build_level();
    B3dGeometry g;
    try {
        g = b3d_decode_geometry(L.image.data(), L.image.size(), "unit.b3d");
    } catch (const ParseError& e) {
        check(false, std::string("decode: ") + e.what());
        return;
    }
    check(g.version == kB3dVersion && g.grid_w == 3 && g.grid_h == 1, "header fields");
    check(g.vertices.size() == 9 && g.mappings.size() == 2 && g.textures.size() == 2, "section counts");
    check(g.textures[1].offset == L.tex + kB3dSectionHeaderSize + 20, "texture 1 offset");
    check(g.polys.size() == 4 && g.links.size() == 1 && g.blocks.size() == 3, "polygons, links, blocks");
    check(g.amap_read && g.amap.size() == 1 && g.amap[0].nodes.empty(), "one moving object");
    check(g.warnings.empty(), "no warnings");
    for (int i = 0; i < 4; i++)
        check(g.polys[(size_t)i].offset == L.poly[i], "polygon offset " + std::to_string(i));
    check(g.links[0].offset == L.link && g.links[0].count == 1 && g.links[0].rec[0].id == 7, "LINK entry");
    check(g.blocks[2].cell == 2 && g.blocks[2].first_poly == 2 && g.blocks[2].num_polys == 2, "block of cell 2");

    // orientation: polygon 0 is clockwise seen from its normal, 1 counter-clockwise; 3 has no area
    check(!b3d_poly_keeps_order(g, g.polys[0]), "polygon 0 is reversed");
    check(b3d_poly_keeps_order(g, g.polys[1]), "polygon 1 keeps its order");
    check(!b3d_poly_keeps_order(g, g.polys[3]), "the zero-area polygon is reversed");
    double n[3], N[3];
    b3d_newell(g, g.polys[0], N);
    check(N[0] == 0 && N[1] == 0 && N[2] == -2, "Newell normal of polygon 0 points away from +Z");
    b3d_poly_normal(g, g.polys[3], n);
    check(n[0] == 0 && n[1] == 0 && n[2] == -1, "non-finite normal, no area: the facing class axis");
    B3dPoly p = g.polys[0];
    p.normal[0] = p.normal[1] = p.normal[2] = 0.0f;
    b3d_poly_normal(g, p, n);
    check(n[0] == 0 && n[1] == 0 && n[2] == 1, "zero normal: the reversed Newell normal");
    p = g.polys[3];
    p.facing = 0;
    b3d_poly_normal(g, p, n);
    check(n[0] == 0 && n[1] == 0 && n[2] == 1, "no normal, no area, no facing class: +Z");

    // texture rules: polygon 1 binds page 0 (its own) in the game rule, page 1 (its mapping's) in the mat rule
    check(b3d_front_texture(g, g.polys[1], GeometryTextureRule::Game) == 0, "game rule");
    check(b3d_front_texture(g, g.polys[1], GeometryTextureRule::Mat) == 1, "mat rule");
    p = g.polys[1];
    p.blend = 16;
    check(b3d_front_texture(g, p, GeometryTextureRule::Game) == 1, "a blended polygon binds its mapping's page");
    p.mat_front = 2;
    check(b3d_front_texture(g, p, GeometryTextureRule::Game) == -1, "an out-of-range mapping");
    p = g.polys[0];
    p.tex_bucket = 2;
    check(b3d_front_texture(g, p, GeometryTextureRule::Game) == -1, "an out-of-range texture");

    const B3dLinkCheck lc = b3d_check_links(g);
    check(lc.records == 1 && lc.unresolved == 0 && lc.shared_ids == 0 && lc.issues.empty(), "the LINK resolves");

    std::string obj, mtl;
    B3dObjStats st;
    b3d_obj_text(g, "UNIT", 0.5f, GeometryTextureRule::Game, obj, mtl, &st);
    check(st.faces == 4 && st.triangles == 2 && st.quads == 2 && st.kept_order == 1 && st.untextured == 0 &&
              st.rule_differs == 2 && st.moving == 1 && st.hidden == 1 && st.moving_objects == 1 && st.texcoords == 14,
          "OBJ statistics");
    const char* expect_obj =
        "# TPM level geometry: UNIT (scale 0.500000; Z up; +X right, +Y forward)\n"
        "mtllib UNIT.mtl\n"
        "v 0.000000 0.000000 0.000000\nv 0.000000 0.500000 0.000000\nv 0.500000 0.500000 0.000000\n"
        "v 0.500000 0.000000 0.000000\nv -0.500000 0.250000 0.000000\nv 1.000000 0.000000 0.125000\n"
        "v 1.000000 0.500000 0.125000\nv 1.750000 0.500000 0.125000\nv 1.750000 0.000000 0.125000\n"
        // static: polygon 0 reversed (0,3,2,1), polygon 1 kept; uv_mode 0x95: TRI 1 (0,1,2), PERM 5 (0,3,2,1)
        "vt 0.000000 1.000000\nvt 0.000000 0.000000\nvt 1.000000 0.000000\nvt 1.000000 1.000000\n"
        "vt 0.250000 0.500000\nvt 0.250000 0.000000\nvt 0.750000 0.000000\n"
        // amap0: polygon 2 reversed; hidden: polygon 3 reversed (0,2,1), uv_mode 0x81: TRI 0, PERM 1
        "vt 0.250000 0.500000\nvt 0.250000 0.000000\nvt 0.750000 0.000000\nvt 0.750000 0.500000\n"
        "vt 1.000000 1.000000\nvt 1.000000 0.000000\nvt 0.000000 1.000000\n"
        "vn 0.000000 0.000000 1.000000\nvn 0.000000 0.000000 1.000000\nvn 0.000000 0.000000 1.000000\n"
        "vn 0.000000 0.000000 -1.000000\n"
        "o static\ng static\nusemtl tex0\nf 1/1/1 4/2/1 3/3/1 2/4/1\nf 1/5/2 4/6/2 5/7/2\n"
        "o amap0\ng amap0\nusemtl tex0\nf 6/8/3 9/9/3 8/10/3 7/11/3\n"
        "o hidden\ng hidden\nusemtl tex0\nf 6/12/4 7/13/4 6/14/4\n";
    expect_str(obj, expect_obj, "OBJ text");
    const std::string block = "Ka 1.000000 1.000000 1.000000\nKd 1.000000 1.000000 1.000000\n"
                              "Ks 0.000000 0.000000 0.000000\nd 1.000000\nillum 1\n";
    expect_str(mtl, "# TPM level materials: UNIT\n\nnewmtl tex0\n" + block + "map_Kd 0.bmp\n\nnewmtl tex1\n" + block +
                        "map_Kd 1.bmp\n",
               "MTL text");

    // the mat rule moves polygons 1 and 2 to page 1
    b3d_obj_text(g, "UNIT", 0.5f, GeometryTextureRule::Mat, obj, mtl, &st);
    check(obj.find("usemtl tex0\nf 1/1/1 4/2/1 3/3/1 2/4/1\nusemtl tex1\nf 1/5/2") != std::string::npos &&
              obj.find("o amap0\ng amap0\nusemtl tex1\n") != std::string::npos && st.rule_differs == 2,
          "OBJ text, mat rule");

    // an untextured face: no vt, "f v//vn", and the "untextured" material
    B3dGeometry u = g;
    u.polys[1].mat_front = -1;
    b3d_obj_text(u, "UNIT", 0.5f, GeometryTextureRule::Game, obj, mtl, &st);
    check(st.untextured == 1 && st.first_untextured == 1 && st.texcoords == 11, "untextured statistics");
    check(obj.find("\nusemtl untextured\nf 1//2 4//2 5//2\n") != std::string::npos, "untextured face");
    check(mtl.find("\nnewmtl untextured\nKa 1.000000 1.000000 1.000000\nKd 0.500000 0.500000 0.500000\n") !=
              std::string::npos,
          "untextured material");
    // a LINK record without target, and a link id used twice
    u = g;
    u.links[0].rec[0].id = 8;
    u.polys[0].render = kB3dRenderLinkTarget;
    u.polys[0].link_id = 7;
    const B3dLinkCheck bad = b3d_check_links(u);
    check(bad.unresolved == 1 && bad.shared_ids == 1 && bad.issues.size() == 2, "LINK problems");
}

void test_damaged()
{
    const Level L = build_level();
    std::string msg;
    struct Case
    {
        const char* what;
        size_t offset;      // where the patch goes (bytes)
        int width;          // 1, 2 or 4
        uint32_t value;
        long long expect;   // the error offset
    };
    const size_t h = kB3dSectionHeaderSize;
    const Case cases[] = {
        {"5 vertices", L.poly[0] + 0x25, 1, 5, (long long)(L.poly[0] + 0x25)},
        {"vertex index out of range", L.poly[1] + 4, 2, 9, (long long)(L.poly[1] + 4)},
        {"a block with 0 entries", L.gmap + h, 1, 0, (long long)(L.gmap + h)},
        {"a LINK entry with 6 records", L.link + 2, 1, 6, (long long)(L.link + 2)},
        {"header entry count", h + kB3dHdrGmapEntries, 4, 6, (long long)(h + kB3dHdrGmapEntries)},
        {"header block count", h + kB3dHdrGmapBlocks, 4, 2, (long long)(h + kB3dHdrGmapBlocks)},
        {"header vertex count", h + kB3dHdrNumVertices, 4, 10, (long long)L.gvrt},
        {"texture over 65,536 texels", L.tex + h, 4, 65536, (long long)(L.tex + h)},
        {"texture pixels past the section", L.tex + h, 4, 300, (long long)(L.tex + h)},
        {"version 294", h + kB3dHdrVersion, 4, 294, (long long)h},
        {"version 277", h + kB3dHdrVersion, 4, 277, (long long)h},
        {"GVRT record size", h + kB3dHdrVertexSize, 4, 16, (long long)(h + kB3dHdrVertexSize)},
        {"a non-finite vertex", L.gvrt + h + 12 * 4 + 4, 4, 0x7FC00000, (long long)(L.gvrt + h + 12 * 4)},
        {"the moving object's size", L.end1 - 0x84 - 4, 4, 0x80, (long long)(L.end1 - 0x84)},
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> img = L.image;
        if (c.width == 1)
            img[c.offset] = (uint8_t)c.value;
        else if (c.width == 2)
            put16(img, c.offset, (uint16_t)c.value);
        else
            put32(img, c.offset, c.value);
        const long long got = error_offset(img, &msg);
        check(got == c.expect, std::string(c.what) + ": error at " + std::to_string(got) + ", expected " +
                                   std::to_string(c.expect) + " (" + msg + ")");
    }
    // cut before B3D_END1: the chain is broken where the cut section starts
    std::vector<uint8_t> cut(L.image.begin(), L.image.begin() + (std::ptrdiff_t)(L.tex + 20));
    check(error_offset(cut) == (long long)L.tex, "cut inside B3D_TEX");
    // after B3D_END1 nothing is read: a damaged editor part does not matter
    std::vector<uint8_t> tail = L.image;
    tail.resize(tail.size() - 5);
    check(error_offset(tail) == -1, "damage after B3D_END1 is ignored");
    // an older header version from 278 on is a warning
    std::vector<uint8_t> older = L.image;
    put32(older, h + kB3dHdrVersion, 280);
    try {
        const B3dGeometry g = b3d_decode_geometry(older.data(), older.size(), "unit.b3d");
        check(g.warnings.size() == 1 && g.warnings[0].offset == h, "version 280: one warning");
    } catch (const ParseError& e) {
        check(false, std::string("version 280: ") + e.what());
    }
    check(error_offset(std::vector<uint8_t>(10, 0)) == 0, "not a level");
    check(error_offset(std::vector<uint8_t>()) == 0, "empty");
}

} // namespace

int main()
{
    test_numbers();
    test_uvs();
    test_level();
    test_damaged();
    std::printf("unit_b3d_geom: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
