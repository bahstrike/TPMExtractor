# TPMExtractor (fork)

> **Fork notice.** This is a fork of
> [bahstrike/TPMExtractor](https://github.com/bahstrike/TPMExtractor) by Strike (Bad Ass Hackers,
> 2021-2024), based on its commit `2cc75d6`. The fork no longer needs Windows to build (it is
> tested on Linux and with MinGW for Windows), fixes crashes and parsing bugs, adds commands for
> single files, and writes models and textures that retail Jedi Knight can load. The original
> program's output is still available, byte for byte, with `--legacy`. This README was rewritten
> for the fork in 2026; every change is listed in [CHANGES.md](CHANGES.md). The original
> description, credits and disclaimer are kept below.

**No game data is included.** This repository holds source code and synthetic test data only. To
extract anything you need your own copy of the game: the PC CD-ROM of *Star Wars: Episode I - The
Phantom Menace* (1999). Do not share what you extract.

## About

TPMExtractor is a tool for extracting assets from the game "Star Wars: Episode I - The Phantom Menace"

Object and Level assets are stored seperately in TPM (The Phantom Menace).

Objects (models/textures/animations) are stored within .BAF files
which are further contained within "big.lab" which is present in compressed form as "BIG.Z"
on the install disc.

Levels are contained within compressed .B3D files which are located on the install disc under /GAMEDATA/LEVEL/.

TPMExtractor is capable of processing these incoming data directly from the install disc.

The output is meant for Jedi Knight: Dark Forces II ("JK") modding: models become JK `.3do` files,
animations `.key` files, textures `.mat` files with `.cmp` colormaps, and every texture is also
written as a `.bmp` preview.

## Usage

`tpmextractor InstallDiscPath DumpDirPath`

Example:
`tpmextractor H: "c:/tpm dump"`

`InstallDiscPath` is the root of the disc (or of a copy of it): the folder that contains `GAMEDATA`.
File and folder names on the disc are found whatever their letter case, so a copy on a Linux or
macOS file system works too:

```
tpmextractor /media/cdrom ~/tpm-dump
tpmextractor --legacy H: "c:/tpm dump legacy"
```

### Commands

The command above is the same as `all`. The other commands work on single files, for example on
files already extracted from `big.lab`:

```
tpmextractor [options] DiscDir DumpDir
tpmextractor [options] COMMAND INPUT OUTDIR
```

| Command | Does |
|---|---|
| `all DiscDir DumpDir` | the whole disc: `GAMEDATA/GOBS/BIG.Z` -> `big.lab` -> `DumpDir/biglab/`, `LOCALIZE.LAB` -> `localizelab/`, `VOICE.LAB` -> `voicelab/`, `GAMEDATA/LEVEL/*.B3D` -> `LEVEL/<NAME>/`, `biglab/*.baf` -> `BAF/<name>/` |
| `z FILE.Z OUTDIR` | every file of an InstallShield v3 archive (`BIG.Z` holds `big.lab`) |
| `lab FILE.LAB OUTDIR` | every file of a LAB archive (`big.lab`, `LOCALIZE.LAB`, `VOICE.LAB`) |
| `b3d FILE.B3D OUTDIR` | one level's textures -> `OUTDIR/<NAME>/<i>.bmp` |
| `b3dall DIR OUTDIR` | every `*.B3D` in `DIR` -> `OUTDIR/<NAME>/` |
| `baf FILE.BAF OUTDIR` | one actor or object -> `OUTDIR/<name>/` |
| `bafall DIR OUTDIR` | every `*.BAF` in `DIR` -> `OUTDIR/<name>/` |

`<NAME>` and `<name>` are the file name up to its first `.`. `OUTDIR` is created with its parents.
Examples:

```
tpmextractor z H:/GAMEDATA/GOBS/BIG.Z work
tpmextractor lab work/big.lab work/biglab
tpmextractor bafall work/biglab work/BAF
tpmextractor --palette mylevel.cmp baf work/biglab/someactor.baf work/BAF
tpmextractor --keep-decompressed b3dall H:/GAMEDATA/LEVEL work/LEVEL
```

Exit status: 0 success (warnings allowed), 1 some files could not be processed, 2 usage error or
fatal error. Errors name the file and, where known, the offset; the run goes on with the next
file. The tool waits for a key before exiting only with `--pause` (the original always waited).

