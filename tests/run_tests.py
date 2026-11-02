#!/usr/bin/env python3
# TPMExtractor fork - end-to-end test runner (Python standard library only).
# Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
# Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
"""End-to-end tests of the tpmextractor executable.

Synthetic suite (always runs): builds the fixtures of make_fixtures.py (a synthetic disc, loose
BAF/level/LAB/.Z files, damaged and hostile inputs), runs the tool on them in --legacy and in
default mode, and checks:
  - --legacy writes exactly the files and bytes of an independent model of the original tool
    (make_fixtures.legacy_*), and, given a build of the original (--ref-exe), exactly what the
    original writes (the 16 "tail" lines the original printed from uninitialised memory masked);
  - default mode differs from --legacy only through the documented changes: padded BMP rows,
    retail CMPs (82,752 bytes, light rows that never brighten as light falls, row 63 identity),
    retail 8-bit MATs (<base>8.mat, checked against the source texels), computed RADIUS values
    that cover the scaled model, the INSERT OFFSET option, and the options that switch outputs off;
  - the subcommands (z, lab, b3d, b3dall, baf, bafall) and --keep-decompressed;
  - the level decompressor (b3d_decompress_file, through the unit_b3d_decompress helper) against
    an independent decoder and, given one (--ref-b3d-decompress), a build of the original decoder;
  - damaged inputs are reported and skipped, never crash or hang; names from data never leave the
    output folder;
  - no AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer report in any run.

Real-data suite (only when TPM_DISC is set; it is skipped otherwise): runs the tool on the disc
in TPM_DISC, writing its dumps outside the source tree (TPM_WORK, or a temporary folder) and
deleting them afterwards:
  - --legacy identity against a manifest of the original's output (TPM_REF_MANIFEST: lines
    "path TAB size TAB sha1", with the original's uninitialised "tail[i]=" lines masked as
    "tail[i]=<undefined>"; TPM_REF_UNDEFINED optionally lists the masked files);
  - a default-mode run over the whole disc: every retail CMP, 8-bit MAT and 3DO is checked as
    above, and every file that default mode must not change is compared with the --legacy dump.
  Only counts are printed; no file content is.

Exit status: 0 all selected tests passed (some may be skipped), 77 every selected test was
skipped, 1 a test failed, 2 usage error.

Usage: python3 -I tests/run_tests.py --exe build/<dir>/tpmextractor [options]
       (TPMX_EXE, TPMX_TEST_TMP, TPM_DISC, TPM_REF_MANIFEST, ... are read from the environment)
"""

import argparse
import hashlib
import math
import os
import re
import shutil
import struct
import subprocess
import signal
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True          # no __pycache__ next to the sources
if HERE not in sys.path:
    sys.path.insert(0, HERE)            # python3 -I does not add the script's folder
import make_fixtures as mf              # noqa: E402

EXE_SUFFIX = '.exe' if os.name == 'nt' else ''


# ============================================================================ results


class Fail(Exception):
    pass


class Skip(Exception):
    pass


class Problems:
    """Collects the problems one test finds; raises Fail with all of them at the end."""

    def __init__(self, limit=40):
        self.items = []
        self.raw = []           # shown in full even in a summary (run problems: exit status, sanitizer)
        self.limit = limit
        self.notes = []

    def add(self, msg):
        self.items.append(msg)

    def check(self, cond, msg):
        if not cond:
            self.items.append(msg)
        return cond

    def note(self, msg):
        self.notes.append(msg)

    def extend(self, items, prefix=''):
        for i in items:
            self.items.append(prefix + i)

    def done(self, summary=False):
        if not self.items and not self.raw:
            return
        if summary:
            raise Fail('\n'.join(self.raw + summarize(self.items)))
        self.items = self.raw + self.items
        shown = self.items[:self.limit]
        more = len(self.items) - len(shown)
        raise Fail('\n'.join(shown) + ('\n... and %d more' % more if more > 0 else ''))


def summarize(items, examples=4):
    """Problems found on real data as counts per kind, with the numbers taken out and a few
    example file names: structural facts only, never values derived from game data."""
    groups = {}
    for it in items:
        first = it.split('\n')[0]
        where, sep, what = first.partition(': ')
        if not sep:
            where, what = '', first
        kind = re.sub(r'\[[^\]]*\]|\([^)]*\)', '(...)', what)
        kind = re.sub(r'0x[0-9A-Fa-f]+|-?\d+(\.\d+)?', '#', kind)
        g = groups.setdefault(kind, [0, []])
        g[0] += 1
        if where and len(g[1]) < examples:
            g[1].append(where)
    return ['%d x %s%s' % (c, k, (' (e.g. %s)' % ', '.join(ex)) if ex else '')
            for k, (c, ex) in sorted(groups.items(), key=lambda kv: -kv[1][0])]


TESTS = []


def test(name, suite='synthetic'):
    def deco(fn):
        TESTS.append((name, suite, fn))
        return fn
    return deco


# ============================================================================ running programs

SANITIZER_RE = re.compile(rb'(ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|'
                          rb'UndefinedBehaviorSanitizer|MemorySanitizer|ThreadSanitizer|'
                          rb'SUMMARY: AddressSanitizer)')


# Limits that keep a misbehaving run from filling the disk or the memory: every file the tool
# writes (POSIX: RLIMIT_FSIZE; the tool is killed with SIGXFSZ beyond it), the watched output
# folder as a whole (polled on every platform), and its stdout/stderr (kept: head and tail).
MAX_FILE_SYNTHETIC = 64 << 20
MAX_FILE_REAL = 1 << 30
MAX_STREAM = 64 << 20
KEEP_STREAM = 256 << 10


class _Capture(threading.Thread):
    """Reads a pipe to its end, keeping the first and last KEEP_STREAM bytes."""

    def __init__(self, pipe):
        super().__init__(daemon=True)
        self.pipe, self.head, self.tail, self.total = pipe, bytearray(), bytearray(), 0

    def run(self):
        fd = self.pipe.fileno()
        while True:
            try:
                chunk = os.read(fd, 1 << 16)
            except OSError:
                break
            if not chunk:
                break
            self.total += len(chunk)
            room = KEEP_STREAM - len(self.head)
            if room > 0:
                self.head += chunk[:room]
                chunk = chunk[room:]
            if chunk:
                self.tail += chunk
                if len(self.tail) > KEEP_STREAM:
                    del self.tail[:len(self.tail) - KEEP_STREAM]

    def data(self):
        skipped = self.total - len(self.head) - len(self.tail)
        return bytes(self.head) + (b'\n[... %d bytes not kept ...]\n' % skipped if skipped > 0 else b'') + \
            bytes(self.tail)


def dir_bytes(root):
    total = 0
    for d, _, files in os.walk(root):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(d, f))
            except OSError:
                pass
    return total


class RunResult:
    def __init__(self, cmd, rc, out, err, secs, timed_out, killed_for=None):
        self.cmd, self.rc, self.out, self.err, self.secs, self.timed_out = cmd, rc, out, err, secs, timed_out
        self.killed_for = killed_for

    def lines(self, prefix):
        return [l for l in self.err.decode('latin-1').splitlines() if l.startswith(prefix)]

    def errors(self):
        return self.lines('error:')

    def warnings(self):
        return self.lines('warning:')

    def sanitizer(self):
        m = SANITIZER_RE.search(self.err) or SANITIZER_RE.search(self.out)
        if not m:
            return None
        text = (self.err if SANITIZER_RE.search(self.err) else self.out).decode('latin-1')
        start = max(0, text.find(m.group(0).decode('latin-1')) - 200)
        return text[start:start + 1200]

    def status(self):
        if self.killed_for:
            return 'stopped by the test runner: %s' % self.killed_for
        if self.timed_out:
            return 'timed out after %.0f s' % self.secs
        if self.rc is not None and self.rc < 0:
            try:
                name = signal.Signals(-self.rc).name
            except ValueError:
                name = 'signal %d' % -self.rc
            extra = ' (it tried to write a file over the size limit)' if name == 'SIGXFSZ' else ''
            return 'killed by %s%s' % (name, extra)
        return 'exit %s' % self.rc

    def describe(self):
        return '%s (%s)' % (' '.join(os.path.basename(c) if i == 0 else c for i, c in enumerate(self.cmd)),
                            self.status())


def run(cmd, timeout, watch=None, max_file=MAX_FILE_SYNTHETIC, max_output=None):
    """Runs cmd with stdin closed. watch: a folder whose total size is limited to max_output
    (default 4 x max_file) while it runs."""
    t0 = time.time()
    preexec = None
    if os.name == 'posix' and max_file:
        import resource

        def preexec():
            resource.setrlimit(resource.RLIMIT_FSIZE, (max_file, max_file))
    try:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             preexec_fn=preexec)
    except OSError as e:
        return RunResult(cmd, 127, b'', str(e).encode(), 0.0, False, 'could not start: %s' % e)
    cap_out, cap_err = _Capture(p.stdout), _Capture(p.stderr)
    cap_out.start()
    cap_err.start()
    limit = max_output or 4 * (max_file or MAX_FILE_SYNTHETIC)
    killed_for, timed_out, last_poll = None, False, 0.0
    while True:
        try:
            p.wait(timeout=0.2)
            break
        except subprocess.TimeoutExpired:
            pass
        now = time.time()
        if now - t0 > timeout:
            timed_out = True
        elif cap_out.total + cap_err.total > MAX_STREAM:
            killed_for = 'more than %d MB of console output' % (MAX_STREAM >> 20)
        elif watch and now - last_poll > 1.0:
            last_poll = now
            if os.path.isdir(watch) and dir_bytes(watch) > limit:
                killed_for = 'its output folder grew past %d MB' % (limit >> 20)
        if timed_out or killed_for:
            p.kill()
            p.wait()
            break
    cap_out.join()
    cap_err.join()
    rc = None if (timed_out or killed_for) else p.returncode
    return RunResult(cmd, rc, cap_out.data(), cap_err.data(), time.time() - t0, timed_out, killed_for)


def sound_run(r, pb, allowed_rc=(0,), allow_errors=False, what=''):
    """Common checks for a run: finished, no sanitizer report, an allowed exit status and (unless
    allowed) no error lines. Returns True if all held."""
    label = what or r.describe()
    ok = True
    if r.timed_out or r.killed_for:
        pb.add('%s: %s; stderr: %s' % (label, r.status(), _tail(r.err, 300)))
        return False
    rep = r.sanitizer()
    if rep:
        pb.add('%s: sanitizer report:\n%s' % (label, rep))
        ok = False
    if r.rc not in allowed_rc:
        pb.add('%s: %s, expected exit %s; stderr: %s' % (label, r.status(), '/'.join(map(str, allowed_rc)),
                                                        _tail(r.err)))
        ok = False
    if not allow_errors and r.errors():
        pb.add('%s: error lines on stderr: %s' % (label, ' | '.join(r.errors()[:5])))
        ok = False
    return ok


def _tail(b, n=600):
    s = b.decode('latin-1').strip()
    return s if len(s) <= n else '...' + s[-n:]


# ============================================================================ files and trees


def read_tree(root):
    """{relative path with '/': bytes} for every file under root."""
    out = {}
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in files:
            p = os.path.join(d, f)
            with open(p, 'rb') as fh:
                out[os.path.relpath(p, root).replace(os.sep, '/')] = fh.read()
    return out


def read_dirs(root):
    out = set()
    for d, dirs, _ in os.walk(root):
        for x in dirs:
            out.add(os.path.relpath(os.path.join(d, x), root).replace(os.sep, '/'))
    return out


def sub_tree(tree, prefix):
    n = len(prefix) + 1
    return {k[n:]: v for k, v in tree.items() if k.startswith(prefix + '/')}


def first_difference(a, b):
    if b'\0' in a[:512] or b'\0' in b[:512]:
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y:
                return 'byte 0x%x (%d vs %d), sizes %d/%d' % (i, x, y, len(a), len(b))
        return 'sizes %d/%d' % (len(a), len(b))
    la, lb = a.split(b'\n'), b.split(b'\n')
    for i, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return 'line %d: %r vs %r' % (i + 1, x[:100], y[:100])
    return 'line count %d/%d' % (len(la), len(lb))


def compare_files(actual, expected, label, pb, ignore=()):
    """actual/expected: {name: bytes}. Names in `ignore` (a predicate or a set) are skipped."""
    def skip(n):
        return n in ignore if not callable(ignore) else ignore(n)
    for n in sorted(set(actual) | set(expected)):
        if skip(n):
            continue
        if n not in actual:
            pb.add('%s: missing %s' % (label, n))
        elif n not in expected:
            pb.add('%s: unexpected file %s' % (label, n))
        elif actual[n] != expected[n]:
            pb.add('%s: %s differs from the expected bytes: %s' % (label, n, first_difference(actual[n], expected[n])))


def write(path, data):
    mf.write_file(path, data)
    return path


