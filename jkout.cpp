// TPMExtractor fork - JK-side writers: retail 8-bit MATs, CMP colormaps, nearest colour.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// See jkout.h for the formats, the OpenJKDF2 functions they follow and the contract.

#include "jkout.h"

#include "binreader.h"
#include "platform.h"
#include "textout.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace tpmx {
namespace jk {

namespace {

static_assert(sizeof(Palette) == 768, "a palette is 768 packed bytes");
static_assert(kCmpHeaderSize + sizeof(Palette) + kCmpLightTableSize == kCmpSizeNoTransparency, "CMP size");
static_assert(kCmpSizeNoTransparency + kCmpTransparencyTableSize == kCmpSizeWithTransparency, "CMP size");
static_assert(kMatHeaderSize == 4 + 4 * 4 + 14 * 4, "MAT header: magic, 4 words, rdTexFormat");

const size_t kCmpReservedBytes = kCmpHeaderSize - 24;  // after magic, version, flags, tint[3]

// ------------------------------------------------------------- little-endian output

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back((uint8_t)(v & 0xFFu));
    out.push_back((uint8_t)((v >> 8) & 0xFFu));
    out.push_back((uint8_t)((v >> 16) & 0xFFu));
    out.push_back((uint8_t)((v >> 24) & 0xFFu));
}

void put_f32(std::vector<uint8_t>& out, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    put_u32(out, bits);
}

void put_bytes(std::vector<uint8_t>& out, const void* data, size_t n)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

bool fail(std::string* err, const std::string& message)
{
    if (err)
        *err = message;
    return false;
}

bool is_pow2_dimension(int v) { return v >= 1 && v <= kMat8MaxDimension && (v & (v - 1)) == 0; }

// Most mip levels a width x height texture has before a side drops below 1.
int max_mip_levels(int width, int height)
{
    int levels = 1;
    while (levels < kMat8MaxMips && (width >> levels) >= 1 && (height >> levels) >= 1)
        levels++;
    return levels;
}

// Integer Rec.601 luma times 1000: the brightness order of the light-table monotonic rule.
int luma(const Rgb& c) { return 299 * c.r + 587 * c.g + 114 * c.b; }

// (2*sum + n) / (2n): the mean rounded half up (sum >= 0, n >= 1).
uint8_t mean_round(uint64_t sum, uint64_t n) { return (uint8_t)((2 * sum + n) / (2 * n)); }

// ------------------------------------------------------------- nearest colour

// Both metrics in 32-bit integers: the largest Redmean distance is 5116 * 255^2 = 332,667,900.
inline int32_t euclidean(int r1, int g1, int b1, int r2, int g2, int b2)
{
    const int32_t dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr * dr + dg * dg + db * db;
}
inline int32_t redmean(int r1, int g1, int b1, int r2, int g2, int b2)
{
    const int32_t rs = r1 + r2;  // twice the mean red
    const int32_t dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return (1024 + rs) * dr * dr + 2048 * dg * dg + (1534 - rs) * db * db;
}
int32_t distance(ColorMetric metric, const Rgb& a, int r, int g, int b)
{
    return metric == ColorMetric::Euclidean ? euclidean(a.r, a.g, a.b, r, g, b) : redmean(a.r, a.g, a.b, r, g, b);
}

// Distance from v to the integer range [lo, hi] along one axis: nearest and farthest.
inline int32_t axis_near(int v, int lo, int hi) { return v < lo ? lo - v : (v > hi ? v - hi : 0); }
inline int32_t axis_far(int v, int lo, int hi) { return std::max(v - lo, hi - v); }

// Exact nearest-colour search over a fixed candidate set, for bulk use. The RGB cube is cut into
// cells; the first query in a cell builds its candidate list: for every candidate a lower and an
// upper bound of its distance to any colour in the cell (exact per-axis extremes; for Redmean the
// red-dependent weights also take their extremes over the cell), then only candidates whose lower
// bound does not exceed the smallest upper bound are kept, since no other can be nearest or tie.
// Fine cells (8x8x8 colour levels) are built from their coarse parent's list (32x32x32 levels): a
// candidate pruned for a cell is pruned for every sub-cell, whose lower bounds can only rise and
// whose smallest upper bound can only fall. Queries scan the fine list in index order with the
// exact metric, so ties go to the lowest index and the result equals a plain scan.
class Matcher
{
public:
    Matcher(const Palette& pal, const IndexSet& candidates, ColorMetric metric)
        : redmean_(metric == ColorMetric::Redmean)
    {
        for (int i = 0; i < 256; i++)
            if (candidates.test((size_t)i))
                cand_.push_back(Cand{pal.c[i].r, pal.c[i].g, pal.c[i].b, i});
        for (size_t k = 0; k < cand_.size(); k++)
            all_.push_back((uint8_t)k);
        coarse_.resize(cell_count(kCoarseBits));
        fine_.resize(cell_count(kFineBits));
    }