### Options

```
--legacy               reproduce the original TPMExtractor output byte for byte
--scale F              scale for 3DO/KEY positions (default 0.2)
--[no-]draw-scale      also multiply positions by the BAF's own draw scale
                       (header +0xAC), as the game does (default on)
--angles M             raw (default: TPM stores degrees) or rad2deg (never right
                       for TPM data; for experiments)
--insert-offset M      floor (default; props), node0 (re-root at hierarchy node 0
                       so actors stand on the floor in JK), zero, legacy, or X,Y,Z
--radius M             computed (default; rest pose), animated (also covers every
                       key frame), stored (TPM's own), legacy (0.1), or a value
--[no-]mat8            retail 8-bit MATs, used by the 3DOs (default on)
--[no-]mat16           16-bit MATs for OpenJKDF2/MotS testing (default on)
--[no-]bmp             BMP previews (default on)
--palette FILE         target palette (.cmp or 768-byte .pal) for the 8-bit MATs
                       and for BAFs that have no colormap
--[no-]debug-dumps     *.3do.txt, *.key.txt and REMAIN.HEX (default on)
--keep-decompressed    keep big.lab and LEVEL/<name>/<name>.dec
--[no-]pause           wait for a key before exiting (default off; the original
                       always waited)
-v, --verbose          more output (twice for debug output)
-q, --quiet            only warnings and errors
-h, --help             show this text
    --version          show the version
```

Options may come before or after the paths; `--opt=value` works too.

### What you get

```
DumpDir/biglab/<entry>          every file of big.lab (big.lab itself is deleted afterwards)
DumpDir/localizelab/<entry>     every file of LOCALIZE.LAB
DumpDir/voicelab/<entry>        every file of VOICE.LAB
DumpDir/LEVEL/<NAME>/<i>.bmp    the level's textures, in their own palettes
DumpDir/BAF/<name>/             one folder per BAF:
    <colormap>.cmp              the BAF's colormap (default mode: a retail JK CMP)
    <mat>.mat                   raw copy of each material as stored in the BAF
    <base>.bmp                  preview of the material's first texture
    <base>8.mat                 retail JK 8-bit MAT (default mode; the 3DO uses it)
    <base>16.mat                16-bit MAT (OpenJKDF2 and Mysteries of the Sith only)
    <model>.3do                 the model
    <key>.key                   each animation
    *.3do.txt, *.key.txt        debug dumps of the parsed structures
    REMAIN.HEX                  the bytes after the last parsed structure
```

**Using the output in retail Jedi Knight.** JK 1.0/1.01 draws 8-bit MATs through the colormap of
the level, so a MAT shows its true colours only with the palette its indices refer to. By default
each BAF's 8-bit MATs index that BAF's own colormap, which is written next to them. To use models
in an existing JK level, give that level's colormap (from your own copy of JK) with
`--palette level.cmp`: every 8-bit MAT is then remapped into it, avoiding its glowing colours.
The 16-bit MATs load in OpenJKDF2 and in Mysteries of the Sith, but retail JK cannot be relied on
to draw them; use the `<base>8.mat` files there.

For actors that stand on the floor (people, creatures), `--insert-offset node0` re-roots the model
so it stands on the floor in JK instead of hovering. The default, `floor`, suits props.

## What `--legacy` does

`--legacy` reproduces the output of the original program (commit `2cc75d6`) byte for byte: the
same files, the same names and the same bytes, including its quirks (placeholder 3DO values,
unpadded BMP rows, the legacy 17,216-byte `.cmp`, the misread colormap-less BAF). It refuses every
option that would change the output. Use it when you need exactly what the original wrote, or to
check that nothing changed.

It was checked on a retail disc against the original sources built unchanged (as a 32-bit
program behind a small Windows shim, not on Windows itself): all 15,027 output files and 320
folders match. There is one difference, in both modes: in 153 debug files (`*.3do.txt`,
`*.key.txt`) the original printed 16 "TAIL 64bytes" lines from uninitialised memory, which
differed from run to run; the fork prints zeros there.

Without `--legacy` ("default mode") the output differs from the original only in the documented
improvements below.

