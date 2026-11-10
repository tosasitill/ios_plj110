/* AP-owned suballocation of the live ADSP AUDIO_COMMON grant. */
#ifndef X9_DSP_SHARED_H
#define X9_DSP_SHARED_H
#include "x9_dsp_pcm.h"
#define X9_DSP_SHARED_CORE_BYTES 16u
#define X9_DSP_SHARED_CORE_ALLOC 256u
#define X9_DSP_SHARED_TASK_BYTES 1024u
#define X9_DSP_SHARED_RING_BYTES 122880u /* 64 * 480 * stereo S16 */
#define X9_DSP_SHARED_MAP_BYTES 0x44000u
#if defined(KERNEL)
struct x9_dsp_shared_policy {
    uint32_t version,generation,task_scene,daemon_scene;
    uint32_t afe_memif,reserved; /* immutable explicit C memif10/14, reserved=0 */
    uint64_t ticket;
    void *context;
    /* Mandatory live authority, NOT a boolean provided by userspace. Prove
     * exclusive physical suballocation, current EMI/SMPU AP/DSP/AFE access,
     * matching firmware build's daemon enum and core-A task routing. No
     * allocation, reset, power, or cache-attribute changes in this callback.
     * Called before map, every memory access, sync, send and cleanup. The
     * authority outlives all faults; mappings/physical lease remain pinned
     * until explicit qualified halt+release. Whole pool is registered to
     * DSP, but AP maps/touches ONLY the inner 16K-aligned suballocation.
     * No implementation is installed by default. Handoff/SMC a0=0/READY
     * alone cannot supply this authority. Never re-enter this module. */
    int (*validate_memory)(void *,uint64_t ticket,uint32_t generation,
        uint32_t task_scene,uint32_t daemon_scene,uint64_t pool_bus,
        uint32_t pool_bytes,uint64_t map_bus,uint32_t map_bytes,uint32_t afe_memif);
    /* Mandatory actual firmware/AFE route supplier, not a second boolean
     * for validate_memory. Shared validates the exact dedicated AFE region;
     * this supplier additionally proves the current task's hardware DL
     * memif, IRQ usage/ownership, format and period contract. No mutation,
     * allocation or re-entry. DL24 C enum10 and DL_24CH enum14 are separate
     * supported layouts; scene32 is neither. Prove policy.afe_memif, never
     * silently substitute the other; old FDT playback14 is not DL24 proof. The route
     * must remain pinned through all faults, halt, and final unmap. */
    int (*validate_afe_route)(void *,uint64_t ticket,uint32_t generation,
        uint32_t task_scene,const struct x9_dsp_pcm_buffer *);
    /* Mandatory independent authoritative SELECTED memif drain/no DMA proof
     * before unmap. Not a cached AFE leased/running flag. Qualify its own
     * power/clock/register path and do not re-enter AFE/shared engines. */
    int (*validate_drained)(void *,uint64_t ticket,uint32_t generation,
        uint64_t afe_bus,uint32_t afe_bytes,uint32_t afe_memif);
};
#define X9_DSP_SHARED_POLICY_VERSION 3u
#ifdef __cplusplus
extern "C" {
#endif
/* Singleton boot-lifetime owner; backend serializes these and PCM operations.
 * One initialization attempt per real READY generation, no retry by creating
 * another context. These do NOT boot/reset DSP, acquire board power, register
 * a speaker backend, authorize unmute, or release the caller's AP ticket. */
int x9_dsp_shared_acquire(const struct x9_dsp_shared_policy *);
int x9_dsp_shared_register(void); /* pool8/COREMEM7 BYPASS != DSP acceptance */
int x9_dsp_shared_pcm_binding(struct x9_dsp_pcm_binding *);
int x9_dsp_shared_afe_region(struct x9_dsp_pcm_region *);
int x9_dsp_shared_afe_sync_device(uint32_t offset,uint32_t bytes);
int x9_dsp_shared_afe_sync_cpu(uint32_t offset,uint32_t bytes);
/* Only the production core-A AUDIO IRQ thread may call this. It must first
 * qualify/ack the real IRQ source; not a mailbox IPI9 or arbitrary RAM poll.
 * Fresh registered core DTOA flag and this exact PCM session's DTOA are
 * observed under SEMA_AUDIO; only then clear the bit. Unknown bits retained.
 * No current IRQ provider is registered by this module. */
int x9_dsp_shared_dtoa(struct x9_dsp_pcm_session *);
/* Explicit error cleanup only: after a failed halt requalifies board access,
 * resolve a retained semaphore without touching RAM/replaying its toggle.
 * Does not clear the shared fault or grant another playback attempt. */
int x9_dsp_shared_resolve_semaphore(void);
/* Call only after producer halt and AFE DMA drain/journal teardown, while
 * board parents and memory authority still live. Rechecks Offline and the
 * same generation; refuses outstanding/uncertain semaphore. Historical fault
 * is not a reset or next-session admission. Caller releases physical lease
 * and board parents AFTER this succeeds, and the AP ticket LAST. */
int x9_dsp_shared_release(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
