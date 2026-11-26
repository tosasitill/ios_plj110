#!/usr/bin/env python3
"""Focused source contract for opt-in cellular RRC diagnostics."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
WDS = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDS.inc'


class CellDiagPolicyTests(unittest.TestCase):
    def test_rrc_diag_is_opt_in_and_before_any_send(self):
        source = WDS.read_text()
        body = re.search(r'static void x9w_up_diag\(\)\{(.*?)\n\}\nstatic void x9w_side_poll', source, re.S)
        self.assertIsNotNone(body)
        text = body.group(1)
        self.assertIn('PE_parse_boot_argn("x9_cell_diag",&v,sizeof(v));enabled=v==1;', text)
        gate = 'if(!enabled||!x9w_session_ready||!x9_md_mipc_online())return;'
        self.assertIn(gate, text)
        self.assertLess(text.index(gate), text.index('x9w_side_send'))
        self.assertIn('NW_GET_CSCON_STATE', text)
        self.assertIn('NW_GET_PHYSICAL_CHANNEL_CONFIGS', text)

    def test_normal_statistics_path_is_unchanged(self):
        source = WDS.read_text()
        body = re.search(r'static void x9w_side_poll\(\)\{(.*?)\n\}', source, re.S)
        self.assertIsNotNone(body)
        self.assertIn('x9w_up_diag();', body.group(1))
        self.assertIn('if(x9w_stats_next&&x9q_ms()>=x9w_stats_next)x9w_stats(false);', body.group(1))

    def test_device_candidate_does_not_enable_diag_by_default(self):
        candidate = (ROOT / 'x9/build/boot-device.json').read_text()
        self.assertNotIn('x9_cell_diag', candidate)


if __name__ == '__main__':
    unittest.main()