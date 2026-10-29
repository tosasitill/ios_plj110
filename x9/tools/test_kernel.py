"""Host fixtures for patch 0186; mocks are not a kernel or MMIO emulation."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FILES = ('osfmk/arm64/sweet_early.c', 'osfmk/arm64/sweet_early.h',
         'iokit/Kernel/arm/SweetBoard.h', 'osfmk/arm64/sweet_diag_record.h')

BOARD_CONFIG = '''#pragma once
#define SWEET 1
#define SWEET_EARLY_BLOCK_SIZE 0x2000000ULL
#define SWEET_PSTORE_PA 0x4811f000ULL
#define SWEET_PSTORE_SIZE 0x40000u
'''
BOOT_HEADER = '''#pragma once
#include <stdint.h>
typedef struct Boot_Video {
    unsigned long v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
} Boot_Video;
typedef struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    Boot_Video Video;
} boot_args;
#define kBootVideoDepthMask 0xff
'''
DT_HEADER = '''#pragma once
typedef void *DTEntry;
#define kSuccess 0
int SecureDTLookupEntry(DTEntry, const char *, DTEntry *);
int SecureDTGetProperty(DTEntry, const char *, const void **, unsigned int *);
'''
MOCK = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/device_tree.h>
#include <arm64/sweet_early.h>

unsigned char iso_font[256 * 16];
uint32_t fbcon_shadow[SWEET_FBCON_MAX_PIXELS];
static const char *compat;
static unsigned compat_len;
static uint64_t console_pa;
static uint32_t console_size;
static uint32_t console[0x40000 / 4];
static uint32_t watchdog[0x4000 / 4];
static unsigned maps;
static int malformed_base, malformed_size;

void _doprnt(const char *fmt, va_list *args, void (*put)(char), int radix)
{
    (void)args; (void)radix;
    while (*fmt) put(*fmt++);
}
int SecureDTLookupEntry(DTEntry start, const char *path, DTEntry *entry)
{
    (void)start;
    if (!strcmp(path, "/")) { *entry = (void *)1; return 0; }
    if (!strcmp(path, "/chosen")) { *entry = (void *)2; return 0; }
    return -1;
}
int SecureDTGetProperty(DTEntry entry, const char *name, const void **out, unsigned *size)
{
    if (entry == (void *)1 && !strcmp(name, "compatible")) {
        *out = compat; *size = compat_len; return 0;
    }
    if (entry == (void *)2 && !strcmp(name, "x9-console-base") && console_pa) {
        *out = &console_pa; *size = malformed_base ? 4 : 8; return 0;
    }
    if (entry == (void *)2 && !strcmp(name, "x9-console-size") && console_size) {
        *out = &console_size; *size = malformed_size ? 8 : 4; return 0;
    }
    return -1;
}
uintptr_t ml_io_map(uintptr_t pa, size_t size)
{
    if (pa != 0x1c010000 || size != 0x1000) return 0;
    maps++;
    return (uintptr_t)watchdog;
}
uintptr_t ml_io_map_wcomb(uintptr_t pa, size_t size)
{
    if (pa != (console_pa & ~0x3fffULL) || (pa & 0x3fff) ||
        (size & 0x3fff) || size < console_size + (console_pa - pa)) return 0;
    maps++;
    return (uintptr_t)console - (console_pa - pa);
}
void fixture_init(const char *list, unsigned length, uint64_t pa, uint32_t size,
    unsigned bad_base, unsigned bad_size, unsigned prefill)
{
    compat = list; compat_len = length; console_pa = pa; console_size = size;
    malformed_base = bad_base; malformed_size = bad_size;
    if (prefill && size >= 16) {
        console[0] = 0x43474244; console[1] = console[2] = prefill;
        memset((char *)console + 12, 'S', prefill);
    }
}
void fixture_start(void)
{
    boot_args args = {0};
    args.physBase = 0x140000000; args.memSize = 0x20000000;
    sweet_early_mark(1, "before ADT");
    sweet_early_init(&args);
    sweet_early_late_init();
}
uint32_t fixture_maps(void) { return maps; }
uint32_t fixture_watchdog(void) { return watchdog[2]; }
uint32_t fixture_header(unsigned field) { return field < 3 ? console[field] : 0; }
void *fixture_console(void) { return (char *)console + 12; }
int fixture_layout(uint64_t pa, uint64_t size, uint64_t w, uint64_t h, uint64_t stride)
{
    return sweet_fb_layout_valid(pa, size, w, h, stride);
}
'''


class KernelPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='x9-kernel-host-')
        cls.directory = Path(cls.work.name)
        tree = cls.directory / 'tree'
        tree.mkdir()
        for patch in sorted((ROOT / 'xnu/patches').glob('*.patch')):
            if int(patch.name[:4]) > 190:
                continue
            text = patch.read_bytes()
            if not any(f'diff --git a/{name} b/{name}'.encode() in text for name in FILES):
                continue
            subprocess.run(['git', '-C', str(tree), 'apply',
                            *[f'--include={name}' for name in FILES], str(patch)],
                           check=True, capture_output=True)
        cls.tree = tree
        include = cls.directory / 'include'
        headers = {
            'arm64/sweet_early.h': (tree / FILES[1]).read_text(),
            'arm64/sweet_diag_record.h': (tree / FILES[3]).read_text(),
            'pexpert/arm64/board_config.h': BOARD_CONFIG,
            'pexpert/arm64/boot.h': BOOT_HEADER,
            'pexpert/device_tree.h': DT_HEADER,
            'kern/misc_protos.h': '#include <stddef.h>\n#include <stdarg.h>\n'
                'void _doprnt(const char *, va_list *, void (*)(char), int);\n',
            'machine/machine_routines.h': '#include <stddef.h>\n#include <stdint.h>\n'
                'typedef uintptr_t vm_offset_t;\ntypedef size_t vm_size_t;\n'
                'uintptr_t ml_io_map(uintptr_t, size_t);\n'
                'uintptr_t ml_io_map_wcomb(uintptr_t, size_t);\n',
            'mach/vm_param.h': '#define PAGE_MASK 0x3fffULL\n'
                '#define round_page(x) (((x) + PAGE_MASK) & ~PAGE_MASK)\n',
        }
        for name, data in headers.items():
            path = include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
        # A host fixture cannot execute ARM barriers. No other source is changed.
        c = (tree / FILES[0]).read_text()
        c = c.replace('__asm__ volatile("dsb sy" ::: "memory");',
                      '__asm__ volatile("" ::: "memory");')
        source = cls.directory / 'sweet_early_host.c'
        source.write_text(c)
        mock = cls.directory / 'mock.c'
        mock.write_text(MOCK)
        cls.library = cls.directory / 'fixture.dylib'
        subprocess.run([shutil.which('clang'), '-std=gnu11', '-O2', '-shared', '-fPIC',
                        '-Wall', '-Wextra', '-Werror', '-I', str(include),
                        str(source), str(mock), '-o', str(cls.library)], check=True, cwd=ROOT)
        cls.include = include

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def fixture(self, compatible=b'mediatek,MT6993\0', base=0,
                size=0, bad_base=0, bad_size=0, prefill=0):
        # A separate dlopen instance gives each fixture fresh kernel globals.
        path = self.directory / f'fixture-{self.id().split(".")[-1]}.dylib'
        shutil.copyfile(self.library, path)
        lib = ctypes.CDLL(str(path))
        lib.fixture_init.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint64,
                                     ctypes.c_uint32, ctypes.c_uint, ctypes.c_uint,
                                     ctypes.c_uint]
        lib.fixture_layout.argtypes = [ctypes.c_uint64] * 5
        lib.fixture_console.restype = ctypes.c_void_p
        lib.sweet_pstore_append.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.fixture_init(compatible, len(compatible), base, size, bad_base, bad_size, prefill)
        return lib

    def test_before_init_has_no_physical_framebuffer_or_pstore(self):
        lib = self.fixture()
        lib.sweet_early_mark(1, b'before ADT')
        lib.sweet_early_putc(ord('A'))
        self.assertEqual(lib.fixture_maps(), 0)
        self.assertEqual(lib.sweet_early_fb_block(), 0)

    def test_x9_no_console_only_maps_its_watchdog(self):
        lib = self.fixture()
        lib.fixture_start()
        self.assertEqual(lib.sweet_early_is_x9(), 1)
        self.assertEqual(lib.sweet_early_is_x676c(), 0)
        self.assertEqual(lib.fixture_maps(), 1)
        self.assertEqual(lib.fixture_watchdog(), 0x1971)
        lib.sweet_watchdog_pet()
        self.assertEqual(lib.fixture_watchdog(), 0x1971)

    def test_exact_console_preserves_shim_prefix_and_clamps_crossing_append(self):
        lib = self.fixture(base=0xaec00000, size=128, prefill=4)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 2)
        before = lib.fixture_header(2)
        head = ctypes.string_at(lib.fixture_console(), before)
        self.assertTrue(head.startswith(b'SSSS'))
        self.assertIn(b'HelioDarwin-X9-0186\n', head)
        lib.sweet_pstore_append(b'X' * 300, 300)
        self.assertEqual(lib.fixture_header(2), 116)
        self.assertEqual(lib.fixture_header(1), 0)
        full = ctypes.string_at(lib.fixture_console(), 116)
        self.assertEqual(full[:before], head)
        lib.sweet_pstore_append(b'overwrite', 9)
        self.assertEqual(ctypes.string_at(lib.fixture_console(), 116), full)

    def test_unaligned_console_request_reserves_every_16k_page(self):
        lib = self.fixture(base=0xaec01000, size=0x40000)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 2)
        self.assertGreater(lib.fixture_header(2), 0)

    def test_invalid_console_size_property_never_maps_console(self):
        lib = self.fixture(base=0xaec00000, size=128, bad_size=1)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 1)

    def test_invalid_console_base_property_never_maps_console(self):
        lib = self.fixture(base=0xaec00000, size=128, bad_base=1)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 1)

    def test_invalid_console_bounds_never_map_legacy_zone(self):
        lib = self.fixture(base=0x4811f000, size=0x40000)
        lib.fixture_start()
        self.assertEqual(lib.fixture_maps(), 1)

    def test_bounded_board_match_rejects_unterminated_identity(self):
        lib = self.fixture(compatible=b'mediatek,MT6993')
        lib.fixture_start()
        self.assertEqual(lib.sweet_early_is_x9(), 0)
        self.assertEqual(lib.fixture_maps(), 0)

    def test_conflicting_identity_cannot_select_x676c_mmio(self):
        lib = self.fixture(compatible=b'infinix,x676c\0mediatek,MT6993\0')
        lib.fixture_start()
        self.assertEqual(lib.sweet_early_is_x9(), 1)
        self.assertEqual(lib.sweet_early_is_x676c(), 0)
        self.assertEqual(lib.fixture_maps(), 1)

    def test_x676c_preserves_known_console_without_x9_watchdog(self):
        lib = self.fixture(compatible=b'infinix,x676c\0mediatek,mt6789\0',
                           base=0x4811f000, size=0x40000, prefill=4)
        lib.fixture_start()
        self.assertEqual(lib.sweet_early_is_x676c(), 1)
        self.assertEqual(lib.sweet_early_is_x9(), 0)
        self.assertEqual(lib.fixture_maps(), 1)
        self.assertEqual(lib.fixture_watchdog(), 0)
        self.assertTrue(ctypes.string_at(lib.fixture_console(), 4).startswith(b'SSSS'))

    def test_framebuffer_layout_rejects_overflow_and_shadow_overrun(self):
        lib = self.fixture()
        valid = (0xfc910000, 0x3660000, 1256, 2760, 5120)
        self.assertEqual(lib.fixture_layout(*valid), 1)
        for bad in [(0, *valid[1:]), (valid[0] + 4, *valid[1:]),
                    (valid[0], 100, *valid[2:]),
                    (valid[0], valid[1], 1257, 2760, 5120),
                    (valid[0], valid[1], 1256, 2760, 5116),
                    (valid[0], valid[1], 1256, 10000, 5120),
                    (valid[0], (1 << 64) - 1, 16, 64, (1 << 64) - 8),
                    ((1 << 64) - 8, *valid[1:])]:
            self.assertEqual(lib.fixture_layout(*bad), 0, bad)

    def test_early_console_cross_compiles_for_arm64(self):
        # Use the actual ARM barriers with fixture declarations, not a full SDK.
        subprocess.run([shutil.which('clang'), '--target=aarch64-none-elf',
                        '-mcpu=cortex-a53', '-ffreestanding', '-mgeneral-regs-only',
                        '-mstrict-align', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-I', str(self.include), '-c',
                        str(self.tree / FILES[0]), '-o', str(self.directory / 'early-arm64.o')],
                       check=True, cwd=ROOT)

    def test_board_header_has_c_linkage_and_compiles_in_cpp(self):
        source = self.directory / 'board.cc'
        source.write_text('#include "SweetBoard.h"\n'
                          'bool fixture_board() { return sweet_board_x9(); }\n')
        subprocess.run([shutil.which('clang++'), '-std=c++11', '-Wall', '-Wextra',
                        '-Werror', '-fsyntax-only', '-I', str(self.include),
                        '-I', str(self.tree / 'iokit/Kernel/arm'), str(source)],
                       check=True, cwd=ROOT)

    def test_diagnostic_reset_is_off_by_default(self):
        """0187/0188 must never change a kernel the owner expects to boot."""
        text = (self.tree / FILES[0]).read_text()
        self.assertIn('#define SWEET_X9_DIAG_RESET 0', text)
        self.assertNotIn('#define SWEET_X9_DIAG_RESET 1', text)
        # The reset lives in sweet_diag_checkpoint, not in the late-init path,
        # and only fires when the opt-in is on.
        body = text[text.index('sweet_diag_checkpoint(unsigned int stage'):]
        self.assertIn('#if SWEET_X9_DIAG_RESET', body)
        self.assertNotIn('#if SWEET_X9_DIAG_RESET',
                         text[:text.index('sweet_diag_checkpoint(unsigned int stage')])
        # Exactly one reset site for the milestone checkpoints. 0187 also reset
        # from sweet_early_late_init, which runs before arm_init.c's checkpoints:
        # that copy always fired first and made every milestone after it
        # unreachable. Two further sites are deliberate diagnostics:
        # sweet_diag_probe() skips the framebuffer mark, sweet_diag_entry_reset()
        # is a bare compare-and-SMC used at a function's first statement.
        self.assertEqual(text.count('smc #0'), 3)
        # All three must honour the same selectable milestone; an unconditional
        # reset would make every build stop at the same place.
        self.assertEqual(text.count('if (stage != SWEET_X9_CHECKPOINT_STAGE) {'), 3)
        # The definition must not be inlinable. Inlined into
        # sweet_bootstrap_mark() the stage compare became "!= 0" rather than
        # "!= 50..61" and the branch could never fire; only the built object's
        # disassembly showed it.
        self.assertIn('__attribute__((noinline)) void\nsweet_diag_checkpoint', text)

    def test_machine_startup_entry_probe_wiring(self):
        """Milestone 50 is the bare entry probe at machine_startup()'s first
        statement, before the fastipi parse; 51-53 bracket the rest. A 50 build
        resets without writing a mark, so its record ends at 49's checkpoint
        label with no "diag:" line -- that shape is what tells
        "machine_startup was entered" apart from a hang before it."""
        patch = (ROOT / 'xnu/patches'
                 / '0188-x9-post-vm-init-milestones.patch').read_text()
        start = patch.index('machine_startup(__unused boot_args * args)')
        body = patch[start:]
        self.assertLess(body.index('sweet_diag_entry_reset(50);'),
                        body.index('PE_parse_boot_argn("fastipi"'))
        for mark in ('sweet_diag_checkpoint(51, "fastipi parsed")',
                     'sweet_diag_checkpoint(52, "machine_conf returned")',
                     'sweet_diag_checkpoint(53, "calling kernel_bootstrap")'):
            self.assertIn(mark, body)
        # arm_init.c hands over to machine_startup right after checkpoint 49,
        # so the 49->50 window is a single call and a 50 build that resets
        # cannot have hung meaningfully inside it.
        handover = patch.index('machine_startup(args);')
        self.assertIn('sweet_diag_checkpoint(49, "calling machine_startup")',
                      patch[handover - 400:handover])


if __name__ == '__main__':
    unittest.main()
