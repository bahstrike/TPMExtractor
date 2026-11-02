// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): new entry point signature with
// Options and a fallback palette; subfile() replaced by platform::safe_output_path(); default
// mode reads the real BAF layout and writes JK-ready 3DO/KEY values, retail CMPs and 8-bit
// MATs, with names that keep scaled models and keys apart.

#pragma once

#include "image.h"
#include "options.h"

#include <string>

namespace tpmx {

// Default-mode refinements, for callers that set them directly. The 4-argument process_baf()
// takes them from Options (--[no-]draw-scale, --radius animated). Both are ignored with --legacy.
struct BafExtras
{
    // Multiply every position by the BAF's own draw scale (header +0xAC; 1.0 on most actors,
    // 0.5..3.0 on the retail disc), as the game does when it draws the actor: effective scale =
    // Options::scale * draw scale, applied to vertices, hierarchy positions and pivots, and KEY
    // positions and position deltas. The draw scale is applied in hundredths (about half of the
    // retail BAFs store 1.00001 for 1.0), and when it is not 1 the 3DO and KEY files are named
    // "<name>_s<hundredths>.<ext>" ("_s110" for 1.1): JK keeps one model and one key per name,
    // and other BAFs may share those names at another scale. Default on (Options::draw_scale).
    bool draw_scale = true;
    // With the computed radius: the model RADIUS also covers every integer frame of every key in
    // the BAF, not only the rest pose. Animation moves vertices past the rest-pose radius in
    // about 40% of the retail actors. Default off; RadiusMode::Animated turns it on, and so does
    // InsertOffsetMode::Node0 with RadiusMode::Computed (the re-rooted origin is the pelvis, and
    // the keys reach past the rest-pose RADIUS around it in 164 of the 303 retail BAFs).
    // InsertOffsetMode::Auto with RadiusMode::Computed covers the keys of each model it re-roots,
    // whatever this flag says.
    bool animated_radius = false;
};

// Processes one BAF actor file (the original dumpbaf()) into `out_dir`, which must exist:
// the colormap (<cmp name>), per material the raw MAT copy, <base>.bmp, <base>16.mat and (default
// mode) <base>8.mat, the model (<model>.3do + .3do.txt), the keyframes (<key>.key + .key.txt) and
// REMAIN.HEX. Default mode also writes <BAF name>.cmp for a BAF without a colormap (the palette
// its 8-bit MATs index; not with a fallback palette), keeps the palette of own-palette materials
// in their raw copies, and names a scaled 3DO and KEYs as BafExtras::draw_scale says.
// fallback_palette: the --palette palette, or nullptr. It is the palette of BAFs without a
// colormap (nullptr: the original's grayscale ramp) and, in default mode, the palette the 8-bit
// MATs are remapped into.
// Returns false, after logging the reason with file and offset, if the BAF could not be
// processed completely; files written before the problem stay.
bool process_baf(const std::string& baf_path, const std::string& out_dir, const Options& opts,
                 const Palette* fallback_palette = nullptr);

// The same with explicit default-mode refinements (the overload above builds them from opts).
bool process_baf(const std::string& baf_path, const std::string& out_dir, const Options& opts,
                 const Palette* fallback_palette, const BafExtras& extras);

} // namespace tpmx
