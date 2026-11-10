/* LK metadata only. None of these fields grants power, firmware or EMI ownership.
 * MT6993 uses CONNSYS_EMI_FW/soc721a, not gps@.../emi-addr (legacy). */
#ifndef X9_GPS_HANDOFF_H
#define X9_GPS_HANDOFF_H
#include <stdint.h>
#define X9_GPS_HANDOFF_BYTES 64u
#define X9_GPS_LAYOUT_VERIFIED 1u
struct x9_gps_handoff {
 uint32_t version, flags, reserve_base, reserve_size;
 uint32_t layout_size, legacy_offset, legacy_size, ap2mcu_offset;
 uint32_t mcu2ap_offset, region_size, data_offset, data_size;
 uint32_t mcu_bus_base, ccif_base, ccif_size, reserved;
};
#endif
