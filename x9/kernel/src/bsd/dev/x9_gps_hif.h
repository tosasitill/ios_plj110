/* Physical soc721a HIF: descriptor mailboxes, NOT producer/consumer rings.
 * One deferred worker owns this object, all APIs, and all mapped resources.
 * No backend registration or fabricated readiness. Board power/EMI MPU/
 * remap/MCU reset+boot/driver ownership must be supplied by a real provider. */
#ifndef X9_GPS_HIF_H
#define X9_GPS_HIF_H
#include "x9_gps_mcudl.h"
#define X9_GPS_HIF_CHANNELS 3u
#define X9_GPS_HIF_MGMT 0u
#define X9_GPS_HIF_NORMAL 1u
#define X9_GPS_HIF_URGENT 2u
struct x9_gps_hif_ops {
 /* Callbacks may stage a bounded outbox for the worker, but must not reenter
  * HIF APIs or release its hardware lease while a callback is in flight. */
 /* 0 means this generation holds all resources, MCU firmware ownership is
  * cleared, and CCIF is readable. reset=1 additionally requires held reset.
  * Keep the lease until confirmed board quiesce; not just a status snapshot.
  * Layout qualification, an idle PC or Android READY does NOT satisfy this. */
 int (*check_owned)(void *, uint64_t generation, int reset);
 int (*control)(void *, unsigned channel, uint8_t type, const uint8_t *, unsigned, uint64_t);
 int (*mnl)(void *, const uint8_t *, unsigned, uint64_t);
 int (*other_ccif)(void *, unsigned channel, uint64_t);
 void *context;
};
struct x9_gps_hif_route {struct x9_gps_hif *hif; unsigned channel;};
struct x9_gps_hif {
 struct x9_gps_handoff layout;
 struct x9_gps_hif_ops ops;
 /* Provider maps exactly the two qualified 16KiB HIF areas and CCIF with
  * device/noncacheable attributes, no conflicting cached aliases. */
 volatile uint8_t *ap, *mcu, *ccif;
 uint64_t generation, last_time, tx_time[3], command_time, start_time;
 uint32_t id[6], length[6], pending, prepared, started, fault;
 uint32_t command, command_ack, commands_used, dispatching;
 uint64_t tx_bytes, rx_bytes, completions;
 struct x9_gps_hif_route route[2];
 struct x9_gps_mcudl_stream stream[2];
 uint8_t scratch[4096]; /* off-stack, stable until callback returns */
};
#ifdef __cplusplus
extern "C" {
#endif
/* Prepare ONLY under provider-confirmed MCU reset. Does not reset/boot MCU,
 * map RAM or clear target-owned completion flags. No retry/reuse while the
 * old session could still access these mappings. Object must be initially
 * zeroed and off-stack; only a verified board quiesce allows disposal.
 * generation is the current GPS output generation. This conservative API
 * does NOT live-rebind posted DMA to a new tracking generation: a transition
 * must quiesce/reset the old session before preparing another. */
int x9_gps_hif_prepare(struct x9_gps_hif *, const struct x9_gps_hif_ops *,
    volatile void *ap, volatile void *mcu, volatile void *ccif, uint64_t generation);
/* After real MCU boot: validate fresh target state; poll subsequently arms
 * MGMT/NORMAL/URGENT RX then sends raw one-byte cmd1. Return 0 is local
 * admission only, NOT command submission, a management ACK or GPS READY. */
int x9_gps_hif_start(struct x9_gps_hif *, uint64_t host_time);
/* Bounded CCIF4/descriptor completion processing + RX rearm. No IRQ/spinlock
 * callbacks: integrate on a dedicated worker, never the QMI bridge stack. */
int x9_gps_hif_poll(struct x9_gps_hif *, uint64_t host_time);
/* Raw HIF bytes. NORMAL/URGENT caller still owns MCUDL slot/flow control. */
int x9_gps_hif_send(struct x9_gps_hif *, unsigned channel, const void *, unsigned, uint64_t);
/* Matching fresh MCU reply, not merely TX done. Only cmd1/2 are supported,
 * once each per reset session; wire has no application transaction ID. */
int x9_gps_hif_management(struct x9_gps_hif *, unsigned command, uint64_t);
int x9_gps_hif_management_result(struct x9_gps_hif *, unsigned command);
#ifdef __cplusplus
}
#endif
#endif
