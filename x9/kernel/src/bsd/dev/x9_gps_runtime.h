/* MT6993 connv2 -> v062 MCU -> HIF. Kernel-only, not a user ioctl ABI. */
#ifndef X9_GPS_RUNTIME_H
#define X9_GPS_RUNTIME_H
#include "x9_gps_hif.h"
enum x9_gps_physical_stage {
 X9_GPS_PHYSICAL_OFF, X9_GPS_PHYSICAL_WAIT_CONNV2,
 X9_GPS_PHYSICAL_ACQUIRE, X9_GPS_PHYSICAL_WAKE,
 X9_GPS_PHYSICAL_RESET, X9_GPS_PHYSICAL_REMAP,
 X9_GPS_PHYSICAL_IMAGE, X9_GPS_PHYSICAL_PLL,
 X9_GPS_PHYSICAL_IDLE, X9_GPS_PHYSICAL_HIF,
 X9_GPS_PHYSICAL_TRANSPORT, X9_GPS_PHYSICAL_QUARANTINE,
 X9_GPS_PHYSICAL_D_DIE, X9_GPS_PHYSICAL_POS_FINALIZE
};
struct x9_gps_connv2_ops {
 uint32_t version; /* 1 or 2; descriptor/context remain live until reboot. */
 void *context;
 /* Exclusive long-lived connv2 lease, NOT Wi-Fi connv3/MT6653 ownership.
  * Must power/qualify MT6661 VGPS18, CONNSYS domain, AP bus clocks/protect,
  * MT6686 setup and ATF EMI configuration/MPU for this exact LK layout.
  * Must also quiesce all old BGF DMA/HIF users BEFORE permitting reset or
  * host RAM clearing. Return a nonzero lease on success. A partial failure
  * must return its retained nonzero lease too; never silently roll it back.
  * This worker deliberately cannot release/quiesce uncertain DMA yet. */
 int (*acquire)(void *, const struct x9_gps_handoff *, uint64_t generation,
     uint64_t *lease);
 /* Prove the above resources are still held and the conninfra host/CFG
  * registers are safe to access. Check every MMIO operation. No constant
  * success implementation, and no ownership inferred from Android READY. */
 int (*validate)(void *, const struct x9_gps_handoff *, uint64_t generation,
     uint64_t lease);
 /* Called with readback-confirmed MCU held reset and qualified remaps.
  * Provision/qualify an actual ROM boot image for this board. Android's
  * zero-length ROM-only load is NOT proof of a usable XNU boot image.
  * Must not release reset, reenter HIF, or relinquish the resource lease. */
 int (*provision)(void *, const struct x9_gps_handoff *, uint64_t generation,
     uint64_t lease);
 /* Complete MCUSYS NV/sync/FSM, slot/flow and MNL handlers. Stage bounded
  * outboxes only; callbacks must not reenter HIF or release/reset resources.
  * Unknown data must return ENOTSUP, not be silently ACKed/discarded. */
 int (*control)(void *, unsigned, uint8_t, const uint8_t *, unsigned, uint64_t);
 int (*mnl)(void *, const uint8_t *, unsigned, uint64_t);
 int (*other_ccif)(void *, unsigned, uint64_t);
 /* Version-2 tail only. Version 1 remains the full vendor POS supplier.
  * Version 2 acquire must instead stop at the cold pre-D-die boundary:
  * real clock schematic/buffer/HWV/AP bus, secure EMI/MPU and EXCLUSIVE
  * whole connv2 SYSRAM + all old BGF DMA quiesce; GPS reset held and
  * function disabled. No D-die/SPI/AFE/ADie/radio initialization yet.
  * The worker then executes native D-die before pos_finalize. Never
  * register v2 merely because a register snapshot or SMC returned zero. */
 uint32_t pos_clock_schematic; /* measured vendor enum 0..3, not Hz/guess */
 /* Every native POS read/write revalidates exclusive SRAM/DMA ownership
  * in addition to validate's power/bus lease. Must be bounded, live and
  * non-reentrant; no constant-success implementation. */
 int (*pos_validate)(void *, const struct x9_gps_handoff *, uint64_t, uint64_t);
 /* Complete actual SPI/AFE/PLL/radio/low-power/MT6686 POS under the same
  * retained lease. Must leave GPS reset held/function disabled, not
  * reenter HIF, clear native SYSRAM, or register a ready backend. */
 int (*pos_finalize)(void *, const struct x9_gps_handoff *, uint64_t, uint64_t);
};
#ifdef __cplusplus
extern "C" {
#endif
/* Registration only arms the dedicated worker. Does NOT register a GPS
 * backend, grant a PDS lease, claim a fix or set receiver ready. No default
 * supplier is installed: without real connv2 this remains WAIT_CONNV2. */
int x9_gps_register_connv2(const struct x9_gps_connv2_ops *);
/* Callback-only bounded outbox. Called on the sole physical worker, never
 * from QRTR/set_state/IRQ or a separate supplier thread. NORMAL/URGENT raw
 * MCUDL bytes must already carry a real slot/flow reservation; this is not
 * a slot allocator. Copies at most4096B, two entries/channel. Worker sends
 * only after fresh management1 ACK, retains busy entries and their lease.
 * Enqueue success is NOT TX completion or an application ACK. */
int x9_gps_runtime_queue(uint64_t generation, unsigned channel,
    const void *, unsigned length);
#ifdef __cplusplus
}
#endif
#endif
