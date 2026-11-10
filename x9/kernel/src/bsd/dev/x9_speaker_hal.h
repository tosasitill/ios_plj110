/* Complete own-code signed HAL resources. No hardware/readiness authority.
 * Definitions are generated only by the speaker builder after codesign. */
#ifndef X9_SPEAKER_HAL_H
#define X9_SPEAKER_HAL_H
#include <stdint.h>
#define X9_SPEAKER_HAL_FILES 4u
#define X9_SPEAKER_HAL_MAX_BYTES (512u * 1024u)
#define X9_SPEAKER_HAL_SOURCE_CAP 128u
#define X9_SPEAKER_HAL_POINT "/System/Library/Audio/Plug-Ins/HAL"
#define X9_SPEAKER_HAL_PREFIX "/private/var/x9/speaker/"
struct x9_speaker_hal_file {
    const char *name;
    uint32_t bytes;
    const uint8_t *data;
    uint8_t sha[32];
};
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
extern const char x9_speaker_hal_source[X9_SPEAKER_HAL_SOURCE_CAP];
extern const struct x9_speaker_hal_file x9_speaker_hal_files[X9_SPEAKER_HAL_FILES];
#ifdef __cplusplus
}
#endif
#endif
#endif
