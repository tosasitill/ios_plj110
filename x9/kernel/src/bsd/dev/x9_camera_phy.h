/* Kernel-private inactive analog lifetime. Never a PHY/lane/DMA lease. */
#ifndef X9_CAMERA_PHY_H
#define X9_CAMERA_PHY_H
#ifdef KERNEL
#include <stdint.h>
#define X9_CAM_PHY_ATTEMPTED 1u
#define X9_CAM_PHY_CALIBRATED 2u
#define X9_CAM_PHY_RESTORED 4u
#define X9_CAM_PHY_RETAINED 8u
#define X9_CAM_PHY_LK_EFUSE 16u
#define X9_CAM_PHY_DEFAULT_TERM 32u
struct x9_camera_phy_status {
    uint64_t sensor_epoch, nominal_data_rate;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t csi_port, cphy, stage, bank;
    uint32_t dirty_count, pending_reg, calibration_pending, calibration_done;
    uint32_t completion[2], ref_ck[2];
};
/* Thread context; takes camera_control_lock. After configured standby,
 * prepare_receiver AND configure_mac, BEFORE configure_outmux. Programs
 * the two real analog banks and nominal preview band parameters; admits
 * only inactive digital lanes and unpowered analog, no inherited force/cal.
 * Uses this boot's LK devinfo, never an arbitrary calibration word argument.
 * OS-cal completion must be observed freshly; no completion-bit writes.
 * Failures pin the journal to the sole sensor close; an unconsumed trigger
 * has no cancellation proof and blocks all receiver/MM/supply teardown.
 * Digital settle/trail/deskew/ULPS/lane enables and sensor CTLE overrides
 * are still separate work. CALIBRATED is not measured rate, packet success,
 * SMI/SID80 ownership or capture READY. Not yet called by native capture. */
int x9_camera_sensor_session_prepare_analog(uint64_t,struct x9_camera_phy_status *);
#endif
#endif
