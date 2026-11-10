/* X9 barometer: MTK sensor/2.0 to native iOS atmospheric-pressure HID. */
#ifndef X9_BARO_H
#define X9_BARO_H

#include <stdint.h>
#include "../../../bsd/dev/x9_scp_handoff.h"

#define X9_BARO_MTK_INFO_BYTES 40u
#define X9_BARO_MTK_DATA_BYTES 36u
#define X9_BARO_MIN_PA 30000u
#define X9_BARO_MAX_PA 120000u
#define X9_BARO_MAX_AGE_NS 5000000000ULL

struct x9_baro_sample {
	uint64_t sequence;
	uint64_t host_time;       /* mach_absolute_time at receipt, NOT SCP ns */
	uint64_t scp_time_ns;
	uint32_t pressure_kpa_q16;             /* 16.16 kPa, as consumed by CLPressure */
	uint32_t diagnostic;
};

#ifdef __cplusplus
extern "C" {
#endif
int x9_baro_init(int diagnostic);
void x9_baro_shutdown(void);
int x9_baro_copy_sample(struct x9_baro_sample *sample);
struct x9_baro_transport_status {
 uint32_t list_pending, enable_pending, control_enabled;
 int32_t last_errno; /* quarantine/error, NOT physical power-off evidence */
};
/* Bounded housekeeping, no hardware operation. Expire missing replies even
 * if no receive callback fires. Quarantine rejects new requests/old data;
 * no automatic retry, restored gain, fabricated ACK or claim of power-off. */
void x9_baro_copy_transport_status(struct x9_baro_transport_status *);
/* Layout qualification is not a power/firmware/transport readiness signal. */
int x9_baro_copy_scp_handoff(struct x9_scp_handoff *layout);
uint32_t x9_baro_get_report_interval_us(void);
/* Physical rate changes are unsupported without the actual IPI sender.
 * Only a matching real ENABLE ACK commits its batch rate. */
int x9_baro_set_report_interval_us(uint32_t interval_us);

/* sensor/2.0 worker context only (never hard IRQ). PRESSURE=6, DATA=0.
 * gain is raw units/hPa from the real sensor list; do not assume gain=100.
 * On SCP reset invalidate BEFORE reconnect/list/configure. After an uncertain
 * send/timeout/failure, invalidate ONLY once the actual transport has proved
 * remote reset/quiesce and discarded old mailbox/FIFO traffic; not just to
 * clear Busy. Invalidate is kernel-only, never a user-controlled retry.
 * SCP transport/firmware and enable/batch remain to be supplied. */
int x9_baro_configure_mtk(const void *info, unsigned int length);
int x9_baro_receive_mtk(const void *record, unsigned int length);
void x9_baro_invalidate_mtk(void);
/* sensorhub v2 host wire path (thread context). These are not MMIO/IPI sends:
 * transport must supply a real SHARE_BUFFER reply plus its IPI32 notification,
 * send the constructed ENABLE on IPI29, then supply its actual IPI30 ACK.
 * v2 DATA is rejected until that matching ACK succeeds. Reset invalidates all
 * state. A single transport worker must serialize list/enable/reset, discard
 * old mailbox/FIFO traffic on reset and invalidate on send/timeout failure;
 * the 8-bit wire sequence alone cannot identify a firmware generation.
 * Duplicate/unrelated ACKs leave state unchanged; ENABLE ACK expires in 1s.
 * No default gain, pressure or synthetic success is supplied. */
/* First construct the LIST snapshot and establish its pending lease. This
 * does not write shared RAM or send IPI31. shared_bytes is the real W region
 * size (MT6993:8192), notification capacity>=28. Distinct output buffers;
 * transport invalidates on send/reset/timeout failure before starting again.
 * configure_list rejects replies without a matching pending request and a
 * 100ms deadline; bad/unrelated packets never revoke an established stream.
 * Existing HID setProperties maps in-flight LIST/ENABLE to kIOReturnBusy. */
int x9_baro_scp_make_list(void *shared_tx, unsigned int shared_bytes,
    void *notify, unsigned int notify_capacity, uint8_t sequence);
int x9_baro_scp_configure_list(const void *shared_rx, unsigned int shared_bytes,
    const void *notify, unsigned int notify_bytes, uint8_t request_sequence);
int x9_baro_scp_make_enable(void *request, unsigned int capacity,
    uint8_t sequence, uint64_t interval_ns);
int x9_baro_scp_accept_enable(const void *ack, unsigned int length);
int x9_baro_scp_receive_data(const void *record, unsigned int length);
#ifdef __cplusplus
}
#endif
#endif