def fresh_dir(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


def inside(child, parent):
    c, p = os.path.realpath(child), os.path.realpath(parent)
    return c == p or c.startswith(p.rstrip(os.sep) + os.sep)


# ============================================================================ format readers (independent of the tool)

def parse_mat(data):
    """Reads a MAT the way JK's loader does (header, cels with their texture headers, textures
    with their mips, then a 768-byte palette when type & 1 and the bytes are there). Raises
    ValueError on a structural problem."""
    if len(data) < 76 or data[:4] != b'MAT ':
        raise ValueError('no "MAT " header')
    rev, mtype, ncel, ntex = struct.unpack_from('<4i', data, 4)
    fmt = struct.unpack_from('<14I', data, 20)
    if not (0 <= ncel <= 4096 and 0 <= ntex <= 4096):
        raise ValueError('cel/texture counts %d/%d' % (ncel, ntex))
    p = 76
    cels = []
    for _ in range(ncel):
        if p + 24 > len(data):
            raise ValueError('cel header past the end')
        ctype, colour = struct.unpack_from('<2i', data, p)
        floats = struct.unpack_from('<4I', data, p + 8)
        p += 24
        ext = None
        if ctype & 8:
            if p + 16 > len(data):
                raise ValueError('cel texture header past the end')
            ext = struct.unpack_from('<4i', data, p)
            p += 16
        cels.append({'type': ctype, 'colour': colour, 'floats': floats, 'ext': ext})
    bpp = fmt[1]
    textures = []
    for _ in range(ntex):
        if p + 24 > len(data):
            raise ValueError('texture header past the end')
        w, h, alpha, u0, u1, nm = struct.unpack_from('<6i', data, p)
        p += 24
        if not (0 < w <= 4096 and 0 < h <= 4096 and 0 < nm <= 16):
            raise ValueError('texture %dx%d with %d mips' % (w, h, nm))
        mips = []
        for k in range(nm):
            mw, mh = w >> k, h >> k
            size = mw * mh * bpp // 8
            if mw < 1 or mh < 1:
                raise ValueError('mip %d of a %dx%d texture is %dx%d' % (k, w, h, mw, mh))
            if p + size > len(data):
                raise ValueError('mip %d past the end' % k)
            mips.append(data[p:p + size])
            p += size
        textures.append({'w': w, 'h': h, 'alpha': alpha & 0xFFFFFFFF, 'u0': u0, 'u1': u1, 'mips': mips})
    palette = None
    if mtype & 1 and len(data) - p >= 768:
        palette = mf.palette_from_bytes(data[p:p + 768])
        p += 768
    return {'rev': rev, 'type': mtype, 'fmt': fmt, 'cels': cels, 'textures': textures, 'palette': palette,
            'trailing': len(data) - p, 'raw_fill': any(w == 0xCDCDCDCD for w in fmt)}


def luma(c):
    return 299 * c[0] + 587 * c[1] + 114 * c[2]


def parse_cmp(data):
    if len(data) < 64 + 768 + 16384 or data[:4] != b'CMP ':
        raise ValueError('not a CMP with a light table (%d bytes)' % len(data))
    version, flags = struct.unpack_from('<iI', data, 4)
    tint = struct.unpack_from('<3f', data, 12)
    pal = mf.palette_from_bytes(data[64:64 + 768])
    light = data[832:832 + 16384]
    trans = data[832 + 16384:832 + 16384 + 65536] if flags & 1 else None
    return {'version': version, 'flags': flags, 'tint': tint, 'reserved': data[24:64], 'palette': pal,
            'light': light, 'trans': trans, 'size': len(data)}


def check_retail_cmp(data, label, pb, palette=None):
    """A retail JK colormap as default mode writes it."""
    try:
        c = parse_cmp(data)
    except ValueError as e:
        pb.add('%s: %s' % (label, e))
        return None
    pb.check(len(data) == 82752, '%s: %d bytes, expected 82,752' % (label, len(data)))
    pb.check(c['version'] == 30, '%s: version %d, expected 30' % (label, c['version']))
    pb.check(c['flags'] == 1, '%s: flags %d, expected 1 (transparency table)' % (label, c['flags']))
    pb.check(c['tint'] == (1.0, 1.0, 1.0), '%s: tint %r, expected 1.0 x3' % (label, c['tint']))
    pb.check(c['reserved'] == bytes(40), '%s: the 40 reserved header bytes are not zero' % label)
    if palette is not None:
        pb.check(c['palette'] == list(palette), '%s: palette differs from the source palette' % label)
    pal, light = c['palette'], c['light']
    pb.check(light[63 * 256:64 * 256] == bytes(range(256)), '%s: light row 63 is not the identity' % label)
    bad_cols = 0
    zero_results = 0
    for i in range(256):
        prev = None
        for lvl in range(64):
            v = light[lvl * 256 + i]
            if i != 0 and v == 0:
                zero_results += 1
            y = luma(pal[v])
            if prev is not None and y < prev:
                bad_cols += 1
                break
            prev = y
    pb.check(bad_cols == 0, '%s: %d light columns get brighter as the light level falls' % (label, bad_cols))
    pb.check(all(light[lvl * 256] == 0 for lvl in range(64)), '%s: light column 0 (the transparent key) is not 0'
             % label)
    pb.check(zero_results == 0, '%s: %d light entries of opaque indices are the transparent index 0'
             % (label, zero_results))
    t = c['trans']
    if pb.check(t is not None and len(t) == 65536, '%s: no 256x256 transparency table' % label):
        diag = sum(1 for i in range(1, 256) if t[i * 256 + i] != i)
        pb.check(diag == 0, '%s: %d indices do not blend with themselves into themselves' % (label, diag))
    return c


def is_pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def mat8_name(name):
    return mf.mat8_name(name).decode('latin-1')


def check_mat8(data, label, pb, src_textures, dst_pal, src_pals, max_report=4):
    """A retail 8-bit MAT from default mode, against its source record.
    src_textures: [(w, h, transparent, [stored mips])]; dst_pal: the palette its indices refer to
    (None: unknown); src_pals: per texture, the palette of the source indices (None: unknown)."""
    try:
        m = parse_mat(data)
    except ValueError as e:
        pb.add('%s: %s' % (label, e))
        return
    n = len(src_textures)
    pb.check(m['rev'] == 0x32, '%s: revision 0x%x' % (label, m['rev']))
    pb.check(m['type'] == 2, '%s: type %d, expected 2 (textured, no palette)' % (label, m['type']))
    pb.check(m['fmt'][0] == 0 and m['fmt'][1] == 8, '%s: colour mode %d, %d bpp' % (label, m['fmt'][0], m['fmt'][1]))
    pb.check(all(w == 0 for w in m['fmt'][2:]), '%s: format block not clean' % label)
    pb.check(m['trailing'] == 0, '%s: %d trailing bytes' % (label, m['trailing']))
    pb.check(len(m['textures']) == n and len(m['cels']) == n,
             '%s: %d cels/%d textures, expected %d' % (label, len(m['cels']), len(m['textures']), n))
    for i, c in enumerate(m['cels']):
        pb.check(c['type'] == 8 and c['ext'] == (0, 0, 0, i) and c['floats'] == (0x3F800000,) * 4
                 and 0 <= c['colour'] <= 255, '%s: cel %d header %r' % (label, i, c))
    for i, (t, s) in enumerate(zip(m['textures'], src_textures)):
        sw, sh, strans, smips = s
        tl = '%s texture %d' % (label, i)
        masked = strans != 0
        pb.check((t['w'], t['h']) == (sw, sh), '%s: %dx%d, source %dx%d' % (tl, t['w'], t['h'], sw, sh))
        pb.check(is_pow2(t['w']) and is_pow2(t['h']) and t['w'] <= 256 and t['h'] <= 256,
                 '%s: %dx%d is not a power of two up to 256' % (tl, t['w'], t['h']))
        pb.check(t['alpha'] == (1 if masked else 0), '%s: transparency flag %d, source %d'
                 % (tl, t['alpha'], strans))
        pb.check(t['u0'] == 0 and t['u1'] == 0, '%s: unused texture words not zero' % tl)
        pb.check(1 <= len(t['mips']) <= 4, '%s: %d mip levels' % (tl, len(t['mips'])))
        spal = src_pals[i] if src_pals else None
        for k, mip in enumerate(t['mips']):
            if k < len(smips):
                index_rule(smips[k], mip, masked, spal, dst_pal, '%s mip %d' % (tl, k), pb, max_report)
            elif not masked:
                pb.check(0 not in mip, '%s mip %d: an opaque texture uses the transparent index 0' % (tl, k))


def _dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def index_rule(src, dst, masked, src_pal, dst_pal, label, pb, max_report=4):
    """Texel by texel: masked holes (index 0) stay 0; opaque texels never become 0; with the same
    palette on both sides opaque indices are unchanged; otherwise the chosen colour is an exact
    match when one exists and within a factor 2 (squared distance) of the nearest otherwise."""
    if len(src) != len(dst):
        pb.add('%s: %d texels, source %d' % (label, len(dst), len(src)))
        return
    same = src_pal is not None and dst_pal is not None and list(src_pal) == list(dst_pal)
    best = {}
    bad = []
    for k, (s, d) in enumerate(zip(src, dst)):
        why = None
        if masked and s == 0:
            if d != 0:
                why = 'hole became index %d' % d
        elif d == 0:
            why = 'opaque index %d became the transparent index 0' % s
        elif same and s != 0:
            if d != s:
                why = 'index %d changed to %d (same palette)' % (s, d)
        elif src_pal is not None and dst_pal is not None:
            c = src_pal[s]
            if s not in best:
                best[s] = min(_dist2(c, dst_pal[j]) for j in range(1, 256))
            got = _dist2(c, dst_pal[d])
            if best[s] == 0 and got != 0:
                why = 'index %d -> %d: an exact colour match exists' % (s, d)
            elif got > 2 * best[s] + 2:
                why = 'index %d -> %d: squared distance %d, nearest %d' % (s, d, got, best[s])
        if why:
            bad.append('texel %d: %s' % (k, why))
    if bad:
        pb.add('%s: %d bad texels (%s)' % (label, len(bad), '; '.join(bad[:max_report])))


# ---------------------------------------------------------------------------- 3DO text


def _rows(lines, i, n):
    """n non-empty lines from index i: [(index, tokens)], next i."""
    rows = []
    while len(rows) < n and i < len(lines):
        l = lines[i]
        i += 1
        if not l:
            continue
        head, _, rest = l.partition(':')
        try:
            idx = int(head)
        except ValueError:
            raise ValueError('expected an indexed row, got %r' % l[:60])
        rows.append((idx, rest.replace(',', ' ').split()))
    if len(rows) < n:
        raise ValueError('expected %d rows' % n)
    return rows, i


def parse_3do(data):
    """A 3DO as the tool writes it, numbers kept as their text. kind 'key' for the key-format
    file the original's read order writes for a colormap-less BAF. Raises ValueError."""
    try:
        return _parse_3do(data)
    except (ValueError, IndexError, KeyError, TypeError, AttributeError) as e:
        raise ValueError('not a 3DO the tool writes (%s: %s)' % (type(e).__name__, e))


def try_parse_3do(data, label, pb):
    try:
        return parse_3do(data)
    except ValueError as e:
        pb.add('%s: %s' % (label, e))
        return None


def _parse_3do(data):
    text = data.decode('latin-1')
    if text.startswith('# KEYFRAME'):
        return {'kind': 'key'}
    lines = [l.split('#', 1)[0].strip() for l in text.replace('\r\n', '\n').split('\n')]
    m = {'kind': 'model', 'materials': [], 'radius': None, 'insert': None, 'meshes': [], 'nodes': []}
    section, cur, i = None, None, 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if not line:
            continue
        if line.startswith('SECTION:'):
            section = line[8:].strip()
            continue
        tok = line.split()
        if section == 'MODELRESOURCE' and tok[0] == 'MATERIALS':
            rows, i = _rows(lines, i, int(tok[1]))
            m['materials'] = [' '.join(r[1]) for r in rows]
        elif section == 'GEOMETRYDEF':
            if tok[0] == 'RADIUS':
                if cur is None:
                    m['radius'] = tok[1]
                else:
                    cur['radius'] = tok[1]
            elif tok[:2] == ['INSERT', 'OFFSET']:
                m['insert'] = tok[2:]
            elif tok[0] == 'MESH':
                cur = {'index': int(tok[1]), 'radius': None, 'verts': [], 'uvs': [], 'norms': [], 'faces': [],
                       'fnorms': []}
                m['meshes'].append(cur)
            elif tok[0] == 'NAME' and cur is not None:
                cur['name'] = line[4:].strip()
            elif tok[0] in ('GEOMETRYMODE', 'LIGHTINGMODE', 'TEXTUREMODE') and cur is not None:
                cur[tok[0]] = tok[1]
            elif tok[0] == 'VERTICES':
                rows, i = _rows(lines, i, int(tok[1]))
                cur['verts'] = [r[1] for r in rows]
            elif tok[:2] == ['TEXTURE', 'VERTICES']:
                rows, i = _rows(lines, i, int(tok[2]))
                cur['uvs'] = [r[1] for r in rows]
            elif tok[:2] == ['VERTEX', 'NORMALS']:
                rows, i = _rows(lines, i, len(cur['verts']))
                cur['norms'] = [r[1] for r in rows]
            elif tok[0] == 'FACES':
                rows, i = _rows(lines, i, int(tok[1]))
                cur['faces'] = [r[1] for r in rows]
            elif tok[:2] == ['FACE', 'NORMALS']:
                rows, i = _rows(lines, i, len(cur['faces']))
                cur['fnorms'] = [r[1] for r in rows]
        elif section == 'HIERARCHYDEF' and tok[:2] == ['HIERARCHY', 'NODES']:
            rows, i = _rows(lines, i, int(tok[2]))
            for _, r in rows:
                m['nodes'].append({'flags': r[0], 'type': r[1], 'mesh': int(r[2]), 'parent': int(r[3]),
                                   'child': int(r[4]), 'sibling': int(r[5]), 'nchild': int(r[6]),
                                   'pos': r[7:10], 'pyr': r[10:13], 'pivot': r[13:16],
                                   'name': ' '.join(r[16:])})
    return m


def _f(v):
    return [float(x) for x in v]


def build34(pyr, pos):
    """OpenJKDF2 rdMatrix_Build34, term for term (angles in degrees): rows of a 3x4 matrix."""
    sx, cx = math.sin(math.radians(pyr[0])), math.cos(math.radians(pyr[0]))
    sy, cy = math.sin(math.radians(pyr[1])), math.cos(math.radians(pyr[1]))
    sz, cz = math.sin(math.radians(pyr[2])), math.cos(math.radians(pyr[2]))
    r = (-(sz * sy) * sx + cz * cy, (sz * cy) * sx + cz * sy, -sz * cx)
    l = (-sy * cx, cy * cx, sx)
    u = ((cz * sy) * sx + sz * cy, -sx * (cy * cz) + sy * sz, cz * cx)
    return [[r[k], l[k], u[k], pos[k]] for k in range(3)]


def translate(v):
    return [[1.0, 0.0, 0.0, v[0]], [0.0, 1.0, 0.0, v[1]], [0.0, 0.0, 1.0, v[2]]]


def mul34(a, b):
    out = []
    for k in range(3):
        row = [sum(a[k][j] * b[j][c] for j in range(3)) for c in range(3)]
        row.append(sum(a[k][j] * b[j][3] for j in range(3)) + a[k][3])
        out.append(row)
    return out


def apply34(m, v):
    return [m[k][0] * v[0] + m[k][1] * v[1] + m[k][2] * v[2] + m[k][3] for k in range(3)]


IDENTITY34 = translate((0.0, 0.0, 0.0))


def model_space_vertices(m):
    """Every geoset-0 vertex in model space at rest pose, as rdModel3_BuildExpandedRadius
    transforms it: World(node) = World(parent) T(-parent.pivot) T(pos) R(pyr) T(pivot), from node
    0 through child and sibling links."""
    nodes, meshes = m['nodes'], m['meshes']
    out = []
    seen = set()

    def visit(k, world, depth):
        if depth > 256 or k < 0 or k >= len(nodes) or k in seen:
            return
        seen.add(k)
        nd = nodes[k]
        local = mul34(build34(_f(nd['pyr']), _f(nd['pos'])), translate(_f(nd['pivot'])))
        if 0 <= nd['parent'] < len(nodes):
            pp = _f(nodes[nd['parent']]['pivot'])
            local = mul34(translate((-pp[0], -pp[1], -pp[2])), local)
        w = mul34(world, local)
        if 0 <= nd['mesh'] < len(meshes):
            for v in meshes[nd['mesh']]['verts']:
                out.append(apply34(w, _f(v[:3])))
        c = nd['child']
        for _ in range(max(0, nd['nchild'])):
            if not 0 <= c < len(nodes):
                break
            visit(c, w, depth + 1)
            c = nodes[c]['sibling']

    if nodes:
        visit(0, IDENTITY34, 0)
    return out


def parse_key_text(data):
    """A JK .key file as written by the tool: {'frames': n, 'nodes': {node: [entries]}}, each
    entry (frame, flags, pos, pyr, dpos, dpyr) with floats; None if it cannot be read."""
    lines = [l.strip() for l in data.decode('latin-1').split('\r\n')]
    out = {'frames': 0, 'nodes': {}}
    node = None
    i = 0
    try:
        while i < len(lines):
            l = lines[i]
            if l.startswith('FRAMES '):
                out['frames'] = int(l.split()[1])
            elif l.startswith('NODE '):
                node = int(l.split()[1])
                out['nodes'][node] = []
            elif node is not None and re.match(r'^\d+:', l):
                a = l.split()
                d = lines[i + 1].split()
                if len(a) != 9 or len(d) != 6:
                    return None
                out['nodes'][node].append((float(a[1]), int(a[2], 16), _f(a[3:6]), _f(a[6:9]), _f(d[0:3]),
                                           _f(d[3:6])))
                i += 1
            i += 1
    except (ValueError, IndexError):
        return None
    return out


def animated_extent(m, keys):
    """The largest distance from the model origin of any geoset-0 vertex over every integer frame
    of every key, as JK poses the hierarchy: for each node, the last entry at or before the frame,
    plus position/rotation deltas times the frames since that entry (flags 1 and 2)."""
    best = 0.0
    for k in keys:
        for t in range(max(1, k['frames'])):
            posed = dict(m)
            posed['nodes'] = [dict(nd) for nd in m['nodes']]
            for j, entries in k['nodes'].items():
                if not 0 <= j < len(posed['nodes']) or not entries:
                    continue
                e = 0
                while e + 1 < len(entries) and entries[e + 1][0] <= t:
                    e += 1
                frame, flags, pos, pyr, dpos, dpyr = entries[e]
                dt = t - frame
                posed['nodes'][j]['pos'] = tuple(p + (dp * dt if flags & 1 else 0.0) for p, dp in zip(pos, dpos))
                posed['nodes'][j]['pyr'] = tuple(r + (dr * dt if flags & 2 else 0.0) for r, dr in zip(pyr, dpyr))
            for v in model_space_vertices(posed):
                best = max(best, math.sqrt(sum(c * c for c in v)))
    return best


def close(a, b, rel=1e-5, abs_=4e-6):
    return abs(a - b) <= abs_ + rel * max(abs(a), abs(b))


def check_3do_numbers(m, label, pb, radius='computed', insert='floor', scale=0.2, stored_radius=None,
                      node0=None, keys=None, keys_complete=True):
    """RADIUS and INSERT OFFSET of a default-mode 3DO, from its own vertices and hierarchy."""
    verts = model_space_vertices(m)
    maxd = max((math.sqrt(sum(c * c for c in v)) for v in verts), default=0.0)
    if m['radius'] is None or m['insert'] is None or len(m['insert']) != 3:
        pb.add('%s: no model RADIUS or INSERT OFFSET line with three values' % label)
        return
    r = float(m['radius'])
    if radius == 'animated':
        # the model radius covers the rest pose and every integer frame of every key in the BAF
        # (keys: the parsed .key files). When a key was overwritten by a later one of the same name
        # (keys_complete false), its frames still count, so the files give only a lower bound.
        # Mesh radii are computed as usual.
        reach = max(maxd, animated_extent(m, keys or []))
        if keys_complete:
            pb.check(r >= reach - 4e-6 and close(r, 1.1 * reach, abs_=2e-5),
                     '%s: RADIUS %s, expected 1.1 x %.6f = %.6f (rest pose and every key frame)'
                     % (label, m['radius'], reach, 1.1 * reach))
        else:
            pb.check(r >= 1.1 * reach - 2e-5, '%s: RADIUS %s does not cover the written keys: 1.1 x %.6f = %.6f'
                     % (label, m['radius'], reach, 1.1 * reach))
        for me in m['meshes']:
            md = max((math.sqrt(sum(c * c for c in _f(v[:3]))) for v in me['verts']), default=0.0)
            mr = float(me['radius'] or 'nan')
            pb.check(mr >= md - 4e-6 and close(mr, 1.1 * md),
                     '%s mesh %d: RADIUS %s, expected 1.1 x %.6f = %.6f' % (label, me['index'], me['radius'], md,
                                                                          1.1 * md))
    elif radius == 'computed':
        pb.check(r >= maxd - 4e-6, '%s: RADIUS %s does not cover the model (max distance %.6f)'
                 % (label, m['radius'], maxd))
        pb.check(close(r, 1.1 * maxd), '%s: RADIUS %s, expected 1.1 x %.6f = %.6f' % (label, m['radius'], maxd,
                                                                                  1.1 * maxd))
        for me in m['meshes']:
            md = max((math.sqrt(sum(c * c for c in _f(v[:3]))) for v in me['verts']), default=0.0)
            mr = float(me['radius'] or 'nan')
            pb.check(mr >= md - 4e-6 and close(mr, 1.1 * md),
                     '%s mesh %d: RADIUS %s, expected 1.1 x %.6f = %.6f' % (label, me['index'], me['radius'], md,
                                                                          1.1 * md))
    elif radius == 'covers':
        # at least the computed radius: a model --insert-offset auto re-rooted, whose RADIUS also
        # covers its keys (not recomputed here)
        pb.check(r >= 1.1 * maxd - 2e-5, '%s: RADIUS %s, less than 1.1 x %.6f = %.6f' % (label, m['radius'], maxd,
                                                                                       1.1 * maxd))
        for me in m['meshes']:
            md = max((math.sqrt(sum(c * c for c in _f(v[:3]))) for v in me['verts']), default=0.0)
            mr = float(me['radius'] or 'nan')
            pb.check(mr >= md - 4e-6 and close(mr, 1.1 * md),
                     '%s mesh %d: RADIUS %s, expected 1.1 x %.6f = %.6f' % (label, me['index'], me['radius'], md,
                                                                          1.1 * md))
    elif radius == 'stored':
        want = mf.f32(mf.f32(stored_radius) * scale)
        pb.check(close(r, want), '%s: RADIUS %s, expected the stored radius x scale = %.6f' % (label, m['radius'],
                                                                                          want))
    else:
        want = float(radius)
        pb.check(m['radius'] == mf.fmt_f(mf.f32(want)), '%s: RADIUS %s, expected %s' % (label, m['radius'],
                                                                                    mf.fmt_f(mf.f32(want))))
        for me in m['meshes']:
            pb.check(me['radius'] == mf.fmt_f(mf.f32(want)), '%s mesh %d: RADIUS %s, expected %s'
                     % (label, me['index'], me['radius'], mf.fmt_f(mf.f32(want))))
    io = [float(x) for x in m['insert']]
    if insert == 'floor':
        zmin = min((v[2] for v in verts), default=0.0)
        want = (0.0, 0.0, -zmin if verts else 0.0)
        pb.check(all(close(a, b) for a, b in zip(io, want)), '%s: INSERT OFFSET %s, expected the floor (0, 0, %.6f)'
                 % (label, ' '.join(m['insert']), want[2]))
    else:
        if insert == 'node0':
            want = node0
        elif insert == 'zero':
            want = (0.0, 0.0, 0.0)
        elif insert == 'legacy':
            want = (0.0, 0.0, mf.f32(0.1))
        else:
            want = insert
        pb.check(all(close(a, b, abs_=1e-6) for a, b in zip(io, want)), '%s: INSERT OFFSET %s, expected %s'
                 % (label, ' '.join(m['insert']), ' '.join('%.6f' % x for x in want)))


def scaled_same(a, b, fac):
    """Position text a (default mode) against b (--legacy): equal, or (fac given) a = b x fac up
    to the 6-decimal rounding of both."""
    if a == b:
        return True
    if fac is None or len(a) != len(b):
        return False
    try:
        return all(abs(float(x) - float(y) * fac) <= 2.5e-6 + 1e-6 * abs(float(x)) for x, y in zip(a, b))
    except ValueError:
        return False


def compare_key(d, l, label, pb, fac=None, node0_dz=None):
    """Default-mode .key text against --legacy: identical, except that with a draw scale (fac)
    the positions and position deltas (columns 4-6 of the entry and delta rows) scale by fac, and
    that for a model re-rooted at node 0 (node0_dz: how far node 0 was lowered, in default-mode
    units) node 0's entry z positions are that much lower."""
    if d == l:
        return
    dl, ll = d.split(b'\r\n'), l.split(b'\r\n')
    if not pb.check(len(dl) == len(ll), '%s: %d lines, --legacy %d' % (label, len(dl), len(ll))):
        return
    bad = 0
    node = None
    for x, y in zip(dl, ll):
        if y.startswith(b'NODE ') and y[5:].isdigit():
            node = int(y[5:])
        if x == y:
            continue
        if x.startswith(b'JOINTS ') and y.startswith(b'JOINTS ') and x[7:].isdigit() and y[7:].isdigit():
            # raised to the model's hierarchy node count (key_joints_cover); checked against the
            # model in real.default_smoke
            if int(x[7:]) < int(y[7:]):
                bad += 1
            continue
        tx, ty = x.decode('latin-1').split('\t'), y.decode('latin-1').split('\t')
        if node0_dz is not None and node == 0 and len(tx) == 9 and len(ty) == 9 and ty[0].endswith(':'):
            try:
                want = float(ty[5]) * (fac or 1.0) - node0_dz
                z_ok = abs(float(tx[5]) - want) <= 3.5e-6 + 1e-6 * abs(want)
            except ValueError:
                z_ok = False
            if tx[:3] != ty[:3] or tx[6:] != ty[6:] or not scaled_same(tx[3:5], ty[3:5], fac) or not z_ok:
                bad += 1
            continue
        if len(tx) != 9 or len(ty) != 9 or tx[:3] != ty[:3] or tx[6:] != ty[6:] or not scaled_same(tx[3:6], ty[3:6], fac):
            bad += 1
    pb.check(bad == 0, '%s: %d lines differ from --legacy beyond the draw scale' % (label, bad))


def compare_3do(dflt, legacy, label, pb, materials, node_types=None, fac=None):
    """Default-mode 3DO against the --legacy one: the geometry, hierarchy and numbers must be the
    same text; only the documented fields may change. materials: the expected MATERIALS names.
    node_types: allowed hierarchy types per node besides the legacy 0x1 (None: any). fac: the
    draw scale default mode applies to positions (None: positions must be the same text)."""
    pb.check(dflt['materials'] == materials, '%s: MATERIALS %r, expected %r' % (label, dflt['materials'], materials))
    if not pb.check(len(dflt['meshes']) == len(legacy['meshes']), '%s: %d meshes, --legacy has %d'
                    % (label, len(dflt['meshes']), len(legacy['meshes']))):
        return
    for a, b in zip(dflt['meshes'], legacy['meshes']):
        ml = '%s mesh %d' % (label, b['index'])
        for key in ('name', 'GEOMETRYMODE', 'LIGHTINGMODE'):
            pb.check(a.get(key) == b.get(key), '%s: %s %r, --legacy %r' % (ml, key, a.get(key), b.get(key)))
        pb.check(a.get('TEXTUREMODE') in (b.get('TEXTUREMODE'), '3'), '%s: TEXTUREMODE %r' % (ml, a.get('TEXTUREMODE')))
        pb.check(len(a['verts']) == len(b['verts']) and all(scaled_same(x[:3], y[:3], fac)
                                                             for x, y in zip(a['verts'], b['verts'])),
                 '%s: vertices differ' % ml)
        pb.check(all(len(v) == 4 and float(v[3]) == 0.0 for v in a['verts']), '%s: vertex intensity not 0' % ml)
        pb.check(a['uvs'] == b['uvs'], '%s: texture vertices differ' % ml)
        pb.check(a['norms'] == b['norms'], '%s: vertex normals differ' % ml)
        pb.check(a['fnorms'] == b['fnorms'], '%s: face normals differ' % ml)
        if not pb.check(len(a['faces']) == len(b['faces']), '%s: face count differs' % ml):
            continue
        for k, (fa, fb) in enumerate(zip(a['faces'], b['faces'])):
            fl = '%s face %d' % (ml, k)
            try:
                ta, tb = int(fa[1], 16), int(fb[1], 16)
            except (ValueError, IndexError):
                pb.add('%s: bad face row' % fl)
                continue
            ok = (fa[0] == fb[0] and ta in (tb, tb & 3) and fa[2:4] == fb[2:4] and fa[4] in (fb[4], '3')
                  and float(fa[5]) == 0.0 and fa[6:] == fb[6:])
            pb.check(ok, '%s: %s, --legacy %s' % (fl, ' '.join(fa), ' '.join(fb)))
    if not pb.check(len(dflt['nodes']) == len(legacy['nodes']), '%s: %d hierarchy nodes, --legacy has %d'
                    % (label, len(dflt['nodes']), len(legacy['nodes']))):
        return
    for k, (a, b) in enumerate(zip(dflt['nodes'], legacy['nodes'])):
        nl = '%s node %d' % (label, k)
        pb.check(int(a['flags'], 16) == 0, '%s: flags %s' % (nl, a['flags']))
        if node_types is not None:
            pb.check(int(a['type'], 16) in (int(b['type'], 16), node_types[k]), '%s: type %s' % (nl, a['type']))
        for key in ('mesh', 'parent', 'child', 'sibling', 'nchild', 'pyr', 'name'):
            pb.check(a[key] == b[key], '%s: %s %r, --legacy %r' % (nl, key, a[key], b[key]))
        for key in ('pos', 'pivot'):
            pb.check(scaled_same(a[key], b[key], fac), '%s: %s %r, --legacy %r' % (nl, key, a[key], b[key]))


# ============================================================================ context


class Ctx:
    def __init__(self, args):
        self.args = args
        self.exe = args.exe
        self.timeout = args.timeout
        self.damaged_timeout = max(10.0, args.timeout / 8)
        self.work = args.work
        self.cache = {}
        self.verbose = args.verbose

    def log(self, msg):
        if self.verbose:
            print('    ' + msg)

    def tool(self, *argv, timeout=None, exe=None, real=False):
        """Runs the tool; its last argument (the output folder) is watched."""
        argv = [str(a) for a in argv]
        watch = argv[-1] if argv and not argv[-1].startswith('-') else None
        r = run([exe or self.exe] + argv, timeout or self.timeout, watch=None if real else watch,
                max_file=MAX_FILE_REAL if real else MAX_FILE_SYNTHETIC)
        self.log('%s  [%.2f s]' % (r.describe(), r.secs))
        return r

    def path(self, *parts):
        return os.path.join(self.work, *parts)

    def memo(self, key, fn):
        if key not in self.cache:
            self.cache[key] = fn()
        return self.cache[key]


# ============================================================================ synthetic fixtures and their expected output

BAF_SPECS = dict(mf.baf_fixtures())                 # file name -> spec
LEVEL_SPECS = mf.level_fixtures()


def loose(ctx):
    """Loose fixture files: {'baf': dir, 'level': dir, 'level_raw': dir, 'lab', 'z', 'palette'}."""
    def make():
        root = fresh_dir(ctx.path('loose'))
        d = {}
        for sub in ('baf', 'level', 'level_raw', 'lab', 'z', 'palette', 'lzss'):
            d[sub] = os.path.join(root, sub)
            os.makedirs(d[sub])
        for name, spec in BAF_SPECS.items():
            write(os.path.join(d['baf'], name), mf.build_baf(spec)[0])
        for name, spec in LEVEL_SPECS.items():
            write(os.path.join(d['level'], name), mf.level_file(name, spec, True)[0])
            write(os.path.join(d['level_raw'], name), mf.level_file(name, spec, False)[0])
        write(os.path.join(d['lab'], 'scrambled.lab'), mf.make_lab(mf.localize_lab_entries(), 'scrambled'))
        write(os.path.join(d['lab'], 'sequential.lab'), mf.make_lab(mf.localize_lab_entries()))
        write(os.path.join(d['z'], 'multi.z'), mf.make_is3(mf.multi_z_members()))
        write(os.path.join(d['palette'], 'perm.pal'), mf.palette_bytes(mf.PAL_PERM))
        write(os.path.join(d['palette'], 'target.cmp'), mf.legacy_cmp_bytes(mf.PAL_D))
        return d
    return ctx.memo('loose', make)


def legacy_level_names(ctx):
    """The level files --legacy processes (the original's fixed list), found from the tool's own
    "not found" warnings on a disc without levels, so that the tests never spell them."""
    def find():
        root = fresh_dir(ctx.path('discover'))
        mf.write_disc(os.path.join(root, 'disc'), levels=[])
        r = ctx.tool('--legacy', '--no-pause', os.path.join(root, 'disc'), os.path.join(root, 'dump'))
        names = []
        for l in r.warnings():
            m = re.search(r'[\\/]([A-Za-z0-9_]+\.[Bb]3[Dd]): not found', l)
            if m and m.group(1) not in names:
                names.append(m.group(1))
        shutil.rmtree(root, ignore_errors=True)
        return names
    return ctx.memo('legacy_names', find)


def disc(ctx):
    """The synthetic disc: the --legacy level list (cycling through the level fixtures), the
    fixtures under generic names, and one uncompressed level with a lower-case name."""
    def make():
        root = fresh_dir(ctx.path('disc'))
        cycle = ['BOX.B3D', 'FLAT.B3D', 'DECOY.B3D']
        levels = [(n, LEVEL_SPECS[cycle[i % 3]]) for i, n in enumerate(legacy_level_names(ctx))]
        levels += [(n, LEVEL_SPECS[n]) for n in cycle if n not in [l[0] for l in levels]]
        levels.append(('raw.b3d', LEVEL_SPECS['FLAT.B3D'], False))
        info = mf.write_disc(root, levels=levels)
        info['root'] = root
        info['legacy_levels'] = legacy_level_names(ctx)
        return info
    return ctx.memo('disc', make)


def expected_lab_dirs(info):
    exp = {}
    for n, d in mf.lab_outputs(info['big_lab'], True):
        exp['biglab/' + n] = d
    for n, d in mf.lab_outputs(info['localize'], True):
        exp['localizelab/' + n] = d
    if info['voice']:
        for n, d in mf.lab_outputs(info['voice'], True):
            exp['voicelab/' + n] = d
    return exp


def baf_members(info):
    """[(file name, bytes)] of the *.baf entries of big.lab (what the BAF stage processes)."""
    return [(n, d) for n, d in mf.lab_outputs(info['big_lab'], True) if n.lower().endswith('.baf')]


def legacy_baf_files(data, **kw):
    """{output name: bytes} the original writes for one BAF (NTFS overwrite semantics)."""
    return {v[0].decode('latin-1'): v[1] for v in mf.resolve_writes(mf.legacy_baf_outputs(data, **kw)).values()}


def legacy_undefined(data):
    return {v[0].decode('latin-1') for v in mf.resolve_writes(mf.legacy_baf_outputs(data)).values() if v[2]}


def expected_disc_legacy(info):
    exp = expected_lab_dirs(info)
    dirs = {'biglab', 'localizelab', 'voicelab', 'LEVEL', 'BAF'}
    for fname in info['legacy_levels']:
        name = fname.split('.')[0]
        dirs.add('LEVEL/' + name)
        for bn, b in (mf.legacy_b3d_outputs(mf.dec_of(info['levels'][fname])) or []):
            exp['LEVEL/%s/%s' % (name, bn)] = b
    undefined = set()
    for bname, data in baf_members(info):
        stem = bname.split('.')[0]
        dirs.add('BAF/' + stem)
        for n, b in legacy_baf_files(data).items():
            exp['BAF/%s/%s' % (stem, n)] = b
        undefined |= {'BAF/%s/%s' % (stem, n) for n in legacy_undefined(data)}
    return exp, dirs, undefined


def level_image_for_tool(info, fname):
    data, image = info['level_files'][fname]
    return mf.dec_of(image) if data != image else image


# ---------------------------------------------------------------------------- default-mode BAF rules


# Default mode multiplies every position by the BAF's own draw scale (header +0xAC), as the game
# draws actors: effective scale = --scale x draw scale. --insert-offset node0 re-roots the model:
# node 0's z moves from the hierarchy (and node 0's key positions) into the insert offset.
DRAW_SCALE_DEFAULT = True


def draw_scale_steps(spec, draw_scale=DRAW_SCALE_DEFAULT):
    """The draw scale default mode applies, in hundredths (100: 1.0, or --no-draw-scale): the
    stored value rounded, so that 1.00001 (as many retail BAFs store 1.0) is 1.0."""
    if not draw_scale:
        return 100
    return int(math.floor(mf.f32(spec.get('fac', 1.0)) * 100.0 + 0.5))


def effective_scale(spec, scale, draw_scale=DRAW_SCALE_DEFAULT):
    return mf.f32(mf.f32(scale) * mf.f32(draw_scale_steps(spec, draw_scale) / 100.0))


def scaled_name(name, steps):
    """A 3DO or KEY name as default mode writes it at a draw scale other than 1: "_s<hundredths>"
    before the extension, since JK holds one model or key per name and other BAFs may share the
    name at another scale. Debug dumps keep the BAF's own names."""
    if steps == 100:
        return name
    stem, dot, ext = name.rpartition('.')
    return stem + '_s%03d' % steps + dot + ext if dot else name + '_s%03d' % steps


def auto_reroot(spec):
    """--insert-offset auto (the default): the height a fixture's model is re-rooted by, node 0's
    z rounded to 1/1000 (baf.cpp kAutoRerootSteps), or 0.0 when node 0 does not lie above z = 0.
    The INSERT OFFSET is then the floor of the re-rooted model."""
    z = mf.f32(spec['model']['nodes'][0]['pos'][2]) if spec['model']['nodes'] else 0.0
    if not z > 0 or math.isinf(z):
        return 0.0
    return mf.f32(round(z * 1000.0) / 1000.0)


class BafOpts:
    def __init__(self, mat8=True, mat16=True, bmp=True, debug=True, scale=0.2, angles='raw', radius='computed',
                 insert='auto', palette=None, draw_scale=DRAW_SCALE_DEFAULT):
        self.mat8, self.mat16, self.bmp, self.debug = mat8, mat16, bmp, debug
        self.scale, self.angles, self.radius, self.insert = scale, angles, radius, insert
        self.draw_scale = draw_scale    # --[no-]draw-scale
        self.palette = palette          # the --palette target as a palette list, or None


def mat_textures(mat_spec):
    return [(t['w'], t['h'], t['transparent'], t['mips']) for t in mat_spec['textures']]


KEY_JOINTS = re.compile(rb'\r\nJOINTS (\d+)\r\n')


def key_joints_cover(text, nodes):
    """A .key text with JOINTS raised to the model's hierarchy node count: JK allocates JOINTS
    joints for a key and its animation code reads the one at every hierarchy node's index (the
    retail engine without a bounds check), so a key that animates fewer nodes must still declare
    them all. NODES stays the number of joints written."""
    m = KEY_JOINTS.search(text)
    if not m or int(m.group(1)) >= nodes:
        return text
    return text[:m.start(1)] + b'%d' % nodes + text[m.end(1):]


def check_baf_default(files, spec, data, o, label, pb):
    """The output of one BAF in default mode (files: {name: bytes}), against the fixture spec and
    the original's output for the same options."""
    has_cmap = bool(spec.get('colormap'))
    fallback = o.palette if o.palette is not None else None
    leg = legacy_baf_files(data, scale=o.scale, angles=o.angles, fallback_palette=fallback, padded_bmp=True,
                           mat16=o.mat16, bmp=o.bmp, debug_dumps=o.debug)
    # the model and keys: the original's output for the same BAF at the effective scale, re-rooted
    # for node0, under names that carry a draw scale other than 1
    scale = effective_scale(spec, o.scale, o.draw_scale)
    steps = draw_scale_steps(spec, o.draw_scale)
    reroot = auto_reroot(spec) if o.insert == 'auto' else 0.0
    insert = 'floor' if o.insert == 'auto' else o.insert
    mspec = mf.rerooted(spec) if o.insert == 'node0' else mf.rerooted(spec, reroot) if reroot > 0 else spec
    model_ref = legacy_baf_files(mf.build_baf(mspec)[0], scale=scale, angles=o.angles, fallback_palette=fallback,
                                 padded_bmp=True, mat16=o.mat16, bmp=o.bmp, debug_dumps=o.debug)
    variant = 'legacy read order'
    if not has_cmap:
        # The original's read order skips the model of a colormap-less BAF; a parser that reads
        # has_colormap writes it. Both are accepted: the model files must then match the same
        # BAF with a colormap block added, read by the original's order.
        cm = legacy_baf_files(mf.build_baf(mf.with_colormap(mspec))[0], scale=scale, angles=o.angles,
                              padded_bmp=True, mat16=o.mat16, bmp=o.bmp, debug_dumps=o.debug)
        model_name = spec['model']['name']
        out_name = scaled_name(model_name, steps)
        got = try_parse_3do(files[out_name], label, Problems()) if out_name in files else None
        if model_name in cm and got is not None and got['kind'] == 'model':
            model_ref = cm
            variant = 'has_colormap read'
    if not has_cmap:
        pb.note('%s: colormap-less BAF written by the %s' % (label, variant))
    seen = set()
    cmp_name = spec['colormap']['name'] if has_cmap else None
    # the palette every 8-bit MAT indexes: --palette, else the colormap, else (no colormap) the
    # first material's own palette, else the grayscale ramp
    own0 = next((m['palette'] for m in spec['materials'] if m['type'] & 1), None)
    if o.palette is not None:
        mat8_pal = o.palette
    elif has_cmap:
        mat8_pal = spec['colormap']['palette']
    else:
        mat8_pal = own0 or mf.grayscale_palette()
    # colormap
    if has_cmap:
        seen.add(cmp_name)
        if pb.check(cmp_name in files, '%s: no colormap %s' % (label, cmp_name)):
            check_retail_cmp(files[cmp_name], '%s/%s' % (label, cmp_name), pb, spec['colormap']['palette'])
    else:
        # a colormap-less BAF gets <BAF name>.cmp with the palette its 8-bit MATs index (none
        # with --palette, whose palette they index instead, or without 8-bit MATs)
        cmps = [n for n in files if n.lower().endswith('.cmp')]
        own_cmp = spec['file'].split('.')[0] + '.cmp'
        if o.mat8 and o.palette is None:
            seen.add(own_cmp)
            if pb.check(cmps == [own_cmp], '%s: colormap-less BAF wrote colormaps %s, expected %s'
                        % (label, cmps, own_cmp)):
                check_retail_cmp(files[own_cmp], '%s/%s' % (label, own_cmp), pb, mat8_pal)
        else:
            pb.check(not cmps, '%s: colormap-less BAF wrote %s' % (label, cmps))
    # materials
    base_pal = spec['colormap']['palette'] if has_cmap else (o.palette or mf.grayscale_palette())
    for mspec in spec['materials']:
        name = mspec['name']
        raw = name
        b16 = mf._mat16_name(name.encode()).decode()
        bmpn = mf._bmp_name(name.encode()).decode()
        m8 = mat8_name(name)
        seen |= {raw, b16, bmpn, m8}
        if pb.check(raw in files, '%s: no raw MAT copy %s' % (label, raw)):
            # the original's copy followed by the palette its type announces (JK's loader reads
            # those 768 bytes after the last mip; the original left them out)
            want = leg.get(raw, b'') + (mf.palette_bytes(mspec['palette']) if mspec['type'] & 1 else b'')
            pb.check(files[raw] == want, '%s: raw MAT copy %s is not the original\'s copy plus the own palette: %s'
                     % (label, raw, first_difference(files[raw], want)))
        for n, on in ((b16, o.mat16), (bmpn, o.bmp)):
            if on:
                if pb.check(n in files, '%s: missing %s' % (label, n)):
                    pb.check(files[n] == leg.get(n), '%s: %s differs from the expected bytes: %s'
                             % (label, n, first_difference(files[n], leg.get(n, b''))))
            else:
                pb.check(n not in files, '%s: %s written although switched off' % (label, n))
        if not o.mat8:
            pb.check(m8 not in files, '%s: %s written with --no-mat8' % (label, m8))
            continue
        if not pb.check(m8 in files, '%s: no 8-bit MAT %s' % (label, m8)):
            continue
        own = mspec['palette'] if mspec['type'] & 1 else base_pal
        check_mat8(files[m8], '%s/%s' % (label, m8), pb, mat_textures(mspec), mat8_pal,
                   [own] * len(mspec['textures']))
    # model and keys
    names3do = [n for n in model_ref if n.endswith('.3do')]
    for ln in names3do:
        n = scaled_name(ln, steps)
        seen.add(n)
        if not pb.check(n in files, '%s: no %s' % (label, n)):
            continue
        d, l = try_parse_3do(files[n], '%s/%s' % (label, n), pb), parse_3do(model_ref[ln])
        if d is None:
            continue
        if l['kind'] != 'model':
            pb.check(files[n] == model_ref[ln], '%s: %s (key format, as the original) differs' % (label, n))
            continue
        if not pb.check(d['kind'] == 'model', '%s: %s is not a model-format 3DO' % (label, n)):
            continue
        names = [(mat8_name(m['name']) if o.mat8 else mf._mat16_name(m['name'].encode()).decode())
                 for m in spec['materials']]
        compare_3do(d, l, '%s/%s' % (label, n), pb, names, [nd.get('type', 1) for nd in spec['model']['nodes']])
        node0 = (0.0, 0.0, mf.f32(mf.f32(spec['model']['nodes'][0]['pos'][2]) * scale))
        keys = [parse_key_text(files[k]) for k in sorted(files) if k.endswith('.key')]
        key_names = [k['name'].lower() for k in spec.get('keys', [])]
        # a model auto re-roots gets node0's radius: with --radius computed it covers the keys too
        radius = 'animated' if reroot > 0 and o.radius == 'computed' else o.radius
        check_3do_numbers(d, '%s/%s' % (label, n), pb, radius, insert, scale, spec['model']['radius'], node0,
                          [k for k in keys if k], keys_complete=len(set(key_names)) == len(key_names))
        if reroot > 0 and spec['file'] == 'stand.baf':
            # stands on its origin: re-rooted, it hangs node 0's height below its origin, which is
            # where JK keeps the origin of a standing thing (template size= at most that - 0.005)
            pb.check(close(float(d['insert'][2]), mf.f32(reroot * scale), abs_=2e-6),
                     '%s/%s: INSERT OFFSET z %s, expected node 0\'s height %.6f: it would not stand on the floor in JK'
                     % (label, n, d['insert'][2], mf.f32(reroot * scale)))
        if o.mat8 or o.mat16:       # with neither, the 3DO names <base>16.mat files that are not written
            for mat in d['materials']:
                pb.check(mat in files, '%s/%s: MATERIALS names %s, which was not written' % (label, n, mat))
    for ln in [n for n in model_ref if n.endswith('.key')]:
        n = scaled_name(ln, steps)
        seen.add(n)
        if pb.check(n in files, '%s: no %s' % (label, n)):
            want = key_joints_cover(model_ref[ln], len(spec['model']['nodes']))
            pb.check(files[n] == want, '%s: %s differs from the expected bytes: %s'
                     % (label, n, first_difference(files[n], want)))
    # debug dumps: present exactly when switched on (content is not part of the contract)
    debug = [n for n in files if n.endswith('.3do.txt') or n.endswith('.key.txt') or n == 'REMAIN.HEX']
    if o.debug:
        for n in model_ref:
            if n.endswith('.3do.txt') or n.endswith('.key.txt'):
                pb.check(n in files, '%s: no debug dump %s' % (label, n))
    else:
        pb.check(not debug, '%s: debug dumps written with --no-debug-dumps: %s' % (label, debug))
    seen |= set(debug)
    extra = sorted(set(files) - seen)
    pb.check(not extra, '%s: unexpected files %s' % (label, extra))


def check_baf_dirs_default(ctx, out_root, o, pb, specs=None):
    specs = specs or BAF_SPECS
    tree = read_tree(out_root)
    for bname, spec in sorted(specs.items()):
        stem = bname.split('.')[0]
        files = sub_tree(tree, stem)
        if not pb.check(files, '%s: no output folder' % stem):
            continue
        check_baf_default(files, spec, mf.build_baf(spec)[0], o, stem, pb)


# ============================================================================ synthetic tests


@test('fixtures.self_test')
def t_fixtures(ctx):
    if mf.self_test() != 0:
        raise Fail('make_fixtures.py --self-test reported problems (see above)')


@test('cli.usage')
def t_cli(ctx):
    pb = Problems()
    r = ctx.tool()
    sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='no arguments')
    r = ctx.tool('--help')
    sound_run(r, pb, what='--help')
    pb.check(b'Usage' in r.out or b'usage' in r.out, '--help prints no usage text')
    r = ctx.tool('--legacy', '--scale', '0.3', ctx.path('none'), ctx.path('none2'))
    sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='--legacy with --scale')
    pb.check(not os.path.exists(ctx.path('none2')), '--legacy with --scale still created the dump folder')
    for opt in (['--no-draw-scale'], ['--radius', 'animated']):
        r = ctx.tool('--legacy', *(opt + [ctx.path('none'), ctx.path('none2')]))
        sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='--legacy with %s' % ' '.join(opt))
        pb.check(not os.path.exists(ctx.path('none2')), '--legacy with %s still created the dump folder'
                 % ' '.join(opt))
    r = ctx.tool('--no-such-option', ctx.path('none'), ctx.path('none2'))
    sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='unknown option')
    r = ctx.tool('--no-pause', ctx.path('no_disc_here'), ctx.path('dump_nodisc'))
    sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='missing disc')
    pb.done()


