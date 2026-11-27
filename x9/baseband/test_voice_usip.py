#!/usr/bin/env python3
"""Host-test the call-voice USIP EMI owner (L5).

The AD50 byte expectations are the reference-derived wire frames in
x9/out/call-md-voice-owner/usip-ad50-and-md-speech-on-contract.json
(derived from mtk-usip.c + audio_messenger_ipi.h for this device's LK
descriptor AP=f0d00000, not a capture)."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / 'x9/kernel/src/bsd/dev/x9_voice_usip.h'


class VoiceUsip(unittest.TestCase):
    def test_owner_and_ad50(self):
        code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "@HEADER@"
static void hex(const uint8_t *p, char *dst) {
 for (unsigned i = 0; i < X9_VOICE_USIP_AD50_FRAME; i++) snprintf(dst + 2*i, 3, "%02x", p[i]);
}
int main() {
 /* EMI table tiles the region exactly. */
 uint32_t off, size, end = 0;
 for (unsigned w = 0; w < 3; w++) {
  assert(x9_voice_usip_subrange((enum x9_voice_usip_sub)w, &off, &size) == 0);
  assert(off == end); end += size;
 }
 assert(end == X9_VOICE_USIP_EXTENT);
 assert(x9_voice_usip_subrange((enum x9_voice_usip_sub)3, &off, &size) < 0);
 assert(X9_VOICE_USIP_PARAM_WRITE_OFF < X9_VOICE_USIP_PARAM_SIZE);

 struct x9_voice_usip o; x9_voice_usip_reset(&o);
 uint8_t frame[X9_VOICE_USIP_AD50_FRAME]; char text[65];
 /* Nothing works before binding. */
 assert(x9_voice_usip_acquire(&o) < 0);
 assert(x9_voice_usip_ad50_build(&o, 0, frame) < 0);
 assert(!x9_voice_usip_qualified(&o));
 /* Binding rejects non-descriptor values. */
 assert(x9_voice_usip_bind(&o, 0, 0x60000, 0x42d00000) < 0);
 assert(x9_voice_usip_bind(&o, 0xf0d00001, 0x60000, 0x42d00000) < 0);
 assert(x9_voice_usip_bind(&o, 0xf0d00000, 0x50000, 0x42d00000) < 0);
 /* This device's LK descriptor (log fact, fed in -- never a default). */
 assert(x9_voice_usip_bind(&o, 0xf0d00000, 0x60000, 0x42d00000) == 0);
 assert(x9_voice_usip_bind(&o, 0xf0d00000, 0x60000, 0x42d00000) < 0);
 /* First pin asks the glue to map; a second acquire while the map is
  * in flight is an error, and AD50 still refuses (not qualified). */
 assert(x9_voice_usip_acquire(&o) == 1);
 assert(x9_voice_usip_acquire(&o) < 0);
 assert(x9_voice_usip_ad50_build(&o, 0, frame) < 0);
 /* A failed map rolls the pin back; the next acquire retries. */
 assert(x9_voice_usip_map_done(&o, 0) == 0);
 assert(o.refs == 0 && !o.mapped);
 assert(x9_voice_usip_acquire(&o) == 1);
 assert(x9_voice_usip_map_done(&o, 1) == 0);
 assert(x9_voice_usip_qualified(&o));
 /* AD50 frames match the reference-derived wire bytes for both scenes. */
 assert(x9_voice_usip_ad50_build(&o, 0, frame) == 0); hex(frame, text);
 assert(!strcmp(text, "88880021010050ad1000000000000000"
                      "0000d3f0000000000000030000000000"));
 assert(x9_voice_usip_ad50_build(&o, 19, frame) == 0); hex(frame, text);
 assert(!strcmp(text, "88881321010050ad1000000000000000"
                      "0000d3f0000000000000030000000000"));
 assert(x9_voice_usip_ad50_build(&o, 1, frame) < 0);
 /* Refcounted lifetime: pins nest, last release asks for the unmap. */
 assert(x9_voice_usip_acquire(&o) == 0 && o.refs == 2);
 assert(x9_voice_usip_release(&o) == 0 && o.refs == 1);
 assert(x9_voice_usip_release(&o) == 1 && o.refs == 0);
 assert(x9_voice_usip_release(&o) < 0);
 assert(x9_voice_usip_unmap_done(&o) == 0 && !o.mapped);
 assert(!x9_voice_usip_qualified(&o));
 assert(x9_voice_usip_ad50_build(&o, 0, frame) < 0);
 /* After unmap the owner can be pinned again. */
 assert(x9_voice_usip_acquire(&o) == 1);
 assert(x9_voice_usip_map_done(&o, 1) == 0);
 assert(x9_voice_usip_qualified(&o));
 printf("voice usip ok\n");
}
'''.replace('@HEADER@', str(HEADER))
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('voice usip ok', out)


if __name__ == '__main__':
    unittest.main()
