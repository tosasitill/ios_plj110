#!/usr/bin/env python3
"""Host termination proof for the bounded FDT collection used by
x9/shim/vcp_handoff.c (the 2026-11-23 revert fix).

The 2026-11-22 version of vcp_handoff qualified the VCP topology with
~35 fdt_property() calls; each call re-validated the whole LK FDT.
In the pre-XNU shim (MMU/D-cache off, no watchdog service) that stalled
the boot. layout() now runs ONE fdt_collect pass (watchdog kick every
2048 tokens, fail-closed after a 2000 ms counter budget).

This test compiles a byte-identical copy of the in-tree fdt_collect.c
(sha256-checked) with a host stub hw.h, then feeds it synthetic FDT
blobs: a valid tree with the five VCP nodes, a large tree that must
trigger watchdog kicks, an over-budget run (counter stub races ahead,
so the budget must refuse), and hostile blobs (NOP flood, unterminated
names, absurd lengths, endless reservation block, excess depth/nodes,
truncation). Every case runs under a hard timeout: a hang is a failure.

Run: python3 x9/shim/test_vcp_collect_termination.py   (exit 0 = pass)
"""
import hashlib, os, struct, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUDGET_STEP_FAST = "1000000000"   # counter races: budget must trip

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdt.h"
static const char paths[5][FDT_CAPTURE_PATH_BYTES] = {
    "", "/soc", "/soc/vcp@31800000", "/soc/mmup@31a00000",
    "/soc/vdec@36000000"};
int main(int argc, char **argv) {
    if (argc != 2) return 64;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 65;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n ? n : 1);
    if (fread(buf, 1, n, f) != (size_t)n) return 66;
    fclose(f);
    static struct fdt_collect c;
    int r = fdt_collect(&c, (uintptr_t)buf, paths, 5, 0x1f);
    const uint8_t *v; size_t len;
    int got = (r == FDT_PROPERTY_PRESENT)
        ? fdt_collect_get(&c, 2, "compatible", &v, &len) : -99;
    printf("result=%d kicks=%u ticks=%llu count=%u get=%d\n", r, c.kicks,
           (unsigned long long)c.ticks, c.count, got);
    return 0;
}
"""

STUB_HW_H = r"""
#pragma once
#include <stdint.h>
#include <stdlib.h>
/* Host stubs: the counter advances by a fixed step per read so tests can
 * model both a normal clock (step 1) and a racing one (huge step, which
 * must trip the collection budget). wdt_kick is a no-op counter sink. */
