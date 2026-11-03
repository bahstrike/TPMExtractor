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
| `--scale F` | 0.2 | scale for 3DO/KEY positions (Strike's world scale) and for level OBJ positions |
| `--[no-]draw-scale` | on | also multiply positions by the BAF's own draw scale (header +0xAC, in hundredths); a 3DO or KEY drawn at a scale other than 1 is named `<name>_s<hundredths>` |
| `--angles raw\|rad2deg` | raw | TPM stores degrees with JK's pitch/yaw/roll; `rad2deg` (the original's disabled experiment) is never right for TPM data and warns |
| `--insert-offset M` | auto | `auto` (re-root at node 0 when node 0 lies above the origin, then `floor`), `floor` (the lowest point on the floor; props), `node0` (re-root at node 0), `zero`, `legacy` (0,0,0.1), or `X,Y,Z` |
| `--radius M` | animated | `animated` (rest pose and every key frame of every model), `computed` (rest pose; for a model `auto` or `node0` re-roots, also every key frame), `stored` (TPM's own), `legacy` (0.1), or a value |
| `--[no-]mat8` | on | retail 8-bit MATs `<base>8.mat`, named by the 3DOs |
| `--[no-]mat16` | on | 16-bit MATs `<base>16.mat` (OpenJKDF2 and MotS only) |
| `--[no-]bmp` | on | BMP previews |
| `--palette FILE` | none | `.cmp` or 768-byte `.pal`: the palette of colormap-less BAFs, and the palette every 8-bit MAT is remapped into |
| `--[no-]debug-dumps` | on | `*.3do.txt`, `*.key.txt`, `REMAIN.HEX` |
| `--geometry obj\|none` | obj | each level's geometry as `<NAME>.obj` and `<NAME>.mtl` next to its textures (see Levels) |
| `--geometry-texture-rule game\|mat` | game | the texture of each OBJ face: `game` the one the game draws the polygon with, `mat` always its UV mapping's |
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
  as a key and wrote a key-format `.3do`. It also gets a retail CMP with the palette its 8-bit
  MATs index (the palette of its first material that has its own), named after the BAF
  (`<BAF name>.cmp`, or `<BAF name>_<n>.cmp` if a material, model or key of the BAF has that
  name), so it comes with the colormap it needs like every other BAF. Not with `--palette`,
  whose palette the MATs index instead.
- **RADIUS** is computed with JK's rules (OpenJKDF2 `rdModel3_CalcRadii`): mesh radius = 1.1 x the
  farthest vertex from the mesh origin; model radius = 1.1 x the farthest vertex from the model
  origin after the rest-pose hierarchy transform (`World(parent) T(-parent.pivot) T(pos) R T(pivot)`),
  after scaling. `--radius animated` also covers every integer frame of every key of the BAF
  (animation moves vertices past the rest-pose radius in about 40% of the retail actors). JK
  culls the whole model with this sphere around its origin and never recomputes it, so with
  `--radius computed` a model that `auto` or `node0` re-roots also gets the key-covering radius:
  its origin is then node 0, and the keys reach past the rest-pose radius around it in 164 of the
  303 retail BAFs. `--radius animated` is the default (decided 2 Nov 2026): with `computed`, the
  RADIUS of 294 of the 303 retail models covered every frame of their keys, and 9 models that are
  not re-rooted were outgrown by theirs (4 by more than 10%). The trade-off: a template without
  `size=` also collides with the larger RADIUS, so templates should set `size=` (they need it to
  stand on the floor anyway).
  `--radius stored` uses TPM's model radius and warns when it is smaller than the model (115 retail
  BAFs); mesh radii are then still computed, since TPM's mesh radius is centred elsewhere.
- **INSERT OFFSET**: `floor` puts the lowest rest-pose point on the floor (right for props).
  `node0` re-roots the model at hierarchy node 0: node 0's z moves to 0 in the hierarchy and in
  node 0's KEY entries, and INSERT OFFSET becomes (0, 0, node0.z x scale), so actors stand on the
  floor in JK instead of hovering. Retail JK (code decompiled by OpenJKDF2) never draws a model
  shifted by its INSERT OFFSET: a thing with physics standing on a floor keeps its origin
  max(INSERT OFFSET z, size + 0.005) above it, `size` being the template's `size=` (the 3DO's
  RADIUS without one). TPM's people and creatures stand on their origin, so with `floor` they
  hover (by about a fifth of their height with a body-sized `size=` on the retail disc, by more
  than their height without `size=`); with `node0` they stand on the floor when the template's
  `size=` is at most INSERT OFFSET z - 0.005. The README and `--help` say so.
  The default is `auto`: a model whose node 0 lies above z = 0 is re-rooted as with `node0`, by
  node 0's height rounded to 1/1000 of a model unit (BAFs that share keys store heights that differ
  in their last bits; the rounded height keeps their re-rooted keys byte-identical), and its
  INSERT OFFSET is then the floor of the re-rooted model; other models get exactly `floor`. With
  `--radius computed`, a re-rooted model's RADIUS covers its keys, as with `node0`. On the retail
  disc `auto` re-roots 250 of the 303 models; the 133 two-legged rigs stand on the floor with a
  body-sized `size=` (lowest point within 5% of their height of the floor in 130, none sunk into it),
  where `floor` left 132 hovering and `node0` sank one below the floor. The rounding means that
  no key or model name that several BAFs share, and that the original wrote with the same bytes,
  gets different contents.
