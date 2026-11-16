/* SPDX-License-Identifier: MIT */
#ifndef X9_CAMERA_PROFILE_H
#define X9_CAMERA_PROFILE_H
#include <stdbool.h>
#include <stdint.h>

/* Registration is not an encoding/recording result. No compression probe is run. */
bool x9_camera_software_hevc_ready(void);
bool x9_camera_video_profile_ready(void);
void x9_camera_install_profile(uintptr_t slide);
#endif
