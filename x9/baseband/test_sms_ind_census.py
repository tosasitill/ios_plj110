#!/usr/bin/env python3
"""Exercise the SMS indication census (x9_sms_ind=1): once-per-boot
naming of kind-1 msg ids that no bridge consumer recognizes.

Coverage, stated honestly:
- The census helpers (seen table + gate + log line) are extracted
  verbatim from x9_mipc_messages.inc and compiled/run for real.
- The bridge call site cannot be compiled standalone (x9mi_rx lives
  in the full MIPC translation unit), so its wiring is covered by
  source assertions on the kind==1 fan-out block: the recognition
  predicate must mirror the consumers consulted above it (x9mm_id,
  x9mc_id, the 4409/4419 diag pair, the 4202/4203/4302/4220 events).
- The 4603 registration rotation is behavior-tested in
  test_sms_ind.py; here it is only source-asserted.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MESSAGES = ROOT / 'x9/kernel/src/bsd/dev/x9_mipc_messages.inc'
BRIDGE = ROOT / 'x9/kernel/src/bsd/dev/x9_mipc_bridge.inc'
X9MESSAGES = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'


class SmsIndCensus(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_census_gate_and_once_per_id(self):
        source = MESSAGES.read_text()
        c0 = source.index('static uint16_t x9mm_census_seen')
        c1 = source.index('static void x9mm_census(', c0)
        c2 = source.index('\n}\n', c1) + 3
        code = r'''
#include <cstdint>
#include <cstdio>
static int ind_on=0;
int x9mm_ind_enabled(void){return ind_on;}
''' + source[c0:c2] + r'''
int main(){
 /* gate off: silent, and the id must NOT be recorded as seen */
 x9mm_census(0x4711,1,24);
 ind_on=1;
 x9mm_census(0x4711,1,24);  /* first sighting: prints */
 x9mm_census(0x4711,2,99);  /* same id, other mask/len: silent */
 x9mm_census(0x4712,2,32);  /* new id: prints */
 for(unsigned i=0;i<40;i++)x9mm_census(0x5000+i,1,16); /* seen table caps at 32 */
 printf("census-test done\n");}
'''
        out = self.compile_run(code)
        lines = [ln for ln in out.splitlines() if 'census unhandled id' in ln]
        self.assertEqual(len(lines), 32)
        self.assertEqual(out.count('0x4711'), 1)
        self.assertIn('census unhandled id 0x4711 mask 0x01 len 24', out)
        self.assertIn('census unhandled id 0x4712 mask 0x02 len 32', out)
        self.assertIn('census-test done', out)

    def test_bridge_wiring(self):
        source = BRIDGE.read_text()
        b0 = source.index('}else if(kind==1){')
        b1 = source.index('}else if(kind==2&&x9mm_id(id))', b0)
        block = source[b0:b1]
        self.assertIn('x9mm_census(id,x9_mi_stream[8],need)', block)
        for token in ('!x9mm_id(id)', '!x9mc_id(id)', '0x4409', '0x4419',
                      '0x4202', '0x4203', '0x4302', '0x4220'):
            self.assertIn(token, block)

    def test_rotation_source(self):
        source = X9MESSAGES.read_text()
        self.assertIn('{0x4602,0x4603,0x4606}', source)
        self.assertIn('no-ack indications registered (4602/4603/4606)', source)


if __name__ == '__main__':
    unittest.main()
