#!/usr/bin/env python3
"""Verify derived diagnostic boot options without building or touching devices."""
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import settings_boot as settings


class BootSettings(unittest.TestCase):
    def setUp(self):
        self.base = json.loads((settings.ROOT / 'x9/build/boot-device.json').read_text())

    def test_modes_preserve_unrelated_options(self):
        configs = settings.derive(self.base)
        for mode, root, diag in (('verify', 0, 1), ('install', 1, 1), ('rollback', 2, 0)):
            opts = configs[mode]
            self.assertEqual(opts['options'], self.base['options'])
            args = opts['boot_args']
            self.assertEqual(args[-2:], [f'x9_bt_root={root}', f'x9_bt_settings_diag={diag}'])
            self.assertLess(len(' '.join(args).encode()), settings.payload.BOOT_LINE_LENGTH)
            if mode == 'rollback':
                self.assertIn('x9_bt_init=0', args)
                self.assertIn('x9_bt_fc=0', args)
            else:
                self.assertIn('x9_bt_init=1', args)
                self.assertIn('x9_bt_fc=1', args)
            changed = {'x9pl', 'x9sl', 'x9_bt_init', 'x9_bt_fc'}
            self.assertEqual([t for t in args[:-2] if t.partition('=')[0] not in changed],
                             [t for t in self.base['boot_args'] if t.partition('=')[0] not in changed])
        self.assertIn('x9pl=1', self.base['boot_args'])

    def test_fails_closed_if_cannot_fit_or_mode_is_preconfigured(self):
        with patch.object(settings, 'board_defaults', return_value={'x9pl': 0, 'x9sl': 0}):
            with self.assertRaisesRegex(ValueError, 'exceeds'):
                settings.derive(self.base)
        for value in ('x9_bt_root=1', 'x9_bt_settings_diag=0', 'x9_bt_init=0'):
            with self.assertRaisesRegex(ValueError, 'preconfigured selector'):
                settings.derive(dict(self.base, boot_args=[*self.base['boot_args'], value]))


if __name__ == '__main__':
    unittest.main()
