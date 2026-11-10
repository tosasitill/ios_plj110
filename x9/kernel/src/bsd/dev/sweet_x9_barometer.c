/* X9 BARO phase 1: bounded MTK sensor/2.0 ingress and diagnostic injection.
 * Wire reference: OPPO kernel_device_modules-6.12 sensor/2.0/{core,sensorhub}.
 * No SCP/I2C/MMIO/firmware operations here; no invented physical readings. */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include "../../iokit/Kernel/arm/X9Barometer.h"

static LCK_GRP_DECLARE(x9_baro_group, "x9_baro");
static LCK_MTX_DECLARE(x9_baro_lock, &x9_baro_group);
static struct x9_baro_sample latest;
static uint32_t mtk_gain;
static uint64_t last_scp_time;
static int active, test_mode, valid;

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* sensor_info.gain scales raw data to Android hPa; HID uses 16.16 kPa.
 * Use 64-bit arithmetic throughout: Pa does not fit signed 16.16 at sea level.
 * 20H392 locationd reads event type 31, field 0x1f0000 at 0x1004aaa38. */
static int
decode_mtk(const void *record, unsigned int length, uint32_t gain,
    uint32_t *pressure, uint64_t *scp_time)
{
 const uint8_t *p = record;
 if (!p || length != X9_BARO_MTK_DATA_BYTES || !gain) return EINVAL;
 if (p[0] != 6 || p[1] != 0) return ENOTSUP;
 int32_t raw = (int32_t)le32(p + 12);
 uint64_t time = le64(p + 4);
 if (raw <= 0 || !time || (time >> 63) || p[2] > 3) return ERANGE;
 /* Compare before rounding, in raw*100 / gain Pa. */
 uint64_t pa_numerator = (uint64_t)raw * 100;
 if (pa_numerator < (uint64_t)X9_BARO_MIN_PA * gain ||
     pa_numerator > (uint64_t)X9_BARO_MAX_PA * gain) return ERANGE;
 uint64_t denominator = (uint64_t)gain * 10;
 *pressure = (uint32_t)(((uint64_t)raw * 65536 + denominator / 2) / denominator);
 *scp_time = time;
 return 0;
}

/* Caller holds x9_baro_lock. Sequence stays monotonic across invalidation. */
static void
publish(uint32_t pressure, uint64_t scp_time, int diagnostic)
{
	latest.sequence++;
	latest.host_time = mach_absolute_time();
	latest.scp_time_ns = scp_time;
	latest.pressure_kpa_q16 = pressure;
	latest.diagnostic = diagnostic;
	valid = 1;
}

int
x9_baro_init(int diagnostic)
{
	lck_mtx_lock(&x9_baro_lock);
	if (active) {
		lck_mtx_unlock(&x9_baro_lock);
		return EBUSY;
	}
	active = 1;
	test_mode = diagnostic != 0;
	valid = 0;
	mtk_gain = 0;
	last_scp_time = 0;
	lck_mtx_unlock(&x9_baro_lock);
	return 0;
}

void
x9_baro_shutdown(void)
{
	lck_mtx_lock(&x9_baro_lock);
	active = test_mode = valid = 0;
	mtk_gain = 0;
	last_scp_time = 0;
	lck_mtx_unlock(&x9_baro_lock);
}

int
x9_baro_copy_sample(struct x9_baro_sample *sample)
{
	int ok;
	uint64_t age_ns;
	if (!sample) return EINVAL;
	lck_mtx_lock(&x9_baro_lock);
	ok = active && valid;
	if (ok) *sample = latest;
	lck_mtx_unlock(&x9_baro_lock);
	if (!ok) return ENOENT;
	absolutetime_to_nanoseconds(mach_absolute_time() - sample->host_time, &age_ns);
	return age_ns <= X9_BARO_MAX_AGE_NS ? 0 : ETIMEDOUT;
}

int
x9_baro_configure_mtk(const void *info, unsigned int length)
{
	const uint8_t *p = info;
	uint32_t gain;
	int error;
	if (!info || length != X9_BARO_MTK_INFO_BYTES) return EINVAL;
	if (p[0] != 6) return ENOTSUP;
	gain = le32(p + 4);
	if (!gain) return EINVAL;
	lck_mtx_lock(&x9_baro_lock);
	error = !active ? ENXIO : (test_mode ? EBUSY : 0);
	if (!error) {
		mtk_gain = gain;
		last_scp_time = 0;
		valid = 0;
	}
	lck_mtx_unlock(&x9_baro_lock);
	if (!error) printf("X9 BARO: MTK PRESSURE configured gain=%u; awaiting DATA (not a hardware enable)\n", gain);
	return error;
}

int
x9_baro_receive_mtk(const void *record, unsigned int length)
{
	uint32_t pressure;
	uint64_t scp_time;
	int error;
	lck_mtx_lock(&x9_baro_lock);
	error = !active ? ENXIO : (test_mode ? EBUSY : (!mtk_gain ? EAGAIN : 0));
	if (!error) error = decode_mtk(record, length, mtk_gain, &pressure, &scp_time);
	if (!error && scp_time <= last_scp_time) error = EALREADY;
	if (!error) {
		last_scp_time = scp_time;
		publish(pressure, scp_time, 0);
	}
	lck_mtx_unlock(&x9_baro_lock);
	return error;
}

void
x9_baro_invalidate_mtk(void)
{
	lck_mtx_lock(&x9_baro_lock);
	if (!test_mode) {
		valid = 0;
		mtk_gain = 0;
		last_scp_time = 0;
	}
	lck_mtx_unlock(&x9_baro_lock);
}

/* Explicit diagnostic only, root-only by sysctl policy; never continuously
 * synthesize pressure or mark a diagnostic as a physical sample. Input is Pa. */
static int
sysctl_x9_baro_test_pa SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
 struct x9_baro_sample sample;
 int value = x9_baro_copy_sample(&sample) == 0 ?
     (int)(((uint64_t)sample.pressure_kpa_q16 * 1000 + 32768) / 65536) : -1;
 int error = sysctl_handle_int(oidp, &value, 0, req);
 if (error || !req->newptr) return error;
 lck_mtx_lock(&x9_baro_lock);
 if (!active || !test_mode) error = EPERM;
 else if (value == -1) valid = 0;
 else if (value < (int)X9_BARO_MIN_PA || value > (int)X9_BARO_MAX_PA) error = ERANGE;
 else {
  uint8_t record[X9_BARO_MTK_DATA_BYTES] = {6, 0, 3, 0, 1};
  uint32_t raw = (uint32_t)value, pressure;
  uint64_t scp_time;
  for (unsigned i = 0; i < 4; i++) record[12 + i] = (uint8_t)(raw >> (8 * i));
  error = decode_mtk(record, sizeof(record), 100, &pressure, &scp_time);
  if (!error) publish(pressure, 0, 1);
 }
 lck_mtx_unlock(&x9_baro_lock);
 return error;
}
SYSCTL_PROC(_debug, OID_AUTO, x9_baro_test_pa, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_x9_baro_test_pa, "I", "X9 pressure diagnostic Pa (-1 invalid), requires x9_baro=2");
