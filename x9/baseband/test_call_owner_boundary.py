#!/usr/bin/env python3
"""Host ownership checks for the real call reducers/encoder; no MD or device I/O."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEV = ROOT / 'x9/kernel/src/bsd/dev'

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "x9_call_state.h"

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __func__, __LINE__, #cond); return 1; } } while (0)

static struct x9cw_progress progress(uint32_t id, unsigned direction, unsigned mode,
                                     unsigned event, unsigned cause) {
    struct x9cw_progress c;
    memset(&c, 0, sizeof(c));
    c.valid = 1; c.mask = 2; c.id = id; c.direction = direction; c.mode = mode;
    c.event = event; c.number.valid = 1; c.number.presentation = 0;
    if (cause) { c.has_cause = 1; c.cause = cause; }
    return c;
}
static struct x9cw_incoming incoming(uint32_t id, uint32_t sequence) {
    struct x9cw_incoming c;
    memset(&c, 0, sizeof(c));
    c.call_id = id; c.sequence = sequence; c.mode = 20; c.sim = 1;
    c.mask = 2; c.response_mask = 2; c.txid = 7;
    return c;
}
static struct x9cw_dial_profile profile(void) {
    struct x9cw_dial_profile p = {1, 2, 1, 0, 0, 0};
    return p;
}

static int outbound_lifecycle_and_late_end(void) {
    struct x9cw_outbound_state s; memset(&s, 0, sizeof(s));
    struct x9cw_progress c = progress(9, 0, 0, 130, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == 1);
    CHECK(s.seen && !s.ended && s.ios_state == 1 && s.epoch == 1);
    c = progress(9, 0, 20, 2, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == 1 && s.ios_state == 5);
    c = progress(9, 0, 20, 132, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == 1 && s.ios_state == 3);
    c = progress(9, 0, 20, 133, 16);
    CHECK(x9cw_outbound_apply(&s, &c) == 1 && s.ended && s.ios_state == 9);
    CHECK(s.end_reason.known && s.end_reason.apple == 145);
    uint64_t revision = s.revision;
    c = progress(9, 0, 20, 2, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == 0 && s.revision == revision && s.ended);
    unsigned id; CHECK(x9cw_outbound_id(&s, &id) == 0);
    puts("PASS outbound_lifecycle_and_late_end"); return 0;
}
static int mt_cannot_adopt_mo(void) {
    struct x9cw_outbound_state s; memset(&s, 0, sizeof(s));
    struct x9cw_progress c = progress(9, 0, 0, 130, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == 1);
    c = progress(9, 1, 20, 132, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == -1 && s.blocked);
    struct x9cw_outbound_state fresh; memset(&fresh, 0, sizeof(fresh));
    c = progress(3, 1, 20, 0, 0);
    CHECK(x9cw_outbound_apply(&fresh, &c) == -1 && fresh.blocked && !fresh.seen);
    puts("PASS mt_cannot_adopt_mo"); return 0;
}
static int inbound_lifecycle_and_new_epoch(void) {
    struct x9cw_inbound_state s; memset(&s, 0, sizeof(s));
    struct x9cw_incoming cmd = incoming(4, 100);
    CHECK(x9cw_inbound_begin(&s, &cmd) == 1 && s.pending && !s.seen);
    struct x9cw_progress c = progress(4, 1, 20, 0, 0);
    CHECK(x9cw_inbound_apply(&s, &c) == 1 && s.seen && s.ios_state == 2 && s.epoch == 1);
    c = progress(4, 1, 20, 132, 0);
    CHECK(x9cw_inbound_apply(&s, &c) == 1 && s.ios_state == 3);
    c = progress(4, 1, 20, 133, 16);
    CHECK(x9cw_inbound_apply(&s, &c) == 1 && s.ended && s.ios_state == 9);
    uint64_t revision = s.revision;
    c = progress(4, 1, 20, 0, 0);
    CHECK(x9cw_inbound_apply(&s, &c) == 0 && s.revision == revision && s.ended);
    cmd = incoming(5, 101);
    CHECK(x9cw_inbound_begin(&s, &cmd) == 1 && s.pending && !s.ended && s.native_id == 5);
    puts("PASS inbound_lifecycle_and_new_epoch"); return 0;
}
static int dial_join_requires_owned_empty_baseline(void) {
    struct x9cw_dial_profile p = profile();
    struct x9cw_outbound_state s; memset(&s, 0, sizeof(s));
    struct x9cw_dial_join j; memset(&j, 0, sizeof(j));
    CHECK(x9cw_dial_join_begin(&j, &s, &p) == 0);
    s.count_known = 1; s.native_count = 1;
    CHECK(x9cw_dial_join_begin(&j, &s, &p) == 0);
    s.native_count = 0; s.seen = 1; s.ended = 0; s.native_id = 9;
    CHECK(x9cw_dial_join_begin(&j, &s, &p) == 0);
    s.ended = 1;
    CHECK(x9cw_dial_join_begin(&j, &s, &p) == 1 && j.started);
    CHECK(x9cw_dial_join_sent(&j, 0) == 0);
    CHECK(x9cw_dial_join_sent(&j, 65535) == 0);
    CHECK(x9cw_dial_join_sent(&j, 77) == 1);
    CHECK(x9cw_dial_join_sent(&j, 78) == 0);
    unsigned id;
    CHECK(x9cw_dial_join_id(&j, &s, &id) == 0);
    j.confirmed = 1;
    struct x9cw_progress c = progress(10, 0, 0, 130, 0);
    CHECK(x9cw_outbound_apply(&s, &c) == 1);
    CHECK(x9cw_dial_join_id(&j, &s, &id) == 1 && id == 10);
    puts("PASS dial_join_requires_owned_empty_baseline"); return 0;
}
static int calllist_encoder_fail_closed(void) {
    uint8_t out[32];
    struct x9cw_end_reason reason = {1, 145};
    CHECK(x9cw_calllist(out, sizeof(out), 1, 3, 1, 2, 0, &reason) == 0);
    CHECK(x9cw_calllist(out, sizeof(out), 1, 4, 1, 2, 1, &reason) == 0);
    CHECK(x9cw_calllist(out, sizeof(out), 1, 3, 0, 2, 1, &reason) == 0);
    CHECK(x9cw_calllist(out, sizeof(out), 0, 3, 1, 2, 1, &reason) == 0);
    CHECK(x9cw_calllist(out, sizeof(out), 256, 3, 1, 2, 1, &reason) == 0);
    unsigned n = x9cw_calllist(out, sizeof(out), 1, 3, 1, 2, 1, &reason);
    CHECK(n == 18 && out[11] == 1 && out[12] == 3 && out[14] == 1 && out[15] == 2);
    n = x9cw_calllist(out, sizeof(out), 1, 9, 2, 2, 1, &reason);
    CHECK(n == 25 && out[12] == 9 && out[14] == 2);
    struct x9cw_end_reason unknown = {0, 0};
    n = x9cw_calllist(out, sizeof(out), 1, 9, 2, 2, 1, &unknown);
    CHECK(n == 18);
    puts("PASS calllist_encoder_fail_closed"); return 0;
}
static int end_reason_only_proven_cause(void) {
    struct x9cw_progress c = progress(9, 0, 20, 133, 16);
    struct x9cw_end_reason r = x9cw_end_reason_metadata(&c);
    CHECK(r.known && r.apple == 145);
    c = progress(9, 0, 20, 133, 17);
    r = x9cw_end_reason_metadata(&c);
    CHECK(!r.known);
    c = progress(9, 0, 20, 132, 16);
    r = x9cw_end_reason_metadata(&c);
    CHECK(!r.known);
    c = progress(9, 0, 0, 133, 16);
    r = x9cw_end_reason_metadata(&c);
    CHECK(!r.known);
    puts("PASS end_reason_only_proven_cause"); return 0;
}
static int number_presentation_fail_closed(void) {
    struct x9cw_number n; memset(&n, 0, sizeof(n));
    unsigned out;
    CHECK(x9cw_number_presentation(&n, &out) == 0);
    n.valid = 1; n.presentation = 0;
    CHECK(x9cw_number_presentation(&n, &out) == 1 && out == 0);
    n.presentation = 1;
    CHECK(x9cw_number_presentation(&n, &out) == 1 && out == 1);
    n.presentation = 2;
    CHECK(x9cw_number_presentation(&n, &out) == 1 && out == 2);
    n.presentation = 4;
    CHECK(x9cw_number_presentation(&n, &out) == 1 && out == 2);
    n.presentation = 5;
    CHECK(x9cw_number_presentation(&n, &out) == 0);
    puts("PASS number_presentation_fail_closed"); return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct { const char *name; int (*fn)(void); } cases[] = {
        {"outbound_lifecycle_and_late_end", outbound_lifecycle_and_late_end},
        {"mt_cannot_adopt_mo", mt_cannot_adopt_mo},
        {"inbound_lifecycle_and_new_epoch", inbound_lifecycle_and_new_epoch},
        {"dial_join_requires_owned_empty_baseline", dial_join_requires_owned_empty_baseline},
        {"calllist_encoder_fail_closed", calllist_encoder_fail_closed},
        {"end_reason_only_proven_cause", end_reason_only_proven_cause},
        {"number_presentation_fail_closed", number_presentation_fail_closed},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].fn();
    return 2;
}
'''


class CallOwnerBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='x9-call-host-')
        cls.dir = Path(cls.tmp.name)
        (cls.dir / 'harness.c').write_text(HARNESS)
        cc = os.environ.get('CC', 'clang')
        sdk = os.environ.get('SDKROOT')
        cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-I', str(DEV), str(cls.dir / 'harness.c'), '-o', str(cls.dir / 'harness')]
        if sdk:
            cmd[1:1] = ['-isysroot', sdk]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stderr + build.stdout)
        cls.binary = cls.dir / 'harness'

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn(f'PASS {name}', result.stdout)

    def test_outbound_lifecycle_and_late_end_cannot_revive(self):
        self.run_case('outbound_lifecycle_and_late_end')

    def test_mt_progress_cannot_adopt_or_complete_mo(self):
        self.run_case('mt_cannot_adopt_mo')

    def test_inbound_lifecycle_and_new_epoch_after_end(self):
        self.run_case('inbound_lifecycle_and_new_epoch')

    def test_dial_join_requires_owned_empty_baseline_and_real_token(self):
        self.run_case('dial_join_requires_owned_empty_baseline')

    def test_calllist_encoder_requires_proven_technology_and_state(self):
        self.run_case('calllist_encoder_fail_closed')

    def test_end_reason_requires_proven_native_cause(self):
        self.run_case('end_reason_only_proven_cause')

    def test_number_presentation_does_not_disclose_unknown(self):
        self.run_case('number_presentation_fail_closed')

    def test_publication_and_dial_submit_keep_runtime_fences(self):
        source = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Calls.inc').read_text()
        self.assertIn('c.bridge_generation!=p.bridge_generation', source)
        self.assertIn('!x9c_bound_live(p.slot,p.sim,p.generation,p.binding)', source)
        self.assertIn('id!=p.id||direction!=p.direction||epoch!=p.epoch||revision!=p.revision', source)
        self.assertIn('x9c_bind_pending(slot,true)||x9c_dial_job.used', source)
        self.assertIn('!x9cw_dial_join_begin(&join,&state,&profile)', source)
        self.assertIn('if(ios_state!=9||!same_call)return;', source)
        self.assertIn('technology=c.calllist_technology;', source)


if __name__ == '__main__':
    unittest.main(verbosity=2)
