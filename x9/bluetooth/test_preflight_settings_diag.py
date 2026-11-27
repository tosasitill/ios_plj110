#!/usr/bin/env python3
"""Host unit tests for strictly read-only single-file BT diag preflight."""
import hashlib
from pathlib import Path
import stat
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import preflight_settings_diag as pf
from stage_targeted import BLOCK, LiveView


class FakeHFS:
    def __init__(self, entries, clean_journal=True):
        self.entries = dict(entries)
        self.clean_journal = clean_journal
        self.journal_checked = False

    def require_clean_journal(self):
        self.journal_checked = True
        if not self.clean_journal:
            raise ValueError('dirty HFSX journal')

    def lookup(self, path, follow_symlinks=True):
        return self.entries.get(path)

    def read_file(self, path, max_bytes=1 << 20):
        e = self.entries.get(path)
        if e is None or 'bytes' not in e:
            raise ValueError('missing file')
        return e['bytes']


def _reg(data, mode=0o755, uid=0, gid=0):
    return {
        'type': 'file',
        'mode': stat.S_IFREG | mode,
        'uid': uid,
        'gid': gid,
        'size': len(data),
        'bytes': data,
    }


def _dir():
    return {'type': 'dir', 'mode': stat.S_IFDIR | 0o755, 'uid': 0, 'gid': 0}


class TestPreflightSettingsDiag(unittest.TestCase):
    def setUp(self):
        self.diag_bytes = b'X9SettingsBTDiag-fixture'
        self.btd_bytes = b'bluetoothd-fixture'
        self.bt_bytes = b'BlueTool-fixture'
        self.spec = {
            'diag': {
                'path': pf.DIAG_PATH,
                'size': len(self.diag_bytes),
                'mode': 0o755,
                'sha256': hashlib.sha256(self.diag_bytes).hexdigest(),
            },
            'requires': [
                {
                    'path': '/usr/sbin/bluetoothd',
                    'size': len(self.btd_bytes),
                    'sha256': hashlib.sha256(self.btd_bytes).hexdigest(),
                },
                {
                    'path': '/usr/sbin/BlueTool',
                    'size': len(self.bt_bytes),
                    'sha256': hashlib.sha256(self.bt_bytes).hexdigest(),
                },
            ],
            'manifest_checked': False,
        }
        self.base_entries = {
            '/usr': _dir(),
            '/usr/lib': _dir(),
            '/usr/sbin': _dir(),
            '/usr/sbin/bluetoothd': _reg(self.btd_bytes),
            '/usr/sbin/BlueTool': _reg(self.bt_bytes),
        }

    def test_install_ready_when_absent(self):
        hfs = FakeHFS(self.base_entries)
        res = pf.audit_hfs_target(hfs, self.spec, 'install')
        self.assertTrue(hfs.journal_checked)
        self.assertEqual(res['status'], 'install_ready')

    def test_install_idempotent_when_matched(self):
        entries = dict(self.base_entries)
        entries[pf.DIAG_PATH] = _reg(self.diag_bytes)
        res = pf.audit_hfs_target(FakeHFS(entries), self.spec, 'install')
        self.assertEqual(res['status'], 'already_installed')

    def test_temp_file_blocks_all_modes(self):
        for mode in ('verify', 'install', 'rollback'):
            entries = dict(self.base_entries)
            entries[pf.TEMP_PATH] = _reg(b'stale')
            with self.assertRaises(pf.PreflightError):
                pf.audit_hfs_target(FakeHFS(entries), self.spec, mode)

    def test_unknown_diag_or_bad_owner_fails_closed(self):
        entries = dict(self.base_entries)
        entries[pf.DIAG_PATH] = _reg(b'unknown-other-dylib')
        with self.assertRaises(pf.PreflightError):
            pf.audit_hfs_target(FakeHFS(entries), self.spec, 'install')

        entries[pf.DIAG_PATH] = _reg(self.diag_bytes, uid=501, gid=0)
        with self.assertRaises(pf.PreflightError):
            pf.audit_hfs_target(FakeHFS(entries), self.spec, 'rollback')

    def test_rollback_ready_and_absent(self):
        entries = dict(self.base_entries)
        entries[pf.DIAG_PATH] = _reg(self.diag_bytes)
        res = pf.audit_hfs_target(FakeHFS(entries), self.spec, 'rollback')
        self.assertEqual(res['status'], 'rollback_ready')
        res2 = pf.audit_hfs_target(FakeHFS(self.base_entries), self.spec, 'rollback')
        self.assertEqual(res2['status'], 'already_absent')

    def test_install_requires_real_sbin_directory(self):
        entries = dict(self.base_entries)
        entries['/usr/sbin'] = {'type': 'file', 'mode': stat.S_IFLNK | 0o755}
        with self.assertRaises(pf.PreflightError):
            pf.audit_hfs_target(FakeHFS(entries), self.spec, 'install')

    def test_live_budget_counts_aligned_reads(self):
        class ReadSession:
            total = 2 * BLOCK

            def read_global(self, offset, length):
                return bytes(length)

        reader = LiveView(ReadSession(), max_read_bytes=BLOCK)
        self.assertEqual(reader.read(1, 1), b'\0')
        self.assertEqual(reader.physical_read_bytes, BLOCK)
        with self.assertRaises(ValueError):
            reader.read(BLOCK + 1, 1)

    def test_readonly_budgeted_device_forbids_write_and_overread(self):
        dev = pf.ReadOnlyBudgetedDevice(lambda off, n: b'\x00' * n, max_bytes=16)
        self.assertEqual(dev.read(0, 8), b'\x00' * 8)
        with self.assertRaises(pf.PreflightError):
            dev.read(8, 16)
        with self.assertRaises(RuntimeError):
            dev.write(0, b'x')


if __name__ == '__main__':
    unittest.main()