- **Draw scale**: positions (vertices, hierarchy positions and pivots, KEY positions and position
  deltas) are multiplied by `--scale` x the BAF's draw scale, as the game draws the actor
  (`--no-draw-scale` turns it off). The draw scale is applied in hundredths: 134 retail BAFs
  store one other than exactly 1.0, but 80 of them store 1.0 about 1e-5 too high, so 54 are drawn
  at another scale (0.5 to 3.0). JK keeps one model and one key per name, and other BAFs use the
  same model and key names unscaled or at another scale, so a 3DO or KEY written at a draw scale
  other than 1 is named `<name>_s<hundredths>.<ext>` (`_s110` for 1.1) and the tool prints which
  BAFs this applies to (on the retail disc 54 3DOs and 446 KEYs). Debug dumps keep the BAF's own
  names.
- **KEY JOINTS** is at least the model's number of hierarchy nodes: JK allocates JOINTS joints
  for a key, and its animation code reads the joint at every node's index (the retail engine
  without a bounds check). `NODES` stays the number of joints written; 3 retail keys animate
  fewer nodes than their model has.
- **Names shared by several BAFs**: default mode never gives the same 3DO or KEY name to files
  that differ only because of the fork (the real-data test checks that every name the original
  wrote with the same bytes for several BAFs still has the same bytes). TPM itself stores
  different data under some names: on the retail disc, different BAFs write 64 KEY, 9 3DO, 251
  raw MAT and 2 CMP names with different contents (as the original did), and 260 8-bit MAT names
  (191 with `--palette`). The README tells users to rename them when they combine actors.
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
  raw MAT gets `<base>8_<n>.mat` and a warning (none on the retail disc). A warning names the BAF
  and its worst material when 8-bit MATs lose their colours in the palette they index (a mean RGB
  error over 48 on a scale of 0 to 441; 18 retail BAFs get it in default mode).
- **Raw MAT copies**: the copy of a material with its own palette (type 3; 435 on the retail disc)
  keeps the 768-byte palette after the pixels, where JK's loader reads it. The original left it
  out, so JK read past the end of those files; `--legacy` still leaves it out.
- **Angles** stay raw: TPM stores degrees with JK's rotation order (verified against the game's
  matrix code and the data), so no conversion is needed.
- **Debug dumps** (`.3do.txt`, `.key.txt`, `REMAIN.HEX`) keep the original's text format; for every
  BAF except the colormap-less one they are byte-identical to `--legacy`.
- BMP rows are padded to 4 bytes, as the BMP format requires. No retail texture is affected.
- Unchanged: raw `.mat` copies (except that a material with its own palette keeps it), the BMP
  previews' colours, the 16-bit MATs, KEY text (apart from the draw scale, the `node0` re-root and
  the JOINTS count).

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
- `b3d.h` exposes the section walk, named header fields and level loading.
- **Level geometry** (new; default mode, `--geometry obj`): `b3d`, `b3dall` and the disc run write
  each level's geometry as `<NAME>.obj` and `<NAME>.mtl` next to its textures. The original never
  decoded it. The decoder (`b3d_geom.cpp`) follows the fork's level-geometry specification,
  recovered by static analysis of the game's level loader and renderer (never run) and checked on
  every retail level:
  - It reads only the game part of the section chain, `B3D FILE` to `B3D_END1`, as the game does:
    `B3D_GMAP` (a grid of cells holding 64-byte polygon records, and LINK entries naming polygons
    that also cover other cells), `B3D_GVRT` (vertices), `B3D_MAT ` (UV mappings onto the
    textures), `B3D_TEX ` and `B3D_AMAP` (which polygons the moving objects move).
  - Every count, record size and index is checked against the header and the file. A level that
    fails gets no OBJ/MTL and an error naming the file and the offset in the decompressed image;
    its textures are still written. Header versions 278 to 292 are read with a warning; older
    ones (another polygon layout) and newer ones (refused by the game too) are refused.
  - One `v` per vertex, in stored order, without welding: position x `--scale`, computed as the
    exact product of the two float32 values so that every platform writes the same digits.
  - One face per polygon (triangles and quads), grouped as `static`, `amap<k>` (moved by moving
    object k, at their stored rest position) and `hidden`, in grid order within each group.
  - Faces run counter-clockwise around the side the game draws. TPM stores polygons clockwise
    around their normal, except 1,293 retail polygons, which keep their order; vertex 0 stays
    first. Each face gets that side's normal (the 12 retail polygons that store a non-finite
    normal get their facing axis) and its UVs (`vt`, with v flipped so that texture row 0 is at
    the top).
  - Materials `tex<i>` with `map_Kd <i>.bmp`, one per texture. `--geometry-texture-rule game`
    (the default) binds the texture the game binds: the polygon's own texture index when it is
    opaque, its mapping's texture when it is blended. `mat` always binds the mapping's texture.
    The two differ on 88 retail polygons.
  - A polygon whose mapping or texture index is out of range (the game skips it), whose mapping
    holds a non-finite UV, or a quad flagged as a triangle in its UV mode is written with the grey
    material `untextured`, with a warning. No retail polygon is. LINK entries are checked (each
    record must name a target; link ids must be unique), and problems are warnings.
  - Numbers have 6 decimals, correctly rounded with ties to even, never `-0.000000`; lines end in
    CRLF like the tool's other text files. A summary line per level gives the vertex, face, moving,
    hidden, kept-order and untextured counts, the faces the other texture rule would texture
    otherwise, and unresolved LINK records.
  - On the retail disc: 11 levels, 435,560 vertices and 334,396 faces, none untextured, every
    LINK record resolved. The files equal the tests' independent model byte for byte and the
    specification's reference implementation line for line.
  - `--legacy` never writes geometry and refuses the geometry options, so its output is
    unchanged.

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
  `unit_jkout` (MAT and CMP encoders), `unit_b3d_decompress` (the level decoder), `unit_b3d_geom`
  (the geometry decoder and OBJ/MTL writer: number format, UV corner tables, normals and winding,
  texture rules, LINK checks, error offsets of damaged levels).
