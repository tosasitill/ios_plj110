#!/usr/bin/env python3
"""List C++ vtables inside one kext of the 20H392 legacy prelinked kernelcache.

    python3 -I x9/usb/xdc/kc_vtables.py <kernelcache.macho> <bundle id> [--min N]

A vtable is two zero words followed by a run of pointers into executable segments.
Prints address, slot count and the indices of slots equal to the most common
target (__cxa_pure_virtual). Read-only.
"""
import collections
import re
import struct
import sys


def segments(d, off=0):
    n, = struct.unpack_from('<I', d, off + 16); o = off + 32; out = []
    for _ in range(n):
        c, sz = struct.unpack_from('<II', d, o)
        if c == 0x19:
            nm = d[o + 8:o + 24].rstrip(b'\0').decode(); va, vs, fo, fs = struct.unpack_from('<QQQQ', d, o + 24)
            out.append((nm, va, vs, fo, fs))
        o += sz
    return out


def main():
    path, bid = sys.argv[1], sys.argv[2]
    mn = int(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 100
    d = open(path, 'rb').read()
    top = segments(d)
    va2fo = lambda v: next(fo + v - va for nm, va, vs, fo, fs in top if fs and va <= v < va + fs)
    info = next(s for s in top if s[0] == '__PRELINK_INFO')
    xml = d[info[3]:info[3] + info[4]].decode('utf-8', 'replace')
    i = xml.index('<string>%s</string>' % bid)
    start = xml.rindex('<dict><key>CFBundleName', 0, i)
    m = re.search(r'<key>_PrelinkExecutableLoadAddr</key><integer[^>]*>(0x[0-9a-f]+)<', xml[start:i + 4000])
    hdr = int(m.group(1), 16)
    ks = segments(d, va2fo(hdr))
    code = [(va, va + vs) for nm, va, vs, fo, fs in top if 'TEXT_EXEC' in nm]
    iscode = lambda v: any(a <= v < b for a, b in code)
    for nm, va, vs, fo, fs in ks:
        print('kext seg', nm, hex(va), hex(vs))
    words = {}
    allc = collections.Counter()
    for nm, va, vs, fo, fs in top:
        if 'CONST' in nm:
            for k in range(0, fs - 7, 8):
                v, = struct.unpack_from('<Q', d, fo + k)
                if iscode(v):
                    allc[v] += 1
    pv = allc.most_common(1)[0][0]
    print('pure_virtual', hex(pv))
    for nm, va, vs, fo, fs in ks:
        if 'CONST' not in nm:
            continue
        a = va
        fo = va2fo(va)
        end = va + vs
        while a < end - 16:
            w0, w1 = struct.unpack_from('<QQ', d, va2fo(a))
            if w0 == 0 and w1 == 0:
                b = a + 16; n = 0; pures = []
                while b < end:
                    v, = struct.unpack_from('<Q', d, va2fo(b))
                    if not iscode(v):
                        break
                    if v == pv:
                        pures.append(n)
                    n += 1; b += 8
                if n >= mn:
                    print('vtable %#x slots %d pure %s' % (a, n, pures))
                a = b if n else a + 8
            else:
                a += 8


if __name__ == '__main__':
    main()