    bool empty() const { return cand_.empty(); }

    uint8_t nearest(const Rgb& c)
    {
        if (cand_.empty())
            return 0;
        Cell& fine = fine_[cell_of(c, kFineBits)];
        if (fine.start < 0) {
            Cell& coarse = coarse_[cell_of(c, kCoarseBits)];
            if (coarse.start < 0)
                build(c, kCoarseBits, all_.data(), all_.size(), coarse_pool_, coarse);
            build(c, kFineBits, &coarse_pool_[(size_t)coarse.start], coarse.count, fine_pool_, fine);
        }
        const uint8_t* list = &fine_pool_[(size_t)fine.start];
        int32_t best = INT32_MAX;
        int32_t best_index = 0;
        for (uint32_t k = 0; k < fine.count; k++) {  // ascending index: a tie keeps the lower one
            const Cand& p = cand_[list[k]];
            const int32_t d =
                redmean_ ? redmean(p.r, p.g, p.b, c.r, c.g, c.b) : euclidean(p.r, p.g, p.b, c.r, c.g, c.b);
            if (d < best) {
                best = d;
                best_index = p.index;
            }
        }
        return (uint8_t)best_index;
    }

private:
    struct Cand
    {
        int32_t r, g, b, index;
    };
    struct Cell
    {
        int32_t start = -1;  // offset into its pool, -1 = not built yet
        uint32_t count = 0;
    };
    static constexpr int kCoarseBits = 3;  // 8 cells per channel
    static constexpr int kFineBits = 5;    // 32 cells per channel

    static size_t cell_count(int bits) { return (size_t)1 << (3 * bits); }
    static size_t cell_of(const Rgb& c, int bits)
    {
        const int shift = 8 - bits;
        return ((size_t)(c.r >> shift) << (2 * bits)) | ((size_t)(c.g >> shift) << bits) | (size_t)(c.b >> shift);
    }

    // Builds the list of the `bits`-level cell holding `c` from the candidate positions `src`.
    void build(const Rgb& c, int bits, const uint8_t* src, size_t n, std::vector<uint8_t>& pool, Cell& cell)
    {
        const int shift = 8 - bits;
        const int r0 = (c.r >> shift) << shift, g0 = (c.g >> shift) << shift, b0 = (c.b >> shift) << shift;
        const int r1 = r0 + (1 << shift) - 1, g1 = g0 + (1 << shift) - 1, b1 = b0 + (1 << shift) - 1;
        lower_.resize(n);
        int32_t bound = INT32_MAX;  // smallest upper bound over the candidates
        for (size_t k = 0; k < n; k++) {
            const Cand& p = cand_[src[k]];
            const int32_t nr = axis_near(p.r, r0, r1), ng = axis_near(p.g, g0, g1), nb = axis_near(p.b, b0, b1);
            const int32_t fr = axis_far(p.r, r0, r1), fg = axis_far(p.g, g0, g1), fb = axis_far(p.b, b0, b1);
            int32_t lo, hi;
            if (redmean_) {  // weights (1024 + rs) and (1534 - rs), rs = p.r + query red in [r0, r1]
                lo = (1024 + p.r + r0) * nr * nr + 2048 * ng * ng + (1534 - p.r - r1) * nb * nb;
                hi = (1024 + p.r + r1) * fr * fr + 2048 * fg * fg + (1534 - p.r - r0) * fb * fb;
            } else {
                lo = nr * nr + ng * ng + nb * nb;
                hi = fr * fr + fg * fg + fb * fb;
            }
            lower_[k] = lo;
            bound = std::min(bound, hi);
        }
        cell.start = (int32_t)pool.size();
        for (size_t k = 0; k < n; k++)  // src is ascending, so the list stays ascending
            if (lower_[k] <= bound)
                pool.push_back(src[k]);
        cell.count = (uint32_t)(pool.size() - (size_t)cell.start);
    }