def disc_dump(ctx, mode):
    """(dump folder, run) of the synthetic disc in 'legacy' or 'default' mode (run once)."""
    def make():
        info = disc(ctx)
        out = ctx.path('dump_' + mode)
        shutil.rmtree(out, ignore_errors=True)
        r = ctx.tool(*((['--legacy'] if mode == 'legacy' else []) + ['--no-pause', info['root'], out]))
        return out, r
    return ctx.memo('dump_' + mode, make)


@test('disc.legacy')
def t_disc_legacy(ctx):
    pb = Problems()
    info = disc(ctx)
    if not info['legacy_levels']:
        ctx.log('no legacy level list found in the tool\'s warnings; the disc run has no levels')
    out, r = disc_dump(ctx, 'legacy')
    sound_run(r, pb)
    exp, dirs, _ = expected_disc_legacy(info)
    compare_files(read_tree(out), exp, '--legacy', pb)
    got_dirs = read_dirs(out)
    pb.check(got_dirs == dirs, '--legacy: folders differ: missing %s, extra %s'
             % (sorted(dirs - got_dirs), sorted(got_dirs - dirs)))
    pb.done()


@test('disc.legacy_vs_original')
def t_disc_original(ctx):
    if not ctx.args.ref_exe:
        raise Skip('no build of the original tool (--ref-exe / TPMX_REF_EXE)')
    info = disc(ctx)
    if len(info['legacy_levels']) == 0:
        raise Skip('the legacy level list is unknown; the original would stop at the first missing level')
    pb = Problems()
    out = ctx.path('dump_original')
    shutil.rmtree(out, ignore_errors=True)
    r = run([ctx.args.ref_exe, info['root'], out], ctx.timeout, watch=out)
    pb.check(not r.timed_out and r.rc == 0, 'the original tool failed: %s' % r.describe())
    legacy_out, _ = disc_dump(ctx, 'legacy')
    _, _, undefined = expected_disc_legacy(info)
    tail = re.compile(rb'^(tail\[\d+\]=)[^\r\n]*', re.M)
    a, b = read_tree(legacy_out), read_tree(out)
    for k in undefined:
        if k in a:
            a[k] = tail.sub(rb'\1*', a[k])
        if k in b:
            b[k] = tail.sub(rb'\1*', b[k])
    compare_files(a, b, '--legacy vs the original', pb)
    pb.check(read_dirs(legacy_out) == read_dirs(out), '--legacy vs the original: folders differ')
    pb.done()


