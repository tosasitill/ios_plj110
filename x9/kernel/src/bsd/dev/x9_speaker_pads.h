#ifndef X9_SPEAKER_PADS_H
#define X9_SPEAKER_PADS_H
#include <stdint.h>
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#define X9_SPEAKER_PADS_POLICY_VERSION 1u
struct x9_speaker_pads_policy {
    uint32_t version,generation;
    uint64_t ticket;
    void *context;
    /* Mandatory REAL dual-PA mute confirmation, before pinmux changes.
     * No empty success provider or cached software mute flag. */
    int (*mute)(void *,uint64_t ticket);
    /* Independent outstanding-DL24 drain, not enable-image/PCM counters. */
    int (*validate_drained)(void *,uint64_t ticket,uint64_t afe_bus,uint32_t bytes);
};
/* Current-FDT I2SIN4/OUT4, GPIO108..111 mode5. Parent must be the production
 * AUDIO/APLL2 sublease with real Online generation. Own ONLY MODE/DIR/SMT/
 * PU/PD bits; preserve and recheck DO/IES/drive strength. Refuse inherited
 * alternate mode/output. Failures retain journals and clock subchild.
 * release: mute -> real board halt/drain -> restore pads -> child drop.
 * Does not release the Speaker ticket, restart DSP or enable OUT4/PA/DMA.
 * validate proves physical pad image, not permission to start a PCM stream.
 * Backend serializes lifecycle; AFE -> pads -> clock -> board/AP leaf.
 * Policy callbacks MUST NOT re-enter AFE/pads or clock lifecycle. The PA
 * mute provider MAY use read-only PA reset/clock physical validation
 * (pads -> PA -> clock); never invoke it from a clock-held callback. */
int x9_speaker_pads_acquire(const struct x9_speaker_pads_policy *);
int x9_speaker_pads_validate(uint64_t ticket);
int x9_speaker_pads_release(uint64_t ticket);
int x9_speaker_pads_held(uint64_t ticket); /* cached, not readiness */
#ifdef __cplusplus
}
#endif
#endif
#endif
