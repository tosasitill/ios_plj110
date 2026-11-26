#!/usr/bin/env python3
"""Host wake-gesture boundary checks: real motion admission, no synthetic wake."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BACKEND = ROOT / 'x9/kernel/src/bsd/dev/sweet_x9_motion.c'

STUBS = {
    'sys/systm.h': '#include <stdio.h>\n#include <string.h>\n#include <stdint.h>\n',
    'sys/errno.h': r'''
#ifndef X9_HOST_ERRNO_STUB
#define X9_HOST_ERRNO_STUB
enum { EPERM = 1, ENOENT = 2, ENXIO = 6, EPIPE = 32, EBUSY = 16, EINVAL = 22,
       ERANGE = 34, EAGAIN = 35, EALREADY = 37, ENOTSUP = 45, ETIMEDOUT = 60,
       EBADMSG = 94 };
#endif
''',
    'sys/kauth.h': r'''
typedef unsigned uid_t;
static inline void *kauth_cred_get(void) { return NULL; }
static inline uid_t kauth_cred_getuid(void *cred) { (void)cred; return 0; }
''',
    'sys/sysctl.h': r'''
#include <stdint.h>
#include <string.h>
struct sysctl_oid { int dummy; };
struct sysctl_req { void *oldptr; size_t oldlen; const void *newptr; size_t newlen; };
#define SYSCTL_HANDLER_ARGS (struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
#define OID_AUTO 0
#define CTLTYPE_INT 0
#define CTLTYPE_OPAQUE 0
#define CTLTYPE_STRING 0
#define CTLFLAG_RD 0
#define CTLFLAG_RW 0
#define CTLFLAG_ANYBODY 0
#define CTLFLAG_LOCKED 0
#define SYSCTL_PROC(parent,oid,name,flags,a1,a2,handler,fmt,desc) \
    static void *const __sysctl_ref_##name __attribute__((used)) = (void *)(handler)
static inline int sysctl_out_stub(struct sysctl_req *req, const void *data, size_t len) {
    if (req->oldptr && req->oldlen >= len) memcpy(req->oldptr, data, len);
    return 0;
}
#define SYSCTL_OUT(req, ptr, len) sysctl_out_stub((req), (ptr), (len))
static inline int sysctl_handle_int(struct sysctl_oid *oidp, int *value, int arg2,
                                    struct sysctl_req *req) {
    (void)oidp; (void)arg2;
    if (req->oldptr && req->oldlen >= sizeof(int)) memcpy(req->oldptr, value, sizeof(int));
    if (req->newptr && req->newlen >= sizeof(int)) memcpy(value, req->newptr, sizeof(int));
    return 0;
}
static inline int sysctl_handle_string(struct sysctl_oid *oidp, char *value, size_t len,
                                       struct sysctl_req *req) {
    (void)oidp;
    if (req->oldptr && req->oldlen >= len) memcpy(req->oldptr, value, len);
    if (req->newptr && req->newlen >= len) memcpy(value, req->newptr, len);
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
#include "sweet_x9_motion.c"

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __func__, __LINE__, #cond); return 1; } } while (0)

static void info(uint8_t out[X9_MOTION_INFO_BYTES]) {
    memset(out, 0, X9_MOTION_INFO_BYTES);
    out[0] = X9_MOTION_MTK_TYPE; out[4] = 0x40; out[5] = 0x01; /* gain 65856 */
}
static void data(uint8_t out[X9_MOTION_DATA_BYTES], uint64_t t, int32_t x) {
    memset(out, 0, X9_MOTION_DATA_BYTES);
    out[0] = X9_MOTION_MTK_TYPE; out[2] = 3;
    for (unsigned i = 0; i < 8; i++) out[4+i] = (uint8_t)(t >> (8*i));
    for (unsigned i = 0; i < 4; i++) out[12+i] = (uint8_t)((uint32_t)x >> (8*i));
}
static int ready(void) {
    uint8_t i[X9_MOTION_INFO_BYTES]; info(i);
    CHECK(x9_motion_init() == 0);
    CHECK(x9_motion_native_configure(i, sizeof(i)) == 0);
    return 0;
}
static int enable_ack(void) {
    x9_motion_native_control(1, 20000, 0);
    struct x9_motion_status st; x9_motion_copy_status(&st);
    CHECK(st.enabled == 1 && st.error == 0);
    return 0;
}

static int enable_ack_data(void) {
    CHECK(ready() == 0);
    uint8_t r[X9_MOTION_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_motion_native_data(r, sizeof(r)) == EAGAIN);
    CHECK(enable_ack() == 0);
    CHECK(x9_motion_native_data(r, sizeof(r)) == 0);
    struct x9_motion_sample s; CHECK(x9_motion_copy_sample(&s) == 0);
    CHECK(s.version == X9_MOTION_ABI && s.sequence == 1 && s.fixed_g[0] == 0);
    puts("PASS enable_ack_data"); return 0;
}
static int stale_data_expires(void) {
    CHECK(ready() == 0); CHECK(enable_ack() == 0);
    uint8_t r[X9_MOTION_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_motion_native_data(r, sizeof(r)) == 0);
    test_host_time += 600ULL * 1000000ULL;
    struct x9_motion_sample s; CHECK(x9_motion_copy_sample(&s) == ETIMEDOUT);
    puts("PASS stale_data_expires"); return 0;
}
static int disconnect_invalidates(void) {
    CHECK(ready() == 0); CHECK(enable_ack() == 0);
    uint8_t r[X9_MOTION_DATA_BYTES]; data(r, 100, 0);
    CHECK(x9_motion_native_data(r, sizeof(r)) == 0);
    x9_motion_native_disconnect(EPIPE);
    struct x9_motion_sample s; CHECK(x9_motion_copy_sample(&s) == EPIPE);
    struct x9_motion_status st; x9_motion_copy_status(&st);
    CHECK(st.configured == 0 && st.enabled == 0 && st.error == EPIPE);
    CHECK(x9_motion_native_data(r, sizeof(r)) == EPIPE);
    puts("PASS disconnect_invalidates"); return 0;
}
static int malformed_records_rejected(void) {
    CHECK(ready() == 0); CHECK(enable_ack() == 0);
    uint8_t r[X9_MOTION_DATA_BYTES]; data(r, 100, 0);
    r[0] = 2; CHECK(x9_motion_native_data(r, sizeof(r)) == ENOTSUP);
    data(r, 100, 0); r[2] = 4; CHECK(x9_motion_native_data(r, sizeof(r)) == ERANGE);
    data(r, 0, 0); CHECK(x9_motion_native_data(r, sizeof(r)) == ERANGE);
    data(r, 100, 0); CHECK(x9_motion_native_data(r, sizeof(r)) == 0);
    CHECK(x9_motion_native_data(r, sizeof(r)) == EALREADY);
    puts("PASS malformed_records_rejected"); return 0;
}
static int late_ack_cannot_clear_fault(void) {
    CHECK(ready() == 0);
    x9_motion_native_disconnect(EPIPE);
    x9_motion_native_control(1, 20000, 0);
    struct x9_motion_status st; x9_motion_copy_status(&st);
    CHECK(st.error == EPIPE && st.enabled == 0);
    puts("PASS late_ack_cannot_clear_fault"); return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct { const char *name; int (*fn)(void); } cases[] = {
        {"enable_ack_data", enable_ack_data},
        {"stale_data_expires", stale_data_expires},
        {"disconnect_invalidates", disconnect_invalidates},
        {"malformed_records_rejected", malformed_records_rejected},
        {"late_ack_cannot_clear_fault", late_ack_cannot_clear_fault},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].fn();
    return 2;
}
'''


class WakeGestureBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='x9-motion-host-')
        cls.dir = Path(cls.tmp.name)
        for rel, text in STUBS.items():
            path = cls.dir / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (cls.dir / 'harness.c').write_text(HARNESS)
        cc = os.environ.get('CC', 'clang')
        sdk = os.environ.get('SDKROOT')
        cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
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

    def test_motion_requires_real_enable_ack_before_data(self):
        self.run_case('enable_ack_data')

    def test_motion_sample_expires_when_stale(self):
        self.run_case('stale_data_expires')

    def test_disconnect_invalidates_motion_and_late_ack_cannot_clear(self):
        self.run_case('disconnect_invalidates')
        self.run_case('late_ack_cannot_clear_fault')

    def test_malformed_motion_records_are_rejected(self):
        self.run_case('malformed_records_rejected')

    def test_no_synthetic_wake_shake_touch_or_gesture_contract(self):
        motion = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Motion.h').read_text()
        self.assertNotIn('receive_mtk', motion)
        self.assertNotIn('configure_mtk', motion)
        bridge = (HERE / 'orientation_bridge.m').read_text()
        for forbidden in ('UIEvent', 'UIApplication', 'wake', 'Wake', 'shake', 'Shake',
                          'S3910', 'touch', 'Touch', 'IOHIDEvent', 'pickup', 'Pickup'):
            self.assertNotIn(forbidden, bridge)
        self.assertIn('gravity orientation estimator', bridge)
        self.assertIn('no synthetic orientation callback', bridge)
        accel = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/hid/X9Accelerometer.inc').read_text()
        self.assertNotIn('kIOHIDEventTypeKeyboard', accel)
        self.assertNotIn('kIOHIDEventTypePower', accel)
        self.assertNotIn('wake', accel.lower())


if __name__ == '__main__':
    unittest.main(verbosity=2)