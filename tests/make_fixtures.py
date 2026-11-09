#!/usr/bin/env python3
# TPMExtractor fork - synthetic test fixtures (Python standard library only).
# Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
# Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
"""Synthetic fixtures for the TPMExtractor test suite.

Everything here is generated: generic names (box, rig, node_a, mat0), checker and gradient
textures, ramp and pseudo-random palettes, boxes and pyramids. No game data is read or copied.

Formats written (little-endian unless noted):
  LAB     "LABN" archives (16-byte header, 16-byte entries, name table, data).
  BAF     actor files: 0x120-byte header, optional 0x34C-byte colormap block, materials (name +
          MAT stream), one model blob (rdModel3, rdMesh[], per-mesh arrays, TPM faces, hierarchy
          nodes), key blocks (64-byte wrapper, rdKeyframe, joints, entries) and the 96-byte
          records after the keys. Every header size and offset field is consistent, so a strict
          parser of the real layout and the original tool's sequential read both accept them.
  B3D     level files: the complete section chain from "B3D FILE" to "B3D_END2", including the
          two editor sections whose size fields are not their length ("B3D TMAP" overcounts,
          "B3D_TLIT" undercounts), stored compressed (the TPM LZSS stream after the 0x8A4-byte
          raw header) or uncompressed. Level files embed no other files; the files once thought
          to be embedded (a default colormap, MATs, BMPs, text and sprite files) are big.lab
          entries, and the samples of them are written there, with their LAB type codes.
  .Z      InstallShield v3 archives with stored and PKWARE DCL "implode" members.
  WV      voice files: the "INDYWV" header (little-endian fields) and a VIMA ADPCM stream (its
          header predictor big-endian), mono or stereo, or the block-coded "WVSM" variant,
          encoded here from generated signals (sines, chirps, noise, clipping, silence) with
          escapes and clamps placed on purpose.
  IMC     music files: an "MCMP" container (big-endian) of raw and VIMA blocks holding an
          "iMUS" header with its map (FRMT, TEXT, REGN, JUMP, STOP, SYNC) and the audio.
  DLL     a PE32 file whose data section holds music tables in the layout the tool reads
          (76-byte state and sequence records, 64-byte file records, generic keys).

  SHAPES  the level-geometry test vector: 14 abstract polygons on a 5 x 3 grid covering every UV
          symmetry, the texture rules, LINK entries, moving and hidden polygons, a two-sided,
          a translucent and an animated polygon, one stored counter-clockwise and one with a
          NaN normal (shapes_image, shapes_file), and patched copies of it (SHAPES_MUTATIONS).

  damaged inputs: copies of the fixtures cut at structure boundaries or with one count, size or
          offset field overwritten (baf_mutants, lab_mutants, level_mutants, z_mutants,
          voice_mutants, music_mutants, imuse_mutants), and files whose entry names try to
          leave the output folder (unsafe_name_*).

Besides the writers, the module holds independent models that the test runner compares the tool
against:
  legacy_baf_outputs()   the original tool's BAF read order and output, file by file;
  legacy_b3d_outputs()   the original tool's level texture dump from a decompressed image;
  decode_geometry(), geometry_text(), level_geometry_files(), obj_name()
                         the level geometry decoder and the OBJ/MTL files of default mode, and
                         the name those files take for a level name;
  lab_outputs()          LAB extraction from the stored offsets and from the sequential read;
  is3_outputs()          the files a .Z archive holds;
  wv_model(), imc_model(), imuse_model()
                         the voice decoder, the music decoder with its map text, and the music
                         table reader, written from the format notes (independent of the tool's
                         code; vima_decode_body() is the shared ADPCM reference), each raising
                         SoundError at the offset the tool must name for a damaged file;
  lzss_decode(), dcl_explode(), walk_sections(): reference readers used by --self-test and by
                         the decompressor differential.

Usage:
  python3 -I make_fixtures.py OUTDIR      write a synthetic disc (OUTDIR/disc), loose files and
                                          the damaged inputs
  python3 -I make_fixtures.py --self-test check every writer against the readers above
"""

import argparse
import hashlib
import math
import os
import re
import struct
import sys

# ============================================================================ basics


