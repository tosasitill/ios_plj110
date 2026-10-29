"""Exercise the opt-in pmsg path with real kernel C and mocked mappings."""
import binascii
import ctypes
from pathlib import Path
import shutil
import subprocess
import unittest
import test_kernel as kernel
MOCK, ROOT = kernel.MOCK, kernel.ROOT
from decode_boot_journal import analyze


class KernelJournalTests(unittest.TestCase):
    fixture = kernel.KernelPatchTests.fixture

    @classmethod
    def setUpClass(cls):
        kernel.KernelPatchTests.setUpClass.__func__(cls)
        mock = MOCK.replace('static unsigned maps;',
                            'static unsigned maps;\nstatic uint32_t pmsg[0x100000 / 4];')
        old = 'uintptr_t ml_io_map_wcomb(uintptr_t pa, size_t size)\n{'
        new = old + '''
    if (pa == 0xaee0c000 && size == 0x84000) {
        maps++;
        return (uintptr_t)pmsg - 0x3000;
    }
'''
        assert mock.count(old) == 1
        mock = mock.replace(old, new)
        mock += r'''
void fixture_pmsg(unsigned size, unsigned invalid) {
    pmsg[0] = invalid ? 0 : 0x43474244;
    pmsg[1] = pmsg[2] = size;
    memset((char *)pmsg + 12, 'S', size < 64 ? size : 64);
    if (size && size <= 64) ((char *)pmsg)[12 + size - 1] = '\n';
}
void *fixture_pmsg_data(void) { return (char *)pmsg + 12; }
uint32_t fixture_pmsg_size(void) { return pmsg[2]; }
uint32_t fixture_pmsg_start(void) { return pmsg[1]; }
'''
        (cls.directory / 'mock-journal.c').write_text(mock)
        subprocess.run([shutil.which('clang'), '-std=gnu11', '-O2', '-shared', '-fPIC',
                        '-Wall', '-Wextra', '-Werror', '-DSWEET_X9_DIAG_PMSG=1',
                        '-I', str(cls.include), str(cls.directory / 'sweet_early_host.c'),
                        str(cls.directory / 'mock-journal.c'), '-o', str(cls.library)],
                       check=True, cwd=ROOT)

    @classmethod
    def tearDownClass(cls):
        kernel.KernelPatchTests.tearDownClass.__func__(cls)

    def journal_fixture(self, **kwargs):
        lib = self.fixture(base=0xaec0f000, size=0x200000, **kwargs)
        lib.fixture_pmsg_data.restype = ctypes.c_void_p
        return lib

    def read(self, lib):
        return ctypes.string_at(lib.fixture_pmsg_data(), lib.fixture_pmsg_size())

    def test_adopts_shim_record_and_persists_late_milestones_with_crc(self):
        lib = self.journal_fixture()
        lib.fixture_pmsg(32, 0)
        lib.fixture_start()
        self.assertEqual(self.read(lib)[:32], b'S' * 31 + b'\n')
        lib.sweet_early_disable()
        lib.sweet_bootstrap_mark(b'bsd_init')
        result = analyze([('fixture', self.read(lib))])
        self.assertFalse(result['rejected'])
        stages = [s['stage'] for s in result['samples']]
        self.assertEqual(stages, [21] * 5 + [80] * 5)
        late = result['samples'][-1]
        self.assertTrue(late['early_disabled'] and late['console_mapped'] and late['board_x9'])

    def test_invalid_header_is_not_erased_or_adopted(self):
        lib = self.journal_fixture()
        lib.fixture_pmsg(32, 1)
        lib.fixture_start()
        lib.sweet_bootstrap_mark(b'bsd_init')
        self.assertEqual(lib.fixture_pmsg_size(), 32)
        self.assertEqual(self.read(lib), b'S' * 31 + b'\n')

    def test_near_capacity_never_emits_partial_crc_records(self):
        lib = self.journal_fixture()
        size = 0x80000 - 12 - 10
        lib.fixture_pmsg(size, 0)
        lib.fixture_start()
        self.assertEqual(lib.fixture_pmsg_size(), size)
        lib.sweet_bootstrap_mark(b'bsd_init')
        self.assertEqual(lib.fixture_pmsg_size(), size)

    def test_budget_exhaustion_does_not_wrap_the_larger_pmsg_ring(self):
        lib = self.journal_fixture()
        cap = 0x80000 - 12
        lib.fixture_pmsg(cap - 75, 0)
        lib.fixture_start()  # exactly three complete 25-byte records fit
        self.assertEqual(lib.fixture_pmsg_size(), cap)
        self.assertEqual(lib.fixture_pmsg_start(), cap)
        records = analyze([('tail', self.read(lib)[-75:])])['samples']
        self.assertEqual([r['stage'] for r in records], [21, 21, 21])

    def test_non_x9_never_maps_or_writes_the_pmsg_zone(self):
        lib = self.journal_fixture(compatible=b'other,board\0')
        lib.fixture_pmsg(32, 0)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 0)
        self.assertEqual(self.read(lib), b'S' * 31 + b'\n')

    def test_wrong_console_geometry_never_maps_pmsg(self):
        lib = self.fixture(base=0xaec10000, size=0x10000)
        lib.fixture_pmsg_data.restype = ctypes.c_void_p
        lib.fixture_pmsg(32, 0)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 2)  # watchdog plus console, no pmsg
        self.assertEqual(self.read(lib), b'S' * 31 + b'\n')

    def test_crc_rejects_every_single_bit_change_to_a_complete_record(self):
        prefix = b'x9m:80:00000007'
        line = prefix + b':' + f'{binascii.crc32(prefix):08x}'.encode()
        self.assertEqual(len(analyze([('clean', line)])['samples']), 1)
        for index in range(len(line)):
            for bit in range(8):
                damaged = bytearray(line)
                damaged[index] ^= 1 << bit
                self.assertFalse(analyze([('damaged', bytes(damaged))])['samples'])

    def test_cascade_parser_preserves_phase_and_rejects_bad_crc(self):
        prefix = b'q480105:00000011'
        line = prefix + b':' + f'{binascii.crc32(prefix):08x}'.encode()
        result = analyze([('fixture', line)])['samples'][0]
        self.assertEqual((result['blender'], result['phase'], result['value']),
                         (6, 'post_frames', '0x00000011'))
        self.assertFalse(analyze([('bad', line[:-1] + b'z')])['samples'])


if __name__ == '__main__':
    unittest.main()
