/* MT6993 DL24 -> OUT4 engine. Does not manufacture an audio backend. */
#ifndef X9_SPEAKER_AFE_H
#define X9_SPEAKER_AFE_H
#include <stdbool.h>
#include <stdint.h>
/* Cached diagnostic ABI; reading it never touches an off-domain register. */
#define X9_SPEAKER_AFE_STATUS_VERSION 4u
enum { X9AFEGuardNone, X9AFEGuardPrepare, X9AFEGuardStart, X9AFEGuardCopy,
       X9AFEGuardPoll, X9AFEGuardDisable, X9AFEGuardRestore };
struct x9_speaker_afe_status {
    uint32_t version, prepared, running, error, restore_error;
    uint32_t cursor, dl24, out4;
    uint64_t played, submitted, anchor;
    uint32_t registered, leased, dirty_mask, uncertain_mask;
    uint32_t poisoned, copy_pending, mute_error, disable_error, quiesce_error;
    uint32_t reserved;
    uint64_t low_water_stops, late_polls;
    uint32_t lease_error, config_error, guard_stage, guard_reg;
    uint32_t guard_expected, guard_actual;
    uint64_t lease_checks, config_checks;
    /* First guard failure, if journal-related; values under its field mask. A valid
     * prior is meaningful only for an uncertain last write, never a lease
     * proof or permission to replay a hardware action. */
    uint32_t guard_prior, guard_saved, guard_index, guard_prior_valid;
    uint64_t prior_observations, no_write_reconciliations;
};
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* The platform must coordinate with mic/ADSP: acquire owns AUDIO domain,
 * APLL2 VLP/48k clock path (captured etdm-vlp[4]=1), ETDM4 pads and DL24/OUT4 route registers.
 * It must establish safe PA mute before returning; never an empty callback.
 * DMA bus mapping/cache attributes are verified independently of a CPU VA.
 * Private platform v3: held is a cached boot-lifetime ownership predicate,
 * not a fresh MMIO read. acquire may partially fail; held MUST stay true
 * until mute/quiesce/restore/release authoritatively finish. acquire never
 * enables DMA. release returning zero must also leave held false. The engine
 * owns a shared AP transition ticket until physical cleanup finishes.
 * validate must recheck the SAME physical lease, using AO-safe power/clock
 * checks before any domain MMIO, and the original pad/route and DMA/cache
 * binding. It does not acquire/reset/reparent, mutate readiness or return 0
 * from a cached held flag alone. It validates ownership/accessibility, not
 * an idle-only register image: engine-owned OUT4/DL24 may be running. Return
 * a positive errno on failure (EAGAIN is fatal here, not a cursor retry).
 * Failure leaves mappings/resources pinned.
 * DMA VA/bus/cache bindings cannot change or be unmapped during the lease.
 * All callbacks are bounded, serialized and must not reenter this engine.
 * mute/quiesce independently qualify their own hardware paths; even after
 * validate fails they may be called for safe cleanup, or must fail without
 * accessing an off-domain/unowned CSR. No engine AFE access follows failure. */
#define X9_SPEAKER_AFE_PLATFORM_VERSION 3u
struct x9_speaker_afe_platform {
    uint32_t version, verified, dma_bytes;
    volatile uint32_t *afe;
    void *dma, *context;
    uint64_t dma_bus;
    int (*acquire)(void *);
    int (*release)(void *);
    int (*mute)(void *); /* both physical PAs, on every stop/fault */
    int (*quiesce)(void *); /* authoritative no-outstanding-DL24 DMA, bounded */
    int (*sync_device)(void *,uint32_t offset,uint32_t bytes);
    bool (*held)(void *);
    int (*validate)(void *);
};
#define X9_SPEAKER_AFE_RESOURCES 15u /* power/VLP clock + pads/route + DMA/cache + dual PA mute */
int x9_speaker_afe_register(const struct x9_speaker_afe_platform *);
int x9_speaker_afe_prepare(void); /* configure with DL24/OUT4 disabled */
int x9_speaker_afe_start(const int16_t *stereo,uint32_t frames,uint64_t *anchor);
int x9_speaker_afe_submit(const int16_t *stereo,uint32_t frames);
int x9_speaker_afe_position(uint64_t *played);
int x9_speaker_afe_stop(void); /* mute -> disable -> restore -> release */
int x9_speaker_afe_status(struct x9_speaker_afe_status *);
#ifdef __cplusplus
}
#endif
#endif
#endif