    bool redmean_;
    std::vector<Cand> cand_;  // ascending palette index
    std::vector<uint8_t> all_;
    std::vector<Cell> coarse_, fine_;
    std::vector<uint8_t> coarse_pool_, fine_pool_;  // candidate positions per cell, ascending
    std::vector<int32_t> lower_;                    // scratch for build
};

// ------------------------------------------------------------- MAT validation

bool check_mat8(const Mat8& mat, std::string* err)
{
    const size_t textures = mat.textures.size();
    const size_t cels = mat.cels.empty() ? textures : mat.cels.size();
    if (cels == 0)
        return fail(err, "MAT has no cels and no textures");
    if (cels > (size_t)kMat8MaxCels)
        return fail(err, format("MAT has %zu cels, retail JK allows at most %d", cels, kMat8MaxCels));
    if (textures > (size_t)kMat8MaxTextures)
        return fail(err, format("MAT has %zu textures, at most %d", textures, kMat8MaxTextures));
    if (textures > 0) {
        for (size_t k = 0; k < mat.cels.size(); k++) {
            const int t = mat.cels[k].texture;
            if (t < 0 || (size_t)t >= textures)
                return fail(err, format("cel %zu: texture index %d outside 0..%zu", k, t, textures - 1));
        }
    }
    for (size_t k = 0; k < textures; k++) {
        const Mat8Texture& t = mat.textures[k];
        if (!is_pow2_dimension(t.width) || !is_pow2_dimension(t.height))
            return fail(err, format("texture %zu: size %dx%d is not a power of two in 1..%d on both sides", k,
                                    t.width, t.height, kMat8MaxDimension));
        const size_t mips = t.mips.size();
        if (mips < 1 || mips > (size_t)kMat8MaxMips)
            return fail(err, format("texture %zu: %zu mip levels, retail JK needs 1..%d", k, mips, kMat8MaxMips));
        if ((int)mips > max_mip_levels(t.width, t.height))
            return fail(err, format("texture %zu: %zu mip levels do not fit a %dx%d texture (each level must be "
                                    "at least 1x1)", k, mips, t.width, t.height));
        for (size_t m = 0; m < mips; m++) {
            const size_t expect = (size_t)(t.width >> m) * (size_t)(t.height >> m);
            if (t.mips[m].size() != expect)
                return fail(err, format("texture %zu: mip %zu holds %zu texels, a %dx%d level needs %zu", k, m,
                                        t.mips[m].size(), t.width >> m, t.height >> m, expect));
        }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------- nearest colour

uint8_t nearest_index(const Palette& pal, Rgb c, const IndexSet& allowed, ColorMetric metric)
{
    int32_t best = INT32_MAX;
    int best_index = -1;
    for (int i = 0; i < 256; i++) {
        if (!allowed.test((size_t)i))
            continue;
        const int32_t d = distance(metric, pal.c[i], c.r, c.g, c.b);
        if (best_index < 0 || d < best) {  // ascending index: a tie keeps the lower one
            best = d;
            best_index = i;
        }
    }
    return (uint8_t)(best_index < 0 ? 0 : best_index);
}

uint8_t nearest_index(const Palette& pal, Rgb c, const IndexSet& allowed)
{
    return nearest_index(pal, c, allowed, ColorMetric::Euclidean);
}

std::array<uint8_t, 256> remap_table(const Palette& src, const Palette& dst, const IndexSet& allowed,
                                     bool keep_index0, ColorMetric metric)
{
    std::array<uint8_t, 256> table;
    table.fill(0);
    IndexSet candidates = allowed;
    candidates.reset(0);  // index 0 is never a target for a colour
    Matcher matcher(dst, candidates, metric);
    if (matcher.empty())
        return table;
    for (int i = 0; i < 256; i++)
        table[(size_t)i] = (i == 0 && keep_index0) ? 0 : matcher.nearest(src.c[i]);
    return table;
}

std::array<uint8_t, 256> remap_table(const Palette& src, const Palette& dst, const IndexSet& allowed,
                                     bool keep_index0)
{
    return remap_table(src, dst, allowed, keep_index0, ColorMetric::Euclidean);
}

// ------------------------------------------------------------- 8-bit MAT

bool generate_mips(Mat8Texture& t, int levels, const Palette& pal, const IndexSet& allowed, ColorMetric metric,
                   std::string* err)
{
    if (levels < 1 || levels > kMat8MaxMips)
        return fail(err, format("generate_mips: %d levels requested, retail JK allows 1..%d", levels, kMat8MaxMips));
    if (!is_pow2_dimension(t.width) || !is_pow2_dimension(t.height))
        return fail(err, format("generate_mips: size %dx%d is not a power of two in 1..%d on both sides", t.width,
                                t.height, kMat8MaxDimension));
    if (t.mips.empty())
        return fail(err, "generate_mips: the texture has no mip 0");
    const size_t target = (size_t)std::min(levels, max_mip_levels(t.width, t.height));
    const size_t keep = std::min(t.mips.size(), target);
    for (size_t m = 0; m < keep; m++) {  // validate before changing anything
        const size_t expect = (size_t)(t.width >> m) * (size_t)(t.height >> m);
        if (t.mips[m].size() != expect)
            return fail(err, format("generate_mips: mip %zu holds %zu texels, a %dx%d level needs %zu", m,
                                    t.mips[m].size(), t.width >> m, t.height >> m, expect));
    }
    IndexSet candidates = allowed;
    candidates.reset(0);
    Matcher matcher(pal, candidates, metric);
    if (keep < target && matcher.empty())
        return fail(err, "generate_mips: no palette index other than 0 is allowed");
    t.mips.resize(keep);

    while (t.mips.size() < target) {
        const size_t m = t.mips.size();
        const int pw = t.width >> (m - 1), ph = t.height >> (m - 1);
        const int nw = pw >> 1, nh = ph >> 1;
        const std::vector<uint8_t>& prev = t.mips[m - 1];
        std::vector<uint8_t> next((size_t)nw * (size_t)nh, 0);
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                const size_t row0 = (size_t)(2 * y) * (size_t)pw + (size_t)(2 * x);
                const uint8_t block[4] = {prev[row0], prev[row0 + 1], prev[row0 + (size_t)pw],
                                          prev[row0 + (size_t)pw + 1]};
                uint64_t sr = 0, sg = 0, sb = 0, n = 0;
                for (uint8_t v : block) {
                    if (t.masked && v == 0)
                        continue;  // a hole
                    sr += pal.c[v].r;
                    sg += pal.c[v].g;
                    sb += pal.c[v].b;
                    n++;
                }
                if (t.masked && n <= 1)
                    continue;  // three or four holes of four: stays a hole (0)
                const Rgb mean{mean_round(sr, n), mean_round(sg, n), mean_round(sb, n)};
                next[(size_t)y * (size_t)nw + (size_t)x] = matcher.nearest(mean);
            }
        }
        t.mips.push_back(std::move(next));
    }
    return true;
}

bool generate_mips(Mat8Texture& t, int levels, const Palette& pal, const IndexSet& allowed, std::string* err)
{
    return generate_mips(t, levels, pal, allowed, ColorMetric::Euclidean, err);
}

uint8_t representative_index(const Mat8Texture& t, const Palette& pal, const IndexSet& allowed, ColorMetric metric)
{
    if (t.mips.empty())
        return 0;
    uint64_t sr = 0, sg = 0, sb = 0, n = 0;
    for (uint8_t v : t.mips[0]) {
        if (t.masked && v == 0)
            continue;
        sr += pal.c[v].r;
        sg += pal.c[v].g;
        sb += pal.c[v].b;
        n++;
    }
    if (n == 0)
        return 0;
    IndexSet candidates = allowed;
    candidates.reset(0);
    if (candidates.none())
        return 0;
    return nearest_index(pal, Rgb{mean_round(sr, n), mean_round(sg, n), mean_round(sb, n)}, candidates, metric);
}

bool encode_mat8(const Mat8& mat, std::vector<uint8_t>& out, std::string* err)
{
    out.clear();
    if (!check_mat8(mat, err))
        return false;
    const bool solid = mat.textures.empty();
    std::vector<Mat8Cel> cels = mat.cels;
    if (cels.empty())
        for (size_t k = 0; k < mat.textures.size(); k++)
            cels.push_back(Mat8Cel{(int)k, 0});

    size_t total = kMatHeaderSize + cels.size() * (kMatTexinfoSize + (solid ? 0 : kMatTexinfoExtSize));
    for (const Mat8Texture& t : mat.textures) {
        total += kMatTextureHeaderSize;
        for (const std::vector<uint8_t>& level : t.mips)
            total += level.size();
    }
    out.reserve(total);

    // rdMaterialHeader: magic, revision, type, num_texinfo, num_textures, rdTexFormat (14 words:
    // colour mode 0 = indexed, 8 bpp, then the unused channel fields, written as zeros).
    put_bytes(out, "MAT ", 4);
    put_u32(out, kMatRevision);
    put_u32(out, solid ? kMatTypeColor : kMatTypeTexture);
    put_u32(out, (uint32_t)cels.size());
    put_u32(out, (uint32_t)mat.textures.size());
    put_u32(out, 0);
    put_u32(out, 8);
    for (int k = 0; k < 12; k++)
        put_u32(out, 0);

    // Per cel: rdTexinfoHeader {texture_type, solidColor, field_8..14}; textured cels add
    // rdTexinfoExtHeader {unk_00, height, alpha_en, unk_0c = texture index}.
    for (const Mat8Cel& cel : cels) {
        put_u32(out, solid ? 0 : kMatTexinfoTextured);
        put_u32(out, cel.solid_color);
        for (int k = 0; k < 4; k++)
            put_u32(out, kMatTexinfoUnused);
        if (!solid) {
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, (uint32_t)cel.texture);
        }
    }

