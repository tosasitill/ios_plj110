/* Kernel-private, inactive ISP8s CAMSV0 configuration; never a DMA lease. */
#ifndef X9_CAMERA_SV_HW_H
#define X9_CAMERA_SV_HW_H
#ifdef KERNEL
#include <stdint.h>
#define X9_CAM_SV_HW_ATTEMPTED 1u
#define X9_CAM_SV_HW_SNAPSHOT 2u
#define X9_CAM_SV_HW_CONFIGURED 4u
#define X9_CAM_SV_HW_RESTORED 8u
#define X9_CAM_SV_HW_RETAINED 16u
#define X9_CAM_SV_HW_REQUEST 32u
#define X9_CAM_SV_HW_CONFIG_FIELDS 10u
/* Geometry fields0..9 followed by A0 base low/high and tag0 FH_SPARE.
 * This result is kernel-private, not the userspace frame/ioctl ABI. */
#define X9_CAM_SV_HW_FIELDS 13u
struct x9_camera_sv_hw_status {
    uint64_t sensor_epoch, command, completed;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t dirty, pending, commit_pending, db_phase, db_issued, terminal;
    uint32_t sampled, db_before[2], db_after[2];
    uint32_t before[X9_CAM_SV_HW_FIELDS], target[X9_CAM_SV_HW_FIELDS];
    uint32_t after[2][X9_CAM_SV_HW_FIELDS];
    uint64_t request_generation, request_capture_id;
    uint32_t request_sequence, request_errno;
};
/* configure_outmux invokes the real locked engine after OUTMUX consumption.
 * It writes only tag0 packed RAW10 geometry, A0 stride/burst, group0 and
 * first/last tag, then follows the upstream DB_EN down/up sequence. Only
 * sampled complete outer/inner targets prove configuration consumption;
 * no inner data writes, force-load, FBC, address, IRQ, CQ or DMA enable.
 * The separate normal IRQ begin path, AFTER an actual S1 lease/RAW arm,
 * journals only A0 base low/high and the frame's tag0 FH_SPARE. It checks
 * the same request/lease before every store and consumes the complete
 * outer/inner target BEFORE GIC publication. REQUEST means inactive
 * address/metadata consumption, not a DMA/source/FBC enable or frame.
 * Sensor close retires this journal BEFORE OUTMUX/PHY/MAC/SMI/MM. Pending
 * writes require their actual targets, not cancellation by a prior value.
 * This state is neither GIC/IRQ history nor producer/DMA/cache/worker drain.
 * No new standalone entrypoint, ioctl, READY or ownership claim. */
#endif
#endif
