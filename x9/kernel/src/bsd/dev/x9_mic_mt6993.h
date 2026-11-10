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
    uint32_t version, adc_channel; /* version=3; verified board mic mapping */
    volatile uint32_t *afe;       /* device mapping of X9_MIC_AFE_PHYS */
    void *dma;                    /* exclusively owned stereo S16 ring */
    uint64_t dma_bus_address;    /* AFE-visible, NOT a guessed CPU physical address */
    uint32_t dma_bytes;          /* 16-byte aligned, 64..128 KiB */
    /* Claim exclusive VUL1/ADDA/codec, SCP wake+I2C arbitration, GPIO9 fresh
     * codec power, MT6687 DCXO, proven ordinary 48k/26M-family clocks,
     * aud_ul0 and pads (do not assume an APLL2 requirement from the rate).
     * prepare does NOT start ADC/TX/DMA. Codec init/calibration run centrally
     * after prepare succeeds. No borrowed active codec or dummy success.
     * unprepare handles partial resource prepare; after codec/ADC/DMA stop
     * it must restore the cold GPIO/rails baseline and prove readback.
     * All callbacks run with the polling lock held: never wait for worker. */
    int (*prepare)(void);
    int (*unprepare)(void);
    struct x9_mic_mt6681_io codec;
    /* Full verified AIN/PGA/DCC/micbias/ADC RC calibration -> real ADDA UL
     * at 48k, R-MISO1 capture routing and RX delay using chosen_phase/cycle.
     * Only called after successful calibration and loopback-off readback.
     * Must not overwrite calibrated codec phase bits or emit test/sine PCM.
     * capture_unprepare safely unwinds even a partially failed capture_prepare,
     * before codec clocks/transport are restored or released. */
    int (*capture_prepare)(const struct x9_mic_hardware_status *);
    int (*capture_unprepare)(void);
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
/* Call only after platform transport/identity/calibration and exclusive DMA
 * contract have been verified. There is NO board caller in the current tree. */
int x9_mic_mt6993_register(const struct x9_mic_mt6993_platform *);
void x9_mic_mt6993_get_hardware(struct x9_mic_hardware_status *);
int x9_mic_mt6993_power_probe(void);
int x9_mic_mt6993_calibrate(void);
int x9_mic_mt6993_analog_l(void);
int x9_mic_mt6993_digital_l(void);
void x9_mic_mt6993_get_digital(struct x9_mic_digital_status *);
void x9_mic_mt6993_get_dma(struct x9_mic_dma_status *);
void x9_mic_mt6993_get_analog(struct x9_mic_analog_status *);
void x9_mic_mt6993_get_afe(struct x9_mic_afe_status *);
void x9_mic_mt6993_get_board(struct x9_mic_board_status *);
#endif
#endif
