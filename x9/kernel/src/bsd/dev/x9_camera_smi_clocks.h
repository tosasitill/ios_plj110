/* Kernel-private CAMSV0 clock prerequisites, not an SMI DMA grant. */
#ifndef X9_CAMERA_SMI_CLOCKS_H
#define X9_CAMERA_SMI_CLOCKS_H
#ifdef KERNEL
#include <stdint.h>
#define X9_CAM_SMI_CLOCKS_ATTEMPTED 1u
#define X9_CAM_SMI_CLOCKS_CONFIGURED 2u
#define X9_CAM_SMI_CLOCKS_RESTORED 4u
#define X9_CAM_SMI_CLOCKS_RETAINED 8u
#define X9_CAM_SMI_A0_ATTEMPTED 1u
#define X9_CAM_SMI_A0_SNAPSHOT 2u
#define X9_CAM_SMI_A0_SHARED_PROFILE_MATCHED 4u
#define X9_CAM_SMI_A0_CONFIGURED 8u
#define X9_CAM_SMI_A0_RESTORED 16u
#define X9_CAM_SMI_A0_RETAINED 32u
#define X9_CAM_SMI_A0_LARB_WORDS 15u
#define X9_CAM_SMI_A0_COMMON_WORDS 12u
struct x9_camera_smi_a0_status {
    uint32_t flags, stage, last_errno, restore_errno;
    uint32_t owned, pending, pending_word;
    uint32_t larb_before[X9_CAM_SMI_A0_LARB_WORDS];
    uint32_t larb_after[X9_CAM_SMI_A0_LARB_WORDS];
    uint32_t common_before[X9_CAM_SMI_A0_COMMON_WORDS];
    uint32_t common_after[X9_CAM_SMI_A0_COMMON_WORDS];
    uint32_t outstanding[2];
};
struct x9_camera_smi_clock_status {
    uint64_t sensor_epoch;
    uint32_t sensor, flags, last_errno, restore_errno, stage, pending;
    uint32_t parent_cfg, seen, completed;
    uint32_t mask[2], owned[2], borrowed[2], gate_before[2];
    uint32_t ap[2], global[2], gate[2];
    uint32_t larb_busy[3][2];
    struct x9_camera_smi_a0_status a0;
};
/* Thread context, camera_control_lock. After configured sensor standby and
 * prepare_receiver, BEFORE MAC/PHY/OUTMUX; no new diagnostic ioctl/caller.
 * Takes actual CAM_VCORE_R1A subcomm1/2/3 and CAM_MRAW larb19/14/29 gates
 * through MM HWV, retaining the existing MM/CSI/secure power references.
 * Unique physical gates are deduplicated across supplier and consumer.
 * After clocks, qualifies the shared subcomm3/CAMSV A0 misc profile and
 * journals only L19/P0's default OSTD and initial SRT permission/throttle.
 * Common and larb-wide misc must already match; they are NOT overwritten.
 * CQ, other image/STG ports, routing/MMU/SID and common BW remain untouched.
 * A0_CONFIGURED is the inactive port default, NOT complete SMI, a bandwidth
 * lease, SID80 attach, PHY/IRQ/DMA enable or READY. CLOCKS_CONFIGURED remains
 * clock readback only, including when the later A0 preparation fails.
 * Admitted errors pin the journal to the sole sensor close; an unconsumed
 * request or newly active owner keeps all remaining MM/sensor dependencies.
 * Native capture has not yet called this entrypoint. */
int x9_camera_sensor_session_prepare_smi_clocks(uint64_t,
    struct x9_camera_smi_clock_status *);
#endif
#endif
