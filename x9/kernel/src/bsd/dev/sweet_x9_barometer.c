/* X9 BARO phase 1: bounded MTK sensor/2.0 ingress and diagnostic injection.
 * Wire reference: OPPO kernel_device_modules-6.12 sensor/2.0/{core,sensorhub}.
 * No SCP/I2C/MMIO/firmware operations here; no invented physical readings. */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <pexpert/device_tree.h>
#include "../../iokit/Kernel/arm/X9Barometer.h"

static LCK_GRP_DECLARE(x9_baro_group, "x9_baro");
static LCK_MTX_DECLARE(x9_baro_lock, &x9_baro_group);
static struct x9_baro_sample latest;
static uint32_t mtk_gain;
static uint64_t last_scp_time;
static uint64_t scp_request_time;
static uint64_t scp_request_interval_ns;
static uint32_t report_interval_us = 200000;
static struct x9_scp_handoff scp_layout;
static int scp_layout_valid;
static int active, test_mode, valid;
static int scp_pending, scp_enabled;
static uint8_t scp_sequence;
static int scp_list_pending;
static uint8_t scp_list_sequence;
static uint64_t scp_list_request_time;
static int scp_error; /* uncertain remote transaction; reset/drain required */
#define BARO_LIST_TIMEOUT_NS 100000000ULL
#define BARO_ENABLE_TIMEOUT_NS 1000000000ULL

/* Caller holds x9_baro_lock. A missing reply may mean hardware accepted the
 * request. Stop ALL ingress, retain the fault, and never auto-retry/claim off.
 * Only the real transport's explicit reset/drain invalidation clears it. */
static void baro_quarantine_locked(int error)
{
 scp_error = error ? error : EIO;
 valid = scp_enabled = scp_pending = scp_list_pending = 0;
 mtk_gain = 0; last_scp_time = 0;
 scp_request_time = scp_request_interval_ns = scp_list_request_time = 0;
}
static int baro_expire_locked(void)
{
 if (!active || test_mode) return 0;
 if (scp_error) return scp_error;
 uint64_t now = mach_absolute_time(), age = 0;
 if (scp_list_pending) {
  absolutetime_to_nanoseconds(now-scp_list_request_time, &age);
  if (age > BARO_LIST_TIMEOUT_NS) baro_quarantine_locked(ETIMEDOUT);
 }
 if (scp_pending) {
  absolutetime_to_nanoseconds(now-scp_request_time, &age);
  if (age > BARO_ENABLE_TIMEOUT_NS) baro_quarantine_locked(ETIMEDOUT);
 }
 return scp_error;
}

void x9_baro_copy_transport_status(struct x9_baro_transport_status *out)
{
 if (!out) return;
 lck_mtx_lock(&x9_baro_lock);
 (void)baro_expire_locked();
 out->list_pending = (uint32_t)scp_list_pending;
 out->enable_pending = (uint32_t)scp_pending;
 out->control_enabled = (uint32_t)scp_enabled;
 out->last_errno = active ? scp_error : ENXIO;
 lck_mtx_unlock(&x9_baro_lock);
}

/* Read only the shim's qualified copy, never stale SRAM or an assumed EMI
 * pointer. FDT facts are necessary but insufficient for hardware ownership. */
static void
copy_scp_layout(void)
{
 DTEntry chosen; const void *p; unsigned size;
 struct x9_scp_handoff h;
 scp_layout_valid = 0; bzero(&scp_layout, sizeof(scp_layout));
 if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
     SecureDTGetProperty(chosen, "x9-scp-handoff", &p, &size) != kSuccess ||
     size != sizeof(h)) return;
 memcpy(&h, p, sizeof(h));
 if (h.version != 1 || h.flags != X9_SCP_LAYOUT_VERIFIED ||
     h.reserve_base != 0xe8000000 || h.reserve_size != 0x18c0000 ||
     !h.dump_size || h.memory[0].size != h.dump_size ||
     h.allocated_size > h.reserve_size ||
     h.mbox_base != 0x1dfb0000 || h.mbox_set != 0x1dfb0100 ||
     h.mbox_clear != 0x1dfb010c || h.mbox_init != 0x1df2a024 ||
     h.sram_base != 0x1d000000 || h.sram_size != 0x400000 || h.region_info_op != 10) return;
 uint32_t end = 0;
 for (unsigned i = 0; i < X9_SCP_MEM_COUNT; i++) {
  uint32_t start = h.memory[i].offset, bytes = h.memory[i].size;
  if (start < end || start > h.allocated_size || bytes > h.allocated_size-start) return;
  end = start+bytes;
 }
 if (end != h.allocated_size || h.memory[2].size != 0x100000 ||
     h.memory[8].size != 0x10000 || h.memory[9].size != 0x2000 || h.memory[10].size != 0x2000) return;
 const unsigned channels[] = {29,30,31,32}, lengths[] = {64,8,28,28};
 for (unsigned i = 0; i < 4; i++) {
  const struct x9_scp_pin *pin = &h.sensor_pin[i];
  if (pin->channel != channels[i] || pin->message_bytes != lengths[i] ||
      pin->pin_index >= 32 || pin->offset_bytes != pin->pin_index*8 ||
      pin->offset_bytes > 256-pin->message_bytes) return;
  for (unsigned j = 0; j < i; j++) {
   const struct x9_scp_pin *other = &h.sensor_pin[j];
   unsigned endj = other->offset_bytes+((other->message_bytes+7)&~7u);
   unsigned endi = pin->offset_bytes+((pin->message_bytes+7)&~7u);
   if (pin->offset_bytes < endj && other->offset_bytes < endi) return;
  }
 }
 for (unsigned i = 0; i < 7; i++) if (h.reserved[i]) return;
 scp_layout = h; scp_layout_valid = 1;
}