## What changed

The summary; [CHANGES.md](CHANGES.md) has the details.

- **Portable.** The code no longer depends on Windows: it builds with Visual Studio, CMake or a
  plain Makefile, with g++, clang or MinGW-w64, as 32-bit or 64-bit, and gives the same output on
  each (fixed-width file structures, its own number formatting, CRLF text as before).
- **Safety.** Every read is bounds-checked, every count from a file is limited, and names taken
  from game files are checked before they become file names (no absolute paths, `..`, or Windows
  device names). Damaged files are reported with file and offset and skipped instead of crashing
  or hanging the run.
- **Commands** for single archives, levels and BAFs (above), exit codes, case-insensitive input,
  and no key press at the end unless `--pause` is given.
- **Default-mode BAF output for JK** (`--legacy` gives the original output; several items also
  have their own option):
  - the real BAF layout is read, so the one colormap-less BAF on the retail disc gets its model;
  - computed 3DO `RADIUS` values (JK's rules) instead of the placeholder 0.1, optionally also
    covering every animation frame (`--radius animated`);
  - `INSERT OFFSET` on the floor for props, or a re-root for actors (`--insert-offset node0`);
  - positions multiplied by each BAF's own draw scale, as the game draws it (`--no-draw-scale`
    turns this off);
  - the real hierarchy node types (so partial-body animations blend as authored), perspective
    texture mode (`TEXTUREMODE 3`), and face flags limited to the ones JK uses;
  - retail JK colormaps (version 30, with light and transparency tables, 82,752 bytes) instead of
    the original's 17,216-byte file with an empty light table;
  - retail 8-bit MATs `<base>8.mat` (with mipmaps, index 0 kept as the transparent colour only
    where the texture is masked), which the 3DOs now name.
- **Levels:** the level decompressor works in memory, reports truncated files, and produces the
  same bytes as before; the level file is walked section by section with every size checked;
  `--keep-decompressed` keeps the decompressed `<NAME>.dec`.
- **Archives:** LAB files are read through their stored offsets (the original assumed sequential
  storage); `.Z` files are decompressed with a size check, and a damaged one no longer ends the run.
- **Tests:** unit tests and an end-to-end suite on generated, generic fixtures (boxes, pyramids,
  simple rigs; no game content), plus optional real-data tests that read your own disc.

### Bugs found in the original

These were found while porting the code, by static analysis of the game's own loaders, and by
checking every file on a retail disc. `--legacy` keeps the original's behaviour; default mode
fixes them.

- **Model count.** The original reads the BAF's "has colormap" field as a model count. In the one
  BAF without a colormap this skips the model, reads the model data as a key, and writes a
  key-format `.3do`. Every BAF has exactly one model.
- **Colormap detection** by "the name ends in `t`" works only by accident (colormap names end in
  `.cmp`, material names in `.mat`); the field at header offset 0xCC says it.
- **Per-vertex arrays.** Two per-vertex float arrays (intensity and an unused one) are read as
  interleaved pairs; they are two consecutive arrays. The byte count was right, so nothing else
  shifted.
- **Per-face UV indices** are read for every face; the game reads them only for textured
  materials. Every retail material is textured, so no file was misread.
- **Placeholder 3DO values.** `RADIUS 0.1`, `INSERT OFFSET 0 0 0.1` and hierarchy type `0x1` are
  written although the file has the real values. 209 of the 302 models the original writes reach
  farther than 0.1 from their origin, so JK culls them while parts are still on screen.
- **Draw scale ignored.** Each BAF stores the scale the game draws it at; 134 retail BAFs have one
  other than 1.
- **Uninitialised memory in the debug dumps.** When the last 64-byte read hits the end of the file,
  the original printed whatever was on the stack (153 files).
- **32-bit Windows only.** The file structures were read straight into C structs with a pointer
  field and `long` fields, so a 64-bit build misread the keyframes, and a 64-bit Linux or macOS
  build (where `long` is 8 bytes) also the material and colormap headers.
- **Crashes and hangs on damaged files**: a signed overflow in the mipmap size, null buffers
  passed to `fread`/`fwrite`, loops bounded only by a corrupted count, `assert` aborts in the
  `.Z` reader, undefined behaviour in the `.Z` decompressor (a pointer computed before the start
  of its buffer), and a crash on a missing level.
