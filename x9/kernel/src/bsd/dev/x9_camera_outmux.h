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
#define X9_CAM_OUT_IRQ_ATTEMPTED 1u
#define X9_CAM_OUT_IRQ_MODE_READY 2u
#define X9_CAM_OUT_IRQ_BASELINE_CLEAR 4u
#define X9_CAM_OUT_IRQ_HISTORY_RETIRED 8u
#define X9_CAM_OUT_IRQ_MODE_RESTORED 16u
#define X9_CAM_OUT_IRQ_RETAINED 32u
struct x9_camera_outmux_irq_status {
    uint64_t sensor_epoch, command, completed;
    uint32_t flags, last_errno, sampled, raw_before, raw_after;
    uint32_t mode_owned, mode_pending, ack_pending, event_seen;
};
struct x9_camera_outmux_status {
    uint64_t sensor_epoch;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t dirty, uncertain, commit_pending, cg_owned, cg_uncertain;
    uint32_t image_vc, first_vc, last_vc, vcdt, expected_size;
    struct x9_camera_outmux_irq_status irq;
};
/* Standalone thread context, takes camera_control_lock. Requires the same
 * configured standby sensor's successful prepare_receiver. No arbitrary
 * route/tag/VC/register input. OUTMUX0 only, image tag0, normal mode, RAW10.
 * Actual masked shadow writes and CFG_DONE consumption, with DL/IRQ/DMA
 * kept off. The sole thread-only raw STATUSX sampler journals WCLR policy,
 * retires a qualified stale CFG_DONE baseline, then binds each fresh event
 * to our posted command AND exact inner target before a single-bit W1C.
 * Other/unknown events are not acknowledged. Neither this result nor an
 * idle bank grants normal SID80,
 * PHY/rate/SMI ownership, DMA memory or exclusive AP/SCP authority.
 * No ioctl/READY is added. Native callbacks already holding the mutex use
 * the internal locked helper. Sensor close is the ONLY retirement path;
 * a failed/pending commit keeps the route and MM/sensor dependencies.
 * Direct inner writes are restricted to WCLR policy setup/restoration,
 * never route/tag/DL or fake shadow consumption. Restoration follows
 * command/event retirement; no history is read after restoring policy.
 * RESTORED includes policy/CG restoration; HISTORY_RETIRED separately proves
 * the sampled masked CFG_DONE history (partial setup may abort before any
 * history read). Neither means GIC ownership, IRQ/worker/DMA drain, or a
 * capture completion. No AP or
 * CCU IRQ is enabled; future capture must acquire its actual GIC sources. */
int x9_camera_sensor_session_configure_outmux(uint64_t sensor_epoch,
    struct x9_camera_outmux_status *);
#endif
#endif
