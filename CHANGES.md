# Changes in this fork

This is a fork of [bahstrike/TPMExtractor](https://github.com/bahstrike/TPMExtractor) at commit
`2cc75d6`. The original credits are unchanged:

- KnightCop - Project lead
- Strike [@bahstrike](https://github.com/bahstrike) - Code, File analysis
- shiny [@shinyquagsire23](https://github.com/shinyquagsire23) - .B3D decompression
- Wolfgang Frisch [@wfr](https://github.com/wfr) - .Z decompression

The fork is licensed under Apache-2.0 like the original (see `LICENSE` and `NOTICE`).
`unshieldv3/` keeps its own LICENSE; `unshieldv3/blast.c` and `blast.h` are Mark Adler's
zlib-licensed code. Every file changed by the fork carries a "Modified 2026 in the TPMExtractor
fork" notice near the top; new files say so in their header.

"Default mode" below means running without `--legacy`.

## Unreleased (0.1.0-dev)

### Compatibility

- `tpmextractor DiscDir DumpDir` works as before.
- `--legacy` reproduces the original program's output byte for byte: the same files, names and
  bytes. Checked on a retail disc against the original sources built unchanged (as a 32-bit
  program behind a Windows shim): all 15,027 output files and 320 folders match, with builds by
  g++ (64-bit and 32-bit static), clang and the sanitizer builds. `--legacy` refuses every option
  that would change the output.
- One difference in both modes: the debug files `*.3do.txt` and `*.key.txt` end with a
  "TAIL 64bytes" section. When that read hits the end of the BAF (153 files on the retail disc),
  the original printed uninitialised memory there, different on every run. The fork prints zeros.
- Console output is not part of the compatibility promise: progress lines moved to `-v`, and
  warnings and errors go to stderr.

### Command line

- Commands `all`, `z`, `lab`, `b3d`, `b3dall`, `baf`, `bafall` (`tpmextractor DiscDir DumpDir` is
  `all`). Each command's `OUTDIR` is created with its parents.
- Exit status: 0 success (warnings allowed), 1 some files could not be processed, 2 usage error or
  fatal error. Usage errors print a short synopsis to stderr; `--help` prints everything.
- Options may appear anywhere; `--opt value` and `--opt=value` both work; `--` ends the options.
- Default mode processes every `GAMEDATA/LEVEL/*.B3D`, found ignoring case, into `LEVEL/<NAME>` in
  upper case. `--legacy` keeps the original's fixed list of 11 levels.
- The tool waits for a key before exiting only with `--pause`; the original always waited, which
  blocked scripts.
- A missing LAB file or level is a warning (the original printed "Couldn't open" for a missing LAB
  and crashed on a missing level). A damaged `BIG.Z` skips `biglab/` and `BAF/` instead of ending
  the run; a missing `BIG.Z` is still fatal.
- Default mode creates the dump folder's parents; `--legacy` creates only its last level, as the
  original did.
- Arguments beyond `DiscDir DumpDir` (or `COMMAND INPUT OUTDIR`) are a usage error; the original
  ignored extra arguments.

Options (default-mode values; `--legacy` pins each output-affecting one to the original's
behaviour and refuses it on the command line):

| Option | Default | Effect |
|---|---|---|
| `--scale F` | 0.2 | scale for 3DO/KEY positions (Strike's world scale) |
| `--[no-]draw-scale` | on | also multiply positions by the BAF's own draw scale (header +0xAC) |
| `--angles raw\|rad2deg` | raw | TPM stores degrees with JK's pitch/yaw/roll; `rad2deg` (the original's disabled experiment) is never right for TPM data and warns |
| `--insert-offset M` | floor | `floor` (props), `node0` (re-root actors), `zero`, `legacy` (0,0,0.1), or `X,Y,Z` |
| `--radius M` | computed | `computed` (rest pose), `animated` (also every key frame), `stored` (TPM's own), `legacy` (0.1), or a value |
| `--[no-]mat8` | on | retail 8-bit MATs `<base>8.mat`, named by the 3DOs |
| `--[no-]mat16` | on | 16-bit MATs `<base>16.mat` (OpenJKDF2 and MotS only) |
| `--[no-]bmp` | on | BMP previews |
| `--palette FILE` | none | `.cmp` or 768-byte `.pal`: the palette of colormap-less BAFs, and the palette every 8-bit MAT is remapped into |
| `--[no-]debug-dumps` | on | `*.3do.txt`, `*.key.txt`, `REMAIN.HEX` |
| `--keep-decompressed` | off | keep `big.lab` and `LEVEL/<NAME>/<NAME>.dec` |
| `--[no-]pause` | off | wait for a key before exiting |
| `-v`, `-vv`, `-q` | | verbose, debug, quiet |

### BAF output in default mode

- **Parser.** The real BAF layout is read from memory with bounds checks: `has_colormap` is the
  u16 at 0xCC (byte 0xCF is an independent flag), every BAF has exactly one model, per-face UV
  indices are read only for textured materials, the two per-vertex float arrays are read as two
  arrays, and the keys are followed by 96-byte per-key sound-event records. The size and offset
  fields of the header (model size, key size, model and key offsets, allocation size, materials
  size) are validated; a mismatch is a warning with file and offset. Every count and index is
  bounded (an out-of-range face material index is written as -1, JK's "no material"). A
  structural error is reported with file and offset and stops that file; what was parsed before
  it is still written.
- **The colormap-less BAF** (one on the retail disc) gets its model: the original read its model
  as a key and wrote a key-format `.3do`.
- **RADIUS** is computed with JK's rules (OpenJKDF2 `rdModel3_CalcRadii`): mesh radius = 1.1 x the
  farthest vertex from the mesh origin; model radius = 1.1 x the farthest vertex from the model
  origin after the rest-pose hierarchy transform (`World(parent) T(-parent.pivot) T(pos) R T(pivot)`),
  after scaling. `--radius animated` also covers every integer frame of every key of the BAF
  (animation moves vertices past the rest-pose radius in about 40% of the retail actors).
  `--radius stored` uses TPM's model radius and warns when it is smaller than the model (115 retail
  BAFs); mesh radii are then still computed, since TPM's mesh radius is centred elsewhere.
- **INSERT OFFSET**: `floor` puts the lowest rest-pose point on the floor (right for props).
  `node0` re-roots the model at hierarchy node 0: node 0's z moves to 0 in the hierarchy and in
  node 0's KEY entries, and INSERT OFFSET becomes (0, 0, node0.z x scale), so actors stand on the
  floor in JK instead of hovering.
- **Draw scale**: positions (vertices, hierarchy positions and pivots, KEY positions and position
  deltas) are multiplied by `--scale` x the BAF's draw scale, as the game draws the actor. This
  changes 134 retail BAFs, 54 of them by more than 1e-4 (`--no-draw-scale` turns it off).
- **3DO values**: the real hierarchy node types (+0x48; partial-body animations then blend as
  authored), `TEXTUREMODE 3` (perspective) for meshes and faces, face flags masked to 0x3 (the
  bits retail JK uses), and `RADIUS %f` / `INSERT OFFSET %f %f %f` without the `# dunno` comments.
- **CMPs**: the BAF's colormap is written as a retail JK CMP (version 30, flags 1, tint 1.0, a
  64-level light table that never brightens a colour as light falls, a transparency table blending
  about 35% source and 65% destination, fitted to the game's own default colormap; 82,752 bytes)
  instead of the original's 17,216-byte version-20 file with an empty light table.
- **8-bit MATs** `<base>8.mat`: retail JK MATs (type 2, 8 bits per pixel), one cel per texture,
  mipmaps generated up to 4 levels while both sides stay at least 8 texels, a clean format block.
  A texture is masked (index 0 transparent) when its transparency value is non-zero; in an
  unmasked texture, texels using index 0 move to their nearest other colour, so they never turn
  transparent. Materials with their own palette are remapped into the BAF's colormap; with
  `--palette`, every material is remapped into that palette, avoiding its glowing colours. The 3DO
  `MATERIALS` list names the 8-bit MATs (with `--no-mat8`, the 16-bit ones as before; with
  `--no-mat16` as well, the tool warns that those files are not written). A name clash with a
  raw MAT gets `<base>8_<n>.mat` and a warning (none on the retail disc).
- **Angles** stay raw: TPM stores degrees with JK's rotation order (verified against the game's
  matrix code and the data), so no conversion is needed.
- **Debug dumps** (`.3do.txt`, `.key.txt`, `REMAIN.HEX`) keep the original's text format; for every
  BAF except the colormap-less one they are byte-identical to `--legacy`.
- BMP rows are padded to 4 bytes, as the BMP format requires. No retail texture is affected.
- Unchanged: raw `.mat` copies (materials with their own palette still omit it, as stored in the
  BAF), the BMP previews' colours, the 16-bit MATs, KEY text (apart from the draw scale and the
  `node0` re-root).

### BAF output in `--legacy`

- The original sequential read is kept, now from memory with every count bounded: a negative or
  impossible count is an error with file and offset instead of a crash, a hang or an unbounded
  write, and mipmap loops stop at an empty level. Output for valid files is unchanged.
- The legacy `.cmp` is written by the fork's CMP writer (`jk::legacy_cmp`), byte-identical to
  Strike's `CCMP` class on all 302 retail colormaps, so `CMP.h`/`CMP.cpp` were removed.

### Levels (.B3D)

- The decompressor works in memory with bounds checks, reproduces the original `.dec` byte for
  byte (truncated files included), reports truncated input, and refuses images over 128 MiB.
- Default mode walks the B3D section chain (including the two editor sections whose size fields
  are wrong), checks every section, texture size and palette index against the file, reads
  uncompressed levels such as kept `.dec` files as they are (the compressed flag decides, as in
  the game), salvages the palettes and textures by tag search when the chain is broken (with a
  warning), and reports errors with file and offset instead of crashing.
- `--legacy` keeps the original's tag search, texture loop, and write-then-delete of the `.dec`;
  `--keep-decompressed` (default mode) keeps `<NAME>.dec`.
- Level files embed no other files: `DFLT.CMP`, `DFLT.MAT` and the BMP, TXT and SPR files are
  `big.lab` entries, written to `biglab/` in both modes.
- `b3d.h` exposes the section walk, named header fields and level loading for a future geometry
  decoder.

### Archives (.LAB, .Z)

- LAB: rewritten on a bounds-checked reader. The header, entry table, name table and every data
  range are checked against limits and the file size; an error names the file, offset and entry
  and skips only that entry. Default mode reads the stored name and data offsets; `--legacy`
  reads sequentially as the original did (identical on the retail disc; a warning when the two
  disagree). Duplicate names give a warning. Data is copied in 64 KiB chunks.
- `.Z`: decompressed by streaming `blast()` with each input byte supplied once (upstream
  unshieldv3 handed `blast()` the same buffer again on a truncated file and decoded garbage);
  the output must equal the size in the table of contents, and a file that fails is removed and
  reported. Split files are skipped with an error.
- `unshieldv3/ISArchiveV3.cpp`: the three header `assert`s, short reads and table entries shorter
  than their fields are thrown errors with offsets (marked "(fork)"). With the asserts gone, the
  build no longer forces `-UNDEBUG` on the vendored code.

### Robustness

- Errors name the file (and the offset where known); the run goes on with the next file.
- Every output name taken from data goes through one check: absolute paths, drive letters, empty,
  `.` or `..` components, control characters, characters Windows does not allow, trailing dots or
  spaces, Windows device names and over-long components are refused.
- `unshieldv3/blast.c`: the window copy no longer computes a pointer before the start of its
  buffer (undefined behaviour in C). Output unchanged.
- Sanitizers: the tool and tests run clean under AddressSanitizer and UndefinedBehaviorSanitizer
  (g++ and clang), on the synthetic suite, the whole retail disc in both modes, and damaged inputs.

### Portability

- Builds and runs on Linux (g++, clang) and builds for Windows (Visual Studio project, or
  MinGW-w64 32/64-bit, statically linked: the `.exe` needs only KERNEL32 and msvcrt). All
  operating-system calls are in `platform.cpp`.
- On-disk structures use fixed-width types, so 64-bit builds give the same output as the 32-bit
  original. (The original's structures assume 32-bit Windows: a 64-bit build of it misreads the
  keyframes, and on 64-bit Linux or macOS also the MAT and colormap headers.)
- Text files are still written with CRLF line endings on every system, as the original did on
  Windows. Numbers are formatted by the fork itself (`%f` correctly rounded, ties to even, as the
  Windows 10 2004+ C runtime and glibc do), so the output does not depend on the C library.
- Files on the disc are found whatever their case. Output names are the same as before.
- Float results are the same on every compiler and CPU: builds disable floating-point contraction
  and use SSE2 on 32-bit x86.

### Building

- CMake: `cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release`
  (`-DTPMX_SANITIZE=ON` for ASan + UBSan, `-DTPMX_WERROR=ON`, `-DTPMX_FUZZ=ON` for the fuzz
  targets, `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake` or `toolchain-mingw32.cmake`
  for Windows).
- Without CMake: `make` (`make check`, `make tests`, `make SANITIZE=1`, `make WERROR=1`,
  `make CXX=x86_64-w64-mingw32-g++ CC=x86_64-w64-mingw32-gcc` for Windows).
- Visual Studio: `TPMExtractor.sln` as before; the project lists the fork's source files (not
  built by the fork's maintainers, who had no Visual Studio).

### Tests

- `tests/run_tests.py` (Python 3, standard library only) runs the tool on synthetic fixtures from
  `tests/make_fixtures.py` (boxes, pyramids, generic rigs; generated pixels and palettes) in
  `--legacy` and default mode. It compares `--legacy` output byte for byte with an independent
  model of the original (and with a build of the original when one is given), checks default mode
  against its documented changes (retail CMPs, 8-bit MATs texel by texel, RADIUS and INSERT OFFSET
  recomputed from the written 3DO, every option), checks the commands, the level decoder against
  an independent decoder, damaged and hostile inputs, and every run for crashes, hangs and
  sanitizer reports.
- With `TPM_DISC` set, real-data tests run on your own disc: a default-mode run of the whole disc
  checked as above and against `--legacy`, and (with `TPM_REF_MANIFEST`, a manifest of the
  original's output with the uninitialised tail lines masked) the `--legacy` identity test. Dumps
  go outside the source tree and are deleted.
- Unit tests: `unit_layer` (platform, BMP, reader, options), `unit_textout` (number formatting),
  `unit_jkout` (MAT and CMP encoders), `unit_b3d_decompress` (the level decoder).
- Fuzz targets `tests/fuzz_*.cpp` (LAB, B3D, CMP parsers) with libFuzzer (clang) or a replay
  driver (`-DTPMX_FUZZ=ON`).
- `make check` and `ctest` run everything; real data is never part of the repository.

### Files

| File | Change |
|---|---|
| `main.cpp` | options, `--legacy`, commands, level scan, exit codes; same steps and output names as before |
| `baf.*` | real BAF layout in default mode, JK-ready 3DO/KEY values, retail CMPs and 8-bit MATs; bounded legacy read |
| `b3d.*`, `b3d_decompress.*` | in-memory decoder with limits, section walk, checked textures |
| `lab.*` | bounds-checked LAB reader, stored or sequential layout |
| `CMP.h`, `CMP.cpp` | removed (replaced by `jk::legacy_cmp`, byte-identical) |
| `unshieldv3/ISArchiveV3.cpp`, `blast.c`, `unshieldv3.cpp` | see Archives and Robustness |
| `TPMExtractor.vcxproj`, `.filters` | the fork's source files |
| `README.md` | rewritten for the fork; original text, credits and disclaimer kept |
| new: `platform.*`, `textout.*`, `bmp.*`, `image.*`, `binreader.*`, `log.*`, `options.*` | portability layer, output writers, options |
| new: `zextract.*` | `.Z` extraction |
| new: `jkout.*` | retail JK MAT and CMP writers, nearest-colour remapping, mipmaps |
| new: `tests/` | unit tests, end-to-end runner, fixture generator, fuzz targets |
| new: `CMakeLists.txt`, `Makefile`, `cmake/*.cmake`, `.gitignore`, `CHANGES.md`, `NOTICE` | build files and documentation |
