/* Kernel-private ISP8s CAMSV0 configuration/receive-arm journal; never a DMA lease. */
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
#define X9_CAM_SV_HW_FBC_EMPTY 64u
#define X9_CAM_SV_HW_FBC_CREDIT 128u
#define X9_CAM_SV_HW_RECEIVE_ARMED 256u
#define X9_CAM_SV_HW_RECEIVE_QUIESCENT 512u
#define X9_CAM_SV_CREDIT_EMPTY 0u
#define X9_CAM_SV_CREDIT_PENDING 1u
#define X9_CAM_SV_CREDIT_ACCEPTED 2u
#define X9_CAM_SV_RECEIVE_IDLE 0u
#define X9_CAM_SV_RECEIVE_PENDING 1u
#define X9_CAM_SV_RECEIVE_ARMED 2u
#define X9_CAM_SV_RECEIVE_QUIESCING 3u
#define X9_CAM_SV_RECEIVE_QUIESCENT 4u
#define X9_CAM_SV_RECEIVE_FIELDS 4u
#define X9_CAM_SV_HW_CONFIG_FIELDS 10u
/* Geometry fields0..9 followed by A0 base low/high, tag0 FH_SPARE and FBC0.
 * This result is kernel-private, not the userspace frame/ioctl ABI. */
#define X9_CAM_SV_HW_FIELDS 14u
struct x9_camera_sv_hw_status {
    uint64_t sensor_epoch, command, completed;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t dirty, pending, commit_pending, db_phase, db_issued, terminal;
    uint32_t sampled, db_before[2], db_after[2];
    uint32_t before[X9_CAM_SV_HW_FIELDS], target[X9_CAM_SV_HW_FIELDS];
    uint32_t after[2][X9_CAM_SV_HW_FIELDS];
    uint64_t request_generation, request_capture_id;
    uint32_t request_sequence, request_errno;
    uint32_t fbc_sampled, fbc_before[2], fbc_after[2], fbc_errno;
    uint32_t credit_phase, credit_issued, credit_seen, credit_ack, credit_samples, credit_errno;
    uint32_t credit_control[2], credit_target[2];
    uint32_t receive_phase, receive_issued, receive_off_issued;
    uint32_t receive_seen[2], receive_off_seen[2], receive_samples, receive_errno;
    uint32_t receive_before[X9_CAM_SV_RECEIVE_FIELDS], receive_target[X9_CAM_SV_RECEIVE_FIELDS];
    uint32_t receive_after[2][X9_CAM_SV_RECEIVE_FIELDS];
};
/* configure_outmux invokes the real locked engine after OUTMUX consumption.
 * It writes only tag0 packed RAW10 geometry, A0 stride/burst, group0 and
 * first/last tag, then follows the upstream DB_EN down/up sequence. Only
 * sampled complete outer/inner targets prove configuration consumption;
 * no inner data writes, force-load, FBC, address, IRQ, CQ or DMA enable.
 * The separate normal IRQ begin path, AFTER an actual S1 lease/RAW arm,
 * journals A0 base low/high and the frame's tag0 FH_SPARE, then initializes
 * tag0 FBC_EN without RCNT, reset, lock, subsampling or valid/sub ratio.
 * Before the single RCNT command, empty FBC1 snapshots remain EXACTLY unchanged;
 * EMPTY means no observed credit mutation, not a hardware/AXI drain grant.
 * Other tags and all source/DMA/CMOS/VF/CQ enables are off at this stage. It checks
 * the same request/lease before every store and consumes the complete
 * outer/inner target BEFORE GIC publication. REQUEST means inactive
 * address/metadata consumption, not a DMA/source/credit enable or frame.
 * AFTER all three GIC lines are actually published, begin emits exactly one
 * outer RCNT command for that immutable S1 request. It accepts credit only
 * after two complete both-bank observations of RCNT+1(mod256), CNT=1,
 * unchanged WCNT/DROP/reserved, and original EN control with RCNT cleared.
 * No DB pulse/no-op/retry can replay that command; PENDING timeout is terminal.
 * CREDIT means this observed command acceptance, not frame completion or
 * hardware's single-frame write/drain guarantee. The credit helper itself
 * leaves source/DMA off. Begin next journals four normal RW receive enables:
 * SW_DONE group0, SRAM/tag0 ERR, tag0 SOF and IMG0A DMA, with complete actual
 * both-bank targets twice for each single store, no DB/RCNT/reset/CQ write.
 * CMOS/VF/PHY/MAC lanes/OUTMUX DL/sensor source remain off; FBC1 remains at
 * the accepted credit target. RECEIVE_ARMED is not a started producer/frame.
 * Close can quiesce this same inactive receive arm only, before GIC masking.
 * Per-bank target observations never regress; raw receiver observations are
 * journaled before historical comparisons, not adopted by canonicalization.
 * QUIESCENT describes four actual disabled controls only, not RAW/AXI/map
 * drain, prior-counter restoration, credit cancellation or dependency release.
 * An issued credit prohibits rearm/config restore until the actual producer
 * retirement chain is implemented; no caller-provided consumed boolean.
 * Sensor close retires this journal BEFORE OUTMUX/PHY/MAC/SMI/MM. Pending
 * writes require their actual targets, not cancellation by a prior value.
 * This state is neither GIC/IRQ history nor producer/DMA/cache/worker drain.
 * No new standalone entrypoint, ioctl, READY or ownership claim. */
#endif
#endif
