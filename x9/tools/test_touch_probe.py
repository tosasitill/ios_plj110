"""Source assertions for the S3910 read-only periodic probe.

The probe exists to split the intermittent no-touch fault into
controller-not-producing / ATTN-path / read-path on the next incident.
Its safety contract is structural: it must aggregate only what the poll
loop already observes, add no SPI traffic and no MMIO writes, and leave
the existing alive line and stream behaviour untouched. These tests pin
that contract to the driver source.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/bsd/dev/sweet_x9_touch.c'


def loop_body(text):
    start = text.index('printf("X9 TOUCH: LBP reporting enabled;')
    return text[start:]


class TouchProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SOURCE.read_text()
        cls.loop = loop_body(cls.text)

    def test_gate_defaults_on(self):
        self.assertIn('sweet_x9_arg("x9_touch_probe",1)==1', self.text)

    def test_probe_line_fields(self):
        self.assertIn(
            'X9 TOUCH: probe=1 polls=%u idle=%u report=%u other=%u err=%u '
            'irq_assert=%u a_rep=%u a_idle=%u a_err=%u d_rep=%u hid=%u '
            'din=%08x ies=%08x', self.text)

    def test_attn_sampled_before_each_poll_read(self):
        sample = self.loop.index('irq_before=touch_irq_level()')
        read = self.loop.index('if(message(&code,response,&n))')
        classify = self.loop.index('probe_assert_report++')
        self.assertLess(sample, read)
        self.assertLess(read, classify)

    def test_attn_uses_same_din_register_as_alive(self):
        self.assertIn(
            'static unsigned touch_irq_level(void)'
            '{return (mmrd(touch_gpio,0x200)>>4)&1;}', self.text)

    def test_probe_block_is_read_only(self):
        start = self.loop.index('printf("X9 TOUCH: probe=1')
        block = self.loop[start:self.loop.index(
            'clock_interval_to_deadline(10,NSEC_PER_SEC,&heartbeat);', start)]
        for forbidden in ('transfer(', 'request(', 'mmwr(', 'wr(0x'):
            self.assertNotIn(forbidden, block)
        # The sampling helper itself must only read.
        helper = self.text[self.text.index('static unsigned touch_irq_level'):]
        helper = helper[:helper.index('\n')]
        self.assertNotIn('mmwr', helper)
        self.assertIn('mmrd', helper)

    def test_window_counters_reset_after_print(self):
        self.assertIn('probe_polls=probe_idle=probe_report=probe_other='
                      'probe_err=0;', self.text)
        self.assertIn('probe_assert_report=probe_assert_idle='
                      'probe_assert_err=probe_deassert_report=0;', self.text)

    def test_alive_line_unchanged(self):
        self.assertIn(
            'printf("X9 TOUCH: alive reports=%u contacts=%03x irq=%u\\n",'
            'reports,downmask,(mmrd(touch_gpio,0x200)>>4)&1);', self.text)

    def test_no_recovery_action_introduced(self):
        # The probe must not grow a reset/enable path: the only request()
        # calls in the file remain the bring-up ones before the loop.
        loop = self.loop
        self.assertNotIn('request(', loop)
        self.assertNotIn('touch_reset_release', loop)


if __name__ == '__main__':
    unittest.main()
