/* X9 ALS: MTK sensor/2.0 native SCP transport, ingress and diagnostics.
 * Wire reference: OPPO kernel_device_modules-6.12 sensor/2.0/{core,sensorhub}.
 * Mode 3 starts the LK-loaded secure firmware; mode 2 is synthetic only.
 * No AP sensor I2C, partition writes or invented physical readings. */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include "../../iokit/Kernel/arm/X9ALS.h"
#include "../../iokit/Kernel/arm/X9Proximity.h"

static LCK_GRP_DECLARE(x9_als_group, "x9_als");
static LCK_MTX_DECLARE(x9_als_lock, &x9_als_group);
static struct x9_als_sample latest;
static uint32_t mtk_gain;
static uint64_t last_scp_time;
static int active, test_mode, valid;
static int physical_mode, transport_started;
static int ingress_claimed, shared_transport_requested;
static struct x9_als_status status = { .report_interval_us = 200000 };
static uint32_t requested_interval_us = 200000;

void x9_als_copy_status(struct x9_als_status *out)
{
 if (!out) return;
 lck_mtx_lock(&x9_als_lock); *out = status; lck_mtx_unlock(&x9_als_lock);
}
uint32_t x9_als_get_report_interval_us(void)
{
 lck_mtx_lock(&x9_als_lock); uint32_t us = status.report_interval_us;
 lck_mtx_unlock(&x9_als_lock); return us;
}
int x9_als_set_report_interval_us(uint32_t us)
{
 if (us < 20000 || us > 1000000) return EINVAL;
 lck_mtx_lock(&x9_als_lock);
 int e = !active ? ENXIO : status.error ? status.error :
     physical_mode && (!status.control_enabled || requested_interval_us != status.report_interval_us) ? EBUSY : 0;
 if (!e) {
  requested_interval_us = us;
  if (!physical_mode) status.report_interval_us = us; /* host cadence only */
 }
 lck_mtx_unlock(&x9_als_lock); return e;
}

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

/* Byte decoding tolerates packed/unaligned records. Do not mistake proximity,
 * raw/calibration/flush packets, accuracy, or a raw ADC channel for lux. */
static int
decode_mtk(const void *record, unsigned int length, uint32_t gain,
    uint32_t *lux, uint64_t *scp_time)
{
	const uint8_t *p = record;
	int32_t raw;
	uint64_t time, level;
	if (!record || length != X9_ALS_MTK_DATA_BYTES || !gain) return EINVAL;
	if (p[0] != 5 || p[1] != 0) return ENOTSUP; /* LIGHT / DATA_ACTION */
	raw = (int32_t)le32(p + 12);
	time = le64(p + 4);
 if (raw < 0 || !time || (time >> 63) || p[2] > 3) return ERANGE;
	level = ((uint64_t)raw + gain / 2u) / gain;
	if (level > X9_ALS_MAX_LUX) return ERANGE;
	*lux = (uint32_t)level;
	*scp_time = time;
	return 0;
}

/* Caller holds x9_als_lock. Sequence stays monotonic across invalidation. */
static void
publish(uint32_t lux, uint64_t scp_time, int diagnostic)
{
	latest.sequence++;
	latest.host_time = mach_absolute_time();
	latest.scp_time_ns = scp_time;
	latest.lux = lux;
	latest.diagnostic = diagnostic;
	valid = 1;
}

int
x9_als_init(int diagnostic)
{
	lck_mtx_lock(&x9_als_lock);
 if (active || transport_started) {
		/* Another HID may have started the shared worker before ALS. */
		if (!diagnostic && !test_mode && active && physical_mode && !ingress_claimed) {
			ingress_claimed = 1;
			lck_mtx_unlock(&x9_als_lock);
			return 0;
		}
		lck_mtx_unlock(&x9_als_lock);
		return EBUSY;
	}
	active = 1;
	ingress_claimed = 1;
	test_mode = diagnostic != 0;
	valid = 0;
	mtk_gain = 0;
	last_scp_time = 0;
	lck_mtx_unlock(&x9_als_lock);
	return 0;
}

