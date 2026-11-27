#!/usr/bin/env python3
"""Host-test the Baseband Voice provider contract (L6/L1).

The format constants are this phone's captured factory mMdConfig
(mo-call-audio.txt:633); the qualification rules mirror the
VirtualAudio factory's real-stream enumeration from the 20H392
static evidence."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / 'x9/kernel/src/bsd/dev/x9_voice_bbdev.h'


class VoiceBbdev(unittest.TestCase):
    def test_contract(self):
        code = r'''
#include <cassert>
#include <cstdio>
#include "@HEADER@"
static struct x9_voice_bbdev_desc good() {
 struct x9_voice_bbdev_desc d = {1, 1, 1, 0, 2, 48000, 0};
 return d;
}
int main() {
 /* Captured modem PCM geometry: 1024-frame periods, 2 periods,
  * 2ch S16 -> 4096 B per period, 8192 B per buffer. */
 assert(X9_VOICE_PCM_PERIOD_BYTES == 4096);
 assert(X9_VOICE_PCM_BUFFER_BYTES == 8192);
 struct x9_voice_bbdev_desc d = good();
 assert(x9_voice_bbdev_qualify(&d));
 /* Every single missing requirement disqualifies. */
 d = good(); d.has_input_stream = 0;  assert(!x9_voice_bbdev_qualify(&d));
 d = good(); d.has_output_stream = 0; assert(!x9_voice_bbdev_qualify(&d));
 d = good(); d.has_ioproc = 0;        assert(!x9_voice_bbdev_qualify(&d));
 d = good(); d.channels = 1;          assert(!x9_voice_bbdev_qualify(&d));
 d = good(); d.rate = 16000;          assert(!x9_voice_bbdev_qualify(&d));
 d = good(); d.format = 1;            assert(!x9_voice_bbdev_qualify(&d));
 /* The media Speaker shape (streams + IOProc at 48k stereo) is
  * still refused: it is a media owner, not the baseband device. */
 d = good(); d.is_media_device = 1;   assert(!x9_voice_bbdev_qualify(&d));
 assert(!x9_voice_bbdev_qualify(0));
 /* Registry: empty stays fail-closed; only a qualified provider
  * registers, only once. */
 struct x9_voice_bbdev_registry r; x9_voice_bbdev_registry_init(&r);
 assert(!x9_voice_bbdev_ingress_open(&r));
 d = good(); d.rate = 8000;
 assert(x9_voice_bbdev_register(&r, &d) == -2);
 assert(!x9_voice_bbdev_ingress_open(&r));
 d = good();
 assert(x9_voice_bbdev_register(&r, &d) == 0);
 assert(x9_voice_bbdev_ingress_open(&r));
 assert(x9_voice_bbdev_register(&r, &d) == -1);
 x9_voice_bbdev_unregister(&r);
 assert(!x9_voice_bbdev_ingress_open(&r));
 printf("voice bbdev ok\n");
}
'''.replace('@HEADER@', str(HEADER))
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('voice bbdev ok', out)


if __name__ == '__main__':
    unittest.main()
