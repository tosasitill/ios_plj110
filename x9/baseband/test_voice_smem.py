#!/usr/bin/env python3
"""Host-test the call-voice SMEM (USIP id 24) descriptor evaluator."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / 'x9/kernel/src/bsd/dev/x9_voice_smem.h'


class VoiceSmem(unittest.TestCase):
    def test_evaluate(self):
        code = r'''
#include <cassert>
#include <cstdio>
#include "@HEADER@"
static struct x9_voice_smem_report run(struct x9_voice_smem_desc d){
 struct x9_voice_smem_report r; x9_voice_smem_evaluate(&d,&r); return r; }
int main(){
 /* This device's Android LK log: AP=f0d00000 MD=42d00000 size 60000 flags 2. */
 struct x9_voice_smem_report r=run({0x42d00000,0x60000,2,0xffffffff,1,1,1});
 assert(r.present&&r.android_match&&r.md_view==0x42d00000&&r.size==0x60000&&r.flags==2);
 /* Absent from the emulated table: not present, never a match. */
 r=run({0,0,0,0,0,0,0}); assert(!r.present&&!r.android_match);
 /* In the LK table but zero-sized: not present. */
 r=run({0x42d00000,0,2,0xffffffff,1,1,1}); assert(!r.present&&!r.android_match);
 /* Present but a different size than the Android log: finding, not match. */
 r=run({0x42d00000,0x50000,2,0xffffffff,1,1,1}); assert(r.present&&!r.android_match);
 /* Present but different flags: not a match. */
 r=run({0x42d00000,0x60000,0,0xffffffff,1,1,1}); assert(r.present&&!r.android_match);
 /* Static-table entry without LK backing: not present. */
 r=run({0x42d00000,0x60000,2,0xffffffff,0,1,1}); assert(!r.present&&!r.android_match);
 printf("voice smem ok\n"); }
'''.replace('@HEADER@', str(HEADER))
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('voice smem ok', out)


if __name__ == '__main__':
    unittest.main()
