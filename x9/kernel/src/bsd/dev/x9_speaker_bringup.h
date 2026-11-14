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
/* audio43: the mic asks an IDLE speaker backend to hand back the shared
 * audio lease (full teardown, re-taken on the next start). 0 = released. */
int x9_speaker_backend_yield(void);
/* audio46: take (and keep) the speaker AFE base so the mic can borrow it;
 * base_held reports whether that base is currently held. */
int x9_speaker_backend_hold_for_mic(void);
int x9_speaker_backend_base_held(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