    // Per texture: rdTextureHeader {width, height, alpha_en, unk_0c, unk_10 = key colour index,
    // num_mipmaps}, then the levels.
    for (const Mat8Texture& t : mat.textures) {
        put_u32(out, (uint32_t)t.width);
        put_u32(out, (uint32_t)t.height);
        put_u32(out, t.masked ? 1u : 0u);
        put_u32(out, 0);
        put_u32(out, 0);
        put_u32(out, (uint32_t)t.mips.size());
        for (const std::vector<uint8_t>& level : t.mips)
            put_bytes(out, level.data(), level.size());
    }
    if (out.size() != total) {  // cannot happen; guards the size arithmetic above
        out.clear();
        return fail(err, "internal error: MAT size mismatch");
    }
    return true;
}

bool write_mat8(const std::string& path, const Mat8& mat, std::string* err)
{
    std::vector<uint8_t> data;
    std::string why;
    if (!encode_mat8(mat, data, &why))
        return fail(err, path + ": " + why);
    if (!platform::write_file(path, data, &why))
        return fail(err, why);
    return true;
}

// ------------------------------------------------------------- CMP

Cmp legacy_cmp(const Palette& pal)
{
    Cmp c;
    c.version = kCmpVersionLegacy;
    c.flags = 0;
    c.tint[0] = c.tint[1] = c.tint[2] = 0.0f;
    c.palette = pal;
    c.light.assign(kCmpLightTableSize, 0);
    c.transparency.clear();
    return c;
}

