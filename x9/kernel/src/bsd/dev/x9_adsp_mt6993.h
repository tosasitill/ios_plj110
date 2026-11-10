/* MT6993 ADSP-v3 AP board owner; no speaker-ready or firmware upload ABI. */
#ifndef X9_ADSP_MT6993_H
#define X9_ADSP_MT6993_H
#include "x9_audio.h"
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
int x9_adsp_mt6993_init(void); /* maps/registers only, NO MMIO/SMC */
int x9_adsp_mt6993_control(uint32_t operation); /* 0 boot, 1 stop; mode 2 only */
int x9_adsp_mt6993_status(X9AudioBoardStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
