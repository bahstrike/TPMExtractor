// TPMExtractor fork - run options, command-line parsing and the --legacy profile.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// The default values below are the fork's improved defaults. apply_legacy() pins every
// value that affects output bytes to the original tool's (commit 2cc75d6) behaviour.
// Each field below states its default and what --legacy pins it to.

#pragma once

#include <string>
#include <vector>

namespace tpmx {

// Version string of the fork (printed by --version and in the banner).
extern const char* const kForkVersion;

// Hierarchy-node and keyframe angles.
enum class AngleMode {
    Raw,       // write TPM's values unchanged. TPM stores degrees with JK's pitch/yaw/roll
               // convention (found by static analysis of the game executable), so this is
               // right for JK. Default and --legacy.
    RadToDeg,  // multiply by 180/pi: the original's disabled "#else" experiment. Never right
               // for TPM data (TPM already stores degrees); kept for experiments only, and the
               // tool warns once when it is used.
};

enum class RadiusMode {
    Fixed,     // write `value` for the model and every mesh. --legacy: Fixed 0.1.
    Computed,  // JK rules: mesh = 1.1 * max |v| over the mesh's (scaled) vertices, measured from
               // the mesh origin; model = 1.1 * max distance from the model origin of any vertex
               // after the rest-pose hierarchy transform. Default.
    Stored,    // the model RADIUS is TPM's own (model header +0x5C) times the effective scale,
               // with a warning when it is smaller than the model; mesh radii stay computed
               // (TPM's mesh radius is a sphere around the mesh's bounding-box centre, not the
               // mesh origin JK culls around).
    Animated,  // as Computed, but the model RADIUS also covers every integer frame of every
               // key in the BAF (animation moves vertices past the rest-pose radius in about
               // 40% of the retail actors).
};

struct RadiusOption
{
    RadiusMode mode = RadiusMode::Computed;
    float value = 0.1f;  // only for Fixed
};

// How retail JK uses a 3DO's INSERT OFFSET (the engine code OpenJKDF2 decompiled): JK never
// draws the model shifted by it; it only places things with it.
//   - A thing created at another thing (COG CreateThing at a marker) gets its origin at the
//     marker plus the difference of the two insert offsets.
//   - A thing with physics standing on a floor (actors, players, anything with gravity) keeps
//     its origin max(insert offset z, size + 0.005) above the floor, where size is the
//     template's size= (the model's RADIUS when the template has no size=; movesize= does not
//     count for actors and players), unless the template sets height=.
// So the model's lowest point, -insert offset z below the origin, touches the floor only when
// insert offset z >= size + 0.005.
enum class InsertOffsetMode {
    Fixed,  // write x, y, z. --legacy: Fixed (0, 0, 0.1), printed as "0 0 0.100000 #dunno".
    Floor,  // (0, 0, -zmin): zmin = lowest vertex z in model space at rest pose (scaled). The
            // model touches the floor where JK places it by the insert offset alone, and on a
            // physics thing only when -zmin >= size + 0.005. TPM actors stand on their origin
            // (zmin is about 0), so with this mode they hover in JK by size + 0.005, or by about
            // their own height when the template has no size=. Right for props.
    Node0,  // re-root at hierarchy node 0: node 0's z moves to 0 in the HIERARCHYDEF and in node
            // 0's KEY entries (the model now hangs node0.z * scale below its origin), and INSERT
            // OFFSET becomes (0, 0, node0.z * scale). An actor that stands on its origin then
            // stands on the floor in JK when its template has size= at most node0.z * scale -
            // 0.005. Use it for people and creatures. (Node 0's position as the insert offset
            // without the re-root would leave them hovering.)
    Auto,   // per model: when hierarchy node 0 lies above z = 0, re-root at node 0 as Node0 does,
            // by node 0's z rounded to 1/1000 of a model unit (BAFs that share keys then keep
            // byte-identical keys), and give the re-rooted model Floor's insert offset (0, 0,
            // -zmin); every other model gets Floor. A model that stands on its origin, as TPM's
            // people and creatures do, then hangs node 0's height below its origin and stands on
            // the floor in JK (template size= at most INSERT OFFSET z - 0.005), and no model's
            // lowest point ends up below the floor. Default.
};

struct InsertOffsetOption
{
    InsertOffsetMode mode = InsertOffsetMode::Auto;
    float x = 0.0f, y = 0.0f, z = 0.0f;  // only for Fixed
};

struct Options
{
    bool legacy = false;              // --legacy: reproduce 2cc75d6 byte for byte
    float scale = 0.2f;               // position scale for 3DO/KEY output (Strike's worldScale)
    bool draw_scale = true;           // also multiply positions by each BAF's own draw scale
                                      // (header +0xAC), as the game draws the actor. --legacy: off
    AngleMode angles = AngleMode::Raw;
    InsertOffsetOption insert_offset;
    RadiusOption radius;
    bool mat8 = true;                 // write retail 8-bit MATs <base>8.mat; the 3DO refers to them
    bool mat16 = true;                // write 16-bit MATs <base>16.mat (OpenJKDF2/MotS testing only)
    bool bmp = true;                  // write BMP previews
    std::string palette;              // target palette file (.cmp or raw .pal); "" = automatic
    bool debug_dumps = true;          // write *.3do.txt, *.key.txt and REMAIN.HEX
    bool keep_decompressed = false;   // keep big.lab and LEVEL/<L>/<L>.dec
    bool pause = false;               // wait for a key before exiting
    int verbose = 0;                  // -1 quiet, 0 normal, 1 verbose, 2 debug
};

// Sets every output-affecting field to the original tool's behaviour (legacy = true,
// scale 0.2, draw_scale off, angles Raw, insert offset Fixed (0,0,0.1), radius Fixed 0.1, mat8
// off, mat16 on, bmp on, palette "", debug dumps on, keep_decompressed off). pause and verbose
// are kept.
void apply_legacy(Options& o);

struct ParsedArgs
{
    Options opts;
    std::vector<std::string> positional;  // in order, e.g. {disc, dump}
    bool help = false;
    bool version = false;
    bool pause_explicit = false;  // --pause or --no-pause was given (else main may decide)
};

// Parses argv[1..argc-1]. Options may appear anywhere; "--" ends option parsing. Values may
// be given as "--opt value" or "--opt=value". With --legacy, any explicit option that would
// change output bytes is a usage error. Returns false and sets `err` on a usage error.
bool parse_args(int argc, const char* const* argv, ParsedArgs& out, std::string& err);

// The --help text.
std::string usage_text(const char* program_name);

// One line describing the effective options, for the log.
std::string describe(const Options& o);

} // namespace tpmx