void
x9_als_shutdown(void)
{
	lck_mtx_lock(&x9_als_lock);
	ingress_claimed = 0;
	if (shared_transport_requested) {
		valid = 0; /* next real sample can be cached; shared owner stays up */
		lck_mtx_unlock(&x9_als_lock);
		return;
	}
	active = test_mode = valid = 0;
	mtk_gain = 0;
	last_scp_time = 0;
	/* Native worker observes active=0, sends a real DISABLE if it owns a
	 * confirmed stream, then exits. It retains secure votes/mappings if the
	 * remote may still run; stop/start is NOT permission to reset SCP. */
	lck_mtx_unlock(&x9_als_lock);
}

int
x9_als_copy_sample(struct x9_als_sample *sample)
{
	int ok;
	uint64_t age_ns;
	if (!sample) return EINVAL;
	lck_mtx_lock(&x9_als_lock);
	ok = active && valid;
	if (ok) *sample = latest;
	lck_mtx_unlock(&x9_als_lock);
	if (!ok) return ENOENT;
	absolutetime_to_nanoseconds(mach_absolute_time() - sample->host_time, &age_ns);
	return age_ns <= X9_ALS_MAX_AGE_NS ? 0 : ETIMEDOUT;
}

int
x9_als_configure_mtk(const void *info, unsigned int length)
{
	const uint8_t *p = info;
	uint32_t gain;
	int error;
	if (!info || length != X9_ALS_MTK_INFO_BYTES) return EINVAL;
	if (p[0] != 5) return ENOTSUP;
	gain = le32(p + 4);
	if (!gain) return EINVAL;
	lck_mtx_lock(&x9_als_lock);
 error = !active ? ENXIO : physical_mode ? EPERM : (test_mode ? EBUSY : 0);
	if (!error) {
		mtk_gain = gain;
		last_scp_time = 0;
		valid = 0;
	}
	lck_mtx_unlock(&x9_als_lock);
	if (!error) printf("X9 ALS: MTK LIGHT configured gain=%u; awaiting DATA (not a hardware enable)\n", gain);
	return error;
}

int
x9_als_receive_mtk(const void *record, unsigned int length)
{
	uint32_t lux;
	uint64_t scp_time;
	int error;
	lck_mtx_lock(&x9_als_lock);
 error = !active ? ENXIO : status.error ? status.error :
     physical_mode && !status.control_enabled ? EAGAIN :
     (test_mode ? EBUSY : (!mtk_gain ? EAGAIN : 0));
	if (!error) error = decode_mtk(record, length, mtk_gain, &lux, &scp_time);
	if (!error && scp_time <= last_scp_time) error = EALREADY;
	if (!error) {
		last_scp_time = scp_time;
		publish(lux, scp_time, 0);
	}
	lck_mtx_unlock(&x9_als_lock);
	return error;
}

void
x9_als_invalidate_mtk(void)
{
	lck_mtx_lock(&x9_als_lock);
 if (!test_mode && !physical_mode) {
		valid = 0;
		mtk_gain = 0;
		last_scp_time = 0;
	}
	lck_mtx_unlock(&x9_als_lock);
}

/* Root-only sysctl, additionally gated by explicit x9_als=2 diagnostic mode.
 * Writing -1 invalidates. Other writes traverse the same MTK byte decoder,
 * then enter the same latest-sample -> HID path, labelled diagnostic. */
static int
sysctl_x9_als_test_lux SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	struct x9_als_sample sample;
	int value = x9_als_copy_sample(&sample) == 0 ? (int)sample.lux : -1;
	int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	lck_mtx_lock(&x9_als_lock);
	if (!active || !test_mode) error = EPERM;
	else if (value == -1) valid = 0;
	else if (value < 0 || value > (int)X9_ALS_MAX_LUX) error = ERANGE;
	else {
		uint8_t record[X9_ALS_MTK_DATA_BYTES] = {5, 0, 3, 0, 1};
		uint32_t raw = (uint32_t)value * 1000u;
		uint32_t lux;
		uint64_t scp_time;
		for (unsigned i = 0; i < 4; i++) record[12 + i] = (uint8_t)(raw >> (8 * i));
		error = decode_mtk(record, sizeof(record), 1000, &lux, &scp_time);
		if (!error) publish(lux, 0, 1); /* synthetic SCP timestamp not exported */
	}
	lck_mtx_unlock(&x9_als_lock);
	return error;
}
SYSCTL_PROC(_debug, OID_AUTO, x9_als_test_lux, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_x9_als_test_lux, "I", "X9 ALS diagnostic lux (-1 invalid), requires x9_als=2");

#include "x9_als_scp.inc"
