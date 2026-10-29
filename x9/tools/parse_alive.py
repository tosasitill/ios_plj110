#!/usr/bin/env python3
"""Parse the shim's 5x-redundant alive-probe records from a pmsg capture.

Each probe register is written five times as ``R<idx:2><off:3><val:8>``.
The pmsg zone corrupts single characters, so per character position the
five copies are majority-voted; a position survives unless three of five
copies are corrupted in the same spot.  Domain power-on status lines use
idx >= 0xf0 and print the SPM MTCMOS domain index instead.
"""
import sys
from collections import Counter

REGS = []
for base, name in [(0x3E080000, "ex5"), (0x3E050000, "ex2")]:
    for off in (0x000, 0x00C, 0x010, 0x014, 0x018, 0x024, 0x02C, 0x040,
                0x094, 0x0F00, 0x0F10, 0x0F40):
        REGS.append((name, off))
for off in (0xEC8, 0xED0, 0xED8, 0xEE0, 0xEE8, 0xEF0, 0xEF8, 0xF00, 0xF08,
            0xF10, 0xF18, 0xF20, 0xF28, 0xF30, 0xF38, 0xF40):
    REGS.append(("mmsys", off))
for off in (0x000, 0x00C, 0x010, 0x014, 0x018, 0x1D8, 0x200, 0x608):
    REGS.append(("dsi", off))
for off in (0x000, 0x024, 0x080):
    REGS.append(("dscw", off))
for off in (0x000, 0x008, 0x010, 0x01C):
    REGS.append(("mux", off))

DOMAINS = {11: "disp_vcore", 12: "dis0", 13: "dis1", 25: "mminfra0",
           28: "mm_proc", 22: "ovl0", 23: "ovl1"}


def vote(copies):
    out = []
    for chars in zip(*copies):
        best, n = Counter(chars).most_common(1)[0]
        out.append(best if n * 2 > len(chars) else "?")
    return "".join(out)


def main(path):
    copies = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        # idx chars must be hex to bucket the line; every later character may
        # be corrupted — the majority vote fixes it.
        if not line.startswith("R") or len(line) < 13:
            continue
        idx = line[1:3]
        if not all(c in "0123456789abcdef" for c in idx):
            continue
        copies.setdefault(idx, []).append(line[1:])
    for idx in sorted(copies):
        recs = copies[idx]
        if len(recs) < 2:
            print("%s: only %d copy, raw %s" % (idx, len(recs), recs[0]))
            continue
        body = vote(recs)
        n = int(idx, 16)
        if n >= 0xF0:
            dom = n - 0xF0
            print("domain %-2d %-10s status=%s (%d copies)" %
                  (dom, DOMAINS.get(dom, "?"), body[2], len(recs)))
            continue
        r = REGS[n] if n < len(REGS) else ("?", 0)
        print("%-6s +0x%03x = 0x%s  (%d copies)" % (r[0], int(body[2:5], 16), body[5:13], len(recs)))


if __name__ == "__main__":
    main(sys.argv[1])