@test('disc.default')
def t_disc_default(ctx):
    pb = Problems()
    info = disc(ctx)
    out, r = disc_dump(ctx, 'default')
    sound_run(r, pb)
    tree = read_tree(out)
    exp = expected_lab_dirs(info)
    compare_files({k: v for k, v in tree.items() if k.split('/')[0] in ('biglab', 'localizelab', 'voicelab')},
                  exp, 'default mode', pb)
    level_exp = {}
    for fname in info['levels']:
        name = fname.split('.')[0].upper()
        for bn, b in mf.level_bmps(level_image_for_tool(info, fname), True):
            level_exp['LEVEL/%s/%s' % (name, bn)] = b
    compare_files({k: v for k, v in tree.items() if k.startswith('LEVEL/')}, level_exp, 'default mode', pb)
    stems = {b.split('.')[0]: b for b, _ in baf_members(info)}
    pb.check({k.split('/')[1] for k in tree if k.startswith('BAF/')} == set(stems),
             'default mode: BAF folders %s, expected %s' % (sorted({k.split('/')[1] for k in tree
                                                                     if k.startswith('BAF/')}), sorted(stems)))
    check_baf_dirs_default(ctx, os.path.join(out, 'BAF'), BafOpts(), pb)
    for n in pb.notes:
        ctx.log(n)
    pb.done()


