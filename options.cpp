// TPMExtractor fork - run options, command-line parsing and the --legacy profile.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "options.h"

#include "textout.h"

#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace tpmx {

const char* const kForkVersion = "0.1.0-dev (fork of bahstrike/TPMExtractor 2cc75d6)";

namespace {

bool parse_float(const std::string& s, float& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(d) || d > FLT_MAX || d < -FLT_MAX)
        return false;
    out = (float)d;
    return true;
}

const char* angles_name(AngleMode m) { return m == AngleMode::Raw ? "raw" : "rad2deg"; }

std::string fmt_float(float f) { return format("%g", (double)f); }

} // namespace

void apply_legacy(Options& o)
{
    o.legacy = true;
    o.scale = 0.2f;
    o.draw_scale = false;
    o.angles = AngleMode::Raw;
    o.insert_offset.mode = InsertOffsetMode::Fixed;
    o.insert_offset.x = 0.0f;
    o.insert_offset.y = 0.0f;
    o.insert_offset.z = 0.1f;
    o.radius.mode = RadiusMode::Fixed;
    o.radius.value = 0.1f;
    o.mat8 = false;
    o.mat16 = true;
    o.bmp = true;
    o.palette.clear();
    o.debug_dumps = true;
    o.geometry = GeometryMode::None;
    o.geometry_texture_rule = GeometryTextureRule::Game;
    o.keep_decompressed = false;
}

