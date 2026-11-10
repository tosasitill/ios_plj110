/* MT6993 VUL1 DMA engine. Board-side prerequisites are intentionally explicit. */
#ifndef X9_MIC_MT6993_H
#define X9_MIC_MT6993_H
#include "x9_microphone.h"
#ifdef KERNEL
/* Captured PLJ110 DT: AFE 0x1a110000/0x10000, audio SRAM
 * 0x1a120000/0x20000. Do not assume LK powered it or left any route valid. */
#define X9_MIC_AFE_PHYS 0x1a110000ULL
#define X9_MIC_AFE_BYTES 0x10000u
struct x9_mic_mt6993_platform {
    uint32_t version, adc_channel; /* 0/1, verified mapping to bottom microphone */
    volatile uint32_t *afe;       /* device mapping of X9_MIC_AFE_PHYS */
    void *dma;                    /* exclusively owned stereo S16 ring */
    uint64_t dma_bus_address;    /* AFE-visible, NOT a guessed CPU physical address */
    uint32_t dma_bytes;          /* 16-byte aligned, 64..128 KiB */
    /* claim/release VUL1, CONN020/021, ADDA and resources against other audio
     * users. prepare must power/read actual MT6681, set DCC mic type (DT=3),
     * bias/ADC, pads, aud_ul0 clock, APLL2/48k family, MTKAIFv4 calibration,
     * audio-r-miso1 route and ADDA UL 48k. No sine/loopback source is allowed.
     * unprepare must safely undo partial prepare and prove DMA drained; it
     * runs with the AFE polling lock held, so never waits for that worker. */
    int (*prepare)(void);
    int (*unprepare)(void);
    /* For noncoherent mappings, invalidate only DMA-written complete bytes
     * before CPU loads; also prevent dirty CPU lines overwriting DMA output. */
    void (*sync_for_cpu)(uint32_t offset, uint32_t bytes);
    void (*sync_for_device)(void);
};
/* Call only after platform transport/identity/calibration and exclusive DMA
 * contract have been verified. There is NO board caller in the current tree. */
int x9_mic_mt6993_register(const struct x9_mic_mt6993_platform *);
#endif
#endif
