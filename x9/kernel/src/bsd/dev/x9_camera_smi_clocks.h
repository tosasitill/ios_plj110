/* Kernel-private CAMSV0 clock prerequisites, not an SMI DMA grant. */
#ifndef X9_CAMERA_SMI_CLOCKS_H
#define X9_CAMERA_SMI_CLOCKS_H
#ifdef KERNEL
#include <stdint.h>
#define X9_CAM_SMI_CLOCKS_ATTEMPTED 1u
#define X9_CAM_SMI_CLOCKS_CONFIGURED 2u
#define X9_CAM_SMI_CLOCKS_RESTORED 4u
#define X9_CAM_SMI_CLOCKS_RETAINED 8u
struct x9_camera_smi_clock_status {
    uint64_t sensor_epoch;
    uint32_t sensor, flags, last_errno, restore_errno, stage, pending;
    uint32_t parent_cfg, seen, completed;
    uint32_t mask[2], owned[2], borrowed[2], gate_before[2];
    uint32_t ap[2], global[2], gate[2];
    uint32_t larb_busy[3][2];
};
/* Thread context, camera_control_lock. After configured sensor standby and
 * prepare_receiver, BEFORE MAC/PHY/OUTMUX; no new diagnostic ioctl/caller.
 * Takes actual CAM_VCORE_R1A subcomm1/2/3 and CAM_MRAW larb19/14/29 gates
 * through MM HWV, retaining the existing MM/CSI/secure power references.
 * Unique physical gates are deduplicated across supplier and consumer.
 * Does NOT configure common/larb routing, QoS, outstanding limits, SID80,
 * PHY lanes, IRQs or DMA. CONFIGURED is clock readback only, not READY.
 * Admitted errors pin the journal to the sole sensor close; an unconsumed
 * request or newly active owner keeps all remaining MM/sensor dependencies.
 * Native capture has not yet called this entrypoint. */
int x9_camera_sensor_session_prepare_smi_clocks(uint64_t,
    struct x9_camera_smi_clock_status *);
#endif
#endif