int
x9_baro_copy_scp_handoff(struct x9_scp_handoff *out)
{
 if (!out) return EINVAL;
 lck_mtx_lock(&x9_baro_lock);
 int error = !active ? ENXIO : !scp_layout_valid ? EAGAIN : 0;
 if (!error) *out = scp_layout;
 lck_mtx_unlock(&x9_baro_lock); return error;
}

uint32_t
x9_baro_get_report_interval_us(void)
{
 lck_mtx_lock(&x9_baro_lock); (void)baro_expire_locked(); uint32_t us = report_interval_us;
 lck_mtx_unlock(&x9_baro_lock); return us;
}

int
x9_baro_set_report_interval_us(uint32_t us)
{
 if (us < 20000 || us > 1000000) return EINVAL;
 lck_mtx_lock(&x9_baro_lock);
 /* Until a real IPI sender owns cadence changes, do not report a host-poll
  * change as a hardware batch update. Diagnostics are explicitly host-only. */
 int expired = baro_expire_locked();
 int error = !active ? ENXIO : expired ? expired : scp_pending || scp_list_pending ? EBUSY :
     !test_mode && us != report_interval_us ? ENOTSUP : 0;
 if (!error) report_interval_us = us;
 lck_mtx_unlock(&x9_baro_lock); return error;
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
	scp_pending = scp_enabled = 0;
	scp_request_time = 0;
	scp_request_interval_ns = 0;
	scp_list_pending = 0; scp_list_request_time = 0;
	report_interval_us = 200000;
	scp_error = 0;
	copy_scp_layout();
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
	scp_pending = scp_enabled = 0;
	scp_request_time = 0;
	scp_request_interval_ns = 0;
	scp_list_pending = 0; scp_list_request_time = 0;
	scp_layout_valid = 0; scp_error = 0;
	lck_mtx_unlock(&x9_baro_lock);
}

int
x9_baro_copy_sample(struct x9_baro_sample *sample)
{
	int ok;
	uint64_t age_ns;
	if (!sample) return EINVAL;
	lck_mtx_lock(&x9_baro_lock);
	int expired = baro_expire_locked();
	ok = active && valid;
	if (ok) *sample = latest;
	lck_mtx_unlock(&x9_baro_lock);
	if (!ok) return expired ? expired : ENOENT;
	absolutetime_to_nanoseconds(mach_absolute_time() - sample->host_time, &age_ns);
	return age_ns <= X9_BARO_MAX_AGE_NS ? 0 : ETIMEDOUT;
}

/* Caller holds x9_baro_lock, and has validated the whole transaction. */
static void
configure_mtk_locked(uint32_t gain)
{
	mtk_gain = gain;
	last_scp_time = 0;
	valid = 0;
	scp_pending = scp_enabled = 0;
	scp_request_time = scp_request_interval_ns = 0;
	scp_list_pending = 0; scp_list_request_time = 0;
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
	int expired = baro_expire_locked();
	error = !active ? ENXIO : expired ? expired : test_mode || scp_list_pending || scp_pending ? EBUSY : 0;
	if (!error) configure_mtk_locked(gain);
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
	int expired = baro_expire_locked();
	error = !active ? ENXIO : expired ? expired : (test_mode || scp_pending || scp_list_pending ? EBUSY : (!mtk_gain ? EAGAIN : 0));
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
		scp_error = 0;
		valid = 0;
		mtk_gain = 0;
		last_scp_time = 0;
		scp_pending = scp_enabled = 0;
		scp_request_time = 0;
		scp_request_interval_ns = 0;
		scp_list_pending = 0; scp_list_request_time = 0;
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

#include "x9_baro_scp.inc"