def f32(x):
    """x rounded to the nearest float32, as a Python float."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def bits_f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def i32_of(u):
    return struct.unpack('<i', struct.pack('<I', u & 0xFFFFFFFF))[0]


def name_bytes(name, size):
    """A NUL-padded fixed-size name field. `name` may be str or bytes (bytes may carry junk
    after an embedded NUL, as the editor left in some real files)."""
    b = name.encode('ascii') if isinstance(name, str) else bytes(name)
    if len(b) > size:
        raise ValueError('name too long for a %d-byte field: %r' % (size, name))
    return b + b'\0' * (size - len(b))


class Out:
    """Little-endian byte builder."""

    def __init__(self):
        self.b = bytearray()

    def __len__(self):
        return len(self.b)

    def u8(self, *v):
        self.b += bytes(x & 0xFF for x in v)

    def u16(self, *v):
        self.b += struct.pack('<%dH' % len(v), *(x & 0xFFFF for x in v))

    def u32(self, *v):
        self.b += struct.pack('<%dI' % len(v), *(x & 0xFFFFFFFF for x in v))

    def i32(self, *v):
        self.b += struct.pack('<%di' % len(v), *v)

    def f32(self, *v):
        self.b += struct.pack('<%df' % len(v), *v)

    def raw(self, data):
        self.b += data

    def name(self, s, size):
        self.b += name_bytes(s, size)

    def bytes(self):
        return bytes(self.b)


class Lcg:
    """Deterministic pseudo-random numbers (no dependence on Python's random module)."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s >> 8

    def below(self, n):
        return self.next() % n


# ============================================================================ palettes and textures

RAMP_BASES = ((255, 255, 255), (255, 72, 40), (72, 230, 72), (60, 110, 255),
              (250, 220, 70), (70, 220, 220), (220, 80, 220), (255, 160, 50))


def _distinct(pal):
    seen = set()
    out = []
    for c in pal:
        c = list(c)
        while tuple(c) in seen:
            c[2] = (c[2] + 1) % 256
        seen.add(tuple(c))
        out.append(tuple(c))
    return out


def ramp_palette():
    """8 hue ramps of 32 shades each: smooth enough for meaningful light tables."""
    pal = []
    for base in RAMP_BASES:
        for s in range(32):
            pal.append(tuple(c * (s + 1) // 32 for c in base))
    return _distinct(pal)


def random_palette(seed):
    rng = Lcg(seed)
    return _distinct([(rng.below(256), rng.below(256), rng.below(256)) for _ in range(256)])


def grayscale_palette():
    return [(i, i, i) for i in range(256)]


def palette_bytes(pal):
    return bytes(v for c in pal for v in c)


def palette_from_bytes(b):
    return [tuple(b[i * 3:i * 3 + 3]) for i in range(256)]


def palette_permutation(pal, seed):
    """(new palette, perm): `pal` with entries 1..255 shuffled and entry 0 kept, and perm[i] =
    the new index of pal[i]. With distinct entries every colour of `pal` occurs exactly once,
    so remapping a texture into the new palette has exactly one right answer per texel."""
    rng = Lcg(seed)
    order = list(range(1, 256))
    for i in range(len(order) - 1, 0, -1):
        j = rng.below(i + 1)
        order[i], order[j] = order[j], order[i]
    out = [pal[0]] + [None] * 255
    perm = [0] * 256
    for src, dst in zip(range(1, 256), order):
        out[dst] = pal[src]
        perm[src] = dst
    return out, perm


PAL_A = ramp_palette()                  # BAF colormap palette
PAL_B = random_palette(0x5EED01)        # a type-3 MAT's own palette
PAL_C = random_palette(0x5EED02)        # the colormap-less BAF's type-3 MAT palette
PAL_D = random_palette(0x5EED03)        # a --palette target
PAL_LEVEL = random_palette(0x5EED04)    # second level palette
PAL_PERM, PERM_A = palette_permutation(PAL_A, 0x5EED05)   # PAL_A reordered: a --palette target


def tex_gradient(w, h, lo, hi):
    n = max(1, w + h - 2)
    return bytes(lo + (x + y) * (hi - lo) // n for y in range(h) for x in range(w))


def tex_checker(w, h, a, b, cell=2):
    return bytes(a if ((x // cell) + (y // cell)) % 2 == 0 else b for y in range(h) for x in range(w))


def tex_holes(pix, every=5, phase=0):
    p = bytearray(pix)
    for i in range(phase, len(p), every):
        p[i] = 0
    return bytes(p)


def tex_half(pix, w, h):
    """The next mip: the top-left texel of every 2x2 block (any content will do for fixtures)."""
    w2, h2 = max(1, w >> 1), max(1, h >> 1)
    return bytes(pix[(2 * y) * w + 2 * x] for y in range(h2) for x in range(w2))


# ============================================================================ MAT streams and CMP files

MAT_REVISION = 0x32
MAT_FMT_FILL = 0xCDCDCDCD   # MSVC debug-heap fill, present in the format words of real files


def mat_stream(mtype, textures, palette=None, fmt_fill=False, cels=None):
    """A material as stored in a BAF after its 32-byte name: header (0x4C), cels (0x18 + 0x10),
    textures (0x18 + mips), and a 768-byte palette when mtype & 1.
    textures: dicts with w, h, transparent (u32), mips (list of bytes, largest first).
    cels: list of (texture index, colour index); default one cel per texture."""
    if cels is None:
        cels = [(i, 0) for i in range(len(textures))]
    o = Out()
    o.raw(b'MAT ')
    o.i32(MAT_REVISION, mtype, len(cels), len(textures))
    o.i32(0, 8)                                  # colour mode 0, 8 bits per pixel
    if fmt_fill:
        o.u32(0, 0, 0, *([MAT_FMT_FILL] * 6), 0, MAT_FMT_FILL, MAT_FMT_FILL)
    else:
        o.u32(*([0] * 12))
    for tex_index, colour in cels:
        o.i32(8, colour)
        o.u32(*([0x3F800000] * 4))
        o.i32(0, 0, 0, tex_index)
    for t in textures:
        o.i32(t['w'], t['h'])
        o.u32(t['transparent'])
        o.i32(0, 0, len(t['mips']))
        w, h = t['w'], t['h']
        for m in t['mips']:
            if len(m) != w * h:
                raise ValueError('mip size')
            o.raw(m)
            w, h = max(1, w >> 1), max(1, h >> 1)
    if mtype & 1:
        o.raw(palette_bytes(palette))
    return o.bytes()


def legacy_cmp_bytes(pal):
    """The 17,216-byte colormap the original tool writes: "CMP ", version 20, flags 0, zero
    header padding, the palette, an all-zero 64x256 light table."""
    return b'CMP ' + struct.pack('<ii', 20, 0) + bytes(52) + palette_bytes(pal) + bytes(64 * 256)


def ramp_light_table(fullbright=frozenset()):
    """A light table for ramp_palette(): darker shades of the same ramp (cheap to build)."""
    t = bytearray(64 * 256)
    for level in range(64):
        for i in range(256):
            if i in fullbright or level == 63:
                t[level * 256 + i] = i
            else:
                ramp, shade = divmod(i, 32)
                t[level * 256 + i] = ramp * 32 + max(0, (shade + 1) * (level + 1) // 64 - 1)
    return bytes(t)


def retail_cmp_bytes(pal, light, transparency=None, tint=(1.0, 1.0, 1.0)):
    """A retail-format colormap: "CMP ", version 30, flags (1 with a transparency table)."""
    flags = 1 if transparency is not None else 0
    head = b'CMP ' + struct.pack('<iI3f', 30, flags, *tint) + bytes(40)
    return head + palette_bytes(pal) + light + (transparency or b'')


# ============================================================================ TPM level LZSS

LZSS_RING = 4096
LZSS_MAX_COPY = 17


class BitWriterMSB:
    """Bits written most significant first, packed into bytes (the TPM level stream)."""

    def __init__(self):
        self.out = bytearray()
        self.cur = 0
        self.n = 0

    def bits(self, v, count):
        for i in range(count - 1, -1, -1):
            self.cur = (self.cur << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.cur)
                self.cur = 0
                self.n = 0

    def bit_length(self):
        return len(self.out) * 8 + self.n

    def getvalue(self, pad_ones=False):
        out = bytearray(self.out)
        if self.n:
            fill = ((1 << (8 - self.n)) - 1) if pad_ones else 0
            out.append(((self.cur << (8 - self.n)) | fill) & 0xFF)
        return bytes(out)


def lzss_encode(data, mode='greedy', window=48, end_marker=True, pad_ones=False):
    """TPM's level LZSS: flag 1 + 8-bit literal, or flag 0 + 12-bit absolute ring offset +
    4-bit length L (copies L + 2 bytes); offset 0 ends the stream. The 4,096-byte ring starts
    zeroed with its write position at 1.
    mode: 'literal' (no copies), 'greedy' (copies from earlier output), 'exotic' (also copies
    zeros from ring positions that were never written, and prefers the longest distances)."""
    w = BitWriterMSB()
    n = 0
    size = len(data)
    index = {}

    def remember(i):
        if i + 1 < size:
            index.setdefault(data[i:i + 2], []).append(i)

    while n < size:
        best_len, best_off = 0, 0
        maxl = min(LZSS_MAX_COPY, size - n)
        if mode != 'literal' and maxl >= 2:
            if mode == 'exotic' and data[n] == 0:
                run = 0
                while run < maxl and data[n + run] == 0:
                    run += 1
                q = LZSS_RING - run          # ring positions q..4095 are still zero
                if run >= 2 and q >= n + 1:
                    best_len, best_off = run, q
            cands = index.get(data[n:n + 2])
            if cands and best_len < maxl:
                order = cands[-window:] if mode == 'exotic' else reversed(cands[-window:])
                for p in order:
                    if n - p > LZSS_RING:
                        if mode == 'exotic':
                            continue        # oldest first: skip until inside the ring
                        break               # newest first: everything older is out of reach
                    off = (1 + p) & 0xFFF
                    if off == 0:
                        continue
                    ln = 2
                    while ln < maxl and data[p + ln] == data[n + ln]:
                        ln += 1
                    if ln > best_len:
                        best_len, best_off = ln, off
                        if ln == maxl:
                            break
        if best_len >= 2:
            w.bits(0, 1)
            w.bits(best_off, 12)
            w.bits(best_len - 2, 4)
            for i in range(n, n + best_len):
                remember(i)
            n += best_len
        else:
            w.bits(1, 1)
            w.bits(data[n], 8)
            remember(n)
            n += 1
    if end_marker:
        w.bits(0, 13)
    return w.getvalue(pad_ones)


def lzss_decode(stream, quirks=True, max_output=1 << 27):
    """Reference decoder. Returns (output, end, consumed) with end 'marker', 'input' (the
    stream ran out) or 'limit'. With quirks, a token cut off by the end of the input is decoded
    as the original reader did (a cut literal gives one 0 byte; a cut length copies 2 bytes)."""
    out = bytearray()
    ring = bytearray(LZSS_RING)
    state = {'pos': 0, 'mask': 0x80, 'cur': 0}
    wpos = [1]

    def bit():
        if state['mask'] == 0x80:
            if state['pos'] >= len(stream):
                return None
            state['cur'] = stream[state['pos']]
            state['pos'] += 1
        b = 1 if state['cur'] & state['mask'] else 0
        state['mask'] >>= 1
        if not state['mask']:
            state['mask'] = 0x80
        return b

    def bits(n):
        v = 0
        for i in range(n - 1, -1, -1):
            b = bit()
            if b is None:
                return None
            v |= b << i
        return v

    def put(c):
        if len(out) >= max_output:
            return False
        out.append(c)
        ring[wpos[0]] = c
        wpos[0] = (wpos[0] + 1) & 0xFFF
        return True

    end = 'input'
    while True:
        flag = bit()
        if flag is None:
            break
        if flag:
            c = bits(8)
            if c is None:
                if quirks and not put(0):
                    end = 'limit'
                break
            if not put(c):
                end = 'limit'
                break
            continue
        off = bits(12)
        if off is None:
            break
        if off == 0:
            end = 'marker'
            break
        ln = bits(4)
        cut = ln is None
        if cut:
            if not quirks:
                break
            ln = 0
        ok = True
        for i in range(ln + 2):
            if not put(ring[(off + i) & 0xFFF]):
                ok = False
                break
        if not ok:
            end = 'limit'
            break
        if cut:
            break
    return bytes(out), end, state['pos']


# ============================================================================ PKWARE DCL implode (blast format)

DCL_LITLEN = (11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
              9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
              7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
              8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
              44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
              44, 173)
DCL_LENLEN = (2, 35, 36, 53, 38, 23)
DCL_DISTLEN = (2, 20, 53, 230, 247, 151, 248)
DCL_BASE = (3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264)
DCL_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8)
DCL_MAXBITS = 13
DCL_END_LENGTH = 519


def _dcl_lengths(rep):
    out = []
    for b in rep:
        out += [b & 15] * ((b >> 4) + 1)
    return out


def _dcl_canonical(lengths):
    """Codes as blast's decode() reads them: per length, the first code is (first code of the
    previous length + its count) << 1; symbols in length, then symbol, order."""
    count = [0] * (DCL_MAXBITS + 1)
    for ln in lengths:
        count[ln] += 1
    first, f = {}, 0
    for ln in range(1, DCL_MAXBITS + 1):
        first[ln] = f
        f = (f + count[ln]) << 1
    nxt = dict(first)
    codes = {}
    for sym, ln in sorted(((s, ln) for s, ln in enumerate(lengths) if ln), key=lambda t: (t[1], t[0])):
        codes[sym] = (ln, nxt[ln])
        nxt[ln] += 1
    return codes, count, sorted((s for s, ln in enumerate(lengths) if ln), key=lambda s: (lengths[s], s))


DCL_LIT_CODES = _dcl_canonical(_dcl_lengths(DCL_LITLEN))
DCL_LEN_CODES = _dcl_canonical(_dcl_lengths(DCL_LENLEN))
DCL_DIST_CODES = _dcl_canonical(_dcl_lengths(DCL_DISTLEN))


class BitWriterLSB:
    """Bits packed least significant first (blast's bit order)."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def bits(self, v, count):
        self.acc |= (v & ((1 << count) - 1)) << self.n
        self.n += count
        while self.n >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def code(self, ln_code):
        ln, c = ln_code
        for i in range(ln - 1, -1, -1):          # first stream bit = MSB of the code, inverted
            self.bits(((c >> i) & 1) ^ 1, 1)

    def getvalue(self):
        out = bytearray(self.out)
        if self.n:
            out.append(self.acc & 0xFF)
        return bytes(out)


def _dcl_len_symbol(ln):
    for sym in range(16):
        if DCL_BASE[sym] <= ln < DCL_BASE[sym] + (1 << DCL_EXTRA[sym]):
            return sym
    raise ValueError(ln)


def dcl_implode(data, coded_literals=False, dict_bits=6, matches=True, window=48):
    """PKWARE DCL "implode" stream as blast() decodes it (literal flag, dictionary size, then
    literals and length/distance pairs, ended by length 519)."""
    if dict_bits not in (4, 5, 6):
        raise ValueError('dict_bits')
    w = BitWriterLSB()
    w.bits(1 if coded_literals else 0, 8)
    w.bits(dict_bits, 8)
    maxdist = 64 << dict_bits
    n, size, index = 0, len(data), {}

    def remember(i):
        if i + 1 < size:
            index.setdefault(data[i:i + 2], []).append(i)

    while n < size:
        best_len, best_dist = 0, 0
        if matches and size - n >= 2:
            maxl = min(DCL_END_LENGTH - 1, size - n)
            for p in reversed(index.get(data[n:n + 2], [])[-window:]):
                d = n - p
                if d > maxdist:
                    break
                ln = 2
                while ln < maxl and data[p + ln] == data[n + ln]:
                    ln += 1
                if ln == 2 and d > 256:
                    continue
                if ln > best_len:
                    best_len, best_dist = ln, d
                    if ln == maxl:
                        break
        if best_len >= 2:
            sym = _dcl_len_symbol(best_len)
            w.bits(1, 1)
            w.code(DCL_LEN_CODES[0][sym])
            w.bits(best_len - DCL_BASE[sym], DCL_EXTRA[sym])
            sbits = 2 if best_len == 2 else dict_bits
            w.code(DCL_DIST_CODES[0][(best_dist - 1) >> sbits])
            w.bits((best_dist - 1) & ((1 << sbits) - 1), sbits)
            for i in range(n, n + best_len):
                remember(i)
            n += best_len
        else:
            w.bits(0, 1)
            if coded_literals:
                w.code(DCL_LIT_CODES[0][data[n]])
            else:
                w.bits(data[n], 8)
            remember(n)
            n += 1
    w.bits(1, 1)
    w.code(DCL_LEN_CODES[0][15])
    w.bits(DCL_END_LENGTH - DCL_BASE[15], DCL_EXTRA[15])
    return w.getvalue()


def dcl_explode(stream):
    """Reference decoder following blast.c (raises ValueError on bad data or short input)."""
    st = {'pos': 0, 'buf': 0, 'cnt': 0}

    def bits(need):
        val = st['buf']
        while st['cnt'] < need:
            if st['pos'] >= len(stream):
                raise ValueError('out of input')
            val |= stream[st['pos']] << st['cnt']
            st['pos'] += 1
            st['cnt'] += 8
        st['buf'] = val >> need
        st['cnt'] -= need
        return val & ((1 << need) - 1)

    def decode(table):
        _, count, symbols = table
        code = first = index = 0
        for ln in range(1, DCL_MAXBITS + 1):
            code |= bits(1) ^ 1
            c = count[ln]
            if code < first + c:
                return symbols[index + (code - first)]
            index += c
            first += c
            first <<= 1
            code <<= 1
        raise ValueError('ran out of codes')

    lit = bits(8)
    dictb = bits(8)
    if lit > 1 or dictb not in (4, 5, 6):
        raise ValueError('bad header')
    out = bytearray()
    while True:
        if bits(1):
            sym = decode(DCL_LEN_CODES)
            ln = DCL_BASE[sym] + bits(DCL_EXTRA[sym])
            if ln == DCL_END_LENGTH:
                break
            sb = 2 if ln == 2 else dictb
            dist = (decode(DCL_DIST_CODES) << sb) + bits(sb) + 1
            if dist > len(out):
                raise ValueError('distance too far back')
            for _ in range(ln):
                out.append(out[-dist])
        else:
            out.append(decode(DCL_LIT_CODES) if lit else bits(8))
    return bytes(out)


# ============================================================================ LAB archives

LAB_VERSION = 0x10000


def make_lab(entries, layout='sequential', version=LAB_VERSION):
    """entries: (name, type, data) with the type in reading order ("PALT"); it is stored
    byte-reversed. layout 'sequential' is the retail layout (names and data in entry order, no
    gaps); 'scrambled' stores the names in reverse order with padding after the table and a
    3-byte gap before each data block, so the stored offsets and a sequential read disagree."""
    n = len(entries)
    names = [name_bytes(e[0], len(e[0])) + b'\0' for e in entries]
    order = list(range(n)) if layout == 'sequential' else list(reversed(range(n)))
    table = bytearray()
    name_off = [0] * n
    for i in order:
        name_off[i] = len(table)
        table += names[i]
    if layout != 'sequential':
        table += b'\xEE\xEE'
    out = bytearray(struct.pack('<4sIII', b'LABN', version, n, len(table)) + bytes(16 * n) + table)
    for i, (name, typ, data) in enumerate(entries):
        if layout != 'sequential':
            out += b'\xCC' * 3
        struct.pack_into('<III4s', out, 16 + 16 * i, name_off[i], len(out), len(data), typ.encode('ascii')[::-1])
        out += data
    return bytes(out)


def lab_outputs(lab, sequential):
    """The files a LAB extraction writes, in write order: [(name, data)]. sequential=False reads
    the stored offsets (default mode); True reads names and data one after another as the
    original tool did (--legacy)."""
    _, _, n, tsize = struct.unpack_from('<4sIII', lab, 0)
    recs = [struct.unpack_from('<III4s', lab, 16 + 16 * i) for i in range(n)]
    table_at = 16 + 16 * n
    out = []
    if sequential:
        p = table_at
        names = []
        for _ in range(n):
            e = lab.index(b'\0', p)
            names.append(lab[p:e].decode('latin-1'))
            p = e + 1
        for i in range(n):
            ln = recs[i][2]
            out.append((names[i], lab[p:p + ln]))
            p += ln
    else:
        for name_off, data_off, ln, _ in recs:
            s = table_at + name_off
            e = lab.index(b'\0', s)
            out.append((lab[s:e].decode('latin-1'), lab[data_off:data_off + ln]))
    return out


# ============================================================================ InstallShield v3 (.Z)

IS3_SIG1 = 0x8C655D13
IS3_SIG2 = 0x02013A
IS3_HEADER_AREA = 0xFF
IS3_ATTR_STORED = 0x10
IS3_ATTR_ARCHIVE = 0x20
IS3_DATETIME = ((1999 - 1980) << 9 | 5 << 5 | 19) | ((12 << 11 | 30 << 5) << 16)   # a fixed DOS date/time


def make_is3(members):
    """members: dicts with path ("name" or "dir\\name"), data, and method 'stored' or
    'implode' (with coded: bool and dict_bits: 4..6). Directories are written in order of first
    use; files are grouped by directory as the format requires."""
    dirs = []
    by_dir = {}
    for m in members:
        d, _, f = m['path'].rpartition('\\')
        if d not in by_dir:
            by_dir[d] = []
            dirs.append(d)
        by_dir[d].append((f, m))
    out = bytearray(IS3_HEADER_AREA)
    placed = []
    total_c = total_u = 0
    for d in dirs:
        for f, m in by_dir[d]:
            if m.get('method', 'implode') == 'stored':
                comp, attrib = m['data'], IS3_ATTR_STORED | IS3_ATTR_ARCHIVE
            else:
                comp = dcl_implode(m['data'], m.get('coded', False), m.get('dict_bits', 6))
                attrib = IS3_ATTR_ARCHIVE
            placed.append((d, f, len(out), comp, attrib, len(m['data'])))
            out += comp
            total_c += len(comp)
            total_u += len(m['data'])
    toc = len(out)
    for d in dirs:
        nb = d.encode('ascii')
        chunk = 6 + len(nb)
        out += struct.pack('<HHH', len(by_dir[d]), chunk, len(nb)) + nb
    for index, (d, f, off, comp, attrib, usize) in enumerate(placed):
        nb = f.encode('ascii')
        chunk = 30 + len(nb) + 2                          # 2 bytes of slack, skipped by readers
        out += struct.pack('<BHIIIIIHBBBBB', 0, index, usize, len(comp), off, IS3_DATETIME, 0, chunk,
                           attrib, 0, 0, 0, len(nb)) + nb + b'\0\0'
    struct.pack_into('<IIHHHIIII', out, 0, IS3_SIG1, IS3_SIG2, 0, 0, len(placed), IS3_DATETIME, total_c, total_u, 0)
    struct.pack_into('<BBB', out, 30, 1, 1, 0)
    struct.pack_into('<IIII', out, 33, 0, 0, toc, 0)
    struct.pack_into('<H', out, 49, len(dirs))
    return bytes(out)


def is3_outputs(members):
    """The files extracting `members` writes: [(path with '/', data)]."""
    out = []
    for m in members:
        d, _, f = m['path'].rpartition('\\')
        out.append(((d + '/' if d else '') + f, m['data']))
    return out


# ============================================================================ BAF actors

BAF_MAGIC = 0x00010001
BAF_HEADER = 0x120
BAF_CMP_BLOCK = 0x34C
MODEL_HEADER = 0x84
MESH_HEADER = 0x70
FACE_SIZE = 0x4C
NODE_SIZE = 0xB4
KEY_WRAPPER = 0x40
KEY_HEADER = 0x84
JOINT_SIZE = 0x2C
ENTRY_SIZE = 0x38
EXTRA_SIZE = 0x60
STALE = 0x00A3F2C0                      # an editor pointer value (meaningless in the file)
FILL_CC = 0xCCCCCCCC                    # MSVC stack fill, present in unused node fields


def _model_blob(model, mat_types):
    o = Out()
    meshes, nodes = model['meshes'], model['nodes']
    o.name(model['name'], 32)
    o.u32(0)                                              # id
    o.u32(len(meshes), STALE + 0x100)                     # geoset 0: mesh count, pointer
    o.u32(*([0] * 6))                                     # geosets 1..3
    o.u32(1, STALE + 0x200, len(mat_types), 0)            # numGeosets, materials ptr, count, curGeoset
    o.u32(len(nodes), STALE + 0x300)                      # hierarchy nodes, pointer
    o.f32(model['radius'])                                # TPM's stored model radius
    o.u32(*([0] * 6))
    o.f32(*model['insert'])
    for i, me in enumerate(meshes):
        o.name(me['name'], 32)
        o.i32(i)
        o.u32(me.get('geo', 4), me.get('light', 3), me.get('tex', 0))
        nv, nuv, nf = len(me['verts']), len(me['uvs']), len(me['faces'])
        ptrs = [STALE + 0x1000 * (i + 1) + k * 0x40 if c else 0 for k, c in enumerate((nv, nuv, nv, nv, nf, nv))]
        o.u32(*ptrs)
        o.u32(nv, nuv, nf)
        o.f32(me.get('radius', 0.0), *me.get('bbox', (0.0,) * 6))
    for me in meshes:
        nv = len(me['verts'])
        if nv:
            for v in me['verts']:
                o.f32(*v)
            for v in me['norms']:
                o.f32(*v)
            o.f32(*me.get('vi', [0.0] * nv))
            vunk = me.get('vunk')
            if vunk == 'fill':
                o.u32(*([FILL_CC] * nv))
            else:
                o.f32(*(vunk or [0.0] * nv))
        for uv in me['uvs']:
            o.f32(*uv)
        for k, fc in enumerate(me['faces']):
            o.u32(fc['mat'], fc.get('flags', 0), 4, fc.get('light', 3), 0, len(fc['pos']))
            o.u32(STALE + 0x8000 + k * 16, STALE + 0x8004 + k * 16, STALE + 0x8008)
            o.i32(fc.get('wallcel', -1))
            o.f32(0.0, 0.0, fc.get('extralight', 0.0), fc.get('alpha', 0.0), *fc['normal'])
            o.u32(0, fc.get('animfunc', 0))
        for fc in me['faces']:
            o.i32(*fc['pos'])
            if mat_types[fc['mat']] & 2:
                o.i32(*fc['uv'])
    for i, nd in enumerate(nodes):
        o.name(nd['name'], 32)
        o.u32(*nd.get('f20', [0] * 8))
        o.u32(0, i, nd.get('type', 1))
        o.i32(nd['mesh'], nd['parent'])
        o.u32(nd['nchild'])
        o.i32(nd['child'], nd['sibling'])
        o.f32(*nd['pivot'])
        o.f32(*nd['pos'])
        o.f32(*nd['rot'])
        o.u32(*nd.get('mtx', [0] * 12))
    return o.bytes()


def _key_block(key):
    joints = key['joints']
    nent = sum(len(j['entries']) for j in joints)
    size = KEY_WRAPPER + KEY_HEADER + JOINT_SIZE * len(joints) + ENTRY_SIZE * nent
    o = Out()
    o.u32(size)
    o.f32(0.0, key.get('blend', 0.1))
    o.i32(0, 0, 4, 0)
    o.u32(key.get('num_extra', 0))
    o.i32(0)
    o.u32(*([0] * 7))
    o.name(key['name'], 32)
    o.u32(0, 0, len(joints), key.get('type', 0xFFFF))
    o.f32(key.get('fps', 15.0))
    o.u32(key['frames'], len(joints), STALE + 0x4000, 0)
    o.u32(*([0] * 16))
    for j in joints:
        o.name(j['name'], 32)
        o.i32(j['node'])
        o.u32(len(j['entries']), STALE + 0x5000)
    for j in joints:
        for e in j['entries']:
            frame, flags, pos, rot, dpos, drot = e
            o.f32(frame)
            o.u32(flags)
            o.f32(*pos, *rot, *dpos, *drot)
    return o.bytes()


def extra_record(frame, sound):
    o = Out()
    o.u32(frame, 0)
    o.name(sound, 28)
    o.f32(1.0)
    o.u32(0, 0)
    o.f32(2.0, 6.0)
    o.u32(*([0] * 10))
    assert len(o) == EXTRA_SIZE
    return o.bytes()


def build_baf(spec):
    """The bytes of a BAF described by `spec` (see baf_fixtures()), with every header size,
    offset and count consistent with the payload, plus a layout dict (offsets of the parts)."""
    cmp = spec.get('colormap')
    mats = spec['materials']
    mat_types = [m['type'] for m in mats]
    mat_part = b''.join(name_bytes(m['name'], 32) + mat_stream(m['type'], m['textures'], m.get('palette'),
                                                                m.get('fmt_fill', False), m.get('cels'))
                        for m in mats)
    model = _model_blob(spec['model'], mat_types)
    keys = b''.join(_key_block(k) for k in spec.get('keys', []))
    extras = b''.join(spec.get('extras', []))
    nextra = sum(k.get('num_extra', 0) for k in spec.get('keys', []))
    if nextra * EXTRA_SIZE != len(extras):
        raise ValueError('%s: %d extra records declared, %d bytes given' % (spec['file'], nextra, len(extras)))
    cmp_part = b''
    if cmp:
        c = Out()
        c.name(cmp['name'], 32)
        c.u32(0)
        c.f32(*cmp.get('tint', (0.0, 0.0, 0.0)))
        c.raw(palette_bytes(cmp['palette']))
        c.u32(*([0] * 7))
        cmp_part = c.bytes()
        assert len(cmp_part) == BAF_CMP_BLOCK
    num_mats, num_keys = len(mats), len(spec.get('keys', []))
    model_off = BAF_HEADER + len(cmp_part) + len(mat_part)
    key_off = model_off + len(model)
    key_size = len(keys) + len(extras)
    h = Out()
    h.u32(BAF_MAGIC, BAF_HEADER)
    h.name(spec.get('name_field', spec['file']), 32)
    h.name(spec.get('source', 'C:\\fixtures\\%s\\' % spec['file'].split('.')[0]), 128)
    h.u32(BAF_HEADER + (BAF_CMP_BLOCK if cmp else 0) + 4 * num_mats + 4 * num_keys + len(model) + key_size)
    h.f32(spec.get('fac', 1.0), 15.0)
    h.f32(*spec.get('draw_offset', (0.0, 0.0, 0.0)))
    h.u32(len(model), num_mats, num_keys)
    h.u16(1 if cmp else 0)
    h.u8(0, spec.get('flag_cf', 0))
    h.u32(len(mat_part), key_size, 0, BAF_CMP_BLOCK if cmp else 0, model_off - BAF_HEADER, key_off - BAF_HEADER, 0)
    h.f32(spec.get('cyl_radius', 0.25))
    h.u32(spec.get('joint_table', 0))
    h.f32(spec.get('cyl_height', 0.8))
    h.u32(*([0] * 10))
    assert len(h) == BAF_HEADER, len(h)
    data = h.bytes() + cmp_part + mat_part + model + keys + extras
    layout = {'model_off': model_off, 'key_off': key_off, 'extras_off': key_off + len(keys), 'size': len(data)}
    return data, layout


# ---------------------------------------------------------------------------- BAF fixture set

def _box(cx, cy, cz, hx, hy, hz):
    return [(cx + sx * hx, cy + sy * hy, cz + sz * hz)
            for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]


def _unit(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n else (0.0, 0.0, 0.0)


def _box_mesh(name, centre, half, mats, uv_size=8.0):
    verts = _box(*centre, *half)
    norms = [_unit(tuple(v[k] - centre[k] for k in range(3))) for v in verts]
    s = uv_size
    uvs = [(0.0, 0.0), (s, 0.0), (s, -s), (0.0, -s), (0.0078125, -0.0234375)]
    quads = [((0, 2, 3, 1), (0.0, 0.0, -1.0)), ((4, 5, 7, 6), (0.0, 0.0, 1.0)),
             ((0, 1, 5, 4), (0.0, -1.0, 0.0)), ((2, 6, 7, 3), (0.0, 1.0, 0.0)),
             ((0, 4, 6, 2), (-1.0, 0.0, 0.0)), ((1, 3, 7, 5), (1.0, 0.0, 0.0))]
    faces = []
    for k, (q, n) in enumerate(quads):
        faces.append({'mat': mats[k % len(mats)], 'flags': 1 if k == 1 else 0, 'pos': list(q),
                      'uv': [0, 1, 2, 4 if k == 5 else 3], 'normal': n})
    lo = [centre[k] - half[k] for k in range(3)]
    return {'name': name, 'verts': verts, 'norms': norms, 'uvs': uvs, 'faces': faces,
            'radius': 0.01, 'bbox': (*centre, *half), 'lo': lo}


def _pyramid_mesh(name, mat):
    verts = [(-0.1, -0.1, 0.0), (0.1, -0.1, 0.0), (0.1, 0.1, 0.0), (-0.1, 0.1, 0.0), (0.0, 0.0, 0.35)]
    norms = [_unit(v) if v != (0.0, 0.0, 0.0) else (0.0, 0.0, 1.0) for v in verts]
    uvs = [(0.0, 0.0), (2.0, 0.0), (1.0, -4.0)]
    faces = [{'mat': mat, 'pos': [0, 1, 4], 'uv': [0, 1, 2], 'normal': _unit((0.0, -0.35, 0.1))},
             {'mat': mat, 'pos': [1, 2, 4], 'uv': [0, 1, 2], 'normal': _unit((0.35, 0.0, 0.1))},
             {'mat': mat, 'pos': [2, 3, 4], 'uv': [0, 1, 2], 'normal': _unit((0.0, 0.35, 0.1))},
             {'mat': mat, 'pos': [3, 0, 4], 'uv': [0, 1, 2], 'normal': _unit((-0.35, 0.0, 0.1)),
              'alpha': 0.5, 'flags': 2},
             {'mat': mat, 'pos': [0, 3, 2, 1], 'uv': [0, 1, 2, 1], 'normal': (0.0, 0.0, -1.0)}]
    return {'name': name, 'verts': verts, 'norms': norms, 'vi': [0.0, 0.0, 0.0, 0.5, 0.0], 'uvs': uvs,
            'faces': faces, 'radius': 0.2, 'bbox': (0.0, 0.0, 0.175, 0.1, 0.1, 0.175)}


def _tri_mesh(name, mat, z=0.0):
    verts = [(0.0, 0.0, z), (0.3, 0.0, z), (0.0, 0.3, z + 0.1)]
    return {'name': name, 'verts': verts, 'norms': [(0.0, 0.0, 1.0)] * 3, 'uvs': [(0.0, 0.0), (4.0, 0.0), (0.0, -4.0)],
            'faces': [{'mat': mat, 'pos': [0, 1, 2], 'uv': [0, 1, 2], 'normal': (0.0, 0.0, 1.0)}],
            'radius': 0.25, 'bbox': (0.15, 0.15, z + 0.05, 0.15, 0.15, 0.05)}


def _node(name, mesh, parent, nchild, child, sibling, pivot, pos, rot, **kw):
    nd = {'name': name, 'mesh': mesh, 'parent': parent, 'nchild': nchild, 'child': child, 'sibling': sibling,
          'pivot': pivot, 'pos': pos, 'rot': rot}
    nd.update(kw)
    return nd


def _entries(n, seed, frames=(0, 4, 8)):
    rng = Lcg(seed)
    out = []
    for k in range(n):
        p = tuple((rng.below(2001) - 1000) / 1000.0 for _ in range(3))
        r = tuple((rng.below(721) - 360) * 0.5 for _ in range(3))
        out.append((float(frames[k % len(frames)]), (0x2, 0x3, 0x1, 0x0)[k % 4], p, r,
                    (0.0078125, -0.0, 1e-7), (0.0234375, -1e-7, 1.5)))
    return out


def baf_fixtures():
    """The synthetic BAF set: [(file name in big.lab, spec)]. Names are generic."""
    box_tex = bytearray(tex_gradient(8, 8, 0, 63))
    box_tex[9] = 255
    box = {
        'file': 'box.baf',
        'colormap': {'name': 'box.cmp', 'palette': PAL_A},
        'materials': [{'name': 'box.mat', 'type': 2, 'fmt_fill': True,
                       'textures': [{'w': 8, 'h': 8, 'transparent': 0, 'mips': [bytes(box_tex)]}]}],
        'model': {'name': 'box.3do', 'radius': 0.01, 'insert': (0.0, 0.0, 0.0),
                  'meshes': [_box_mesh('node_a', (0.0, 0.0, 0.75), (0.5, 0.5, 0.5), [0])],
                  'nodes': [_node('node_a', 0, -1, 0, -1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0),
                                  mtx=[0x3F800000, 0, 0, 0, 0x3F800000, 0, 0, 0, 0x3F800000, 0, 0, 0])]},
        'keys': [{'name': 'spin.key', 'frames': 10, 'num_extra': 1,
                  'joints': [{'name': 'node_a', 'node': 0, 'entries': _entries(3, 11)}]}],
        'extras': [extra_record(4, 'step.wav')],
    }
    mat0 = tex_holes(tex_gradient(16, 8, 64, 127), every=5)
    glow0 = tex_checker(8, 8, 140, 150)
    glow1 = tex_holes(tex_gradient(4, 4, 160, 175), every=3)
    rig = {
        'file': 'rig.baf',
        'colormap': {'name': 'rig.cmp', 'palette': PAL_A},
        'materials': [
            {'name': 'mat0.mat', 'type': 2, 'textures': [{'w': 16, 'h': 8, 'transparent': 1, 'mips': [mat0]}]},
            {'name': 'skin.mat', 'type': 3, 'palette': PAL_B,
             'textures': [{'w': 2, 'h': 4, 'transparent': 0, 'mips': [tex_checker(2, 4, 0, 200, 1)]}]},
            {'name': 'glow.mat', 'type': 2, 'fmt_fill': True,
             'textures': [{'w': 8, 'h': 8, 'transparent': 0, 'mips': [glow0, tex_half(glow0, 8, 8)]},
                          {'w': 4, 'h': 4, 'transparent': MAT_FMT_FILL, 'mips': [glow1]}]},
        ],
        'model': {'name': 'rig.3do', 'radius': 0.02, 'insert': (0.0, 0.0, 0.6),
                  'meshes': [_box_mesh('torso', (0.0, 0.0, 0.0), (0.25, 0.15, 0.35), [0, 2]),
                             _pyramid_mesh('arm', 1),
                             {'name': 'tip', 'verts': [], 'norms': [], 'uvs': [], 'faces': [],
                              'radius': 0.0, 'bbox': (0.0,) * 6}],
                  'nodes': [_node(b'node_root\0junk', 0, -1, 2, 1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.6),
                                  (0.0, 0.0, 0.0), f20=[FILL_CC] * 8),
                            _node('node_arm_left', 1, 0, 0, -1, 2, (0.1, 0.0, -0.05), (0.45, 0.0, 0.2),
                                  (15.0, 30.0, -10.0), type=0x4,
                                  mtx=[0xFFC00000, 0x7FC00000, 0xFFFFFFFF] + [FILL_CC] * 9),
                            _node('node_tip', 2, 0, 0, -1, -1, (0.0, 0.0, 0.0), (-0.3, 0.1, 0.4),
                                  (0.0, -45.0, 0.0234375))]},
        'keys': [
            {'name': 'wave.key', 'frames': 9, 'type': 0x7F, 'fps': 30.0,
             'joints': [{'name': 'node_root', 'node': 0, 'entries': _entries(2, 21)},
                        {'name': 'node_arm_left', 'node': 1, 'entries': _entries(3, 22)},
                        {'name': 'node_tip', 'node': 2, 'entries': _entries(1, 23)}]},
            {'name': 'turn.key', 'frames': 5, 'num_extra': 2, 'fps': 24.0,
             'joints': [{'name': 'node_root', 'node': 0, 'entries': _entries(2, 31)},
                        {'name': 'node_arm_left', 'node': 1, 'entries': _entries(2, 32)},
                        {'name': 'node_tip', 'node': 2, 'entries': _entries(2, 33)}]},
            {'name': 'WAVE.key', 'frames': 3, 'type': 0xF,
             'joints': [{'name': 'node_root', 'node': 0, 'entries': _entries(1, 41)},
                        {'name': 'node_arm_left', 'node': 1, 'entries': _entries(1, 42)},
                        {'name': 'node_tip', 'node': 2, 'entries': _entries(1, 43)}]},
            # a short key: it animates 2 of the 3 nodes, with a partial body-part mask
            {'name': 'half.key', 'frames': 6, 'type': 0x4, 'fps': 12.0,
             'joints': [{'name': 'node_root', 'node': 0, 'entries': _entries(2, 71, frames=(0, 3))},
                        {'name': 'node_arm_left', 'node': 1, 'entries': _entries(2, 72, frames=(0, 3))}]},
        ],
        'extras': [extra_record(1, 'tick.wav'), extra_record(3, 'tock.wav')],
    }
    stub = {
        'file': 'stub.baf', 'fac': 2.0,         # a draw scale (+0xAC) other than 1, exact in binary
        'colormap': {'name': 'stub.cmp', 'palette': PAL_A, 'tint': (1.0, 1.0, 1.0)},
        # masked (transparent = 1) although no texel uses the key index 0
        'materials': [{'name': 'stub.mat', 'type': 2,
                       'textures': [{'w': 4, 'h': 4, 'transparent': 1, 'mips': [tex_gradient(4, 4, 130, 145)]}]}],
        'model': {'name': 'stub.3do', 'radius': 0.05, 'insert': (0.0, 0.0, 0.0),
                  'meshes': [_tri_mesh('node_a', 0, z=-0.2)],
                  'nodes': [_node('node_a', 0, -1, 0, -1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0))]},
        'keys': [{'name': 'stub.key', 'frames': 2,
                  'joints': [{'name': 'node_a', 'node': 0, 'entries': _entries(2, 51, frames=(0, 1))}]}],
        'extras': [],
    }
    # No colormap (has_colormap = 0) and a type-3 MAT with its own palette. With a key, the
    # original tool's read order takes the model blob for a key and writes it as a key-format
    # "leaf.3do", as it does for the one colormap-less BAF on the retail disc.
    leaf = {
        'file': 'leaf.baf', 'flag_cf': 1,
        'colormap': None,
        'materials': [{'name': 'leaf.mat', 'type': 3, 'palette': PAL_C,
                       'textures': [{'w': 8, 'h': 4, 'transparent': 1,
                                     'mips': [tex_holes(tex_gradient(8, 4, 1, 250), every=4, phase=1)]}]}],
        'model': {'name': 'leaf.3do', 'radius': 0.3, 'insert': (0.0, 0.0, 0.0),
                  'meshes': [_tri_mesh('node_a', 0, z=0.1)],
                  'nodes': [_node('node_a', 0, -1, 0, -1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0))]},
        'keys': [{'name': 'drift.key', 'frames': 4, 'num_extra': 1,
                  'joints': [{'name': 'node_a', 'node': 0, 'entries': _entries(2, 61, frames=(0, 2))}]}],
        'extras': [extra_record(2, 'rustle.wav')],
    }
    bare = {
        'file': 'bare.baf',
        'colormap': None,
        'materials': [{'name': 'bare.mat', 'type': 2,
                       'textures': [{'w': 4, 'h': 4, 'transparent': 0, 'mips': [tex_gradient(4, 4, 10, 240)]}]}],
        'model': {'name': 'bare.3do', 'radius': 0.3, 'insert': (0.0, 0.0, 0.0),
                  'meshes': [_tri_mesh('node_a', 0)],
                  'nodes': [_node('node_a', 0, -1, 0, -1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0))]},
        'keys': [], 'extras': [],
    }
    # Stands on its origin with hierarchy node 0 above it, as the people and creatures of the
    # retail disc are built: --insert-offset auto (the default) re-roots it at node 0, so it hangs
    # node 0's height below its origin and stands on the floor in JK.
    stand = {
        'file': 'stand.baf',
        'colormap': {'name': 'stand.cmp', 'palette': PAL_B},
        'materials': [{'name': 'stand.mat', 'type': 2,
                       'textures': [{'w': 4, 'h': 4, 'transparent': 0, 'mips': [tex_gradient(4, 4, 20, 90)]}]}],
        'model': {'name': 'stand.3do', 'radius': 0.9, 'insert': (0.0, 0.0, 0.5),
                  'meshes': [_box_mesh('body', (0.0, 0.0, -0.1), (0.2, 0.15, 0.4), [0]),
                             _box_mesh('top', (0.0, 0.0, 0.1), (0.1, 0.1, 0.1), [0])],
                  'nodes': [_node('node_body', 0, -1, 1, 1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.5), (0.0, 0.0, 0.0)),
                            _node('node_top', 1, 0, 0, -1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.3), (0.0, 0.0, 0.0))]},
        'keys': [{'name': 'sway.key', 'frames': 4,
                  'joints': [{'name': 'node_body', 'node': 0, 'entries': _entries(2, 81, frames=(0, 2))},
                             {'name': 'node_top', 'node': 1, 'entries': _entries(2, 82, frames=(0, 2))}]}],
        'extras': [],
    }
    return [(s['file'], s) for s in (box, rig, stub, leaf, bare, stand)]


def rerooted(spec, by=None):
    """The same BAF with hierarchy node 0's z moved into the insert offset (the re-root of
    --insert-offset node0): node 0's z position becomes 0 and node 0's key positions lose node
    0's z (float subtraction, before scaling); deltas and every other node stay. `by`: re-root by
    this height instead (--insert-offset auto rounds node 0's z), node 0 keeping the difference."""
    s = dict(spec)
    model = dict(spec['model'])
    nodes = [dict(n) for n in model['nodes']]
    z0 = f32(nodes[0]['pos'][2]) if by is None else f32(by)
    nodes[0]['pos'] = (nodes[0]['pos'][0], nodes[0]['pos'][1], f32(f32(nodes[0]['pos'][2]) - z0))
    model['nodes'] = nodes
    s['model'] = model
    keys = []
    for k in spec.get('keys', []):
        k2 = dict(k)
        joints = []
        for j in k['joints']:
            j2 = dict(j)
            if j['node'] == 0:
                j2['entries'] = [(fr, fl, (p[0], p[1], f32(f32(p[2]) - z0)), r, dp, dr)
                                 for fr, fl, p, r, dp, dr in j['entries']]
            joints.append(j2)
        k2['joints'] = joints
        keys.append(k2)
    s['keys'] = keys
    return s


def with_colormap(spec, palette=None):
    """The same BAF with a colormap block added. The original tool's read order then parses the
    model and the keys as a strict parser of the real layout does, so its output for this copy
    is the reference for what a has_colormap-aware parse writes for the colormap-less BAF."""
    s = dict(spec)
    s['colormap'] = {'name': 'cmap.cmp', 'palette': palette or PAL_A}
    return s


# ============================================================================ the original tool's BAF output (model)

DEFAULT_SCALE = 0.2


def fmt_f(x):
    """C "%f" of a double, as the fork's formatter prints it (round half to even; NaN spellings
    follow the Windows CRT: "-nan(ind)" for the x86 default NaN)."""
    if math.isnan(x):
        bits = struct.unpack('<Q', struct.pack('<d', x))[0]
        if bits == 0xFFF8000000000000:
            return '-nan(ind)'
        return '-nan' if bits >> 63 else 'nan'
    if math.isinf(x):
        return '-inf' if x < 0 else 'inf'
    return '%f' % x


def fmt_f32(u):
    """"%f" of a float32 given by its bits, promoted to double as C varargs do."""
    u &= 0xFFFFFFFF
    if (u & 0x7F800000) == 0x7F800000 and (u & 0x7FFFFF):
        d = ((u >> 31) << 63) | 0x7FF8000000000000 | ((u & 0x3FFFFF) << 29)
        return fmt_f(struct.unpack('<d', struct.pack('<Q', d))[0])
    return fmt_f(bits_f32(u))


def _hex(v, width=0):
    return '%0*X' % (width, v & 0xFFFFFFFF)


def _cstr(b):
    """bytes up to the first NUL (as %s prints a fixed-size field)."""
    i = b.find(b'\0')
    return b if i < 0 else b[:i]


class _Fread:
    """fread() over bytes. A short read copies what is left and zero-fills the rest (the fork
    zeroes every destination first; the original left it uninitialised)."""

    def __init__(self, data):
        self.d = data
        self.p = 0
        self.short = 0
        self.eof_tail = False

    def read(self, n):
        v = self.d[self.p:self.p + n]
        self.p += len(v)
        if len(v) < n:
            self.short += 1
            v = v + bytes(n - len(v))
        return v

    def i32(self):
        return struct.unpack('<i', self.read(4))[0]


class _Text:
    def __init__(self):
        self.parts = []

    def w(self, s):
        self.parts.append(s if isinstance(s, bytes) else s.encode('latin-1'))

    def bytes(self):
        return b''.join(self.parts).replace(b'\n', b'\r\n')


def _f32mul(a_bits, scale):
    """float * float as C computes it: the exact product (it fits a double) rounded to float."""
    return f32(bits_f32(a_bits) * f32(scale))


def _angle(a_bits, angles):
    if angles == 'raw':
        return fmt_f32(a_bits)
    return fmt_f(f32(bits_f32(a_bits) * 180.0) / math.pi)   # float product, then / M_PI in double


def _legacy_model(r, mat_names, num_mats, scale, angles, files):
    model_name = _cstr(r.read(32))
    txt = _Text()
    sep = '---------------------------------------------------------\n'
    txt.w('\n' + sep + '-------------------- HEADER ------------------------\n' + sep)
    mdunno1 = r.i32()
    num_nodes = r.i32()
    txt.w('mdunno1=%d\nnumNodes=%d\n' % (mdunno1, num_nodes))
    md = struct.unpack('<23i', r.read(92))
    for i, v in enumerate(md):
        txt.w('mdunno[%d]=%d\n' % (i, v))
    num_h = md[11]
    nodes = []
    if num_nodes > 0:
        txt.w('\n' + sep + '-------------------- NODE DEFINITIONS ------------------------\n' + sep)
        for _ in range(num_nodes):
            raw = r.read(112)
            w = struct.unpack('<32s10i3i7I', raw)
            nd = {'raw': raw, 'name': _cstr(raw), 'geo': w[2], 'light': w[3], 'tex': w[4], 'nv': w[11],
                  'nuv': w[12], 'nf': w[13]}
            txt.w('\n----------------\n')
            txt.w(b'nodeName=' + nd['name'] + b'\n')
            txt.w('someIndex1=%d\ngeoMode=%d\nlightMode=%d\ntexMode=%d\n' % (w[1], w[2], w[3], w[4]))
            for i in range(6):
                txt.w('ndunno[%d]=%d\n' % (i, w[5 + i]))
            txt.w('numVerts=%d\nnumTexVerts=%d\nnumFaces=%d\n' % (w[11], w[12], w[13]))
            for i in range(7):
                txt.w('fdunno[%d]=%s\n' % (i, fmt_f32(w[14 + i])))
            nodes.append(nd)
    txt.w('\n\n\n' + sep + '-------------------- NODES!!!!!!! ------------------------\n' + sep)
    for nd in nodes:
        txt.w(b'\nnode: ' + nd['name'] + b'\n---------------------------\n')
        nd['verts'], nd['norms'], nd['uvs'], nd['faces'] = [], [], [], []
        if nd['nv'] != 0:
            for i in range(max(0, nd['nv'])):
                v = struct.unpack('<3I', r.read(12))
                nd['verts'].append(v)
                txt.w('vert[%d]: %s/%s/%s\n' % (i, fmt_f32(v[0]), fmt_f32(v[1]), fmt_f32(v[2])))
            for i in range(max(0, nd['nv'])):
                v = struct.unpack('<3I', r.read(12))
                nd['norms'].append(v)
                fx, fy, fz = (bits_f32(c) for c in v)
                mag = f32(math.sqrt(f32(f32(f32(fx * fx) + f32(fy * fy)) + f32(fz * fz))))
                txt.w('norm[%d]: mag=%s\t%s/%s/%s\n' % (i, fmt_f(mag), fmt_f32(v[0]), fmt_f32(v[1]), fmt_f32(v[2])))
            for i in range(max(0, nd['nv'])):
                v = struct.unpack('<2I', r.read(8))
                txt.w('uvscroll?[%d]: %s/%s\n' % (i, fmt_f32(v[0]), fmt_f32(v[1])))
        if nd['nuv'] != 0:
            for i in range(max(0, nd['nuv'])):
                v = struct.unpack('<2I', r.read(8))
                nd['uvs'].append(v)
                txt.w('texvert[%d]: %s/%s\n' % (i, fmt_f32(v[0]), fmt_f32(v[1])))
        if nd['nf'] != 0:
            for i in range(max(0, nd['nf'])):
                fw = struct.unpack('<14i3I2i', r.read(76))
                fc = {'w': fw}
                nd['faces'].append(fc)
                mi = fw[0]
                mname = mat_names[mi] if 0 <= mi < len(mat_names) else b'(invalid material index)'
                rest = (('), flags=%d, geo=%d, light=%d, tex=%d, numVerts=%d, d5=0x%s, d6=0x%s, d7=0x%s, '
                         'd8=%d, d9=%d, d10=%d, d11=%d, d12=%d, norm=(%s/%s/%s), d13=%d, d14=%d\n') %
                        (fw[1], fw[2], fw[3], fw[4], fw[5], _hex(fw[6], 8), _hex(fw[7], 8), _hex(fw[8], 8),
                         fw[9], fw[10], fw[11], fw[12], fw[13], fmt_f32(fw[14]), fmt_f32(fw[15]),
                         fmt_f32(fw[16]), fw[17], fw[18]))
                txt.w(('face[%d]: matIndex=%d (' % (i, mi)).encode() + mname + rest.encode())
            for i, fc in enumerate(nd['faces']):
                n = fc['w'][5]
                pos = struct.unpack('<%di' % max(0, n), r.read(4 * max(0, n)))
                uv = struct.unpack('<%di' % max(0, n), r.read(4 * max(0, n)))
                fc['pos'], fc['uv'] = pos, uv
                txt.w('faceindices[%d]=' % i + ''.join('%d,' % v for v in pos) + ' | ' +
                      ''.join('%d,' % v for v in uv) + '\n')
    txt.w('\n\n\n' + sep + '-------------------- NODE HIERARCHY ------------------------\n' + sep)
    hnodes = []
    if num_h > 0:
        for k in range(num_h):
            raw = r.read(180)
            w = struct.unpack('<8s17i5i9I12i', raw)
            hn = {'name': _cstr(raw), 'w': w}
            hnodes.append(hn)
            txt.w(('%d: hierarchy node=' % k).encode() + hn['name'] + b'\n')
            for i in range(17):
                txt.w('hdunno[%d]=%d\t%s\n' % (i, w[1 + i], fmt_f32(w[1 + i])))
            txt.w('meshIndex=%d\nparentIndex=%d\nnumChildren=%d\nfirstChildIndex=%d\nsiblingIndex=%d\n' %
                  (w[18], w[19], w[20], w[21], w[22]))
            txt.w('pivot=%s/%s/%s\n' % tuple(fmt_f32(v) for v in w[23:26]))
            txt.w('pos=%s/%s/%s\n' % tuple(fmt_f32(v) for v in w[26:29]))
            txt.w('pyr=%s/%s/%s\n' % tuple(fmt_f32(v) for v in w[29:32]))
            for i in range(12):
                txt.w('hdunno2[%d]=%d\t%s\n' % (i, w[32 + i], fmt_f32(w[32 + i])))
            txt.w('\n')
    _legacy_tail(r, txt, sep)
    files.append((model_name + b'.txt', txt.bytes(), r.eof_tail))

    m = _Text()
    m.w(b"# MODEL '" + model_name + b"' created with TPMExtractor\n")
    m.w('\n###############\nSECTION: HEADER\n\n3DO 2.1\n\n###############\nSECTION: MODELRESOURCE\n\n'
        '# Materials list\nMATERIALS %d\n\n' % num_mats)
    for x in range(num_mats):
        m.w(('%d: ' % x).encode() + _mat16_name(mat_names[x]) + b'\n')
    m.w('\n\n###############\nSECTION: GEOMETRYDEF\n\n# Object radius\nRADIUS %s # dunno\n\n'
        '# Insertion offset\nINSERT OFFSET 0 0 %s #dunno\n\n# Number of Geometry Sets\nGEOSETS 1\n\n'
        '# Geometry Set definition\nGEOSET 0\n\n# Number of Meshes\nMESHES %d\n\n'
        % (fmt_f(f32(0.1)), fmt_f(f32(0.1)), num_nodes))
    for k, nd in enumerate(nodes):
        m.w('\n# Mesh definition\nMESH %d\n\n' % k)
        m.w(b'NAME ' + nd['name'] + b'\n\n')
        m.w('RADIUS %s # dunno\n\nGEOMETRYMODE %d\nLIGHTINGMODE %d\nTEXTUREMODE %d\n\n\nVERTICES %d\n\n'
            '# num:     x:         y:         z:         i: \n' % (fmt_f(f32(0.1)), nd['geo'], nd['light'],
                                                                  nd['tex'], nd['nv']))
        for i, v in enumerate(nd['verts']):
            m.w('%d: %s %s %s %s\n' % (i, fmt_f(_f32mul(v[0], scale)), fmt_f(_f32mul(v[1], scale)),
                                       fmt_f(_f32mul(v[2], scale)), fmt_f(0.0)))
        m.w('\n\nTEXTURE VERTICES %d\n\n' % nd['nuv'])
        for i, v in enumerate(nd['uvs']):
            m.w('%d: %s %s\n' % (i, fmt_f32(v[0]), fmt_f32(v[1])))
        m.w('\n\nVERTEX NORMALS\n\n# num:     x:         y:         z:\n')
        for i, v in enumerate(nd['norms']):
            m.w('%d: %s %s %s\n' % (i, fmt_f32(v[0]), fmt_f32(v[1]), fmt_f32(v[2])))
        m.w('\n\nFACES %d\n\n#  num:  material:   type:  geo:  light:   tex:  extralight:  verts:\n' % nd['nf'])
        for i, fc in enumerate(nd['faces']):
            fw = fc['w']
            m.w('%d: %d 0x%s %d %d %d %s %d' % (i, fw[0], _hex(fw[1]), fw[2], fw[3], fw[4], fmt_f(0.0), fw[5]))
            for vi in range(max(0, fw[5])):
                m.w(' %d, %d' % (fc['pos'][vi], fc['uv'][vi]))
            m.w('\n')
        m.w('\n\nFACE NORMALS\n\n# num:     x:         y:         z:\n')
        for i, fc in enumerate(nd['faces']):
            fw = fc['w']
            m.w('%d: %s %s %s\n' % (i, fmt_f32(fw[14]), fmt_f32(fw[15]), fmt_f32(fw[16])))
    m.w('\n\n###############\nSECTION: HIERARCHYDEF\n\n# Hierarchy node list\nHIERARCHY NODES %d\n\n' % num_h)
    m.w('#  num:   flags:   type:    mesh:  parent:  child:  sibling:  numChildren:        x:         y:         z:'
        '     pitch:       yaw:      roll:    pivotx:    pivoty:    pivotz:  hnodename:\n')
    for k, hn in enumerate(hnodes):
        w = hn['w']
        m.w('%d: 0x%X 0x%X %d %d %d %d %d %s %s %s %s %s %s %s %s %s ' %
            (k, 0, 1, w[18], w[19], w[21], w[22], w[20],
             fmt_f(_f32mul(w[26], scale)), fmt_f(_f32mul(w[27], scale)), fmt_f(_f32mul(w[28], scale)),
             _angle(w[29], angles), _angle(w[30], angles), _angle(w[31], angles),
             fmt_f(_f32mul(w[23], scale)), fmt_f(_f32mul(w[24], scale)), fmt_f(_f32mul(w[25], scale))))
        m.w(hn['name'] + b'\n')
    m.w('\n')
    files.append((model_name, m.bytes(), False))


def _legacy_tail(r, txt, sep):
    txt.w('\n\n\n' + sep + '-------------------- TAIL 64bytes ------------------------\n' + sep)
    before = r.p
    tail = struct.unpack('<16I', r.read(64))
    r.eof_tail = (r.p - before) < 64
    for i, v in enumerate(tail):
        txt.w('tail[%d]=%d\t%s\n' % (i, i32_of(v), fmt_f32(v)))


def _legacy_key(r, scale, angles, files):
    raw = r.read(132)
    w = struct.unpack('<32s7i18I', raw)
    name = _cstr(raw)
    t = _Text()
    t.w('dunno1: %d\ndunno2: %d\ndunno3: %d\ntype: %d\nfps: %s\nnumFrames: %d\nnumNodes: %d\n' %
        (w[1], w[2], w[3], w[4], fmt_f32(w[5] & 0xFFFFFFFF), w[6], w[7]))
    for i in range(18):
        t.w('bleh[%d]=%d\t%s\n' % (i, i32_of(w[8 + i]), fmt_f32(w[8 + i])))
    nodes = []
    if w[7] > 0:
        for _ in range(w[7]):
            nraw = r.read(44)
            nw = struct.unpack('<32s2iI', nraw)
            nodes.append({'name': _cstr(nraw), 'frames': nw[2], 'entries': []})
            t.w(b'node=' + _cstr(nraw) + ('\t%d\t%d\n' % (nw[1], nw[2])).encode())
        for nd in nodes:
            for k in range(max(0, nd['frames'])):
                e = struct.unpack('<I1i12I', r.read(56))
                nd['entries'].append(e)
                v = [fmt_f32(x) for x in e[2:]]
                t.w('frame[%d]: %s\t%d\tpos=%s/%s/%s\tpyr=%s/%s/%s\tdeltapos=%s/%s/%s\tdeltapyr=%s/%s/%s\n' %
                    (k, fmt_f32(e[0]), e[1], *v))
    sep = '---------------------------------------------------------\n'
    _legacy_tail(r, t, sep)
    files.append((name + b'.txt', t.bytes(), r.eof_tail))

    k = _Text()
    k.w(b"# KEYFRAME '" + name + b"' created with TPMExtractor\n")
    k.w('\n###############\nSECTION: HEADER\n\nFLAGS 0x0\nTYPE 0x%s\nFRAMES %d\nFPS %s\nJOINTS %d\n\n\n'
        '###############\nSECTION: KEYFRAME NODES\n\nNODES %d\n\n' %
        (_hex(w[4], 4), w[6], fmt_f32(w[5] & 0xFFFFFFFF), w[7], w[7]))
    for i, nd in enumerate(nodes):
        k.w(('NODE %d\nMESH NAME ' % i).encode() + nd['name'] + ('\nENTRIES %d\n\n' % nd['frames']).encode())
        k.w('# num:   frame:   flags:           x:           y:           z:           p:           y:           r:\n'
            '#                                 dx:          dy:          dz:          dp:          dy:          dr:\n')
        for x, e in enumerate(nd['entries']):
            k.w('%d:\t%d\t0x%s\t%s\t%s\t%s\t%s\t%s\t%s\n' %
                (x, int(bits_f32(e[0])), _hex(e[1], 4), fmt_f(_f32mul(e[2], scale)), fmt_f(_f32mul(e[3], scale)),
                 fmt_f(_f32mul(e[4], scale)), _angle(e[5], angles), _angle(e[6], angles), _angle(e[7], angles)))
            k.w('\t\t\t%s\t%s\t%s\t%s\t%s\t%s\n' %
                (fmt_f(_f32mul(e[8], scale)), fmt_f(_f32mul(e[9], scale)), fmt_f(_f32mul(e[10], scale)),
                 _angle(e[11], angles), _angle(e[12], angles), _angle(e[13], angles)))
        k.w('\n')
    files.append((name, k.bytes(), False))


def _mat16_name(name):
    return (name[:-4] if len(name) >= 4 else name) + b'16.mat'


def mat8_name(name):
    """The default mode's 8-bit MAT name: the MAT name minus its last 4 characters + "8.mat"."""
    if isinstance(name, str):
        name = name.encode('latin-1')
    return (name[:-4] if len(name) >= 4 else name) + b'8.mat'


def _bmp_name(name):
    return name[:-3] + b'bmp' if len(name) >= 3 else name + b'.bmp'


def bmp24(w, h, pixels_rgb, padded):
    """24-bit top-down BMP as the tool writes it; padded=False reproduces the original's
    unpadded rows."""
    row = w * 3
    stride = (row + 3) & ~3 if padded else row
    image = bytearray()
    for y in range(h):
        for (r, g, b) in pixels_rgb[y * w:(y + 1) * w]:
            image += bytes((b, g, r))
        image += bytes(stride - row)
    head = struct.pack('<2sIHHI', b'BM', 54 + len(image), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 24, 0, len(image) if padded else w * h * 3, 0, 0, 0, 0)
    return head + info + bytes(image)


def _mat16(w, h, buf, trans, pal):
    has_t = trans and any(p == 0 for p in buf[:w * h])
    if has_t:
        fmt = (0, 16, 5, 5, 5, 0, 5, 10, 3, 3, 3, 1, 15, 1)
    else:
        fmt = (1, 16, 5, 6, 5, 0, 5, 11, 3, 2, 3, 0, 0, 0)
    o = Out()
    o.raw(b'MAT ')
    o.i32(0x32, 2, 1, 1, *fmt)
    o.i32(8, 0)
    o.u32(*([0x3F800000] * 4))
    o.i32(0, 0)
    o.u32(0xBFF78482)
    o.i32(0, w, h, 1 if trans else 0, 0, 0, 1)

    def q(c, n):
        return int(f32(f32(c / 255.0) * float(n)))

    tex = []
    for p in buf[:w * h]:
        r, g, b = pal[p]
        if has_t:
            tex.append(0 if (trans and p == 0) else (0x8000 | q(r, 31) << 10 | q(g, 31) << 5 | q(b, 31)))
        else:
            tex.append(q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31))
    o.raw(struct.pack('<%dH' % len(tex), *tex))
    return o.bytes()


def legacy_baf_outputs(data, scale=DEFAULT_SCALE, angles='raw', fallback_palette=None, padded_bmp=False,
                       mat16=True, bmp=True, debug_dumps=True):
    """What the original tool's dumpbaf() writes for the BAF `data`, emulated from its source
    (the same read order, including its known misreads). Returns a list of (name, bytes,
    undefined_tail) in write order; a later write to a name equal ignoring case replaces the
    earlier content and keeps the first spelling (NTFS). undefined_tail marks debug files whose
    last 64-byte read hit the end of the file: the original printed uninitialised memory there,
    the fork prints zeros.
    The optional arguments describe default-mode variations (scale, angle mode, padded BMP rows,
    --no-mat16, --no-bmp, --no-debug-dumps, a fallback palette); the defaults are --legacy."""
    r = _Fread(data)
    files = []
    r.read(4 + 4)
    r.read(32)
    r.read(128)
    r.read(28)
    num_mats = r.i32()
    num_keys = r.i32()
    num_models = r.i32() & 0xFFFFFF
    r.read(28)
    r.read(12)
    r.read(40)
    cmp_name = _cstr(r.read(32))
    nocolormap = False
    if cmp_name and cmp_name[-1:] == b't':
        pal = list(fallback_palette or grayscale_palette())
        nocolormap = True
    else:
        r.read(16)
        pal = [tuple(r.read(3)) for _ in range(256)]
        r.read(28)
        files.append((cmp_name, legacy_cmp_bytes(pal), False))
    mat_names = []
    for mi in range(num_mats):
        name = cmp_name if (nocolormap and mi == 0) else _cstr(r.read(32))
        mat_names.append(name)
        raw = bytearray()
        hdr = r.read(76)
        raw += hdr
        _, _, mtype, ncel, ntex, _, bpp = struct.unpack_from('<4s6i', hdr)
        for _ in range(ncel):
            ch = r.read(24)
            raw += ch
            if struct.unpack_from('<i', ch)[0] == 8:
                raw += r.read(16)
        firsts = []
        for _ in range(ntex):
            th = r.read(24)
            raw += th
            w, h, tr, _, _, nm = struct.unpack('<6i', th)
            size = (w * h * bpp) // 8 if w * h * bpp >= 0 else -((-w * h * bpp) // 8)
            for k in range(nm):
                m = r.read(size)
                raw += m
                if k == 0:
                    firsts.append((w, h, tr != 0, m))
                size = size // 4 if size >= 0 else -((-size) // 4)
        files.append((name, bytes(raw), False))
        mpal = pal
        if mtype == 3:
            mpal = [tuple(r.read(3)) for _ in range(256)]
        if firsts:
            w, h, trans, m = firsts[0]
            rgb = [(255, 0, 255) if (trans and p == 0) else mpal[p] for p in m[:w * h]]
            if bmp:
                files.append((_bmp_name(name), bmp24(w, h, rgb, padded_bmp), False))
            if mat16:
                files.append((_mat16_name(name), _mat16(w, h, m, trans, mpal), False))
    for _ in range(num_models):
        _legacy_model(r, mat_names, num_mats, scale, angles, files)
    for _ in range(num_keys):
        _legacy_key(r, scale, angles, files)
    if debug_dumps:
        files.append((b'REMAIN.HEX', data[r.p:], False))
    else:
        files = [f for f in files if not (f[0].endswith(b'.3do.txt') or f[0].endswith(b'.key.txt'))]
    return files


def resolve_writes(files):
    """Applies NTFS name semantics to a write list: {lower-case name: (first spelling, last data,
    undefined_tail)}."""
    out = {}
    for name, data, undef in files:
        key = name.lower()
        if key in out:
            out[key] = (out[key][0], data, undef)
        else:
            out[key] = (name, data, undef)
    return out


# ============================================================================ B3D levels

B3D_RAW_HEADER = 0x8A4
B3D_FLAG_OFFSET = 0x328
B3D_VERSION = 293


def _sec(tag, body, size_field=None):
    assert len(tag) == 8
    return tag.encode('ascii') + struct.pack('<I', len(body) if size_field is None else size_field) + body


def _size_table(records):
    return struct.pack('<%dI' % len(records), *(len(r) for r in records)) + b''.join(records)


def build_level(spec):
    """A decompressed level image (the bytes --keep-decompressed writes) for `spec`, with the
    "compressed" flag at 0x328 set as spec['flag'] says (1 = compressed file)."""
    pals = spec['palettes']
    texs = spec['textures']
    grid_w, grid_h = spec.get('grid', (3, 2))
    lit1_n, lit2_n = spec.get('lit1', 2), spec.get('lit2', 1)
    epla_n = spec.get('epla', 1)
    enmy_n, ai_n, adef_n, amap_n = 1, 2, 1, 1
    anam = [b'C:\\fixtures\\box.baf\0', b'C:\\fixtures\\rig.baf\0']
    sref_n, scal_n, spla_n, dlg_n = 2, 3, 1, 4
    rng = Lcg(spec.get('seed', 7))

    # game mesh: one non-empty cell (index 1) with two polygons over four vertices
    gvrt = [(1.0, 0.0, 0.25), (2.0, 0.0, 0.25), (2.0, 1.0, 0.75), (1.0, 1.0, 0.75)]
    gmap = Out()
    cells = grid_w * grid_h
    nonempty = {1}
    for c in range(cells):
        if c not in nonempty:
            gmap.u8(0xFF)
            continue
        gmap.u8(2, 0, 0, 0)
        gmap.f32(0.25, 0.75)
        gmap.u32(0)
        for k in range(2):
            e = Out()
            e.u16(0, 1, 2, 0 if k == 0 else 3)
            e.u16(0)
            e.u8(0, 0, 0, 0, 0, 0)
            e.f32(0.0, -0.447214, 0.894427)
            e.u8(0, 0, 0, 0)
            e.u16(k, k)
            e.u8(0, 3 if k == 0 else 4, 0, 0, 0, 0, 0, 0)
            e.u16(0)
            e.u8(0, 0)
            e.u16(0)
            e.u16(0)
            e.u16(0)
            e.u8(0)
            e.raw(bytes(9))
            assert len(e) == 64, len(e)
            gmap.raw(e.bytes())
    mats = Out()
    mat_n = max(2, len(texs))
    for k in range(mat_n):
        mats.f32(0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0)
        mats.i32(k % len(texs))
        mats.u32((0, 4)[k % 2])
    tex = Out()
    for t in texs:
        tex.i32(t['w'], t['h'], t['pal'], 0)
        tex.raw(t['pix'])
    lit = lambda i: struct.pack('<4i3fI2f5I3f', 30 + i, 3, i & 1, 0, 1.5, 0.5, 2.0, 0, 1.0, 5.0, 0, 0, 0, 0, 0,
                                255.0, 200.0, 100.0)
    lit1 = b''.join(lit(i) for i in range(lit1_n))
    lit2 = b''.join(lit(100 + i) for i in range(lit2_n))
    adef = _size_table([bytes(88)])
    amap = _size_table([bytes(156)])
    enmy_rec = bytearray(252 + 8)
    struct.pack_into('<ii', enmy_rec, 0xA4, 0, 1)
    struct.pack_into('<I3f', enmy_rec, 0xE8, 1, 1.5, 0.5, 0.25)
    enmy = _size_table([bytes(enmy_rec)])
    ai = _size_table([bytes(48), bytes(56)])
    anam_sec = _size_table(anam)
    sref = b''.join(struct.pack('<I40s16x', 0, name_bytes(n, 40)) for n in (b'step.wav', b'hum.wav'))
    scal = b''.join(struct.pack('<16sI44x', name_bytes(n, 16), i) for i, n in enumerate((b'step.wav', b'', b'hum.wav')))
    spla = b''.join(struct.pack('<IIII3f24x', 0, 0, 0, i, 1.5, 0.5, 0.0) for i in range(spla_n))
    epla = b''.join(struct.pack('<3f68x', 2.5, 0.5, 0.0) for _ in range(epla_n))
    dlg = b''.join(struct.pack('<iIII', i, 0, 0, 0) for i in range(dlg_n))
    # editor sections
    tmap = Out()
    tmap_cells = tmap_refs = 0
    for c in range(cells):
        if c not in nonempty:
            tmap.u8(0xFF)
            continue
        refs = 2
        tmap.u8(refs, 0, 1, 0)
        tmap.f32(0.25, 0.75)
        tmap_cells += 1
        tmap_refs += refs
        for k in range(refs):
            tmap.u8(0, 0)
            tmap.u16(1 + k)
            tmap.u32(0, 1)
            tmap.i32(-1)
            tmap.u32(0)
    tlib = Out()
    tlib.u32(1, 0, 0, 0, 0, 0)
    for k in range(2):
        tlib.u8(5, 1)
        tlib.u16(0)
        tlib.u32(0, 0, 0)
        tlib.f32(1.0)
        tlib.u32(0)
        tlib.u16(k)
    tlib.raw(bytes(6))
    tply = Out()
    for k in range(3):
        if k == 2:
            tply.u8(0)
            continue
        tply.u8(1, 4 if k else 3)
        tply.u16(0)
        tply.f32(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.5, 0.0, 1.0, 0.5)
        tply.f32(0.0, -0.447214, 0.894427)
        tply.u16(k, k)
        tply.raw(bytes(28))
    tlit = bytes(68 * (lit1_n + lit2_n))
    tenm = bytes(8576 * enmy_n)
    tai = _size_table([bytes(128) for _ in range(ai_n)])
    tadf = _size_table([bytes(16)])
    tamp = _size_table([bytes(20)])
    grad = bytes(rng.below(256) for _ in range(512))
    tplc = b''.join(struct.pack('<IIII3f36x', 0, 0, 0, i, 2.5, 1.5, 0.0) for i in range(spla_n))

    H = bytearray(2200)

    def put(off, fmt, *v):
        struct.pack_into('<' + fmt, H, off, *v)

    put(0x000, 'I', B3D_VERSION)
    put(0x004, 'f', spec.get('light_level', 0.0))
    put(0x008, '6f', 1.5, 0.5, 0.5, 0.0, 90.0, 0.0)
    put(0x020, 'I', len(pals))
    put(0x028, 'II', grid_w, grid_h)
    put(0x030, 'ii', 0, 1)
    put(0x038, 'IIII', len(nonempty), 80, 2 * len(nonempty), 64)
    put(0x048, 'IIII', len(gvrt), 12, 1, 12)
    put(0x058, 'IIII', mat_n, 40, len(texs), 0)
    put(0x068, 'IIII', lit1_n, 72, lit2_n, 72)
    put(0x080, 'IIIIff', spec.get('origin', (2, 1))[0], spec.get('origin', (2, 1))[1], 1, 0x00402010, 8.0, 20.0)
    put(0x098, 'IIII', ai_n, len(ai), enmy_n, 252)
    put(0x0A8, 'IIIIII', len(anam), len(anam_sec), adef_n, 4, amap_n, 4)
    put(0x0C0, 'iIf', 1, 0x00102030, 0.5)
    put(0x2CC, 'IIIIIIIIIII', len(enmy), sref_n, 0, spla_n, 0, scal_n, 0, epla_n, 80, dlg_n, len(dlg))
    put(0x310, 'IIIII', 0, 2, 0, spec.get('flag', 1), 2)
    H[0x7E0:0x7E8] = name_bytes(spec.get('author', b'fixtr'), 8)
    put(0x804, '5f', 50.0, 40.0, 20.0, 320.0, 30.0)
    put(0x81C, 'IIIIII', 400, 200, tmap_cells, 12, tmap_refs, 20)
    put(0x834, 'IIIIII', 3, 3, 64, 4, 3, 96)
    put(0x84C, 'III', 68, 0, 20)
    put(0x858, 'II', enmy_n, 8576)
    put(0x864, 'I', 436)
    put(0x86C, 'IIIIIiIIIII', 128, adef_n, 2, amap_n, 3, 1, spla_n, 64, scal_n, 64, 80)

    sections = [
        _sec('B3D FILE', bytes(H)),
        _sec('B3D_PALS', b''.join(palette_bytes(p) for p in pals)),
        _sec('B3D_GMAP', gmap.bytes()),
        _sec('B3D_GVRT', b''.join(struct.pack('<3f', *v) for v in gvrt)),
        _sec('B3D_LVRT', bytes(12)),
        _sec('B3D_MAT ', mats.bytes()),
        _sec('B3D_TEX ', tex.bytes()),
        _sec('B3D_LIT1', lit1),
    ]
    if lit2_n:
        sections.append(_sec('B3D_LIT2', lit2))
    sections += [_sec('B3D_ADEF', adef), _sec('B3D_AMAP', amap), _sec('B3D_ENMY', enmy), _sec('B3D_AI  ', ai),
                 _sec('B3D_ANAM', anam_sec), _sec('B3D_SREF', sref), _sec('B3D_SCAL', scal), _sec('B3D_SPLA', spla)]
    if epla_n:
        sections.append(_sec('B3D_EPLA', epla))
    sections += [_sec('B3D_DLG ', dlg), _sec('B3D_END1', b''),
                 _sec('B3D TMAP', tmap.bytes(), cells + 12 * tmap_cells + 20 * tmap_refs),
                 _sec('B3D TLIB', tlib.bytes()),
                 _sec('B3D_TPLY', tply.bytes()),
                 _sec('B3D_TLIT', tlit, 68 * lit1_n),
                 _sec('B3D_TENM', tenm), _sec('B3D_TAI ', tai), _sec('B3D_TADF', tadf), _sec('B3D_TAMP', tamp),
                 _sec('B3D_GRAD', grad), _sec('B3D_TSRF', sref), _sec('B3D_TPLC', tplc), _sec('B3D_TCLL', scal)]
    if epla_n:
        sections.append(_sec('B3D_TEPL', epla))
    sections.append(_sec('B3D_END2', b''))
    return b''.join(sections)


def compress_level(image, mode='greedy'):
    """The .B3D file for a level image: the raw header as it is, then the LZSS stream."""
    return image[:B3D_RAW_HEADER] + lzss_encode(image[B3D_RAW_HEADER:], mode)


def dec_of(image):
    """The decompressed image of a compressed level as the tool writes it (flag byte cleared)."""
    d = bytearray(image)
    d[B3D_FLAG_OFFSET] = 0
    return bytes(d)


def walk_sections(image):
    """[(tag, offset, size_field, extent)] for every section; raises ValueError if the chain does
    not end exactly at the end of the image."""
    H = image[12:12 + 2200]
    hv = lambda off: struct.unpack_from('<I', H, off)[0]
    pos, out = 0, []
    while pos < len(image):
        if pos + 12 > len(image):
            raise ValueError('section header cut off at 0x%x' % pos)
        tag = image[pos:pos + 8]
        size = struct.unpack_from('<I', image, pos + 8)[0]
        body = pos + 12
        if tag == b'B3D TMAP':
            p = body
            for _ in range(hv(0x28) * hv(0x2C)):
                p += 1 if image[p] == 0xFF else 12 + 20 * image[p]
            extent = p - body
        elif tag == b'B3D_TLIT':
            extent = (hv(0x68) + hv(0x70)) * hv(0x84C)
        else:
            extent = size
        out.append((tag.decode('latin-1'), pos, size, extent))
        pos = body + extent
    if pos != len(image):
        raise ValueError('chain ends at 0x%x, image is 0x%x bytes' % (pos, len(image)))
    return out


def _level_textures(image, pals_at, pals_size, tex_at, tex_end):
    pals = [palette_from_bytes(image[pals_at + 768 * k:pals_at + 768 * (k + 1)]) for k in range(pals_size // 768)]
    out = []
    p = tex_at
    while p < tex_end:
        w, h, pal, _ = struct.unpack_from('<4i', image, p)
        pix = image[p + 16:p + 16 + w * h]
        out.append((w, h, [pals[pal][i] for i in pix]))
        p += 16 + w * h
    return out


def legacy_b3d_outputs(dec):
    """The BMPs the original b3dexperiment() writes from a decompressed image: [(name, bytes)].
    It looks for the first "B3D_PALS" and "B3D_TEX" bytes anywhere and reads from there."""
    pals_at, tex_at = dec.find(b'B3D_PALS'), dec.find(b'B3D_TEX')
    if pals_at < 0 or tex_at < 0:
        return None
    pals_size = struct.unpack_from('<i', dec, pals_at + 8)[0]
    tex_end = tex_at + struct.unpack_from('<i', dec, tex_at + 8)[0]
    texs = []
    p = tex_at + 12
    pals = [palette_from_bytes(dec[pals_at + 12 + 768 * k:pals_at + 12 + 768 * (k + 1)])
            for k in range(pals_size // 768)]
    while tex_end - p > 0:
        w, h, pal, _ = struct.unpack_from('<4i', dec, p)
        pix = dec[p + 16:p + 16 + w * h]
        texs.append((w, h, [pals[pal][i] for i in pix]))
        p += 16 + w * h
    return [('%d.bmp' % i, bmp24(w, h, rgb, False)) for i, (w, h, rgb) in enumerate(texs)]


def level_bmps(image, padded):
    """The textures of a level found by walking its sections: [(name, bmp bytes)]."""
    secs = {s[0]: s for s in walk_sections(image)}
    _, pals_off, pals_size, _ = secs['B3D_PALS']
    _, tex_off, tex_size, _ = secs['B3D_TEX ']
    texs = _level_textures(image, pals_off + 12, pals_size, tex_off + 12, tex_off + 12 + tex_size)
    return [('%d.bmp' % i, bmp24(w, h, rgb, padded)) for i, (w, h, rgb) in enumerate(texs)]


def level_fixtures():
    """The synthetic level set: {file name: spec}. Generic names."""
    t0 = {'w': 8, 'h': 8, 'pal': 0, 'pix': tex_checker(8, 8, 3, 200)}
    t1 = {'w': 6, 'h': 4, 'pal': 1, 'pix': tex_gradient(6, 4, 0, 255)}       # 18-byte rows: padding differs
    t2 = {'w': 16, 'h': 16, 'pal': 0, 'pix': tex_gradient(16, 16, 32, 95)}
    t3 = {'w': 3, 'h': 5, 'pal': 0, 'pix': tex_gradient(3, 5, 100, 140)}
    return {
        'BOX.B3D': {'palettes': [PAL_A, PAL_LEVEL], 'textures': [t0, t1, t2], 'lit2': 1, 'epla': 1, 'seed': 1},
        'FLAT.B3D': {'palettes': [PAL_LEVEL], 'textures': [dict(t3), dict(t0, pal=0)], 'lit2': 0, 'epla': 0,
                     'grid': (2, 2), 'seed': 2},
        'DECOY.B3D': {'palettes': [PAL_A, PAL_LEVEL], 'textures': [t0, t1], 'author': b'B3D_TEX\0', 'seed': 3},
    }


def level_file(name, spec, compressed=True):
    """(file bytes, decompressed image) for a level spec."""
    image = build_level(dict(spec, flag=1 if compressed else 0))
    return (compress_level(image) if compressed else image), image


# ============================================================================ B3D level geometry
#
# The SHAPES vector: a level whose geometry covers the decoder's cases with abstract shapes,
# generated palettes and generated pixels. Its bytes are pinned by SHAPES_DEC_SHA256 and
# SHAPES_B3D_SHA256, and the OBJ/MTL default mode writes for it (scale 0.2) by the hashes of
# their data lines (comment and blank lines dropped, '\n' line ends). Polygons in GMAP order:
#   #0  floor quad: stored clockwise, so reversed; shares 2 welded vertices with #6
#   #1  floor quad on a texel-centre sub-rectangle, UV symmetry 5
#   #2  vertical triangle, normal -Y; triangle corner subset 2; UVs tiling to u = 2
#   #3  two-sided wall quad, normal +X, mirrored back mapping
#   #4  zero-area triangle whose stored normal is (-inf, NaN, NaN): facing class 6 gives (0,0,-1)
#   #5  triangle flagged as moving with object 5, which does not exist: static
#   #6  floor quad stored counter-clockwise: kept in its order
#   #7  translucent floor quad (blend 16): textured with its mapping's page
#   #8  floor quad with a texture animation over mappings 4..5
#   #9  quad spanning cells (0,2)-(1,2): LINK target id 1, LINK entry in cell (1,2)
#   #10 opaque quad whose own texture index (2) differs from its mapping's page (1)
#   #11 blend 31 decal quad with mapping flag bit 2
#   #12 quad moving with object 0 node 0 (category amap0)
#   #13 hidden triangle (category hidden)

SHAPES_GRID = (5, 3)
SHAPES_PALETTES = [
    [(i, (3 * i) & 0xFF, 255 - i) for i in range(256)],
    [(255 - i, i, (5 * i) & 0xFF) for i in range(256)],
]
# B3D_TEX: (width, height, palette, pixel(x, y))
SHAPES_TEXTURES = [
    (8, 8, 0, lambda x, y: (y * 8 + x) * 4),
    (8, 4, 1, lambda x, y: (x * 32 + y * 8) & 0xFF),
    (4, 4, 0, lambda x, y: 255 - (y * 4 + x) * 16),
]
# B3D_MAT: (corner uvs (u, v) x 4, texture page, flags)
SHAPES_MAPPINGS = [
    (((0, 0), (1, 0), (1, 1), (0, 1)), 0, 0),                                          # M0 the whole page 0
    (((0.0625, 0.5625), (0.4375, 0.5625), (0.4375, 0.9375), (0.0625, 0.9375)), 0, 0),  # M1 texel centres
    (((0, 0), (2, 0), (2, 1), (0, 1)), 1, 0),                                          # M2 tiles twice in u
    (((0.5, 0), (1, 0), (1, 1), (0.5, 1)), 1, 0),                                      # M3 right half of page 1
    (((0, 0), (0.5, 0), (0.5, 0.5), (0, 0.5)), 2, 0),                                  # M4 animation frame 0
    (((0.5, 0.5), (1, 0.5), (1, 1), (0.5, 1)), 2, 0),                                  # M5 animation frame 1
    (((0, 0), (1, 0), (1, 1), (0, 1)), 1, 0),                                          # M6 the whole page 1
    (((0.5, 0), (1, 0), (1, 0.5), (0.5, 0.5)), 0, 4),                                  # M7 flag bit 2
    (((0, 0), (1, 0), (1, 1), (0, 1)), 2, 8),                                          # M8 flag bit 3, unused
    (((1, 0), (0, 0), (0, 1), (1, 1)), 0, 0),                                          # M9 mirrored (back side)
]
# B3D_GVRT: 50 vertices; 28 and 38 are the same position on purpose
SHAPES_VERTICES = [
    (0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0),
    (1, 0, 0.5), (1, 1, 0.5), (2, 1, 0.5), (2, 0, 0.5),
    (1.25, 0.25, 0.5), (1.5, 0.25, 1.5), (1.75, 0.25, 0.5),
    (2.5, 0, 0), (2.5, 0, 1), (2.5, 1, 1), (2.5, 1, 0),
    (3.25, 0.5, 0), (3.5, 0.5, 0), (3.75, 0.5, 0),
    (3.25, 0.75, 0.25), (3.75, 0.75, 0.25), (3.5, 0.25, 0.25),
    (1, 2, 0), (0, 2, 0),
    (2, 1, 0.25), (2, 2, 0.25), (3, 2, 0.25), (3, 1, 0.25),
    (3, 1, 0), (3, 2, 0), (4, 2, 0), (4, 1, 0),
    (0.5, 2.25, 1), (0.5, 2.75, 1), (1.5, 2.75, 1), (1.5, 2.25, 1),
    (2, 2, 0), (2, 3, 0), (3, 3, 0), (3, 2, 0),
    (2.25, 2.25, 0.25), (2.25, 2.75, 0.25), (2.75, 2.75, 0.25), (2.75, 2.25, 0.25),
    (3, 2.5, 0), (3, 2.5, 1), (4, 2.5, 1), (4, 2.5, 0),
    (3.25, 2.75, 0), (3.5, 2.75, 0.5), (3.75, 2.75, 0),
]
# Polygons in GMAP order. Fields not given are 0, except tile_sx = tile_sy = 1.
#   cell, v, n (normal), mf/mb (front/back mapping), tb (own texture), uvm (uv_mode), facing, extras
SHAPES_POLYS = [
    dict(cell=(0, 0), v=(0, 1, 2, 3), n=(0, 0, 1), mf=0, mb=0, tb=0, uvm=0x00, facing=5),
    dict(cell=(1, 0), v=(4, 5, 6, 7), n=(0, 0, 1), mf=1, mb=1, tb=0, uvm=0x05, facing=5),
    dict(cell=(1, 0), v=(8, 9, 10), n=(0, -1, 0), mf=2, mb=2, tb=1, uvm=0xA3, facing=4, coll=0x0001, tile_h=2),
    dict(cell=(2, 0), v=(11, 12, 13, 14), n=(1, 0, 0), mf=0, mb=9, tb=0, uvm=0x01, facing=1, render=0x0002,
         coll=0x0001),
    dict(cell=(3, 0), v=(15, 16, 17), n='nan', mf=0, mb=0, tb=0, uvm=0x81, facing=6, coll=0x8000),
    dict(cell=(3, 0), v=(18, 19, 20), n=(0, 0, 1), mf=1, mb=1, tb=0, uvm=0x96, facing=5, obj=0x04, node=0,
         obj_index=5),
    dict(cell=(0, 1), v=(1, 2, 21, 22), n=(0, 0, 1), mf=0, mb=0, tb=0, uvm=0x03, facing=5),
    dict(cell=(2, 1), v=(23, 24, 25, 26), n=(0, 0, 1), mf=3, mb=3, tb=1, uvm=0x02, facing=5, blend=16,
         coll=0x0008, surf=0xE000),
    dict(cell=(3, 1), v=(27, 28, 29, 30), n=(0, 0, 1), mf=4, mb=5, tb=2, uvm=0x00, facing=5, render=0x0001,
         speed=500),
    dict(cell=(0, 2), v=(31, 32, 33, 34), n=(0, 0, 1), mf=2, mb=2, tb=1, uvm=0x04, facing=5, render=0x0100,
         link_id=1, tile_sx=2, z_floor=1),
    dict(cell=(2, 2), v=(35, 36, 37, 38), n=(0, 0, 1), mf=6, mb=6, tb=2, uvm=0x06, facing=5),
    dict(cell=(2, 2), v=(39, 40, 41, 42), n=(0, 0, 1), mf=7, mb=7, tb=0, uvm=0x07, facing=5, blend=31,
         coll=0x8000),
    dict(cell=(3, 2), v=(43, 44, 45, 46), n=(0, -1, 0), mf=0, mb=0, tb=0, uvm=0x00, facing=4, obj=0x04, node=0,
         obj_index=0, coll=0x0001),
    dict(cell=(3, 2), v=(47, 48, 49), n=(0, -1, 0), mf=1, mb=1, tb=0, uvm=0xB5, facing=4, obj=0x10, node=0,
         obj_index=0, coll=0x0001),
]
SHAPES_LINKS = {(1, 2): [(0, 2, 1)]}                       # cell -> [(cell_x, cell_y, link id)]
SHAPES_BLOCK_FLAGS = {(3, 0): 0x02, (0, 2): 0x04, (1, 2): 0x08, (3, 2): 0x02}

SHAPES_DEC_SHA256 = 'dcc5ccf43885fee2841f429495c48500e5cec46473672852c2eb390680a9ed1a'
SHAPES_B3D_SHA256 = '428a1d05f0e2aed75aad491bffcc45057d90d8b85ea693aaf5ab32e0da9fc3a6'
SHAPES_OBJ_DATA_SHA256 = 'a5c849e5e40358fd694572c792f051e6ba280e669994fe6e35501f08eb7ed9f6'
SHAPES_MTL_DATA_SHA256 = 'cae0963fcceb0235673fd2009bb5e2ffb36e062e9f2b3e7e712a09c4ce62ec49'


def _shapes_poly_entry(p):
    e = bytearray(64)
    v = list(p['v']) + [0] * (4 - len(p['v']))
    struct.pack_into('<4H', e, 0x00, *v)
    if p['n'] == 'nan':
        struct.pack_into('<3I', e, 0x10, 0xFF800000, 0xFFC00000, 0xFFC00000)   # -inf, NaN, NaN
    else:
        struct.pack_into('<3f', e, 0x10, *p['n'])
    zs = [SHAPES_VERTICES[i][2] for i in p['v']]
    z_floor = p.get('z_floor', math.floor(min(zs)))
    e[0x1C] = z_floor
    struct.pack_into('<hH', e, 0x20, p['mf'], p['mb'])
    e[0x24] = p['tb']
    e[0x25] = len(p['v'])
    e[0x26] = p['uvm']
    e[0x27] = p['facing']
    e[0x28] = p.get('obj', 0)
    e[0x29] = p.get('node', 0)
    e[0x2A] = p.get('obj_index', 0)
    e[0x2B] = p.get('blend', 0)
    struct.pack_into('<H', e, 0x2C, p.get('render', 0))
    struct.pack_into('<H', e, 0x30, p.get('link_id', 0))
    struct.pack_into('<h', e, 0x32, p.get('speed', 0))
    e[0x39] = p.get('tile_sx', 1)
    e[0x3A] = p.get('tile_sy', 1)
    e[0x3B] = p.get('tile_h', max(1, math.ceil(max(zs) - z_floor)))
    struct.pack_into('<HH', e, 0x3C, p.get('coll', 0), p.get('surf', 0))
    return bytes(e)


def _shapes_link_entry(records):
    e = bytearray(64)
    struct.pack_into('<HBB', e, 0, 0xFFFF, len(records), 0)
    for k, (cx, cy, lid) in enumerate(records):
        struct.pack_into('<hhII', e, 4 + 12 * k, cx, cy, 0, lid)
    return bytes(e)


def _shapes_gmap():
    out = bytearray()
    blocks = entries = 0
    for cy in range(SHAPES_GRID[1]):
        for cx in range(SHAPES_GRID[0]):
            polys = [p for p in SHAPES_POLYS if p['cell'] == (cx, cy)]
            links = SHAPES_LINKS.get((cx, cy), [])
            count = len(polys) + (1 if links else 0)
            if count == 0:
                out.append(0xFF)
                continue
            z_min = 0.0 if links else float(min(_shapes_poly_entry(p)[0x1C] for p in polys))
            if polys:
                z_max = float(max(_shapes_poly_entry(p)[0x1C] + _shapes_poly_entry(p)[0x3B] for p in polys))
            else:
                z_max = 2.0                    # a LINK-only block: the target's z_floor + tile_h
            out += struct.pack('<BBBBffI', count, SHAPES_BLOCK_FLAGS.get((cx, cy), 0), 0, 0, z_min, z_max, 0)
            for p in polys:
                out += _shapes_poly_entry(p)
            if links:
                out += _shapes_link_entry(links)
            blocks += 1
            entries += count
    return bytes(out), blocks, entries


def _shapes_amap():
    """One moving object (type 0, active) with one node: pivot (3.5, 2.5, 0), 2 polygons."""
    rec = bytearray(0x84 + 156 + 4 * 2)
    struct.pack_into('<II', rec, 0x00, 1, 0)
    struct.pack_into('<I', rec, 0x18, 0)
    struct.pack_into('<I', rec, 0x24, 1)
    node = 0x84
    pivot = (3.5, 2.5, 0.0)
    struct.pack_into('<3f', rec, node + 0x08, *pivot)
    struct.pack_into('<12f', rec, node + 0x14, 1, 0, 0, 0, 1, 0, 0, 0, 1, *pivot)
    struct.pack_into('<I', rec, node + 0x4C, 2)
    struct.pack_into('<i', rec, node + 0x54, -1)
    struct.pack_into('<12f', rec, node + 0x64, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    return struct.pack('<I', len(rec)) + bytes(rec)


def shapes_image(compressed_flag=0):
    """The SHAPES level image: FILE, PALS, GMAP, GVRT, LVRT, MAT, TEX, AMAP, END1, and nothing
    after B3D_END1 (a level may end there; the game reads no further)."""
    gmap, blocks, entries = _shapes_gmap()
    H = bytearray(2200)

    def put(off, fmt, *v):
        struct.pack_into('<' + fmt, H, off, *v)

    put(0x000, 'I', B3D_VERSION)
    put(0x008, '3f', 0.5, 0.5, 0.0)          # start position, on polygon 0
    put(0x020, 'I', len(SHAPES_PALETTES))
    put(0x028, 'II', *SHAPES_GRID)
    put(0x030, 'ii', 0, 2)                   # z range: ceil(max vertex z)
    put(0x038, 'IIII', blocks, 80, entries, 64)
    put(0x048, 'IIII', len(SHAPES_VERTICES), 12, 1, 12)
    put(0x058, 'III', len(SHAPES_MAPPINGS), 40, len(SHAPES_TEXTURES))
    put(0x068, 'IIII', 0, 72, 0, 72)         # no lights
    put(0x0B0, 'IIII', 0, 4, 1, 4)           # no ADEF; one moving object, AMAP version 4
    put(0x0C0, 'i', 2)                       # next link id: LINK targets + 1
    put(0x31C, 'I', compressed_flag)
    put(0x854, 'I', 20)                      # default view distance
    pals = b''.join(bytes(c for rgb in pal for c in rgb) for pal in SHAPES_PALETTES)
    gvrt = b''.join(struct.pack('<3f', *v) for v in SHAPES_VERTICES)
    mats = b''.join(struct.pack('<8fiI', *[c for uv in uvs for c in uv], tex, flags)
                    for uvs, tex, flags in SHAPES_MAPPINGS)
    tex = b''.join(struct.pack('<4i', w, h, pal, 0) + bytes(px(x, y) for y in range(h) for x in range(w))
                   for w, h, pal, px in SHAPES_TEXTURES)
    return b''.join([_sec('B3D FILE', bytes(H)), _sec('B3D_PALS', pals), _sec('B3D_GMAP', gmap),
                     _sec('B3D_GVRT', gvrt), _sec('B3D_LVRT', bytes(12)), _sec('B3D_MAT ', mats),
                     _sec('B3D_TEX ', tex), _sec('B3D_AMAP', _shapes_amap()), _sec('B3D_END1', b'')])


def shapes_file():
    """SHAPES as a compressed .B3D: the raw header with the flag set, then a literal-only stream."""
    image = shapes_image(compressed_flag=1)
    return image[:B3D_RAW_HEADER] + lzss_encode(image[B3D_RAW_HEADER:], 'literal')


# Patched copies of SHAPES (decompressed): (id, patch, outcome, detail). patch: [(offset, struct
# format, value)], or an int (cut the image there). outcome:
#   'error'  the geometry is refused; detail = the offset the error names (the texture stage
#            reports the version 294 header itself, and the geometry is then not attempted);
#   'face'   exported, with the polygons in detail untextured;
#   'warn'   exported unchanged, with a warning.
# m01..m15 are the level-geometry specification's mutation tests; x01..x05 the fork's own rules
# (a non-finite vertex and a version below 278 are refused; a version from 278 to 292 is read
# with a warning; a quad with the triangle UV flag and a non-finite UV make a face untextured).
SHAPES_MUTATIONS = [
    ('m01', [(3790, '<H', 50)], 'error', 3790),            # polygon 0 vertex index 50 (50 vertices)
    ('m02', [(3820, '<h', 10)], 'face', [0]),              # polygon 0 front mapping 10 (10 mappings)
    ('m03', [(3824, '<B', 3)], 'face', [0]),               # polygon 0 own texture 3 (3 textures)
    ('m04', [(4571, '<I', 99)], 'warn', None),             # the LINK record names id 99
    ('m05', [(3825, '<B', 5)], 'error', 3825),             # polygon 0 with 5 vertices
    ('m06', [(3772, '<B', 0)], 'error', 3772),             # cell (0,0): a block with 0 entries
    ('m07', [(4561, '<B', 6)], 'error', 4561),             # a LINK entry with 6 records
    ('m08', [(76, '<I', 16)], 'error', 76),                # header: 16 GMAP entries (15)
    ('m09', [(3768, '<I', 1139)], 'error', 4911),          # B3D_GMAP size one short: broken chain
    ('m10', [(12, '<I', 294)], 'error', 12),               # header version 294
    ('m11', [(84, '<I', 51)], 'error', 4912),              # header: 51 vertices (B3D_GVRT holds 50)
    ('m12', [(80, '<I', 60)], 'error', 80),                # header: GMAP entry size 60
    ('m13', 5024, 'error', 4912),                          # cut inside B3D_GVRT
    ('m14', [(3820, '<h', -1)], 'face', [0]),              # polygon 0 front mapping -1
    ('m15', [(5712, '<i', 7)], 'face', [7]),               # mapping 3 (blended polygon 7) page 7
    ('x01', [(5040, '<f', float('inf'))], 'error', 5032),  # vertex 9 z = inf
    ('x02', [(12, '<I', 277)], 'error', 12),               # header version 277
    ('x03', [(12, '<I', 280)], 'warn', None),              # header version 280
    ('x04', [(3826, '<B', 0x80)], 'face', [0]),            # quad 0 with the triangle UV flag
    ('x05', [(5560, '<f', float('nan'))], 'face', [0, 3, 4, 6, 12]),  # mapping 0 u0 = NaN
]


def shapes_mutant(patch):
    """SHAPES.dec with one SHAPES_MUTATIONS patch applied."""
    image = bytearray(shapes_image())
    if isinstance(patch, int):
        return bytes(image[:patch])
    for off, fmt, value in patch:
        struct.pack_into(fmt, image, off, value)
    return bytes(image)


# ---------------------------------------------------------------------------- the geometry model
#
# An independent model of the tool's level geometry decoder and OBJ/MTL writer, written from the
# fork's level-geometry specification. The tool must write exactly level_geometry_files(), and
# refuse a level's geometry exactly where decode_geometry() raises GeometryError (same offset).

GEOM_MIN_VERSION = 278          # older levels store their polygon records in another layout
GEOM_MAX_SECTIONS = 4096        # the tool's section walk gives up after this many
GEOM_MAX_TEXELS = 65536
GEOM_ENTRY = struct.Struct('<4H8x3f4xhH8BH2xH10x2H')   # the fields of a 64-byte polygon entry used here
assert GEOM_ENTRY.size == 64
UV_PERM = ((0, 1, 2, 3), (1, 0, 3, 2), (3, 2, 1, 0), (2, 3, 0, 1), (3, 0, 1, 2), (0, 3, 2, 1), (2, 1, 0, 3),
           (1, 2, 3, 0))
UV_TRI = ((0, 1, 3), (0, 1, 2), (1, 2, 3), (0, 2, 3))
FACING_AXIS = {1: (1.0, 0.0, 0.0), 2: (-1.0, 0.0, 0.0), 3: (0.0, 1.0, 0.0), 4: (0.0, -1.0, 0.0),
               5: (0.0, 0.0, 1.0), 6: (0.0, 0.0, -1.0)}
_TAG_CHARS = frozenset(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ')


class GeometryError(ValueError):
    """The level's geometry is refused; `offset` is the image offset of the problem."""

    def __init__(self, offset, message):
        super().__init__('@0x%X: %s' % (offset, message))
        self.offset = offset


class GeomPoly:
    __slots__ = ('index', 'offset', 'cell', 'v', 'normal', 'mat_front', 'mat_back', 'tex_bucket', 'uv_mode',
                 'facing', 'obj_flags', 'amap_node', 'amap_index', 'blend', 'render', 'link_id', 'coll', 'surf')


def _is_tag(t):
    return t[:3] == b'B3D' and t[3:4] in (b'_', b' ') and all(c in _TAG_CHARS for c in t[4:8])


def _game_sections(img):
    """{tag: (offset, size)} of the first section with each tag, from "B3D FILE" to "B3D_END1"."""
    secs, pos, count = {}, 0, 0
    while True:
        if pos == len(img):
            raise GeometryError(pos, 'the image ends before B3D_END1')
        if len(img) - pos < 12:
            raise GeometryError(pos, 'a section header is cut off')
        if count >= GEOM_MAX_SECTIONS:
            raise GeometryError(pos, 'more than %d sections' % GEOM_MAX_SECTIONS)
        tag = img[pos:pos + 8]
        if not _is_tag(tag):
            raise GeometryError(pos, 'no section tag')
        size = struct.unpack_from('<I', img, pos + 8)[0]
        if size > len(img) - pos - 12:
            raise GeometryError(pos, 'section %r runs past the end of the image' % tag)
        secs.setdefault(tag, (pos, size))
        count += 1
        pos += 12 + size
        if tag == b'B3D_END1':
            return secs


def decode_geometry(image):
    """The geometry of a decompressed level image, as a dict: version, grid, vertices,
    mappings [(uv corners, page, flags)], textures [(w, h, palette)], polys [GeomPoly],
    links [(offset, cell, [(cell_x, cell_y, id)])], blocks {cell: (flags, first poly, polys)},
    amap [node count per moving object], warnings [(offset, text)]. Raises GeometryError."""
    img = bytes(image)
    if len(img) < 12 + 2200 or img[:8] != b'B3D FILE' or struct.unpack_from('<I', img, 8)[0] != 2200:
        raise GeometryError(0, 'not a level file')

    def hv(off):
        return struct.unpack_from('<I', img, 12 + off)[0]

    version = hv(0x000)
    if version > B3D_VERSION:
        raise GeometryError(12, 'version %d is newer than %d' % (version, B3D_VERSION))
    if version < GEOM_MIN_VERSION:
        raise GeometryError(12, 'version %d is older than %d' % (version, GEOM_MIN_VERSION))
    warnings = [(12, 'version %d' % version)] if version < B3D_VERSION else []
    for off, need in ((0x03C, 80), (0x044, 64), (0x04C, 12), (0x05C, 40)):
        if hv(off) != need:
            raise GeometryError(12 + off, 'header +0x%03X is %d, not %d' % (off, hv(off), need))
    secs = _game_sections(img)
    end1 = secs[b'B3D_END1'][0]
    for tag in (b'B3D_PALS', b'B3D_GMAP', b'B3D_GVRT', b'B3D_MAT ', b'B3D_TEX '):
        if tag not in secs:
            raise GeometryError(end1, 'no %s section' % tag.decode().strip())
    gw, gh = hv(0x028), hv(0x02C)
    pals_n, gvrt_n, mat_n, tex_n = hv(0x020), hv(0x048), hv(0x058), hv(0x060)
    for tag, need in ((b'B3D_PALS', 768 * pals_n), (b'B3D_GVRT', 12 * gvrt_n), (b'B3D_MAT ', 40 * mat_n)):
        if secs[tag][1] != need:
            raise GeometryError(secs[tag][0], '%s is %d bytes, not %d' % (tag.decode(), secs[tag][1], need))

    def body(tag):
        return secs[tag][0] + 12

    flat = struct.unpack_from('<%df' % (3 * gvrt_n), img, body(b'B3D_GVRT'))
    vertices = [flat[3 * i:3 * i + 3] for i in range(gvrt_n)]
    for i, v in enumerate(vertices):
        if not (math.isfinite(v[0]) and math.isfinite(v[1]) and math.isfinite(v[2])):
            raise GeometryError(body(b'B3D_GVRT') + 12 * i, 'vertex %d is not finite' % i)
    mappings = []
    for i in range(mat_n):
        f = struct.unpack_from('<8fiI', img, body(b'B3D_MAT ') + 40 * i)
        mappings.append((((f[0], f[1]), (f[2], f[3]), (f[4], f[5]), (f[6], f[7])), f[8], f[9]))

    p, end = body(b'B3D_TEX '), body(b'B3D_TEX ') + secs[b'B3D_TEX '][1]
    textures = []
    for i in range(tex_n):
        if end - p < 16:
            raise GeometryError(p, 'texture %d: header cut off' % i)
        w, h, pal, _ = struct.unpack_from('<4i', img, p)
        if w <= 0 or h <= 0 or w * h > GEOM_MAX_TEXELS:
            raise GeometryError(p, 'texture %d is %dx%d' % (i, w, h))
        if end - p - 16 < w * h:
            raise GeometryError(p, 'texture %d: pixels cut off' % i)
        textures.append((w, h, pal))
        p += 16 + w * h
    if p != end:
        raise GeometryError(p, '%d bytes after the textures' % (end - p))

    p, end = body(b'B3D_GMAP'), body(b'B3D_GMAP') + secs[b'B3D_GMAP'][1]
    polys, links, blocks = [], [], {}
    entries = 0
    cell, cells = 0, gw * gh
    while cell < cells:
        if p >= end:
            raise GeometryError(p, 'B3D_GMAP ends before cell %d' % cell)
        count = img[p]
        if count == 0xFF:
            p += 1
            cell += 1
            continue
        if count == 0:
            raise GeometryError(p, 'a block with 0 entries')
        if p + 16 + 64 * count > end:
            raise GeometryError(p, 'a block runs past the end of B3D_GMAP')
        first = len(polys)
        for j in range(count):
            eo = p + 16 + 64 * j
            if img[eo] == 0xFF and img[eo + 1] == 0xFF:
                n = img[eo + 2]
                if not 1 <= n <= 5:
                    raise GeometryError(eo + 2, 'a LINK entry with %d records' % n)
                recs = [struct.unpack_from('<hhII', img, eo + 4 + 12 * k) for k in range(n)]
                links.append((eo, cell, [(cx, cy, lid) for cx, cy, _, lid in recs]))
                continue
            f = GEOM_ENTRY.unpack_from(img, eo)
            nverts = f[10]
            if nverts not in (3, 4):
                raise GeometryError(eo + 0x25, 'a polygon with %d vertices' % nverts)
            for k in range(nverts):
                if f[k] >= gvrt_n:
                    raise GeometryError(eo + 2 * k, 'vertex index %d of %d' % (f[k], gvrt_n))
            q = GeomPoly()
            q.index, q.offset, q.cell, q.v, q.normal = len(polys), eo, cell, f[:nverts], f[4:7]
            (q.mat_front, q.mat_back, q.tex_bucket, _, q.uv_mode, q.facing, q.obj_flags, q.amap_node,
             q.amap_index, q.blend, q.render, q.link_id, q.coll, q.surf) = f[7:21]
            polys.append(q)
        blocks[cell] = (img[p + 1], first, len(polys) - first)
        entries += count
        p += 16 + 64 * count
        cell += 1
    if p != end:
        raise GeometryError(p, '%d bytes after the last cell' % (end - p))
    if len(blocks) != hv(0x038):
        raise GeometryError(12 + 0x038, '%d blocks, the header says %d' % (len(blocks), hv(0x038)))
    if entries != hv(0x040):
        raise GeometryError(12 + 0x040, '%d entries, the header says %d' % (entries, hv(0x040)))

    amap = []
    amap_n, amap_ver = hv(0x0B8), hv(0x0BC)
    if b'B3D_AMAP' in secs and amap_ver >= 4 and amap_n > 0:
        off, size = secs[b'B3D_AMAP']
        if 4 * amap_n > size:
            raise GeometryError(off, 'B3D_AMAP is too small for its size table')
        sizes = struct.unpack_from('<%dI' % amap_n, img, off + 12)
        p, end = off + 12 + 4 * amap_n, off + 12 + size
        for i, sz in enumerate(sizes):
            if sz < 0x84 or sz > end - p:
                raise GeometryError(p, 'moving object %d does not fit' % i)
            nodes = struct.unpack_from('<I', img, p + 0x24)[0]
            if 0x84 + 156 * nodes > sz:
                raise GeometryError(p, 'moving object %d: its nodes do not fit' % i)
            amap.append(nodes)
            p += sz
        if p != end:
            raise GeometryError(p, '%d bytes after the last moving object' % (end - p))
    return {'version': version, 'grid': (gw, gh), 'vertices': vertices, 'mappings': mappings,
            'textures': textures, 'polys': polys, 'links': links, 'blocks': blocks, 'amap': amap,
            'warnings': warnings}


def check_links(g):
    """(LINK records, records naming no target, targets whose link id an earlier one uses)."""
    seen, shared = set(), 0
    for q in g['polys']:
        if q.render & 0x100:
            shared += q.link_id in seen
            seen.add(q.link_id)
    gw, gh = g['grid']
    records = unresolved = 0
    for _, _, recs in g['links']:
        for cx, cy, lid in recs:
            records += 1
            block = g['blocks'].get(cx + cy * gw) if 0 <= cx < gw and 0 <= cy < gh else None
            found = bool(block) and bool(block[0] & 0x04) and any(
                g['polys'][k].render & 0x100 and g['polys'][k].link_id == lid
                for k in range(block[1], block[1] + block[2]))
            unresolved += not found
    return records, unresolved, shared


def _fmt6(x):
    """An OBJ number: exact, 6 decimals, ties to even (Python's %f), never -0.000000."""
    s = '%.6f' % x
    return '0.000000' if s == '-0.000000' else s


def _newell(vertices, q):
    nx = ny = nz = 0.0
    k = len(q.v)
    for i in range(k):
        x0, y0, z0 = vertices[q.v[i]]
        x1, y1, z1 = vertices[q.v[(i + 1) % k]]
        nx += (y0 - y1) * (z0 + z1)
        ny += (z0 - z1) * (x0 + x1)
        nz += (x0 - x1) * (y0 + y1)
    return nx, ny, nz


def effective_normal(vertices, q):
    """(normal, keep): the stored normal when finite and not near zero, else the reversed Newell
    normal, else the facing class axis, else +Z; keep = the stored order is counter-clockwise
    about it (otherwise the face is written reversed, vertex 0 first)."""
    N = _newell(vertices, q)
    L = math.sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2])
    n = q.normal
    if all(math.isfinite(c) for c in n) and math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) >= 1e-6:
        pass
    elif L > 1e-12:
        n = (-N[0] / L, -N[1] / L, -N[2] / L)
    else:
        n = FACING_AXIS.get(q.facing, (0.0, 0.0, 1.0))
    nl = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    keep = L >= 1e-9 and (N[0] * n[0] + N[1] * n[1] + N[2] * n[2]) > 1e-6 * L * nl
    return n, keep


def front_texture(g, q, rule='game'):
    """The page a polygon's front side is textured with, or None: rule 'game' = its own texture
    when opaque, its mapping's when blended; rule 'mat' = always its mapping's."""
    if not 0 <= q.mat_front < len(g['mappings']):
        return None
    t = q.tex_bucket if (rule == 'game' and q.blend == 0) else g['mappings'][q.mat_front][1]
    return t if 0 <= t < len(g['textures']) else None


def poly_uvs(corners, uv_mode, nverts):
    """UVs per stored vertex (the game's corner tables), or None for a quad with the triangle flag."""
    perm = UV_PERM[uv_mode & 7]
    if uv_mode & 0x80:
        if nverts > 3:
            return None
        return [corners[perm[t]] for t in UV_TRI[(uv_mode >> 4) & 3]]
    return [corners[perm[k]] for k in range(nverts)]


_OBJ_NAME_BAD = re.compile(r'[^A-Za-z0-9_.-]')


def obj_name(name):
    """The name of a level's OBJ and MTL files (and of the OBJ's mtllib reference) for the level
    name `name` (str, or bytes as a POSIX file name holds them): every character other than an
    ASCII letter, digit, '_', '.' or '-' becomes '_', and 'level' when only '_' and '.' are left.
    This is the tpm2jk kit's obj_name without its upper-casing, written the kit's way: the bytes
    are decoded as UTF-8 with "surrogateescape" (a byte that is not part of a well-formed UTF-8
    character becomes one character of its own), and a regular expression replaces characters."""
    if isinstance(name, bytes):
        name = name.decode('utf-8', 'surrogateescape')
    out = _OBJ_NAME_BAD.sub('_', name)
    return out if out.strip('._') else 'level'


def geometry_text(g, name, scale=0.2, rule='game'):
    """(OBJ text, MTL text, stats) with '\\n' line ends for decoded geometry `g` of the level
    `name`; both files name obj_name(name)."""
    name = obj_name(name)
    S = f32(scale)
    other = 'mat' if rule == 'game' else 'game'
    nobj = len(g['amap'])
    V = g['vertices']
    faces = []
    st = {'faces': 0, 'triangles': 0, 'quads': 0, 'kept': 0, 'untextured': [], 'rule_differs': 0, 'vt': 0,
          'categories': {}}
    for q in g['polys']:
        if q.obj_flags & 0x10:
            rank, cat = nobj + 1, 'hidden'
        elif q.obj_flags & 0x04 and q.amap_index < nobj:
            rank, cat = 1 + q.amap_index, 'amap%d' % q.amap_index
        else:
            rank, cat = 0, 'static'
        n, keep = effective_normal(V, q)
        tex = front_texture(g, q, rule)
        st['rule_differs'] += front_texture(g, q, other) != tex
        uvs = None
        if tex is not None:
            uvs = poly_uvs(g['mappings'][q.mat_front][0], q.uv_mode, len(q.v))
            if uvs is None or not all(math.isfinite(c) for uv in uvs for c in uv):
                tex = uvs = None
        if tex is None:
            st['untextured'].append(q.index)
        k = len(q.v)
        order = list(range(k)) if keep else [0] + list(range(k - 1, 0, -1))
        faces.append((rank, q.index, cat, q, order, tex, uvs, n))
        st['kept'] += keep
        st['triangles' if k == 3 else 'quads'] += 1
        st['categories'][cat] = st['categories'].get(cat, 0) + 1
    faces.sort(key=lambda f: (f[0], f[1]))
    st['faces'] = len(faces)

    out = ['# TPM level geometry: %s (scale %s; Z up; +X right, +Y forward)' % (name, _fmt6(S)),
           'mtllib %s.mtl' % name]
    out += ['v %s %s %s' % (_fmt6(x * S), _fmt6(y * S), _fmt6(z * S)) for x, y, z in V]
    first_vt, vt = [], 0
    for _, _, _, q, order, tex, uvs, _ in faces:
        first_vt.append(vt + 1)
        if tex is not None:
            for k in order:
                out.append('vt %s %s' % (_fmt6(uvs[k][0]), _fmt6(1.0 - uvs[k][1])))
                vt += 1
    out += ['vn %s %s %s' % (_fmt6(f[7][0]), _fmt6(f[7][1]), _fmt6(f[7][2])) for f in faces]
    used, cur_cat, cur_mat = set(), None, None
    for fi, (_, _, cat, q, order, tex, uvs, _) in enumerate(faces):
        if cat != cur_cat:
            out += ['o ' + cat, 'g ' + cat]
            cur_cat, cur_mat = cat, None
        mat = 'untextured' if tex is None else 'tex%d' % tex
        used.add(mat)
        if mat != cur_mat:
            out.append('usemtl ' + mat)
            cur_mat = mat
        if tex is None:
            out.append('f ' + ' '.join('%d//%d' % (q.v[k] + 1, fi + 1) for k in order))
        else:
            out.append('f ' + ' '.join('%d/%d/%d' % (q.v[k] + 1, first_vt[fi] + j, fi + 1)
                                       for j, k in enumerate(order)))
    st['vt'] = vt
    st['materials_used'] = len(used)

    mtl = ['# TPM level materials: %s' % name]
    for i in range(len(g['textures'])):
        mtl += ['', 'newmtl tex%d' % i, 'Ka 1.000000 1.000000 1.000000', 'Kd 1.000000 1.000000 1.000000',
                'Ks 0.000000 0.000000 0.000000', 'd 1.000000', 'illum 1', 'map_Kd %d.bmp' % i]
    if 'untextured' in used:
        mtl += ['', 'newmtl untextured', 'Ka 1.000000 1.000000 1.000000', 'Kd 0.500000 0.500000 0.500000',
                'Ks 0.000000 0.000000 0.000000', 'd 1.000000', 'illum 1']
    return '\n'.join(out) + '\n', '\n'.join(mtl) + '\n', st


def crlf(text):
    """Text as the tool writes it: every line ending in CRLF."""
    return text.replace('\n', '\r\n').encode('utf-8')


def data_line_sha256(data):
    """SHA-256 of an OBJ/MTL file's data lines (comment and blank lines dropped), '\\n'-joined."""
    lines = [l for l in data.decode('utf-8').replace('\r\n', '\n').split('\n') if l and not l.startswith('#')]
    return hashlib.sha256(('\n'.join(lines) + '\n').encode('utf-8')).hexdigest()


def level_geometry_files(image, name, scale=0.2, rule='game'):
    """{'<stem>.obj': bytes, '<stem>.mtl': bytes}, stem = obj_name(name), that default mode writes
    for a decompressed level image of the level `name`, or {} when its geometry is refused."""
    try:
        g = decode_geometry(image)
    except GeometryError:
        return {}
    obj, mtl, _ = geometry_text(g, name, scale, rule)
    stem = obj_name(name)
    return {stem + '.obj': crlf(obj), stem + '.mtl': crlf(mtl)}


# ============================================================================ sound: VIMA, voice files, music files, music tables
#
# The game codes voice files (INDYWV) and music blocks (MCMP) with VIMA ADPCM. Below: the tables,
# an independent model of the tool's three sound readers (ported from the sound pass's reference
# decoders; every check and error offset as sound.h, vima.h and imuse.h describe them), a test
# encoder, and writers for synthetic voice files, music files and music-table DLLs. Signals are
# generated (sines, chirps, silence, noise, full-scale squares); names are generic.

VIMA_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767)
VIMA_WIDTH = (4,) * 45 + (5,) * 14 + (6,) * 15 + (7,) * 15
VIMA_ADJUST = {
    2: (-1, 4),
    3: (-1, -1, 2, 6),
    4: (-1, -1, -1, -1, 1, 2, 4, 6),
    5: (-1,) * 8 + (1, 1, 1, 2, 2, 4, 5, 6),
    6: (-1,) * 16 + (1,) * 5 + (2,) * 4 + (4,) * 3 + (5,) * 2 + (6,) * 2,
    7: (-1,) * 32 + (1,) * 10 + (2,) * 8 + (4,) * 6 + (5,) * 4 + (6,) * 4,
}
WVSM_BLOCK_BYTES = 0x1000
SOUND_MAX_RATE = 384000
IMUS_MAX_MAP = 0x2000 - 8
WAV_HEADER = 44


def _vima_delta_table():
    """89 x 64: entry j*64 + i = the sum, over the bits of i from 0x20 down, of STEP[j] >> 0..5."""
    t = [0] * (89 * 64)
    for i in range(64):
        for j in range(89):
            s, acc, m = VIMA_STEP[j], 0, 0x20
            while m:
                if i & m:
                    acc += s
                m >>= 1
                s >>= 1
            t[j * 64 + i] = acc & 0xFFFF
    return t


VIMA_DELTA = _vima_delta_table()


def vima_delta(index, magnitude):
    """The amount a non-escape code adds or subtracts."""
    w = VIMA_WIDTH[index]
    d = VIMA_DELTA[(index << 6) | (magnitude << (7 - w))]
    return d + (VIMA_STEP[index] >> (w - 1) if magnitude else 0)


def vima_next_index(index, magnitude):
    n = index + VIMA_ADJUST[VIMA_WIDTH[index]][magnitude]
    return 0 if n <= 0 else (88 if n >= 88 else n)


def _s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def vima_header_predictor(b0, b1, quirk):
    """The voice decoder reads (b0 << 8) | (signed char)b1 (quirk); the music decoder a plain BE16."""
    v = (b0 << 8) | b1
    if quirk and b1 >= 0x80:
        v |= 0xFF00
    return _s16(v)


class SoundError(ValueError):
    """A file the tool must refuse, with the file offset its error line names."""

    def __init__(self, offset, message):
        ValueError.__init__(self, '@0x%X: %s' % (offset, message))
        self.offset = offset


def _need(data, pos, n, base, what='data'):
    if pos + n > len(data):
        raise SoundError(base + pos, 'unexpected end of data reading %s' % what)


def vima_read_header(data, pos, base, quirk):
    """The stream header at data[pos:] (data ends where the stream ends): ({channels, index,
    pred, wvsm, size, offset}, next position). Mirrors vima_read_header: a failed read at its
    position, a step index outside 0..88 (unless WVSM) at its byte."""
    start = pos
    _need(data, pos, 1, base, 'u8')
    b = data[pos]
    ch = 2 if b & 0x80 else 1
    idx = [(~b) & 0xFF if ch == 2 else b, 0]
    _need(data, pos + 1, 1, base, 'u8')
    _need(data, pos + 2, 1, base, 'u8')
    pred = [vima_header_predictor(data[pos + 1], data[pos + 2], quirk), 0]
    pos += 3
    wvsm = False
    if ch == 2:
        for k in range(3):
            _need(data, pos + k, 1, base, 'u8')
        raw1 = data[pos]
        idx[1] = raw1 - 256 if raw1 & 0x80 else raw1
        pred[1] = vima_header_predictor(data[pos + 1], data[pos + 2], quirk)
        pos += 3
        if pred[0] == 0x1111 and raw1 == 0x64 and pred[1] == 0x2222 and data[pos:pos + 4] == b'WVSM':
            pos += 4
            wvsm = True
    if not wvsm:
        for c in range(ch):
            if not 0 <= idx[c] <= 88:
                raise SoundError(base + (start if c == 0 else start + 3), 'step index %d of channel %d' % (idx[c], c))
    return {'channels': ch, 'index': idx, 'pred': pred, 'wvsm': wvsm, 'size': pos - start, 'offset': base + start}, pos


def vima_frames(h, pcm_bytes):
    return pcm_bytes // (2 * h['channels'])


def vima_requested(h, pcm_bytes):
    return pcm_bytes // 2 if h['wvsm'] else vima_frames(h, pcm_bytes) * h['channels']


def vima_room(h, coded):
    return coded if h['wvsm'] else 2 * coded


def vima_decode_body(data, pos, base, h, pcm_bytes, stats=None):
    """The codes in data[pos:] (to the end of `data`): whole frames of interleaved samples. Bytes
    past the end read as zero; codes that need more bits than the data has are refused at the end
    of the data. WVSM streams decode in 0x1000-byte blocks of byte codes."""
    end = len(data)
    avail = end - pos
    frames = vima_frames(h, pcm_bytes)
    nch = h['channels']
    if stats is None:
        stats = {}
    if h['wvsm']:
        keep = frames * 2
        out = [0] * keep
        p = pos

        def byte():
            nonlocal p
            if p >= end:
                raise SoundError(base + end, 'the WVSM data ends before the samples asked for')
            p += 1
            return data[p - 1]
        left, block = pcm_bytes, 0
        while left:
            n = min(left, WVSM_BLOCK_BYTES)
            left -= n
            half = (n & ~1) >> 1
            byte()
            byte()
            sh = byte()
            shifts = (sh >> 4, sh & 15)
            o = block
            i = 0
            while i < half:
                for s in range(2):
                    c = byte()
                    if c == 0x80:
                        v = _s16((byte() << 8) + byte())
                    else:
                        v = _s16((c - 256 if c & 0x80 else c) * (1 << shifts[s]))
                    if o < keep:
                        out[o] = v
                    o += 1
                i += 2
            block += WVSM_BLOCK_BYTES // 2
        stats['coded_bytes'] = p - pos
        return out, p
    out = [0] * (frames * nch)
    state = {'p': pos, 'win': 0, 'used': 0}

    def byte():
        b = data[state['p']] if state['p'] < end else 0
        state['p'] += 1
        return b
    win = (byte() << 8) | byte()
    used = 0
    bits = 0
    short = None
    escapes = clamps = 0
    count = 0
    for c in range(nch):
        idx, prd = h['index'][c], h['pred'][c]
        o = c
        for k in range(frames):
            w = VIMA_WIDTH[idx]
            sign = 1 << (w - 1)
            mask = sign - 1
            used += w
            code = (win >> (16 - used)) & (sign | mask)
            bits += w
            if used > 7:
                used -= 8
                win = ((win << 8) | byte()) & 0xFFFF
            mag = code & mask
            if mag == mask:
                hi = (win << used) & 0xFF00
                win = ((win << 8) | byte()) & 0xFFFF
                lo = (win >> (8 - used)) & 0xFF
                win = ((win << 8) | byte()) & 0xFFFF
                prd = _s16(hi | lo)
                bits += 16
                escapes += 1
            else:
                d = vima_delta(idx, mag)
                if code & sign:
                    prd -= d
                    if prd < -32768:
                        prd, clamps = -32768, clamps + 1
                else:
                    prd += d
                    if prd > 32767:
                        prd, clamps = 32767, clamps + 1
            idx = vima_next_index(idx, mag)
            if short is None and bits > 8 * avail:
                short = (k, c)
            out[o] = prd
            o += nch
            count += 1
    stats.update(escapes=escapes, clamps=clamps, code_bits=bits, coded_bytes=(bits + 7) // 8)
    if short is not None:
        raise SoundError(base + end, 'the coded data ends inside sample %d of %d (channel %d)' % (short[0], frames,
                                                                                               short[1]))
    return out, pos + (bits + 7) // 8


# ---------------------------------------------------------------------------- the test encoder


def vima_encode_channel(bw, samples, index, pred, escape_at=(), rng=None, trace=None):
    """Appends the codes of one channel to the MSB-first bit writer `bw`, starting from the decoder
    state (index, pred); returns (the samples the decoder will produce, final index, final pred).
    A sample whose distance from the prediction exceeds the largest delta is escaped (as the game's
    encoder did); `escape_at` forces escapes. Clamping is allowed: the code whose clamped result
    is nearest wins. `trace` gets the bit count after each sample."""
    out = []
    for k, x in enumerate(samples):
        w = VIMA_WIDTH[index]
        sign = 1 << (w - 1)
        mask = sign - 1
        maxd = vima_delta(index, mask - 1)
        if k in escape_at or abs(x - pred) > maxd:
            s = (rng.below(2) if rng else 0) << (w - 1)      # the escape's sign bit is not used
            bw.bits(s | mask, w)
            bw.bits(x & 0xFFFF, 16)
            pred = _s16(x)
            index = vima_next_index(index, mask)
        else:
            best = None
            for m in range(mask):
                d = vima_delta(index, m)
                for neg in (0, 1):
                    v = max(-32768, pred - d) if neg else min(32767, pred + d)
                    e = abs(v - x)
                    if best is None or e < best[0]:
                        best = (e, m, neg, v)
            _, m, neg, v = best
            bw.bits((sign if neg else 0) | m, w)
            pred = v
            index = vima_next_index(index, m)
        out.append(pred)
        if trace is not None:
            trace.append(bw.bit_length())
    return out, index, pred


def vima_header_bytes(index, pred_bytes, channels):
    """A stream header: index per channel, the two predictor bytes per channel."""
    if channels == 1:
        return bytes([index[0], pred_bytes[0][0], pred_bytes[0][1]])
    return bytes([(~index[0]) & 0xFF, pred_bytes[0][0], pred_bytes[0][1], index[1] & 0xFF, pred_bytes[1][0],
                  pred_bytes[1][1]])


def vima_encode(channels, quirk, index=(0, 0), pred_bytes=((0, 0), (0, 0)), escape_at=(), seed=1, trace=None):
    """One VIMA stream (header + codes) for 1 or 2 channels of equal length. Returns (bytes,
    interleaved samples the decoder produces, final (index, pred) per channel, code bits)."""
    rng = Lcg(seed)
    nch = len(channels)
    head = vima_header_bytes(index, pred_bytes, nch)
    bw = BitWriterMSB()
    rec, final = [], []
    for c in range(nch):
        p0 = vima_header_predictor(pred_bytes[c][0], pred_bytes[c][1], quirk)
        r, i, p = vima_encode_channel(bw, channels[c], index[c], p0, escape_at if c == 0 else (), rng,
                                      trace if c == 0 else None)
        rec.append(r)
        final.append((i, p))
    inter = [rec[c][k] for k in range(len(channels[0])) for c in range(nch)]
    return head + bw.getvalue(), inter, final, bw.bit_length()


def wvsm_encode(samples, pcm_bytes, shifts=((3, 2),), seed=7):
    """A WVSM stream: the stereo signature header, then blocks of 0x1000 output bytes (a u16, a
    shift byte, a byte per sample or 0x80 and a big-endian sample). `samples` are interleaved
    stereo; the decoder reads 2 * ceil(half / 2) samples per block, even ones with the high shift,
    odd ones with the low one. A sample is coded as a byte (x >> shift, kept in -127..127, so lossy)
    or, one time in four, escaped. Returns (bytes, the samples the decoder produces)."""
    rng = Lcg(seed)
    out = bytearray(b'\xFF\x11\x11\x64\x22\x22WVSM')
    dec = []
    left, block, b = pcm_bytes, 0, 0
    while left:
        n = min(left, WVSM_BLOCK_BYTES)
        left -= n
        half = (n & ~1) >> 1
        sa, sb = shifts[b % len(shifts)]
        out += struct.pack('<H', n)
        out.append((sa << 4) | sb)
        for i in range(2 * ((half + 1) // 2)):
            k = block + i
            x = samples[k] if k < len(samples) else 0
            sh = sa if i % 2 == 0 else sb
            if rng.below(4):
                q = max(-127, min(127, x >> sh))
                out.append(q & 0xFF)
                dec.append(_s16(q * (1 << sh)))
            else:
                out.append(0x80)
                out += struct.pack('>H', x & 0xFFFF)
                dec.append(_s16(x))
        block += WVSM_BLOCK_BYTES // 2
        b += 1
    return bytes(out), dec


# ---------------------------------------------------------------------------- generated signals


def sig_sine(n, period, amp, phase=0.0):
    return [int(round(amp * math.sin(2 * math.pi * (k + phase) / period))) for k in range(n)]


def sig_chirp(n, f0, f1, amp, rate=22050.0):
    out, ph = [], 0.0
    for k in range(n):
        f = f0 + (f1 - f0) * k / max(1, n - 1)
        ph += 2 * math.pi * f / rate
        out.append(int(round(amp * math.sin(ph))))
    return out


def sig_noise(n, seed, amp=32767):
    rng = Lcg(seed)
    return [rng.below(2 * amp + 1) - amp for _ in range(n)]


def sig_square(n, period, amp=32767):
    return [amp if (k // (period // 2)) % 2 == 0 else -amp - 1 for k in range(n)]


def sig_mix(*parts):
    """Concatenation of signal parts."""
    out = []
    for p in parts:
        out += p
    return out


# ---------------------------------------------------------------------------- voice files (INDYWV)


def make_wv(stream, pcm_bytes, rate=22050, channels=1, bits=16, sync=b'', data_size=None, trailing=b''):
    """An INDYWV file: header, sync block, PCM byte count, VIMA stream (+ trailing bytes). The data
    size counts the count word and the stream (and trailing bytes) unless given."""
    body = struct.pack('<I', pcm_bytes) + stream + trailing
    if data_size is None:
        data_size = len(body)
    return b'INDYWV' + struct.pack('<5I', rate, bits, channels, data_size, len(sync)) + sync + body


def wv_model(data, base=0):
    """The voice reader (wv_decode): {'rate', 'channels', 'samples', 'warnings': [(offset, kind)]}
    or SoundError. Warning kinds: 'datasize', 'frames'."""
    n = len(data)
    if n < 6 or data[:6] != b'INDYWV':
        raise SoundError(base, 'not an INDYWV voice file')
    vals = []
    for off in (6, 0xA, 0xE, 0x12, 0x16):
        _need(data, off, 4, base, 'u32')
        vals.append(struct.unpack_from('<I', data, off)[0])
    rate, bits, ch, dsize, sync = vals
    if rate == 0 or rate > SOUND_MAX_RATE:
        raise SoundError(base + 6, 'sample rate')
    if bits != 16:
        raise SoundError(base + 0xA, 'bits')
    if ch not in (1, 2):
        raise SoundError(base + 0xE, 'channels')
    if sync > n - 0x1A:
        raise SoundError(base + 0x16, 'sync block')
    count_at = 0x1A + sync
    _need(data, count_at, 4, base, 'u32')
    pcm = struct.unpack_from('<I', data, count_at)[0]
    warnings = []
    if 0x1A + sync + dsize != n:
        warnings.append((base + 0x12, 'datasize'))
    h, pos = vima_read_header(data, count_at + 4, base, quirk=True)
    if h['channels'] != ch:
        raise SoundError(h['offset'], 'channel count of the stream')
    if pcm % (2 * ch):
        warnings.append((base + count_at, 'frames'))
    if vima_requested(h, pcm) > vima_room(h, n - pos):
        raise SoundError(base + count_at, 'the PCM size asks for more samples than the codes hold')
    samples, _ = vima_decode_body(data, pos, base, h, pcm)
    return {'rate': rate, 'channels': ch, 'samples': samples, 'warnings': warnings, 'wvsm': h['wvsm']}


def wav_bytes(rate, channels, samples):
    """The canonical 44-byte-header 16-bit PCM WAV the tool writes."""
    data = struct.pack('<%dh' % len(samples), *samples)
    return (b'RIFF' + struct.pack('<I', len(data) + 36) + b'WAVE' + b'fmt ' +
            struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * channels * 2, channels * 2, 16) + b'data' +
            struct.pack('<I', len(data)) + data)


def wv_expected(data):
    """The WAV the tool writes for a voice file, or SoundError."""
    m = wv_model(data)
    return wav_bytes(m['rate'], m['channels'], m['samples'])


def _voice(name, signal, **kw):
    """A voice fixture: (name, file bytes, the samples the encoder meant (for the self-test))."""
    quirk_bytes = kw.pop('pred_bytes', ((0, 0), (0, 0)))
    index = kw.pop('index', (0, 0))
    escape_at = kw.pop('escape_at', ())
    pcm = kw.pop('pcm_bytes', None)
    chans = signal if signal and isinstance(signal[0], list) else [signal]
    stream, rec, _, _ = vima_encode(chans, True, index, quirk_bytes, escape_at)
    if pcm is None:
        pcm = 2 * len(rec)
    return (name, make_wv(stream, pcm, channels=len(chans), **kw), rec)


def voice_fixtures():
    """[(name, bytes, samples the encoder meant)]: mono and stereo ADPCM, WVSM, every header edge."""
    fx = [
        _voice('sine.wv', sig_sine(1000, 50.0, 8000)),
        _voice('chirp.wv', sig_chirp(3001, 100.0, 9000.0, 20000)),
        _voice('silence.wv', [0] * 500),
        _voice('noise.wv', sig_noise(2000, 11), rate=11025),
        _voice('clip.wv', sig_square(1200, 40)),
        _voice('hot.wv', [max(-32768, min(32767, x)) for x in sig_sine(1500, 90.0, 60000)]),
        _voice('stereo.wv', [sig_sine(900, 37.0, 12000), sig_chirp(900, 300.0, 5000.0, 16000)]),
        _voice('quirk.wv', sig_sine(700, 23.0, 3000), pred_bytes=((0x12, 0x9A), (0, 0)), index=(30, 0)),
        _voice('idx88.wv', sig_noise(300, 5, 9000), index=(88, 0)),
        _voice('escfirst.wv', sig_sine(200, 19.0, 20000, 3.0), escape_at=(0, 1, 2, 199)),
        _voice('one.wv', [1234]),
        _voice('zero.wv', [], pcm_bytes=0),
        _voice('oddpcm.wv', sig_sine(1000, 61.0, 7000), pcm_bytes=2001),
        _voice('sync.wv', sig_chirp(600, 50.0, 2000.0, 9000), sync=struct.pack('<III', 7, 1, 0x55AA55AA)),
        _voice('datasize.wv', sig_sine(400, 33.0, 6000), data_size=17),
        _voice('trailing.wv', sig_sine(400, 33.0, 6000), trailing=b'\x5A' * 9),
        _voice('rate8k.wv', sig_sine(800, 16.0, 10000), rate=8000),
    ]
    # a stream whose code bits end on a byte boundary (the decoder then looks two bytes ahead);
    # the encoder is causal, so a prefix of the signal codes as a prefix of the bits
    base = sig_mix(sig_sine(500, 29.0, 9000), sig_noise(200, 3, 3000))
    trace = []
    vima_encode([base], True, trace=trace)
    n = max(k + 1 for k, bits in enumerate(trace) if bits % 8 == 0)
    fx.append(_voice('edge.wv', base[:n]))
    # WVSM: two whole blocks, and a block of an odd sample count with a PCM size of part frames
    stereo = [x for pair in zip(sig_sine(2048, 64.0, 900), sig_chirp(2048, 50.0, 4000.0, 30000))
              for x in pair]
    for name, pcm in (('wvsm.wv', 0x2000), ('wvsmodd.wv', 0x1006)):
        st, dec = wvsm_encode(stereo, pcm, shifts=((3, 0), (0, 7)))
        frames = pcm // 4
        fx.append((name, make_wv(st, pcm, channels=2), dec[:frames * 2]))
    return fx


def voice_mutants():
    """Damaged copies of voice fixtures: [(name, bytes)]; the model gives each one's outcome."""
    fx = {n: d for n, d, _ in voice_fixtures()}
    sine, stereo, wvsm = fx['sine.wv'], fx['stereo.wv'], fx['wvsm.wv']
    out = [('wv_' + k, d) for k, d in _cuts(sine, [0, 5, 6, 0xB, 0x19, 0x1C, 0x1E, 0x1F, 0x20, len(sine) // 2,
                                                   len(sine) - 1])]
    out += [('wv_st_' + k, d) for k, d in _cuts(stereo, [0x21, 0x23, len(stereo) - 2])]
    out += [('wv_wvsm_' + k, d) for k, d in _cuts(wvsm, [0x2A, len(wvsm) - 1])]
    out += [('wv_%s' % k, _put32(sine, off, v)) for k, off, v in
            [('rate0', 6, 0), ('ratebig', 6, 400000), ('bits8', 0xA, 8), ('chan3', 0xE, 3), ('chan2', 0xE, 2),
             ('synchuge', 0x16, HUGE), ('pcmhuge', 0x1A, HUGE), ('pcmmore', 0x1A, len(sine) * 3)]]
    magic = bytearray(sine)
    magic[3] = ord('X')
    out.append(('wv_magic', bytes(magic)))
    idx = bytearray(sine)
    idx[0x1E] = 89                                        # step index 89
    out.append(('wv_index89', bytes(idx)))
    st = bytearray(stereo)
    st[0x21] = 0x80                                       # channel 1's step index -128
    out.append(('wv_st_index1', bytes(st)))
    mono = bytearray(stereo)
    struct.pack_into('<I', mono, 0xE, 1)                  # a stereo stream in a mono file
    out.append(('wv_st_mono', bytes(mono)))
    return out


# ---------------------------------------------------------------------------- music files (MCMP + iMUS)


def _imus_event(tag, pos, values=(), text=None, data=b''):
    if tag == 'TEXT':
        body = struct.pack('>I', pos) + text + b'\0'
    elif tag == 'SYNC':
        body = struct.pack('>I', pos) + data
    else:
        body = struct.pack('>%dI' % (1 + len(values)), pos, *values)
    return tag.encode('ascii') + struct.pack('>I', len(body)) + body


def imus_header(events, frame_bytes, data_bytes, stream_size=None):
    """The iMUS header for map events given in frames of the audio:
    ('FRMT', frame, flag, bits, rate, channels), ('TEXT', frame, b'text'), ('REGN', frame, frames),
    ('JUMP', frame, dest frame, hook, fade), ('STOP', frame), ('SYNC', frame, b'data'), and
    ('RAW', bytes) for an event given as it is stored."""
    def build(start):
        out = b''
        for e in events:
            tag = e[0]
            if tag == 'RAW':
                out += e[1]
                continue
            pos = start + e[1] * frame_bytes
            if tag == 'FRMT':
                out += _imus_event(tag, pos, e[2:6])
            elif tag == 'TEXT':
                out += _imus_event(tag, pos, text=e[2])
            elif tag == 'REGN':
                out += _imus_event(tag, pos, (e[2] * frame_bytes,))
            elif tag == 'JUMP':
                out += _imus_event(tag, pos, (start + e[2] * frame_bytes, e[3], e[4]))
            elif tag == 'STOP':
                out += _imus_event(tag, pos)
            elif tag == 'SYNC':
                out += _imus_event(tag, pos, data=e[2])
        return out
    start = len(build(0)) + 24
    events_bytes = build(start)
    total = start + data_bytes
    return (b'iMUS' + struct.pack('<I', (stream_size or total) - 8) + b'MAP ' + struct.pack('>I', len(events_bytes)) +
            events_bytes + b'DATA' + struct.pack('>I', data_bytes))


def make_imc(channels, events, block_bytes=8192, raw_split=None, names=b'NULL\0VIMA\0',
             raw_codec=0, vima_codec=1, wvsm_blocks=(), restart_blocks=()):
    """A music file: the iMUS header (raw blocks, split at `raw_split` sizes) and the audio of
    `channels` (1 or 2 lists of samples) in VIMA blocks of `block_bytes` decoded bytes. Each block's
    header holds the encoder state at its start (as the game's files do), except the blocks in
    `restart_blocks`, which start from index 88 and the block's first sample; blocks in
    `wvsm_blocks` are coded as WVSM. Returns (file bytes, the decoded interleaved samples)."""
    nch = len(channels)
    fb = 2 * nch
    frames = len(channels[0])
    head = imus_header(events, fb, frames * fb)
    if raw_split is None:
        raw_split = [len(head)]
    raws, p = [], 0
    for s in raw_split:
        raws.append(head[p:p + s])
        p += s
    assert p == len(head)
    per_block = block_bytes // fb
    state = [(0, 0)] * nch
    blocks, decoded = [], []
    for b, start in enumerate(range(0, frames, per_block)):
        part = [ch[start:start + per_block] for ch in channels]
        n = len(part[0])
        if b in wvsm_blocks and nch == 2:
            inter = [x for pair in zip(part[0], part[1]) for x in pair]
            data, dec = wvsm_encode(inter, n * fb, shifts=((2, 4),), seed=b)
            dec = dec[:n * 2]
            state = [(0, part[0][-1]), (0, part[1][-1])]
        else:
            if b in restart_blocks:
                state = [(88, part[c][0]) for c in range(nch)]
            pb = [(((s[1] & 0xFFFF) >> 8), s[1] & 0xFF) for s in state]
            data, dec, final, _ = vima_encode(part, False, [s[0] for s in state] + [0] * (2 - nch),
                                              pb + [(0, 0)] * (2 - nch), seed=b + 3)
            state = final
        blocks.append((vima_codec, n * fb, data))
        decoded += dec
    entries = [(raw_codec, len(r), r) for r in raws] + blocks
    out = bytearray(b'MCMP' + struct.pack('>H', len(entries)))
    for codec, dsize, data in entries:
        out += struct.pack('>BII', codec, dsize, len(data))
    out += struct.pack('>H', len(names)) + names
    for _, _, data in entries:
        out += data
    return bytes(out), decoded


def _mcmp_layout(data):
    """(block count, [(entry offset, codec, decoded, stored, stored offset)], data0) of a file the
    writer made (no checks)."""
    n = struct.unpack_from('>H', data, 4)[0]
    nsz = struct.unpack_from('>H', data, 6 + 9 * n)[0]
    p = 8 + 9 * n + nsz
    ents = []
    for i in range(n):
        c, d, s = struct.unpack_from('>BII', data, 6 + 9 * i)
        ents.append((6 + 9 * i, c, d, s, p))
        p += s
    return n, ents, 8 + 9 * n + nsz


def imc_model(data, label):
    """The music reader (imc_decode) and the map text: {'rate', 'channels', 'samples', 'text',
    'warnings': [(offset, kind)]} or SoundError. Mirrors every check of sound.cpp in its order."""
    size = len(data)
    if size < 4 or data[:4] != b'MCMP':
        raise SoundError(0, 'not an MCMP music file')
    _need(data, 4, 2, 0, 'bytes')
    n = struct.unpack_from('>H', data, 4)[0]
    if n == 0:
        raise SoundError(4, 'no blocks')
    if 6 + 9 * n + 2 > size:
        raise SoundError(4, 'block table past the end')
    names_at = 6 + 9 * n
    nsz = struct.unpack_from('>H', data, names_at)[0]
    if names_at + 2 + nsz > size:
        raise SoundError(names_at, 'name table past the end')
    names, i, table = [], 0, data[names_at + 2:names_at + 2 + nsz]
    while i < nsz:
        e = table.find(b'\0', i)
        if e < 0:
            raise SoundError(names_at + 2 + i, 'codec name not terminated')
        names.append(table[i:e])
        i = e + 1
    data0 = names_at + 2 + nsz
    blocks, at = [], data0
    for k in range(n):
        eo = 6 + 9 * k
        codec, dec, sto = struct.unpack_from('>BII', data, eo)
        if codec >= len(names):
            raise SoundError(eo, 'codec index')
        cn = names[codec]
        if cn not in (b'VIMA', b'NULL', b'NONE'):
            raise SoundError(eo, 'codec name')
        if at + sto > size:
            raise SoundError(eo, 'block past the end')
        blocks.append({'entry': eo, 'vima': cn == b'VIMA', 'dec': dec, 'sto': sto, 'at': at})
        at += sto
    if at != size:
        raise SoundError(at, 'bytes after the last block')
    hb, audio, seen = 0, 0, False
    for b in blocks:
        if b['vima']:
            seen = True
            audio += b['dec']
            continue
        if seen:
            raise SoundError(b['entry'], 'raw block after VIMA')
        if b['dec'] != b['sto']:
            raise SoundError(b['entry'], 'raw block sizes')
        hb += b['sto']
    if hb == 0:
        raise SoundError(6, 'no raw header block')
    h = data[data0:data0 + hb]
    if h[:4] != b'iMUS':
        raise SoundError(data0, 'no iMUS')
    if hb < 16:
        raise SoundError(data0 + hb, 'iMUS header ends before its map')
    if h[8:12] != b'MAP ':
        raise SoundError(data0 + 8, 'no MAP')
    map_size = struct.unpack_from('>I', h, 12)[0]
    if map_size > IMUS_MAX_MAP:
        raise SoundError(data0 + 12, 'map too large')
    if map_size + 24 > hb:
        raise SoundError(data0 + 12, 'map past the raw blocks')
    data_at = 16 + map_size
    if h[data_at:data_at + 4] != b'DATA':
        raise SoundError(data0 + data_at, 'no DATA')
    data_bytes = struct.unpack_from('>I', h, data_at + 4)[0]
    start = map_size + 24
    if start != hb:
        raise SoundError(data0 + data_at, 'audio start')
    events, p = [], 16
    while p < data_at:
        if data_at - p < 8:
            raise SoundError(data0 + p, 'event header past the map')
        tag = h[p:p + 4].split(b'\0')[0].decode('latin-1')
        esize = struct.unpack_from('>I', h, p + 4)[0]
        body = p + 8
        left = data_at - body
        ev = {'tag': tag, 'offset': data0 + p, 'values': [0, 0, 0, 0], 'text': b'', 'data': 0}
        if tag == 'TEXT':
            if left < 5:
                raise SoundError(data0 + p, 'TEXT past the map')
            ev['pos'] = struct.unpack_from('>I', h, body)[0]
            e = h.find(b'\0', body + 4, data_at)
            if e < 0:
                raise SoundError(data0 + p, 'TEXT not terminated')
            ev['text'] = h[body + 4:e]
            events.append(ev)
            p = e + 1
            continue
        need = {'FRMT': 5, 'REGN': 2, 'JUMP': 4, 'STOP': 1, 'SYNC': 1}.get(tag)
        if need is None:
            raise SoundError(data0 + p, 'unknown event')
        words = esize >> 2
        if words < need:
            raise SoundError(data0 + p, 'event too short')
        if words * 4 > left:
            raise SoundError(data0 + p, 'event past the map')
        vals = struct.unpack_from('>%dI' % need, h, body)
        ev['pos'] = vals[0]
        ev['values'][:need - 1] = vals[1:]
        if tag == 'SYNC':
            ev['data'] = words * 4 - 4
        events.append(ev)
        p = body + words * 4
    if not events or events[0]['tag'] != 'FRMT' or events[0]['pos'] != hb:
        raise SoundError(data0 + 16, 'first event not FRMT')
    f = events[0]
    flag, bits, rate, ch = f['values']
    if bits != 16:
        raise SoundError(f['offset'], 'FRMT bits')
    if rate == 0 or rate > SOUND_MAX_RATE:
        raise SoundError(f['offset'], 'FRMT rate')
    if ch not in (1, 2):
        raise SoundError(f['offset'], 'FRMT channels')
    warnings = [(f['offset'], 'byteorder')] if flag == 0 else []
    fb = 2 * ch
    end = hb + data_bytes
    prev = 0
    for k, e in enumerate(events):
        if e['pos'] < hb or e['pos'] > end:
            raise SoundError(e['offset'], 'position outside the audio')
        if k and e['pos'] < prev:
            raise SoundError(e['offset'], 'position goes backwards')
        if (e['pos'] - hb) % fb:
            raise SoundError(e['offset'], 'position off the frame grid')
        prev = e['pos']
        if e['tag'] == 'FRMT' and k and e['values'][1:4] != f['values'][1:4]:
            raise SoundError(e['offset'], 'second FRMT')
        if e['tag'] == 'REGN' and (e['values'][0] % fb or e['pos'] + e['values'][0] > end):
            raise SoundError(e['offset'], 'region')
        if e['tag'] == 'JUMP':
            d = e['values'][0]
            if d < hb or d >= end or (d - hb) % fb:
                raise SoundError(e['offset'], 'jump destination')
    lps = [e['pos'] for e in events if e['tag'] == 'TEXT' and e['text'] == b'lp']
    for e in events:
        if e['tag'] != 'JUMP':
            continue
        d = e['values'][0]
        if not any(x['tag'] == 'REGN' and x['pos'] == d for x in events):
            raise SoundError(e['offset'], 'no region at the jump destination')
        if lps and d not in lps:
            raise SoundError(e['offset'], 'jump destination is not the loop point')
    if data_bytes % fb:
        raise SoundError(data0 + data_at + 4, 'DATA not whole frames')
    if audio != data_bytes:
        raise SoundError(data0 + data_at + 4, 'DATA size differs from the VIMA blocks')
    samples = []
    for k, b in enumerate(blocks):
        if not b['vima']:
            continue
        blk = data[b['at']:b['at'] + b['sto']]
        hd, pos = vima_read_header(blk, 0, b['at'], quirk=False)
        if hd['channels'] != ch:
            raise SoundError(b['at'], 'block channels')
        if b['dec'] % fb:
            raise SoundError(b['entry'], 'block not whole frames')
        if vima_requested(hd, b['dec']) > vima_room(hd, len(blk) - pos):
            raise SoundError(b['entry'], 'block cannot hold its samples')
        part, _ = vima_decode_body(blk, pos, b['at'], hd, b['dec'])
        samples += part
    text = imc_map_lines(events, hb, data_bytes, rate, ch, label)
    return {'rate': rate, 'channels': ch, 'samples': samples, 'text': text, 'warnings': warnings}


def sound_printable(b):
    """Bytes other than printable ASCII, '"' and '\\' as \\xNN (the tool's quoting)."""
    if isinstance(b, str):
        b = b.encode('latin-1')
    return ''.join(chr(c) if 0x20 <= c < 0x7F and c not in (0x22, 0x5C) else '\\x%02X' % c for c in b)


def imc_map_lines(events, hb, data_bytes, rate, ch, label):
    fb = 2 * ch
    out = ['# iMUSE map of %s, written by TPMExtractor (fork)' % sound_printable(label),
           '# Positions are frames of the audio: (byte position in the decoded stream - header bytes) / frame bytes.',
           'format\tframes=%d\trate=%d\tchannels=%d\tbits=16\tframe_bytes=%d\theader_bytes=%d'
           % (data_bytes // fb, rate, ch, fb, hb),
           '# event\tframe\tvalues']
    for e in events:
        line = '%s\t%d' % (e['tag'], (e['pos'] - hb) // fb)
        v = e['values']
        if e['tag'] == 'FRMT':
            line += '\tflag=%d\tbits=%d\trate=%d\tchannels=%d' % tuple(v)
        elif e['tag'] == 'TEXT':
            line += '\ttext="%s"' % sound_printable(e['text'])
        elif e['tag'] == 'REGN':
            line += '\tframes=%d' % (v[0] // fb)
        elif e['tag'] == 'JUMP':
            line += '\tdest=%d\thook=%d\tfade_ms=%d' % ((v[0] - hb) // fb, v[1], v[2])
        elif e['tag'] == 'SYNC':
            line += '\tbytes=%d' % e['data']
        out.append(line)
    return '\n'.join(out) + '\n'


def imc_expected(data, file_name):
    """{'<file>.wav': bytes, '<file>.imap.txt': bytes} the tool writes for a music file, or SoundError."""
    m = imc_model(data, file_name)
    stem = sound_stem(file_name)
    return {stem + '.wav': wav_bytes(m['rate'], m['channels'], m['samples']), stem + '.imap.txt': crlf(m['text'])}


def sound_stem(file_name):
    """A sound file's name without its extension (from the last '.')."""
    return file_name.rsplit('.', 1)[0] if '.' in file_name else file_name


def _track_events(kind, frames, banner=b'*' * 24, name=b'track', fade=500, sync=False):
    """Map events of the three forms the game's music has: 'loop' (intro, loop, tail), 'end' (a
    one-shot with an "_end" marker), 'plain'."""
    ev = [('FRMT', 0, 1, 16, 22050, 2), ('TEXT', 0, name), ('TEXT', 0, banner)]
    if kind == 'loop':
        lp, j = frames // 7, frames - frames // 9
        ev += [('REGN', 0, lp), ('TEXT', lp, b'lp'), ('REGN', lp, j - lp), ('JUMP', j, lp, 0, fade),
               ('REGN', j, frames - j)]
    elif kind == 'end':
        e = frames - frames // 5
        ev += [('REGN', 0, e), ('TEXT', e, b'_end'), ('REGN', e, frames - e)]
    else:
        ev += [('REGN', 0, frames)]
    if sync:
        ev.insert(3, ('SYNC', 0, struct.pack('>II', 2, 0x01020304)))
    return ev + [('STOP', frames)]


def _frmt(ev, rate, ch):
    return [('FRMT', 0, 1, 16, rate, ch)] + ev[1:]


def music_fixtures():
    """[(name, file bytes, decoded samples)]: the three forms of the game's tracks, mono and
    stereo, raw headers in one or two blocks, restarted and WVSM blocks, and the edge cases."""
    fx = []
    n = 2048 * 3 + 517
    left, right = sig_sine(n, 100.0, 9000), sig_chirp(n, 80.0, 6000.0, 14000)
    data, dec = make_imc([left, right], _track_events('loop', n, name=b'track_a'))
    fx.append(('loop.imc', data, dec))
    n2 = 2048 * 2 + 1000
    data, dec = make_imc([sig_noise(n2, 21, 12000), sig_sine(n2, 41.0, 20000)],
                         _track_events('end', n2, name=b'track_b'))
    fx.append(('oneshot.imc', data, dec))
    n3 = 4096 + 300                                   # mono: 4,096 frames per 8,192-byte block
    data, dec = make_imc([sig_chirp(n3, 40.0, 3000.0, 25000, 11025.0)],
                         _frmt(_track_events('plain', n3, name=b'track_c'), 11025, 1))
    fx.append(('plain.imc', data, dec))
    n4 = 2048 + 700
    ev = _track_events('loop', n4, name=b'track_d', sync=True, fade=200)
    head = imus_header(ev, 4, n4 * 4)
    data, dec = make_imc([sig_square(n4, 64), sig_noise(n4, 9, 32767)], ev, raw_split=[40, len(head) - 40],
                         names=b'NONE\0VIMA\0', restart_blocks=(1,))
    fx.append(('rawsplit.imc', data, dec))
    n5 = 2048 * 2 + 64
    data, dec = make_imc([sig_sine(n5, 30.0, 4096), sig_sine(n5, 45.0, 8192)], _track_events('end', n5, name=b'w'),
                         wvsm_blocks=(1,))
    fx.append(('wvsm.imc', data, dec))
    n6 = 2048 * 2 + 1                                 # a last block of one frame
    data, dec = make_imc([[0] * n6, sig_mix([0] * 2048, sig_noise(n6 - 2048, 4))],
                         _track_events('plain', n6, name=b'edge'))
    fx.append(('edge.imc', data, dec))
    return fx


def music_mutants():
    """Damaged copies of music fixtures: [(name, bytes)]; the model gives each one's outcome."""
    fx = {n: d for n, d, _ in music_fixtures()}
    loop = fx['loop.imc']
    n, ents, data0 = _mcmp_layout(loop)
    hb = ents[0][3]
    map_size = struct.unpack_from('>I', loop, data0 + 12)[0]
    ev0 = data0 + 16
    out = [('imc_' + k, d) for k, d in _cuts(loop, [0, 3, 5, 6 + 9 * n - 1, 6 + 9 * n + 3, data0 + 10,
                                                   ents[1][4] + 3, ents[2][4] + ents[2][3] // 2, len(loop) - 1])]
    out += [('imc_%s' % k, _put16(loop, off, v)) for k, off, v in [('nzero', 4, 0), ('nhuge', 4, 0xFFFF)]]

    def patch(name, off, raw):
        b = bytearray(loop)
        b[off:off + len(raw)] = raw
        out.append(('imc_' + name, bytes(b)))

    def be32(name, off, value):
        patch(name, off, struct.pack('>I', value & 0xFFFFFFFF))
    data_size = struct.unpack_from('>I', loop, data0 + 20 + map_size)[0]
    be32('rawdec', ents[0][0] + 1, hb + 4)
    be32('storedhuge', ents[2][0] + 5, HUGE)
    be32('mapsize', data0 + 12, 0x1FFF)
    be32('mapsmall', data0 + 12, map_size - 8)
    be32('datasize', data0 + 20 + map_size, data_size + 4)
    be32('datapart', data0 + 20 + map_size, data_size + 2)
    patch('codecidx', ents[1][0], b'\x07')
    patch('codecname', 6 + 9 * n + 2, b'XXXX')
    patch('imus', data0, b'iMUZ')
    patch('maptag', data0 + 8, b'MAQ ')
    patch('datatag', data0 + 16 + map_size, b'DATB')
    patch('frmtbits', ev0 + 8 + 8, struct.pack('>I', 8))
    patch('frmtrate', ev0 + 8 + 12, struct.pack('>I', 0))
    patch('frmtchan', ev0 + 8 + 16, struct.pack('>I', 3))
    patch('frmtpos', ev0 + 8, struct.pack('>I', hb + 4))
    patch('frmtflag0', ev0 + 8 + 4, struct.pack('>I', 0))           # valid, with a warning
    patch('unknown', ev0, b'BOGU')
    # the events after FRMT and the two TEXTs: REGN, TEXT(lp), REGN, JUMP, REGN, STOP
    evs = []
    p = ev0
    while p < data0 + 16 + map_size:
        tag = loop[p:p + 4]
        size = struct.unpack_from('>I', loop, p + 4)[0]
        evs.append((tag, p, size))
        p += 8 + size
    by = {}
    for tag, p, size in evs:
        by.setdefault(tag, []).append(p)
    regn, jump, text = by[b'REGN'], by[b'JUMP'][0], by[b'TEXT']
    stop = by[b'STOP'][0]
    patch('regnlen', regn[1] + 12, struct.pack('>I', struct.unpack_from('>I', loop, regn[1] + 12)[0] + 2))
    patch('regnpast', regn[2] + 12, struct.pack('>I', struct.unpack_from('>I', loop, regn[2] + 12)[0] + 4))
    patch('jumpregion', jump + 12, struct.pack('>I', struct.unpack_from('>I', loop, jump + 12)[0] + 4))
    patch('jumplp', jump + 12, struct.pack('>I', hb))           # to the first region, not the loop point
    patch('jumpoff', jump + 12, struct.pack('>I', struct.unpack_from('>I', loop, jump + 12)[0] + 2))
    patch('stopback', stop + 8, struct.pack('>I', hb))           # STOP before the events it follows
    patch('stoppast', stop + 8, struct.pack('>I', hb + 4 * 100000))
    patch('textnul', text[2] + 14, b'\x41')                       # "lp" loses its NUL: the walk goes astray
    patch('textname', text[2] + 13, b'\x41')                      # "lp" becomes "lA": no loop point, still valid
    map_end = data0 + 16 + map_size
    patch('textlong', text[0] + 12, b'\x41' * (map_end - text[0] - 12))   # a text not ended inside the map
    blk1 = ents[1][4]
    patch('blockmono', blk1, b'\x00')                             # a mono block in a stereo track
    patch('blockindex', blk1, b'\x9B')                            # step index 100
    patch('blockindex1', blk1 + 3, b'\x80')                       # channel 1's step index -128
    out.append(('imc_trailing', loop + b'\0\0\0'))
    entries = bytearray(loop)
    struct.pack_into('>I', entries, ents[2][0] + 1, ents[2][2] + 2)  # a block of part frames
    struct.pack_into('>I', entries, ents[3][0] + 1, ents[3][2] - 2)
    out.append(('imc_partframes', bytes(entries)))
    big = bytearray(loop)
    struct.pack_into('>I', big, ents[1][0] + 1, ents[1][3] * 4 + 8)   # more samples than its codes hold
    struct.pack_into('>I', big, data0 + 20 + map_size, struct.unpack_from('>I', big, data0 + 20 + map_size)[0]
                     + ents[1][3] * 4 + 8 - ents[1][2])
    out.append(('imc_blockroom', bytes(big)))
    short = bytearray(loop)
    s2 = ents[2][3] - 40                                  # a block cut short, the file kept consistent
    struct.pack_into('>I', short, ents[2][0] + 5, s2)
    del short[ents[2][4] + s2:ents[2][4] + ents[2][3]]
    out.append(('imc_blockshort', bytes(short)))
    order = bytearray(loop)                               # the last block stored raw, after VIMA blocks
    struct.pack_into('>BI', order, ents[-1][0], 0, ents[-1][3])
    out.append(('imc_rawafter', bytes(order)))
    vima = bytearray(loop)                                # the first block coded VIMA: no raw header
    struct.pack_into('>B', vima, ents[0][0], 1)
    out.append(('imc_noheader', bytes(vima)))
    out.append(('imc_shorthead', b'MCMP' + struct.pack('>HBIIH', 1, 0, 8, 8, 10) + b'NULL\0VIMA\0' + b'iMUS\0\0\0\0'))
    return out


# ---------------------------------------------------------------------------- music tables (IMUSE.DLL)

IMUSE_COUNTS = (87, 82, 167)


def _imuse_rec(key, trans, ident, sym, attrib=0, hook=0, fade=0):
    return (name_bytes(key, 32) + struct.pack('<II', trans, ident) + name_bytes(sym, 24) +
            struct.pack('<III', attrib, hook, fade))


def imuse_tables_spec(counts=IMUSE_COUNTS):
    """Generic records: (states, sequences, files) with keys area<n>/cue<n> and files <KEY>.IMC.
    One state's key differs in letter case from its file key (the engine's lookup is byte for byte)."""
    ns, nq, nf = counts
    files = [('area%03d' % i, 'AREA%03d.IMC' % i) for i in range(ns - 1)]
    files += [('cue%03d' % i, 'CUE%03d.IMC' % i) for i in range(nf - len(files))]
    states = [('', 0, 1000, 'state_none', 0)]
    for i in range(1, ns):
        key = 'area%03d' % (i - 1)
        if i == 11:
            key = 'Area%03d' % (i - 1)
        states.append((key, 3 if i % 13 else 0, 1000 + 5 * i, 'stateArea%03d' % i, 1 if i in (40, 41) else 0))
    seqs = [('', 0, 2000, 'sequence_none', 0)]
    for i in range(1, nq):
        seqs.append(('cue%03d' % (i - 1) if i < nf - ns + 1 else '', 4 if i % 3 else 3, 2000 + 10 * i,
                     'seqCue%03d' % i, 0))
    return states, seqs, files


def make_imuse_dll(states=None, seqs=None, files=None, gap=4, decoy=True, lead=64):
    """A minimal PE32 DLL whose .data section holds the music tables: `lead` bytes of other data
    (with a decoy record that looks like the null state but is not followed by a record), the
    state table, `gap` zero bytes, the sequence table, the file table and its empty end record.
    Returns (bytes, {'state', 'sequence', 'file': file offsets})."""
    if states is None:
        states, seqs, files = imuse_tables_spec()
    data = bytearray()
    lead_bytes = bytearray(b'generic data, not a table\0' + bytes(lead))[:lead]
    data += lead_bytes
    if decoy:
        data += _imuse_rec('', 0, 1000, 'decoy_none') + b'\xEE' * 76
    st_at = len(data)
    for k, t, i, s, a in states:
        data += _imuse_rec(k, t, i, s, a)
    data += bytes(gap)
    sq_at = len(data)
    for k, t, i, s, a in seqs:
        data += _imuse_rec(k, t, i, s, a)
    f_at = len(data)
    for k, f in files:
        data += name_bytes(k, 32) + name_bytes(f, 32)
    data += bytes(64) + b'other data after the tables\0'
    while len(data) % 0x200:
        data.append(0)
    text = b'\xC3' + bytes(0x1FF)
    raw_text, raw_data = 0x400, 0x600
    head = bytearray(0x400)
    head[0:2] = b'MZ'
    struct.pack_into('<I', head, 0x3C, 0x80)
    head[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', head, 0x84, 0x14C, 2, 0, 0, 0, 224, 0x2102)
    opt = 0x98
    struct.pack_into('<H', head, opt, 0x10B)
    struct.pack_into('<I', head, opt + 28, 0x10000000)
    sec = opt + 224
    struct.pack_into('<8sIIIIIIHHI', head, sec, b'.text', 0x200, 0x1000, 0x200, raw_text, 0, 0, 0, 0, 0x60000020)
    struct.pack_into('<8sIIIIIIHHI', head, sec + 40, b'.data', len(data), 0x2000, len(data), raw_data, 0, 0, 0, 0,
                     0xC0000040)
    out = bytes(head) + text + bytes(data)
    return out, {'state': raw_data + st_at, 'sequence': raw_data + sq_at, 'file': raw_data + f_at}


def _field(b, n, allow_empty):
    e = b.find(b'\0', 0, n)
    if e < 0 or (e == 0 and not allow_empty):
        return None
    if any(c < 0x20 or c > 0x7E for c in b[:e]) or any(b[e:n]):
        return None
    return b[:e].decode('ascii')


def _music_rec(b):
    if len(b) < 76:
        return None
    key, sym = _field(b[0:32], 32, True), _field(b[40:64], 24, False)
    trans, ident, attrib, hook, fade = struct.unpack_from('<II', b, 32) + struct.unpack_from('<III', b, 64)
    if key is None or sym is None or trans > 12 or attrib > 0xFFFF or hook > 0xFFFF or fade > 0xFFFF:
        return None
    return (key, trans, ident, sym, attrib, hook, fade)


def _file_rec(b):
    if len(b) < 64:
        return None
    k, f = _field(b[0:32], 32, False), _field(b[32:64], 32, False)
    return None if k is None or f is None else (k, f)


def imuse_model(data, label):
    """The music-table reader (imuse_read_tables) and its text, or SoundError."""
    size = len(data)
    if data[:2] != b'MZ':
        raise SoundError(0, 'no MZ')
    _need(data, 0x3C, 4, 0)
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if pe + 24 > size:
        raise SoundError(0x3C, 'PE offset')
    if data[pe:pe + 4] != b'PE\0\0':
        raise SoundError(pe, 'no PE')
    nsec, = struct.unpack_from('<H', data, pe + 6)
    optsz, = struct.unpack_from('<H', data, pe + 20)
    opt = pe + 24
    _need(data, opt, 2, 0)
    magic, = struct.unpack_from('<H', data, opt)
    if magic == 0x10B:
        _need(data, opt + 28, 4, 0)
        base, = struct.unpack_from('<I', data, opt + 28)
    elif magic == 0x20B:
        _need(data, opt + 24, 8, 0)
        base, = struct.unpack_from('<Q', data, opt + 24)
    else:
        raise SoundError(opt, 'optional header')
    if nsec == 0 or nsec > 96:
        raise SoundError(pe + 6, 'sections')
    table = opt + optsz
    if table + nsec * 40 > size:
        raise SoundError(pe + 6, 'section table')
    secs = []
    for i in range(nsec):
        va, rs, rp = struct.unpack_from('<III', data, table + 40 * i + 12)
        chars, = struct.unpack_from('<I', data, table + 40 * i + 36)
        secs.append({'va': va, 'raw': min(rp, size), 'end': min(rp + rs, size), 'chars': chars})

    def find(ident):
        found = []
        for s in secs:
            if not s['chars'] & 0x40 or s['end'] < s['raw'] + 152:
                continue
            o = (s['raw'] + 3) & ~3
            while o + 152 <= s['end']:
                r = _music_rec(data[o:o + 76])
                if (r and not any(data[o:o + 32]) and r[1] == 0 and r[2] == ident and r[4:] == (0, 0, 0)
                        and _music_rec(data[o + 76:o + 152])):
                    found.append((o, s))
                o += 4
        if not found:
            raise SoundError(0, 'no table')
        if len(found) > 1:
            raise SoundError(found[1][0], 'several candidates')
        return found[0]
    st, ssec = find(1000)
    sq, qsec = find(2000)
    if qsec is not ssec or sq < st + 76:
        raise SoundError(sq, 'sequence table does not follow')
    end = ssec['end']
    states, o = [], st
    while o + 76 <= sq and _music_rec(data[o:o + 76]):
        states.append(_music_rec(data[o:o + 76]))
        o += 76
    if sq - o >= 76:
        raise SoundError(o, 'bad state record')
    for k in range(o, sq):
        if data[k]:
            raise SoundError(k, 'gap not zero')
    seqs, o = [], sq
    while True:
        if o + 64 <= end and _file_rec(data[o:o + 64]):
            break
        if o + 76 > end or not _music_rec(data[o:o + 76]):
            raise SoundError(o, 'bad sequence record')
        seqs.append(_music_rec(data[o:o + 76]))
        o += 76
    f_at = o
    files = []
    while True:
        if o + 64 > end:
            raise SoundError(o, 'file table not ended')
        if data[o] == 0:
            break
        r = _file_rec(data[o:o + 64])
        if not r:
            raise SoundError(o, 'bad file record')
        files.append(r)
        o += 64
    for got, want, at in ((len(states), 87, st), (len(seqs), 82, sq), (len(files), 167, f_at)):
        if got != want:
            raise SoundError(at, 'count')

    def addr(off):
        return base + ssec['va'] + (off - ssec['raw'])
    keys = [k for k, _ in files]

    def resolve(key):
        return -1 if not key or key not in keys else keys.index(key)
    unmatched = sum(1 for r in states + seqs if r[0] and resolve(r[0]) < 0)
    out = ['# iMUSE music tables of %s, written by TPMExtractor (fork)' % sound_printable(label),
           '# states: %d records at file offset 0x%X (address 0x%X)' % (len(states), st, addr(st)),
           '# sequences: %d records at file offset 0x%X (address 0x%X)' % (len(seqs), sq, addr(sq)),
           '# files: %d records at file offset 0x%X (address 0x%X)' % (len(files), f_at, addr(f_at)),
           '# transition: 0 none, 3 crossfade into the file, 4 crossfade, then back to the state at the file\'s '
           '_end marker',
           '# file: the file record whose key equals the record\'s key byte for byte, as the engine looks keys up; '
           '- for none',
           '# records with a key that matches no file key byte for byte: %d' % unmatched,
           '# table\tindex\tid\ttransition\tattribute\thook\tfade\tkey\tsymbol\tfile']
    for name, recs in (('state', states), ('sequence', seqs)):
        for i, (key, trans, ident, sym, attrib, hook, fade) in enumerate(recs):
            f = resolve(key)
            out.append('%s\t%d\t%d\t%d\t%d\t%d\t%d\t"%s"\t"%s"\t%s' % (
                name, i, ident, trans, attrib, hook, fade, sound_printable(key), sound_printable(sym),
                '-' if f < 0 else '"%s"' % sound_printable(files[f][1])))
    out.append('# table\tindex\tkey\tfile')
    for i, (k, f) in enumerate(files):
        out.append('file\t%d\t"%s"\t"%s"' % (i, sound_printable(k), sound_printable(f)))
    return '\n'.join(out) + '\n', {'states': len(states), 'sequences': len(seqs), 'files': len(files),
                                  'unmatched': unmatched}


def imuse_mutants():
    """Damaged and wrong music-table DLLs: [(name, bytes)]; the model gives each one's outcome."""
    states, seqs, files = imuse_tables_spec()
    good, at = make_imuse_dll()
    out = [('dll_short_states', make_imuse_dll(states[:-1], seqs, files)[0]),
           ('dll_more_files', make_imuse_dll(states, seqs, files + [('extra', 'EXTRA.IMC')])[0]),
           ('dll_gap8', make_imuse_dll(states, seqs, files, gap=8)[0]),
           ('dll_gap80', make_imuse_dll(states, seqs, files, gap=80)[0]),
           ('dll_nodecoy', make_imuse_dll(decoy=False)[0])]
    out += [('dll_' + k, d) for k, d in _cuts(good, [0, 1, 0x3D, 0x90, 0x100, at['state'] + 76 * 40,
                                                     at['file'] + 64 * 100])]
    def patch(name, off, raw):
        b = bytearray(good)
        b[off:off + len(raw)] = raw
        out.append(('dll_' + name, bytes(b)))
    patch('mz', 0, b'XZ')
    patch('peoff', 0x3C, struct.pack('<I', 0x7FFFFFF0))
    patch('pesig', 0x80, b'PX')
    patch('magic', 0x98, b'\x00\x00')
    patch('nsec', 0x86, struct.pack('<H', 200))
    patch('trans', at['state'] + 76 * 30 + 32, struct.pack('<I', 99))
    patch('symbol', at['sequence'] + 76 * 20 + 40, b'\x01')
    patch('filekey', at['file'] + 64 * 50, b'\x7F')
    patch('gapbyte', at['sequence'] - 2, b'\x01')
    patch('secondnull', at['state'] + 76 * 5, _imuse_rec('', 0, 1000, 'AGAIN'))
    patch('nonull', at['sequence'] + 36, struct.pack('<I', 2001))
    patch('initflag', 0x98 + 224 + 40 + 36, struct.pack('<I', 0xC0000020))
    return out


# ============================================================================ LAB contents and the synthetic disc

def _bmp_sample(w, h, seed):
    rng = Lcg(seed)
    return bmp24(w, h, [(rng.below(256), rng.below(256), rng.below(256)) for _ in range(w * h)], True)


def _sprite_sample():
    return b'Sprite\0\0' + struct.pack('<4I', 1, 4, 4, 0) + tex_checker(4, 4, 1, 2)


def embedded_samples():
    """Samples of the files once thought to be embedded in level files, as big.lab stores them
    (LAB type codes in reading order): a retail-format default colormap, two default MATs (type 2
    with 3 mips; type 3 with its palette and a masked texture), BMPs, text and a sprite."""
    dflt_mat = mat_stream(2, [{'w': 32, 'h': 32, 'transparent': 0,
                               'mips': [tex_gradient(32, 32, 1, 200), tex_gradient(16, 16, 1, 200),
                                        tex_gradient(8, 8, 1, 200)]}])
    shadow = mat_stream(3, [{'w': 32, 'h': 32, 'transparent': 1, 'mips': [tex_holes(tex_gradient(32, 32, 1, 9), 2)]}],
                        palette=grayscale_palette())
    fullbright = frozenset(range(32)) | {63}
    trans = bytes((d if s == 0 else (s + d) // 2) & 0xFF for s in range(256) for d in range(256))
    return [
        ('dflt.cmp', 'PALT', retail_cmp_bytes(PAL_A, ramp_light_table(fullbright), trans)),
        ('dflt.mat', 'MATS', dflt_mat),
        ('shadow.mat', 'MATS', shadow),
        ('scene1.bmp', 'BMPS', _bmp_sample(12, 6, 1)),
        ('menu1.bmp', 'MENU', _bmp_sample(8, 8, 2)),
        ('readme.txt', 'TEXT', b'Synthetic test text.\r\nLine two.\r\n'),
        ('spark.spr', 'SPRT', _sprite_sample()),
        ('step.wav', 'WAVE', b'RIFF' + struct.pack('<I', 36) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1,
                                                                                         11025, 11025, 1, 8)
         + b'data' + struct.pack('<I', 0)),
    ]


def big_lab_entries():
    """big.lab: the BAF fixtures, the embedded-file samples, and a name that only looks like a
    BAF (it must not be processed)."""
    entries = [(name, 'BAFF', build_baf(spec)[0]) for name, spec in baf_fixtures()]
    entries += embedded_samples()
    entries.append(('notes.bafx', 'TEXT', b'not a BAF\r\n'))
    return entries


def localize_lab_entries():
    return [('font1.spr', 'FONT', _sprite_sample()), ('strings.txt', 'TEXT', b'one\r\ntwo\r\n'),
            ('title.bmp', 'BMPS', _bmp_sample(4, 2, 3))]


def voice_lab_entries():
    """VOICE.LAB: synthetic voice files (type BSND, as the game's), in both letter cases."""
    fx = {n: d for n, d, _ in voice_fixtures()}
    return [('LINE1.WV', 'BSND', fx['sine.wv']), ('line2.wv', 'BSND', fx['stereo.wv']),
            ('Line3.Wv', 'BSND', fx['wvsm.wv'])]


def disc_music_files():
    """The music folder of the synthetic disc: [(file name, bytes)] (lower-case names, so that a
    lower-case copy of the disc gives the same output names)."""
    return [(n, d) for n, d, _ in music_fixtures() if n in ('loop.imc', 'oneshot.imc', 'plain.imc')]


def sound_lab_entries():
    """A LAB with voice files among other entries: a damaged voice file, a voice file whose WAV
    name the archive also uses, and a name that only looks like a voice file."""
    fx = {n: d for n, d, _ in voice_fixtures()}
    return [('first.wv', 'BSND', fx['chirp.wv']), ('notes.txt', 'TEXT', b'not a voice file\r\n'),
            ('broken.wv', 'BSND', fx['noise.wv'][:300]), ('clash.wv', 'BSND', fx['one.wv']),
            ('clash.wav', 'WAVE', b'RIFF0000WAVE'), ('Second.WV', 'BSND', fx['quirk.wv']),
            ('looks.wvx', 'BSND', fx['sine.wv'])]


def write_file(path, data):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_disc(root, levels=None, voice=True, big_members=None, music=None):
    """Writes a synthetic disc under `root` and returns a description of it:
       GAMEDATA/GOBS/BIG.Z (big.lab inside), GAMEDATA/GOBS/LOCALIZE.LAB, optionally
       GAMEDATA/GOBS/VOICE.LAB, and GAMEDATA/LEVEL/<file> for each (file, level spec) or
       (file, level spec, compressed) in `levels` (default: the compressed BOX.B3D and
       FLAT.B3D), and with `music` (a folder relative to the root, e.g. "MUSIC") the
       disc_music_files there. The description maps each level file name to (file bytes,
       decompressed image)."""
    big = make_lab(big_lab_entries())
    members = big_members or [{'path': 'big.lab', 'data': big, 'method': 'implode', 'dict_bits': 6}]
    write_file(os.path.join(root, 'GAMEDATA', 'GOBS', 'BIG.Z'), make_is3(members))
    loc = make_lab(localize_lab_entries())
    write_file(os.path.join(root, 'GAMEDATA', 'GOBS', 'LOCALIZE.LAB'), loc)
    voice_lab = make_lab(voice_lab_entries()) if voice else None
    if voice:
        write_file(os.path.join(root, 'GAMEDATA', 'GOBS', 'VOICE.LAB'), voice_lab)
    fixtures = level_fixtures()
    if levels is None:
        levels = [(n, fixtures[n]) for n in ('BOX.B3D', 'FLAT.B3D')]
    imgs = {}
    files = {}
    for entry in levels:
        fname, spec = entry[0], entry[1]
        compressed = entry[2] if len(entry) > 2 else True
        data, image = level_file(fname, spec, compressed)
        write_file(os.path.join(root, 'GAMEDATA', 'LEVEL', fname), data)
        imgs[fname] = image
        files[fname] = (data, image)
    music_files = disc_music_files() if music else []
    for name, data in music_files:
        write_file(os.path.join(root, music, name), data)
    return {'big_lab': big, 'localize': loc, 'voice': voice_lab, 'levels': imgs, 'level_files': files,
            'music': music_files}


def multi_z_members():
    """Members for a .Z archive that covers every storage case the reader supports."""
    rng = Lcg(99)
    noise = bytes(rng.below(256) for _ in range(3000))
    text = b''.join(b'generic line of fixture text %03d\r\n' % (i % 37) for i in range(400))
    return [
        {'path': 'stored.bin', 'data': bytes(range(256)) * 3, 'method': 'stored'},
        {'path': 'text4.txt', 'data': text, 'method': 'implode', 'dict_bits': 4},
        {'path': 'text5.txt', 'data': text[:5000], 'method': 'implode', 'dict_bits': 5, 'coded': True},
        {'path': 'noise.bin', 'data': noise + noise[:1500] + noise, 'method': 'implode', 'dict_bits': 6},
        {'path': 'empty.bin', 'data': b'', 'method': 'implode', 'dict_bits': 6},
        {'path': 'sub\\nested.bin', 'data': tex_gradient(64, 64, 0, 255), 'method': 'implode', 'dict_bits': 6,
         'coded': True},
        {'path': 'sub\\stored2.bin', 'data': b'xyz' * 10, 'method': 'stored'},
    ]


# ============================================================================ LZSS streams for the decoder differential

def lzss_cases():
    """(name, payload stream) pairs for the level decoder differential. Each becomes a level
    file: a raw 0x8A4-byte header (with the compressed flag set) followed by the stream."""
    rng = Lcg(2024)
    seg = bytes(rng.below(256) for _ in range(700))
    base = bytearray()
    for k in range(240):
        pick = rng.below(4)
        if pick == 0:
            base += bytes(rng.below(256) for _ in range(rng.below(40) + 1))
        elif pick == 1:
            base += seg[rng.below(600):][:rng.below(90) + 2]
        elif pick == 2:
            base += bytes([rng.below(256)]) * (rng.below(30) + 2)
        else:
            start = max(0, len(base) - rng.below(4096) - 1)
            base += base[start:start + rng.below(60) + 2]
    block = bytes(base[:5000])
    far = block + bytes(rng.below(256) for _ in range(4096 - len(block) % 4096)) + block[:200]
    big = bytes(base) * 30
    zeros = bytes(300) + b'\x01\x02' + bytes(40)
    cases = [
        ('literal_only', lzss_encode(bytes(base[:3000]), 'literal')),
        ('greedy_small', lzss_encode(bytes(base[:2000]))),
        ('greedy_ring_wrap', lzss_encode(bytes(base))),
        ('distance_4096', lzss_encode(far)),
        ('unwritten_ring_zeros', lzss_encode(zeros + bytes(base[:500]) + bytes(20), 'exotic')),
        ('overlap_runs', lzss_encode(b'a' * 1000 + b'ab' * 500 + b'abc' * 300)),
        ('long_output_chunks', lzss_encode(big)),
        ('empty_stream', lzss_encode(b'')),
        ('pad_with_ones', lzss_encode(bytes(base[:777]), pad_ones=True)),
        ('trailing_garbage', lzss_encode(bytes(base[:1500])) + bytes(range(256)) * 2),
        ('no_end_marker', lzss_encode(bytes(base[:1500]), end_marker=False)),
        ('no_payload', b''),
    ]
    full = lzss_encode(bytes(base[:2500]))
    for cut in (1, 2, 3, len(full) // 3, len(full) // 2, len(full) - 3, len(full) - 2, len(full) - 1):
        cases.append(('truncated_at_%d' % cut, full[:cut]))
    for nbits in range(8):
        w = BitWriterMSB()
        for b in bytes(base[:nbits + 3]):
            w.bits(1, 1)
            w.bits(b, 8)
        w.bits(0, 13)
        cases.append(('end_marker_phase_%d' % nbits, w.getvalue()))
    return cases


def lzss_level_file(stream):
    """A level file around an LZSS payload: a real header (flag set) and the payload."""
    head = build_level(dict(level_fixtures()['FLAT.B3D'], flag=1))[:B3D_RAW_HEADER]
    return head + stream


# ============================================================================ damaged and hostile inputs
#
# Each generator returns [(file name, bytes)]: copies of the fixtures cut at structure boundaries
# or with one count, size or offset field overwritten by an extreme value. A tool must report
# them (with the file name) and go on; it must never crash, hang or read out of bounds.

HUGE = 0x7FFFFFFF


def _put32(data, off, value):
    b = bytearray(data)
    if 0 <= off and off + 4 <= len(b):
        struct.pack_into('<I', b, off, value & 0xFFFFFFFF)
    return bytes(b)


def _put16(data, off, value):
    b = bytearray(data)
    if 0 <= off and off + 2 <= len(b):
        struct.pack_into('<H', b, off, value & 0xFFFF)
    return bytes(b)


def _cuts(data, points):
    return [('cut%06x' % n, data[:n]) for n in sorted(set(p for p in points if 0 <= p < len(data)))]


def baf_mutants(spec):
    """Damaged copies of one BAF fixture: [(name, bytes)], names '<stem>_<what>.baf'."""
    data, lay = build_baf(spec)
    stem = spec['file'].split('.')[0]
    cmap = BAF_CMP_BLOCK if spec.get('colormap') else 0
    m0 = spec['materials'][0]
    ncel = len(m0.get('cels') or m0['textures'])
    mat0 = BAF_HEADER + cmap + 32                       # first MAT stream
    tex0 = mat0 + 76 + 40 * ncel                        # its first texture header
    model = lay['model_off']
    meshes = spec['model']['meshes']
    mesh0 = model + MODEL_HEADER
    arrays0 = mesh0 + MESH_HEADER * len(meshes)          # mesh 0's vertex arrays
    nv, nuv = len(meshes[0]['verts']), len(meshes[0]['uvs'])
    face0 = arrays0 + 32 * nv + 8 * nuv                  # mesh 0's first face
    key = lay['key_off']
    mats_end = model
    points = [0, 1, 4, BAF_HEADER - 1, BAF_HEADER, BAF_HEADER + cmap // 2, mat0 + 40, tex0 + 10,
              mats_end - 7, model + 0x40, mesh0 + 0x30, arrays0 + 6, face0 + 0x20, key - 1,
              lay['size'] - 1]
    fields = [('nummats', 0xC4, HUGE), ('numkeys', 0xC8, HUGE), ('cmapword', 0xCC, 0x00FFFFFF),
              ('modelsize', 0xC0, HUGE), ('offmodel', 0xE0, HUGE),
              ('celcount', mat0 + 12, HUGE), ('texcount', mat0 + 16, HUGE),
              ('texwidth', tex0, HUGE), ('texnegwidth', tex0, -8), ('texheight0', tex0 + 4, 0),
              ('mipcount', tex0 + 20, HUGE),
              ('meshcount', model + 0x24, HUGE), ('vertcount', mesh0 + 0x48, HUGE),
              ('uvcount', mesh0 + 0x4C, HUGE), ('facecount', mesh0 + 0x50, HUGE),
              ('facevertcount', face0 + 0x14, HUGE), ('facematindex', face0, HUGE),
              ('nodecount', model + 0x54, HUGE)]
    if spec.get('keys'):
        points += [key + 0x30, key + 0x40 + 0x84 + 0x10, lay['extras_off'] + 10]
        fields += [('keyframes', key + 0x40 + 0x34, HUGE), ('keyjoints', key + 0x40 + 0x38, HUGE),
                   ('entrycount', key + 0x40 + 0x84 + 0x24, HUGE), ('extracount', key + 0x1C, HUGE),
                   ('wrappersize', key, HUGE)]
    out = [('%s_%s.baf' % (stem, n), d) for n, d in _cuts(data, points)]
    out += [('%s_%s.baf' % (stem, n), _put32(data, off, v)) for n, off, v in fields]
    # several fields at once: width and height both huge (width * height * bits per pixel
    # overflows 64-bit arithmetic), and a negative colour depth with a texture to read
    out.append(('%s_texhuge.baf' % stem, _put32(_put32(data, tex0, HUGE), tex0 + 4, HUGE)))
    out.append(('%s_bppneg.baf' % stem, _put32(data, mat0 + 24, -0x80000000)))
    return out


def unsafe_name_baf():
    """A BAF whose material, colormap, model and key names try to leave the output folder or
    name devices. Every one must be refused; nothing may be written outside the output folder."""
    spec = dict(baf_fixtures()[2][1])                   # the stub fixture
    spec['file'] = 'names.baf'
    spec['colormap'] = dict(spec['colormap'], name='../up.cmp')
    spec['materials'] = [dict(spec['materials'][0], name='..\\up.mat'),
                         dict(spec['materials'][0], name='nul.mat'),
                         dict(spec['materials'][0], name='ok.mat')]
    spec['model'] = dict(spec['model'], name='/abs.3do')
    spec['keys'] = [dict(spec['keys'][0], name='c:drive.key'), dict(spec['keys'][0], name='fine.key')]
    return spec


def lab_mutants():
    """Damaged copies of a sequential and a scrambled LAB: [(name, bytes)]."""
    out = []
    for layout in ('sequential', 'scrambled'):
        lab = make_lab(localize_lab_entries(), layout)
        n = struct.unpack_from('<I', lab, 8)[0]
        table = 16 + 16 * n
        tsize = struct.unpack_from('<I', lab, 12)[0]
        tag = layout[:3]
        out += [('%s_%s.lab' % (tag, k), d) for k, d in
                _cuts(lab, [0, 3, 15, 16, 16 + 8, table - 1, table + 3, table + tsize + 2, len(lab) - 1])]
        fields = [('count', 8, HUGE), ('countplus', 8, n + 5), ('tablesize', 12, HUGE),
                  ('nameoff', 16, HUGE), ('dataoff', 20, 0xFFFFFFF0), ('length', 24, HUGE),
                  ('lastlength', 16 + 16 * (n - 1) + 8, HUGE)]
        out += [('%s_%s.lab' % (tag, k), _put32(lab, off, v)) for k, off, v in fields]
        unterminated = bytearray(lab)
        for i in range(table, table + tsize):
            if unterminated[i] == 0:
                unterminated[i] = 0x41
        out.append(('%s_noterm.lab' % tag, bytes(unterminated)))
    return out


def unsafe_name_lab():
    """A LAB whose entry names try to leave the output folder or name devices, plus two normal
    entries that must still be extracted."""
    return make_lab([('../up.txt', 'TEXT', b'x'), ('..\\up2.txt', 'TEXT', b'x'), ('sub/dir.txt', 'TEXT', b'x'),
                     ('c:drive.txt', 'TEXT', b'x'), ('con', 'TEXT', b'x'), ('aux.txt', 'TEXT', b'x'),
                     ('dot.', 'TEXT', b'x'), ('ok1.txt', 'TEXT', b'one'), ('', 'TEXT', b'x'),
                     ('ok2.txt', 'TEXT', b'two')])


def level_mutants():
    """Damaged level files, compressed and not: [(name, bytes)]."""
    spec = level_fixtures()['BOX.B3D']
    comp, image = level_file('BOX.B3D', spec, True)
    raw, _ = level_file('BOX.B3D', spec, False)
    secs = {s[0]: s for s in walk_sections(image)}
    pals, tex = secs['B3D_PALS'], secs['B3D_TEX ']
    out = [('c%s.b3d' % k, d) for k, d in
           _cuts(comp, [0, 0x10, B3D_RAW_HEADER - 1, B3D_RAW_HEADER, B3D_RAW_HEADER + 1,
                        B3D_RAW_HEADER + (len(comp) - B3D_RAW_HEADER) // 2, len(comp) - 2, len(comp) - 1])]
    out += [('r%s.b3d' % k, d) for k, d in
            _cuts(raw, [pals[1] + 12 + 100, secs['B3D_GMAP'][1] + 30, tex[1] + 12 + 20, len(raw) - 1])]
    fields = [('texsize', tex[1] + 8, HUGE), ('texwidth', tex[1] + 12, HUGE), ('texnegwidth', tex[1] + 12, -4),
              ('texheight0', tex[1] + 16, 0), ('texpalette', tex[1] + 20, 999), ('texpalneg', tex[1] + 20, -1),
              ('palssize', pals[1] + 8, 100), ('palsbig', pals[1] + 8, HUGE), ('headercount', 12 + 0x20, HUGE),
              ('headertex', 12 + 0x60, HUGE), ('filesize', 8, 2199), ('gridw', 12 + 0x28, HUGE),
              ('lights', 12 + 0x68, HUGE)]
    out += [('r_%s.b3d' % k, _put32(raw, off, v)) for k, off, v in fields]
    rng = Lcg(77)
    noise = bytes(rng.below(256) for _ in range(3000))
    out.append(('c_noise.b3d', comp[:B3D_RAW_HEADER] + noise))
    out.append(('c_nopayload.b3d', comp[:B3D_RAW_HEADER]))
    return out


def z_mutants():
    """Damaged copies of the multi-member .Z archive: [(name, bytes)]."""
    z = make_is3(multi_z_members())
    toc = struct.unpack_from('<I', z, 41)[0]
    ndirs = struct.unpack_from('<H', z, 49)[0]
    p = toc
    for _ in range(ndirs):
        p += struct.unpack_from('<H', z, p + 2)[0]
    file0 = p                                            # first file entry of the table of contents
    file1 = file0 + struct.unpack_from('<H', z, file0 + 23)[0]
    out = [('%s.z' % k, d) for k, d in
           _cuts(z, [0, 4, 0x30, IS3_HEADER_AREA - 1, IS3_HEADER_AREA + 10, toc - 1, toc, toc + 5, file0 + 12,
                     len(z) - 1])]
    out += [('%s.z' % k, _put32(z, off, v)) for k, off, v in
            [('signature', 0, 0x12345678), ('tocoffset', 41, 0xFFFFFFF0), ('usize', file0 + 3, HUGE),
             ('csize', file0 + 7, HUGE), ('offset', file0 + 11, 0xFFFFFFF0), ('csize2', file1 + 7, HUGE)]]
    out += [('%s.z' % k, _put16(z, off, v)) for k, off, v in
            [('filecount', 12, 0xFFFF), ('dircount', 49, 0xFFFF), ('dirchunk', toc + 2, 0),
             ('filechunk', file0 + 23, 0)]]
    namelen = bytearray(z)
    namelen[file0 + 29] = 0xFF
    out.append(('namelen.z', bytes(namelen)))
    damaged = bytearray(z)                               # the second member is imploded: break its stream
    data1 = struct.unpack_from('<I', z, file1 + 11)[0]
    for i in range(data1 + 4, min(data1 + 40, len(damaged))):
        damaged[i] ^= 0x5A
    out.append(('implodedata.z', bytes(damaged)))
    return out


def unsafe_name_z():
    """A .Z archive whose member paths try to leave the output folder, plus normal members."""
    return make_is3([{'path': '..\\up.bin', 'data': b'x', 'method': 'stored'},
                     {'path': 'c:drive.bin', 'data': b'x', 'method': 'stored'},
                     {'path': 'ok.bin', 'data': b'fine', 'method': 'stored'},
                     {'path': 'sub\\..\\..\\up2.bin', 'data': b'x', 'method': 'stored'},
                     {'path': 'sub\\ok2.bin', 'data': b'fine2', 'method': 'implode'}])


# ============================================================================ self-test and command line

def self_test():
    problems = []

    def check(cond, what):
        if not cond:
            problems.append(what)

    for name, stream in lzss_cases():
        out, end, _ = lzss_decode(stream)
        check(end in ('marker', 'input'), 'lzss %s: end %s' % (name, end))
    rng = Lcg(5)
    sample = bytes(rng.below(7) for _ in range(9000)) + bytes(500) + bytes(range(256)) * 4
    for mode in ('literal', 'greedy', 'exotic'):
        enc = lzss_encode(sample, mode)
        out, end, used = lzss_decode(enc)
        check(out == sample and end == 'marker' and used == len(enc), 'lzss round trip (%s)' % mode)
    for coded in (False, True):
        for db in (4, 5, 6):
            for data in (b'', b'a', sample[:5000], bytes(range(256)) * 20):
                check(dcl_explode(dcl_implode(data, coded, db)) == data, 'dcl round trip coded=%s dict=%d' % (coded, db))
    check(sorted(DCL_LIT_CODES[0]) == list(range(256)), 'literal code covers 256 symbols')
    for name, spec in level_fixtures().items():
        image = build_level(dict(spec, flag=1))
        secs = walk_sections(image)
        check(secs[0][0] == 'B3D FILE' and secs[-1][0] == 'B3D_END2', 'level %s chain ends' % name)
        bmps = level_bmps(dec_of(image), False)
        check(len(bmps) == len(spec['textures']), 'level %s texture count' % name)
        legacy = legacy_b3d_outputs(dec_of(image))
        if name == 'DECOY.B3D':
            check(legacy == [], 'decoy level: the original finds no textures')
        else:
            check(legacy == bmps, 'level %s: legacy and walk agree' % name)
        out, end, _ = lzss_decode(compress_level(image)[B3D_RAW_HEADER:])
        check(end == 'marker' and image[:B3D_RAW_HEADER] + out == image, 'level %s compresses' % name)
        g = decode_geometry(image)
        _, _, st = geometry_text(g, name.split('.')[0])
        check(st['faces'] == 2 and st['kept'] == 2 and not st['untextured'],
              'level %s: 2 faces in their stored order (%r)' % (name, st))
    # the geometry vector: pinned bytes, the decoder's view of it, and the pinned OBJ/MTL data
    dec, b3d = shapes_image(), shapes_file()
    check(hashlib.sha256(dec).hexdigest() == SHAPES_DEC_SHA256, 'SHAPES.dec hash')
    check(hashlib.sha256(b3d).hexdigest() == SHAPES_B3D_SHA256, 'SHAPES.B3D hash')
    out, end, _ = lzss_decode(b3d[B3D_RAW_HEADER:])
    check(end == 'marker' and dec_of(b3d[:B3D_RAW_HEADER] + out) == dec, 'SHAPES.B3D decompresses to SHAPES.dec')
    check([s[0] for s in walk_sections(dec)][-1] == 'B3D_END1', 'SHAPES ends with B3D_END1')
    g = decode_geometry(dec)
    obj, mtl, st = geometry_text(g, 'SHAPES')
    check(data_line_sha256(crlf(obj)) == SHAPES_OBJ_DATA_SHA256, 'SHAPES.obj data lines')
    check(data_line_sha256(crlf(mtl)) == SHAPES_MTL_DATA_SHA256, 'SHAPES.mtl data lines')
    check((st['kept'], st['untextured'], st['rule_differs'], st['vt']) == (1, [], 1, 52), 'SHAPES counts %r' % st)
    check(st['categories'] == {'static': 12, 'amap0': 1, 'hidden': 1}, 'SHAPES categories')
    check(check_links(g) == (1, 0, 0), 'SHAPES LINK record resolves')
    check([q.offset for q in g['polys']] == [3788, 3868, 3932, 4012, 4092, 4156, 4237, 4318, 4398, 4479, 4639,
                                             4703, 4783, 4847], 'SHAPES polygon offsets')
    obj1, _, _ = geometry_text(g, 'SHAPES', scale=1.0)
    keep = lambda text: [l for l in text.split('\n') if not l.startswith(('v ', '#'))]
    check(keep(obj1) == keep(obj) and obj1.split('\n')[11] == 'v 1.500000 0.250000 1.500000',
          'SHAPES at scale 1: only the v lines change')
    # OBJ/MTL file names: ASCII, nothing an OBJ reader splits (one '_' per UTF-8 character or stray byte)
    for given, want in (('SHAPES', 'SHAPES'), ('lower', 'lower'), ('my level', 'my_level'), ('a#b(1)', 'a_b_1_'),
                        ('x-y_z.w', 'x-y_z.w'), ('niveau_\u00e9', 'niveau__'), (b'niveau_\xc3\xa9', 'niveau__'),
                        (b'niveau_\xe9', 'niveau__'), (b'a\xe2\x82b', 'a__b'), (b'a\xf0\x9f\x98\x80b', 'a_b'),
                        ('\u00e9', 'level'), ('', 'level'), ('__', 'level'), ('-', '-')):
        check(obj_name(given) == want, 'obj_name(%r) is %r, not %r' % (given, obj_name(given), want))
    objn, mtln, _ = geometry_text(g, 'my level')
    check(objn.split('\n')[1] == 'mtllib my_level.mtl' and objn.replace('my_level', 'SHAPES') == obj
          and mtln.replace('my_level', 'SHAPES') == mtl,
          'geometry_text: a level named "my level" names my_level.mtl, and nothing else changes')
    for mid, patch, outcome, detail in SHAPES_MUTATIONS:
        try:
            gm = decode_geometry(shapes_mutant(patch))
        except GeometryError as e:
            check(outcome == 'error' and e.offset == detail, 'mutation %s: refused at 0x%x' % (mid, e.offset))
            continue
        check(outcome != 'error', 'mutation %s: not refused' % mid)
        objm, _, stm = geometry_text(gm, 'SHAPES')
        if outcome == 'face':
            check(stm['untextured'] == detail, 'mutation %s: untextured %r' % (mid, stm['untextured']))
        else:
            check(objm == obj and (bool(gm['warnings']) or check_links(gm)[1] > 0), 'mutation %s: warning' % mid)
    for name, spec in baf_fixtures():
        data, layout = build_baf(spec)
        files = legacy_baf_outputs(data)
        names = [f[0] for f in files]
        check(b'REMAIN.HEX' in names, 'baf %s REMAIN.HEX' % name)
        if spec.get('colormap'):
            check(spec['model']['name'].encode() in names, 'baf %s model written' % name)
            remain = [f[1] for f in files if f[0] == b'REMAIN.HEX'][0]
            nextra = len(spec['extras'])
            check(len(remain) == max(0, 96 * nextra - 64), 'baf %s REMAIN.HEX size %d' % (name, len(remain)))
        cm = legacy_baf_outputs(build_baf(with_colormap(spec))[0])
        check(spec['model']['name'].encode() in [f[0] for f in cm], 'baf %s: colormap copy writes the model' % name)
        check(len(baf_mutants(spec)) > 20, 'baf %s: damaged copies' % name)
    lab = make_lab([('a.txt', 'TEXT', b'xyz'), ('b.bin', 'BINR', b'12345')], 'scrambled')
    check(lab_outputs(lab, False) == [('a.txt', b'xyz'), ('b.bin', b'12345')], 'lab stored offsets')
    check(lab_outputs(lab, True)[0][0] == 'b.bin', 'lab sequential read takes the names in table order')
    z = make_is3(multi_z_members())
    check(z[:4] == struct.pack('<I', IS3_SIG1), 'is3 signature')
    check(sorted(PERM_A[1:]) == list(range(1, 256)) and PERM_A[0] == 0, 'palette permutation')
    check(all(PAL_PERM[PERM_A[i]] == PAL_A[i] for i in range(256)), 'permuted palette entries')
    check(len(set(PAL_A)) == 256, 'PAL_A has distinct entries')
    for gen in (lab_mutants, level_mutants, z_mutants):
        names = [n for n, _ in gen()]
        check(len(names) == len(set(names)) and len(names) > 10, '%s: distinct names' % gen.__name__)
    # sound: the model decodes what the test encoder meant, and gives the documented warnings
    warn_kinds = {'oddpcm.wv': ['frames'], 'datasize.wv': ['datasize'], 'wvsmodd.wv': ['frames']}
    totals = {'escapes': 0, 'clamps': 0}
    for name, data, rec in voice_fixtures():
        m = wv_model(data)
        check(m['samples'] == rec, 'voice %s: the model does not decode what the encoder meant' % name)
        check([k for _, k in m['warnings']] == warn_kinds.get(name, []), 'voice %s: warnings %r' % (name, m['warnings']))
        check(len(wv_expected(data)) == WAV_HEADER + 2 * len(rec), 'voice %s: WAV size' % name)
        if not m['wvsm']:
            st = {}
            h, pos = vima_read_header(data, 0x1E + struct.unpack_from('<I', data, 0x16)[0], 0, True)
            vima_decode_body(data, pos, 0, h, struct.unpack_from('<I', data, pos - h['size'] - 4)[0], st)
            for k in totals:
                totals[k] += st.get(k, 0)
    check(totals['escapes'] > 100 and totals['clamps'] > 10, 'voice fixtures exercise escapes and clamps %r' % totals)
    for name, data, dec in music_fixtures():
        m = imc_model(data, name)
        check(m['samples'] == dec, 'music %s: the model does not decode what the encoder meant' % name)
        check(m['text'].count('\n') >= 8 and not m['warnings'], 'music %s: map text' % name)
    dll, at = make_imuse_dll()
    text, counts = imuse_model(dll, 'IMUSE.DLL')
    check(counts == {'states': 87, 'sequences': 82, 'files': 167, 'unmatched': 1}, 'music tables %r' % counts)
    check('state\t0\t1000\t0\t0\t0\t0\t""\t"state_none"\t-\n' in text, 'music tables: the null state line')
    for gen, model in ((voice_mutants, lambda d: wv_model(d)), (music_mutants, lambda d: imc_model(d, 'x')),
                       (imuse_mutants, lambda d: imuse_model(d, 'x'))):
        refused = 0
        names = []
        for n, d in gen():
            names.append(n)
            try:
                model(d)
            except SoundError:
                refused += 1
        check(len(names) == len(set(names)) and refused >= len(names) - 3,
              '%s: %d of %d refused, distinct names' % (gen.__name__, refused, len(names)))
    for p in problems:
        print('self-test FAIL:', p)
    print('self-test: %d problem(s)' % len(problems))
    return 0 if not problems else 1


# Byte 0 of a fuzz_baf input chooses the run (see tests/fuzz_baf.cpp): default mode, --legacy,
# --radius animated with --insert-offset node0, --palette with --no-draw-scale and --no-mat8,
# and --no-mat16/--no-bmp/--no-debug-dumps with --radius stored, --angles rad2deg, --scale 0.5.
FUZZ_BAF_MODES = (0x00, 0x01, 0x0E, 0x33, 0xC2)


def write_fuzz_corpus(root):
    """Seed inputs for the fuzz targets (tests/fuzz_*.cpp): OUTDIR/<target>/<file>, one folder
    per target named after it without "fuzz_" (lab, lab_file, b3d, b3d_file, b3d_geom, cmp, baf,
    z). The file-level targets also get the damaged and hostile-name inputs."""
    def put(target, name, data):
        write_file(os.path.join(root, target, name), data)

    for layout in ('sequential', 'scrambled'):
        lab = make_lab(localize_lab_entries(), layout)
        for mode in (0, 1):               # byte 0: the layout the parser assumes; byte 1: extra file size
            put('lab', '%s_%d.bin' % (layout, mode), bytes([mode, 0]) + lab)
        put('lab_file', layout, lab)      # fuzz_lab_file reads the archive as it is
    put('lab_file', 'names', unsafe_name_lab())
    for name, data in lab_mutants():
        put('lab_file', 'damaged_' + name, data)

    for name, spec in level_fixtures().items():
        comp, image = level_file(name, spec, True)
        for target in ('b3d', 'b3d_file'):
            put(target, name.lower(), comp)
            put(target, name.lower() + '.dec', dec_of(image))
        put('b3d_file', name.lower() + '.raw', level_file(name, spec, False)[0])
    for name, stream in lzss_cases()[:12]:
        for target in ('b3d', 'b3d_file'):
            put(target, 'lzss_' + name, lzss_level_file(stream))
    for name, data in level_mutants():
        put('b3d_file', 'damaged_' + name, data)
    for target in ('b3d', 'b3d_file', 'b3d_geom'):
        put(target, 'shapes.b3d', shapes_file())
        put(target, 'shapes.dec', shapes_image())
    for name, spec in level_fixtures().items():
        put('b3d_geom', name.lower() + '.dec', dec_of(level_file(name, spec, True)[1]))
    for mid, patch, _, _ in SHAPES_MUTATIONS:
        for target in ('b3d_file', 'b3d_geom'):
            put(target, 'shapes_' + mid, shapes_mutant(patch))

    trans = bytes((s + d) // 2 for s in range(256) for d in range(256))
    put('cmp', 'retail.cmp', retail_cmp_bytes(PAL_A, ramp_light_table(), trans))
    put('cmp', 'nolight.cmp', retail_cmp_bytes(PAL_B, ramp_light_table()))
    put('cmp', 'legacy.cmp', legacy_cmp_bytes(PAL_C))

    for name, spec in baf_fixtures() + [('names.baf', unsafe_name_baf())]:
        data = build_baf(spec)[0]
        for mode in FUZZ_BAF_MODES:
            put('baf', '%s_%02x' % (name.split('.')[0], mode), bytes([mode]) + data)
    for name, data in baf_mutants(dict(baf_fixtures())['rig.baf']):
        for mode in FUZZ_BAF_MODES[:2]:   # default mode and --legacy
            put('baf', 'damaged_%s_%02x' % (name.split('.')[0], mode), bytes([mode]) + data)

    put('z', 'multi', make_is3(multi_z_members()))
    put('z', 'names', unsafe_name_z())
    for name, data in z_mutants():
        put('z', 'damaged_' + name, data)

    for name, data, _ in voice_fixtures():
        put('wv', name, data)
    for name, data in voice_mutants():
        put('wv', 'damaged_' + name, data)
    for name, data, _ in music_fixtures():
        put('imc', name, data)
    for name, data in music_mutants():
        put('imc', 'damaged_' + name, data)
    put('imuse', 'tables.dll', make_imuse_dll()[0])
    for name, data in imuse_mutants():
        put('imuse', 'damaged_' + name, data)
    put('lab_file', 'voice', make_lab(voice_lab_entries()))   # default mode decodes the voice files
    put('lab_file', 'voice_mixed', make_lab(sound_lab_entries()))


def write_all(outdir):
    disc = os.path.join(outdir, 'disc')
    write_disc(disc)
    loose = os.path.join(outdir, 'loose')
    for name, spec in baf_fixtures():
        write_file(os.path.join(loose, 'baf', name), build_baf(spec)[0])
    for name, spec in level_fixtures().items():
        write_file(os.path.join(loose, 'level', name), level_file(name, spec, True)[0])
        write_file(os.path.join(loose, 'level_raw', name), level_file(name, spec, False)[0])
    write_file(os.path.join(loose, 'level', 'SHAPES.B3D'), shapes_file())
    write_file(os.path.join(loose, 'level_raw', 'SHAPES.B3D'), shapes_image())
    for mid, patch, _, _ in SHAPES_MUTATIONS:
        write_file(os.path.join(outdir, 'damaged', 'geometry', 'shapes_%s.b3d' % mid), shapes_mutant(patch))
    write_file(os.path.join(loose, 'z', 'multi.z'), make_is3(multi_z_members()))
    write_file(os.path.join(loose, 'lab', 'scrambled.lab'), make_lab(localize_lab_entries(), 'scrambled'))
    write_file(os.path.join(loose, 'palette', 'target.pal'), palette_bytes(PAL_D))
    write_file(os.path.join(loose, 'palette', 'target.cmp'), legacy_cmp_bytes(PAL_D))
    write_file(os.path.join(loose, 'palette', 'perm.pal'), palette_bytes(PAL_PERM))
    for name, stream in lzss_cases():
        write_file(os.path.join(loose, 'lzss', name + '.b3d'), lzss_level_file(stream))
    for name, spec in baf_fixtures():
        for mname, data in baf_mutants(spec):
            write_file(os.path.join(outdir, 'damaged', 'baf', mname), data)
    for sub, gen in (('lab', lab_mutants), ('level', level_mutants), ('z', z_mutants)):
        for mname, data in gen():
            write_file(os.path.join(outdir, 'damaged', sub, mname), data)
    write_fuzz_corpus(os.path.join(outdir, 'fuzz'))
    write_file(os.path.join(outdir, 'hostile', 'names.baf'), build_baf(unsafe_name_baf())[0])
    write_file(os.path.join(outdir, 'hostile', 'names.lab'), unsafe_name_lab())
    write_file(os.path.join(outdir, 'hostile', 'names.z'), unsafe_name_z())
    for name, data, _ in voice_fixtures():
        write_file(os.path.join(loose, 'wv', name), data)
    for name, data, _ in music_fixtures():
        write_file(os.path.join(loose, 'imc', name), data)
    write_file(os.path.join(loose, 'imuse', 'IMUSE.DLL'), make_imuse_dll()[0])
    write_file(os.path.join(loose, 'lab', 'voice.lab'), make_lab(sound_lab_entries()))
    for sub, gen in (('wv', voice_mutants), ('imc', music_mutants), ('imuse', imuse_mutants)):
        for mname, data in gen():
            write_file(os.path.join(outdir, 'damaged', sub, mname), data)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('outdir', nargs='?', help='directory to write the fixtures to (created)')
    ap.add_argument('--self-test', action='store_true', help='check the writers against the readers')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.outdir:
        ap.error('give OUTDIR or --self-test')
    return write_all(a.outdir)


if __name__ == '__main__':
    sys.exit(main())