- Level geometry: `make_fixtures.py` builds the specification's synthetic test vector (14 abstract
  polygons on a 5 x 3 grid with generated palettes and pixels, covering every UV mode, both
  texture rules, LINK entries, moving and hidden polygons and the special cases), pinned by
  SHA-256, and holds an independent model of the decoder and the OBJ/MTL writer. The runner checks
  the tool's OBJ/MTL against the model and the pinned data-line hashes (compressed and not, every
  geometry option, `b3dall`, `--legacy`, usage errors), and runs the specification's mutation tests
  plus the fork's own: each patched copy must be refused at the right offset, written with
  untextured faces, or written with a warning. With `TPM_DISC`, every level's OBJ/MTL is compared
  with the model byte for byte, and the disc run with `b3dall`.
- Fuzz targets `tests/fuzz_*.cpp` with libFuzzer (clang) or a replay driver (`-DTPMX_FUZZ=ON`):
  in memory the LAB directory, the B3D section walk and LZSS decoder, the level geometry decoder
  with its OBJ text (`fuzz_b3d_geom`), and CMPs; and through the tool's file entry points, BAF
  files (`fuzz_baf`: both parsers and every BAF writer, in
  `--legacy` and default mode with the BAF options), levels (`fuzz_b3d_file`: default,
  `--keep-decompressed`, `--legacy`), LAB extraction (`fuzz_lab_file`) and `.Z` extraction
  (`fuzz_z`). The file-level targets run in a scratch folder of their own and fail when anything
  is written outside the output folder. With clang every target is built with coverage
  instrumentation (`-fsanitize=fuzzer-no-link`), so libFuzzer is guided by the tool's code, not
  only by the target's. `make_fixtures.py` writes seeds for each target (including damaged and
  hostile-name inputs), and `ctest -L fuzz` runs every target briefly on them. Seeded only with
  synthetic files, `fuzz_baf` finds the signed overflow in the legacy mipmap size (fixed) within
  about 1,200 inputs.
- Log level -2 (silent: warnings and errors are counted, not printed), for the fuzz targets.
- `make check` and `ctest` run everything; real data is never part of the repository.

### Files

| File | Change |
|---|---|
| `main.cpp` | options, `--legacy`, commands, level scan, exit codes; same steps and output names as before |
| `baf.*` | real BAF layout in default mode, JK-ready 3DO/KEY values, retail CMPs and 8-bit MATs; bounded legacy read |
| `b3d.*`, `b3d_decompress.*` | in-memory decoder with limits, section walk, checked textures, level geometry export |
| `lab.*` | bounds-checked LAB reader, stored or sequential layout |
| `CMP.h`, `CMP.cpp` | removed (replaced by `jk::legacy_cmp`, byte-identical) |
| `unshieldv3/ISArchiveV3.cpp`, `blast.c`, `unshieldv3.cpp` | see Archives and Robustness |
| `TPMExtractor.vcxproj`, `.filters` | the fork's source files |
| `README.md` | rewritten for the fork; original text, credits and disclaimer kept |
| new: `platform.*`, `textout.*`, `bmp.*`, `image.*`, `binreader.*`, `log.*`, `options.*` | portability layer, output writers, options |
| new: `zextract.*` | `.Z` extraction |
| new: `jkout.*` | retail JK MAT and CMP writers, nearest-colour remapping, mipmaps |
| new: `b3d_geom.*` | level geometry decoder and OBJ/MTL writer |
| new: `tests/` | unit tests, end-to-end runner, fixture generator, fuzz targets |
| new: `CMakeLists.txt`, `Makefile`, `cmake/*.cmake`, `.gitignore`, `CHANGES.md`, `NOTICE` | build files and documentation |
