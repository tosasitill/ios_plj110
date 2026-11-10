/* v062 MCUSYS receive schema, not an application ACK or an NV lease.
 * Actual gps_drv_dl_v062.ko33628 dispatches types0/1/2/5 from NORMAL a0.
 * Header is LE32 type, LE16 data_size, two padding bytes; union starts+8.
 * Host sender33778 emits16B. Decoder also accepts a shortened union when
 * all declared data is present; host TX size alone is not MCU RX proof. */
#ifndef X9_GPS_MCUSYS_H
#define X9_GPS_MCUSYS_H
#include <stdint.h>
#define X9_GPS_MCUSYS_PACKET 0xa0u
#define X9_GPS_MCUSYS_FRAME_BYTES 16u
#define X9_GPS_MCUSYS_NV_IDS 21u
enum x9_gps_mcusys_type {
 X9_GPS_MCUSYS_MNLBIN=0, X9_GPS_MCUSYS_NVLOCK=1,
 X9_GPS_MCUSYS_NVDATA=2, X9_GPS_MCUSYS_HOST_STATUS=3,
 X9_GPS_MCUSYS_HOST_LPPMODE=4, X9_GPS_MCUSYS_SCIF=5
};
enum x9_gps_mcusys_mnl_event {
 X9_GPS_MCUSYS_SYS_ON=0, X9_GPS_MCUSYS_INIT_DONE=1,
 X9_GPS_MCUSYS_CTLR_CREATED=2, X9_GPS_MCUSYS_RESET_START=3,
 X9_GPS_MCUSYS_RESET_END=4
};
enum x9_gps_mcusys_mnl_state {
 X9_GPS_MCUSYS_UNINIT=0, X9_GPS_MCUSYS_INITIALIZED=1,
 X9_GPS_MCUSYS_CONTROLLER=2, X9_GPS_MCUSYS_RESETTING=3
};
struct x9_gps_mcusys_message {
 uint32_t type;
 uint32_t id; /* NV ID; UINT32_MAX for MNLBIN/SCIF */
 uint32_t event; /* MNLBIN/NV event, or the real SCIF tick */
};
#ifdef __cplusplus
extern "C" {
#endif
/* No MMIO or state effects. Padding is ignored, never an identity/ACK.
 * AP-only commands, unknown types/events/IDs and inconsistent data sizes
 * fail without changing output. No partial union is zero-filled into an
 * invented SYS_ON/NV event, unlike the vendor's unchecked memcpy path. */
int x9_gps_mcusys_decode(const void *, unsigned,
    struct x9_gps_mcusys_message *);
#ifdef __cplusplus
}
#endif
#endif
