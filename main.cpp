// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): portable (no Win32 calls; the
// platform layer does directories, globbing and the key press), command-line options and
// --legacy, the commands all/z/lab/b3d/b3dall/baf/bafall, a case-insensitive level scan, errors
// that skip one file instead of ending the run, exit codes, and no key press at the end unless
// --pause is given; level geometry (OBJ/MTL) in the level commands' help; sound: the command wv
// and, in default mode, the voice files of LAB archives decoded to WAV.
//
// "tpmextractor DiscDir DumpDir" (the command "all") does what the original main() did, in the
// same order and with the same output names: BIG.Z -> big.lab -> biglab/, LOCALIZE.LAB ->
// localizelab/, VOICE.LAB -> voicelab/, the levels -> LEVEL/<NAME>/, and every biglab/*.baf ->
// BAF/<name>/. Default mode also decodes the voice files.

#include "b3d.h"
#include "baf.h"
#include "image.h"
#include "lab.h"
#include "log.h"
#include "options.h"
#include "platform.h"
#include "sound.h"
#include "zextract.h"

#include <cstdio>
#include <exception>
#include <set>
#include <string>
#include <vector>

using namespace tpmx;

namespace {

// Exit codes (also in the --help text).
const int kExitOk = 0;      // everything was processed (warnings are allowed)
const int kExitErrors = 1;  // the run finished, but some files could not be processed
const int kExitFatal = 2;   // usage error, or nothing could be done: missing input, an output
                            // directory that cannot be created, an unreadable --palette

// The levels the original processed, in its order (alphabetical: the order a scan of
// GAMEDATA/LEVEL finds on the retail disc). --legacy keeps this list; default mode scans.
const char* const kLegacyLevels[] = {"ASSAULT.B3D", "BIGCITY.B3D", "ESPA.B3D",  "FEDSHIP.B3D",
                                     "FINAL.B3D",   "GARDEN.B3D",  "GUNGA.B3D", "MAUL.B3D",
                                     "QUEEN.B3D",   "RACE.B3D",    "SWAMP.B3D"};

enum class Command { All, Z, Lab, B3d, B3dAll, Baf, BafAll, Wv };

struct CommandSpec
{
    const char* name;
    Command command;
    const char* args;
    bool sound;  // new in the fork: decodes sound (--legacy has no original output to reproduce)
};

const CommandSpec kCommands[] = {
    {"all", Command::All, "DiscDir DumpDir", false},    {"z", Command::Z, "FILE.Z OUTDIR", false},
    {"lab", Command::Lab, "FILE.LAB OUTDIR", false},    {"b3d", Command::B3d, "FILE.B3D OUTDIR", false},
    {"b3dall", Command::B3dAll, "DIR OUTDIR", false},   {"baf", Command::Baf, "FILE.BAF OUTDIR", false},
    {"bafall", Command::BafAll, "DIR OUTDIR", false},   {"wv", Command::Wv, "FILE.WV OUTDIR", true},
};

const CommandSpec* find_command(const std::string& name)
{
    for (const CommandSpec& c : kCommands)
        if (name == c.name)
            return &c;
    return nullptr;
}

const char kCommandHelp[] =
    "\n"
    "  DiscDir   root of the TPM install disc (the folder that contains GAMEDATA)\n"
    "  DumpDir   output folder, created with its parents (with --legacy only its last level\n"
    "            is created, as in the original)\n"
    "\n"
    "Commands (OUTDIR is created with its parents):\n"
    "  all DiscDir DumpDir    the whole disc; the same as giving no command:\n"
    "                           GAMEDATA/GOBS/BIG.Z -> big.lab -> DumpDir/biglab/\n"
    "                           GAMEDATA/GOBS/LOCALIZE.LAB -> DumpDir/localizelab/\n"
    "                           GAMEDATA/GOBS/VOICE.LAB -> DumpDir/voicelab/ (default mode:\n"
    "                           also each voice file decoded to <file>.wav)\n"
    "                           GAMEDATA/LEVEL/*.B3D -> DumpDir/LEVEL/<NAME>/\n"
    "                           DumpDir/biglab/*.baf -> DumpDir/BAF/<name>/\n"
    "  z FILE.Z OUTDIR        every file of an InstallShield v3 archive (BIG.Z holds big.lab)\n"
    "  lab FILE.LAB OUTDIR    every file of a LAB archive (big.lab, LOCALIZE.LAB, VOICE.LAB);\n"
    "                         default mode also decodes each *.wv to <file>.wav next to it\n"
    "  b3d FILE.B3D OUTDIR    one level -> OUTDIR/<NAME>/: its textures <i>.bmp and (default\n"
    "                         mode) its geometry <NAME>.obj with <NAME>.mtl\n"
    "  b3dall DIR OUTDIR      every *.B3D file in DIR -> OUTDIR/<NAME>/\n"
    "  baf FILE.BAF OUTDIR    one actor -> OUTDIR/<name>/ (colormap, MATs, BMPs, 3DO, KEYs)\n"
    "  bafall DIR OUTDIR      every *.BAF file in DIR -> OUTDIR/<name>/\n"
    "  wv FILE.WV OUTDIR      one voice file -> OUTDIR/<file>.wav (16-bit PCM)\n"
    "  <NAME> and <name> are the file name up to its first '.', <file> the file name without\n"
    "  its extension. Levels found by a directory scan get <NAME> in upper case, as the\n"
    "  original's level list. In the OBJ/MTL file names, characters of <NAME> other than ASCII\n"
    "  letters, digits, '_' and '-' become '_'. The sound command wv is new in the fork: --legacy\n"
    "  refuses it.\n"
    "\n";

const char kHelpNotes[] =
    "\n"
    "Input files and folders are found ignoring case. With --legacy, \"all\" processes the 11\n"
    "retail levels from the original's fixed list; otherwise it processes every *.B3D file in\n"
    "GAMEDATA/LEVEL. The original always waited for a key before exiting; this version waits\n"
    "only with --pause.\n";

std::string program_name(int argc, char* argv[])
{
    const std::string base = (argc > 0 && argv[0]) ? platform::base_name(argv[0]) : std::string();
    return base.empty() ? std::string("tpmextractor") : base;
}

std::string synopsis(const std::string& prog)
{
    return "Usage: " + prog + " [options] DiscDir DumpDir\n" + "       " + prog +
           " [options] COMMAND INPUT OUTDIR\n";
}

void print_banner()
{
    log::info("---------------------------------------------------\n");
    log::info("|          TPM Extractor - BAH 2021-2024          |\n");
    log::info("|            Strike & shiny & KnightCop           |\n");
    log::info("|                                                 |\n");
    log::info("|       .Z decompression by Wolfgang Frisch       |\n");
    log::info("|                                                 |\n");
    log::info("|     This project is in no way affiliated with   |\n");
    log::info("|       LEC or Disney. Use at your own peril.     |\n");
    log::info("---------------------------------------------------\n");
    log::info("fork %s\n", kForkVersion);
}

void print_help(const std::string& prog)
{
    // The option list is usage_text()'s, from its "Options:" heading on.
    const std::string options = usage_text(prog.c_str());
    const size_t at = options.find("Options:");
    const std::string text = synopsis(prog) + kCommandHelp +
                             (at == std::string::npos ? options : options.substr(at)) + kHelpNotes;
    std::fwrite(text.data(), 1, text.size(), stdout);
}

// After a usage error: the synopsis and a hint, on stderr.
void print_short_usage(const std::string& prog)
{
    const std::string text = synopsis(prog) + "Run " + prog + " --help for the commands and options.\n";
    std::fflush(stdout);
    std::fwrite(text.data(), 1, text.size(), stderr);
}

int finish(int code, const Options& opts)
{
    std::fflush(stdout);
    if (opts.pause) {
        std::printf("\n\n---------------Press any key to exit---------------\n");
        std::fflush(stdout);
        platform::wait_key();
    }
    return code;
}

// Control and non-ASCII bytes as \xNN, for messages that echo arguments or file names.
std::string printable(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

std::string upper_ascii(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    return u;
}

// An output directory's name for an input file: its file name up to the first '.'.
std::string stem(const std::string& file_name) { return file_name.substr(0, file_name.find('.')); }

// Creates one directory level, as the original's CreateDirectory; an existing directory is fine.
bool ensure_dir(const std::string& path)
{
    std::string why;
    const platform::MkdirResult r = platform::make_dir(path, &why);
    if (r == platform::MkdirResult::Created || platform::is_dir(path))
        return true;
    log::error("%s\n", why.empty() ? (path + ": exists and is not a directory").c_str() : why.c_str());
    return false;
}

// A command's OUTDIR: created with its parents.
bool prepare_out_dir(const std::string& dir)
{
    std::string why;
    if (platform::make_dirs(dir, &why))
        return true;
    log::error("cannot create the output directory: %s\n", why.c_str());
    return false;
}

// An input file named on the command line, found ignoring case; "" after an error.
std::string input_file(const std::string& path)
{
    const std::string found = platform::find_ci(path);
    if (platform::is_file(found))
        return found;
    log::error("%s: %s\n", path.c_str(), platform::is_dir(found) ? "is a folder, not a file" : "file not found");
    return std::string();
}

// An input folder named on the command line; "" after an error.
std::string input_dir(const std::string& path)
{
    const std::string found = platform::find_ci(path);
    if (platform::is_dir(found))
        return found;
    log::error("%s: %s\n", path.c_str(), platform::exists(found) ? "is not a folder" : "folder not found");
    return std::string();
}

// The files (not folders) in `dir` matching the case-insensitive glob `pattern`, in the order
// the original's FindFirstFile loop saw them (NTFS order).
bool list_files(const std::string& dir, const char* pattern, std::vector<std::string>& names)
{
    std::string why;
    const std::vector<platform::DirEntry> entries = platform::list_dir(dir, pattern, &why);
    if (!why.empty()) {
        log::error("%s\n", why.c_str());
        return false;
    }
    for (const platform::DirEntry& e : entries)
        if (!e.is_dir)
            names.push_back(e.name);
    return true;
}

// ------------------------------------------------------------------------------- levels

// --legacy: the original's 11 levels from `level_dir` (found ignoring case; a missing one is
// a warning, where the original crashed). Returns the number of levels that failed.
unsigned process_legacy_levels(const std::string& level_dir, const std::string& level_root, const Options& opts)
{
    unsigned failed = 0;
    for (const char* level : kLegacyLevels) {
        const std::string b3dfile = platform::path_join(level_dir, level);
        if (!platform::is_file(platform::find_ci(b3dfile))) {
            log::warn("%s: not found, skipped\n", b3dfile.c_str());
            continue;
        }
        if (!process_b3d(b3dfile, level_root, opts))
            failed++;
    }
    return failed;
}

// Every *.B3D file in `level_dir` (any case), in NTFS order, into `level_root`/<NAME>/. The
// file is passed to process_b3d() with its name in upper case (process_b3d opens it ignoring
// case), so <NAME> is upper case whatever the case of the copy, as with the original's list.
// Returns the number of levels that failed; `found` counts the levels attempted.
unsigned process_level_dir(const std::string& level_dir, const std::string& level_root, const Options& opts,
                           unsigned& found)
{
    found = 0;
    std::vector<std::string> names;
    if (!list_files(level_dir, "*.b3d", names))
        return 1;
    unsigned failed = 0;
    std::set<std::string> used;
    for (const std::string& name : names) {
        const std::string file = upper_ascii(name);
        const std::string level = stem(file);
        std::string out, why;
        if (!platform::safe_output_path(level_root, level, out, &why)) {
            log::error("%s: %s\n", platform::path_join(level_dir, name).c_str(), why.c_str());
            failed++;
            continue;
        }
        if (!used.insert(level).second) {
            log::warn("%s: skipped, another level file already writes %s\n",
                      platform::path_join(level_dir, name).c_str(), out.c_str());
            continue;
        }
        found++;
        if (!process_b3d(platform::path_join(level_dir, file), level_root, opts))
            failed++;
    }
    return failed;
}

// --------------------------------------------------------------------------------- BAFs

// One BAF into `out_root`/<name>/, <name> = `file_name` up to its first '.'.
bool process_one_baf(const std::string& baf_path, const std::string& file_name, const std::string& out_root,
                     const Options& opts, const Palette* fallback)
{
    std::string dir, why;
    if (!platform::safe_output_path(out_root, stem(file_name), dir, &why)) {
        log::error("%s: %s\n", baf_path.c_str(), why.c_str());
        return false;
    }
    if (!ensure_dir(dir))
        return false;
    return process_baf(baf_path, dir, opts, fallback);
}

// Every *.baf file (any case; folders skipped, as the original) in `baf_dir`, in NTFS order.
// Returns the number of BAFs that failed; `found` counts the BAFs attempted.
unsigned process_baf_dir(const std::string& baf_dir, const std::string& out_root, const Options& opts,
                         const Palette* fallback, unsigned& found)
{
    found = 0;
    std::vector<std::string> names;
    if (!list_files(baf_dir, "*.baf", names))
        return 1;
    unsigned failed = 0;
    std::set<std::string> used;
    for (const std::string& name : names) {
        const std::string baffile = platform::path_join(baf_dir, name);
        // Two BAFs with the same name up to the first '.' share a folder, as in the original.
        if (!used.insert(upper_ascii(stem(name))).second)
            log::warn("%s: shares the output folder %s/ with another BAF; files may be overwritten\n",
                      baffile.c_str(), stem(name).c_str());
        found++;
        if (!process_one_baf(baffile, name, out_root, opts, fallback))
            failed++;
    }
    return failed;
}

// ---------------------------------------------------------------------------- the disc

// One LAB archive from the disc into <dump_root>/<dump_name>. The folder is always created; a
// LAB that is not on the disc is a warning (the original printed "Couldn't open" and went on).
bool extract_disc_lab(const std::string& disc_root, const std::string& dump_root, const char* lab_rel,
                      const char* lab_name, const char* dump_name, const Options& opts)
{
    const std::string dump_dir = platform::path_join(dump_root, dump_name);
    if (!ensure_dir(dump_dir))
        return false;
    log::info("Extract %s to: %s...\n", lab_name, dump_dir.c_str());
    const std::string lab_path = platform::resolve_ci(disc_root, lab_rel);
    if (!platform::is_file(lab_path)) {
        log::warn("%s: not found, skipped\n", lab_path.c_str());
        return true;
    }
    return extract_lab(lab_path, dump_dir, opts);
}

int run_all(const std::string& disc_root, const std::string& dump_root, const Options& opts,
            const Palette* fallback)
{
    const std::string big_z = platform::resolve_ci(disc_root, "GAMEDATA/GOBS/BIG.Z");
    if (!platform::is_file(big_z)) {
        log::error("Could not find: %s\n", big_z.c_str());
        return kExitFatal;
    }
    log::info("Install disc: %s\n", disc_root.c_str());

    log::info("Dump directory: %s\n", dump_root.c_str());
    {
        std::string why;
        // The original created only the last level of DumpDir and gave up if its parent was
        // missing; --legacy keeps that, the default mode creates the parents.
        bool created = true;
        if (opts.legacy) {
            if (platform::make_dir(dump_root, &why) == platform::MkdirResult::ParentMissing)
                created = false;
        } else {
            created = platform::make_dirs(dump_root, &why);
        }
        if (!created || !platform::is_dir(dump_root)) {
            log::error("Couldn't create dump directory: %s%s%s\n", dump_root.c_str(), why.empty() ? "" : ": ",
                       why.c_str());
            return kExitFatal;
        }
    }

    unsigned failed = 0;

    // BIG.Z -> <dump>/big.lab -> <dump>/biglab/. If big.lab cannot be extracted, biglab/ and
    // BAF/ are skipped (the original stopped the run) and the other stages still run.
    const std::string biglab_dir = platform::path_join(dump_root, "biglab");
    bool have_biglab = ensure_dir(biglab_dir);
    if (!have_biglab)
        failed++;
    log::info("Extracting BIG.Z -> big.lab\n");
    std::vector<std::string> written;
    if (!extract_z(big_z, dump_root, opts, &written))
        failed++;
    std::string big_lab;
    for (const std::string& w : written)
        if (platform::iequals_ascii(platform::base_name(w), "big.lab"))
            big_lab = w;
    if (big_lab.empty()) {
        log::error("%s: big.lab was not extracted; skipping biglab/ and BAF/\n", big_z.c_str());
        failed++;
        have_biglab = false;
    } else {
        if (have_biglab) {
            log::info("Extract big.lab to: %s...\n", biglab_dir.c_str());
            if (!extract_lab(big_lab, biglab_dir, opts))
                failed++;
        }
        if (!opts.keep_decompressed) {
            std::string why;
            if (!platform::remove_file(big_lab, &why))
                log::warn("%s\n", why.c_str());
        }
    }

    if (!extract_disc_lab(disc_root, dump_root, "GAMEDATA/GOBS/LOCALIZE.LAB", "LOCALIZE.LAB", "localizelab", opts))
        failed++;
    if (!extract_disc_lab(disc_root, dump_root, "GAMEDATA/GOBS/VOICE.LAB", "VOICE.LAB", "voicelab", opts))
        failed++;

    // Levels -> <dump>/LEVEL/<NAME>/
    {
        const std::string level_root = platform::path_join(dump_root, "LEVEL");
        const std::string level_dir = platform::resolve_ci(disc_root, "GAMEDATA/LEVEL");
        if (!ensure_dir(level_root)) {
            failed++;
        } else if (opts.legacy) {
            failed += process_legacy_levels(level_dir, level_root, opts);
        } else if (!platform::is_dir(level_dir)) {
            log::warn("%s: not found, no levels processed\n", level_dir.c_str());
        } else {
            unsigned found = 0;
            failed += process_level_dir(level_dir, level_root, opts, found);
            if (found == 0)
                log::warn("%s: no *.B3D files found\n", level_dir.c_str());
        }
    }

    // biglab/*.baf -> <dump>/BAF/<name>/
    if (have_biglab) {
        log::info("Processing .BAF files...\n");
        const std::string baf_root = platform::path_join(dump_root, "BAF");
        if (!ensure_dir(baf_root)) {
            failed++;
        } else {
            unsigned found = 0;
            failed += process_baf_dir(biglab_dir, baf_root, opts, fallback, found);
        }
    }

    return failed ? kExitErrors : kExitOk;
}

// ------------------------------------------------------------------------ the commands

int run_z(const std::string& input, const std::string& out_dir, const Options& opts)
{
    const std::string path = input_file(input);
    if (path.empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    log::info("Extract %s to: %s...\n", path.c_str(), out_dir.c_str());
    return extract_z(path, out_dir, opts) ? kExitOk : kExitErrors;
}

int run_lab(const std::string& input, const std::string& out_dir, const Options& opts)
{
    const std::string path = input_file(input);
    if (path.empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    log::info("Extract %s to: %s...\n", path.c_str(), out_dir.c_str());
    return extract_lab(path, out_dir, opts) ? kExitOk : kExitErrors;
}

int run_b3d(const std::string& input, const std::string& out_dir, const Options& opts)
{
    // process_b3d() names the output after the path as given and opens it ignoring case.
    if (input_file(input).empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    return process_b3d(input, out_dir, opts) ? kExitOk : kExitErrors;
}

int run_b3dall(const std::string& input, const std::string& out_dir, const Options& opts)
{
    const std::string dir = input_dir(input);
    if (dir.empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    unsigned found = 0;
    const unsigned failed = process_level_dir(dir, out_dir, opts, found);
    if (found == 0 && failed == 0) {
        log::error("%s: no *.B3D files found\n", dir.c_str());
        return kExitFatal;
    }
    return failed ? kExitErrors : kExitOk;
}

int run_baf(const std::string& input, const std::string& out_dir, const Options& opts, const Palette* fallback)
{
    const std::string path = input_file(input);
    if (path.empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    return process_one_baf(path, platform::base_name(input), out_dir, opts, fallback) ? kExitOk : kExitErrors;
}

int run_bafall(const std::string& input, const std::string& out_dir, const Options& opts, const Palette* fallback)
{
    const std::string dir = input_dir(input);
    if (dir.empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    log::info("Processing .BAF files...\n");
    unsigned found = 0;
    const unsigned failed = process_baf_dir(dir, out_dir, opts, fallback, found);
    if (found == 0 && failed == 0) {
        log::error("%s: no *.BAF files found\n", dir.c_str());
        return kExitFatal;
    }
    return failed ? kExitErrors : kExitOk;
}

int run_wv(const std::string& input, const std::string& out_dir, const Options& opts)
{
    const std::string path = input_file(input);
    if (path.empty() || !prepare_out_dir(out_dir))
        return kExitFatal;
    return process_wv(path, platform::base_name(input), out_dir, opts) ? kExitOk : kExitErrors;
}

int run_command(Command command, const std::string& input, const std::string& out_dir, const Options& opts,
                const Palette* fallback)
{
    switch (command) {
    case Command::All: return run_all(input, out_dir, opts, fallback);
    case Command::Z: return run_z(input, out_dir, opts);
    case Command::Lab: return run_lab(input, out_dir, opts);
    case Command::B3d: return run_b3d(input, out_dir, opts);
    case Command::B3dAll: return run_b3dall(input, out_dir, opts);
    case Command::Baf: return run_baf(input, out_dir, opts, fallback);
    case Command::BafAll: return run_bafall(input, out_dir, opts, fallback);
    case Command::Wv: return run_wv(input, out_dir, opts);
    }
    return kExitFatal;
}

} // namespace

int main(int argc, char* argv[])
{
    ParsedArgs args;
    std::string err;
    const bool parsed = parse_args(argc, argv, args, err);
    // opts.pause is set only by --pause: the original always waited for a key, which blocked
    // scripts; the fork waits only when asked to.
    Options& opts = args.opts;
    log::set_verbosity(opts.verbose);
    const std::string prog = program_name(argc, argv);

    if (!parsed) {
        print_banner();
        log::error("%s\n", err.c_str());
        print_short_usage(prog);
        return finish(kExitFatal, opts);
    }
    if (args.version) {
        std::printf("TPMExtractor fork %s\n", kForkVersion);
        return finish(kExitOk, opts);
    }
    print_banner();
    if (args.help) {
        print_help(prog);
        return finish(kExitOk, opts);
    }

    // "DiscDir DumpDir" or "COMMAND INPUT OUTDIR". Every command takes two arguments, so two
    // arguments are always DiscDir and DumpDir, even when the first is spelled like a command
    // (a folder named "lab" still works); a command name that is not an existing folder is
    // reported as a command missing an argument.
    const std::vector<std::string>& pos = args.positional;
    const CommandSpec* command = nullptr;
    if (pos.size() == 3) {
        command = find_command(pos[0]);
        if (!command) {
            log::error("unknown command \"%s\" (commands: all, z, lab, b3d, b3dall, baf, bafall, wv; to extract a "
                       "disc give only DiscDir and DumpDir)\n",
                       printable(pos[0]).c_str());
            print_short_usage(prog);
            return finish(kExitFatal, opts);
        }
        if (command->sound && opts.legacy) {
            log::error("--legacy reproduces the original program's output; the %s command is new in the fork and has "
                       "none\n",
                       command->name);
            print_short_usage(prog);
            return finish(kExitFatal, opts);
        }
        if (command->sound && opts.sound == SoundMode::None) {
            log::error("--sound none turns sound decoding off, which is what the %s command does\n", command->name);
            print_short_usage(prog);
            return finish(kExitFatal, opts);
        }
    } else if (pos.size() == 2) {
        const CommandSpec* named = find_command(pos[0]);
        if (named && !platform::is_dir(pos[0])) {
            log::error("%s needs two arguments: %s %s\n", named->name, named->name, named->args);
            print_short_usage(prog);
            return finish(kExitFatal, opts);
        }
        command = &kCommands[0];
    } else {
        if (!pos.empty())
            log::error("expected DiscDir DumpDir or COMMAND INPUT OUTDIR, got %zu argument(s)\n", pos.size());
        print_short_usage(prog);
        return finish(kExitFatal, opts);
    }
    const std::string& input = pos[pos.size() - 2];
    const std::string& out_dir = pos[pos.size() - 1];
    log::verbose("%s\n", describe(opts).c_str());
    // The 3DO MATERIALS list names the 8-bit MATs, else the 16-bit ones; with both switched off
    // it still names the 16-bit ones, which this run does not write.
    const bool writes_3do = command->command == Command::All || command->command == Command::Baf ||
                            command->command == Command::BafAll;
    if (writes_3do && !opts.legacy && !opts.mat8 && !opts.mat16)
        log::warn("--no-mat8 with --no-mat16: the 3DO files name <base>16.mat materials, which are not "
                  "written\n");

    int code = kExitOk;
    try {
        // --palette is loaded before any work starts; a bad file is fatal.
        Palette palette;
        const Palette* fallback = nullptr;
        if (!opts.palette.empty()) {
            std::string why;
            if (!load_palette_file(opts.palette, palette, &why)) {
                log::error("%s\n", why.c_str());
                return finish(kExitFatal, opts);
            }
            fallback = &palette;
        }
        code = run_command(command->command, input, out_dir, opts, fallback);
    } catch (const std::exception& e) {
        log::error("fatal: %s\n", e.what());
        code = kExitFatal;
    }
    if (code == kExitOk && log::error_count())
        code = kExitErrors;
    if (log::error_count() || log::warning_count())
        log::info("%u error(s), %u warning(s)\n", log::error_count(), log::warning_count());
    return finish(code, opts);
}
