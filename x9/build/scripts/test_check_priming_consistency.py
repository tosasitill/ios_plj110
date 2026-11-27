#!/usr/bin/env python3
"""Host tests for check-priming-consistency.py (the rev287 guard).

Synthetic bundles/sources only; the checker was additionally validated by
hand against the shipped rev286 (pass + warning) and rev287 (fail)
bundles, which are the ground truth for the marker bytes.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CHECKER = Path(__file__).with_name('check-priming-consistency.py')
MARKER = b'x9-prime: pass'


def run(*args):
    return subprocess.run([sys.executable, '-I', str(CHECKER), *args],
                          text=True, capture_output=True)


def source_dir(root: Path, *, code: bool, inventory: bool) -> Path:
    if code:
        (root / 'X9MetalPriming.inc').write_text('// priming\n')
        (root / 'NativeBridgeMetalFrontend.mm').write_text(
            '#include "X9MetalPriming.inc"\n')
    else:
        (root / 'NativeBridgeMetalFrontend.mm').write_text('// frontend\n')
    if inventory:
        (root / 'X9GeneralMetal.primelist').write_text('/x/default.metallib\n')
    return root


def bundle_dir(root: Path, *, code: bool, inventory: bool) -> Path:
    (root / 'X9GeneralMetal').write_bytes(
        b'\xcf\xfa\xed\xfe' + (MARKER if code else b'no priming here'))
    if inventory:
        (root / 'X9GeneralMetal.primelist').write_text('/x/default.metallib\n')
    return root


class PrimingConsistencyTest(unittest.TestCase):
    def test_source_inventory_without_code_fails(self):
        with tempfile.TemporaryDirectory() as td:
            d = source_dir(Path(td), code=False, inventory=True)
            self.assertEqual(run('--source', str(d)).returncode, 1)

    def test_source_both_present_passes(self):
        with tempfile.TemporaryDirectory() as td:
            d = source_dir(Path(td), code=True, inventory=True)
            self.assertEqual(run('--source', str(d)).returncode, 0)

    def test_source_code_without_inventory_passes(self):
        with tempfile.TemporaryDirectory() as td:
            d = source_dir(Path(td), code=True, inventory=False)
            self.assertEqual(run('--source', str(d)).returncode, 0)

    def test_source_neither_passes(self):
        with tempfile.TemporaryDirectory() as td:
            d = source_dir(Path(td), code=False, inventory=False)
            self.assertEqual(run('--source', str(d)).returncode, 0)

    def test_source_inc_present_but_not_included_fails(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / 'X9MetalPriming.inc').write_text('// priming\n')
            (d / 'NativeBridgeMetalFrontend.mm').write_text('// frontend\n')
            (d / 'X9GeneralMetal.primelist').write_text('/x/default.metallib\n')
            self.assertEqual(run('--source', str(d)).returncode, 1)

    def test_bundle_inventory_without_code_fails(self):
        with tempfile.TemporaryDirectory() as td:
            d = bundle_dir(Path(td), code=False, inventory=True)
            result = run('--bundle', str(d))
            self.assertEqual(result.returncode, 1)
            self.assertIn('no priming code', result.stderr)

    def test_bundle_both_present_passes(self):
        with tempfile.TemporaryDirectory() as td:
            d = bundle_dir(Path(td), code=True, inventory=True)
            self.assertEqual(run('--bundle', str(d)).returncode, 0)

    def test_bundle_code_without_inventory_warns_but_passes(self):
        with tempfile.TemporaryDirectory() as td:
            d = bundle_dir(Path(td), code=True, inventory=False)
            result = run('--bundle', str(d))
            self.assertEqual(result.returncode, 0)
            self.assertIn('WARNING', result.stderr)

    def test_bundle_missing_binary_fails(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(run('--bundle', td).returncode, 1)


if __name__ == '__main__':
    unittest.main()
