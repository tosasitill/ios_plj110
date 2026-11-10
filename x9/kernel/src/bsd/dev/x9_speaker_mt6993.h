/* Cold register-clock/I2S cycle. Separate from physical playback readiness. */
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
#ifdef __cplusplus
}
#endif
#endif
#endif
