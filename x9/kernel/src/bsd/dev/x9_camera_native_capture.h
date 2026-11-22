/* Native source -> owned normal SMMU -> CAMSV request composition. */
#ifndef X9_CAMERA_NATIVE_CAPTURE_H
#define X9_CAMERA_NATIVE_CAPTURE_H
#ifdef KERNEL
#include "x9_camera_native_sensor.h"
/* Getter only: it does not probe, publish READY or register the engine.
 * Admit checks actual EL1/16KiB, this boot's FDT and absence of owners.
 * Powered source/rate/BW/SMMU capabilities are proved only in prepare.
 * READY never-issued STOP has a separate CANCELLED/RAW_CANCELLED scope;
 * registration still requires actual sensor ID and whole physical close.
 * No callback alone grants DMA or claims a physical preview. */
const struct x9_camera_native_engine *x9_camera_native_capture_engine(void);
#endif
#endif