bool parse_args(int argc, const char* const* argv, ParsedArgs& out, std::string& err)
{
    out = ParsedArgs();
    Options& o = out.opts;
    std::vector<std::string> output_options;  // explicit options that change output bytes
    bool legacy = false;
    bool options_done = false;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (options_done || arg.size() < 2 || arg[0] != '-') {
            out.positional.push_back(arg);
            continue;
        }
        if (arg == "--") {
            options_done = true;
            continue;
        }
        // short verbosity flags: -v, -vv, -q
        if (arg[1] != '-') {
            if (arg.find_first_not_of('v', 1) == std::string::npos) {
                o.verbose += (int)arg.size() - 1;
                if (o.verbose > 2)
                    o.verbose = 2;
                continue;
            }
            if (arg == "-q") {
                o.verbose = -1;
                continue;
            }
            if (arg == "-h") {
                out.help = true;
                continue;
            }
            err = "unknown option " + arg;
            return false;
        }

        std::string name = arg, value;
        bool has_value = false;
        const size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            has_value = true;
        }
        auto take_value = [&]() -> bool {
            if (has_value)
                return true;
            if (i + 1 >= argc || !argv[i + 1]) {
                err = name + " needs a value";
                return false;
            }
            value = argv[++i];
            has_value = true;
            return true;
        };
        auto no_value = [&]() -> bool {
            if (has_value) {
                err = name + " does not take a value";
                return false;
            }
            return true;
        };
        auto flag = [&](bool& field, bool v) -> bool {
            if (!no_value())
                return false;
            field = v;
            output_options.push_back(name);
            return true;
        };

        if (name == "--help") {
            if (!no_value())
                return false;
            out.help = true;
        } else if (name == "--version") {
            if (!no_value())
                return false;
            out.version = true;
        } else if (name == "--legacy") {
            if (!no_value())
                return false;
            legacy = true;
        } else if (name == "--verbose") {
            if (!no_value())
                return false;
            if (o.verbose < 2)
                o.verbose = o.verbose < 0 ? 1 : o.verbose + 1;
        } else if (name == "--quiet") {
            if (!no_value())
                return false;
            o.verbose = -1;
        } else if (name == "--pause" || name == "--no-pause") {
            if (!no_value())
                return false;
            o.pause = name == "--pause";
            out.pause_explicit = true;
        } else if (name == "--scale") {
            if (!take_value())
                return false;
            float f;
            if (!parse_float(value, f) || !(f > 0.0f)) {
                err = "--scale needs a positive number, got \"" + value + "\"";
                return false;
            }
            o.scale = f;
            output_options.push_back(name);
        } else if (name == "--angles") {
            if (!take_value())
                return false;
            if (value == "raw")
                o.angles = AngleMode::Raw;
            else if (value == "rad2deg")
                o.angles = AngleMode::RadToDeg;
            else {
                err = "--angles must be raw or rad2deg, got \"" + value + "\"";
                return false;
            }
            output_options.push_back(name);
        } else if (name == "--insert-offset") {
            if (!take_value())
                return false;
            InsertOffsetOption io;
            if (value == "auto") {
                io.mode = InsertOffsetMode::Auto;
            } else if (value == "floor") {
                io.mode = InsertOffsetMode::Floor;
            } else if (value == "node0") {
                io.mode = InsertOffsetMode::Node0;
            } else if (value == "zero") {
                io.mode = InsertOffsetMode::Fixed;
            } else if (value == "legacy") {
                io.mode = InsertOffsetMode::Fixed;
                io.z = 0.1f;
            } else {
                const size_t c1 = value.find(',');
                const size_t c2 = c1 == std::string::npos ? c1 : value.find(',', c1 + 1);
                if (c2 == std::string::npos || value.find(',', c2 + 1) != std::string::npos ||
                    !parse_float(value.substr(0, c1), io.x) || !parse_float(value.substr(c1 + 1, c2 - c1 - 1), io.y) ||
                    !parse_float(value.substr(c2 + 1), io.z)) {
                    err = "--insert-offset must be auto, floor, node0, zero, legacy or X,Y,Z, got \"" + value + "\"";
                    return false;
                }
                io.mode = InsertOffsetMode::Fixed;
            }
            o.insert_offset = io;
            output_options.push_back(name);
        } else if (name == "--radius") {
            if (!take_value())
                return false;
            RadiusOption r;
            if (value == "computed") {
                r.mode = RadiusMode::Computed;
            } else if (value == "animated") {
                r.mode = RadiusMode::Animated;
            } else if (value == "stored") {
                r.mode = RadiusMode::Stored;
            } else if (value == "legacy") {
                r.mode = RadiusMode::Fixed;
                r.value = 0.1f;
            } else {
                float f;
                if (!parse_float(value, f) || f < 0.0f) {
                    err = "--radius must be computed, animated, stored, legacy or a number >= 0, got \"" + value +
                          "\"";
                    return false;
                }
                r.mode = RadiusMode::Fixed;
                r.value = f;
            }
            o.radius = r;
            output_options.push_back(name);
        } else if (name == "--draw-scale" || name == "--no-draw-scale") {
            if (!flag(o.draw_scale, name == "--draw-scale"))
                return false;
        } else if (name == "--mat8" || name == "--no-mat8") {
            if (!flag(o.mat8, name == "--mat8"))
                return false;
        } else if (name == "--mat16" || name == "--no-mat16") {
            if (!flag(o.mat16, name == "--mat16"))
                return false;
        } else if (name == "--bmp" || name == "--no-bmp") {
            if (!flag(o.bmp, name == "--bmp"))
                return false;
        } else if (name == "--debug-dumps" || name == "--no-debug-dumps") {
            if (!flag(o.debug_dumps, name == "--debug-dumps"))
                return false;
        } else if (name == "--keep-decompressed" || name == "--no-keep-decompressed") {
            if (!flag(o.keep_decompressed, name == "--keep-decompressed"))
                return false;
        } else if (name == "--palette") {
            if (!take_value())
                return false;
            if (value.empty()) {
                err = "--palette needs a file name";
                return false;
            }
            o.palette = value;
            output_options.push_back(name);
        } else if (name == "--geometry") {
            if (!take_value())
                return false;
            if (value == "obj") {
                o.geometry = GeometryMode::Obj;
            } else if (value == "none") {
                o.geometry = GeometryMode::None;
            } else {
                err = "--geometry must be obj or none, got \"" + value + "\"";
                return false;
            }
            output_options.push_back(name);
        } else if (name == "--geometry-texture-rule") {
            if (!take_value())
                return false;
            if (value == "game") {
                o.geometry_texture_rule = GeometryTextureRule::Game;
            } else if (value == "mat") {
                o.geometry_texture_rule = GeometryTextureRule::Mat;
            } else {
                err = "--geometry-texture-rule must be game or mat, got \"" + value + "\"";
                return false;
            }
            output_options.push_back(name);
        } else {
            err = "unknown option " + arg;
            return false;
        }
    }

    if (legacy) {
        if (!output_options.empty()) {
            std::string list;
            for (const std::string& n : output_options)
                list += (list.empty() ? "" : ", ") + n;
            err = "--legacy reproduces the original output byte for byte and cannot be combined with " + list;
            return false;
        }
        apply_legacy(o);
    }
    return true;
}

