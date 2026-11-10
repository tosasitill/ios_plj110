/* MT6993 VUL1 DMA engine. Board-side prerequisites are intentionally explicit. */
#ifndef X9_MIC_MT6993_H
#define X9_MIC_MT6993_H
#include "x9_microphone.h"
#ifdef KERNEL
#include "x9_mic_mt6681.h"
/* Captured PLJ110 DT: AFE 0x1a110000/0x10000, audio SRAM
 * 0x1a120000/0x20000. Do not assume LK powered it or left any route valid. */
#define X9_MIC_AFE_PHYS 0x1a110000ULL
#define X9_MIC_AFE_BYTES 0x10000u
struct x9_mic_mt6993_platform {
    uint32_t version, adc_channel; /* version=6; verified board mic mapping */
    volatile uint32_t *afe;       /* device mapping of X9_MIC_AFE_PHYS */
    void *dma;                    /* exclusively owned stereo S16 ring */
    uint64_t dma_bus_address;    /* AFE-visible, NOT a guessed CPU physical address */
    uint32_t dma_bytes;          /* 16-byte aligned, 64..128 KiB */
    uint32_t dma_memory_type;    /* SRAM/DRAM; DRAM also requires APSRC/DDREN */
    /* v6: one board-owned journal for cold power, MTKAIF calibration,
     * ADC RC, codec SRC/TX and AFE RX. prepare leaves real capture active,
     * but never writes memif/IRQ/DMA. No second engine-owned codec journal.
     * Status pointers remain valid until unprepare; callbacks hold the AFE
     * polling lock and must never wait for the worker. unprepare is called
     * only after complete DMA drain/address restoration, including failures.
     * A failed partial prepare must also be unwound; no borrowed live codec. */
    int (*prepare)(volatile uint32_t *afe, struct x9_mic_hardware_status *,
        struct x9_mic_board_status *, struct x9_mic_afe_status *,
        struct x9_mic_analog_status *, struct x9_mic_digital_status *,
        struct x9_mic_dma_status *, uint32_t dma_memory_type);
    int (*unprepare)(void);
    /* Before clearing prior status/journal storage. A successful full STOP
     * receipt permits another cold transaction; a diagnostic/failed/retained
     * transaction must never be recycled. Memory-only, with AFE lock held. */
    int (*rearm)(void);
    /* Called after ON clear/readback, including partial start. Must prove
     * the entire write path has drained, NOT just ON=0, stable CUR or the
     * local VUL1 MON0. Failure pins journal/buffer/clocks/transport for boot.
     * Runs with polling lock held; no worker waits or guessed-delay success. */
    int (*dma_quiesce)(void);
    /* The provider owns CPU/cache semantics. For device-mapped SRAM use
     * aligned volatile MMIO loads (not ordinary int16_t/memcpy loads). For
     * DRAM synchronize complete DMA-written bytes first. Copy exactly frames
     * of the selected channel; never synthesize data. Offsets cannot wrap.
     * sync_for_device prevents dirty CPU lines overwriting DMA output. */
    int (*copy_frames)(uint32_t offset, int16_t *mono, uint32_t frames, uint32_t channel);
    int (*sync_for_device)(void);
};
/* Generic providers must establish the full board pipeline above. PLJ110
 * uses x9_mic_plj110_register_dma; no unverified provider is auto-registered. */
int x9_mic_mt6993_register(const struct x9_mic_mt6993_platform *);
void x9_mic_mt6993_get_hardware(struct x9_mic_hardware_status *);
int x9_mic_mt6993_power_probe(void);
int x9_mic_mt6993_calibrate(void);
int x9_mic_mt6993_analog_l(void);
int x9_mic_mt6993_digital_l(void);
void x9_mic_mt6993_get_digital(struct x9_mic_digital_status *);
void x9_mic_mt6993_get_dma(struct x9_mic_dma_status *);
void x9_mic_mt6993_get_signal(struct x9_mic_signal_status *);
void x9_mic_mt6993_get_analog(struct x9_mic_analog_status *);
void x9_mic_mt6993_get_afe(struct x9_mic_afe_status *);
void x9_mic_mt6993_get_board(struct x9_mic_board_status *);
#endif
#endif
