#!/usr/bin/env python3
"""Host admission checks for the real proximity backend; no device or SCP I/O."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BACKEND = ROOT / 'x9/kernel/src/bsd/dev/sweet_x9_proximity.c'

STUBS = {
    'sys/systm.h': '#include <stdio.h>\n#include <string.h>\n#include <stdint.h>\n',
    'sys/errno.h': r'''
#ifndef X9_HOST_ERRNO_STUB
#define X9_HOST_ERRNO_STUB
enum { EPERM = 1, ENOENT = 2, ENXIO = 6, EBUSY = 16, EINVAL = 22,
       ERANGE = 34, EAGAIN = 35, EALREADY = 37, ENOTSUP = 45,
       ETIMEDOUT = 60, EPROTO = 71, EBADMSG = 94 };
#endif
''',
    'sys/sysctl.h': r'''
#include <stdint.h>
#include <string.h>
struct sysctl_oid { int dummy; };
struct sysctl_req { void *oldptr; size_t oldlen; const void *newptr; size_t newlen; };
#define SYSCTL_HANDLER_ARGS (struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
#define OID_AUTO 0
#define CTLTYPE_INT 0
#define CTLFLAG_RW 0
#define CTLFLAG_LOCKED 0
#define SYSCTL_PROC(parent,oid,name,flags,a1,a2,handler,fmt,desc) \
    static void *const __sysctl_ref_##name __attribute__((used)) = (void *)(handler)
static inline int sysctl_handle_int(struct sysctl_oid *oidp, int *value, int arg2,
                                    struct sysctl_req *req) {
    (void)oidp; (void)arg2;
    if (req->oldptr && req->oldlen >= sizeof(int)) memcpy(req->oldptr, value, sizeof(int));
    if (req->newptr && req->newlen >= sizeof(int)) memcpy(value, req->newptr, sizeof(int));
    return 0;
}
''',
    'kern/clock.h': r'''
#include <stdint.h>
extern uint64_t test_host_time;
static inline uint64_t mach_absolute_time(void) { return test_host_time; }
static inline void absolutetime_to_nanoseconds(uint64_t delta, uint64_t *ns) { *ns = delta; }
''',
    'kern/locks.h': r'''
typedef int lck_mtx_t;
#define LCK_GRP_DECLARE(name, desc) int name##_grp
#define LCK_MTX_DECLARE(name, grp) lck_mtx_t name
static inline void lck_mtx_lock(lck_mtx_t *lock) { (void)lock; }
static inline void lck_mtx_unlock(lck_mtx_t *lock) { (void)lock; }
''',
    'libkern/libkern.h': '#include <string.h>\n',
}

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint64_t test_host_time = 1000;
#include "sweet_x9_proximity.c"

static int req_calls, req_enabled, req_interval, req_error;
static int request(uint32_t enabled, uint32_t interval) {
    req_calls++; req_enabled = (int)enabled; req_interval = (int)interval;
    return req_error;
}
static const struct x9_prox_transport test_transport = { request };
static void info(uint8_t out[X9_PROX_INFO_BYTES]) {
    memset(out, 0, X9_PROX_INFO_BYTES); out[0] = X9_PROX_MTK_TYPE; out[4] = 1;
}
static void data(uint8_t out[X9_PROX_DATA_BYTES], uint64_t t, int32_t oneshot) {
    memset(out, 0, X9_PROX_DATA_BYTES);
    out[0] = X9_PROX_MTK_TYPE; out[2] = 3;
    for (unsigned i = 0; i < 8; i++) out[4+i] = (uint8_t)(t >> (8*i));
    for (unsigned i = 0; i < 4; i++) out[12+i] = (uint8_t)((uint32_t)oneshot >> (8*i));
}
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __func__, __LINE__, #cond); return 1; } } while (0)

static int native_ready(void) {
    uint8_t i[X9_PROX_INFO_BYTES]; info(i);
    CHECK(x9_prox_init(3) == 0);
    CHECK(x9_prox_bind_transport(&test_transport) == 0);
    CHECK(x9_prox_native_configure(i, sizeof(i)) == 0);
    return 0;
}
static int enable_ack(void) {
    CHECK(x9_prox_set_enabled(1, 200000) == 0);
    CHECK(req_calls == 1 && req_enabled == 1);
    x9_prox_native_control(1, 0);
    struct x9_prox_status st; x9_prox_copy_status(&st);
    CHECK(st.enabled == 1 && st.pending == 0 && st.error == 0);
    return 0;
}

static int enable_ack_data(void) {
    CHECK(native_ready() == 0);
    uint8_t r[X9_PROX_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_prox_native_data(r, sizeof(r)) == EAGAIN);
    CHECK(enable_ack() == 0);
    CHECK(x9_prox_native_data(r, sizeof(r)) == 0);
    struct x9_prox_sample s; CHECK(x9_prox_copy_sample(&s) == 0);
    CHECK(s.near == 1 && s.oneshot == 0 && s.diagnostic == 0);
    puts("PASS enable_ack_data"); return 0;
}
static int on_change_no_expiry(void) {
    CHECK(native_ready() == 0); CHECK(enable_ack() == 0);
    uint8_t r[X9_PROX_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_prox_native_data(r, sizeof(r)) == 0);
    test_host_time += 60ULL * 1000000000ULL;
    struct x9_prox_sample s; CHECK(x9_prox_copy_sample(&s) == 0);
    CHECK(s.near == 1);
    puts("PASS on_change_no_expiry"); return 0;
}
static int cadence_pending_enable(void) {
    CHECK(native_ready() == 0); CHECK(enable_ack() == 0);
    uint8_t r[X9_PROX_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_prox_native_data(r, sizeof(r)) == 0);
    CHECK(x9_prox_set_enabled(1, 400000) == 0);
    CHECK(req_calls == 2 && req_interval == 400000);
    data(r, 200, 1);
    CHECK(x9_prox_native_data(r, sizeof(r)) == 0);
    struct x9_prox_sample s; CHECK(x9_prox_copy_sample(&s) == 0);
    CHECK(s.near == 0 && s.oneshot == 1);
    x9_prox_native_control(1, 0);
    struct x9_prox_status st; x9_prox_copy_status(&st);
    CHECK(st.enabled == 1 && st.pending == 0 && st.interval_us == 400000);
    puts("PASS cadence_pending_enable"); return 0;
}
static int pending_disable_rejects(void) {
    CHECK(native_ready() == 0); CHECK(enable_ack() == 0);
    uint8_t r[X9_PROX_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_prox_native_data(r, sizeof(r)) == 0);
    CHECK(x9_prox_set_enabled(0, 200000) == 0);
    data(r, 200, 1);
    CHECK(x9_prox_native_data(r, sizeof(r)) == EAGAIN);
    x9_prox_native_control(0, 0);
    struct x9_prox_sample s; CHECK(x9_prox_copy_sample(&s) == ENOENT);
    puts("PASS pending_disable_rejects"); return 0;
}
static int mismatch_ack_fail_closed(void) {
    CHECK(native_ready() == 0);
    CHECK(x9_prox_set_enabled(1, 200000) == 0);
    x9_prox_native_control(0, 0);
    struct x9_prox_status st; x9_prox_copy_status(&st);
    CHECK(st.error == EPROTO && st.pending == 0 && st.enabled == 0);
    CHECK(x9_prox_set_enabled(1, 200000) == EPROTO);
    puts("PASS mismatch_ack_fail_closed"); return 0;
}
static int timeout_fail_closed(void) {
    CHECK(native_ready() == 0);
    CHECK(x9_prox_set_enabled(1, 200000) == 0);
    test_host_time += 2ULL * 1000000000ULL;
    struct x9_prox_sample s; CHECK(x9_prox_copy_sample(&s) == ETIMEDOUT);
    struct x9_prox_status st; x9_prox_copy_status(&st);
    CHECK(st.error == ETIMEDOUT && st.pending == 0 && st.enabled == 0);
    x9_prox_native_control(1, 0);
    x9_prox_copy_status(&st);
    CHECK(st.error == ETIMEDOUT && st.enabled == 0);
    puts("PASS timeout_fail_closed"); return 0;
}
static int external_mode_gate(void) {
    CHECK(x9_prox_init(1) == 0);
    uint8_t i[X9_PROX_INFO_BYTES]; info(i);
    CHECK(x9_prox_native_configure(i, sizeof(i)) == EBUSY);
    CHECK(x9_prox_configure_mtk(i, sizeof(i)) == 0);
    CHECK(x9_prox_set_enabled(1, 200000) == 0);
    uint8_t r[X9_PROX_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_prox_receive_mtk(r, sizeof(r)) == 0);
    CHECK(x9_prox_native_data(r, sizeof(r)) == EBUSY);
    puts("PASS external_mode_gate"); return 0;
}
static int diagnostic_polarity(void) {
    CHECK(x9_prox_init(2) == 0);
    CHECK(x9_prox_set_enabled(1, 200000) == 0);
    struct x9_prox_sample s;
    CHECK(x9_prox_copy_sample(&s) == ENOENT);
    uint8_t r[X9_PROX_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_prox_receive_mtk(r, sizeof(r)) == EBUSY);
    /* The diagnostic sysctl follows the existing unverified host mapping:
     * value 1 synthesizes oneshot 0 (near), value 0 synthesizes oneshot 1. */
    int value = 1;
    struct sysctl_req req = { NULL, 0, &value, sizeof(value) };
    CHECK(sysctl_prox_test(NULL, NULL, 0, &req) == 0);
    CHECK(x9_prox_copy_sample(&s) == 0);
    CHECK(s.near == 1 && s.diagnostic == 1);
    value = 0; req.newptr = &value;
    CHECK(sysctl_prox_test(NULL, NULL, 0, &req) == 0);
    CHECK(x9_prox_copy_sample(&s) == 0);
    CHECK(s.near == 0 && s.diagnostic == 1);
    puts("PASS diagnostic_polarity"); return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct { const char *name; int (*fn)(void); } cases[] = {
        {"enable_ack_data", enable_ack_data},
        {"on_change_no_expiry", on_change_no_expiry},
        {"cadence_pending_enable", cadence_pending_enable},
        {"pending_disable_rejects", pending_disable_rejects},
        {"mismatch_ack_fail_closed", mismatch_ack_fail_closed},
        {"timeout_fail_closed", timeout_fail_closed},
        {"external_mode_gate", external_mode_gate},
        {"diagnostic_polarity", diagnostic_polarity},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].fn();
    return 2;
}
'''


class ProximityBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='x9-prox-host-')
        cls.dir = Path(cls.tmp.name)
        for rel, text in STUBS.items():
            path = cls.dir / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (cls.dir / 'harness.c').write_text(HARNESS)
        cc = os.environ.get('CC', 'clang')
        sdk = os.environ.get('SDKROOT')
        cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I', str(cls.dir), '-I', str(BACKEND.parent),
               str(cls.dir / 'harness.c'), '-o', str(cls.dir / 'harness')]
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

    def test_enable_requires_real_ack_before_data(self):
        self.run_case('enable_ack_data')

    def test_on_change_level_does_not_expire(self):
        self.run_case('on_change_no_expiry')

    def test_cadence_only_pending_enable_accepts_real_edge(self):
        self.run_case('cadence_pending_enable')

    def test_pending_disable_rejects_data(self):
        self.run_case('pending_disable_rejects')

    def test_mismatched_ack_fails_closed(self):
        self.run_case('mismatch_ack_fail_closed')

    def test_control_timeout_fails_closed_and_late_ack_cannot_revive(self):
        self.run_case('timeout_fail_closed')

    def test_external_and_native_modes_do_not_cross_admit(self):
        self.run_case('external_mode_gate')

    def test_diagnostic_mapping_is_explicit_and_unverified(self):
        self.run_case('diagnostic_polarity')


if __name__ == '__main__':
    unittest.main(verbosity=2)