/* In-TU native sensor configuration, not a DMA/rate ownership grant. */
#ifndef X9_CAMERA_NATIVE_SOURCE_H
#define X9_CAMERA_NATIVE_SOURCE_H
#ifdef KERNEL
#include <stdint.h>
enum x9_cam_native_source_stage {
    X9_CAM_NATIVE_SOURCE_NONE,
    X9_CAM_NATIVE_SOURCE_RECEIVER,
    X9_CAM_NATIVE_SOURCE_SMI,
    X9_CAM_NATIVE_SOURCE_MAC,
    X9_CAM_NATIVE_SOURCE_ANALOG,
    X9_CAM_NATIVE_SOURCE_TIMING,
    X9_CAM_NATIVE_SOURCE_RATE,
    X9_CAM_NATIVE_SOURCE_CTLE,
    X9_CAM_NATIVE_SOURCE_OUTMUX,
    X9_CAM_NATIVE_SOURCE_CONFIGURED
};
struct x9_camera_native_source_status {
    uint64_t sensor_epoch, csi_clock_hz, nominal_data_rate;
    uint32_t sensor, stage, last_errno, completed;
    uint32_t receiver_flags, smi_flags, smi_a0_flags, smi_shared_flags;
    uint32_t mac_flags, phy_flags, outmux_flags;
    uint32_t csi_parent, clock_count[2], inherited_step, required_vcore_uv;
    uint32_t pll_rate, pll_fenc, vcore[3];
};
/* Included after x9_camera_receiver.inc. The native platform's config
 * callback already holds camera_control_lock and must call the _locked
 * helper, not the standalone session functions (which acquire that lock).
 * This runs actual MM/SMI/MAC/PHY/OUTMUX preparation after the sensor's
 * verified preview configuration, before core sets capture/stream attempted.
 * There is one attempt per cold sensor epoch. No automatic unwind or retry;
 * native power(false) owns the original sensor-close journal.
 * CONFIGURED means inactive register consumption only. The two real CSI
 * counts, native-only cold parent9, unchanged PLL PCW/FENC and the held
 * UFS monotonic voltage floor qualify the real CSI step0 rate. VCORE is
 * the real controller's request/gear, not a PMIC measurement. No new PLL,
 * voltage or bandwidth request is issued. The one SENINF parent restores
 * after unvote through the original MM close. Capture needs its real host
 * lease and CAMSV/SMI bandwidth/rate supplier.
 */
#endif
#endif
