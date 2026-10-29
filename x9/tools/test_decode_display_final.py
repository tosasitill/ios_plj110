import binascii
import unittest
from decode_display_final import ADDRESSES, NAMES, analyze


def record(stage=2, phase=1, index=12, value=0x01000008):
    prefix = f'q46{stage}{phase}{index:02x}:{value:08x}'.encode('ascii')
    return prefix + f':{binascii.crc32(prefix):08x}'.encode('ascii')


class FinalDecoderTests(unittest.TestCase):
    def test_valid_record_has_correct_stage_phase_and_address(self):
        sample = analyze([('pmsg', record())])['samples'][0]
        self.assertEqual((sample['stage'], sample['phase'], sample['address']),
                         (2, 'post_frames', '0x3e730274'))
        self.assertEqual(sample['values'][0]['value'], '0x01000008')

    def test_each_single_bit_change_in_identity_or_value_is_not_accepted(self):
        original = record()
        for offset in range(16):
            for bit in range(8):
                changed = bytearray(original)
                changed[offset] ^= 1 << bit
                self.assertEqual(analyze([('bad', bytes(changed))])['samples'], [],
                                 (offset, bit))

    def test_damaged_crc_is_rejected(self):
        changed = record()[:-1] + (b'0' if record()[-1:] != b'0' else b'1')
        result = analyze([('pmsg', changed)])
        self.assertEqual(result['samples'], [])
        self.assertEqual(result['rejected'][0]['reason'], 'CRC mismatch')

    def test_conflicts_are_kept_not_voted_away(self):
        result = analyze([('pmsg', record(value=1) + b'\n' + record(value=3) + b'\n'
                           + record(value=1))])
        sample = result['samples'][0]
        self.assertEqual(sample['status'], 'CONFLICT')
        self.assertEqual([v['copies'] for v in sample['values']], [2, 1])

    def test_stages_and_phases_do_not_merge(self):
        result = analyze([('pmsg', b'\n'.join(record(stage=s, phase=p)
                                             for s in range(5) for p in range(2)))])
        self.assertEqual(len(result['samples']), 10)

    def test_lf_numbers_ignore_cr_and_find_concatenated_valid_records(self):
        result = analyze([('pmsg', b'header\rnoise\n' + record() + b'\r' + record())])
        values = result['samples'][0]['values'][0]
        self.assertEqual(values['copies'], 2)
        self.assertEqual([v['LF_line'] for v in values['evidence']], [2, 2])

    def test_out_of_range_index_is_not_assigned_to_another_field(self):
        result = analyze([('pmsg', record(index=255))])
        self.assertEqual(result['samples'], [])
        self.assertEqual(result['rejected'][0]['reason'], 'unknown index')

    def test_metadata_has_no_fake_mmio_address(self):
        result = analyze([('pmsg', record(index=37, value=2))])
        self.assertEqual(len(ADDRESSES), len(NAMES))
        self.assertIsNone(result['samples'][0]['address'])
        self.assertEqual(result['samples'][0]['register'], 'diagnostic.good_streak')

    def test_partial_or_overlong_record_is_not_reconstructed(self):
        for data in (record()[:-2], record() + b'0', record().replace(b':', b'?', 1)):
            self.assertEqual(analyze([('pmsg', data)])['samples'], [])


if __name__ == '__main__':
    unittest.main()
