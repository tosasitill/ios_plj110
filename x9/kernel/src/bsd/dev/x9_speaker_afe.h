/* MT6993 DL24 -> OUT4 engine. Does not manufacture an audio backend. */
#ifndef X9_SPEAKER_AFE_H
#define X9_SPEAKER_AFE_H
#include <stdint.h>
/* Cached diagnostic ABI; reading it never touches an off-domain register. */
#define X9_SPEAKER_AFE_STATUS_VERSION 2u
struct x9_speaker_afe_status {
    uint32_t version, prepared, running, error, restore_error;
    uint32_t cursor, dl24, out4;
    uint64_t played, submitted, anchor;
    uint32_t registered, leased, dirty_mask, uncertain_mask;
    uint32_t poisoned, copy_pending, mute_error, disable_error, quiesce_error;
    uint32_t reserved;
    uint64_t low_water_stops, late_polls;
};
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* The platform must coordinate with mic/ADSP: acquire owns AUDIO domain,
 * APLL2 VLP/48k clock path (captured etdm-vlp[4]=1), ETDM4 pads and DL24/OUT4 route registers.
 * It must establish safe PA mute before returning; never an empty callback.
 * DMA bus mapping/cache attributes are verified independently of a CPU VA.
 * acquire failure fully unwinds; release failure retains the resources. */
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
