#!/usr/bin/env python3
"""Host-test the call-audio probe decision/statistics logic (x9_call_probe.h)."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / 'x9/kernel/src/bsd/dev/x9_call_probe.h'


class CallProbe(unittest.TestCase):
    def test_probe(self):
        code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "@HEADER@"
int main() {
 static struct x9_call_probe p; memset(&p, 0, sizeof p);
 /* An end without a seen call, and unknown states, do nothing. */
 assert(x9cp_observe(&p, 1, 7, 9) == X9CP_NONE);
 assert(x9cp_observe(&p, 1, 7, 4) == X9CP_NONE);
 /* MO: dialing -> alerting -> active -> ended; republish is a duplicate. */
 assert(x9cp_observe(&p, 1, 7, 1) == X9CP_START && p.calls == 1);
 assert(x9cp_observe(&p, 1, 7, 1) == X9CP_NONE);
 assert(x9cp_observe(&p, 1, 7, 5) == X9CP_SNAP);
 assert(x9cp_observe(&p, 1, 7, 3) == X9CP_SNAP);
 assert(x9cp_observe(&p, 1, 7, 9) == X9CP_END && !p.active);
 assert(x9cp_observe(&p, 1, 7, 9) == X9CP_NONE);
 /* A different call while one is up: END for the old one (identity kept for
  * the report), then START on the second pass. */
 assert(x9cp_observe(&p, 0, 3, 2) == X9CP_START && p.calls == 2);
 assert(x9cp_observe(&p, 1, 4, 1) == X9CP_END && p.sim == 0 && p.id == 3);
 assert(x9cp_observe(&p, 1, 4, 1) == X9CP_START && p.sim == 1 && p.id == 4 && p.calls == 3);

 /* ch4 print budget only while a call is up. */
 for (unsigned i = 0; i < X9CP_AUD_LOG_MAX; i++) assert(x9cp_log_aud(&p) == 1);
 assert(x9cp_log_aud(&p) == 0);
 p.active = 0; p.aud_logged = 0; assert(x9cp_log_aud(&p) == 0);

 /* Channel deltas: only non-zero, rebased, wrap-safe, capped. */
 static uint32_t now[X9CP_CH]; memset(now, 0, sizeof now); memset(p.base, 0, sizeof p.base);
 uint32_t d[4][2];
 now[4] = 3; now[216] = 9;
 assert(x9cp_channel_deltas(&p, now, d, 4) == 2 && d[0][0] == 4 && d[0][1] == 3 && d[1][0] == 216);
 assert(x9cp_channel_deltas(&p, now, d, 4) == 0);
 p.base[10] = 0xfffffffe; now[10] = 1;
 assert(x9cp_channel_deltas(&p, now, d, 4) == 1 && d[0][0] == 10 && d[0][1] == 3);
 for (unsigned c = 0; c < 10; c++) now[c] += 1;
 assert(x9cp_channel_deltas(&p, now, d, 4) == 4);

 /* Region stats: all-zero has signature 0; content/position changes move it. */
 static uint32_t w[256]; memset(w, 0, sizeof w);
 struct x9cp_region a, b, c;
 x9cp_region_stat(w, 256, &a);
 assert(a.nonzero == 0 && a.signature == 0 && a.first_nonzero == 0xffffffffu);
 w[5] = 0x2f20; x9cp_region_stat(w, 256, &b);
 assert(b.nonzero == 1 && b.first_nonzero == 5 && b.signature != 0);
 w[5] = 0; w[6] = 0x2f20; x9cp_region_stat(w, 256, &c);
 assert(c.nonzero == 1 && c.signature != b.signature);
 puts("call probe ok");
}
'''.replace('@HEADER@', str(HEADER))
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('call probe ok', out)


if __name__ == '__main__':
    unittest.main()