static uint64_t stub_now;
static inline uint64_t stub_rsr(void) {
    const char *s = getenv("STUB_STEP");
    stub_now += s ? strtoull(s, 0, 0) : 1;
    return stub_now;
}
#define rsr(r) stub_rsr()
static inline uint64_t counter_frequency(void) { return 1000000000ull; }
static inline void wdt_kick(void) {}
"""


def be32(x):
    return struct.pack(">I", x)


class Blob:
    """Minimal FDT v17 builder (struct/strings/reserve)."""
    def __init__(self):
        self.struct = bytearray()
        self.strings = bytearray()
        self.stroff = {}

    def s(self, name):
        if name not in self.stroff:
            self.stroff[name] = len(self.strings)
            self.strings += name.encode() + b"\0"
        return self.stroff[name]

    def begin(self, name):
        self.struct += be32(1) + name.encode() + b"\0"
        while len(self.struct) % 4:
            self.struct += b"\0"

    def prop(self, name, value):
        self.struct += be32(3) + be32(len(value)) + be32(self.s(name))
        self.struct += value
        while len(self.struct) % 4:
            self.struct += b"\0"

    def end_node(self):
        self.struct += be32(2)

    def build(self, reserve=b"\0" * 16, tail=b""):
        self.struct += tail
        total = 40 + len(reserve) + len(self.struct) + len(self.strings)
        hdr = struct.pack(">10I", 0xD00DFEED, total, 40 + len(reserve),
                          40 + len(reserve) + len(self.struct), 40, 17, 16,
                          0, len(self.strings), len(self.struct))
        return hdr + reserve + bytes(self.struct) + bytes(self.strings)


def cells(*vals):
    return b"".join(be32(v) for v in vals)


def valid_blob(dummies=0):
    b = Blob()
    b.begin("")
    b.prop("#address-cells", cells(2)); b.prop("#size-cells", cells(2))
    for i in range(dummies):
        b.begin(f"dummy{i}@0")
        b.prop("reg", cells(0, 0x1000)); b.prop("status", b"okay\0")
        b.end_node()
    b.begin("soc")
    b.prop("#address-cells", cells(2)); b.prop("#size-cells", cells(2))
    b.prop("ranges", b"")
    for name, compat in (("vcp@31800000", "mediatek,vcp"),
                         ("mmup@31a00000", "mediatek,mmup"),
                         ("vdec@36000000", "mediatek,vcodec-dec")):
        b.begin(name)
        b.prop("compatible", compat.encode() + b"\0")
        b.prop("status", b"okay\0")
        b.end_node()
    b.end_node(); b.end_node()
    return b.build(tail=be32(9))


def hostile_cases():
    cases = {}
    # NOP flood, no END: walk must run out of structure block, not spin.
    b = Blob(); b.begin(""); b.end_node()
    cases["nop-flood"] = b.build(tail=be32(4) * 200000)
    # Node name byte clobbered so the name scan runs long.
    raw = bytearray(valid_blob()); raw[40 + 16 + 4] = 0x41
    cases["name-clobber"] = bytes(raw)
    # Property with absurd length.
    b = Blob(); b.begin("")
    b.struct += be32(3) + be32(0x7FFFFFFF) + be32(b.s("reg")) + cells(0)
    b.end_node()
    cases["huge-prop"] = b.build(tail=be32(9))
    # Reservation block that never terminates (nonzero entries fill it).
    b = Blob(); b.begin(""); b.end_node()
    cases["reserve-forever"] = b.build(reserve=b"\x01" * 16 * 64,
                                        tail=be32(9))
    # Nesting far beyond the depth cap.
    b = Blob(); b.begin("")
    for i in range(200):
        b.begin(f"n{i}")
    for i in range(200):
        b.end_node()
    b.end_node()
    cases["deep-nest"] = b.build(tail=be32(9))
    # Truncated valid blob (cut mid-structure).
    cases["truncated"] = valid_blob()[:100]
    # Bad magic.
    raw = bytearray(valid_blob()); raw[0] = 0
    cases["bad-magic"] = bytes(raw)
    return cases


def main():
    src = (HERE / "fdt_collect.c").read_bytes()
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "fdt_collect.c").write_bytes(src)
        (td / "fdt.h").write_bytes((HERE / "fdt.h").read_bytes())
        (td / "hw.h").write_text(STUB_HW_H)
        (td / "harness.c").write_text(HARNESS_C)
        digest = hashlib.sha256((td / "fdt_collect.c").read_bytes()).hexdigest()
        assert digest == hashlib.sha256(src).hexdigest()
        exe = td / "harness"
        cc = ["cc", "-O2", "-Wall", "-Wextra", "-Werror"]
        # Repo-known environment quirk: the default CLT SDK (27.2) breaks
        # tapi linking in non-login shells; pin the 26.5 SDK when present.
        sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
        if sdk.is_dir():
            cc += ["-isysroot", str(sdk)]
        subprocess.run(cc + ["-I", str(td), str(td / "harness.c"),
                             str(td / "fdt_collect.c"), "-o", str(exe)],
                       check=True)

        def run(blob, step=None, timeout=10):
            p = td / "blob.dtb"; p.write_bytes(blob)
            env = dict(os.environ)
            if step:
                env["STUB_STEP"] = step
            t0 = time.time()
            r = subprocess.run([str(exe), str(p)], capture_output=True,
                               text=True, timeout=timeout, env=env)
            return r.stdout.strip(), time.time() - t0

        failures = []
        def check(name, cond, detail):
            print(f"{'PASS' if cond else 'FAIL'} {name}: {detail}")
            if not cond:
                failures.append(name)

        out, dt = run(valid_blob())
        check("valid-tree", "result=1" in out and "get=1" in out,
              f"{out} in {dt:.3f}s")
        out, dt = run(valid_blob(dummies=3000))
        kicks = int(out.split("kicks=")[1].split()[0]) if "kicks=" in out else 0
        check("large-tree-kicks", "result=1" in out and kicks >= 1,
              f"{out} in {dt:.3f}s (watchdog serviced during the pass)")
        out, dt = run(valid_blob(dummies=3000), step=BUDGET_STEP_FAST)
        check("over-budget-refuses", "result=1" not in out,
              f"{out} in {dt:.3f}s (budget fails closed)")
        for name, blob in hostile_cases().items():
            try:
                out, dt = run(blob)
                check(name, "result=1" not in out, f"{out} in {dt:.3f}s")
            except subprocess.TimeoutExpired:
                check(name, False, "TIMEOUT (hang)")
        print("FAILURES:", failures if failures else "none")
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