bool build_cmp(const Palette& pal, const CmpBuildOptions& opts, Cmp& out, std::string* err)
{
    if (opts.transparency_alpha < 0 || opts.transparency_alpha > 256)
        return fail(err, format("build_cmp: transparency alpha %d/256 outside 0..256", opts.transparency_alpha));
    IndexSet results;  // indices a table entry may name for another index
    results.set();
    results &= ~opts.reserved;
    if (opts.key_index0)
        results.reset(0);
    if (results.none())
        return fail(err, "build_cmp: every palette index is reserved; the tables need at least one result");
    IndexSet identity = opts.fullbright;  // light columns that never darken
    if (opts.key_index0)
        identity.set(0);

    Matcher matcher(pal, results, opts.metric);
    Cmp c;
    c.version = kCmpVersionRetail;
    c.flags = opts.transparency_table ? kCmpFlagTransparency : 0;
    c.tint[0] = c.tint[1] = c.tint[2] = 1.0f;
    c.palette = pal;

    // Light table: light[L*256 + i], L = 0 (darkest) .. 63 (full light, the identity).
    c.light.assign(kCmpLightTableSize, 0);
    for (int i = 0; i < 256; i++)
        c.light[(size_t)kCmpFullLight * 256 + (size_t)i] = (uint8_t)i;
    for (int level = 0; level < kCmpFullLight; level++) {
        uint8_t* row = &c.light[(size_t)level * 256];
        for (int i = 0; i < 256; i++) {
            if (identity.test((size_t)i)) {
                row[i] = (uint8_t)i;
                continue;
            }
            const Rgb& p = pal.c[i];
            // round(v * L / 63), exact: 63 is odd, so the quotient is never exactly half-way
            const Rgb target{(uint8_t)((p.r * level + 31) / 63), (uint8_t)((p.g * level + 31) / 63),
                             (uint8_t)((p.b * level + 31) / 63)};
            row[i] = matcher.nearest(target);
        }
    }
    if (opts.monotonic_light) {
        for (int i = 0; i < 256; i++) {
            if (identity.test((size_t)i))
                continue;
            uint8_t above = c.light[(size_t)kCmpFullLight * 256 + (size_t)i];
            for (int level = kCmpFullLight - 1; level >= 0; level--) {
                uint8_t& entry = c.light[(size_t)level * 256 + (size_t)i];
                if (luma(pal.c[entry]) > luma(pal.c[above]))
                    entry = above;  // never brighter than the next lighter row
                above = entry;
            }
        }
    }

    // Transparency table: trans[src*256 + dst], src the lit texel, dst the framebuffer index.
    if (opts.transparency_table) {
        const int a = opts.transparency_alpha, b = 256 - a;
        c.transparency.assign(kCmpTransparencyTableSize, 0);
        for (int s = 0; s < 256; s++) {
            const Rgb& ps = pal.c[s];
            for (int d = 0; d < 256; d++) {
                const Rgb& pd = pal.c[d];
                const Rgb blend{(uint8_t)((a * ps.r + b * pd.r + 128) >> 8),
                                (uint8_t)((a * ps.g + b * pd.g + 128) >> 8),
                                (uint8_t)((a * ps.b + b * pd.b + 128) >> 8)};
                c.transparency[(size_t)s * 256 + (size_t)d] = matcher.nearest(blend);
            }
        }
        for (int i = 0; i < 256; i++)
            if (results.test((size_t)i))
                c.transparency[(size_t)i * 256 + (size_t)i] = (uint8_t)i;  // a colour over itself is itself
    }
    out = std::move(c);
    return true;
}

