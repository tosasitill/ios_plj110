"""Source/mock tests for final mutex stages; not physical scanout tests."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from decode_display_final import ADDRESSES, analyze

SHIM = Path(__file__).resolve().parent.parent / 'shim'


def body(name):
    source = (SHIM / 'display.c').read_text()
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2].replace(
        '__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')


def run_fixture(text):
    with tempfile.TemporaryDirectory(prefix='x9-final-source-test-') as work:
        c, exe = Path(work) / 'fixture.c', Path(work) / 'fixture'
        c.write_text(text)
        subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(c), '-o', str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True).stdout


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class FinalDisplayTests(unittest.TestCase):
    def test_stages_are_bounded_cumulative_and_touch_only_named_mutex_fields(self):
        fixture = r'''
#include <assert.h>
#include <stdint.h>
static unsigned frame_mutex_rearm;
static uint64_t addresses[16];
static uint32_t values[16];
static unsigned writes;
static uint32_t rd32(uint64_t addr) {
    assert(addr == 0x32920038UL || addr == 0x3e720038UL);
    return addr == 0x32920038UL ? 0x80010004u : 0x80004003u;
}
static void wr32(uint64_t addr, uint32_t value) {
    assert(writes < 16);
    addresses[writes] = addr;
    values[writes++] = value;
}
''' + body('static void display_final_mutex_stage(unsigned stage)') + r'''
int main(void) {
    display_final_mutex_stage(0);
    assert(!writes && !frame_mutex_rearm);
    display_final_mutex_stage(1);
    const uint64_t owners[] = {0x3e320024UL, 0x32920024UL, 0x3e720024UL, 0x32c20024UL};
    assert(writes == 4 && !frame_mutex_rearm);
    for (unsigned i = 0; i < 4; i++) assert(addresses[i] == owners[i] && !values[i]);
    writes = 0;
    display_final_mutex_stage(2);
    assert(!writes && frame_mutex_rearm);
    display_final_mutex_stage(3);
    assert(writes == 2 && addresses[0] == 0x32920020UL && !values[0]);
    assert(addresses[1] == 0x32920034UL && values[1] == ((1u<<3) | (0x3fu<<9) | (1u<<16)));
    writes = 0;
    display_final_mutex_stage(4);
    assert(writes == 4 && addresses[0] == 0x32920020UL && addresses[1] == 0x3e720020UL);
    assert(!values[0] && !values[1]);
    assert(addresses[2] == 0x32920038UL && values[2] == 0x00010004u);
    assert(addresses[3] == 0x3e720038UL && values[3] == 0x00004003u);
    assert(frame_mutex_rearm);
    return 0;
}
'''
        run_fixture(fixture)

    def test_frame_enable_matches_cfg_mask_and_bank_order_without_reset_or_acquire(self):
        fixture = r'''
#include <assert.h>
#include <stdint.h>
static const uint64_t bases[] = {0x3e320000UL, 0x32920000UL, 0x3e720000UL, 0x32c20000UL};
static unsigned writes;
static uint32_t rd32(uint64_t addr) {
    assert(addr == bases[writes / 3] + 8);
    return 0xa5a500ffu;
}
static void wr32(uint64_t addr, uint32_t value) {
    assert(writes < 12);
    unsigned bank = writes / 3, step = writes % 3;
    assert(addr == bases[bank] + (step ? 0x20 : 8));
    assert(value == (step ? step - 1u : 0xa5a500feu));
    writes++;
}
''' + body('static void display_mutex_frame_enable(void)') + r'''
int main(void) {
    for (unsigned frame = 0; frame < 4; frame++) {
        writes = 0;
        display_mutex_frame_enable();
        assert(writes == 12);
    }
    return 0;
}
'''
        run_fixture(fixture)

    def test_normal_verdict_requires_frame_done_and_rejects_idle_cmd_done_busy_errors(self):
        fixture = '#include <assert.h>\n#include <stdint.h>\n' + body(
            'static int display_normal_frame_ok(uint32_t status)') + r'''
int main(void) {
    assert(!display_normal_frame_ok(0));
    assert(!display_normal_frame_ok(2));
    assert(display_normal_frame_ok(0x10));
    assert(display_normal_frame_ok(0x12));
    assert(!display_normal_frame_ok(0x80000010));
    assert(!display_normal_frame_ok(0x1010));
    assert(!display_normal_frame_ok(0x40010));
    return 0;
}
'''
        run_fixture(fixture)

    def test_snapshot_reads_once_before_logging_and_c_crc_matches_python_decoder(self):
        addresses = ','.join(f'{a:#x}UL' for a in ADDRESSES if a is not None)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static const uint64_t expected[] = {''' + addresses + r'''};
static uint32_t frame_last_status = 0x12, frame_last_wait = 7;
static unsigned frame_good_count = 2, reads, logs, kicks;
static uint32_t rd32(uint64_t addr) {
    assert(!logs && reads < 35 && addr == expected[reads]);
    return 0x31415926u ^ reads++;
}
static void plog_str(const char *line) {
    assert(reads == 35);
    logs++;
    fputs(line, stdout);
}
static void wdt_kick(void) { kicks++; }
''' + body('static uint32_t display_record_crc(const char *data, unsigned len)') + '\n' + body(
            'static void display_final_path_snapshot(unsigned stage, unsigned after)') + r'''
int main(void) {
    assert(display_record_crc("123456789", 9) == 0xcbf43926u);
    display_final_path_snapshot(3, 1);
    assert(reads == 35 && logs == 38 * 8 && kicks == 8);
    return 0;
}
'''
        output = run_fixture(fixture)
        result = analyze([('C fixture', output)])
        self.assertEqual(len(result['samples']), 38)
        self.assertEqual(result['rejected'], [])
        for sample in result['samples']:
            self.assertEqual((sample['stage'], sample['phase'], sample['status']),
                             (3, 'post_frames', 'consistent'))
            expected = (0x31415926 ^ sample['index'] if sample['index'] < 35
                        else (0x12, 7, 2)[sample['index'] - 35])
            self.assertEqual(sample['values'][0]['value'], f'0x{expected:08x}')
            self.assertEqual(sample['values'][0]['copies'], 8)

    def test_cascade_snapshot_reads_once_before_logging_and_preserves_crc(self):
        from decode_boot_journal import analyze as decode_journal
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static unsigned reads, logs, kicks;
static uint32_t rd32(uint64_t addr) {
    assert(!logs && reads < 6);
    assert(addr == 0x329c0010UL + reads * 0x10000UL);
    reads++;
    return reads == 1 ? 0x20 : (reads == 6 ? 0x11 : 0x21);
}
static void plog_str(const char *line) {
    assert(reads == 6); logs++; fputs(line, stdout);
}
static void wdt_kick(void) { kicks++; }
''' + body('static uint32_t display_record_crc(') + '\n' + body(
            'static void display_cascade_snapshot(unsigned after)') + r'''
int main(void) {
    display_cascade_snapshot(1);
    assert(reads == 6 && logs == 48 && kicks == 8);
    return 0;
}
'''
        result = decode_journal([('fixture', run_fixture(fixture))])
        self.assertFalse(result['rejected'])
        self.assertEqual(len(result['samples']), 48)
        actual = {r['blender']: r['value'] for r in result['samples']}
        self.assertEqual(actual, {1: '0x00000020', 2: '0x00000021',
                                 3: '0x00000021', 4: '0x00000021',
                                 5: '0x00000021', 6: '0x00000011'})

    def test_diagnostic_logger_is_read_only_and_not_inside_start_pulse(self):
        snapshot = body('static void display_final_path_snapshot(unsigned stage, unsigned after)')
        for forbidden in ('wr32(', 'dsi_dcs_read_k(', 'gce_event_clr('):
            self.assertNotIn(forbidden, snapshot)
        trigger = body('static void display_cmd_frame_trigger(uint64_t dsi)')
        self.assertLess(trigger.index('wr32(dsi + 0x0c,'),
                        trigger.index('display_mutex_frame_enable();'))
        self.assertLess(trigger.index('display_mutex_frame_enable();'),
                        trigger.index('wr32(dsi + 0x00,'))
        pulse = trigger[trigger.index('wr32(dsi + 0x00,'):trigger.index('first_status =')]
        for forbidden in ('dump_line(', 'plog_', 'wait_ms(', 'wdt_kick(', 'snapshot('):
            self.assertNotIn(forbidden, pulse)

    def test_single_boot_stages_stop_at_two_successes_and_hold_before_power_read_or_pattern(self):
        source = (SHIM / 'display.c').read_text()
        video = source[source.index('void display_video_start(void)'):]
        self.assertIn('stage < 5', video)
        self.assertIn('f < 4', video)
        self.assertIn('frame_good_count >= 2', video)
        self.assertIn('frame_good_count = 0;', video)
        hold = video[video.index('if (winning_stage >= 0) {'):video.index('/* Events were cleared')]
        self.assertIn('return;', hold)
        self.assertNotIn('dsi_dcs_read_k(', hold)
        self.assertNotIn('0x230,', hold)
        self.assertLess(video.index('normal frame hold done'), video.index('dsi_dcs_read_k('))

    def test_actual_stage_loop_resets_streak_stops_early_and_has_twenty_frame_bound(self):
        source = (SHIM / 'display.c').read_text()
        start = source.index('\tint winning_stage = -1;')
        end = source.index('\tdisplay_dsi_snapshot(dsi, "ds1");', start)
        loop = source[start:end]
        fixture = r'''
#include <assert.h>
#include <stdint.h>
static unsigned frame_good_count, current, frames, total, snapshots, scenario;
static void display_cascade_snapshot(unsigned after) { assert(after == 1); }
static void display_graph_snapshot(unsigned stage, unsigned after) {
    assert(stage == current && after <= 1);
}
static uint32_t frame_last_status;
static void display_final_mutex_stage(unsigned stage) { current = stage; frames = 0; }
static void display_final_path_snapshot(unsigned stage, unsigned after) {
    assert(stage == current);
    if (!after) assert(frame_good_count == 0);
    snapshots++;
}
static void display_cmd_frame_trigger(uint64_t dsi) { assert(dsi == 0x3e730000UL); frames++; total++; }
static void wait_ms(unsigned ms) { assert(ms == 40); }
static void wdt_kick(void) {}
static uint32_t rd32(uint64_t addr) {
    assert(addr == 0x3e73000cUL);
    if (scenario == 1) return 0x10;
    if (scenario == 2) return 0x80000010;
    if (current != 2) return 2;
    return frames == 2 ? 0x1010 : 0x10;
}
''' + body('static int display_normal_frame_ok(uint32_t status)') + r'''
static int run_case(unsigned mode) {
    const uint64_t dsi = 0x3e730000UL;
    scenario = mode; total = snapshots = 0;
''' + loop + r'''
    return winning_stage;
}
int main(void) {
    assert(run_case(0) == 2 && total == 12 && snapshots == 6 && frame_good_count == 2);
    assert(run_case(1) == 0 && total == 2 && snapshots == 2);
    assert(run_case(2) == -1 && total == 20 && snapshots == 10 && current == 4);
    return 0;
}
'''
        run_fixture(fixture)


if __name__ == '__main__':
    unittest.main()
