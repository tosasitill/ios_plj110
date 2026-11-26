#!/usr/bin/env python3
"""Host-only single-file header selection checks; no device access."""
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import settings_header as s

class HeaderTest(unittest.TestCase):
    def test_single_file_and_reject_existing_output(self):
        good = (s.PATH, b'diag', 0o755, bytes(32), None)
        other = ('/usr/lib/X9H4Transport.dylib', b'bridge', 0o755, bytes(32), None)
        with tempfile.TemporaryDirectory() as d, patch.object(s.root_header, 'load', return_value=([other, good], [])):
            out = Path(d) / 'header'
            s.generate(Path(d), Path(d), out)
            result = out.read_text()
            self.assertIn(s.PATH, result)
            self.assertNotIn(other[0], result)
            with self.assertRaises(ValueError): s.generate(Path(d), Path(d), out)
    def test_reject_duplicate_or_unsafe(self):
        good = (s.PATH, b'diag', 0o755, bytes(32), None)
        for entries in ([], [good, good], [(s.PATH, b'diag', 0o644, bytes(32), None)],
                        [(s.PATH, b'diag', 0o755, bytes(32), (1, bytes(32))) ]):
            with tempfile.TemporaryDirectory() as d, patch.object(s.root_header, 'load', return_value=(entries, [])):
                with self.assertRaises(ValueError): s.generate(Path(d), Path(d), Path(d)/'out')

    def test_replacement_pins_only_original_package_diagnostic(self):
        previous = b'old diagnostic'
        digest = hashlib.sha256(previous).digest()
        good = (s.PATH, previous, 0o755, digest, None)
        other = ('/usr/lib/X9H4Transport.dylib', b'bridge', 0o755, bytes(32), None)
        with tempfile.TemporaryDirectory() as d, patch.object(s.root_header, 'load', return_value=([other, good], [])):
            directory = Path(d)
            replacement = directory / 'new.dylib'
            replacement.write_bytes(b'new diagnostic')
            out = directory / 'header'
            s.generate(directory, directory, out, replacement)
            result = out.read_text()
            self.assertIn(f'{{{len(previous)}u,{s.root_header.c_bytes(digest)}}}', result)
            self.assertIn('x9_btr_p0,1u', result)
            self.assertNotIn(other[0], result)
            replacement.write_bytes(previous)
            with self.assertRaises(ValueError):
                s.generate(directory, directory, directory / 'same', replacement)

if __name__ == '__main__': unittest.main()