bool encode_cmp(const Cmp& cmp, std::vector<uint8_t>& out, std::string* err)
{
    out.clear();
    if (cmp.flags & kCmpFlagRgb16)
        return fail(err, format("CMP flags 0x%X: bit 2 (the RGB16 variant) is not written: retail JK reads it "
                                "without a light table, MotS and OpenJKDF2 with one", cmp.flags));
    if (cmp.light.size() != kCmpLightTableSize)
        return fail(err, format("CMP light table holds %zu bytes, needs %zu", cmp.light.size(), kCmpLightTableSize));
    const bool trans = (cmp.flags & kCmpFlagTransparency) != 0;
    if (trans && cmp.transparency.size() != kCmpTransparencyTableSize)
        return fail(err, format("CMP flags 0x%X announce a transparency table, but it holds %zu bytes, needs %zu",
                                cmp.flags, cmp.transparency.size(), kCmpTransparencyTableSize));
    if (!trans && !cmp.transparency.empty())
        return fail(err, format("CMP flags 0x%X have bit 0 clear, but a %zu-byte transparency table is present",
                                cmp.flags, cmp.transparency.size()));
    out.reserve(trans ? kCmpSizeWithTransparency : kCmpSizeNoTransparency);
    put_bytes(out, "CMP ", 4);
    put_u32(out, (uint32_t)cmp.version);
    put_u32(out, cmp.flags);
    for (float t : cmp.tint)
        put_f32(out, t);
    out.insert(out.end(), kCmpReservedBytes, 0);
    put_bytes(out, &cmp.palette, sizeof(Palette));
    put_bytes(out, cmp.light.data(), cmp.light.size());
    if (trans)
        put_bytes(out, cmp.transparency.data(), cmp.transparency.size());
    return true;
}