def lowercase_copy(src, dst):
    for d, dirs, files in os.walk(src):
        rel = os.path.relpath(d, src)
        target = dst if rel == '.' else os.path.join(dst, rel.lower())
        os.makedirs(target, exist_ok=True)
        for f in files:
            shutil.copyfile(os.path.join(d, f), os.path.join(target, f.lower()))


@test('disc.lowercase_copy')
def t_lowercase(ctx):
    """Input files are found ignoring case: a lower-case copy of the disc gives the same dump."""
    pb = Problems()
    info = disc(ctx)
    low = ctx.path('disc_lower')
    shutil.rmtree(low, ignore_errors=True)
    lowercase_copy(info['root'], low)
    for mode in ('legacy', 'default'):
        ref_out, _ = disc_dump(ctx, mode)
        out = ctx.path('dump_lower_' + mode)
        shutil.rmtree(out, ignore_errors=True)
        r = ctx.tool(*((['--legacy'] if mode == 'legacy' else []) + ['--no-pause', low, out]))
        sound_run(r, pb, what='%s run on a lower-case copy' % mode)
        compare_files(read_tree(out), read_tree(ref_out), '%s, lower-case copy' % mode, pb)
    pb.done()


@test('disc.dump_folder')
def t_dump_folder(ctx):
    """--legacy creates only the last level of the dump folder (the original's CreateDirectory);
    default mode creates its parents."""
    pb = Problems()
    info = disc(ctx)
    base = fresh_dir(ctx.path('dumpdir'))
    r = ctx.tool('--legacy', '--no-pause', info['root'], os.path.join(base, 'a', 'b'))
    sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='--legacy with a missing parent folder')
    pb.check(os.listdir(base) == [], '--legacy created folders although the parent was missing')
    r = ctx.tool('--no-pause', '-q', info['root'], os.path.join(base, 'c', 'd'))
    sound_run(r, pb, allow_errors=True, allowed_rc=(0, 1), what='default mode with a missing parent folder')
    pb.check(os.path.isdir(os.path.join(base, 'c', 'd', 'biglab')), 'default mode did not create the parents')
    pb.done()


@test('disc.keep_decompressed')
def t_keep(ctx):
    pb = Problems()
    info = disc(ctx)
    out = ctx.path('dump_keep')
    shutil.rmtree(out, ignore_errors=True)
    r = ctx.tool('--no-pause', '--keep-decompressed', info['root'], out)
    sound_run(r, pb)
    big = os.path.join(out, 'big.lab')
    if pb.check(os.path.isfile(big), 'big.lab was not kept'):
        pb.check(open(big, 'rb').read() == info['big_lab'], 'the kept big.lab differs from the archived one')
    for fname in info['levels']:
        name = fname.split('.')[0].upper()
        dec = os.path.join(out, 'LEVEL', name, name + '.dec')
        if pb.check(os.path.isfile(dec), '%s: no %s.dec' % (fname, name)):
            pb.check(open(dec, 'rb').read() == level_image_for_tool(info, fname),
                     '%s: %s.dec differs from the decompressed image' % (fname, name))
    shutil.rmtree(out, ignore_errors=True)
    pb.done()


def bafall(ctx, tag, *opts):
    d = loose(ctx)
    out = ctx.path('bafall_' + tag)
    shutil.rmtree(out, ignore_errors=True)
    r = ctx.tool('--no-pause', *(list(opts) + ['bafall', d['baf'], out]))
    return r, out


def option_test(tag, cli, o, allowed_rc=(0,), expect_stderr=None):
    def fn(ctx):
        pb = Problems()
        r, out = bafall(ctx, tag, *cli)
        if r.rc == 2 and b'usage' in r.err.lower() + r.out.lower():
            pb.add('%s: the option was rejected: %s' % (' '.join(cli), _tail(r.err)))
            pb.done()
        sound_run(r, pb, allowed_rc=allowed_rc)
        if expect_stderr is not None:
            pb.check(expect_stderr in r.err, '%s: stderr does not say %r' % (' '.join(cli), expect_stderr))
        check_baf_dirs_default(ctx, out, o, pb)
        pb.done()
    fn.__name__ = 't_opt_' + tag
    return fn


for _tag, _cli, _o, *_more in [
        ('no_mat8', ['--no-mat8'], BafOpts(mat8=False)),
        ('no_mat16', ['--no-mat16'], BafOpts(mat16=False)),
        # the 3DOs then name <base>16.mat files that are not written: the tool says so
        ('no_mat8_no_mat16', ['--no-mat8', '--no-mat16'], BafOpts(mat8=False, mat16=False),
         b'which are not written'),
        ('no_bmp', ['--no-bmp'], BafOpts(bmp=False)),
        ('no_debug_dumps', ['--no-debug-dumps'], BafOpts(debug=False)),
        ('scale', ['--scale', '0.25'], BafOpts(scale=0.25)),
        ('angles_rad2deg', ['--angles', 'rad2deg'], BafOpts(angles='rad2deg')),
        ('radius_legacy', ['--radius', 'legacy'], BafOpts(radius=0.1)),
        ('radius_fixed', ['--radius', '0.5'], BafOpts(radius=0.5)),
        ('radius_stored', ['--radius', 'stored'], BafOpts(radius='stored')),
        ('radius_animated', ['--radius', 'animated'], BafOpts(radius='animated')),
        ('no_draw_scale', ['--no-draw-scale'], BafOpts(draw_scale=False)),
        ('insert_floor', ['--insert-offset', 'floor'], BafOpts(insert='floor')),
        ('insert_auto', ['--insert-offset', 'auto'], BafOpts(insert='auto')),
        ('insert_zero', ['--insert-offset', 'zero'], BafOpts(insert='zero')),
        # the re-rooted origin is node 0: the computed RADIUS then also covers every key frame
        ('insert_node0', ['--insert-offset', 'node0'], BafOpts(insert='node0', radius='animated')),
        ('insert_legacy', ['--insert-offset', 'legacy'], BafOpts(insert='legacy')),
        ('insert_xyz', ['--insert-offset', '1.5,-2,0.25'], BafOpts(insert=(1.5, -2.0, 0.25))),
]:
    test('baf.option.' + _tag)(option_test(_tag, _cli, _o, expect_stderr=_more[0] if _more else None))


