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
 X9_GPS_PHYSICAL_D_DIE, X9_GPS_PHYSICAL_POS_FINALIZE,
 X9_GPS_PHYSICAL_POS_SPI, X9_GPS_PHYSICAL_POS_AFE,
 X9_GPS_PHYSICAL_POS_PLL, X9_GPS_PHYSICAL_POS_RADIO,
 X9_GPS_PHYSICAL_POS_LOW_POWER, X9_GPS_PHYSICAL_POS_CLOCK
};
struct x9_gps_afe_smc_result { uint64_t a[4]; };
struct x9_gps_connv2_ops {
 uint32_t version; /* 1..5; descriptor/context remain live until reboot. */
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
 /* Accept complete BODY (no vendor synthetic4B header) only in a proved
  * application session, with measurement age qualified (not just packet
  * arrival/cache presence). Event2 PVT begins at BODY+6. Native worker then
  * translates its qualified fields; this callback must not also submit the
  * same event2 position. No fix or session qualification from a cache flag. */
 int (*mnl)(void *, const uint8_t *, unsigned, uint64_t);
 int (*other_ccif)(void *, unsigned, uint64_t);
 /* Version-2..5 tail. Version 1 remains the full vendor POS supplier.
  * Version 2..5 acquire must instead stop at the cold pre-D-die boundary:
  * real clock schematic/buffer/HWV/AP bus, secure EMI/MPU and EXCLUSIVE
  * whole connv2 SYSRAM + all old BGF DMA quiesce; GPS reset held and
  * function disabled. No D-die/SPI/AFE/ADie/radio initialization yet.
  * The worker then executes native D-die before pos_finalize. Never
  * register v2..5 merely because a register snapshot or SMC returned zero. */
 /* With CLOCK_LAYOUT_VERIFIED the worker additionally takes/retains the
  * measured CKSYS_TOP mainPLL selector before D-die. The supplier must
  * hold its real upstream PLL and exclusive selector ownership throughout;
  * the selector bit alone is not a running-PLL or a CONN power lease.
  * Old handoffs without this optional topology keep supplier-only clocks. */
 uint32_t pos_clock_schematic; /* measured vendor enum 0..3, not Hz/guess */
 /* Every native POS read/write revalidates exclusive SRAM/DMA ownership
  * in addition to validate's power/bus lease. Must be bounded, live and
  * non-reentrant; no constant-success implementation. For v3/v4/v5, also
  * dynamically prove EXCLUSIVE WT_SLP/AFE ownership on every access. */
 int (*pos_validate)(void *, const struct x9_gps_handoff *, uint64_t, uint64_t);
 /* Complete actual SPI/AFE/PLL/radio/low-power/MT6686 POS under the same
  * retained lease. Must leave GPS reset held/function disabled, not
  * reenter HIF, clear native SYSRAM, or register a ready backend. */
 int (*pos_finalize)(void *, const struct x9_gps_handoff *, uint64_t, uint64_t);
 /* Version-3/4/5 tail; v1/v2 descriptors end before this member.
  * v3 moves the actual MT6686 WT_SLP setup and MT6993 subsystem PLL
  * writes into the sole worker: D-die -> native SPI -> this AFE callback
  * -> native PLL -> pos_finalize. Acquire has the same cold contract,
  * with EXCLUSIVE WT_SLP/AFE access as well as SYSRAM/all BGF DMA.
  * Complete and dynamically qualify this board's actual AFE patch
  * (conninfra SMC c2000534/op14, HW02050800/ADie6686). SMC return0 by
  * itself is NOT proof. No duplicate native D-die/SPI/PLL, low-power,
  * radio activation, reset release, backend registration or reentry.
  * v3 pos_finalize completes the remaining radio/voltage/low-power/
  * MT6686 qualification, NOT the already executed native stages. */
 int (*pos_afe)(void *, const struct x9_gps_handoff *, uint64_t, uint64_t);
 /* Version-4/5 tail; v3 ends at 96B. Worker executes the actual radio
  * bookkeeping bit, this voltage/pre-low-power callback, then native
  * low-power/WLA/CMDBT and finally pos_finalize (remaining PMIC/MT6686
  * qualification). A bookkeeping bit is NOT RF or receiver readiness.
  * Acquire must additionally retain TOP force-wake 400601a0[0]=1, and
  * pos_validate must dynamically qualify EXCLUSIVE access to ALL native
  * LP banks, including CFG WLA +420..430 (beyond the compact FDT +138
  * tuple), the entire 8KiB SYSRAM and all old BGF DMA quiesce. The
  * CMDBT restore engine must be quiescent and CFG_PWRCTRL0[0]=0 at
  * acquire and until the worker arms hardware-control AFTER code readback.
  * Keep the RC mode stable and TOP awake across hardware-control and
  * finalization; no restoring power, resuming CMDBT or releasing wake
  * behind the worker. No constant-success callback or ACK fabrication.
  * pos_radio completes actual board voltage/pre-LP qualification after
  * the native bit update; it must not duplicate native LP/CMDBT writes,
  * activate GPS, release reset, register ready or reenter the worker. */
 int (*pos_radio)(void *, const struct x9_gps_handoff *, uint64_t, uint64_t);
 /* EXACT original MT6993 CMDBT 4096B little-endian image, not GPS MCU
  * ROM/MNL firmware. Kernel-only readable input: registration snapshots
  * and verifies identity BEFORE arming the worker or acquiring hardware.
  * No default image is embedded. Wrong/missing image cannot arm v4/v5. */
 const uint8_t *pos_cmdbt;
 uint32_t pos_cmdbt_bytes;
 /* Version-5 ONLY; v4 ends at120B. Native op14 SMC is executed once by
  * the worker AFTER SPI, BEFORE this verifier and PLL. pos_afe is unused
  * for v5 and need not be provided; v1..v4 keep their original behavior.
  * acquire/pos_validate must additionally qualify this actual running ATF
  * ABI and EXCLUSIVE secure patch access (not merely FDT/Android READY).
  * Verify these raw return semantics AND actual completed AFE patch using
  * measured board evidence. VOID vendor call defines no universal return0
  * convention. No constant success, duplicate SMC, inferred ACK, reset
  * release, reentry or READY. A raw zero is NOT sufficient; ambiguity must
  * fail and retain the lease in quarantine. Result lifetime is this call. */
 int (*pos_afe_verify)(void *, const struct x9_gps_handoff *, uint64_t,
     uint64_t, const struct x9_gps_afe_smc_result *);
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
