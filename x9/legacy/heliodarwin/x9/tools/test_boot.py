"""Offline tests; this does not emulate MT6993 display, UFS or LK firmware."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import bootimg
from bootimg import check_image, empty_cpio, pack, unpack
import mkadt
import mkpayload
import bootctl
from shared import adt, payload, load

stub = load('x9_test_stub', 'x676c/tools/stub/mkstub.py')
SHIM = HERE.parent / 'shim'


class ImportTests(unittest.TestCase):
    def test_shared_import_does_not_redirect_x9_modules(self):
        script = (
            'from pathlib import Path; import sys; '
            f'sys.path.insert(0, {str(HERE)!r}); '
            'before = sys.path[:]; import shared; assert sys.path == before; '
            'import mkadt; '
            f'assert Path(mkadt.__file__).resolve() == Path({str(HERE / "mkadt.py")!r})'
        )
        subprocess.run([sys.executable, '-I', '-c', script], check=True, cwd=HERE.parents[1])


@unittest.skipUnless(shutil.which('clang') and shutil.which('llvm-objdump'),
                     'needs clang and llvm-objdump')
class ShimEntryTests(unittest.TestCase):
    """Assemble start.S and pin the EL2 normalisation order in the machine code.

    This cannot execute on a host; it catches the ordering mistakes that only a
    phone boot would otherwise reveal.
    """

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='x9-entry-tests-')
        obj = Path(cls.work.name) / 'start.o'
        subprocess.run([shutil.which('clang'), '--target=aarch64-none-elf', '-march=armv8-a',
                        '-c', str(SHIM / 'start.S'), '-o', str(obj)], check=True)
        text = subprocess.check_output([shutil.which('llvm-objdump'), '-d', '--no-show-raw-insn',
                                        str(obj)], text=True)
        cls.code = []
        for line in text.splitlines():
            parts = line.split(None, 1)
            if len(parts) != 2 or not parts[0].endswith(':'):
                continue
            try:
                cls.code.append((int(parts[0][:-1], 16), parts[1].strip()))
            except ValueError:
                continue  # symbol header such as "0000000000000000 <_head>:"

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def at(self, mnemonic):
        return [addr for addr, text in self.code if text.lower().startswith(mnemonic.lower())]

    def test_el2_normalisation_order(self):
        hcr, vbar, eret = self.at('msr\tHCR_EL2'), self.at('msr\tVBAR_EL2'), self.at('eret')
        self.assertTrue(hcr and vbar and eret, 'EL2 entry sequence missing')
        # Under VHE, VBAR_EL2 aliases the EL1&0 vector base; clearing E2H first
        # is what makes the later write install real EL2 vectors.
        self.assertLess(hcr[0], vbar[0], 'HCR_EL2 must be written before VBAR_EL2')
        self.assertLess(vbar[0], eret[0])

    def test_gic_and_identification_registers_are_published_before_eret(self):
        eret = self.at('eret')[0]
        for reg in ('ICC_SRE_EL2', 'ICH_HCR_EL2', 'VPIDR_EL2', 'VMPIDR_EL2', 'CPTR_EL2'):
            addresses = self.at('msr\t' + reg)
            self.assertTrue(addresses, f'{reg} is never written')
            self.assertLess(addresses[0], eret, f'{reg} written after ERET')

    def test_cache_maintenance_runs_at_el1_not_el2(self):
        # DC CISW/IC IALLU are undefined at EL2 when HCR_EL2.TGE is clear, so
        # the flush must be reached through the EL1 path, after the ERET.
        eret = self.at('eret')[0]
        calls = [addr for addr, text in self.code
                 if text.lower().startswith('bl') and 'clean_dcache' in text]
        self.assertEqual(len(calls), 1, 'expected exactly one cache flush call site')
        self.assertGreater(calls[0], eret, 'cache flush is called before the ERET to EL1')


class PstoreLayoutTests(unittest.TestCase):
    """The console zone is not the carveout base; a wrong offset loses the log."""

    @staticmethod
    def console_zone(mem, console, ftrace, pmsg):
        records = mem - console - ftrace - pmsg
        return records

    def test_offset_reproduces_the_hardware_proven_x676c_zone(self):
        # x676c/README.md records this zone as read back from a real phone.
        self.assertEqual(self.console_zone(0xe0000, 0x40000, 0x1000, 0x10000), 0x8f000)
        self.assertEqual(0x48090000 + 0x8f000, 0x4811f000)

    def test_x9_zone_matches_the_shim_constant(self):
        # Live X9 ramoops parameters, read from /sys/module/ramoops/parameters.
        off = self.console_zone(0x310000, 0x200000, 0x1000, 0x100000)
        self.assertEqual(0xaec00000 + off, 0xaec0f000)
        self.assertEqual(off + 0x200000 + 0x1000 + 0x100000, 0x310000)
        cfg = (ROOT / 'x9/shim/config.h').read_text()
        self.assertIn('#define X9_PSTORE_BASE 0xaec0f000UL', cfg)
        self.assertIn('#define X9_PSTORE_SIZE 0x00200000UL', cfg)


class ADTTests(unittest.TestCase):
    def test_real_board_values_and_no_unported_devices(self):
        root = mkadt.build(seed=bytes(64))
        children = {c.name: c for c in root.children}
        self.assertIn(b'oppo,x9\0', root.prop('compatible'))
        self.assertNotIn(b'infinix,x676c', root.prop('compatible'))
        self.assertEqual(len(children['cpus'].children), 1)
        self.assertEqual(children['chosen'].prop('dram-size'), struct.pack('<Q', 0x20000000))
        self.assertEqual(children['sweet-fbcon'].prop('stride'), struct.pack('<I', 5120))
        self.assertNotIn('x676c-ufs', children)
        blob = adt.serialize(root)
        parsed, end = adt.parse(blob)
        self.assertEqual(end, len(blob))
        self.assertEqual(adt.serialize(parsed), blob)

    def test_bad_console_zone(self):
        for base, size in [(0xaec00000, 0), (0, 0x40000), (0xaec00001, 4096),
                           (0x4811f000, 0x40000), (0xaef10000, 4096)]:
            with self.assertRaises(ValueError):
                mkadt.build(console_base=base, console_size=size)


class ABTests(unittest.TestCase):
    @staticmethod
    def snapshot():
        import zlib
        b = bytearray(32)
        b[:4] = b'_b\0\0'
        struct.pack_into('<I', b, 4, 0x42414342)
        b[8], b[9] = 1, 2
        b[12], b[14] = 14, 0x8f
        struct.pack_into('<I', b, 28, zlib.crc32(b[:28]))
        return bytes(b)

    def test_one_try_preserves_successful_fallback(self):
        before = self.snapshot()
        after = bootctl.prepare(before, 'a')
        slots = bootctl.control.parse(after)
        self.assertEqual(slots[0], {'priority': 15, 'tries': 1, 'successful': 0})
        self.assertEqual(slots[1]['priority'], 14)
        self.assertEqual(slots[1]['successful'], 1)
        self.assertTrue(bootctl.control.crc_ok(after))
        self.assertEqual(before, self.snapshot())

    def test_missing_fallback_refused(self):
        with self.assertRaises(ValueError):
            bootctl.prepare(self.snapshot(), 'b')
        with self.assertRaises(ValueError):
            bootctl.prepare(bytes(32), 'a')

    def test_corrupt_fallback_refused(self):
        import zlib
        b = bytearray(self.snapshot())
        b[15] = 1
        struct.pack_into('<I', b, 28, zlib.crc32(b[:28]))
        with self.assertRaises(ValueError):
            bootctl.prepare(b, 'a')

    def test_new_candidate_clears_only_its_stale_verity_flag(self):
        import zlib
        b = bytearray(self.snapshot())
        b[13] = 0xa5
        b[15] = 0xa4
        struct.pack_into('<I', b, 28, zlib.crc32(b[:28]))
        after = bootctl.prepare(b, 'a')
        self.assertEqual(after[13], 0xa4)
        self.assertEqual(after[15], b[15])
        self.assertTrue(bootctl.control.crc_ok(after))


class ImageTests(unittest.TestCase):
    @staticmethod
    def image():
        b = bytearray(0x3000)
        b[:4] = b'MZ@\xfa'
        struct.pack_into('<I', b, 4, 0x14000000 | ((0x1000 - 4) // 4))
        struct.pack_into('<QQQ', b, 8, 0, len(b), 0xa)
        b[56:60] = b'ARM\x64'
        struct.pack_into('<I', b, 60, 0x40)
        b[0x40:0x44] = b'PE\0\0'
        struct.pack_into('<I', b, 0x40 + 24 + 16, 0x1000)
        b[0x1000:0x1004] = b'\x1f\x20\x03\xd5'
        return bytes(b)

    def test_boot_roundtrip(self):
        image = self.image()
        boot = pack(image)
        decoded, ramdisk, info = unpack(boot)
        self.assertEqual(decoded, image)
        self.assertEqual(ramdisk, empty_cpio())
        self.assertEqual(info['entry_offset'], '0x1000')
        self.assertEqual(info['header_version'], 4)
        self.assertEqual(info['header_size'], 1584)
        self.assertFalse(info['hardware_tested'])
        self.assertEqual(boot, pack(image))

    def test_header_matches_the_stock_v4_layout(self):
        boot = pack(self.image(), cmdline='console=null')
        magic, ks, rs, osv, hs = struct.unpack_from('<8sIIII', boot, 0)
        self.assertEqual((magic, osv, hs), (b'ANDROID!', 0, 1584))
        self.assertEqual(boot[24:40], bytes(16), 'reserved must stay zero')
        self.assertEqual(struct.unpack_from('<I', boot, 40)[0], 4)
        self.assertEqual(boot[44:44 + 13], b'console=null\0')
        self.assertEqual(boot[44 + 1536:1584], bytes(4), 'signature_size must be 0')
        self.assertEqual(boot[1584:4096], bytes(4096 - 1584), 'v4 padding is zero')
        self.assertEqual(len(boot), 4096 + bootimg.align(ks) + bootimg.align(rs))
        self.assertEqual(rs, len(empty_cpio()))

    def test_multiblock_lz4_roundtrip(self):
        b = bytearray(self.image()) + bytes((8 << 20) + 4096 - len(self.image()))
        struct.pack_into('<Q', b, 16, len(b))
        b[-16:] = bytes(range(16))
        self.assertEqual(unpack(pack(b))[0], bytes(b))

    def test_empty_ramdisk_refused(self):
        with self.assertRaises(ValueError):
            pack(self.image(), b'')

    def test_stock_branch_to_missing_trampoline_refused(self):
        b = bytearray(self.image())
        struct.pack_into('<I', b, 4, 0x14848419)
        with self.assertRaises(ValueError):
            check_image(b)

    def test_header_tamper_refused(self):
        b = bytearray(pack(self.image()))
        struct.pack_into('<I', b, 0x20, 0xa0200000)
        with self.assertRaises(ValueError):
            unpack(b)

    def test_non_v4_header_refused(self):
        b = bytearray(pack(self.image()))
        struct.pack_into('<I', b, 40, 0)
        with self.assertRaisesRegex(ValueError, 'header v4'):
            unpack(b)

    def test_kernel_tamper_refused(self):
        b = bytearray(pack(self.image()))
        b[0x1000] ^= 1
        with self.assertRaises(ValueError):
            unpack(b)


class PayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='x9-host-tests-')
        cls.directory = Path(cls.work.name)
        library = cls.directory / 'validate.dylib'
        subprocess.run([shutil.which('clang'), '-O2', '-shared', '-fPIC', '-Wall', '-Wextra',
                        '-Werror', str(SHIM / 'validate.c'), str(SHIM / 'fdt.c'), '-o', str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.payload_validate.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.payload_validate.restype = ctypes.c_char_p
        cls.lib.fdt_chosen_u64.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                          ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.fdt_chosen_u64.restype = ctypes.c_int
        kernel = cls.directory / 'stub.macho'
        kernel.write_bytes(stub.macho(bytes(0x2000)))
        tree = cls.directory / 'x9.adt'
        tree.write_bytes(adt.serialize(mkadt.build(seed=bytes(64))))
        cls.good, cls.info = payload.build(kernel, tree, 'cpus=1')

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def validate(self, blob):
        buf = ctypes.create_string_buffer(bytes(blob))
        dt, args = ctypes.c_uint64(), ctypes.c_uint64()
        error = self.lib.payload_validate(buf, len(blob), ctypes.byref(dt), ctypes.byref(args))
        return error, dt.value, args.value

    def test_shared_payload_format(self):
        error, dt, args = self.validate(self.good)
        self.assertIsNone(error)
        self.assertGreaterEqual(dt, 0x140000000)
        self.assertEqual(args % 0x4000, 0)
        self.assertLess(args + 0x4000, 0x160000000)

    def test_bad_payloads_refused_before_copy(self):
        for off, fmt, value in [(8, '<I', 9), (12, '<I', 129),
            (0x20, '<Q', len(self.good) + 8), (0x38, '<Q', 0x40000000),
            (0x40, '<Q', 0x40000000), (0x18, '<Q', 0),
            (0x2a8 + 16, '<Q', len(self.good) + 8)]:
            b = bytearray(self.good)
            struct.pack_into(fmt, b, off, value)
            self.assertIsNotNone(self.validate(b)[0], hex(off))
        self.assertIsNotNone(self.validate(self.good[:200])[0])

    def test_unterminated_command_line_refused(self):
        b = bytearray(self.good)
        b[0x2a7] = 1
        self.assertIsNotNone(self.validate(b)[0])

    def test_virtual_range_overflow_refused(self):
        b = bytearray(self.good)
        struct.pack_into('<Q', b, 0x10, 0xffffffffe0000000)
        self.assertIsNotNone(self.validate(b)[0])

    def test_old_kernel_without_board_isolation_refused(self):
        with self.assertRaisesRegex(ValueError, 'patch 0186'):
            mkpayload.build(self.directory / 'stub.macho', self.directory / 'x9.adt', 'cpus=1')

    def test_stub_is_explicit_and_cannot_claim_kernel_isolation(self):
        kernel = self.directory / 'marked-stub.macho'
        kernel.write_bytes(stub.macho(bytes(32) + mkpayload.STUB_TAG))
        tree = self.directory / 'x9.adt'
        blob, info = mkpayload.build(kernel, tree, 'cpus=1', loader_stub=True)
        self.assertIsNone(self.validate(blob)[0])
        self.assertEqual(info['kernel_kind'], 'loader-test-stub')
        self.assertFalse(info['x9_board_isolation_tag'])
        with self.assertRaisesRegex(ValueError, 'patch 0186'):
            mkpayload.build(kernel, tree, 'cpus=1')

    @staticmethod
    def fdt():
        def be(*values):
            return struct.pack('>%dI' % len(values), *values)
        strings = b'linux,initrd-start\0linux,initrd-end\0'
        start_name, end_name = 0, len(b'linux,initrd-start\0')
        body = be(1) + bytes(4) + be(1) + b'chosen\0\0'
        body += be(3, 8, start_name) + be(0, 0xa3800000)
        body += be(3, 8, end_name) + be(0, 0xa3810000)
        body += be(2, 2, 9)
        total = 40 + 16 + len(body) + len(strings)
        header = be(0xd00dfeed, total, 56, 56 + len(body), 40, 17, 16, 0, len(strings), len(body))
        return header + bytes(16) + body + strings

    def test_bounded_fdt_parser(self):
        fdt = self.fdt()
        buf = ctypes.create_string_buffer(fdt)
        value = ctypes.c_uint64()
        self.assertEqual(self.lib.fdt_chosen_u64(buf, b'linux,initrd-start', ctypes.byref(value)), 1)
        self.assertEqual(value.value, 0xa3800000)
        self.assertEqual(self.lib.fdt_chosen_u64(buf, b'absent', ctypes.byref(value)), 0)
        bad = bytearray(fdt)
        struct.pack_into('>I', bad, 8, len(fdt) + 4)
        buf = ctypes.create_string_buffer(bytes(bad))
        self.assertEqual(self.lib.fdt_chosen_u64(buf, b'linux,initrd-start', ctypes.byref(value)), 0)


if __name__ == '__main__':
    unittest.main()
