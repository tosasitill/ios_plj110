#ifndef X9_MIC_PLJ110_H
#define X9_MIC_PLJ110_H
#include "x9_mic_mt6681.h"
#ifdef KERNEL
/* Called with the MT6993 control lock held, never concurrently with capture.
 * Only cold GPIO9 and unused SCP-I2C1 are supported. power_probe has no AFE
 * access; calibrate owns real AFE resources but neither entry starts DMA. */
int x9_mic_plj110_power_probe(struct x9_mic_hardware_status *,
    struct x9_mic_board_status *);
int x9_mic_plj110_calibrate(struct x9_mic_hardware_status *,
    struct x9_mic_board_status *, struct x9_mic_afe_status *);
#endif
#endif
