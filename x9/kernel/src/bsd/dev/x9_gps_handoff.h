/* LK metadata only. None of these fields grants power, firmware or EMI ownership.
 * MT6993 uses CONNSYS_EMI_FW/soc721a, not gps@.../emi-addr (legacy). */
#ifndef X9_GPS_HANDOFF_H
#define X9_GPS_HANDOFF_H
#include <stdint.h>
#define X9_GPS_HANDOFF_BYTES 64u
#define X9_GPS_LAYOUT_VERIFIED 1u
/* Optional topology proof only: SID3 LNC0_8 / PMIF-P / CONN parent.
 * Does not prove regulator, clock, secure EMI or DMA ownership. */
#define X9_GPS_POWER_LAYOUT_VERIFIED 2u
/* Optional native D-die register topology, still NOT a power/DMA lease. */
#define X9_GPS_POS_LAYOUT_VERIFIED 4u
/* Optional WT_SLP/AFE topology only, NOT RF/clock/exclusive ownership. */
#define X9_GPS_RF_LAYOUT_VERIFIED 8u
/* Optional native low-power/RC bank topology, never a live power lease. */
#define X9_GPS_LP_LAYOUT_VERIFIED 16u
struct x9_gps_handoff {
 uint32_t version, flags, reserve_base, reserve_size;
 uint32_t layout_size, legacy_offset, legacy_size, ap2mcu_offset;
 uint32_t mcu2ap_offset, region_size, data_offset, data_size;
 uint32_t mcu_bus_base, ccif_base, ccif_size, reserved;
};
#endif
