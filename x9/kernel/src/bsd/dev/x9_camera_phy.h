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
#define X9_CAM_PHY_CTLE_ATTEMPTED 64u
#define X9_CAM_PHY_CTLE_CONFIGURED 128u
#define X9_CAM_PHY_TIMING_ATTEMPTED 256u
#define X9_CAM_PHY_TIMING_CONFIGURED 512u
#define X9_CAM_PHY_DIGITAL_CONFIGURED 1024u
struct x9_camera_phy_status {
    uint64_t sensor_epoch, nominal_data_rate;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t csi_port, cphy, stage, bank;
    uint32_t dirty_count, pending_reg, calibration_pending, calibration_done;
    uint32_t completion[2], ref_ck[2];
    uint32_t cdr_delay, eq_control;
    uint64_t csi_clock_hz;
    uint32_t clock_count[2], clock_pending, clock_busy_seen;
    uint32_t clock_dirty_mask, clock_pending_field;
    uint32_t data_settle, clock_settle, hs_trail;
    uint32_t deskew_reset_pending, deskew_reset_released, lane_select;
};
/* Thread context; takes camera_control_lock. After configured standby,
 * prepare_receiver AND configure_mac, BEFORE configure_outmux. Programs
 * the two real analog banks and nominal preview band parameters; admits
 * only inactive digital lanes and unpowered analog, no inherited force/cal.
 * Uses this boot's LK devinfo, never an arbitrary calibration word argument.
 * OS-cal completion must be observed freshly; no completion-bit writes.
 * Failures pin the journal to the sole sensor close; an unconsumed trigger
 * has no cancellation proof and blocks all receiver/MM/supply teardown.
 * configure_timing separately programs digital HS timing/CPHY POST from
 * measured CSI clock counts, then applies fixed inactive digital settings.
 * No PHY lane enable, AFIFO flush, packet or IRQ/DMA ownership is granted.
 * configure_ctle applies the sensor override separately, after timing/generic
 * PHY configuration and before OUTMUX. CALIBRATED is not measured rate, packet success,
 * SMI/SID80 ownership or capture READY. Not yet called by native capture. */
int x9_camera_sensor_session_prepare_analog(uint64_t,struct x9_camera_phy_status *);
/* Same lifetime/status, standalone thread context. Requires successful
 * prepare_analog, no OUTMUX, configured sensor still in standby. Front's
 * static EQ override and main preview's EQ/CDR9 override are fixed profiles,
 * not user-supplied tuning. configure_timing and future generic PHY configuration must
 * precede this final override. No lane enable, APPLY pulse, IRQ or DMA.
 * Errors after CTLE_ATTEMPTED pin this journal to the sole sensor close.
 * Close restores delay-force controls BEFORE analog power-off, using only
 * exact confirmed controls and no replay of read-only DA/AD results. */
int x9_camera_sensor_session_configure_ctle(uint64_t,struct x9_camera_phy_status *);
/* Inactive digital HS timing, lane-map/normal protocol and deskew configuration, after analog
 * calibration, BEFORE final CTLE and OUTMUX. Measures the current selected
 * SENINF clock twice (CKGEN_CK2 ID18/21) with a fresh BUSY cycle and stable
 * count. 26MHz reference is a vendor hardware contract, not measured ppm.
 * Front's >1500Mbps deskew profile enables the generic engine but keeps
 * lane DELAY_EN/PERIODIC/APPLY and all IRQs off (sensor support flags0).
 * Its dedicated inactive reset is a confirmed assertion/deassertion only,
 * never a calibration result or IRQ-history drain. An uncertain reset
 * retains dependencies. Both preview profiles leave ULPS/ALP/LRTE off.
 * No rate/PLL change, nominal fallback, PHY lane enable, AFIFO flush or DMA.
 * A posted unconsumed meter/reset request blocks the sole close's teardown. */
int x9_camera_sensor_session_configure_timing(uint64_t,struct x9_camera_phy_status *);
#endif
#endif
