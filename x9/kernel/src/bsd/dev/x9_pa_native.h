/* Native dual-PA safety mute; not a container/profile/unmute grant. */
#ifndef X9_PA_NATIVE_H
#define X9_PA_NATIVE_H
#include <stdint.h>
#define X9_PA_NATIVE_STATUS_VERSION 1u
#define X9_PA_NATIVE_SYSCTRL 0u
#define X9_PA_NATIVE_AMPE_MASK 8u
#define X9_PA_NATIVE_I2CR_MASK 2u
typedef struct {
    uint32_t version,generation,error,muted,revision_verified,mute_uncertain;
    uint16_t revision[2],sysctrl[2];
    uint32_t side_error[2];
    uint64_t mute_attempts[2],checks;
} X9AudioPANativeStatus; /* 64B cached; no I2C/reset/Ready authorization */
/* Full stepping ids present in OPPO tfa98xx_v6.14.2 tfa_init.c. This lookup
 * alone is not identity or a lease. Only common AMPE-off is admitted, not
 * the vendor's key/boost/OTP/calibration/init sequences. */
#ifdef __cplusplus
static constexpr int
#else
static inline int
#endif
x9_pa_native_revision_supported(uint16_t revision)
{
    switch(revision) {
    case 0x1a72:case 0x2a72:case 0x1b72:case 0x2b72:case 0x3b72:
    case 0x0a73:case 0x0b73:case 0x1a73:
    case 0x0a74:case 0x0b74:case 0x0c74:return 1;
    default:return 0;
    }
}
#if defined(KERNEL)
#include "x9_tfa_re25.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Caller already owns a real native I2C6 child. Same controller mutex covers
 * the WHOLE dual-side transaction, so controller release/other transfers
 * cannot interleave ID/SYSCTRL/write/readback. Initial reg03 reads happen
 * once per actual reset epoch; unsupported/failed identity is never retried
 * into write authority. Unknown mute effects are retained and never replayed.
 * Known side may still be muted after a definite NACK on the other side;
 * pending/uncertain controller effects block every following transfer.
 * Fresh physical cleanup is allowed after historical transport faults, but
 * these APIs never erase the first fault or authorize playback/profile.
 * A new binding requires actual new READY generation AND PA reset epoch,
 * proved by native I2C6 parents, not a newly allocated status/context object.
 * No clock/pad/rail/reset/key/status-IRQ access, no AMPE-on, no verified=7.
 * Lock: AFE/pads -> shared I2C6 -> PA reset/power -> PMIF -> clock/board/AP. */
int x9_pa_native_mute(uint64_t ticket,uint64_t i2c6_lease);
int x9_pa_native_muted_validate(uint64_t ticket,uint64_t i2c6_lease);
int x9_pa_native_status(X9AudioPANativeStatus *);
/* Kernel-only fresh factory shadow observation, NOT a cached ioctl/Ready
 * or grant to write SetRe25/MTP. Full native identity and AMPE-off for BOTH
 * sides must already exist; actual same I2C6/PA/READY epoch remains pinned.
 * Reads ONLY f0(MTPOTC/MTPEX) and f5(R25C), with whole-word paired double
 * snapshots and fresh full ID/SYSCTRL/parent before and after. Requires
 * OTC+EX and two positive non-erased independent values. Never reads status
 * or IRQ banks, writes OTP/key/trigger, polls MTP or substitutes/duplicates
 * values. Does not prove MTP engine no-outstanding, board-specific bounds,
 * DSP channel mapping, calibration-done or firmware profile protection;
 * those remain independent actual supplier prerequisites before parameter
 * execution. One attempt per real reset/READY epoch, fault blocks reuse.
 * Output must be kernel-resident, writable/pinned for the whole call and
 * disjoint from driver state/MMIO; it changes only after complete success. */
struct x9_pa_re25_snapshot {
    uint32_t version,generation;
    uint64_t ticket,i2c6_lease,reset_epoch;
    uint16_t revision[2],sysctrl[2],mtp_control[2],r25c_mohm[2];
}; /* 48B native observation; no serialized trust/user ABI. */
int x9_pa_native_re25_read(uint64_t ticket,uint64_t i2c6_lease,
    struct x9_pa_re25_snapshot *);
enum { X9PARe25Admission=1u,X9PARe25Factory,X9PARe25Set,
       X9PARe25SetConsumed,X9PARe25Read,X9PARe25Complete };
#define X9_PA_RE25_PROGRAM_POLICY_VERSION 1u
struct x9_pa_re25_program_policy {
    uint32_t version,generation,wire_packing,read_form;
    uint64_t ticket,i2c6_lease,reset_epoch;
    struct x9_tfa_re25_limits side_limits; /* Native side0/1, not DSP words. */
    uint32_t word_side[2],set_selector,read_selector;
    void *context;
    /* Mandatory ACTUAL same-epoch factory/profile/PA protection/MTP engine
     * and whole SET->read-command->GET exclusion of ALL SmartPA writers,
     * not just the I2C mutex. Prove wire mode/selectors and side->DSP word
     * mapping; bounds from THIS board/profile authority, no defaults.
     * Admission has NULL snapshot/reply; Factory onwards has fresh native
     * snapshot, Complete also has decoded actual GET. Stage is our journal,
     * NOT acceptance, calibration-done or protected-output proof. Complete
     * must independently establish the actual result identity and required
     * DSP calibration/protection state; equality of Q16 alone is inadequate.
     * Never consume sticky STATUS/IRQ/GetStatusChange as a readonly probe.
     * Runs under I2C6 mutex: bounded, immutable, no mutation or reentry into
     * I2C/PA/AFE/clock/transport/status/IPI, no reset/SMC/unmute. Exclusion
     * continues through faults/physical halt; no default supplier. Borrowed
     * policy/snapshot/reply pointers are private stack values; never retain
     * them. The actual context/resource lifetime is caller-owned. */
    int (*validate)(void *,const struct x9_pa_re25_program_policy *,
        const struct x9_pa_re25_snapshot *,const struct x9_tfa_re25_pair *,
        uint32_t stage);
}; /* 88B private kernel policy, not user-provided authorization. */
struct x9_pa_re25_program_result {
    struct x9_pa_re25_snapshot factory;
    struct x9_tfa_re25_pair requested,observed;
}; /* 96B observation only, not backend/Ready/unmute authorization. */
/* Production canonical batch SET -> explicitly framed readonly GetRe25C
 * exchange -> exact two-word Q16 comparison and independent final supplier.
 * SAME I2C6 mutex over the whole call pins controller/reset/power parents;
 * IPI supplier must exclude every other SmartPA writer across SET AND GET.
 * One attempt per actual READY/reset epoch, no parameter replay/retry, power
 * changes, status/IRQ reads, automatic release/reset, backend/Ready/unmute.
 * IPI must already be acquired/registered on this ticket/generation. Failure
 * retains native error/parents, including after successful SET or GET. Safe
 * cleanup remains the existing mute/drain/halt/release chain. Policy/output
 * pinned kernel spans, disjoint virtual/physical bytes, immutable policy.
 * Caller also keeps output disjoint from its authority context/resources;
 * this API cannot infer the extent of that opaque external context.
 * Lock: AFE -> I2C6 -> IPI/transport -> board/AP; suppliers never reverse it. */
int x9_pa_native_re25_program(const struct x9_pa_re25_program_policy *,
    struct x9_pa_re25_program_result *);
#ifdef __cplusplus
}
#endif
#endif
#endif