std::string usage_text(const char* program_name)
{
    const std::string prog = program_name && *program_name ? program_name : "tpmextractor";
    return "Usage: " + prog +
           " [options] DiscDir DumpDir\n"
           "\n"
           "  DiscDir   root of the TPM install disc (the folder that contains GAMEDATA)\n"
           "  DumpDir   output folder (created with its parents; with --legacy only the last\n"
           "            level is created, as in the original)\n"
           "\n"
           "Options:\n"
           "  --legacy               reproduce the original TPMExtractor output byte for byte\n"
           "  --scale F              scale for 3DO/KEY and level OBJ positions (default 0.2)\n"
           "  --[no-]draw-scale      also multiply positions by the BAF's own draw scale\n"
           "                         (header +0xAC), as the game does (default on); a 3DO or\n"
           "                         KEY drawn at a scale other than 1 is named *_s<hundredths>\n"
           "  --angles M             raw (default: TPM stores degrees) or rad2deg (never right\n"
           "                         for TPM data; for experiments)\n"
           "  --insert-offset M      auto (default: re-root at hierarchy node 0 when it lies above\n"
           "                         the origin, then the lowest point on the floor; people and\n"
           "                         creatures then stand on the floor in JK when their template\n"
           "                         has size= at most the INSERT OFFSET z - 0.005), floor (the\n"
           "                         lowest point; props), node0, zero, legacy, or X,Y,Z\n"
           "  --radius M             animated (default: the rest pose and every key frame of\n"
           "                         every model), computed (the rest pose, and every key frame\n"
           "                         of a model that auto or node0 re-roots), stored (TPM's\n"
           "                         own), legacy (0.1), or a value\n"
           "  --[no-]mat8            retail 8-bit MATs, used by the 3DOs (default on)\n"
           "  --[no-]mat16           16-bit MATs for OpenJKDF2/MotS testing (default on)\n"
           "  --[no-]bmp             BMP previews (default on)\n"
           "  --palette FILE         target palette (.cmp or 768-byte .pal) for the 8-bit MATs\n"
           "                         and for BAFs that have no colormap\n"
           "  --[no-]debug-dumps     *.3do.txt, *.key.txt and REMAIN.HEX (default on)\n"
           "  --geometry M           obj (default: each level's geometry as <NAME>.obj and\n"
           "                         <NAME>.mtl next to its textures) or none\n"
           "  --geometry-texture-rule R\n"
           "                         game (default: the texture the game draws each polygon\n"
           "                         with) or mat (always its mapping's texture)\n"
           "  --keep-decompressed    keep big.lab and LEVEL/<name>/<name>.dec\n"
           "  --[no-]pause           wait for a key before exiting (default off; the original\n"
           "                         always waited)\n"
           "  -v, --verbose          more output (twice for debug output)\n"
           "  -q, --quiet            only warnings and errors\n"
           "  -h, --help             show this text\n"
           "      --version          show the version\n"
           "\n"
           "Exit status: 0 success, 1 some files could not be processed, 2 usage or fatal error.\n";
}

std::string describe(const Options& o)
{
    std::string io;
    switch (o.insert_offset.mode) {
    case InsertOffsetMode::Auto: io = "auto"; break;
    case InsertOffsetMode::Floor: io = "floor"; break;
    case InsertOffsetMode::Node0: io = "node0"; break;
    case InsertOffsetMode::Fixed:
        io = fmt_float(o.insert_offset.x) + "," + fmt_float(o.insert_offset.y) + "," + fmt_float(o.insert_offset.z);
        break;
    }
    std::string radius;
    switch (o.radius.mode) {
    case RadiusMode::Computed: radius = "computed"; break;
    case RadiusMode::Animated: radius = "animated"; break;
    case RadiusMode::Stored: radius = "stored"; break;
    case RadiusMode::Fixed: radius = fmt_float(o.radius.value); break;
    }
    return std::string(o.legacy ? "legacy" : "default") + " mode: scale=" + fmt_float(o.scale) +
           " draw-scale=" + (o.draw_scale ? "on" : "off") + " angles=" + angles_name(o.angles) +
           " insert-offset=" + io + " radius=" + radius +
           " mat8=" + (o.mat8 ? "on" : "off") + " mat16=" + (o.mat16 ? "on" : "off") + " bmp=" + (o.bmp ? "on" : "off") +
           " palette=" + (o.palette.empty() ? std::string("auto") : o.palette) +
           " debug-dumps=" + (o.debug_dumps ? "on" : "off") +
           " geometry=" + (o.geometry == GeometryMode::Obj ? "obj" : "none") +
           " geometry-texture-rule=" + (o.geometry_texture_rule == GeometryTextureRule::Game ? "game" : "mat") +
           " keep-decompressed=" + (o.keep_decompressed ? "on" : "off");
}

} // namespace tpmx
