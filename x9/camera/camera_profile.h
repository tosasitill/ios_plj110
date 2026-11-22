/* SPDX-License-Identifier: MIT */
#ifndef X9_CAMERA_PROFILE_H
#define X9_CAMERA_PROFILE_H
#include <stdbool.h>
#include <stdint.h>

/* Every selectable lens can realize this range within its actual 16-bit
 * frame-length limit. Wide/front cannot implement a fixed 2fps mode. */
#define X9_CAMERA_MIN_FRAME_RATE 4
#define X9_CAMERA_MAX_FRAME_RATE 30

/* Registration is not an encoding/recording result. No compression probe is run. */
bool x9_camera_software_hevc_ready(void);
bool x9_camera_video_profile_ready(void);
void x9_camera_install_profile(uintptr_t slide);
#endif
