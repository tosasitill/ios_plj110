/* Cached disabled DL24 -> OUT4 configuration cycle. Never Ready or DMA proof. */
#ifndef X9_SPEAKER_PATH_H
#define X9_SPEAKER_PATH_H
#include <stdint.h>
#define X9_AUDIO_PATH_STATUS_VERSION 1u
enum { X9PathAttempted=1u, X9PathCaptured=2u, X9PathConfigured=4u,
    X9PathRestored=8u, X9PathHeld=16u, X9PathPoisoned=32u };
enum { X9PathNone, X9PathAdmission, X9PathConfigure, X9PathVerify,
    X9PathRestore, X9PathDone };
typedef struct {
    uint32_t version, flags, error, restore_error, stage, last_register, last_value;
    uint32_t dirty_mask, uncertain_mask, reserved[3];
    uint32_t before[4], configured[4], after[4]; /* cowork, DL24 CON0, routes */
    uint32_t routes_before[14], routes_after[14]; /* 116/117, words0/1/2/4/5/6/7 */
    uint32_t dma_before[4], dma_after[4]; /* BASE_MSB/BASE/END_MSB/END, never written */
    uint32_t control_before[3], control_after[3]; /* MEM_UP_MSB/MEM_UP/PAD_TOP, read only */
    uint64_t ticket;
} X9AudioPathStatus; /* v1 / 272B */
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
int x9_speaker_mt6993_path_status(X9AudioPathStatus *);
int x9_speaker_mt6993_path_probe(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