- **Levels.** The textures were found by searching for the tag bytes (which other data could
  contain) and the texture loop stopped 12 bytes short of the section's end (harmless on retail
  data). The header fields read as "magic" 2200 and 293 are the section size and the version.
- **Archives.** LAB entries were read one after another, ignoring the stored offsets (identical on
  the retail disc, wrong for LAB files whose entries are not stored in order). The `.Z` reader's
  decompressor handed the same input again when a truncated file asked for more, so it decoded
  garbage instead of failing.

The raw `.mat` copy of a material that carries its own palette keeps that palette in default mode,
after the pixels where JK's loader reads it; the original left it out, and `--legacy` still does.
`<base>8.mat` is the file retail JK draws through the level's colormap.

## Building

The tool is C++17 with no dependencies; the `.Z` code (unshieldv3 and blast) is included.

**Visual Studio (Windows).** Open `TPMExtractor.sln` (Visual Studio 2022, toolset v143) and build
`Release`. The project lists every source file. The fork's maintainers had no Visual Studio, so
this project file is not tested; the MinGW and CMake builds are.

**CMake (any system).**

```
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Options: `-DTPMX_WERROR=ON` (warnings are errors), `-DTPMX_SANITIZE=ON` (AddressSanitizer and
UndefinedBehaviorSanitizer), `-DTPMX_FUZZ=ON` (fuzz targets, see `tests/CMakeLists.txt`). Windows
cross builds with MinGW-w64: `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake` (or
`toolchain-mingw32.cmake`). With Visual Studio installed, CMake can also generate a Visual Studio
build.

**Make (GNU make with g++ or clang).**

```
make                      # build/<target>/tpmextractor
make check                # build and run the tests
make SANITIZE=1 check     # with AddressSanitizer and UndefinedBehaviorSanitizer
make WERROR=1             # warnings are errors
make CXX=x86_64-w64-mingw32-g++ CC=x86_64-w64-mingw32-gcc        # Windows .exe, static
make CXX=clang++ CC=clang
```

**macOS.** CMake or make with Apple's clang should work (the code has no platform-specific parts
besides the small `platform.cpp`), but the fork's maintainers have not built it on a Mac.

The builds tested by the fork: g++ 13 and clang 18 on Linux (64-bit, and 32-bit static), MinGW-w64
for 32-bit and 64-bit Windows, all with no warnings at `-Wall -Wextra`. The Windows executables
were built but not run.

### Tests

`make check` or `ctest` runs the unit tests and `tests/run_tests.py` (Python 3, standard library
only; skipped if Python is missing). The end-to-end tests build their own fixtures. With your own
disc, also run the real-data tests:

```
TPM_DISC=/path/to/disc make check
```

They write their dumps to a temporary folder (or `TPM_WORK`) outside the source tree and delete
them afterwards. `TPM_REF_MANIFEST` (a manifest of the original program's output with the
uninitialised "tail" lines masked) adds the `--legacy` identity test.

Fuzz targets (CMake with `-DTPMX_FUZZ=ON`, best with clang and `-DTPMX_SANITIZE=ON`) cover every
input the tool reads: BAF files through all their writers in both modes, levels, LAB and `.Z`
archives and colormaps. The file-level ones fail when anything is written outside their output
folder. `ctest -L fuzz` runs each one briefly on generated seeds; `tests/CMakeLists.txt` shows how
to run them for longer.

## License

Apache License 2.0, like the original (see `LICENSE` and `NOTICE`). `unshieldv3/` is Wolfgang
Frisch's unshieldv3 (Apache 2.0, its own `LICENSE`), and `unshieldv3/blast.c`/`blast.h` are Mark
Adler's blast (zlib license). Files changed by the fork say so at the top.


## Credits
- KnightCop - Project lead
- Strike [@bahstrike](https://github.com/bahstrike) - Code, File analysis
- shiny [@shinyquagsire23](https://github.com/shinyquagsire23) - .B3D decompression
- Wolfgang Frisch [@wfr](https://github.com/wfr) - .Z decompression

### Disclaimer
This project is in no way affiliated with LEC or Disney. Use at your own peril.
