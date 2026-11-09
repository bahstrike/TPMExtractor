# TPMExtractor (fork)

> **Fork notice.** This is a fork of
> [bahstrike/TPMExtractor](https://github.com/bahstrike/TPMExtractor) by Strike (Bad Ass Hackers,
> 2021-2024), based on its commit `2cc75d6`. The fork no longer needs Windows to build (it is
> tested on Linux and with MinGW for Windows), fixes crashes and parsing bugs, adds commands for
> single files, writes models and textures that retail Jedi Knight can load, exports the
> levels' geometry, which the original never decoded, as OBJ models, and decodes the game's voice
> lines and music to WAV files. The original program's output is still available, byte for byte,
> with `--legacy`. This README was rewritten for the fork in 2026; every change is listed in
> [CHANGES.md](CHANGES.md). The original description, credits and disclaimer are kept below.
>
> Fork improvements (2026): Andrei Kiralv/Darkahn, with Claude (Anthropic), Red in Andrei's
> projects.

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
written as a `.bmp` preview. Each level's geometry becomes a Wavefront OBJ model (`<NAME>.obj` with
its `<NAME>.mtl`) textured with the level's `.bmp` files, for Blender and other 3D tools. The voice
lines (`.WV` files in `VOICE.LAB`) and the music (`.IMC` files in the `MUSIC` folder) are decoded
to plain 16-bit PCM `.wav` files, which retail JK can load, and each music file's loop points and
markers are written next to it as text.

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
| `all DiscDir DumpDir` | the whole disc: `GAMEDATA/GOBS/BIG.Z` -> `big.lab` -> `DumpDir/biglab/`, `LOCALIZE.LAB` -> `localizelab/`, `VOICE.LAB` -> `voicelab/` (with each voice file also decoded to `.wav`), the music folder `MUSIC` or `GAMEDATA/MUSIC` (when there is one) -> `music/`, `GAMEDATA/LEVEL/*.B3D` -> `LEVEL/<NAME>/`, `biglab/*.baf` -> `BAF/<name>/` |
| `z FILE.Z OUTDIR` | every file of an InstallShield v3 archive (`BIG.Z` holds `big.lab`) |
| `lab FILE.LAB OUTDIR` | every file of a LAB archive (`big.lab`, `LOCALIZE.LAB`, `VOICE.LAB`); each voice file `<file>.wv` is also decoded to `<file>.wav` next to it |
| `b3d FILE.B3D OUTDIR` | one level -> `OUTDIR/<NAME>/`: its textures `<i>.bmp` and its geometry `<NAME>.obj` with `<NAME>.mtl` |
| `b3dall DIR OUTDIR` | every `*.B3D` in `DIR` -> `OUTDIR/<NAME>/` |
| `baf FILE.BAF OUTDIR` | one actor or object -> `OUTDIR/<name>/` |
| `bafall DIR OUTDIR` | every `*.BAF` in `DIR` -> `OUTDIR/<name>/` |
| `wv FILE.WV OUTDIR` | one voice file -> `OUTDIR/<file>.wav` |
| `imc FILE.IMC OUTDIR` | one music file -> `OUTDIR/<file>.wav` and its map `OUTDIR/<file>.imap.txt` |
| `music DIR OUTDIR` | every `*.IMC` in `DIR`, as `imc` does |
| `imuse IMUSE.DLL OUTDIR` | the music tables of the game's `IMUSE.DLL` (which music id plays which file) -> `OUTDIR/imuse_tables.txt` |

