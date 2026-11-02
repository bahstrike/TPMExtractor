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

enum class InsertOffsetMode {
    Fixed,  // write x, y, z. --legacy: Fixed (0, 0, 0.1), printed as "0 0 0.100000 #dunno".
    Floor,  // (0, 0, -zmin): zmin = lowest vertex z in model space at rest pose (scaled), so
            // a model standing on the floor touches it in JK. Default; right for props.
    Node0,  // re-root at hierarchy node 0 so actors stand on the floor in JK: node 0's z moves
            // to 0 in the HIERARCHYDEF and in node 0's KEY entries, and INSERT OFFSET becomes
            // (0, 0, node0.z * scale). (Node 0's position as the insert offset alone would leave
            // TPM actors hovering, since they stand on their origin.)
};

struct InsertOffsetOption
{
    InsertOffsetMode mode = InsertOffsetMode::Floor;
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
