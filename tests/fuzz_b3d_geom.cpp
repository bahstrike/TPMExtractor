// TPMExtractor fork - fuzz target: the level geometry decoder and the OBJ/MTL text, in memory.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Built only with -DTPMX_FUZZ=ON (see tests/CMakeLists.txt). The input is a level image, or a
// compressed level file (decompressed first, as process_b3d does). Whatever the input, decoding
// either throws ParseError or yields geometry whose OBJ text is consistent: one face per polygon
// with 3 or 4 corners, every v/vt/vn index in range, one vn per face, and as many vt lines as the
// corners of the textured faces; both texture rules; the LINK check. The input's first bytes also
// serve as a level name: b3d_obj_name must make an ASCII file name of it, and the OBJ text must
// name that MTL file and stay ASCII.

#include "b3d.h"
#include "b3d_decompress.h"
#include "b3d_geom.h"
#include "binreader.h"
#include "log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const size_t kMaxInput = 4u << 20;

[[noreturn]] void broken(const char* what)
{
    std::fprintf(stderr, "fuzz_b3d_geom: %s\n", what);
    std::abort();
}

// Checks every line of an OBJ text against the counts it must respect.
void check_obj(const std::string& obj, const tpmx::B3dGeometry& g, const tpmx::B3dObjStats& st)
{
    uint64_t v = 0, vt = 0, vn = 0, f = 0;
    size_t pos = 0;
    while (pos < obj.size()) {
        size_t end = obj.find('\n', pos);
        if (end == std::string::npos)
            broken("the OBJ text does not end with a line end");
        const std::string line = obj.substr(pos, end - pos);
        pos = end + 1;
        if (line.compare(0, 2, "v ") == 0)
            v++;
        else if (line.compare(0, 3, "vt ") == 0)
            vt++;
        else if (line.compare(0, 3, "vn ") == 0)
            vn++;
        else if (line.compare(0, 2, "f ") == 0) {
            f++;
            int corners = 0;
            const char* p = line.c_str() + 1;
            while (*p == ' ') {
                char* q = nullptr;
                const unsigned long long a = std::strtoull(p + 1, &q, 10);
                if (*q != '/')
                    broken("a face corner without '/'");
                unsigned long long b = 0;
                if (q[1] != '/')
                    b = std::strtoull(q + 1, &q, 10);
                else
                    q++;
                if (*q != '/')
                    broken("a face corner without a normal");
                const unsigned long long c = std::strtoull(q + 1, &q, 10);
                if (a < 1 || a > g.vertices.size() || b > st.texcoords || c != f)
                    broken("a face index out of range");
                p = q;
                corners++;
            }
            if (*p != '\0' || corners < 3 || corners > 4)
                broken("a face line with a bad corner list");
        }
    }
    if (v != g.vertices.size() || vt != st.texcoords || vn != g.polys.size() || f != g.polys.size() ||
        st.faces != g.polys.size())
        broken("OBJ line counts do not match the geometry");
}

bool ascii(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](char c) { return (unsigned char)c < 0x80; });
}

// The input's first bytes as a level name. Its OBJ/MTL file name is non-empty, holds only
// [A-Za-z0-9_.-], is no longer than the name (or "level"), and stays the same on a second pass.
std::string level_name_of(const uint8_t* data, size_t size)
{
    const std::string given(reinterpret_cast<const char*>(data), std::min<size_t>(size, 64));
    const std::string stem = tpmx::b3d_obj_name(given);
    if (stem.empty() || stem.size() > std::max<size_t>(given.size(), 5) || tpmx::b3d_obj_name(stem) != stem ||
        stem.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") !=
            std::string::npos)
        broken("b3d_obj_name made a bad file name");
    return given;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool silent = (tpmx::log::set_verbosity(-2), true);
    (void)silent;
    if (size > kMaxInput)
        return 0;
    const std::string level_name = level_name_of(data, size);
    std::vector<uint8_t> image(data, data + size);
    if (size >= tpmx::kB3dRawHeaderSize) {
        uint32_t flag;
        std::memcpy(&flag, data + tpmx::kB3dCompressedFlagOffset, 4);
        if (flag != 0) {
            std::vector<uint8_t> out;
            tpmx::LzssResult r;
            std::string err;
            if (!tpmx::b3d_decompress(data, size, "fuzz.b3d", out, false, &r, &err))
                return 0;
            image.swap(out);
        }
    }
    tpmx::B3dGeometry g;
    try {
        g = tpmx::b3d_decode_geometry(image.data(), image.size(), "fuzz.b3d");
    } catch (const tpmx::ParseError& e) {
        if (e.offset() > image.size())
            broken("an error offset beyond the image");
        return 0;
    }
    for (const tpmx::B3dPoly& p : g.polys) {
        if (p.nverts != 3 && p.nverts != 4)
            broken("a polygon with a bad vertex count");
        for (int k = 0; k < p.nverts; k++)
            if (p.v[k] >= g.vertices.size())
                broken("a vertex index out of range");
        if (p.offset + tpmx::kB3dGmapEntrySize > image.size())
            broken("a polygon offset beyond the image");
    }
    const tpmx::B3dLinkCheck lc = tpmx::b3d_check_links(g);
    if (lc.unresolved > lc.records)
        broken("more unresolved LINK records than records");
    for (int rule = 0; rule < 2; rule++) {
        std::string obj, mtl;
        tpmx::B3dObjStats st;
        tpmx::b3d_obj_text(g, rule ? std::string("LVL") : level_name, rule ? 1.0f : 0.2f,
                           rule ? tpmx::GeometryTextureRule::Mat : tpmx::GeometryTextureRule::Game, obj, mtl, &st);
        check_obj(obj, g, st);
        const std::string mtllib = "\nmtllib " + tpmx::b3d_obj_name(rule ? std::string("LVL") : level_name) + ".mtl\n";
        const size_t first_end = obj.find('\n');
        if (first_end == std::string::npos || obj.compare(first_end, mtllib.size(), mtllib) != 0)
            broken("the OBJ's second line does not name the MTL file");
        if (!ascii(obj) || !ascii(mtl))
            broken("the OBJ/MTL text is not ASCII");
        if (st.untextured != 0 && mtl.find("\nnewmtl untextured\n") == std::string::npos)
            broken("untextured faces without the untextured material");
    }
    return 0;
}
