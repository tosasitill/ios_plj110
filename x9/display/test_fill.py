#!/usr/bin/env python3
"""Offline fill candidate: logical ADT/API invariants and real kernel helpers."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ARM = HERE.parent / 'kernel/src/iokit/Kernel/arm'
sys.path.insert(0, str(HERE.parent / 'tools'))
import mkadt
import x9_boot_defaults


class FillTests(unittest.TestCase):
    def test_shared_geometry_matches_adt(self):
        text = (ARM / 'X9DisplayGeometry.h').read_text()
        macros = dict((k, int(v)) for k, v in re.findall(r'#define (X9_(?:PANEL|CANVAS)_[WH]) (\d+)u', text))
        self.assertEqual((macros['X9_CANVAS_W'], macros['X9_CANVAS_H']), (mkadt.CANVAS_W, mkadt.CANVAS_H))
        self.assertEqual((macros['X9_PANEL_W'], macros['X9_PANEL_H']), (mkadt.FB_W, mkadt.FB_H))
        self.assertEqual((mkadt.CANVAS_W / 3, mkadt.CANVAS_H / 3), (414, 920))
        # This switch is deliberately not part of the ADT/MG identity.
        self.assertNotIn('x9_fill', (HERE.parent / 'tools/product_identity.py').read_text())

    def test_default_off_and_independent_ui(self):
        self.assertEqual(x9_boot_defaults.value([], 'x9_fill'), 0)
        self.assertEqual(x9_boot_defaults.value(['x9_fill=1'], 'x9_fill'), 1)
        self.assertEqual(x9_boot_defaults.value(['x9_fill=0', 'x9_fill=1'], 'x9_fill'), 0)
        self.assertEqual(x9_boot_defaults.value(['x9_fill=1'], 'x9_ui'), 0)

    def test_real_cpp_helpers(self):
        compiler = os.environ.get('CXX', shutil.which('c++'))
        self.assertTrue(compiler, 'host C++ compiler required')
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'test_fill'
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('X9_TEST_SANITIZE') == '1' else []
            subprocess.run([compiler, '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror',
                            *shlex.split(os.environ.get('CXXFLAGS', '')), *flags,
                            str(HERE / 'test_fill.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
