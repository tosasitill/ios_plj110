#!/usr/bin/env python3
"""Host-test the sph_info builder (L3).

Byte expectations come from the factory MO capture of this phone
(mo-call-audio.txt:790 configSpeechInfo log line, :794 the 0x2f20
send) and the HAL/MD disassembly archived under
x9/out/call-md-voice-owner/."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / 'x9/kernel/src/bsd/dev/x9_voice_sph.h'


class VoiceSph(unittest.TestCase):
    def test_build(self):
        code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "@HEADER@"
int main() {
 uint8_t body[X9_VOICE_SPH_SIZE];
 /* This phone's factory MO call: the pinned fields land at their
  * offsets and every unpinned byte stays zero. */
 x9_voice_sph_build(&X9_VOICE_SPH_PLJ110_MO, body);
 assert(body[0x00] == 0 && body[0x01] == 0 && body[0x02] == 3);
 assert(body[0x03] == 3);            /* enh config from DT via usip */
 assert(body[0x05] == 2);            /* SmartPA branch byte */
 assert(body[0x1c] == 6);            /* com par[0] */
 assert(body[0x34] == 3);            /* debug info[0] */
 assert(body[0x54] == 2);            /* smartPA config 1 + dual */
 assert(body[0x55] == 0);            /* hwif from DT via usip */
 for (unsigned i = 0; i < X9_VOICE_SPH_SIZE; i++) {
  if (i==0x02||i==0x03||i==0x05||i==0x1c||i==0x34||i==0x54) continue;
  assert(body[i] == 0);
 }
 /* MD consumption mirror for this phone (hwif 0 branch):
  * state+54 = (3 << 1) & 2 = 2, state+55 = 0, state+56 = 0. */
 uint8_t st[3]; x9_voice_sph_md_state(body, st);
 assert(st[0] == 2 && st[1] == 0 && st[2] == 0);
 /* hwif nonzero branch: state+54 = 2, state+56 = ((cfg>>3)&3)+1. */
 struct x9_voice_sph_inputs in = X9_VOICE_SPH_PLJ110_MO;
 in.hwif = 1; in.enh_config = 0x1b;   /* (0x1b>>3)&3 = 3 -> +56 = 4 */
 x9_voice_sph_build(&in, body);
 assert(body[0x03] == 0x1b && body[0x55] == 1);
 x9_voice_sph_md_state(body, st);
 assert(st[0] == 2 && st[1] == 1 && st[2] == 4);
 /* Gating: ADSP path inactive forces both dynamic fields to 0,
  * exactly as configSpeechInfo skips the getters. */
 in = X9_VOICE_SPH_PLJ110_MO; in.adsp_active = 0;
 x9_voice_sph_build(&in, body);
 assert(body[0x03] == 0 && body[0x55] == 0);
 assert(body[0x02] == 3 && body[0x54] == 2);   /* others unaffected */
 /* Wrapper: pinned fields only. */
 uint8_t wrap[X9_VOICE_SPH_WRAP_SIZE];
 x9_voice_sph_wrap(wrap, body);
 assert(wrap[0] == 1);
 assert(wrap[2] == 0x20 && wrap[3] == 0x2f);          /* 0x2f20 LE */
 assert(wrap[0x0c] == 25 && wrap[0x0d] == 0);         /* type 25 */
 assert(wrap[0x0e] == 128 && wrap[0x0f] == 0);        /* size 128 */
 uint64_t ptr = 0;
 for (unsigned i = 0; i < 8; i++) ptr |= (uint64_t)wrap[0x10+i] << (8*i);
 assert(ptr == (uint64_t)(uintptr_t)body);
 printf("voice sph ok\n");
}
'''.replace('@HEADER@', str(HEADER))
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('voice sph ok', out)


if __name__ == '__main__':
    unittest.main()