@test('baf.option.palette')
def t_palette(ctx):
    pb = Problems()
    d = loose(ctx)
    for tag, path, pal in (('pal', os.path.join(d['palette'], 'perm.pal'), mf.PAL_PERM),
                           ('cmp', os.path.join(d['palette'], 'target.cmp'), mf.PAL_D)):
        r, out = bafall(ctx, 'palette_' + tag, '--palette', path)
        sound_run(r, pb, what='--palette %s' % os.path.basename(path))
        check_baf_dirs_default(ctx, out, BafOpts(palette=pal), pb)
    r = ctx.tool('--no-pause', '--palette', os.path.join(d['palette'], 'missing.pal'), 'bafall', d['baf'],
                 ctx.path('bafall_palette_missing'))
    sound_run(r, pb, allowed_rc=(2,), allow_errors=True, what='--palette with a missing file')
    pb.done()


@test('baf.legacy_commands')
def t_baf_legacy(ctx):
    pb = Problems()
    d = loose(ctx)
    out = ctx.path('bafall_legacy')
    shutil.rmtree(out, ignore_errors=True)
    r = ctx.tool('--legacy', '--no-pause', 'bafall', d['baf'], out)
    sound_run(r, pb)
    tree = read_tree(out)
    for bname, spec in BAF_SPECS.items():
        stem = bname.split('.')[0]
        compare_files(sub_tree(tree, stem), legacy_baf_files(mf.build_baf(spec)[0]), 'bafall --legacy %s' % stem, pb)
    one = ctx.path('baf_one')
    shutil.rmtree(one, ignore_errors=True)
    r = ctx.tool('--legacy', '--no-pause', 'baf', os.path.join(d['baf'], 'rig.baf'), one)
    sound_run(r, pb)
    compare_files(read_tree(os.path.join(one, 'rig')), legacy_baf_files(mf.build_baf(BAF_SPECS['rig.baf'])[0]),
                  'baf --legacy rig', pb)
    pb.done()


@test('baf.command_default')
def t_baf_default(ctx):
    pb = Problems()
    d = loose(ctx)
    one = ctx.path('baf_one_default')
    shutil.rmtree(one, ignore_errors=True)
    r = ctx.tool('--no-pause', 'baf', os.path.join(d['baf'], 'box.baf'), one)
    sound_run(r, pb)
    check_baf_dirs_default(ctx, one, BafOpts(), pb, {'box.baf': BAF_SPECS['box.baf']})
    pb.done()


def bafall_specs(ctx, tag, specs, *opts):
    """Runs bafall in default mode over the BAFs `specs` ({file name: spec}); (run, output dir)."""
    src = fresh_dir(ctx.path(tag + '_in'))
    for n, s in specs.items():
        write(os.path.join(src, n), mf.build_baf(s)[0])
    out = ctx.path(tag + '_out')
    shutil.rmtree(out, ignore_errors=True)
    return ctx.tool('--no-pause', *(list(opts) + ['bafall', src, out])), out


@test('baf.draw_scale_names')
def t_draw_scale_names(ctx):
    """BAFs that share a model and a key but are drawn at different scales: JK holds one model or
    key per name, so default mode names the scaled copies apart (<name>_s<hundredths>), and a
    draw scale stored 1e-5 off its value (as about half of the retail BAFs store theirs) gives the
    same output as the exact value. Within the output of all of them, a 3DO or KEY name always
    means the same bytes."""
    pb = Problems()
    base = BAF_SPECS['box.baf']
    specs = {'box.baf': base,
             'boxnear.baf': dict(base, file='boxnear.baf', fac=mf.bits_f32(0x3F800054)),   # 1.00001
             'boxbig.baf': dict(base, file='boxbig.baf', fac=1.5),
             'boxbig2.baf': dict(base, file='boxbig2.baf', fac=mf.bits_f32(0x3FC0007E))}  # 1.500015
    r, out = bafall_specs(ctx, 'drawscale', specs)
    sound_run(r, pb)
    check_baf_dirs_default(ctx, out, BafOpts(), pb, specs)
    tree = read_tree(out)
    by_name = {}
    for k, v in tree.items():
        stem, _, name = k.partition('/')
        if name.endswith('.3do') or name.endswith('.key'):
            by_name.setdefault(name.lower(), {})[stem] = v
    for name, per in sorted(by_name.items()):
        pb.check(len(set(per.values())) == 1, '%s: written with different bytes by %s' % (name, sorted(per)))
    for stem, names in (('box', ('box.3do', 'spin.key')), ('boxnear', ('box.3do', 'spin.key')),
                        ('boxbig', ('box_s150.3do', 'spin_s150.key')), ('boxbig2', ('box_s150.3do', 'spin_s150.key'))):
        for n in names:
            pb.check(stem + '/' + n in tree, '%s: no %s' % (stem, n))
    pb.check(sum(1 for l in r.out.decode('latin-1').splitlines() if 'drawn at scale 1.50' in l) == 2,
             'the renamed outputs of the two BAFs drawn at 1.5 are not reported')
    pb.done()


@test('baf.mat8_colour_warning')
def t_mat8_colour_warning(ctx):
    """A material whose own palette has no close match in the colormap loses its colours in its
    8-bit MAT (JK draws everything through one palette): the tool says so, naming the BAF and the
    material. A material whose palette is the colormap's, reordered, keeps them: no warning."""
    pb = Problems()
    base = BAF_SPECS['stub.baf']
    tex = mf.tex_gradient(4, 4, 1, 200)

    def variant(fname, mname, pal):
        s = dict(base, file=fname, fac=1.0)
        s['materials'] = [{'name': mname, 'type': 3, 'palette': pal,
                           'textures': [{'w': 4, 'h': 4, 'transparent': 0, 'mips': [tex]}]}]
        return s
    specs = {'far.baf': variant('far.baf', 'far.mat', [(255, 0, 128)] * 256),
             'near.baf': variant('near.baf', 'near.mat', mf.PAL_PERM)}
    r, out = bafall_specs(ctx, 'colourwarn', specs)
    sound_run(r, pb)
    lines = [l for l in r.warnings() if 'lose their colours' in l]
    pb.check(any('far.baf' in l and '"far.mat"' in l for l in lines), 'no colour warning for far.mat: %s' % lines)
    pb.check(not any('near.baf' in l for l in lines), 'a colour warning for exact palette matches: %s' % lines)
    check_baf_dirs_default(ctx, out, BafOpts(), pb, specs)
    pb.done()


@test('lab.commands')
def t_lab(ctx):
    pb = Problems()
    d = loose(ctx)
    for layout in ('scrambled', 'sequential'):
        lab = os.path.join(d['lab'], layout + '.lab')
        data = open(lab, 'rb').read()
        for legacy in (False, True):
            out = ctx.path('lab_%s_%s' % (layout, 'legacy' if legacy else 'default'))
            shutil.rmtree(out, ignore_errors=True)
            r = ctx.tool(*((['--legacy'] if legacy else []) + ['--no-pause', 'lab', lab, out]))
            sound_run(r, pb, what='%slab %s' % ('--legacy ' if legacy else '', layout))
            exp = {n: b for n, b in mf.lab_outputs(data, legacy)}
            compare_files(read_tree(out), exp, 'lab %s (%s)' % (layout, 'legacy' if legacy else 'default'), pb)
    pb.done()


@test('z.commands')
def t_z(ctx):
    pb = Problems()
    d = loose(ctx)
    exp = dict(mf.is3_outputs(mf.multi_z_members()))
    for legacy in (False, True):
        out = ctx.path('z_' + ('legacy' if legacy else 'default'))
        shutil.rmtree(out, ignore_errors=True)
        r = ctx.tool(*((['--legacy'] if legacy else []) + ['--no-pause', 'z', os.path.join(d['z'], 'multi.z'), out]))
        sound_run(r, pb)
        compare_files(read_tree(out), exp, 'z (%s)' % ('legacy' if legacy else 'default'), pb)
    pb.done()


@test('b3d.commands')
def t_b3d(ctx):
    pb = Problems()
    d = loose(ctx)
    for name, spec in LEVEL_SPECS.items():
        comp, image = mf.level_file(name, spec, True)
        raw, _ = mf.level_file(name, spec, False)
        stem = name.split('.')[0]
        for sub, data, img in (('level', comp, mf.dec_of(image)), ('level_raw', raw, image)):
            src = os.path.join(d[sub], name)
            out = ctx.path('b3d_%s_%s' % (sub, stem))
            shutil.rmtree(out, ignore_errors=True)
            r = ctx.tool('--no-pause', 'b3d', src, out)
            sound_run(r, pb, what='b3d %s/%s' % (sub, name))
            compare_files(read_tree(out), {'%s/%s' % (stem, n): b for n, b in mf.level_bmps(img, True)},
                          'b3d %s/%s' % (sub, name), pb)
            # --legacy decompresses whatever the flag says, as the original did
            out = ctx.path('b3d_legacy_%s_%s' % (sub, stem))
            shutil.rmtree(out, ignore_errors=True)
            r = ctx.tool('--legacy', '--no-pause', 'b3d', src, out)
            dec = bytearray(data[:mf.B3D_RAW_HEADER])
            dec[mf.B3D_FLAG_OFFSET] = 0
            dec += mf.lzss_decode(data[mf.B3D_RAW_HEADER:])[0]
            legacy = mf.legacy_b3d_outputs(bytes(dec))
            sound_run(r, pb, allowed_rc=(0,) if legacy is not None else (1,), allow_errors=legacy is None,
                      what='--legacy b3d %s/%s' % (sub, name))
            compare_files(read_tree(out), {'%s/%s' % (stem, n): b for n, b in (legacy or [])},
                          '--legacy b3d %s/%s' % (sub, name), pb)
    # b3dall: every *.b3d of a folder (any case), folders named in upper case
    mixed = fresh_dir(ctx.path('b3dall_in'))
    write(os.path.join(mixed, 'box.b3d'), open(os.path.join(d['level'], 'BOX.B3D'), 'rb').read())
    write(os.path.join(mixed, 'Flat.B3d'), open(os.path.join(d['level_raw'], 'FLAT.B3D'), 'rb').read())
    write(os.path.join(mixed, 'notes.b3dx'), b'not a level')
    out = ctx.path('b3dall_out')
    shutil.rmtree(out, ignore_errors=True)
    r = ctx.tool('--no-pause', 'b3dall', mixed, out)
    sound_run(r, pb)
    exp = {}
    for stem, img in (('BOX', mf.dec_of(mf.level_file('BOX.B3D', LEVEL_SPECS['BOX.B3D'])[1])),
                      ('FLAT', mf.level_file('FLAT.B3D', LEVEL_SPECS['FLAT.B3D'], False)[1])):
        for n, b in mf.level_bmps(img, True):
            exp['%s/%s' % (stem, n)] = b
    compare_files(read_tree(out), exp, 'b3dall', pb)
    # --keep-decompressed on a single level
    out = ctx.path('b3d_keep')
    shutil.rmtree(out, ignore_errors=True)
    r = ctx.tool('--no-pause', '--keep-decompressed', 'b3d', os.path.join(d['level'], 'BOX.B3D'), out)
    sound_run(r, pb)
    dec = os.path.join(out, 'BOX', 'BOX.dec')
    if pb.check(os.path.isfile(dec), 'b3d --keep-decompressed wrote no BOX.dec'):
        pb.check(open(dec, 'rb').read() == mf.dec_of(mf.level_file('BOX.B3D', LEVEL_SPECS['BOX.B3D'])[1]),
                 'BOX.dec differs from the decompressed image')
    pb.done()


def helper_path(ctx):
    if ctx.args.helper:
        return ctx.args.helper
    base = os.path.dirname(os.path.abspath(ctx.exe))
    for cand in (os.path.join(base, 'tests', 'unit_b3d_decompress' + EXE_SUFFIX),
                 os.path.join(base, 'tests', os.path.basename(base), 'unit_b3d_decompress' + EXE_SUFFIX),
                 os.path.join(base, 'unit_b3d_decompress' + EXE_SUFFIX)):
        if os.path.isfile(cand):
            return cand
    return None


@test('lzss.differential')
def t_lzss(ctx):
    pb = Problems()
    helper = helper_path(ctx)
    ref = ctx.args.ref_b3d_decompress
    root = fresh_dir(ctx.path('lzss'))
    if not helper and not ref:
        raise Skip('neither the unit_b3d_decompress helper (--helper) nor the original decoder was found')
    cases = mf.lzss_cases()
    complete = 0
    for name, stream in cases:
        level = mf.lzss_level_file(stream)
        src = write(os.path.join(root, name + '.b3d'), level)
        out, end, _ = mf.lzss_decode(stream)
        dec = bytearray(level[:mf.B3D_RAW_HEADER])
        dec[mf.B3D_FLAG_OFFSET] = 0
        expect = bytes(dec) + out
        results = {}
        if helper:
            dst = os.path.join(root, name + '.fork.dec')
            r = run([helper, '--decompress', src, dst], ctx.timeout, watch=root)
            sound_run(r, pb, allowed_rc=(0,) if end == 'marker' else (0, 1), allow_errors=True,
                      what='b3d_decompress_file %s' % name)
            if os.path.isfile(dst):
                results['fork'] = open(dst, 'rb').read()
            else:
                pb.add('%s: b3d_decompress_file wrote nothing' % name)
        if ref:
            dst = os.path.join(root, name + '.orig.dec')
            r = run([ref, src, dst], ctx.timeout, watch=root)
            if r.timed_out or r.rc is None or r.rc < 0:
                pb.add('%s: the original decoder failed: %s' % (name, r.describe()))
            elif os.path.isfile(dst):
                results['original'] = open(dst, 'rb').read()
        for who, got in results.items():
            pb.check(got == expect, '%s: %s output differs from the independent decoder: %s'
                     % (name, who, first_difference(got, expect)))
        if end == 'marker':
            complete += 1
            out_dir = os.path.join(root, 'cli_' + name)
            r = ctx.tool('--no-pause', '--keep-decompressed', 'b3d', src, out_dir)
            sound_run(r, pb, allowed_rc=(0, 1), allow_errors=True, what='b3d --keep-decompressed %s' % name)
            decs = [k for k in (read_tree(out_dir) if os.path.isdir(out_dir) else {}) if k.lower().endswith('.dec')]
            if pb.check(len(decs) == 1, '%s: default mode kept %d .dec files' % (name, len(decs))):
                pb.check(open(os.path.join(out_dir, decs[0]), 'rb').read() == expect,
                         '%s: default mode .dec differs' % name)
    ctx.log('%d streams (%d complete); compared: %s' % (len(cases), complete,
                                                       ', '.join(x for x, y in (('fork', helper), ('original', ref))
                                                                 if y)))
    pb.done()


