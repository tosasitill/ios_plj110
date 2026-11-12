#!/usr/bin/env python3
"""Symbolicate iOS userspace addresses from X9 "CRASH <proc> bt ..." log lines.

    python3 -I x9/tools/dsc_symbolicate.py .local/apple/dsc-20H392 <slide> <addr> [<addr> ...]

<slide> is the shared-cache slide printed in the CRASH line. Addresses may be
passed as one space-separated string. The __TEXT map of the extracted cache is
cached in x9/out/dsc-20H392-textmap.json. Needs llvm-nm (Homebrew llvm@22).
"""
import os, struct, sys, json, subprocess, bisect
ROOT, SLIDE = sys.argv[1], int(sys.argv[2], 16)
addrs = [int(a, 16) for arg in sys.argv[3:] for a in arg.split()]
cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../out/dsc-20H392-textmap.json')
if os.path.exists(cache):
    imgs = json.load(open(cache))
else:
    imgs = []
    for dp, _, fs in os.walk(ROOT):
        for f in fs:
            p = os.path.join(dp, f)
            try:
                with open(p, 'rb') as fh:
                    h = fh.read(32)
                    if h[:4] != b'\xcf\xfa\xed\xfe': continue
                    nc, sz = struct.unpack_from('<II', h, 16); cmds = fh.read(sz); o = 0
                    for _ in range(nc):
                        c, cs = struct.unpack_from('<II', cmds, o)
                        if c == 0x19 and cmds[o+8:o+24].rstrip(b'\0') == b'__TEXT':
                            va, vs = struct.unpack_from('<QQ', cmds, o+24); imgs.append([va, va+vs, p])
                        o += cs
            except Exception: pass
    json.dump(imgs, open(cache, 'w'))
nmcache = {}
for a in addrs:
    u = a - SLIDE
    hit = [i for i in imgs if i[0] <= u < i[1]]
    if not hit: print(hex(a), '?'); continue
    lo, hi, p = hit[0]
    if p not in nmcache:
        out = subprocess.run(['/opt/homebrew/opt/llvm@22/bin/llvm-nm', '-n', '--defined-only', p], capture_output=True, text=True).stdout
        syms = []
        for l in out.splitlines():
            parts = l.split(None, 2)
            if len(parts) == 3 and parts[1] in 'tTsS':
                syms.append((int(parts[0], 16), parts[2]))
        nmcache[p] = syms
    syms = nmcache[p]; k = bisect.bisect_right([s[0] for s in syms], u) - 1
    name = '%s+%#x' % (syms[k][1], u - syms[k][0]) if k >= 0 else '?'
    print(hex(a), os.path.basename(p), name)
