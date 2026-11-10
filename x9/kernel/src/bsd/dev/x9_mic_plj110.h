#ifndef X9_MIC_PLJ110_H
#define X9_MIC_PLJ110_H
#include "x9_mic_mt6681.h"
#ifdef KERNEL
/* DMA ownership/mapping/cache/drain are separate from power and calibration.
 * A real provider must retain the ring/lease for the entire boot, including
 * poisoned STOP. These attestations are requirements on the caller, not
 * observations derived from idle registers, source XML or SRAM_REQUEST.
 * Only AIN0/ADC_L is implemented here; its physical mic mapping must be
 * verified before binding. No automatic caller exists until these are met. */
#define X9_MIC_PLJ110_DMA_OWNED 1u
#define X9_MIC_PLJ110_DMA_MAPPED 2u
#define X9_MIC_PLJ110_DMA_DRAIN_VERIFIED 4u
#define X9_MIC_PLJ110_AIN0_VERIFIED 8u
#define X9_MIC_PLJ110_DMA_REQUIRED 15u
struct x9_mic_plj110_dma {
    uint32_t version, flags; /* version=1, flags=REQUIRED */
    volatile uint32_t *afe;
    void *ring;
    uint64_t bus_address;
    uint32_t ring_bytes, adc_channel; /* ADC_L/channel0 only */
    int (*quiesce)(void);
    int (*copy_frames)(uint32_t, int16_t *, uint32_t, uint32_t);
    int (*sync_for_device)(void);
};
int x9_mic_plj110_register_dma(const struct x9_mic_plj110_dma *);
/* Internal lifecycle, called only with MT6993 control lock held. Shares the
 * explicit diagnostic cold transaction budget; one session per boot until
 * actual hardware has qualified repeatable cold restart. No memif writes. */
int x9_mic_plj110_capture_prepare(volatile uint32_t *,
    struct x9_mic_hardware_status *, struct x9_mic_board_status *,
    struct x9_mic_afe_status *, struct x9_mic_analog_status *,
    struct x9_mic_digital_status *, struct x9_mic_dma_status *);
int x9_mic_plj110_capture_unprepare(void);
void x9_mic_plj110_capture_retain(int error);
/* Caller holds MT6993 AFE lock; cached memory only. */
void x9_mic_plj110_get_signal(struct x9_mic_signal_status *);
int x9_mic_plj110_digital_l(struct x9_mic_hardware_status *,
    struct x9_mic_board_status *, struct x9_mic_afe_status *, struct x9_mic_analog_status *,
    struct x9_mic_digital_status *, struct x9_mic_dma_status *);
int x9_mic_plj110_analog_l(struct x9_mic_hardware_status *,
    struct x9_mic_board_status *, struct x9_mic_afe_status *, struct x9_mic_analog_status *);
/* Called with the MT6993 control lock held, never concurrently with capture.
 * Only cold GPIO9 and unused SCP-I2C1 are supported. power_probe has no AFE
 * access; calibrate owns real AFE resources but neither entry starts DMA. */
int x9_mic_plj110_power_probe(struct x9_mic_hardware_status *,
    struct x9_mic_board_status *);
int x9_mic_plj110_calibrate(struct x9_mic_hardware_status *,
    struct x9_mic_board_status *, struct x9_mic_afe_status *);
#endif
#endif
