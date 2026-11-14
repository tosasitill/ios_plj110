/* One-shot speaker bring-up (x9_dsp_boot test path, mode2). */
#ifndef X9_SPEAKER_BRINGUP_H
#define X9_SPEAKER_BRINGUP_H
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
int x9_speaker_bringup_run(void);
/* Backend mode (x9_spk_backend, default 1): call WITHOUT the X9Audio lock. */
int x9_speaker_backend_register(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
