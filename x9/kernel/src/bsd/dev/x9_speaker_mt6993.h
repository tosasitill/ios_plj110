/* Cold diagnostics and live combined AUDIO/APLL sublease. Neither is Ready. */
#ifndef X9_SPEAKER_MT6993_H
#define X9_SPEAKER_MT6993_H
#include <stdint.h>
#define X9_AUDIO_CLOCK_STATUS_VERSION 1u
enum { X9ClockAttempted=1u, X9ClockAOCaptured=2u, X9ClockAFECaptured=4u,
    X9ClockConfigured=8u, X9ClockRestored=16u, X9ClockHeld=32u,
    X9ClockPoisoned=64u, X9ClockInitIssued=128u, X9ClockSidebandsIssued=256u,
    X9ClockAPLLCaptured=512u };
enum { X9ClockNone, X9ClockMap, X9ClockCold, X9ClockInit, X9ClockDomains,
    X9ClockMux, X9ClockIdle, X9ClockGates, X9ClockSidebands, X9ClockI2S,
    X9ClockRecheck, X9ClockRestore, X9ClockDone };
typedef struct {
    uint32_t version, flags, stage, error, restore_error, last_register, last_value, owned_vote;
    uint32_t borrowed_vote, parked_vote, dirty_mask, uncertain_mask;
    uint32_t ap_before, ap_after, all_before, all_after, ack_before, ack_after;
    uint32_t done, foreign_before, foreign_after;
    uint32_t vlp_before, vlp_after, fenc_before, fenc_after;
    uint32_t top_before, top_configured, top_after;
    uint32_t out_before[4], out_configured[4], out_after[4];
    /* CON0/CON1/CON3/FENC/TUNER/PCW0/PCW1: AO snapshot only, never written.
     * FENC bit4 is not a PLL lock/rate/ownership certificate. */
    uint32_t apll[7], reserved;
    uint64_t smc_result[2], ticket;
} X9AudioClockStatus; /* 216 bytes, cached selector 12 */
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit mode2 only; once per boot AFTER AP lease admission. No DMA, engine
 * enable, pad/route, PA writes or APLL retuning. Failures preserve journals.
 * AO erratum floor, if added, remains parked just as in ADSP stop; it is not
 * an active playback lease. This cycle does NOT register an AFE/backend. */
int x9_speaker_mt6993_clock_probe(void);
int x9_speaker_mt6993_clock_status(X9AudioClockStatus *);
int x9_speaker_mt6993_pll_probe(void);
struct x9_speaker_clock_policy {
    uint32_t version,generation;
    uint64_t ticket;
    void *context;
    /* Mandatory physical no-outstanding-DL24 access proof before clock
     * teardown. Independently qualify hardware; not a cached enable bit,
     * empty callback or x9_speaker_afe_status. Never re-enter this module. */
    int (*validate_drained)(void *,uint64_t ticket,uint64_t afe_bus,uint32_t bytes);
};
#define X9_SPEAKER_CLOCK_POLICY_VERSION 1u
/* Production sublease: starts AFTER real ADSP boot/READY on the same ticket.
 * Owns AUDIO AP vote, VLP clocks, preconfigured APLL2 and AFE tuner/ENGEN2;
 * preserves board-owned TOP/INFRA/AO. No OUT4/route/pad/PA/DMA enable writes.
 * Backend serializes calls. Failed acquisition retains Held until explicit
 * release after real producer halt/drain; it does not release AP ticket.
 * A new attempt requires full cleanup and a new real READY generation.
 * validate/afe prove physical accessibility, NOT permission for a new stream:
 * same-generation transport quarantine or a historical stream error alone
 * does not prevent AFE disable/drain. They still require fresh board/HWV,
 * mappings and owned clock images; unknown requests/journals stay retained.
 * Caller must independently gate start/submit on its live DSP/PCM session.
 * Lock order: AFE -> clock -> board or transport -> board -> AP leaf.
 * No provider callback may call back into this module or the AFE engine. */
int x9_speaker_mt6993_clock_acquire(const struct x9_speaker_clock_policy *);
int x9_speaker_mt6993_clock_validate(uint64_t ticket);
int x9_speaker_mt6993_clock_release(uint64_t ticket);
int x9_speaker_mt6993_clock_afe(uint64_t ticket,volatile uint32_t **);
int x9_speaker_mt6993_clock_held(uint64_t ticket); /* cached only */
/* One native pad subchild, monotonically minted (never a readiness bit).
 * Hold requires fresh Online/no-error. Validate permits physical cleanup
 * after a same-generation transport fault. Drop requires real board halt.
 * An outstanding pad child makes clock_release return EBUSY BEFORE any
 * clock mutation or flow poisoning. Pads -> clock -> board lock order. */
int x9_speaker_mt6993_clock_pad_hold(uint64_t ticket,uint64_t *lease);
int x9_speaker_mt6993_clock_pad_validate(uint64_t ticket,uint64_t lease);
int x9_speaker_mt6993_clock_pad_drop(uint64_t ticket,uint64_t lease);
#ifdef __cplusplus
}
#endif
#endif
#endif
