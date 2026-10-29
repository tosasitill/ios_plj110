"""Mock MMIO tests; these do not emulate the DSI engine or panel scanout."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SHIM = Path(__file__).resolve().parent.parent / 'shim'


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class DisplayUlpsTests(unittest.TestCase):
    def test_ulps_exit_end_preserves_fields_and_follows_kernel_order(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_ulps_exit_end(uint64_t dsi)')
        end = source.index('\n}', start) + 2
        # The barrier is arm64-only; mock it along with MMIO on the host.
        body = source[start:end].replace(
            '__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
        fixture = r'''
#include <assert.h>
#include <stdint.h>
static uint32_t regs[0x1000 / 4];
static uint32_t offsets[16], values[16];
static unsigned writes, dumps;
static uint32_t rd32(uint64_t addr) { return regs[addr / 4]; }
static void wr32(uint64_t addr, uint32_t value) {
    offsets[writes] = (uint32_t)addr;
    values[writes++] = value;
    regs[addr / 4] = value;
}
static void dump_line(const char *tag, uint32_t off, uint32_t value) {
    (void)tag;
    assert(regs[off / 4] == value);
    dumps++;
}
''' + body + r'''
int main(void) {
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        writes = dumps = 0;
        regs[0x08 / 4] = 0xa5a500ff;
        regs[0x1d4 / 4] = 0x1234000f;
        regs[0x34 / 4] = 0x13520000;
        regs[0x00 / 4] = 0x00010005;
        regs[0x30 / 4] = 0x01000014 | (enabled << 1);
        uint32_t con = regs[0x30 / 4];
        display_ulps_exit_end(0);
        const uint32_t expected_offsets[] = {0x08, 0x1d4, 0x34, 0x00, 0x30, 0x30};
        const uint32_t expected_values[] = {
            0xa5a500bf, 0x12340007, 0x13420000, 0x00010001, con | 1, con
        };
        assert(writes == 6 && dumps == 5);
        for (unsigned i = 0; i < 6; i++) {
            assert(offsets[i] == expected_offsets[i]);
            assert(values[i] == expected_values[i]);
        }
        assert((regs[0x30 / 4] & 2) == (enabled << 1));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-display-tests-') as work:
            c = Path(work) / 'ulps.c'
            exe = Path(work) / 'ulps'
            c.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_exit_cleanup_is_called_after_wait_before_timing(self):
        source = (SHIM / 'display.c').read_text()
        body = source[source.index('void display_video_start(void)'):]
        wait = body.index('plog_hex("ulps done"')
        cleanup = body.index('display_ulps_exit_end(dsi);')
        timing = body.index('wr32(dsi + 0x600,')
        self.assertLess(wait, cleanup)
        self.assertLess(cleanup, timing)
        self.assertEqual(body.count('display_ulps_exit_end(dsi);'), 1)


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class DisplayConnectionTests(unittest.TestCase):
    def test_pan47_cascade_routes_only_last_to_outproc(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_blender_path_connect(')
        body = source[start:source.index('\n}', start) + 2].replace(
            '__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
        fixture = r'''
#include <assert.h>
#include <stdint.h>
static uint32_t regs[6][0x40/4];
static unsigned idx(uint64_t addr) {
    assert(addr >= 0x329c0000UL && addr < 0x32a20000UL);
    return (addr - 0x329c0000UL) / 0x10000;
}
static uint32_t rd32(uint64_t addr) { return regs[idx(addr)][(addr & 0xffff)/4]; }
static void wr32(uint64_t addr, uint32_t val) {
    unsigned i = idx(addr), off = addr & 0xffff;
    assert(off < 0x40);
    if (off == 0x20) {
        assert(regs[i][0x30/4] == 0x0ac804e8);
        assert(regs[i][0x24/4] == 0 && regs[i][0x1c/4] == 1);
        assert((regs[i][0x10/4] & 0x31) == (i == 0 ? 0x20u : (i == 5 ? 0x11u : 0x21u)));
    }
    regs[i][off/4] = val;
}
''' + body + r'''
int main(void) {
    for (unsigned bits = 0; bits < 8; bits++) {
        uint32_t old = 0xa5a500ce | (bits & 1) | ((bits & 6) << 3);
        for (unsigned i = 0; i < 6; i++) {
            regs[i][0x10/4] = old;
            regs[i][0x20/4] = 0x80;
        }
        display_blender_path_connect(0x329c0000UL, 0x0ac804e8);
        for (unsigned i = 0; i < 6; i++) {
            assert((regs[i][0x10/4] & ~0x31u) == (old & ~0x31u));
            assert(regs[i][0x20/4] == 0x81);
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-pan47-cascade-') as work:
            c = Path(work) / 'cascade.c'
            exe = Path(work) / 'cascade'
            c.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_pan47_cascade_config_precedes_mutex(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_ovlsys_layer_config(')
        body = source[start:source.index('\n}', start) + 2]
        self.assertLess(body.index('wr32(bl + 0x200, 0x10ffu);'),
                        body.index('display_blender_path_connect(bl, size);'))
        video = source[source.index('void display_video_start(void)'):]
        self.assertLess(video.index('display_ovlsys_layer_config(dsi);'),
                        video.index('display_mutex_kernel();'))

    def test_path_snapshots_are_read_only_and_straddle_normal_frames(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_frame_path_snapshot(int after)')
        end = source.index('\n}', start) + 2
        body = source[start:end]
        self.assertNotIn('wr32(', body)
        self.assertNotIn('dsi_dcs_read_k(', body)
        self.assertIn('off = 0xaa8; off <= 0xabc', body)
        self.assertIn('off = 0xa38; off <= 0xa54', body)
        self.assertIn('off = 0xa3c; off <= 0xa58', body)
        video = source[source.index('void display_video_start(void)'):]
        before = video.index('display_frame_path_snapshot(0);')
        trigger = video.index('display_cmd_frame_trigger(dsi);')
        after = video.index('display_frame_path_snapshot(1);')
        self.assertLess(before, trigger)
        self.assertLess(trigger, after)
        self.assertNotIn('dsi_dcs_read_k(', video[before:after])


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class DisplayPqBypassTests(unittest.TestCase):
    def test_only_rsz_branch_is_disconnected(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_pq_bypass_source_select(uint64_t m0)')
        end = source.index('\n}', start) + 2
        body = source[start:end].replace(
            '__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint32_t reg, old, writes, dumps;
static uint32_t rd32(uint64_t addr) {
    assert(addr == 0x3e300f34UL);
    return reg;
}
static void wr32(uint64_t addr, uint32_t value) {
    assert(addr == 0x3e300f34UL);
    reg = value;
    writes++;
}
static void dump_line(const char *tag, uint32_t off, uint32_t value) {
    assert(off == 0xf34);
    assert(strcmp(tag, dumps ? "pq1" : "pq0") == 0);
    assert(value == (dumps ? reg : old));
    dumps++;
}
''' + body + r'''
int main(void) {
    const uint32_t cases[] = {0, 1, 2, 3, 0x30003, 0xa5a55aff, 0xffffffff};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        old = reg = cases[i];
        writes = dumps = 0;
        display_pq_bypass_source_select(0x3e300000UL);
        assert(writes == 1 && dumps == 2);
        assert((reg & 3) == 2);
        assert((reg & ~3u) == (old & ~3u));
        if (old == 0x30003) assert(reg == 0x30002);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-pq-tests-') as work:
            c = Path(work) / 'pq.c'
            exe = Path(work) / 'pq'
            c.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_selection_follows_shadow_setup_and_precedes_mutex(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_routing_config(uint64_t dsi)')
        end = source.index('\n}', start) + 2
        body = source[start:end]
        select = body.index('display_pq_bypass_source_select(m0);')
        self.assertLess(body.index('wr32(m0 + 0x240,'), select)
        self.assertEqual(body.count('display_pq_bypass_source_select(m0);'), 1)
        self.assertIn('display_pq_bypass_output_select(m0);', body)
        video = source[source.index('void display_video_start(void)'):]
        self.assertLess(video.index('display_routing_config(dsi);'),
                        video.index('display_mutex_kernel();'))

    def test_mutex_post_dump_restores_bank_tag(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_mutex_kernel(void)')
        end = source.index('\n}', start) + 2
        body = source[start:end]
        after = body[body.index("tag[1] = 'n';"):]
        self.assertLess(after.index('tag[2] = pre[b];'),
                        after.index('dump_line(tag,'))


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class DisplayPqOutputTests(unittest.TestCase):
    def test_only_dlo1_branch_is_disconnected(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_pq_bypass_output_select(uint64_t m0)')
        end = source.index('\n}', start) + 2
        body = source[start:end].replace(
            '__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint32_t reg, old, writes, dumps;
static uint32_t rd32(uint64_t addr) {
    assert(addr == 0x3e300ee4UL);
    return reg;
}
static void wr32(uint64_t addr, uint32_t value) {
    assert(addr == 0x3e300ee4UL);
    reg = value;
    writes++;
}
static void dump_line(const char *tag, uint32_t off, uint32_t value) {
    assert(off == 0xee4);
    assert(strcmp(tag, dumps ? "po1" : "po0") == 0);
    assert(value == (dumps ? reg : old));
    dumps++;
}
''' + body + r'''
int main(void) {
    const uint32_t cases[] = {0, 1, 2, 3, 0x30003, 0xa5a55aff, 0xffffffff};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        old = reg = cases[i];
        writes = dumps = 0;
        display_pq_bypass_output_select(0x3e300000UL);
        assert(writes == 1 && dumps == 2);
        assert((reg & 3) == 1);
        assert((reg & ~3u) == (old & ~3u));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-pq-output-tests-') as work:
            c = Path(work) / 'output.c'
            exe = Path(work) / 'output'
            c.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_output_selection_preserves_side_route_and_precedes_mutex(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_routing_config(uint64_t dsi)')
        body = source[start:source.index('\n}', start) + 2]
        select = body.index('display_pq_bypass_output_select(m0);')
        self.assertEqual(body.count('display_pq_bypass_output_select(m0);'), 1)
        self.assertLess(body.index('display_pq_bypass_source_select(m0);'), select)
        self.assertLess(body.index('wr32(m0 + 0xbd0,'), select)
        self.assertIn('wr32(m1 + 0xee4, 0x00000000u);', body)
        self.assertNotIn('wr32(m0 + 0xee4,', body)
        video = source[source.index('void display_video_start(void)'):]
        self.assertLess(video.index('display_routing_config(dsi);'),
                        video.index('display_mutex_kernel();'))


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class DisplayCheckedSnapshotTests(unittest.TestCase):
    def test_snapshot_reads_once_before_logging_and_encodes_complement(self):
        import decode_display_checks
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_checked_path_snapshot(int after)')
        body = source[start:source.index('\n}', start) + 2]
        expected = ','.join(hex(addr) + 'UL' for addr in decode_display_checks.ADDRESSES)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
static const uint64_t expected[] = {''' + expected + r'''};
static unsigned reads, dumps;
static int phase;
static uint32_t sample(unsigned i) { return 0x31415926u ^ (i * 0x1020304u); }
static uint32_t rd32(uint64_t addr) {
    assert(!dumps && reads < 16 && addr == expected[reads]);
    return sample(reads++);
}
static void dump_line(const char *tag, uint32_t value, uint32_t inverse) {
    assert(reads == 16 && dumps < 16);
    assert(tag[0] == 'k' && tag[1] == (char)('A' + dumps));
    assert(tag[2] == '0' + phase && tag[3] == 0);
    assert(value == sample(dumps) && (value ^ inverse) == 0xffffffffu);
    dumps++;
}
''' + body + r'''
int main(void) {
    for (phase = 0; phase < 2; phase++) {
        reads = dumps = 0;
        display_checked_path_snapshot(phase);
        assert(reads == 16 && dumps == 16);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-checked-snapshot-tests-') as work:
            c = Path(work) / 'checked.c'
            exe = Path(work) / 'checked'
            c.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_checked_snapshots_are_read_only_outside_start_pulse(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('static void display_checked_path_snapshot(int after)')
        body = source[start:source.index('\n}', start) + 2]
        for forbidden in ('wr32(', 'dsi_dcs_read_k(', 'gce_event_clr('):
            self.assertNotIn(forbidden, body)
        video = source[source.index('void display_video_start(void)'):]
        self.assertLess(video.index('display_checked_path_snapshot(0);'),
                        video.index('display_cmd_frame_trigger(dsi);'))
        self.assertLess(video.index('display_cmd_frame_trigger(dsi);'),
                        video.index('display_checked_path_snapshot(1);'))
        trigger_start = source.index('static void display_cmd_frame_trigger(uint64_t dsi)')
        trigger = source[trigger_start:source.index('\n}', trigger_start) + 2]
        self.assertNotIn('display_checked_path_snapshot(', trigger)


if __name__ == '__main__':
    unittest.main()
