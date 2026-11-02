// TPMExtractor fork - JK-side writers: retail 8-bit MATs, CMP colormaps, nearest colour.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Callers: baf.cpp (and possibly b3d.cpp). The declarations marked "contract" are the API the
// other modules were written against; later additions are marked "added".
//
// Target: unpatched retail JK 1.0/1.01. The layouts follow the retail code paths that OpenJKDF2
// decompiled, identical in upstream OpenJKDF2 and the MotS-Fixes fork:
//   MAT  src/Engine/rdMaterial.c rdMaterial_LoadEntry_Common: magic "MAT " and revision 0x32 are
//        the only checks; colour mode and bpp are copied from the header unchecked (no
//        Main_bMotsCompat/JKM guard anywhere in the MAT or CMP load path); a 768-byte palette
//        follows when type & 1. Structs: src/types.h rdMaterialHeader, rdTexFormat,
//        rdTexinfoHeader, rdTexinfoExtHeader, rdTextureHeader.
//   CMP  src/Engine/rdColormap.c rdColormap_LoadEntry (the "// JKDF2" block is the retail read:
//        64-byte header, palette, 64x256 light table unless flags & 4, 256x256 transparency table
//        when flags & 1); rdColormap_Write emits version 30. Struct: src/types.h rdColormapHeader.
// Why 8-bit: every table the retail renderers draw through is indexed by an 8-bit palette index
// (light[level*256 + index], transparency[src*256 + dst]; src/Raster/rdCache.c
// rdCache_SendFaceListToHardware looks up light[256*(level & 0x3F) + index] with level =
// light*63). Retail's texel paths themselves (rdCache_DrawFaceN/Z, std3D_AddToTextureCache) are
// not decompiled, so whether retail draws a 16-bit MAT is unproven; 8-bit + CMP is the only
// retail-safe output.
//
// Index 0 is the transparent key of masked textures (texture header alpha_en bit 0, key colour
// unk_10 = 0). remap_table, generate_mips and representative_index never map a colour to index 0,
// and build_cmp's tables never name it unless key_index0 is cleared (nearest_index is the raw
// search and returns whatever `allowed` permits): OpenJKDF2's GL shader discards every index-0
// texel whenever blending is on (resource/shaders/default_f.glsl), so even an opaque texel at 0
// becomes a hole on a translucent face, and LucasArts' DFLT.CMP on the TPM disc never emits 0
// from its light or transparency tables.

#pragma once

