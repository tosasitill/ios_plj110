#!/usr/bin/env python3
"""Host tests for x9_platform_serial (SweetX9USB.cpp).

The getter is IOKit C++ and cannot be compiled on the host, so this test
combines (a) source assertions that pin the production text of the two
paths that failed on device in the wake2 round, and (b) a C harness that
mirrors the device-tree branch expression-for-expression and asserts its
behaviour on the byte shapes this port actually emits. The mirror is
pinned by (a): if the production expressions change, the assertions fail
and the mirror must be updated with them.

On-device failure being pinned (2026-11-28): mode 3 enumerated with the
synthetic serial "X9USBLOG1" because both getter paths missed:
  * IOPlatformSerialNumber is published by XNU on the platform expert's
    provider (the DT root nub), not on the platform expert object;
  * the DT serial-number property is the ASCII serial NUL-padded to
    exactly 32 bytes (product_identity.serial_property), which a strict
    length < 32 gate rejects unconditionally.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = (HERE.parent / "kernel" / "src" / "iokit" / "Kernel" / "arm" /
       "SweetX9USB.cpp").read_text()

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirror of the device-tree branch + validation tail of
 * x9_platform_serial (SweetX9USB.cpp), with OSData replaced by a raw
 * buffer. Return 0 and the string on success, nonzero on rejection. */
static int parse_dt_serial(const uint8_t *bytes, unsigned length,
                           char *out, unsigned capacity)
{
    char value[32] = {};
    if (bytes && length && length <= sizeof(value)) {
        unsigned n = 0;
        while (n < length && bytes[n]) n++;
        if (n && n < sizeof(value)) { memcpy(value, bytes, n); value[n] = 0; }
    }
    unsigned n = 0;
    while (value[n]) {
        if ((uint8_t)value[n] < 0x20 || (uint8_t)value[n] > 0x7e) return 2;
        n++;
    }
    if (!n || n + 1 > capacity) return 3;
    memcpy(out, value, n + 1);
    return 0;
}

int main(void)
{
    char out[64];
    /* The real unit's shape: 'ST0RMYN0E' NUL-padded to exactly 32. */
    uint8_t padded[32] = {};
    memcpy(padded, "ST0RMYN0E", 9);
    if (parse_dt_serial(padded, 32, out, sizeof(out)) != 0) return 11;
    if (strcmp(out, "ST0RMYN0E") != 0) return 12;
    /* 32 bytes with no NUL at all must be rejected, not truncated. */
    uint8_t unterminated[32];
    memset(unterminated, 'A', sizeof(unterminated));
    if (parse_dt_serial(unterminated, 32, out, sizeof(out)) == 0) return 13;
    /* A short NUL-terminated property is accepted as before. */
    if (parse_dt_serial((const uint8_t *)"ABC123\0", 7, out, sizeof(out)) != 0)
        return 14;
    if (strcmp(out, "ABC123") != 0) return 15;
    /* Empty property and non-ASCII content are rejected. */
    if (parse_dt_serial((const uint8_t *)"", 0, out, sizeof(out)) == 0) return 16;
    uint8_t bad[4] = { 'A', 0x01, 0, 0 };
    if (parse_dt_serial(bad, 4, out, sizeof(out)) == 0) return 17;
    printf("platform-serial harness OK\n");
    return 0;
}
"""


class TestPlatformSerialSource(unittest.TestCase):
    def test_provider_is_consulted_for_platform_serial(self):
        self.assertIn('platform->getProvider()->getProperty(', SRC)
        self.assertIn('"IOPlatformSerialNumber"', SRC)

    def test_dt_gate_accepts_exactly_32_byte_property(self):
        self.assertIn('data->getLength() <= sizeof(value)', SRC)
        # The strict gate must not survive anywhere in the DT branch.
        branch = SRC.split('IODeviceTree:/')[1].split('entry->release()')[0]
        self.assertNotIn('data->getLength() < sizeof(value)', branch)

    def test_dt_scan_still_bounds_the_copy(self):
        self.assertRegex(SRC, re.compile(
            r'while \(n < data->getLength\(\) && bytes\[n\]\) n\+\+;'))
        self.assertIn('if (n && n < sizeof(value))', SRC)


class TestPlatformSerialBehaviour(unittest.TestCase):
    def test_dt_branch_on_real_byte_shapes(self):
        compiler = shutil.which("cc") or shutil.which("clang")
        self.assertIsNotNone(compiler, "no host C compiler on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "harness.c"
            exe = Path(tmp) / "harness"
            src.write_text(HARNESS)
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 str(src), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0,
                             f"harness rc={run.returncode} out={run.stdout}")
            self.assertIn("OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
