#!/usr/bin/env python3
"""Host tests for the stock-mode (x9_haptic=3) event engine.

Compiles the production header x9_haptic_stock.h (plus the shipped stock
waves) into a small harness and drives synthetic PCM through it. Run with
the repo's host-test toolchain; on this Mac that means the c++ SDK shim
first on PATH (see AGENTS.md)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEV = HERE.parent / "kernel" / "src" / "bsd" / "dev"


class TestHapticStock(unittest.TestCase):
    def test_engine_behavior(self):
        compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("cc")
        self.assertIsNotNone(compiler, "no host C++ compiler on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "harness"
            build = subprocess.run(
                [compiler, "-std=gnu++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(DEV), "-x", "c++", str(HERE / "test_haptic_stock_harness.c"),
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