# ---------------------------------------------------------------------------- damaged and hostile inputs


def batch(ctx, tag, cmd, files, good=None, legacy=False, per_file=None):
    """Writes `files` into a folder, runs `cmd` (bafall/b3dall) over it once, and, if that run is
    not sound, every file on its own to name the culprits. Returns (problems found, output dir)."""
    src = fresh_dir(ctx.path('damaged_' + tag))
    for n, d in files:
        write(os.path.join(src, n), d)
    if good:
        write(os.path.join(src, good[0]), good[1])
    out = ctx.path('damaged_%s_out' % tag)
    shutil.rmtree(out, ignore_errors=True)
    pre = ['--legacy'] if legacy else []
    r = ctx.tool(*(pre + ['--no-pause', '-q', cmd, src, out]), timeout=ctx.damaged_timeout)
    pb = Problems()
    if sound_run(r, pb, allowed_rc=(0, 1), allow_errors=True, what='%s over %d damaged files' % (cmd, len(files))):
        if r.rc == 1:
            pb.check(r.errors(), '%s: exit 1 without an error line' % tag)
        return pb, out, r
    # find the files that crash or hang
    culprits = []
    for n, d in files:
        one = fresh_dir(ctx.path('damaged_one'))
        write(os.path.join(one, n), d)
        o2 = ctx.path('damaged_one_out')
        shutil.rmtree(o2, ignore_errors=True)
        r1 = ctx.tool(*(pre + ['--no-pause', '-q', cmd, one, o2]), timeout=ctx.damaged_timeout)
        p1 = Problems()
        if not sound_run(r1, p1, allowed_rc=(0, 1), allow_errors=True, what=n):
            culprits.append(p1.items[0].split('\n')[0] + ('\n' + p1.items[0].split('\n', 2)[1]
                                                          if '\n' in p1.items[0] else ''))
    pb.extend(culprits[:12], '%s%s: ' % ('--legacy ' if legacy else '', tag))
    return pb, out, r


def clean_run(ctx, cmd, name, data, legacy):
    """The output of `cmd` (bafall/b3dall) for one undamaged file on its own."""
    src = fresh_dir(ctx.path('clean_in'))
    write(os.path.join(src, name), data)
    out = ctx.path('clean_out')
    shutil.rmtree(out, ignore_errors=True)
    ctx.tool(*((['--legacy'] if legacy else []) + ['--no-pause', '-q', cmd, src, out]))
    return read_tree(out)


@test('robust.baf')
def t_robust_baf(ctx):
    pb = Problems()
    good_spec = BAF_SPECS['stub.baf']
    good = mf.build_baf(good_spec)[0]
    for legacy in (False, True):
        alone = clean_run(ctx, 'bafall', 'mid.baf', good, legacy)
        for bname in ('box.baf', 'rig.baf', 'leaf.baf'):
            files = mf.baf_mutants(BAF_SPECS[bname])
            p, out, r = batch(ctx, '%s_%s' % (bname.split('.')[0], 'legacy' if legacy else 'default'), 'bafall',
                              files, good=('mid.baf', good), legacy=legacy)
            pb.extend(p.items)
            if not p.items:
                compare_files(sub_tree(read_tree(out), 'mid'), sub_tree(alone, 'mid'),
                              '%sundamaged BAF among damaged ones' % ('--legacy ' if legacy else ''), pb)
                named = sum(1 for n, _ in files if n.encode() in r.err)
                pb.check(named > 0, '%s: no error line names a damaged file' % bname)
    pb.done()


@test('robust.level')
def t_robust_level(ctx):
    pb = Problems()
    files = mf.level_mutants()
    good = mf.level_file('BOX.B3D', LEVEL_SPECS['BOX.B3D'])[0]
    for legacy in (False, True):
        alone = clean_run(ctx, 'b3dall', 'mid.b3d', good, legacy)
        p, out, r = batch(ctx, 'level_%s' % ('legacy' if legacy else 'default'), 'b3dall', files,
                          good=('mid.b3d', good), legacy=legacy)
        pb.extend(p.items)
        if not p.items:
            got = read_tree(out)
            key = [k for k in alone if k.split('/')[0].lower() == 'mid']
            pb.check(key, 'the undamaged level wrote nothing on its own')
            compare_files({k: v for k, v in got.items() if k.split('/')[0].lower() == 'mid'},
                          {k: alone[k] for k in key},
                          '%sundamaged level among damaged ones' % ('--legacy ' if legacy else ''), pb)
    pb.done()


@test('robust.lab_z')
def t_robust_lab_z(ctx):
    pb = Problems()
    root = fresh_dir(ctx.path('damaged_labz'))
    for cmd, gen in (('lab', mf.lab_mutants), ('z', mf.z_mutants)):
        for n, data in gen():
            src = write(os.path.join(root, n), data)
            for legacy in (False, True):
                out = os.path.join(root, 'out')
                shutil.rmtree(out, ignore_errors=True)
                r = ctx.tool(*((['--legacy'] if legacy else []) + ['--no-pause', '-q', cmd, src, out]),
                             timeout=ctx.damaged_timeout)
                sound_run(r, pb, allowed_rc=(0, 1), allow_errors=True,
                          what='%s%s %s' % ('--legacy ' if legacy else '', cmd, n))
    pb.done()


def files_outside(root, allowed):
    """Files and folders under `root` other than the `allowed` top-level entries."""
    return sorted(set(os.listdir(root)) - set(allowed))


@test('robust.unsafe_names')
def t_unsafe(ctx):
    pb = Problems()
    root = fresh_dir(ctx.path('hostile'))
    cases = [('baf', 'names.baf', mf.build_baf(mf.unsafe_name_baf())[0]),
             ('lab', 'names.lab', mf.unsafe_name_lab()),
             ('z', 'names.z', mf.unsafe_name_z())]
    for cmd, fname, data in cases:
        for legacy in (False, True):
            case = fresh_dir(os.path.join(root, '%s_%s' % (cmd, 'legacy' if legacy else 'default')))
            inner = fresh_dir(os.path.join(case, 'in'))
            src = write(os.path.join(inner, fname), data)
            nest = fresh_dir(os.path.join(case, 'a', 'b'))
            out = os.path.join(nest, 'out')
            r = ctx.tool(*((['--legacy'] if legacy else []) + ['--no-pause', '-q', cmd, src, out]),
                         timeout=ctx.damaged_timeout)
            label = '%s%s %s' % ('--legacy ' if legacy else '', cmd, fname)
            # The contract: every unsafe name is refused and reported, nothing lands outside the
            # output folder, and the tool goes on (a reader may also refuse the whole file).
            sound_run(r, pb, allowed_rc=(1,), allow_errors=True, what=label)
            pb.check(r.errors(), '%s: unsafe names were not reported as errors' % label)
            pb.check(files_outside(case, ('in', 'a')) == [] and files_outside(os.path.join(case, 'a'), ('b',)) == []
                     and files_outside(nest, ('out',)) == [], '%s: something was written outside the output folder'
                     % label)
            got = read_tree(out) if os.path.isdir(out) else {}
            if cmd == 'lab':
                pb.check(got.get('ok1.txt') == b'one' and got.get('ok2.txt') == b'two',
                         '%s: the safe entries were not extracted' % label)
                pb.check(set(got) <= {'ok1.txt', 'ok2.txt'}, '%s: unsafe entries written: %s' % (label, sorted(got)))
            elif cmd == 'z':
                pb.check(set(got) <= {'ok.bin', 'sub/ok2.bin'}, '%s: unsafe members written: %s' % (label, sorted(got)))
                for n, want in (('ok.bin', b'fine'), ('sub/ok2.bin', b'fine2')):
                    pb.check(got.get(n, want) == want, '%s: %s has the wrong content' % (label, n))
            else:
                names = set(got)
                bad = [n for n in names if re.search(r'up\.|abs\.|drive\.|\.\.', n.lower())
                       or n.lower().split('/')[-1] in ('nul.mat', 'nul.bmp')]
                pb.check(not bad, '%s: unsafe outputs written: %s' % (label, bad))
            ctx.log('%s wrote %s' % (label, sorted(got)))
    pb.done()


# ============================================================================ real-data tests (TPM_DISC)


def real_root(ctx):
    disc_root = ctx.args.disc
    if not disc_root:
        raise Skip('TPM_DISC is not set')
    if not os.path.isdir(os.path.join(disc_root, 'GAMEDATA')) and not os.path.isdir(os.path.join(disc_root, 'gamedata')):
        raise Fail('TPM_DISC=%s has no GAMEDATA folder' % disc_root)
    return disc_root


def real_work(ctx):
    def make():
        base = ctx.args.real_work
        if base:
            os.makedirs(base, exist_ok=True)
            work = tempfile.mkdtemp(prefix='tpmx-real-', dir=base)
        else:
            work = tempfile.mkdtemp(prefix='tpmx-real-')
        src = os.environ.get('TPMX_SOURCE_DIR') or os.path.dirname(HERE)
        if inside(work, src):
            shutil.rmtree(work, ignore_errors=True)
            raise Fail('refusing to write real game data inside the source tree (%s); set TPM_WORK' % work)
        ctx.cache['real_cleanup'] = work
        return work
    return ctx.memo('real_work', make)


def real_dump(ctx, mode):
    """Runs the tool on the real disc (mode 'legacy' or 'default'); returns (dump dir, run)."""
    def make():
        disc_root = real_root(ctx)
        out = os.path.join(real_work(ctx), mode)
        args = (['--legacy'] if mode == 'legacy' else []) + ['--no-pause', disc_root, out]
        r = ctx.tool(*args, timeout=ctx.timeout * 30, real=True)
        return out, r
    return ctx.memo('real_' + mode, make)


def manifest_rows(path):
    rows = {}
    with open(path, 'r', encoding='utf-8', newline='') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'):
                continue
            rel, size, sha = line.split('\t')
            rows[rel] = (int(size), sha)
    return rows


TAIL_LINE = re.compile(rb'^(tail\[\d+\]=)[^\r\n]*(\r?\n?)$', re.M)


@test('real.legacy_identity', suite='real')
def t_real_identity(ctx):
    real_root(ctx)
    if not ctx.args.ref_manifest:
        raise Skip('TPM_REF_MANIFEST is not set (manifest of the original\'s output)')
    pb = Problems()
    ref = manifest_rows(ctx.args.ref_manifest)
    out, r = real_dump(ctx, 'legacy')
    run_pb = Problems()
    sound_run(r, run_pb)
    pb.raw += run_pb.items
    got = {}
    masked = set()
    unmasked_ref = set()    # zero tails here, but the reference hashes the original's garbage
    for d, dirs, files in os.walk(out):
        for f in files:
            p = os.path.join(d, f)
            rel = os.path.relpath(p, out).replace(os.sep, '/')
            data = open(p, 'rb').read()
            row = ref.get(rel)
            entry = (len(data), hashlib.sha1(data).hexdigest())
            if row and row != entry:
                m, n = TAIL_LINE.subn(rb'\1<undefined>\2', data)
                tails = re.findall(rb'^tail\[\d+\]=([^\r\n]*)\r\n', data, re.M)
                zero_tails = len(tails) == 16 and all(t == b'0\t0.000000' for t in tails)
                if n == 16 and row == (len(m), hashlib.sha1(m).hexdigest()):
                    pb.check(zero_tails, '%s: the masked tail lines are not zeros' % rel)
                    masked.add(rel)
                    entry = row
                elif n == 16 and zero_tails:
                    unmasked_ref.add(rel)
            got[rel] = entry
    if unmasked_ref:
        # A raw manifest of the original's output: its 'tail[i]=' lines are uninitialised memory
        # (different on every run of the original), so no tool can match their hashes.
        pb.add('TPM_REF_MANIFEST: looks like a raw manifest of the original output, %d files with zero tails '
               'differ; use a manifest whose tail lines in those files are masked as <undefined>, since the '
               'original printed uninitialised memory there' % len(unmasked_ref))
    missing = sorted(set(ref) - set(got))
    extra = sorted(set(got) - set(ref))
    differ = sorted(k for k in set(ref) & set(got) if ref[k] != got[k])
    pb.check(not missing, '%d files of the reference are missing, e.g. %s' % (len(missing), missing[:5]))
    pb.check(not extra, '%d files are not in the reference, e.g. %s' % (len(extra), extra[:5]))
    pb.check(not differ, '%d files differ from the reference, e.g. %s' % (len(differ), differ[:5]))
    if ctx.args.ref_undefined:
        listed = {l.strip() for l in open(ctx.args.ref_undefined) if l.strip() and not l.startswith('#')}
        pb.check(masked == listed, 'masked tails in %d files, the reference lists %d' % (len(masked), len(listed)))
    dirs = read_dirs(out)
    with_files = {os.path.dirname(k) for k in got} | {'/'.join(k.split('/')[:i]) for k in got
                                                       for i in range(1, k.count('/'))}
    empty = sorted(d for d in dirs - with_files if not os.listdir(os.path.join(out, d)))
    pb.check(set(empty) <= {'biglab', 'localizelab', 'voicelab'}, 'unexpected empty folders %s' % empty[:5])
    print('    %d files compared with the reference manifest; %d with masked tails; empty folders: %s'
          % (len(got), len(masked), ', '.join(empty) or 'none'))
    pb.done(summary=True)


def real_baf_dirs(root):
    base = os.path.join(root, 'BAF')
    return sorted(d for d in os.listdir(base) if os.path.isdir(os.path.join(base, d))) if os.path.isdir(base) else []


