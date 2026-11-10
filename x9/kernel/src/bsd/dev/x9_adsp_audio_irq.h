/* This-boot core-A AUDIO interrupt topology; not DMA/output authority. */
#ifndef X9_ADSP_AUDIO_IRQ_H
#define X9_ADSP_AUDIO_IRQ_H
#include <stdint.h>
#define X9_ADSP_AUDIO_INTID 570u /* GIC SPI 0x21a + 32 */
struct x9_adsp_audio_irq_layout {
    uint32_t version,core,intid,trigger;
    uint32_t cfg_base,gicd_base,gicd_bytes,reserved;
};
static inline int x9_adsp_audio_irq_layout_valid(const struct x9_adsp_audio_irq_layout *p)
{
    return p && p->version==1 && !p->core && p->intid==X9_ADSP_AUDIO_INTID &&
        p->trigger==4 && p->cfg_base==0x1a000000 && p->gicd_base==0x06800000 &&
        p->gicd_bytes==0x40000 && !p->reserved;
}
#if defined(KERNEL)
#include <stdbool.h>
#include <kern/locks.h>
struct x9_dsp_pcm_session;
#ifdef __cplusplus
extern "C" {
#endif
/* Backend serializes ALL PCM/lifecycle operations with this same mutex.
 * Bind/quiesce/release must be called WITHOUT holding it. The session and
 * lock live through release. No alternate poll or simulated IRQ admission.
 * One bind attempt per real READY generation. Bind requires the actual
 * shared/PCM and firmware AFE route binding already established. */
int x9_adsp_audio_irq_bind(uint64_t ticket,uint32_t generation,
    struct x9_dsp_pcm_session *,lck_mtx_t *session_lock);
/* Mask and drain threaded delivery/active GIC ownership, then permit halt;
 * board child stays pinned through final release. Does NOT restore routing
 * or clear pending while the producer is live. Failure stops new callbacks
 * and retains all ownership; a lost mapping cannot be used to force a mask. */
int x9_adsp_audio_irq_quiesce(void);
/* After real board halt, retire pending and restore ONLY this SPI's saved
 * priority/route. Retains first error and uncertain journal on conflict. */
int x9_adsp_audio_irq_release(void);
/* sleh_irq only: masks this SPI and wakes the real thread; NO board/RAM
 * access, sleep lock, DTOA inference or EOI here. sleh retains the IAR EOI. */
bool x9_adsp_audio_irq_filter(uint32_t intid);
#ifdef __cplusplus
}
#endif
#endif
#endif