bool write_cmp(const std::string& path, const Cmp& cmp, std::string* err)
{
    std::vector<uint8_t> data;
    std::string why;
    if (!encode_cmp(cmp, data, &why))
        return fail(err, path + ": " + why);
    if (!platform::write_file(path, data, &why))
        return fail(err, why);
    return true;
}

bool parse_cmp(const uint8_t* data, size_t size, const std::string& name, Cmp& out, std::string* err)
{
    try {
        BinReader r(data, size, name);
        if (!r.peek_tag("CMP "))
            r.fail("not a CMP file: the magic is not \"CMP \"");
        r.skip(4);
        if (!r.can_read(kCmpHeaderSize - 4))
            r.fail(format("truncated CMP header: %zu bytes, the header needs %zu", size, kCmpHeaderSize));
        Cmp c;
        c.version = r.i32();
        const size_t flags_at = r.pos();
        c.flags = r.u32();
        if (c.flags & kCmpFlagRgb16)
            r.fail_at(flags_at, format("flags 0x%X: bit 2 (the RGB16 variant) is not supported: retail JK reads it "
                                       "without a light table, MotS and OpenJKDF2 with one", c.flags));
        for (float& t : c.tint)
            t = r.f32();
        r.skip(kCmpReservedBytes);
        const bool trans = (c.flags & kCmpFlagTransparency) != 0;
        const size_t need = (trans ? kCmpSizeWithTransparency : kCmpSizeNoTransparency) - kCmpHeaderSize;
        if (!r.can_read(need))
            r.fail(format("truncated CMP: flags 0x%X need %zu bytes after the header, %zu left", c.flags, need,
                          r.remaining()));
        r.bytes(&c.palette, sizeof(Palette));
        c.light = r.bytes(kCmpLightTableSize);
        if (trans)
            c.transparency = r.bytes(kCmpTransparencyTableSize);
        out = std::move(c);
        return true;
    } catch (const ParseError& e) {
        return fail(err, e.what());
    }
}

bool read_cmp(const std::string& path, Cmp& out, std::string* err)
{
    std::vector<uint8_t> data;
    std::string why;
    if (!platform::read_file(path, data, &why))
        return fail(err, why);
    return parse_cmp(data.data(), data.size(), path, out, err);
}

IndexSet fullbright_indices(const Cmp& cmp)
{
    IndexSet fb;
    if (cmp.light.size() != kCmpLightTableSize)
        return fb;
    for (int i = 0; i < 256; i++) {
        bool self = true;
        for (int level = 0; level < kCmpLightLevels && self; level++)
            self = cmp.light[(size_t)level * 256 + (size_t)i] == i;
        fb.set((size_t)i, self);
    }
    return fb;
}

IndexSet glow_indices(const Cmp& cmp, int min_channel)
{
    IndexSet glow = fullbright_indices(cmp);
    glow.reset(0);
    for (int i = 1; i < 256; i++) {
        const Rgb& p = cmp.palette.c[i];
        if (glow.test((size_t)i) && std::max(p.r, std::max(p.g, p.b)) <= min_channel)
            glow.reset((size_t)i);
    }
    return glow;
}

int light_monotonic_violations(const Cmp& cmp)
{
    if (cmp.light.size() != kCmpLightTableSize)
        return -1;
    int columns = 0;
    for (int i = 0; i < 256; i++) {
        for (int level = 0; level < kCmpFullLight; level++) {
            const uint8_t lo = cmp.light[(size_t)level * 256 + (size_t)i];
            const uint8_t hi = cmp.light[(size_t)(level + 1) * 256 + (size_t)i];
            if (luma(cmp.palette.c[hi]) < luma(cmp.palette.c[lo])) {
                columns++;
                break;
            }
        }
    }
    return columns;
}

} // namespace jk
} // namespace tpmx