@test('real.default_smoke', suite='real')
def t_real_default(ctx):
    real_root(ctx)
    pb = Problems()
    out, r = real_dump(ctx, 'default')
    run_pb = Problems()
    sound_run(r, run_pb)
    pb.raw += run_pb.items
    stats = {'cmp': 0, 'mat8': 0, '3do': 0, 'bafs': 0, 'key3do': 0, 'node0_low': 0}
    for stem in real_baf_dirs(out):
        stats['bafs'] += 1
        d = os.path.join(out, 'BAF', stem)
        files = {f: open(os.path.join(d, f), 'rb').read() for f in os.listdir(d)}
        cmps = [f for f in files if f.lower().endswith('.cmp')]
        pb.check(cmps or not any(f.lower().endswith('.mat') for f in files),
                 'BAF/%s: no colormap next to the 8-bit MATs that index one' % stem)
        pal = None
        for f in cmps:
            c = check_retail_cmp(files[f], 'BAF/%s/%s' % (stem, f), pb)
            stats['cmp'] += 1
            if c:
                pal = c['palette']
        mats = {}
        for f, data in files.items():
            if f.lower().endswith('.mat'):
                try:
                    mats[f] = parse_mat(data)
                except ValueError as e:
                    pb.add('BAF/%s/%s: %s' % (stem, f, e))
        for f, m in mats.items():
            if not m['raw_fill'] or m['fmt'][1] != 8:
                continue                      # not a raw copy (16-bit MAT or an 8-bit MAT written by the tool)
            pb.check(not m['type'] & 1 or (m['palette'] is not None and m['trailing'] == 0),
                     'BAF/%s/%s: type %d announces a 768-byte palette after the pixels, which the copy lacks'
                     % (stem, f, m['type']))
            m8 = mat8_name(f)
            if not pb.check(m8 in files, 'BAF/%s: no %s for %s' % (stem, m8, f)):
                continue
            stats['mat8'] += 1
            own = m['palette']
            src_pal = own if m['type'] & 1 else pal
            texs = [(t['w'], t['h'], t['alpha'], t['mips']) for t in m['textures']]
            check_mat8(files[m8], 'BAF/%s/%s' % (stem, m8), pb, texs, pal if pal is not None else own,
                       [src_pal] * len(texs))
        for f, data in files.items():
            if not f.endswith('.3do'):
                continue
            m = try_parse_3do(data, 'BAF/%s/%s' % (stem, f), pb)
            if m is None:
                continue
            if m['kind'] != 'model':
                stats['key3do'] += 1
                continue
            stats['3do'] += 1
            # --insert-offset auto (the default): the lowest point on the floor, after a re-root at
            # node 0 when node 0 lay above z = 0 (it is then at about z 0, and RADIUS covers the keys)
            low = bool(m['nodes']) and abs(float(m['nodes'][0]['pos'][2])) <= 1e-3
            stats['node0_low'] += low
            check_3do_numbers(m, 'BAF/%s/%s' % (stem, f), pb, radius='covers' if low else 'computed')
            for mat in m['materials']:
                pb.check(mat in files, 'BAF/%s/%s: MATERIALS names %s, which was not written' % (stem, f, mat))
            for kf, kdata in files.items():
                if kf.endswith('.key'):
                    km = KEY_JOINTS.search(kdata)
                    pb.check(km is not None and int(km.group(1)) >= len(m['nodes']),
                             'BAF/%s/%s: JOINTS %s, fewer than the %d hierarchy nodes of %s'
                             % (stem, kf, km.group(1).decode() if km else '(none)', len(m['nodes']), f))
    levels = sum(1 for k in os.listdir(os.path.join(out, 'LEVEL'))) if os.path.isdir(os.path.join(out, 'LEVEL')) else 0
    print('    default mode: %d BAF folders, %d CMPs, %d 8-bit MATs, %d model 3DOs (%d key-format, %d with node 0 '
          'at z 0), %d levels' % (stats['bafs'], stats['cmp'], stats['mat8'], stats['3do'], stats['key3do'],
                                  stats['node0_low'], levels))
    pb.check(stats['bafs'] > 0 and stats['3do'] > 0, 'no BAF output to check')
    pb.done(summary=True)


@test('real.default_vs_legacy', suite='real')
def t_real_compare(ctx):
    real_root(ctx)
    pb = Problems()
    lout, lr = real_dump(ctx, 'legacy')
    dout, dr = real_dump(ctx, 'default')
    if lr.rc != 0 or dr.rc != 0:
        raise Fail('a real-data run failed (legacy exit %s, default exit %s)' % (lr.rc, dr.rc))

    def digest(p):
        with open(p, 'rb') as f:
            return f.read()

    # each BAF's applied draw scale (header +0xAC, in hundredths), from the BAFs the run extracted
    # from big.lab; a 3DO or KEY written at a scale other than 1 carries it in its name
    steps = {}
    big = os.path.join(lout, 'biglab')
    for f in os.listdir(big) if os.path.isdir(big) else []:
        if f.lower().endswith('.baf'):
            with open(os.path.join(big, f), 'rb') as fh:
                head = fh.read(0xB0)
            if len(head) == 0xB0:
                steps[f.split('.')[0]] = draw_scale_steps({'fac': struct.unpack_from('<f', head, 0xAC)[0]})

    def fac_of(stem):
        s = steps.get(stem, 100)
        return None if s == 100 else s / 100.0

    # --insert-offset auto re-roots a model at node 0 when node 0 lies above z = 0: how far node 0
    # was lowered in each such BAF (default-mode units), from the two 3DOs
    rerooted = {}
    for stem in real_baf_dirs(dout):
        ld, dd = os.path.join(lout, 'BAF', stem), os.path.join(dout, 'BAF', stem)
        legacy_files = set(os.listdir(ld)) if os.path.isdir(ld) else set()
        renamed = {scaled_name(f, steps.get(stem, 100)): f for f in legacy_files if f.endswith('.3do')}
        for f in os.listdir(dd):
            lf = renamed.get(f, f)
            if not f.endswith('.3do') or lf not in legacy_files:
                continue
            try:
                a, b = parse_3do(digest(os.path.join(dd, f))), parse_3do(digest(os.path.join(ld, lf)))
            except ValueError:
                continue                      # reported by the comparison below
            if a['kind'] == 'model' and b['kind'] == 'model' and a['nodes'] and b['nodes']:
                dz = float(b['nodes'][0]['pos'][2]) * (fac_of(stem) or 1.0) - float(a['nodes'][0]['pos'][2])
                if dz > 1e-5:
                    rerooted[stem] = dz

    same = changed = palette_appended = 0
    changed_kinds = {}
    for d, dirs, files in os.walk(lout):
        for f in files:
            p = os.path.join(d, f)
            rel = os.path.relpath(p, lout).replace(os.sep, '/')
            q = os.path.join(dout, rel)
            low = f.lower()
            free = (low.endswith('.3do') or low.endswith('.3do.txt') or low.endswith('.key.txt') or f == 'REMAIN.HEX'
                    or (rel.startswith('BAF/') and low.endswith('.cmp')))
            if rel.startswith('BAF/') and (low.endswith('.key') or low.endswith('.3do')):
                stem = rel.split('/')[1]
                q = os.path.join(dout, 'BAF', stem, scaled_name(f, steps.get(stem, 100)))
            if rel.startswith('BAF/') and low.endswith('.key'):
                if os.path.isfile(q):
                    compare_key(digest(q), digest(p), rel, pb, fac_of(stem), rerooted.get(stem))
                    same += 1
                    continue
            if not os.path.isfile(q):
                pb.add('default mode lacks %s' % rel)
                continue
            if free:
                changed_kinds[low.rsplit('.', 1)[-1]] = changed_kinds.get(low.rsplit('.', 1)[-1], 0) + 1
                continue
            a, b = digest(p), digest(q)
            if a == b:
                same += 1
            elif low.endswith('.mat') and len(b) == len(a) + 768 and b[:len(a)] == a:
                palette_appended += 1
            else:
                changed += 1
                pb.add('%s differs between --legacy and default mode' % rel)
    shared = {}   # default-mode 3DO or KEY name -> [(BAF, its --legacy file, its default-mode file)]
    for stem in real_baf_dirs(dout):
        ld = os.path.join(lout, 'BAF', stem)
        dd = os.path.join(dout, 'BAF', stem)
        legacy_files = set(os.listdir(ld)) if os.path.isdir(ld) else set()
        renamed = {scaled_name(f, steps.get(stem, 100)): f for f in legacy_files if f.endswith('.3do') or f.endswith('.key')}
        for f in os.listdir(dd):
            lf = renamed.get(f, f)
            if (f.endswith('.3do') or f.endswith('.key')) and lf in legacy_files:
                shared.setdefault(f.lower(), []).append((stem, lf, f))
            if f.endswith('.3do') and lf in legacy_files:
                a = try_parse_3do(digest(os.path.join(dd, f)), 'BAF/%s/%s' % (stem, f), pb)
                b = try_parse_3do(digest(os.path.join(ld, lf)), 'BAF/%s/%s (--legacy)' % (stem, lf), pb)
                if a and b and a['kind'] == 'model' and b['kind'] == 'model':
                    # --legacy lists <base>16.mat, default mode <base>8.mat
                    names = [x[:-6] + '8.mat' if x.endswith('16.mat') else x for x in b['materials']]
                    if stem in rerooted:      # node 0 lowered by the re-root (checked through the keys)
                        f0 = fac_of(stem)
                        b['nodes'][0]['pos'] = b['nodes'][0]['pos'][:2] + [
                            a['nodes'][0]['pos'][2] if f0 is None else '%.9f' % (float(a['nodes'][0]['pos'][2]) / f0)]
                    compare_3do(a, b, 'BAF/%s/%s' % (stem, f), pb, names, fac=fac_of(stem))
            elif lf not in legacy_files:
                # a colormap-less BAF also gets the colormap its 8-bit MATs index
                ok = (f.endswith('8.mat') or f.endswith('.3do') or f.endswith('.key') or f.endswith('.txt')
                      or (f.endswith('.cmp') and not any(x.lower().endswith('.cmp') for x in legacy_files)))
                pb.check(ok, 'default mode adds BAF/%s/%s' % (stem, f))
    # JK holds one model and one key per name: where BAFs share a name and the original wrote the
    # same bytes for all of them, default mode must too (a draw scale other than 1 renames them)
    clashes = 0
    for name, members in sorted(shared.items()):
        if len(members) < 2:
            continue
        if len({digest(os.path.join(lout, 'BAF', s, lf)) for s, lf, _ in members}) != 1:
            continue        # the BAFs' own data differ: a clash in the original's output too
        if len({digest(os.path.join(dout, 'BAF', s, f)) for s, _, f in members}) != 1:
            clashes += 1
            pb.add('%s: identical in the original\'s output of BAFs %s, different in default mode'
                   % (name, ', '.join(s for s, _, _ in members[:4])))
    print('    %d files identical in both modes (keys: up to the draw scale, %d BAFs drawn at a scale other '
          'than 1), %d raw MATs with their palette appended, %d allowed to differ %s; %d model/key names shared '
          'by several BAFs' % (same, sum(1 for v in steps.values() if v != 100), palette_appended,
                               sum(changed_kinds.values()), changed_kinds,
                               sum(1 for m in shared.values() if len(m) > 1)))
    pb.done(summary=True)


# ============================================================================ main


def main(argv=None):
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--exe', default=env('TPMX_EXE'), help='the tpmextractor executable (TPMX_EXE)')
    ap.add_argument('--helper', default=env('TPMX_B3D_HELPER'),
                    help='the unit_b3d_decompress program (default: next to the executable, under tests/)')
    ap.add_argument('--suite', choices=('synthetic', 'real', 'all'), default='all')
    ap.add_argument('--work', default=None, help='scratch folder for synthetic outputs (default: '
                    '$TPMX_TEST_TMP/run_tests, else a temporary folder)')
    ap.add_argument('--ref-exe', default=env('TPMX_REF_EXE'), help='a build of the original tool (TPMX_REF_EXE)')
    ap.add_argument('--ref-b3d-decompress', default=env('TPMX_REF_B3D_DECOMPRESS'),
                    help='a build of the original level decoder: PROG IN.B3D OUT.dec (TPMX_REF_B3D_DECOMPRESS)')
    ap.add_argument('--disc', default=env('TPM_DISC'), help='real disc root (TPM_DISC); real-data tests skip without it')
    ap.add_argument('--ref-manifest', default=env('TPM_REF_MANIFEST'), help='manifest of the original\'s output')
    ap.add_argument('--ref-undefined', default=env('TPM_REF_UNDEFINED'), help='files with masked tails (optional)')
    ap.add_argument('--real-work', default=env('TPM_WORK'), help='folder for real-data dumps (outside the source tree)')
    ap.add_argument('--keep', action='store_true', help='keep the synthetic outputs')
    ap.add_argument('--keep-real', action='store_true', help='keep the real-data dumps (game data: never share them)')
    ap.add_argument('-k', dest='only', action='append', default=[], help='run only tests whose name contains this')
    ap.add_argument('-x', dest='exclude', action='append', default=[], help='skip tests whose name contains this')
    ap.add_argument('--list', action='store_true', help='list the tests and exit')
    ap.add_argument('--timeout', type=float, default=float(env('TPMX_TEST_TIMEOUT') or 120),
                    help='seconds per tool run (real-data runs get 30 times this)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)

    selected = [t for t in TESTS if (args.suite in ('all', t[1])) and (not args.only or any(k in t[0] for k in args.only))
                and not any(x in t[0] for x in args.exclude)]
    if args.list:
        for name, suite, _ in selected:
            print('%-10s %s' % (suite, name))
        return 0
    if not args.exe:
        ap.error('give --exe or set TPMX_EXE')
    args.exe = os.path.abspath(args.exe)
    if not os.path.isfile(args.exe):
        ap.error('no executable at %s' % args.exe)
    if args.work is None:
        tmp = env('TPMX_TEST_TMP')
        args.work = os.path.join(tmp, 'run_tests') if tmp else tempfile.mkdtemp(prefix='tpmx-tests-')
    args.work = os.path.abspath(args.work)
    if os.path.isdir(args.work):
        shutil.rmtree(args.work)
    os.makedirs(args.work)
    ctx = Ctx(args)
    passed = failed = skipped = 0
    t_start = time.time()
    try:
        for name, suite, fn in selected:
            t0 = time.time()
            try:
                fn(ctx)
                passed += 1
                print('ok   %s (%.1f s)' % (name, time.time() - t0))
            except Skip as e:
                skipped += 1
                print('skip %s: %s' % (name, e))
            except Fail as e:
                failed += 1
                print('FAIL %s:\n  %s' % (name, str(e).replace('\n', '\n  ')))
            except Exception as e:      # a bug in the test or an unexpected tool output
                failed += 1
                import traceback
                print('FAIL %s: %s: %s\n%s' % (name, type(e).__name__, e, traceback.format_exc()))
            sys.stdout.flush()
    finally:
        real = ctx.cache.get('real_cleanup')
        if real and not args.keep_real:
            shutil.rmtree(real, ignore_errors=True)
        if not args.keep and failed == 0:
            shutil.rmtree(args.work, ignore_errors=True)
    print('run_tests: %d passed, %d skipped, %d failed (%.0f s)' % (passed, skipped, failed, time.time() - t_start))
    if failed:
        return 1
    if passed == 0 and skipped:
        return 77
    return 0


if __name__ == '__main__':
    sys.exit(main())