`<NAME>` and `<name>` are the file name up to its first `.`, in the letter case you typed for `b3d`
and `baf`. `b3dall` and the disc run name level folders in upper case, as the original's level
list did; `bafall` and the disc run keep each BAF file's case. A level's OBJ and MTL files keep
only the ASCII letters, digits, `_` and `-` of `<NAME>` (see **Level geometry** under
[What you get](#what-you-get)). `<file>` is a sound file's name without its extension, in its own
case (as typed for `wv` and `imc`). `OUTDIR` is created with its parents. Examples:

```
tpmextractor z H:/GAMEDATA/GOBS/BIG.Z work
tpmextractor lab work/big.lab work/biglab
tpmextractor bafall work/biglab work/BAF
tpmextractor --palette mylevel.cmp baf work/biglab/someactor.baf work/BAF
tpmextractor --keep-decompressed b3dall H:/GAMEDATA/LEVEL work/LEVEL
tpmextractor lab H:/GAMEDATA/GOBS/VOICE.LAB work/voicelab
tpmextractor music "C:/Games/TPM/MUSIC" work/music
tpmextractor imuse "C:/Games/TPM/IMUSE.DLL" work
```

Exit status: 0 success (warnings allowed), 1 some files could not be processed, 2 usage error or
fatal error. Errors name the file and, where known, the offset; the run goes on with the next
file. The tool waits for a key before exiting only with `--pause` (the original always waited).

### Options

```
--legacy               reproduce the original TPMExtractor output byte for byte
--scale F              scale for 3DO/KEY and level OBJ positions (default 0.2)
--[no-]draw-scale      also multiply positions by the BAF's own draw scale
                       (header +0xAC), as the game does (default on); a 3DO or
                       KEY drawn at a scale other than 1 is named *_s<hundredths>
--angles M             raw (default: TPM stores degrees) or rad2deg (never right
                       for TPM data; for experiments)
--insert-offset M      auto (default: re-root at hierarchy node 0 when it lies above
                       the origin, then the lowest point on the floor; people and
                       creatures then stand on the floor in JK when their template
                       has size= at most the INSERT OFFSET z - 0.005), floor (the
                       lowest point; props), node0, zero, legacy, or X,Y,Z
--radius M             animated (default: the rest pose and every key frame of
                       every model), computed (the rest pose, and every key frame
                       of a model that auto or node0 re-roots), stored (TPM's
                       own), legacy (0.1), or a value
--[no-]mat8            retail 8-bit MATs, used by the 3DOs (default on)
--[no-]mat16           16-bit MATs for OpenJKDF2/MotS testing (default on)
--[no-]bmp             BMP previews (default on)
--palette FILE         target palette (.cmp or 768-byte .pal) for the 8-bit MATs
                       and for BAFs that have no colormap
--[no-]debug-dumps     *.3do.txt, *.key.txt and REMAIN.HEX (default on)
--geometry M           obj (default: each level's geometry as <NAME>.obj and
                       <NAME>.mtl next to its textures) or none
--geometry-texture-rule R
                       game (default: the texture the game draws each polygon
                       with) or mat (always its mapping's texture)
--sound M              wav (default: each voice file of a LAB also decoded to
                       <name>.wav next to it, and the disc's music folder
                       decoded into DumpDir/music/) or none
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
DumpDir/voicelab/<entry>        every file of VOICE.LAB (the voice files <file>.WV)
DumpDir/voicelab/<file>.wav     each voice file decoded (default mode)
DumpDir/music/<file>.wav        each music file of the disc's music folder decoded (default
DumpDir/music/<file>.imap.txt   mode, when the disc has the folder), with its map
DumpDir/LEVEL/<NAME>/<i>.bmp    the level's textures, in their own palettes
DumpDir/LEVEL/<NAME>/<NAME>.obj the level's geometry (default mode), with its materials in
DumpDir/LEVEL/<NAME>/<NAME>.mtl <NAME>.mtl, which name the <i>.bmp files
DumpDir/BAF/<name>/             one folder per BAF:
    <colormap>.cmp              the BAF's colormap (default mode: a retail JK CMP; a BAF
                                without one gets <name>.cmp, the palette its MATs index)
    <mat>.mat                   raw copy of each material as stored in the BAF (default
                                mode: with the palette a material of its own carries)
    <base>.bmp                  preview of the material's first texture
    <base>8.mat                 retail JK 8-bit MAT (default mode; the 3DO uses it)
    <base>16.mat                16-bit MAT (OpenJKDF2 and Mysteries of the Sith only)
    <model>.3do                 the model
    <key>.key                   each animation
    *.3do.txt, *.key.txt        debug dumps of the parsed structures
    REMAIN.HEX                  the bytes after the last parsed structure
```

In default mode a BAF drawn at a scale other than 1 writes its model and keys as
`<model>_s<hundredths>.3do` and `<key>_s<hundredths>.key` (for example `walk_s110.key` at 1.1):
the positions in those files include that scale, and other BAFs use the same model or key names
unscaled or at another scale. The tool prints which BAFs this applies to. Debug dumps keep the
BAF's own names.

**Using the output in retail Jedi Knight.** JK 1.0/1.01 draws 8-bit MATs through the colormap of
the level, so a MAT shows its true colours only with the palette its indices refer to. By default
each BAF's 8-bit MATs index that BAF's own colormap, which is written next to them. To use models
in an existing JK level, give that level's colormap (from your own copy of JK) with
`--palette level.cmp`: every 8-bit MAT is then remapped into it, avoiding its glowing colours.
The 16-bit MATs load in OpenJKDF2 and in Mysteries of the Sith, but retail JK cannot be relied on
to draw them; use the `<base>8.mat` files there.

**Standing on the floor.** JK never draws a model shifted by its `INSERT OFFSET`; it only places
things with it. A thing with physics that stands on a floor (actors, players, anything with
gravity) keeps its origin `max(INSERT OFFSET z, size + 0.005)` above the floor, where `size` is
the template's `size=` (without `size=`, the 3DO's `RADIUS`; `movesize=` does not count for actors
and players), unless the template sets `height=`. TPM's people and creatures stand on their
origin, so with `--insert-offset floor` (right for props) they would hover in JK, by about their
own height when the template has no `size=`. The default, `--insert-offset auto`, re-roots every
model whose hierarchy node 0 lies above its origin at node 0 (on TPM's people, the pelvis or a
root node at about hip height), as `--insert-offset node0` does, and then puts the lowest point on
the floor, as `floor` does: a person then hangs node 0's height below its origin, and `INSERT
OFFSET` z is that height. Give the template a `size=` of at most `INSERT OFFSET z - 0.005`; with a
larger `size=`, or none, it still floats. `auto` re-roots by node 0's height rounded to 1/1000 of
a model unit, so BAFs that share a key still write identical keys (`node0` re-roots by the exact
height). On a retail disc `auto` re-roots 250 of the 303 models; the others have node 0 at or
below their origin and get exactly the `floor` output. By default every model's `RADIUS` also
covers every frame of its keys (`--radius animated`), since JK culls the whole model with that
sphere around its origin; with `--radius computed` only re-rooted models' do.

**Several actors in one game.** JK keeps one model, key, MAT and colormap per file name, so two
different files with the same name cannot be used side by side: the first one loaded wins.
Default mode never gives the same name to files that differ only because of the fork (the draw
scale above). TPM itself, however, stores different data under some names in different BAFs: on a
retail disc, different BAFs write 64 key, 9 model, 251 raw MAT and 2 colormap names with
different contents (as the original tool did), and 260 8-bit MAT names (191 with `--palette`,
which maps every MAT into one palette). When you install actors from several BAFs together,
rename such files, and the references to them in 3DOs, PUPs and templates.

**Level geometry.** In default mode each level's polygons are written as `<NAME>.obj`, with its
materials in `<NAME>.mtl`, next to the level's textures (`--geometry none` turns this off;
`--legacy` never writes them). The original program never decoded the geometry; its format was
recovered for the fork by static analysis of the game's level loader and checked on every level of
a retail disc.

- **Axes and scale.** JK's axes: +X right, +Y forward, +Z up (in Blender's OBJ import, set the up
  axis to Z and the forward axis to Y). Positions are multiplied by `--scale`, 0.2 by default as
  for the models: one cell of the level's grid is one TPM unit and becomes 0.2 JK units.
- **Vertices and faces.** Every vertex of the level is written once, in its stored order, without
  welding. Each polygon is one face, a triangle or a quad (quads are not split), wound
  counter-clockwise around the side the game draws, with that side's normal and texture
  coordinates. Texture coordinates may run past 0..1 where the game repeats a texture.
- **Materials.** Each texture is one material, `tex<i>`, drawn with `<i>.bmp`. With
  `--geometry-texture-rule game` (the default) each polygon gets the texture the game draws it
  with; `mat` gives it the texture its UV mapping names. The two differ on a few polygons (88 on a
  retail disc, some of which look like mistakes in the game's data). A polygon whose UV mapping or
  texture cannot be used (none on a retail disc) gets the grey material `untextured`, and the tool
  says so.
- **Objects.** Faces are grouped as `static`, then `amap<k>`, the polygons the level's moving
  object `k` (a door, a lift) moves, at their stored rest position, then `hidden`, polygons the
  game keeps hidden or switched off.
- **File names.** The OBJ and MTL files take the level's `<NAME>` with every character other than
  an ASCII letter, digit, `_` or `-` replaced by `_` (one `_` for each character, also a non-ASCII
  one), or `level` if nothing else is left. OBJ readers split the OBJ's `mtllib` line at spaces,
  and the files are plain ASCII. So `b3d "my level.b3d" out` writes `out/my level/my_level.obj`
  and `my_level.mtl`, and says so; retail level names are unchanged. The letter case follows
  `<NAME>`: `b3d lower.b3d out` writes `lower.obj`, while `b3dall` and the disc run write upper
  case.
- **Not in the OBJ:** the back side of two-sided polygons, translucency, texture animation,
  lighting, collision and the motion of moving objects.

### Sound

In default mode the voice lines and the music are decoded to WAV files: canonical RIFF/WAVE files
of 16-bit PCM with nothing between the `fmt ` and `data` chunks, which is what retail JK's loader
reads. `--sound none` turns this off; `--legacy` never decodes. The original program left the
voice files as they were stored and never read the music. The formats were recovered for the fork
by static analysis of the game's code, and the decoder gives the same samples as an independent
decoder on every voice file and music track of a retail copy.

- **Voice files** (`VOICE.LAB`, entries `*.WV`): a small header (`INDYWV`: sample rate, 16 bits,
  channels, sizes) and one coded stream. `lab` and the disc run write `<file>.wav` next to each
  `<file>.WV`, at the rate and channel count of its header (22,050 Hz mono on a retail disc); `wv`
  decodes a single file. A voice file whose `.wav` name is another entry of the archive is not
  decoded (with a warning).
- **Music** (`*.IMC`, in the disc's or installation's `MUSIC` or `GAMEDATA/MUSIC` folder, or
  wherever you point `music` or `imc`): an `MCMP` container of blocks; the first holds the track's
  map (`iMUS`), the others the coded audio. `<file>.wav` is the decoded audio exactly as stored,
  at the rate and channel count the map's format event gives (22,050 Hz stereo on a retail disc).
  The game does not simply play the file from start to end: it plays regions and follows the map's
  jumps. `<file>.imap.txt` lists the map's events, one per line, tab-separated, positions given as
  frames of the WAV (a frame is one sample of each channel):

  ```
  # iMUSE map of <file>.IMC, written by TPMExtractor (fork)
  # Positions are frames of the audio: ...
  format  frames=<n>  rate=<n>  channels=<n>  bits=16  frame_bytes=<n>  header_bytes=<n>
  # event  frame  values
  FRMT    <frame>  flag=<n>  bits=<n>  rate=<n>  channels=<n>
  TEXT    <frame>  text="<marker>"
  REGN    <frame>  frames=<length>
  JUMP    <frame>  dest=<frame>  hook=<n>  fade_ms=<n>
  SYNC    <frame>  bytes=<n>
  STOP    <frame>
  ```

  A looping track has a `JUMP` back to its loop start (the marker `lp`); when the game takes it, it
  crossfades the audio after the jump point into the loop over `fade_ms`. A one-shot cue has an
  `_end` marker where the game's music script hands back to the area's music. Texts are quoted;
  bytes other than printable ASCII, `"` and `\` are written as `\xNN`.
- **Music tables** (`imuse`): the game picks music by number. Its `IMUSE.DLL` holds a table of
  states (an area's music), a table of sequences (cues) and a table of files. `imuse` finds the
  three tables in the DLL file by their structure (it never loads or runs the DLL) and writes them
  to `imuse_tables.txt`: per state and sequence its id, transition, attribute, hook, fade, key and
  symbol, and the file the engine plays for it; then the file table. The engine looks keys up byte
  for byte, so a key that matches a file's key only ignoring case plays nothing (one record on a
  retail DLL); such records show `-`. A DLL whose tables do not have the retail record counts is
  refused.
- **Checks.** Beyond decoding, the tool checks what the formats promise and refuses a file, with
  its offset, when a check fails: block sizes that add up to the file, the decoded audio exactly as
  long as the map says, map events in order and on whole frames, jumps that land on a region start
  and on the loop point, and codes that fit in their blocks. A damaged file writes nothing.

**Decoded audio is for your own use.** Like everything the tool extracts, it is the game's data,
from your copy of the game: keep it on your computer, and do not share it, upload it or commit it
anywhere.

## What `--legacy` does

`--legacy` reproduces the output of the original program (commit `2cc75d6`) byte for byte: the
same files, the same names and the same bytes, including its quirks (placeholder 3DO values,
unpadded BMP rows, the legacy 17,216-byte `.cmp`, the misread colormap-less BAF). It refuses every
option that would change the output, and the sound commands (`wv`, `imc`, `music`, `imuse`), which
the original did not have; it never decodes sound. Use it when you need exactly what the original
wrote, or to check that nothing changed.

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
  - computed 3DO `RADIUS` values (JK's rules) instead of the placeholder 0.1, covering every
    animation frame of the model (`--radius animated`, the default; `--radius computed` covers
    the rest pose, and the keys only of re-rooted models);
  - a re-root at hierarchy node 0 for models whose node 0 lies above their origin, then the lowest
    point on the floor (`INSERT OFFSET`), so that people and creatures stand on the floor in JK
    (`--insert-offset auto`, the default; see "Standing on the floor");
  - positions multiplied by each BAF's own draw scale, as the game draws it, with the scale in
    the names of the models and keys it changes (`--no-draw-scale` turns this off);
  - KEY files whose `JOINTS` covers every node of the model, which JK's animation code expects;
  - for the BAF without a colormap, a colormap with the palette its 8-bit MATs index;
  - the real hierarchy node types (so partial-body animations blend as authored), perspective
    texture mode (`TEXTUREMODE 3`), and face flags limited to the ones JK uses;
  - retail JK colormaps (version 30, with light and transparency tables, 82,752 bytes) instead of
    the original's 17,216-byte file with an empty light table;
  - retail 8-bit MATs `<base>8.mat` (with mipmaps, index 0 kept as the transparent colour only
    where the texture is masked), which the 3DOs now name.
- **Levels:** each level's geometry, which the original never decoded, is written as an OBJ model
  with its materials (`--geometry`, see "Level geometry"); the level decompressor works in memory,
  reports truncated files, and produces the same bytes as before; the level file is walked section
  by section with every size checked; `--keep-decompressed` keeps the decompressed `<NAME>.dec`.
- **Archives:** LAB files are read through their stored offsets (the original assumed sequential
  storage); `.Z` files are decompressed with a size check, and a damaged one no longer ends the run.
- **Sound:** the voice files and the music, which the original left coded or never read, are
  decoded to WAV, with each music file's map as text and the game's music tables (`--sound`, the
  commands `wv`, `imc`, `music` and `imuse`; see "Sound").
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
- **Draw scale ignored.** Each BAF stores the scale the game draws it at; 54 retail BAFs are drawn
  at a scale other than 1 (0.5 to 3.0). Another 80 store 1.0 a few millionths too high.
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
uninitialised "tail" lines masked) adds the `--legacy` identity test. For sound, set any of
`TPM_VOICE_LAB` (your `VOICE.LAB`), `TPM_MUSIC_DIR` (your music folder) and `TPM_IMUSE_DLL` (your
`IMUSE.DLL`); with `TPM_WV_MANIFEST` and `TPM_IMC_MANIFEST` (tab-separated manifests of the decoded
PCM's SHA-256, kept outside the repository) every voice file and track is compared with them.
The synthetic sound tests compare the tool with an independent model of the formats
(`tests/make_fixtures.py`) on files made by a test encoder, including damaged copies.

Fuzz targets (CMake with `-DTPMX_FUZZ=ON`, best with clang and `-DTPMX_SANITIZE=ON`) cover every
input the tool reads: BAF files through all their writers in both modes, levels and their
geometry, LAB and `.Z` archives, colormaps, voice files, music files and the music tables. The
file-level ones fail when anything is written outside their output folder. `ctest -L fuzz` runs
each one briefly on generated seeds; `tests/CMakeLists.txt` shows how to run them for longer.

## License

Apache License 2.0, like the original (see `LICENSE` and `NOTICE`). `unshieldv3/` is Wolfgang
Frisch's unshieldv3 (Apache 2.0, its own `LICENSE`), and `unshieldv3/blast.c`/`blast.h` are Mark
Adler's blast (zlib license). Files changed by the fork say so at the top.


## Credits
- KnightCop - Project lead
- Strike [@bahstrike](https://github.com/bahstrike) - Code, File analysis
- shiny [@shinyquagsire23](https://github.com/shinyquagsire23) - .B3D decompression
- Wolfgang Frisch [@wfr](https://github.com/wfr) - .Z decompression

### Fork credits
- Andrei Kiralv/Darkahn, with Claude (Anthropic), Red in Andrei's projects - fork improvements
  (2026)

### Disclaimer
This project is in no way affiliated with LEC or Disney. Use at your own peril.
