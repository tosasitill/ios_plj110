#!/usr/bin/env python3
"""Offline regression: artwork-only ADT delta, projection, real C geometry."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import mkadt
import x9_boot_defaults
from shared import adt


class DisplayTests(unittest.TestCase):
    def test_default_adt_unchanged(self):
        base = adt.serialize(mkadt.build(seed=bytes(64)))
        self.assertEqual(base, adt.serialize(mkadt.build(seed=bytes(64), island=False)))

    def test_island_changes_only_subtype(self):
        base = mkadt.build(seed=bytes(64))
        island = mkadt.build(seed=bytes(64), island=True)
        product = next(n for n in island.children if n.prop('name') == b'product\0')
        self.assertEqual(product.prop('artwork-device-subtype'), struct.pack('<I', 2556))
        product.props = [(k, flags, struct.pack('<I', 2436) if k == 'artwork-device-subtype' else val)
                         for k, flags, val in product.props]
        self.assertEqual(adt.serialize(base), adt.serialize(island))

    def test_opt_in_projection(self):
        self.assertEqual(x9_boot_defaults.value([], 'x9_ui'), 1)
        self.assertEqual(x9_boot_defaults.value(['x9_ui=1'], 'x9_ui'), 1)
        self.assertEqual(x9_boot_defaults.value(['x9_ui=0'], 'x9_ui'), 0)
        # Packaging and kernel both select the FIRST duplicate, not the last.
        self.assertEqual(x9_boot_defaults.value(['x9_ui=0', 'x9_ui=1'], 'x9_ui'), 0)

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
    def test_shared_c_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'geometry.c'
            binary = Path(tmp) / 'geometry'
            source.write_text('''#include <assert.h>
#include "statusbar_geometry.h"
int main(void) {
    assert(fabs(x9_statusbar_offset(3, 1) * 3 - 50) < 1e-12);
    assert(x9_statusbar_offset(2, 1) == 25);
    assert(x9_statusbar_offset(3, 0) == 0);
    assert(x9_statusbar_offset(0, 1) == 0);
    assert(x9_statusbar_offset(NAN, 1) == 0);
    assert(x9_statusbar_offset(INFINITY, 1) == 0);
    for (int i=0; i<10000; i++) {
        double original_y = 0; /* Apple's reset EACH layout */
        double final_y = original_y + x9_statusbar_offset(3, 1);
        assert(fabs(final_y - 50.0/3.0) < 1e-12);
    }
    return 0;
}''')
            subprocess.run([os.environ.get('CC', shutil.which('cc')), '-Wall', '-Wextra', '-Werror',
                            '-I', str(HERE), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
