#!/usr/bin/env python3
"""Dump C++ vtables of a KDK arm64e kext (MH_KEXT_BUNDLE with LC_DYLD_CHAINED_FIXUPS, pointer_format 7).

    python3 -I x9/usb/xdc/kext_vtable.py <kext binary> <vtable symbol> [--json out.json]

Each slot is printed as: index, target name (local symbol for a rebase, import name
for a bind), pointer-auth diversity. Slot 0 is the first function pointer (vtable
symbol + 16). Read-only; uses llvm-nm/llvm-cxxfilt from Homebrew llvm@22.
"""
import json
import struct
import subprocess
import sys

LLVM = '/opt/homebrew/opt/llvm@22/bin/'


def load(path):
    d = open(path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, sizeofcmds = struct.unpack_from('<IIIIII', d, 0)
    assert magic == 0xfeedfacf
    segs, fixups, off = [], None, 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19:
            name = d[off + 8:off + 24].rstrip(b'\0').decode()
            va, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
            segs.append((name, va, vs, fo, fs))
        elif cmd == 0x80000034:  # LC_DYLD_CHAINED_FIXUPS
            fixups = struct.unpack_from('<II', d, off + 8)
        off += size
    return d, segs, fixups


def walk(d, segs, fixups):
    """Map file offset -> ('rebase', vmaddr, div) or ('bind', name, div)."""
    dataoff, _ = fixups
    ver, starts_off, imports_off, symbols_off, imports_count, imports_fmt, sym_fmt = struct.unpack_from('<IIIIIII', d, dataoff)
    assert imports_fmt == 1, imports_fmt
    names = []
    for i in range(imports_count):
        v, = struct.unpack_from('<I', d, dataoff + imports_off + 4 * i)
        no = v >> 9
        e = d.index(b'\0', dataoff + symbols_off + no)
        names.append(d[dataoff + symbols_off + no:e].decode())
    base = segs[0][1]
    s0 = dataoff + starts_off
    seg_count, = struct.unpack_from('<I', d, s0)
    out = {}
    for si in range(seg_count):
        so, = struct.unpack_from('<I', d, s0 + 4 + 4 * si)
        if not so:
            continue
        p = s0 + so
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', d, p)
        assert fmt in (7, 8, 1), fmt
        stride = 4 if fmt in (7, 8) else 8
        starts = struct.unpack_from('<%dH' % page_count, d, p + 22)
        seg_fo = next(s[3] for s in segs if s[1] - base == seg_offset) if True else 0
        for pi, st in enumerate(starts):
            if st == 0xffff:
                continue
            fo = seg_fo + pi * page_size + st
            while True:
                v, = struct.unpack_from('<Q', d, fo)
                auth, bind = v >> 63 & 1, v >> 62 & 1
                nxt = v >> 51 & 0x7ff
                div = v >> 32 & 0xffff if auth else None
                if bind:
                    out[fo] = ('bind', names[v & 0xffff], div)
                else:
                    tgt = (v & 0xffffffff) if auth else (v & 0x7ffffffffff)
                    out[fo] = ('rebase', base + tgt, div)
                if not nxt:
                    break
                fo += nxt * stride
    return out


def symbols(path):
    syms = {}
    for l in subprocess.run([LLVM + 'llvm-nm', '--defined-only', path], capture_output=True, text=True).stdout.splitlines():
        a, t, n = l.split(None, 2)
        syms.setdefault(int(a, 16), n)
    return syms


def demangle(names):
    r = subprocess.run([LLVM + 'llvm-cxxfilt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, r))


def vtable(path, sym):
    d, segs, fixups = load(path)
    fx = walk(d, segs, fixups)
    syms = symbols(path)
    byname = {n: a for a, n in syms.items()}
    va = byname[sym] + 16
    seg = next(s for s in segs if s[1] <= va < s[1] + s[2])
    fo = seg[3] + va - seg[1]
    slots = []
    while fo in fx:
        kind, t, div = fx[fo]
        name = t if kind == 'bind' else syms.get(t, hex(t))
        slots.append({'mangled': name, 'local': kind == 'rebase', 'div': div})
        fo += 8
    dm = demangle([s['mangled'] for s in slots])
    for s in slots:
        s['name'] = dm[s['mangled']]
    return slots


if __name__ == '__main__':
    slots = vtable(sys.argv[1], sys.argv[2])
    if '--json' in sys.argv:
        json.dump(slots, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
    for i, s in enumerate(slots):
        print('%3d %s %-6s %s' % (i, 'L' if s['local'] else 'I', '%04x' % s['div'] if s['div'] is not None else '-', s['name']))
