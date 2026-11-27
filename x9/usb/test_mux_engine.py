#!/usr/bin/env python3
"""Host tests for the usbmux protocol engine (x9_usb_mux.h).

Compiles the production header into a scripted harness that plays both
the usbmuxd host and the local loopback services, asserting the wire
behaviour the real daemon requires (byte-exact version reply, SYN/ACK
fields, ACK-only data, window pacing, RST refusal/teardown, stream
reassembly, version-1 framing). Run with the repo's host-test toolchain
(system clang + MacOSX26.5 SDKROOT on this Mac).
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEV = HERE.parent / "kernel" / "src" / "bsd" / "dev"


class TestMuxEngine(unittest.TestCase):
    def test_mux_engine_behavior(self):
        compiler = shutil.which("cc") or shutil.which("clang")
        self.assertIsNotNone(compiler, "no host C compiler on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "harness"
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(DEV),
                 str(HERE / "test_mux_engine.c"),
                 "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0,
                             f"harness build failed:\n{build.stdout}\n{build.stderr}")
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0,
                             f"harness failed:\n{run.stdout}\n{run.stderr}")
            self.assertIn("RESULT PASS", run.stdout)


if __name__ == "__main__":
    unittest.main()
