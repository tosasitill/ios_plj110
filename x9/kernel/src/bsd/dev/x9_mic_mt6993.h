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
    uint32_t version, adc_channel; /* version=2; verified bottom mic mapping */
    volatile uint32_t *afe;       /* device mapping of X9_MIC_AFE_PHYS */
    void *dma;                    /* exclusively owned stereo S16 ring */
    uint64_t dma_bus_address;    /* AFE-visible, NOT a guessed CPU physical address */
    uint32_t dma_bytes;          /* 16-byte aligned, 64..128 KiB */
    /* Claim exclusive VUL1/ADDA/codec, SCP wake+I2C arbitration, GPIO9 fresh
     * codec power, MT6687 DCXO, APLL2/48k-family clocks, aud_ul0 and pads.
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
    /* For noncoherent mappings, invalidate only DMA-written complete bytes
     * before CPU loads; also prevent dirty CPU lines overwriting DMA output. */
    void (*sync_for_cpu)(uint32_t offset, uint32_t bytes);
    void (*sync_for_device)(void);
};
/* Call only after platform transport/identity/calibration and exclusive DMA
 * contract have been verified. There is NO board caller in the current tree. */
int x9_mic_mt6993_register(const struct x9_mic_mt6993_platform *);
void x9_mic_mt6993_get_hardware(struct x9_mic_hardware_status *);
int x9_mic_mt6993_power_probe(void);
int x9_mic_mt6993_calibrate(void);
int x9_mic_mt6993_analog_l(void);
void x9_mic_mt6993_get_analog(struct x9_mic_analog_status *);
void x9_mic_mt6993_get_afe(struct x9_mic_afe_status *);
void x9_mic_mt6993_get_board(struct x9_mic_board_status *);
#endif
#endif