#include "image.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {
namespace jk {

using IndexSet = std::bitset<256>;  // contract

// ------------------------------------------------------------- format constants (added)

constexpr uint32_t kMatRevision = 0x32;          // '2'; any other value fails the load
constexpr uint32_t kMatTypeColor = 0;            // solid-colour MAT, no textures
constexpr uint32_t kMatTypeTexture = 2;          // textured MAT (bit 0 would append a palette)
constexpr uint32_t kMatTexinfoTextured = 8;      // texinfo texture_type: an ext header follows
constexpr uint32_t kMatTexinfoUnused = 0x3F800000;  // texinfo field_8..14: float 1.0 in real files
constexpr size_t kMatHeaderSize = 76;            // magic, revision, type, 2 counts, rdTexFormat
constexpr size_t kMatTexinfoSize = 24;
constexpr size_t kMatTexinfoExtSize = 16;
constexpr size_t kMatTextureHeaderSize = 24;
constexpr int kMat8MaxCels = 16;                 // retail stack array textures_idk[16]
constexpr int kMat8MaxTextures = 16;             // a material never needs more textures than cels
constexpr int kMat8MaxMips = 4;                  // rdTexture.texture_struct[4]
constexpr int kMat8MaxDimension = 256;           // std3D_GetValidDimensions(..., 256, 256)

constexpr int32_t kCmpVersionRetail = 30;        // rdColormap_Write; DFLT.CMP on the TPM disc
constexpr int32_t kCmpVersionLegacy = 20;        // what the original tool wrote
constexpr uint32_t kCmpFlagTransparency = 1;     // a 256x256 transparency table follows
constexpr uint32_t kCmpFlagRgb16 = 4;            // RGB16 variant: never written, refused on read
constexpr int kCmpLightLevels = 64;
constexpr int kCmpFullLight = 63;                // row 63 = full light = identity
constexpr size_t kCmpHeaderSize = 64;
constexpr size_t kCmpLightTableSize = 64 * 256;
constexpr size_t kCmpTransparencyTableSize = 256 * 256;
constexpr size_t kCmpSizeNoTransparency = 17216;     // header + palette + light table
constexpr size_t kCmpSizeWithTransparency = 82752;   // + transparency table

// Source weight of the transparency blend, in 1/256: trans[s][d] = nearest((a*s + (256-a)*d)/256).
// 90/256 (about 0.35) reproduces LucasArts' DFLT.CMP on the TPM disc best: build_cmp with its
// default options matches 72.8% of that file's 65,536 entries (63.5% with the Euclidean metric),
// a 0.5 blend only 33.3% (31.6%). The engine checks none of this; it is a look, not a format rule.
constexpr int kTransparencyAlphaDefault = 90;

// ------------------------------------------------------------- nearest colour

// Colour distance used by the nearest-colour searches (added).
//   Euclidean: dR^2 + dG^2 + dB^2. The contract functions below use it unless told otherwise.
//   Redmean:   the "redmean" perceptual approximation, scaled to integers:
//              (1024 + rs)dR^2 + 2048dG^2 + (1534 - rs)dB^2 with rs = R1 + R2. build_cmp uses it
//              by default: it reproduces LucasArts' DFLT.CMP on the TPM disc more closely (light
//              76.1% vs 74.2% of the non-fullbright entries, transparency 72.8% vs 63.5%).
// Ties always go to the lowest index, so results never depend on search order.
enum class ColorMetric { Euclidean, Redmean };

// contract: index of the entry of `pal` closest to `c` among the indices in `allowed`
// (Euclidean; ties go to the lowest index). Returns 0 if `allowed` is empty.
uint8_t nearest_index(const Palette& pal, Rgb c, const IndexSet& allowed);
uint8_t nearest_index(const Palette& pal, Rgb c, const IndexSet& allowed, ColorMetric metric);  // added

// contract: table mapping every index of `src` to the nearest allowed index of `dst`
// (Euclidean). Index 0 is never chosen for any source index but 0: with keep_index0, source 0
// (the transparent key of a masked texture) maps to 0; without it, source 0 is an ordinary
// opaque colour and maps to its nearest allowed non-zero index. If `allowed` holds no index
// but 0, every entry maps to 0.
std::array<uint8_t, 256> remap_table(const Palette& src, const Palette& dst, const IndexSet& allowed,
                                     bool keep_index0);
std::array<uint8_t, 256> remap_table(const Palette& src, const Palette& dst, const IndexSet& allowed,
                                     bool keep_index0, ColorMetric metric);  // added

// ------------------------------------------------------------- 8-bit MAT (contract)

struct Mat8Texture
{
    int width = 0;   // mip 0; power of two, 1..256
    int height = 0;  // mip 0; power of two, 1..256
    bool masked = false;  // texture header alpha_en bit 0: palette index 0 is transparent
    // mips[0] holds width*height palette indices, mips[k] (width>>k)*(height>>k); 1..4 levels.
    std::vector<std::vector<uint8_t>> mips;
};

struct Mat8Cel
{
    int texture = 0;          // index into Mat8::textures (ignored for a solid-colour MAT)
    uint8_t solid_color = 0;  // texinfo solidColor (used by solid geometry mode)
};

struct Mat8
{
    // Empty with cels present: a solid-colour MAT (type 0; each cel draws its solid_color).
    std::vector<Mat8Texture> textures;
    // One entry per cel. Empty means one cel per texture, cel i -> texture i, solid_color 0.
    std::vector<Mat8Cel> cels;
};

// contract: brings t.mips to min(levels, the most levels the size allows) entries. Levels
// already present are kept, levels beyond `levels` are dropped, missing ones are built from the
// previous level: each 2x2 block is averaged in RGB through `pal` (rounded half up) and matched
// with nearest_index over `allowed` minus index 0. In a masked texture a block with three or
// four holes (index 0) stays a hole, otherwise its opaque texels are averaged; index 0 is never
// produced for an opaque texel. Fails (false, *err) on levels outside 1..4, a size that is not a
// power of two in 1..256, a missing or wrongly sized level, or no allowed index but 0.
bool generate_mips(Mat8Texture& t, int levels, const Palette& pal, const IndexSet& allowed, std::string* err);
bool generate_mips(Mat8Texture& t, int levels, const Palette& pal, const IndexSet& allowed, ColorMetric metric,
                   std::string* err);  // added

// added: a solidColor for a textured cel: the allowed index nearest the mean colour of mip 0's
// opaque texels (index-0 holes of a masked texture are skipped; index 0 is never returned
// unless nothing else is allowed). Returns 0 for a texture without opaque texels or mips.
uint8_t representative_index(const Mat8Texture& t, const Palette& pal, const IndexSet& allowed,
                             ColorMetric metric = ColorMetric::Euclidean);

// contract: encodes a retail MAT:
//   header   "MAT ", 0x32, type (2 textured / 0 solid colour), cel count, texture count,
//            rdTexFormat {colour mode 0, bpp 8, 12 zeros}
//   per cel  texinfo {texture_type 8, solid_color, 0x3F800000 x4} + ext {0, 0, 0, texture index}
//            (solid-colour MAT: texinfo {0, solid_color, 0x3F800000 x4}, no ext)
//   per texture {width, height, alpha_en (masked), 0, 0 (key colour index), mip count}, then the
//            mips, largest first, rows top-down, tightly packed.
// Validates: 1..16 cels, 0..16 textures (0 only with cels: solid colour), every cel's texture
// index in range, power-of-two sizes 1..256, 1..4 mips whose sizes match and stay >= 1x1.
bool encode_mat8(const Mat8& mat, std::vector<uint8_t>& out, std::string* err);
bool write_mat8(const std::string& path, const Mat8& mat, std::string* err);

// ------------------------------------------------------------- CMP

struct Cmp  // contract
{
    int32_t version = 30;
    uint32_t flags = 1;                     // bit 0: transparency table present
    float tint[3] = {1.0f, 1.0f, 1.0f};
    Palette palette;
    std::vector<uint8_t> light;             // 64*256 bytes: light[level*256 + index]
    std::vector<uint8_t> transparency;      // 256*256 bytes when flags & 1: [src*256 + dst]
};

struct CmpBuildOptions
{
    // contract
    bool transparency_table = true;  // write flags 1 and a 256x256 table
    IndexSet fullbright;             // light columns stay identity for these indices (DFLT.CMP: 0..31, 63)
    IndexSet reserved;               // never chosen as a light or transparency result for another index
    // added (defaults follow LucasArts' DFLT.CMP on the TPM disc)
    bool key_index0 = true;          // index 0: identity light column, never a table result
    int transparency_alpha = kTransparencyAlphaDefault;  // source weight in 1/256, 0..256
    bool monotonic_light = true;     // a column's brightness never rises as the light level falls
    ColorMetric metric = ColorMetric::Redmean;
};

// contract: the colormap the original tool writes (CCMP::saveColormap): "CMP ", version 20,
// flags 0, tint 0,0,0, zero reserved bytes, `pal`, an all-zero 64x256 light table, no
// transparency table: 17,216 bytes when encoded. This is what --legacy writes.
Cmp legacy_cmp(const Palette& pal);

// contract: a retail-ready colormap: version 30, flags 1 (0 without the transparency table),
// tint 1.0, reserved bytes zero, `pal`, then
//   light   row 63 is the identity; columns of fullbright indices (and of 0 with key_index0) are
//           the identity in every row; every other entry is row[L][i] = nearest(round(pal[i] *
//           L / 63)). Fullbright indices may be results (DFLT.CMP darkens to a fullbright
//           black). With monotonic_light, an entry brighter (integer Rec.601 luma) than the row
//           above it is replaced by that row's entry, so no column brightens as L falls.
//   trans   trans[s*256 + d] = nearest(round((a*pal[s] + (256-a)*pal[d]) / 256)), a =
//           transparency_alpha; trans[i*256 + i] = i for every index that may be a result.
// "nearest" searches every index except `reserved` (and 0 with key_index0), with `metric`.
// 82,752 bytes encoded (17,216 without the transparency table). Fails on transparency_alpha
// outside 0..256 or when no index may be a result.
bool build_cmp(const Palette& pal, const CmpBuildOptions& opts, Cmp& out, std::string* err);

// contract: serialises a 64-byte header ("CMP ", version, flags, tint[3] as floats, 40 zero
// bytes), the palette, the light table, then the transparency table if flags & 1. Fails when
// the light table is not 16,384 bytes, the transparency table does not match flags bit 0
// (65,536 bytes when set, empty when clear), or flags has bit 2 (the RGB16 variant, read
// differently by retail JK and by MotS/OpenJKDF2). Other flag bits are written as given.
bool encode_cmp(const Cmp& cmp, std::vector<uint8_t>& out, std::string* err);
bool write_cmp(const std::string& path, const Cmp& cmp, std::string* err);

// contract: parses a CMP the way retail JK reads it: header, palette, light table, and the
// transparency table when flags & 1. The version is not checked (as in the engine) and bytes
// after the tables are ignored. Fails, with "<name> @0x<offset>: ..." in *err, on a bad magic,
// a short file, or flags bit 2. `name` is used in error messages (see binreader.h).
bool parse_cmp(const uint8_t* data, size_t size, const std::string& name, Cmp& out, std::string* err);
bool read_cmp(const std::string& path, Cmp& out, std::string* err);

// added: indices whose light column is the identity at all 64 levels ("self-illuminated":
// they never darken in the software renderer). Empty if cmp.light is not 16,384 bytes.
IndexSet fullbright_indices(const Cmp& cmp);

// added: the fullbright indices that visibly glow: fullbright, not 0, brightest channel above
// `min_channel`. Exclude these from `allowed` when remapping ordinary texels into an existing
// CMP's palette, so they do not glow in the dark. (Not every fullbright index: DFLT.CMP's only
// non-zero black is fullbright and is its light table's darkest target.)
IndexSet glow_indices(const Cmp& cmp, int min_channel = 8);

// added: number of light columns whose brightness (integer Rec.601 luma of the result) drops
// somewhere as the level rises from 0 to 63. 0 for every table build_cmp writes with
// monotonic_light; DFLT.CMP on the TPM disc has 19. -1 if cmp.light is not 16,384 bytes.
int light_monotonic_violations(const Cmp& cmp);

} // namespace jk
} // namespace tpmx
