/* Native shared PCM -> core-A IRQ -> actual producer halt lifecycle.
 * Not a registered speaker backend or an output publication source. */
#ifndef X9_DSP_PLAYBACK_H
#define X9_DSP_PLAYBACK_H
#include "x9_dsp_pcm.h"
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* Shared must already be acquired/registered under this real Speaker ticket
 * and READY generation. Validate the explicit AFE descriptor with the actual
 * shared/firmware route binding BEFORE OPEN; no guessed memif/IRQ or caller
 * supplied success callbacks. Fixed stereo S16/48k/480-frame periods.
 * One attempt per generation; failure pins acquired dependencies until
 * explicit halt/release. Never boot DSP, enable AFE or unmute. */
int x9_dsp_playback_prepare(uint64_t ticket,uint32_t generation,
    const struct x9_dsp_pcm_buffer *afe,uint32_t start_frames);
/* Kernel-resident input only, pinned and immutable through this bounded call.
 * No user pointer. Copies ONLY the DSP input ring; output is DSP-owned. */
int x9_dsp_playback_submit(uint64_t ticket,uint32_t generation,
    const int16_t *stereo,uint32_t frames);
/* Bind actual SPI570 to the same session mutex before PCM START. Success is
 * a START command, not output committed, AFE-on or audible audio. */
int x9_dsp_playback_start(uint64_t ticket,uint32_t generation);
/* For the coordinator's halt_producer callback AFTER dual PA mute and AFE
 * disable/drain: close submissions -> targeted IRQ quiesce/worker drain ->
 * best-effort PCM STOP/CLOSE -> independent selected DMA drain -> board op1
 * -> fresh halt. No AFE re-entry or journal/map/clock/rail/ticket release.
 * Return 0 proves physical halt, NOT successful protocol cleanup/health;
 * first errors survive. Retained SEMA_AUDIO requires explicit shared resolve
 * after board halt requalifies access, then another explicit halt call.
 * Failure BEFORE obtaining a shared pin cannot authorize reset: coordinator
 * must independently halt/drain, then this validates that halt. */
int x9_dsp_playback_halt(uint64_t ticket,uint32_t generation);
/* AFTER actual halt: retire IRQ references, drop shared child after another
 * independent drain, finally retire session. Any failure retains object/pins.
 * Caller then restores/releases AFE/shared, pads/PA/clocks/board in dependency
 * order, and releases the AP ticket LAST. No same-generation retry. */
int x9_dsp_playback_release(uint64_t ticket,uint32_t generation);
/* Lock order: external AFE -> lifecycle -> session -> IRQ/shared -> board.
 * Bind/quiesce/release NEVER hold session mutex. All suppliers must outlive
 * this object and must not reenter AFE/playback/shared/IRQ. No user ABI change. */
#ifdef __cplusplus
}
#endif
#endif
#endif
