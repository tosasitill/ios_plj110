#!/usr/bin/env python3
"""Render X9 SAMPLE lines from a sweet console log as symbolicated stacks.

  x9-sample-report.py SYMB_TABLE LOG

SYMB_TABLE is sweet-crash.py output ("0xADDR lib symbol (in lib) + N").
Records split on the console's line-truncation (a new "X9 SAMPLE:" prefix can
appear mid-line), so each line is split on that marker first.
"""
import re, sys

sym = {}
for l in open(sys.argv[1]):
    m = re.match(r'\s*\d*?\s*(0x[0-9a-f]+)\s+(.*)', l)
    if m:
        sym[int(m.group(1), 16)] = m.group(2).strip()

def s(a):
    return sym.get(a, hex(a) + " ?")


def main():
    for raw in open(sys.argv[2]):
        for rec in re.split(r'(?=\[?\s*[0-9.]*\]? ?X9 SAMPLE:)', raw):
            m = re.search(r'X9 SAMPLE: (begin|end) (\d+)', rec)
            if m:
                print(('=== pass %s ===' % m.group(2)) if m.group(1) == 'begin' else '')
                continue
            m = re.search(r'X9 SAMPLE: (\w+)\[(\d+)\] t(\d+) info=\d+ run=(\d) flags=(0x[0-9a-f]+) '
                          r'sleep=\d+ pc=(0x[0-9a-f]+) lr=(0x[0-9a-f]+) sp=(0x[0-9a-f]+) fp=(0x[0-9a-f]+) bt ((?:0x[0-9a-f]+ ?)*)', rec)
            if not m:
                continue
            proc, pid, tid, run, flags, pc, lr, _sp, _fp, bts = m.groups()
            addrs = [int(a, 16) for a in re.findall(r'0x[0-9a-f]+', bts)]
            head = '%s[%s] t%s run=%s flags=%s' % (proc, pid, tid, run, flags)
            print('%s  pc %s' % (head, s(int(pc, 16))))
            print('       lr %s' % s(int(lr, 16)))
            for i, a in enumerate(addrs):
                print('       #%d %s' % (i + 1, s(a)))


if __name__ == '__main__':
    main()
