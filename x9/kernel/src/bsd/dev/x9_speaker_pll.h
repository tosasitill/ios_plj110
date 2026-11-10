/* Cached, reference-relative clock measurements. Never speaker readiness. */
#ifndef X9_SPEAKER_PLL_H
#define X9_SPEAKER_PLL_H
#include <stdint.h>
#define X9_AUDIO_PLL_STATUS_VERSION 1u
enum { X9PLLAttempted=1u, X9PLLBaseline=2u, X9PLLCaptured=4u,
    X9PLLPowered=8u, X9PLLMeasured=16u, X9PLLRestored=32u,
    X9PLLHeld=64u, X9PLLPoisoned=128u, X9PLLMeterCaptured=256u,
    X9PLLReference=512u };
enum { X9PLLNone, X9PLLCold, X9PLLMeter, X9PLLReferenceStage,
    X9PLLPower, X9PLLMux, X9PLLMeasure, X9PLLRestore, X9PLLDone };
typedef struct {
    uint32_t version, flags, error, restore_error, stage, last_register, last_value;
    uint32_t dirty_mask, uncertain_mask, mux_owned, meter_hw, reserved0;
    uint32_t pll_before[7], pll_active[7], pll_after[7];
    uint32_t consumers[6]; /* VLP CFG7/FENC0, TOP CFG9/FENC1, MM CFG3/FENC0 */
    uint32_t meter_before[4], meter_after[4]; /* selector, CON0/1/2 */
    /* 33/34 at 26M, then three 30/34 pairs at 49.152M/196.608M.
     * Hz computed against nominal 26MHz reference; no acoustic assertion. */
    uint32_t counts[8], hz[8], reserved1;
    uint64_t ticket;
} X9AudioPLLStatus; /* 264 bytes */
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
int x9_speaker_mt6993_pll_status(X9AudioPLLStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
