"""Graph samples are mocks, not physical display completion evidence."""
import binascii
import unittest
from decode_display_graph import REGISTERS, analyze, set_signals
from test_display_final import body, run_fixture


class GraphSnapshotTests(unittest.TestCase):
    def test_c_sampler_matches_all_register_addresses_and_crc_mapping(self):
        addresses = ','.join(hex(addr) + 'UL' for _, addr in REGISTERS)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static unsigned reads, logs, kicks;
static const uint64_t addresses[] = {''' + addresses + r'''};
static uint32_t rd32(uint64_t addr) {
    assert(!logs && reads < sizeof(addresses)/sizeof(addresses[0]));
    assert(addr == addresses[reads]);
    return 0x12340000u | reads++;
}
static void plog_str(const char *line) {
    assert(reads == sizeof(addresses)/sizeof(addresses[0]));
    logs++; fputs(line, stdout);
}
static void wdt_kick(void) { kicks++; }
''' + body('static uint32_t display_record_crc(') + '\n' + body(
            'static void display_graph_snapshot(unsigned stage, unsigned after)') + r'''
int main(void) {
    display_graph_snapshot(3, 1);
    assert(logs == 8 * sizeof(addresses)/sizeof(addresses[0]));
    assert(kicks == 8);
    return 0;
}
'''
        result = analyze([('fixture', run_fixture(fixture))])
        self.assertFalse(result['rejected'])
        self.assertEqual(len(result['samples']), len(REGISTERS) * 8)
        for index, (name, address) in enumerate(REGISTERS):
            rows = [row for row in result['samples'] if row['register'] == name]
            self.assertEqual(len(rows), 8)
            for row in rows:
                self.assertEqual(row['value'], f'0x{0x12340000 | index:08x}')
                self.assertEqual(row['address'], f'0x{address:08x}')
                self.assertEqual((row['stage'], row['phase']), (3, 'post_frames'))

    def test_valid0_and_valid1_bit9_are_not_interchangeable(self):
        self.assertEqual(set_signals(0, 1 << 9)[0]['name'],
                         'ovl_blender4_out1__to__ovl_blender5_in0')
        self.assertEqual(set_signals(1, 1 << 9)[0]['name'],
                         'ovl_exdma3_out1__to__ovl_exdma_out_crossbar_in4')
        bits = {r['bit']: r['name'] for r in set_signals(1, 0x40300600)}
        self.assertEqual(set(bits), {9, 10, 20, 21, 30})
        self.assertIn('exdma4_out1', bits[10])
        self.assertIn('blender1_in1', bits[20])
        self.assertIn('outproc0', bits[30])

    def test_single_bit_corruption_cannot_create_accepted_graph_record(self):
        prefix = b'q493100:00017e08'
        record = prefix + b':' + f'{binascii.crc32(prefix):08x}'.encode()
        self.assertEqual(len(analyze([('clean', record)])['samples']), 1)
        for i in range(len(record)):
            for bit in range(8):
                damaged = bytearray(record)
                damaged[i] ^= 1 << bit
                self.assertFalse(analyze([('damaged', damaged)])['samples'])

    def test_snapshot_has_no_configuration_or_protocol_writes(self):
        source = body('static void display_graph_snapshot(unsigned stage, unsigned after)')
        for forbidden in ('wr32(', 'dsi_dcs_read', 'gce_event_', 'smc('):
            self.assertNotIn(forbidden, source)


if __name__ == '__main__':
    unittest.main()
