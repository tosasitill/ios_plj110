"""Offline checks of apfs_userdata: superblock parsing, iOS 16 allowlist, sparse plan."""
import struct
import tempfile
import unittest
from pathlib import Path

import apfs_userdata as au


def nxsb(blocks, desc_base=1, desc_blocks=8, incompat=2, features=0, block_size=4096):
    b = bytearray(4096)
    b[32:36] = b'NXSB'
    struct.pack_into('<IQQQQ', b, 36, block_size, blocks, features, 0, incompat)
    struct.pack_into('<IIQQ', b, 104, desc_blocks, 332, desc_base, desc_base + desc_blocks)
    return bytes(b)


def apsb(index, role, name, xid=2, features=0x12, incompat=0x8):
    b = bytearray(4096)
    b[32:36] = b'APSB'
    struct.pack_into('<Q', b, 16, xid)
    struct.pack_into('<I', b, 36, index)
    struct.pack_into('<QQQ', b, 40, features, 0, incompat)
    struct.pack_into('<H', b, 964, role)
    b[704:704 + len(name)] = name.encode()
    return bytes(b)


class ApfsUserdataTests(unittest.TestCase):
    def image(self, blocks, writes):
        tmp = tempfile.NamedTemporaryFile(delete=False)
        tmp.truncate(blocks * 4096)
        for block, data in writes:
            tmp.seek(block * 4096)
            tmp.write(data)
        tmp.close()
        self.addCleanup(Path(tmp.name).unlink)
        return Path(tmp.name)

    def test_superblock_parse_and_rejections(self):
        nx = au.parse_nx(nxsb(16384))
        self.assertEqual((nx['block_count'], nx['xp_desc_base'], nx['xp_desc_blocks']), (16384, 1, 8))
        for bad in [bytes(4096), nxsb(16384, block_size=16384), nxsb(16384, desc_blocks=0x80000008)]:
            with self.assertRaises(ValueError):
                au.parse_nx(bad)

    def test_allowlist_and_roles(self):
        nx = au.parse_nx(nxsb(16384))
        system = {'role': 1, 'name': 'System', 'features': 0x12, 'ro_compat': 0, 'incompat': 0x8}
        data = dict(system, role=0x40, name='Data', features=0x2)
        au.check_compat(nx, [system], False)
        au.check_compat(nx, [system, data], True)
        for volumes, data_role in [([system], True), ([system, data], False), ([data], False),
                                   ([dict(system, incompat=0x28)], False),     # sealed
                                   ([dict(system, incompat=0x9)], False),      # case-insensitive
                                   ([dict(system, ro_compat=1)], False),
                                   ([dict(system, features=0x40)], False)]:
            with self.assertRaises(ValueError):
                au.check_compat(nx, volumes, data_role)
        for bad_nx in [dict(nx, incompat=0x102), dict(nx, features=1), dict(nx, ro_compat=1)]:
            with self.assertRaises(ValueError):
                au.check_compat(bad_nx, [system], False)

    def test_sparse_plan_covers_block0_desc_ring_and_nonzero_chunks(self):
        blocks = 16384   # 64 MiB
        # Descriptor ring at blocks 70..77 (chunk 1); data at block 9000 (chunk 140).
        path = self.image(blocks, [(0, nxsb(blocks, desc_base=70)), (9000, apsb(0, 1, 'System'))])
        sha, chunks, nx = au.chunk_plan(path, blocks * 4096)
        self.assertEqual(chunks, [0, 256 * 1024, 9000 * 4096 // au.CHUNK * au.CHUNK])
        self.assertEqual(len(sha), 64)
        report = au.inspect(path, False)
        self.assertEqual([v['name'] for v in report['volumes']], ['System'])
        self.assertEqual(report['write_bytes'], 3 * au.CHUNK)

    def test_block_count_must_cover_image(self):
        path = self.image(16384, [(0, nxsb(8192))])
        with self.assertRaises(ValueError):
            au.chunk_plan(path, 16384 * 4096)

    def test_latest_volume_superblock_wins(self):
        blocks = {3: apsb(0, 1, 'System', xid=2), 4: apsb(0, 0x40, 'Renamed', xid=5)}
        vols = au.scan_volumes(lambda i: blocks.get(i, bytes(4096)), 8)
        self.assertEqual([(v['role'], v['name']) for v in vols], [(0x40, 'Renamed')])


if __name__ == '__main__':
    unittest.main()
