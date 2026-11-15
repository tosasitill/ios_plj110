/* Native source -> owned normal SMMU -> CAMSV request composition. */
#ifndef X9_CAMERA_NATIVE_CAPTURE_H
#define X9_CAMERA_NATIVE_CAPTURE_H
#ifdef KERNEL
#include "x9_camera_native_sensor.h"
/* Getter only: it does not probe, publish READY or register the engine.
 * Admission remains closed until continuous credit/AE and full producer,
 * translation and supplier retirement are implemented together. */
const struct x9_camera_native_engine *x9_camera_native_capture_engine(void);
#endif
#endif
