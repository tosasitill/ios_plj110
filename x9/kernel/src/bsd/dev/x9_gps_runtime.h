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
 X9_GPS_PHYSICAL_POS_LOW_POWER, X9_GPS_PHYSICAL_POS_CLOCK,
 X9_GPS_PHYSICAL_BUS, X9_GPS_PHYSICAL_EMI_CPU,
 X9_GPS_PHYSICAL_SECURE_REMAP
};
/* Profile values are not permission to call secure firmware. The saved
 * PLJ110 ATF dispatches 0415 only BEFORE KERNEL_BOOT(c2000115). Saved LK
 * calls KERNEL_BOOT on its kernel jump, which sets ATF132080=1; BOTH kernel
 * whitelists then reject 0415. Keep value1 for descriptor compatibility,
 * but registration rejects it (and every other v6 profile). There is no
 * supported kernel SR/AID READ profile yet. Do not change caller bits,
 * resend KERNEL_BOOT, or infer callability from a supplier assertion/DT. */
enum x9_gps_secure_profile {
 X9_GPS_SECURE_UNQUALIFIED=0,
 X9_GPS_SECURE_0415_4K_V1=1 /* retired BL-only ABI; never accepted */
};
struct x9_gps_afe_smc_result { uint64_t a[4]; };
struct x9_gps_connv2_ops {
 uint32_t version; /* 1..6; descriptor/context remain live until reboot. */
 void *context;
 /* generation in ALL physical callbacks is hardware_generation, not the
  * application/output generation passed to backend.set_state. Successful
  * application transitions keep this lease and its posted DMA intact.
  * Exclusive long-lived connv2 lease, NOT Wi-Fi connv3/MT6653 ownership.
  * Must power/qualify MT6661 VGPS18, CONNSYS domain, AP bus clocks/protect,
  * MT6686 setup and ATF EMI configuration/MPU for this exact LK layout.
  * Must also quiesce all old BGF DMA/HIF users BEFORE permitting reset or
  * host RAM clearing. Return a nonzero lease on success. A partial failure
  * must return its retained nonzero lease too; never silently roll it back.
  * This worker deliberately cannot release/quiesce uncertain DMA yet. */
 int (*acquire)(void *, const struct x9_gps_handoff *, uint64_t generation,
     uint64_t *lease);
 /* Prove the above resources are still held and the conninfra host/CFG
  * registers are safe to access. AON-safe power/upstream-clock checks MUST
  * precede any CONN access, including native bus detection. Native v4/v5
  * additionally qualifies actual TOP wake/off clock/IP/IRQ before BGF/POS
  * mapping; this does not replace secure EMI or exclusive DMA ownership.
  * Check every MMIO operation. No constant
  * success implementation, and no ownership inferred from Android READY. */
 int (*validate)(void *, const struct x9_gps_handoff *, uint64_t generation,
     uint64_t lease);
 /* Called with readback-confirmed MCU held reset and qualified remaps.
  * Provision/qualify an actual ROM boot image for this board. Android's
  * zero-length ROM-only load is NOT proof of a usable XNU boot image.
  * Must not release reset, reenter HIF, or relinquish the resource lease.
  * May explicitly call x9_gps_nv_prepare() (x9_gps_nv.h) with a REAL cold/
  * live NV authority verifier. Old registration/acquire contracts alone
  * do not opt into native NV clearing. No native NV/persistent producer
  * or default authority verifier is installed. */
 int (*provision)(void *, const struct x9_gps_handoff *, uint64_t generation,
     uint64_t lease);
 /* Complete MCUSYS NV/sync/FSM and MNL handlers. v1..v4 also own slot/flow;
  * v5's native worker consumes NORMAL35/36 and owns NORMAL host-status7e.
  * URGENT35/36 still reach this real handler (no invented urgent ACK).
  * Stage bounded
  * outboxes only; callbacks must not reenter HIF or release/reset resources.
  * Unknown data must return ENOTSUP, not be silently ACKed/discarded. */
 int (*control)(void *, unsigned, uint8_t, const uint8_t *, unsigned, uint64_t);
 /* Accept complete BODY (no vendor synthetic4B header) only in a proved
  * application session, with measurement age qualified (not just packet
  * arrival/cache presence). Event2 PVT begins at BODY+6. Native worker then
  * translates its qualified fields; this callback must not also submit the
  * same event2 position. No fix or session qualification from a cache flag. */
 /* Native control monitor observes generic event0 and STOP_DONE128, but
  * does not match either to an application transaction or grant readiness.
  * MNL terminal event5 revokes hardware/output epochs before this callback;
  * the callback still receives it for teardown, then worker quarantines.
  * It cannot queue or publish traffic using the revoked generation. */
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
  * Acquire must additionally provide safe HOST/CFG_ON/debug access and a
  * quiesced CMDBT engine with CFG_PWRCTRL0[0]=0. The worker retains/sets
  * TOP force-wake 400601a0[0]=1 BEFORE native POS, and
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
 /* Version-5/6 tail; v4 ends at120B. Native op14 SMC is executed once by
  * the worker AFTER SPI, BEFORE this verifier and PLL. pos_afe is unused
  * for v5/6 and need not be provided; v1..v4 keep their original behavior.
  * acquire/pos_validate must additionally qualify this actual running ATF
  * ABI and EXCLUSIVE secure patch access (not merely FDT/Android READY).
  * Verify these raw return semantics AND actual completed AFE patch using
  * measured board evidence. VOID vendor call defines no universal return0
  * convention. No constant success, duplicate SMC, inferred ACK, reset
  * release, reentry or READY. A raw zero is NOT sufficient; ambiguity must
  * fail and retain the lease in quarantine. Result lifetime is this call. */
 int (*pos_afe_verify)(void *, const struct x9_gps_handoff *, uint64_t,
     uint64_t, const struct x9_gps_afe_smc_result *);
 /* Version-6 tail retained at136B, not an operational READ route. Saved
  * ATF's 050b/op2 returns -2 and 0415/op2 is BL-only after tracing the
  * actual dispatch/boot chain, not merely its service name. All v5/v6
  * registrations return ENOTSUP BEFORE acquire, CMDBT or worker arming.
  * A new production profile needs an actually kernel-reachable service,
  * selected-slot provenance and bounded read/selector semantics; merely
  * declaring profile1 cannot supply these. No probing or fallback occurs.
  * Secure remap/AP+BGF authority and the real op14 source verifier remain
  * necessary, but cannot make a BL-only READ callable from this kernel. */
 uint32_t secure_profile, secure_reserved;
};
/* v1..v4 prefixes/behavior, the128B v5 size and136B v6 size are unchanged.
 * v5/v6 are disabled before hardware arming, not silently downgraded to
 * v4. SMPU_LAYOUT_VERIFIED only describes topology, never live policy or
 * SMC callability. Retained policy/remap code cannot authorize BGF DMA or
 * receiver readiness; its READ entry is fail-closed with no SMC emitted. */
