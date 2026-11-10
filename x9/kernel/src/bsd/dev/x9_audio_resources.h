/* AP audio transition arbiter; software ownership is NOT hardware readiness. */
#ifndef X9_AUDIO_RESOURCES_H
#define X9_AUDIO_RESOURCES_H
#include <stdint.h>
#define X9_AUDIO_RESOURCE_STATUS_VERSION 1u
enum { X9AudioResourceNone, X9AudioResourceADSP, X9AudioResourceMic,
       X9AudioResourceSpeaker };
typedef struct {
    uint32_t version, owner, last_owner, held_error;
    uint32_t last_error, release_error, refusals, reserved;
    uint64_t ticket, claims, releases, rejected_releases;
} X9AudioResourceStatus;
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* Nonrecursive, fail-fast claim. Keep the ticket across physical callbacks,
 * including failed rollback. Release ONLY after authoritative cleanup.
 * Lock order: consumer lock -> this short leaf lock. No MMIO/callback/wait
 * runs under the leaf lock; status acquires no consumer locks.
 * Present independent owners are mutually exclusive. A future combined
 * DSP+AFE backend must coordinate one lease, not nest independent claims.
 * This excludes local AP owners only; HWV/foreign-XPU/clock/pad/DMA admission
 * and readback remain mandatory. No user-controlled claim/release API. */
int x9_audio_resources_claim(uint32_t owner, uint64_t *ticket);
int x9_audio_resources_release(uint32_t owner, uint64_t ticket);
void x9_audio_resources_fault(uint32_t owner, uint64_t ticket, int error);
int x9_audio_resources_status(X9AudioResourceStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
