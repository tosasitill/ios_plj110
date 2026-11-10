/* Kernel-private MT6993 OUTMUX0 configuration result, not a DMA lease. */
#ifndef X9_CAMERA_OUTMUX_H
#define X9_CAMERA_OUTMUX_H
#ifdef KERNEL
#include <stdint.h>
#define X9_CAM_OUT_ATTEMPTED 1u
#define X9_CAM_OUT_SHADOW 2u
#define X9_CAM_OUT_CONSUMED 4u
#define X9_CAM_OUT_RESTORED 8u
#define X9_CAM_OUT_RETAINED 16u
struct x9_camera_outmux_status {
    uint64_t sensor_epoch;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t dirty, uncertain, commit_pending, cg_owned, cg_uncertain;
    uint32_t image_vc, first_vc, last_vc, vcdt, expected_size;
};
/* Standalone thread context, takes camera_control_lock. Requires the same
 * configured standby sensor's successful prepare_receiver. No arbitrary
 * route/tag/VC/register input. OUTMUX0 only, image tag0, normal mode, RAW10.
 * Actual masked shadow writes and CFG_DONE consumption, with DL/IRQ/DMA
 * kept off. Neither a stable idle bank nor this result grants normal SID80,
 * PHY/rate/SMI ownership, DMA memory or exclusive AP/SCP authority.
 * No ioctl/READY is added. Native callbacks already holding the mutex use
 * the internal locked helper. Sensor close is the ONLY retirement path;
 * a failed/pending commit keeps the route and MM/sensor dependencies.
 * RESTORED describes config/CG, not IRQ-status retirement. CFG_DONE may
 * latch a masked event; this service does not read/ack IRQ_STATUS. A future
 * sole native IRQ owner must qualify/retire that history before unmasking. */
int x9_camera_sensor_session_configure_outmux(uint64_t sensor_epoch,
    struct x9_camera_outmux_status *);
#endif
#endif