#ifdef __cplusplus
extern "C" {
#endif
/* Registration only arms the dedicated worker. Does NOT register a GPS
 * backend, grant a PDS lease, claim a fix or set receiver ready. No default
 * supplier is installed: without real connv2 this remains WAIT_CONNV2. */
int x9_gps_register_connv2(const struct x9_gps_connv2_ops *);
/* Callback-only bounded outbox. Called on the sole physical worker, never
 * from QRTR/set_state/IRQ or a separate supplier thread. v1..v4 retain raw
 * at-most4096B/two entries/channel and require supplier slot/flow admission.
 * Native v6 instead validates/reserves ONE complete MCUDL xlink envelope,
 * payload<=1800, seq00 NORMAL/20 URGENT (agent a3 only). Its real local
 * slots have64 entries/10KiB per channel, byte-credit/window accounting,
 * near-full admission and critical reserves. No provider-made7e or76.
 * Worker sends only after fresh management1 ACK, retains busy entries and
 * their lease. Native stats ACKs commit only at real HIF TX completion;
 * a type36 request never resets parser/credit counters or live DMA.
 * generation is hardware_generation; application reservations/ACK epochs
 * remain the protocol handler's responsibility, not a transport rebind.
 * Enqueue success is NOT TX completion or an application ACK. */
int x9_gps_runtime_queue(uint64_t generation, unsigned channel,
    const void *, unsigned length);
/* Callback-only AP MNL command path: validated payload -> one native inner
 * packet -> MCUDL envelope -> existing bounded worker outbox. The serializer
 * uses zero-based channels; this wrapper converts them to HIF NORMAL1 /
 * URGENT2. v1..v4 callers MUST supply a slot_seq reserved under their REAL
 * slot/flow implementation; this wrapper does not invent a packet ID or
 * reserve/refund supplier credit. Native v5/v6 are still unregistrable.
 * generation is the physical epoch. Caller must separately prove the
 * application session, config/NV authority and command order; there is no
 * request matcher here. INIT on v2..v4 snapshots the caller's136B payload
 * and applies original post-NV precedence under the current booted lease:
 * measured POS schematic -> +0x13 source, and52M Hz ONLY on52M CoTMS.
 * The26M frequency stays unchanged and must be nonzero; it is NOT
 * validated as calibration here. +0x14/+0x24/+0x3c..4b and all other
 * fields stay unchanged. Schematic3 is ENOTSUP, not mislabeled26M.
 * v1 has no such POS contract and retains its caller-owned configuration.
 * INIT NULL/0 on v2..v4 builds the original native136B cold factory
 * baseline, including the AP's explicit no-import26M fallback, then
 * applies that same post-NV source/52M override. MD words stay at their
 * explicit original zero defaults, NOT qualified calibration or NV
 * authority. Existing caller configurations never use this fallback.
 * Factory fields are not proof of measured board tuning or aiding.
 * v1 NULL/0 is rejected; v5/v6 remain disabled. No guessed drift.
 * The caller's payload is never modified, including on enqueue failure.
 * DRIVER3 may instead use NULL/0 on v2..v4: the worker produces the
 * 216B baseline from the original AP zero-fill and MCU explicit default
 * stores/version/footer (not inferred semantics of vendor ISA helpers).
 * It omits the1256B host tail, disables Android logfile/HW-suspend/resume
 * and optional aiding, and requires a current booted/POS/HIF owned lease.
 * This is the known baseline driver policy, NOT complete board tuning,
 * calibration, NV authority or permission to bypass INIT/session ordering.
 * v1 has no native-default contract and rejects NULL/0 for DRIVER3.
 * HOST_TIME7 and BOOT_TIME15 instead require NULL/0 input: this worker
 * generates the real XNU calendar seconds / continuous boot nanoseconds
 * and raw CNTVCT pair immediately before serialization. No caller-made
 * timestamp, Android ioctl, fabricated zero pair or fixed tick frequency.
 * The15 pair is sampled with local IRQs disabled and then restored;
 * continuous time includes suspend. These are HOST assistance inputs,
 * not receiver measurement time, GNSS UTC/leap authority or PDS validity.
 * libmnl53e4d8/53e4dc sends7/15 after configs2/3; ordering/slot reservations
 * remain the real protocol handler's job, not inferred from local enqueue.
 * Success is only a copied local outbox reservation,
 * not TX completion, OPEN/STOP_DONE, receiver READY or a PDS transition.
 * Inputs may be released after return; on error no outbox entry is added. */
int x9_gps_runtime_mnl_command(uint64_t generation, uint8_t slot_seq,
    uint32_t command, const void *payload, unsigned payload_bytes);
#ifdef __cplusplus
}
#endif
#endif
