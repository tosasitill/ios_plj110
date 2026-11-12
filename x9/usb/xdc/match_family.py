#!/usr/bin/env python3
"""Name functions of the stripped 20H392 IOUSBDeviceFamily from the symbolized KDK 13.5 copy.

    python3 -I x9/usb/xdc/match_family.py [--want name ...]   # writes x9/out/usbdf-match.json

Seeds: vtable slots. Every C++ vtable of the KDK kext (minus macOS _RESERVED slots)
is paired with the 20H392 vtable of the same length whose slots agree. Then the
call graph is propagated: for each matched function pair whose direct-call (bl)
sequences have the same length, callees are paired by position; a name observed
with two different addresses is reported as a conflict and dropped. Calls through
stubs are compared by import name (KDK) / GOT target (20H392). Read-only.
"""
import collections
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from kext_vtable import load, walk, symbols, demangle  # noqa: E402

LLVM = '/opt/homebrew/opt/llvm@22/bin/'
KDK = '/Library/Developer/KDKs/KDK_13.5_22G74.kdk/System/Library/Extensions/IOUSBDeviceFamily.kext/Contents/MacOS/IOUSBDeviceFamily'
KC = str(ROOT / '.local/x9/usb/native-20H392/kernelcache.macho')
KEXT_TEXT = (0xfffffff005ce2000, 0x19ee4)       # 20H392 IOUSBDeviceFamily __TEXT_EXEC
KEXT_CONST = (0xfffffff006e0ed40, 0x4138)       # __DATA_CONST
INS = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')


