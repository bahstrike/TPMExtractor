#!/usr/bin/env python3
# TPMExtractor fork - synthetic test fixtures (Python standard library only).
# Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
# Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
"""Synthetic fixtures for the TPMExtractor test suite.

Everything here is generated: generic names (box, rig, node_a, mat0), checker and gradient
textures, ramp and pseudo-random palettes, boxes and pyramids. No game data is read or copied.

Formats written (all little-endian):
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

  damaged inputs: copies of the fixtures cut at structure boundaries or with one count, size or
          offset field overwritten (baf_mutants, lab_mutants, level_mutants, z_mutants), and
          files whose entry names try to leave the output folder (unsafe_name_*).

Besides the writers, the module holds independent models that the test runner compares the tool
against:
  legacy_baf_outputs()   the original tool's BAF read order and output, file by file;
  legacy_b3d_outputs()   the original tool's level texture dump from a decompressed image;
  lab_outputs()          LAB extraction from the stored offsets and from the sequential read;
  is3_outputs()          the files a .Z archive holds;
  lzss_decode(), dcl_explode(), walk_sections(): reference readers used by --self-test and by
                         the decompressor differential.

Usage:
  python3 -I make_fixtures.py OUTDIR      write a synthetic disc (OUTDIR/disc), loose files and
                                          the damaged inputs
  python3 -I make_fixtures.py --self-test check every writer against the readers above
"""

import argparse
import math
import os
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
    return [(s['file'], s) for s in (box, rig, stub, leaf, bare)]


def rerooted(spec):
    """The same BAF with hierarchy node 0's z moved into the insert offset (the re-root of
    --insert-offset node0): node 0's z position becomes 0 and node 0's key positions lose node
    0's z (float subtraction, before scaling); deltas and every other node stay."""
    s = dict(spec)
    model = dict(spec['model'])
    nodes = [dict(n) for n in model['nodes']]
    z0 = f32(nodes[0]['pos'][2])
    nodes[0]['pos'] = (nodes[0]['pos'][0], nodes[0]['pos'][1], f32(z0 - z0))
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
    return [('line1.wv', 'WVSM', bytes(range(32))), ('line2.wv', 'WVSM', bytes(range(64, 80)))]


def write_file(path, data):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_disc(root, levels=None, voice=True, big_members=None):
    """Writes a synthetic disc under `root` and returns a description of it:
       GAMEDATA/GOBS/BIG.Z (big.lab inside), GAMEDATA/GOBS/LOCALIZE.LAB, optionally
       GAMEDATA/GOBS/VOICE.LAB, and GAMEDATA/LEVEL/<file> for each (file, level spec) or
       (file, level spec, compressed) in `levels` (default: the compressed BOX.B3D and
       FLAT.B3D). The description maps each level file name to (file bytes, decompressed
       image)."""
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
    return {'big_lab': big, 'localize': loc, 'voice': voice_lab, 'levels': imgs, 'level_files': files}


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
    for p in problems:
        print('self-test FAIL:', p)
    print('self-test: %d problem(s)' % len(problems))
    return 0 if not problems else 1


def write_fuzz_corpus(root):
    """Seed inputs for the fuzz targets (tests/fuzz_*.cpp): OUTDIR/<target>/<file>."""
    for layout in ('sequential', 'scrambled'):
        lab = make_lab(localize_lab_entries(), layout)
        for mode in (0, 1):               # byte 0: the layout the parser assumes; byte 1: extra file size
            write_file(os.path.join(root, 'lab', '%s_%d.bin' % (layout, mode)), bytes([mode, 0]) + lab)
    for name, spec in level_fixtures().items():
        comp, image = level_file(name, spec, True)
        write_file(os.path.join(root, 'b3d', name.lower()), comp)
        write_file(os.path.join(root, 'b3d', name.lower() + '.dec'), dec_of(image))
    for name, stream in lzss_cases()[:12]:
        write_file(os.path.join(root, 'b3d', 'lzss_' + name), lzss_level_file(stream))
    trans = bytes((s + d) // 2 for s in range(256) for d in range(256))
    write_file(os.path.join(root, 'cmp', 'retail.cmp'), retail_cmp_bytes(PAL_A, ramp_light_table(), trans))
    write_file(os.path.join(root, 'cmp', 'nolight.cmp'), retail_cmp_bytes(PAL_B, ramp_light_table()))
    write_file(os.path.join(root, 'cmp', 'legacy.cmp'), legacy_cmp_bytes(PAL_C))


def write_all(outdir):
    disc = os.path.join(outdir, 'disc')
    write_disc(disc)
    loose = os.path.join(outdir, 'loose')
    for name, spec in baf_fixtures():
        write_file(os.path.join(loose, 'baf', name), build_baf(spec)[0])
    for name, spec in level_fixtures().items():
        write_file(os.path.join(loose, 'level', name), level_file(name, spec, True)[0])
        write_file(os.path.join(loose, 'level_raw', name), level_file(name, spec, False)[0])
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
