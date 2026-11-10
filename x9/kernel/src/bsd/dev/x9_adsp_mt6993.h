/* MT6993 ADSP-v3 AP board owner; no speaker-ready or firmware upload ABI. */
#ifndef X9_ADSP_MT6993_H
#define X9_ADSP_MT6993_H
#include "x9_audio.h"
#include "x9_adsp_handoff.h"
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
int x9_adsp_mt6993_init(void); /* maps/registers only, NO MMIO/SMC */
int x9_adsp_mt6993_control(uint32_t operation); /* 0 boot, 1 stop; mode 2 only */
/* Kernel-only combined owner; does not claim/release the supplied Speaker
 * ticket. operation: 0 first boot, 1 explicit halt/recovery keeping parents,
 * 2 release parents after AFE/pad/clock teardown. Caller owns one immutable
 * ticket across all components and releases it LAST. Admin control is blocked
 * until op2 succeeds, even when a failed boot unwound without physical votes.
 * Boot must precede AFE parent acquisition; neither op1 nor op2 may be issued
 * while DSP-output DMA is active. These calls never grant speaker Ready. */
int x9_adsp_mt6993_speaker_control(uint32_t operation,uint64_t ticket);
/* Boot-lifetime copy, validated against /chosen/x9-ram-map. Does not map or
 * touch the pool. Caller must prove EMI/cache and the same combined lease
 * before memory operations; no captured-address fallback on missing handoff. */
int x9_adsp_mt6993_copy_handoff(struct x9_adsp_handoff *);
int x9_adsp_mt6993_status(X9AudioBoardStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
