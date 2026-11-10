/* Kernel-private MT6993 inactive packet-link configuration, not capture. */
#ifndef X9_CAMERA_MAC_H
#define X9_CAMERA_MAC_H
#ifdef KERNEL
#include <stdint.h>
#define X9_CAM_MAC_ATTEMPTED 1u
#define X9_CAM_MAC_CONFIGURED 2u
#define X9_CAM_MAC_RESTORED 4u
#define X9_CAM_MAC_RETAINED 8u
struct x9_camera_mac_status {
    uint64_t sensor_epoch;
    uint32_t sensor, flags, last_errno, restore_errno;
    uint32_t dirty, uncertain, cg_owned, cg_uncertain;
    uint32_t csi_port, cphy, packet_option, header_mode, async_split, afifo_split;
};
/* Thread context; takes camera_control_lock. After prepare_receiver and
 * BEFORE optional configure_outmux. Fixed front DPHY4 / main CPHY3 packet
 * format, RAW10 DT identity, unsplit async/AFIFO. No arbitrary registers,
 * lane enable, analog calibration, rate claim, IRQ policy or DMA lease.
 * Admitted failures retain a journal; sensor close is the sole, one-shot
 * restoration path. Native same-TU callbacks use the internal locked helper.
 * This API has no diagnostic ioctl and is not yet called by native capture. */
int x9_camera_sensor_session_configure_mac(uint64_t sensor_epoch,
    struct x9_camera_mac_status *);
#endif
#endif