def disasm(path, start=None, stop=None):
    cmd = [LLVM + 'llvm-objdump', '-d', '--no-show-raw-insn', '--no-leading-addr' if False else '--macho', path]
    if start:
        cmd = [LLVM + 'llvm-objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % start, '--stop-address=%#x' % stop, path]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    ins = []
    for l in out.splitlines():
        m = INS.match(l)
        if m:
            ins.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return ins


def stubs_of(ins):
    """stub address -> GOT address for adrp/ldr/br (x16/x17) and arm64e auth stubs."""
    res = {}
    for i in range(len(ins) - 2):
        a, op, args = ins[i]
        if op == 'adrp' and args.startswith(('x16', 'x17')):
            page = int(re.search(r'0x([0-9a-f]+)', args).group(1), 16)
            op2, args2 = ins[i + 1][1], ins[i + 1][2]
            m = re.search(r'#(0x[0-9a-f]+|\d+)', args2)
            off = int(m.group(1), 0) if m else 0
            if op2 in ('ldr', 'add') and ins[i + 2][1] in ('br', 'braa', 'ldr'):
                res[a] = page + off
    return res


def calls(ins):
    """(site, target) of every bl."""
    out = []
    for a, op, args in ins:
        if op == 'bl':
            m = re.match(r'#?0x([0-9a-f]+)', args)
            if m:
                out.append((a, int(m.group(1), 16)))
    return out


def kdk_side():
    d, segs, fx = load(KDK)
    fixes = walk(d, segs, fx)
    def fva(fo):
        for nm, va, vs, sfo, fs in segs:
            if sfo <= fo < sfo + fs:
                return va + fo - sfo
    syms = symbols(KDK)
    out = subprocess.run([LLVM + 'llvm-objdump', '-d', '--macho', '--no-show-raw-insn', KDK], capture_output=True, text=True).stdout
    func_calls = collections.defaultdict(list)
    cur = None
    for l in out.splitlines():
        if l.endswith(':') and not l.startswith(' ') and not l.startswith('/'):
            cur = l[:-1]
            continue
        m = re.match(r'^\s*[0-9a-f]+:\s+bl\s+(\S+)(?:\s*;\s*symbol stub for:\s*(\S+))?', l)
        if m and cur:
            t = m.group(1)
            if m.group(2):
                func_calls[cur].append(m.group(2))
            elif t.startswith('0x'):
                func_calls[cur].append(syms.get(int(t, 16)) or t)
            else:
                func_calls[cur].append(t)
    vts = {}
    byname = {n: a for a, n in syms.items()}
    rebases = {fva(fo): v[1] for fo, v in fixes.items() if v[0] == 'rebase'}
    binds = {fva(fo): v[1] for fo, v in fixes.items() if v[0] == 'bind'}
    for n, a in byname.items():
        if n.startswith('__ZTV') and 'MetaClass' not in n:
            p = a + 16; slots = []
            while p in rebases or p in binds:
                slots.append(binds[p] if p in binds else syms.get(rebases[p], hex(rebases[p])))
                p += 8
            vts[n] = [x for x in slots if '_RESERVED' not in x]
    return func_calls, vts, set(syms.values())


def kc_side():
    d = open(KC, 'rb').read()
    n, = struct.unpack_from('<I', d, 16); o = 32; segs = []
    for _ in range(n):
        c, sz = struct.unpack_from('<II', d, o)
        if c == 0x19:
            va, vs, fo, fs = struct.unpack_from('<QQQQ', d, o + 24); segs.append((va, vs, fo, fs))
        o += sz
    rd = lambda va: struct.unpack_from('<Q', d, next(fo + va - a for a, vs, fo, fs in segs if fs and a <= va < a + fs))[0]
    t0, tl = KEXT_TEXT
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    code = d[next(fo + t0 - a for a, vs, fo, fs in segs if fs and a <= t0 < a + fs):][:tl]
    ins = [(i.address, i.mnemonic, i.op_str.replace('#', '')) for i in md.disasm(code, t0)]
    stub = {}
    for k in range(len(ins) - 2):
        a, op, args = ins[k]
        if op == 'adrp' and args.startswith('x16') and ins[k + 1][1] == 'ldr' and ins[k + 2][1] == 'br':
            page = int(args.split(',')[1], 0)
            mm = re.search(r'\[x16, (0x[0-9a-f]+|\d+)\]', ins[k + 1][2])
            stub[a] = rd(page + (int(mm.group(1), 0) if mm else 0))
    inkext = lambda x: t0 <= x < t0 + tl
    # vtables in __DATA_CONST
    c0, cl = KEXT_CONST
    vts = []
    a = c0
    while a < c0 + cl - 16:
        if rd(a) == 0 and rd(a + 8) == 0:
            p = a + 16; slots = []
            while p < c0 + cl:
                v = rd(p)
                if not (0xfffffff004000000 <= v < 0xfffffff00a000000):
                    break
                slots.append(v); p += 8
            if slots:
                vts.append((a, slots)); a = p; continue
        a += 8
    starts = set(t for _, t in calls(ins) if inkext(t) and t not in stub)
    for _, sl in vts:
        starts |= {s for s in sl if inkext(s)}
    starts = sorted(starts)
    import bisect
    func_calls = collections.defaultdict(list)
    for site, tgt in calls(ins):
        k = bisect.bisect_right(starts, site) - 1
        if k < 0:
            continue
        func_calls[starts[k]].append(stub.get(tgt, tgt))
    return func_calls, vts


def main():
    want = sys.argv[sys.argv.index('--want') + 1:] if '--want' in sys.argv else []
    kcalls, kvts, knames = kdk_side()
    ccalls, cvts = kc_side()
    pairs, conflicts = {}, set()
    def add(name, addr):
        if name in conflicts or not name.startswith('__Z') and not name.startswith('_'):
            return False
        old = pairs.get(name)
        if old is None:
            pairs[name] = addr; return True
        if old != addr:
            conflicts.add(name); del pairs[name]
        return False
    used = set()
    for vn, ks in sorted(kvts.items(), key=lambda x: -len(x[1])):
        cands = [(a, sl) for a, sl in cvts if len(sl) == len(ks) and a not in used]
        if len(cands) == 1:
            a, sl = cands[0]; used.add(a)
            add(vn, a)
            for n, t in zip(ks, sl):
                if 'pure_virtual' not in n:
                    add(n, t)
    print('vtable seeds:', len(pairs), 'vtables', len(used), 'of', len(cvts), file=sys.stderr)
    import difflib
    changed = True
    while changed:
        changed = False
        rev = {v: k for k, v in pairs.items()}
        for name, addr in list(pairs.items()):
            kc_ = kcalls.get(name); cc = ccalls.get(addr)
            if not kc_ or not cc:
                continue
            kt = [k if k in pairs else 'K:' + str(k) for k in kc_]
            ct = [rev.get(c, 'C:%x' % c) for c in cc]
            for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, kt, ct, autojunk=False).get_opcodes():
                if op == 'replace' and i2 - i1 == j2 - j1:
                    for kn, ca in zip(kc_[i1:i2], cc[j1:j2]):
                        if isinstance(kn, str) and kn.startswith('_') and add(kn, ca):
                            changed = True
                            rev[ca] = kn
    print('matched', len(pairs), 'conflicts', len(conflicts), file=sys.stderr)
    out = {k: hex(v) for k, v in sorted(pairs.items())}
    Path(ROOT / 'x9/out').mkdir(exist_ok=True)
    (ROOT / 'x9/out/usbdf-match.json').write_text(json.dumps({'pairs': out, 'conflicts': sorted(conflicts)}, indent=1))
    dm = demangle(list(out) + want)
    for w in want:
        print(dm.get(w, w), '->', out.get(w, 'NOT FOUND' + (' (conflict)' if w in conflicts else '')))


if __name__ == '__main__':
    main()
