#!/usr/bin/env python3
"""Exercise the NAS 0x0073 GetImsPreferenceInfo experiment answer.

The response shape (TLV 0x10, 9-byte IpPrefInfo {u8 sys_mode, u64}) is
static evidence from 20H392 libCommCenterMCommandDrivers; the sys_mode
byte must come from the freshest real serving sample, and the default
(no x9_ims_pref_diag) behavior must stay UNSUPPORTED.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9QMIAT.inc'


class ImsPrefInfo(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_sys_mode_from_real_serving_sample(self):
        source = SOURCE.read_text()
        b0 = source.index('static uint8_t x9q_ims_sys_mode(')
        b1 = source.index('static void x9q_send(', b0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
struct x9q_data_radio { uint64_t sampled; uint32_t rat; };
static x9q_data_radio x9q_data_radios[2];
static uint64_t clock_ms=100000;
uint64_t x9q_ms(){return clock_ms;}
''' + source[b0:b1] + r'''
int main(){
 // No serving sample yet: kNoService, not a guessed capability.
 assert(x9q_ims_sys_mode(0)==0);
 // Fresh LTE sample (libqmi RAT bit 5, as set by x9q_nas for AcT 7/9/13).
 x9q_data_radios[0]={clock_ms,1u<<5};
 assert(x9q_ims_sys_mode(0)==8);
 // Fresh NR sample (bit 10, AcT 11) wins over LTE, mirroring x9q_nas order.
 x9q_data_radios[0]={clock_ms,(1u<<10)|(1u<<5)};
 assert(x9q_ims_sys_mode(0)==12);
 // Unrelated RAT bits are not an IMS sys mode.
 x9q_data_radios[0]={clock_ms,1u<<2};
 assert(x9q_ims_sys_mode(0)==0);
 // Freshness boundary: exactly 30 s old still counts, 30001 ms does not.
 x9q_data_radios[0]={clock_ms-30000,1u<<5};
 assert(x9q_ims_sys_mode(0)==8);
 x9q_data_radios[0]={clock_ms-30001,1u<<5};
 assert(x9q_ims_sys_mode(0)==0);
 // Per-SIM state is independent.
 x9q_data_radios[0]={clock_ms,1u<<10};
 x9q_data_radios[1]={clock_ms,1u<<5};
 assert(x9q_ims_sys_mode(0)==12);
 assert(x9q_ims_sys_mode(1)==8);
 return 0;
}
'''
        self.compile_run(code)

    def test_default_behavior_and_gate_boundary(self):
        source = SOURCE.read_text()
        # x9q_supported must not admit 0x73: default stays UNSUPPORTED.
        s0 = source.index('static bool x9q_supported(')
        s1 = source.index('\n}\n', s0)
        self.assertNotIn('0x73', source[s0:s1])
        # The only 0x73 answer is the gated experiment in x9q_send.
        h0 = source.index('if(service==3&&id==0x73')
        h1 = source.index('p.send(local,port);return;', h0)
        block = source[h0:h1]
        self.assertIn('sweet_x9_arg("x9_ims_pref_diag",0)==1', block)
        self.assertIn('len==7', block)
        self.assertIn('uint8_t info[9]={};info[0]=x9q_ims_sys_mode(c.sim_slot);', block)
        self.assertIn('p.tlv(0x10,info,9);', block)


if __name__ == '__main__':
    unittest.main()
