/* Only calibration provenance. No MMIO address, DMA or ownership grant. */
#ifndef X9_CAMERA_CALIBRATION_H
#define X9_CAMERA_CALIBRATION_H
#include <stdint.h>
#define X9_CAM_CALIBRATION_BYTES 32u
#define X9_CAM_CALIBRATION_LK_DATA 1u
struct x9_camera_calibration {
    uint32_t version, flags, word_count, port_mask;
    uint32_t front, main, reserved[2];
};
#endif
