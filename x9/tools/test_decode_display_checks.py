"""Tests for diagnostic integrity handling, not hardware frame delivery."""
import unittest
from decode_display_checks import analyze


def record(tag, value, inverse=None):
    if inverse is None:
        inverse = value ^ 0xffffffff
    return f'{tag}+off=0x{value:08x}{inverse:08x}'.encode()


class DisplayCheckDecoderTests(unittest.TestCase):
    def test_valid_payload_is_not_treated_as_offset(self):
        result = analyze([('con', record('kA1', 1))])
        sample = result['samples'][0]
        self.assertEqual(sample['address'], '0x3e300ee4')
        self.assertEqual(sample['values'][0]['value'], '0x00000001')
        self.assertEqual(sample['phase'], 'post_frames')

    def test_one_half_corruption_is_rejected_without_repair(self):
        result = analyze([('pmsg', record('kM1', 0x01000008, 0xfefffff6))])
        self.assertEqual(result['samples'], [])
        self.assertEqual(len(result['rejected']), 1)

    def test_conflicting_valid_records_are_all_retained(self):
        result = analyze([('con', record('kA1', 1)), ('pmsg', record('kA1', 3))])
        sample = result['samples'][0]
        self.assertEqual(sample['status'], 'CONFLICT')
        self.assertEqual([v['value'] for v in sample['values']], ['0x00000001', '0x00000003'])

    def test_duplicate_copies_and_phases_remain_distinct(self):
        result = analyze([('con', record('kA0', 1) + b'\n' + record('kA1', 1)),
                          ('pmsg', record('kA1', 1))])
        self.assertEqual(len(result['samples']), 2)
        self.assertEqual(result['samples'][1]['values'][0]['copies'], 2)

    def test_lf_line_numbers_do_not_count_carriage_returns(self):
        result = analyze([('con', b'header\rjunk\n' + record('kP0', 0x05cd0001))])
        self.assertEqual(result['samples'][0]['values'][0]['evidence'][0]['LF_line'], 2)

    def test_damaged_tag_delimiter_and_partial_payload_are_not_guessed(self):
        for data in (b'k?1+off=0x00000001fffffffe', b'kA1+off90x00000001fffffffe',
                     b'kA1+off=0x00000001ffff?ffe', b'kA1+off=0x00000001fffffffe0'):
            with self.subTest(data=data):
                self.assertEqual(analyze([('con', data)])['samples'], [])

    def test_equal_corruption_in_both_halves_is_explicitly_not_certified(self):
        result = analyze([('con', record('kA1', 3))])
        self.assertIn('equal corruption', result['integrity_note'])


if __name__ == '__main__':
    unittest.main()
