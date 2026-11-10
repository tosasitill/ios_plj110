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
/* Fresh same-ticket board validation, not READY or an EMI grant. */
int x9_adsp_mt6993_speaker_validate(uint64_t ticket);
int x9_adsp_mt6993_speaker_halted_validate(uint64_t ticket);
/* One physical shared-RAM child pins board parents before map. Reset/halt is
 * allowed; restart/parent release is not. Drop only after qualified halt and
 * actual unmap, while caller still owns the same ticket. No power acquisition. */
int x9_adsp_mt6993_shared_hold(uint64_t ticket,uint64_t *lease);
int x9_adsp_mt6993_shared_drop(uint64_t ticket,uint64_t lease);
/* Independent AUDIO/APLL child, alongside the shared-RAM child. Holds the
 * same board parents but never claims another AP ticket. */
int x9_adsp_mt6993_clock_hold(uint64_t ticket,uint64_t *lease);
int x9_adsp_mt6993_clock_drop(uint64_t ticket,uint64_t lease);
/* MT6993 v3 SEMA_AUDIO: CFG+0x64, AP way bit 4*3+2. Thread context,
 * nonrecursive. lease is a monotonically minted per-acquisition token,
 * including an uncertain toggle: retain it on error for explicit release.
 * No reset/start/power-off is allowed while held or uncertain. A failed
 * qualification performs no further domain access. An explicit halt may
 * requalify but returns EBUSY without resetting an outstanding semaphore;
 * the caller must then resolve release before attempting halt again. */
int x9_adsp_mt6993_audio_sem_acquire(uint64_t ticket,uint64_t *lease);
int x9_adsp_mt6993_audio_sem_validate(uint64_t ticket,uint64_t lease);
int x9_adsp_mt6993_audio_sem_release(uint64_t ticket,uint64_t lease);
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
