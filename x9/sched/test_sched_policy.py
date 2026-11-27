#!/usr/bin/env python3
"""Host tests for the scheduler policy v2 header (x9_sched_policy.h).

Compiles the production header into a small harness and asserts the
policy decisions (OPP selection, thermal cap, scene classification,
per-domain commit rules). Run with the repo's host-test toolchain
(system clang + MacOSX26.5 SDKROOT on this Mac)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEV = HERE.parent / "kernel" / "src" / "bsd" / "dev"


class TestSchedPolicy(unittest.TestCase):
    def test_policy_behavior(self):
        compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("cc")
        self.assertIsNotNone(compiler, "no host C++ compiler on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "harness"
            build = subprocess.run(
                [compiler, "-std=gnu++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(DEV), "-x", "c++",
                 str(HERE / "test_sched_policy_harness.c"),
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
