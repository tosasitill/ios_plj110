/* Native PLJ110 power sublease, kernel only. This is NOT the complete
 * connv2 lease: secure EMI/MPU, cold BGF DMA/SYSRAM and ROM/MNL still need
 * their own qualification before registering the physical supplier. */
#ifndef X9_GPS_NATIVE_POWER_H
#define X9_GPS_NATIVE_POWER_H
#include "x9_gps_handoff.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Sole physical worker only. clock is the measured vendor schematic enum,
 * never a guessed frequency. Partial failure retains its nonzero token
 * and every possibly accepted write. No release/retry API is provided. */
int x9_gps_native_power_acquire(const struct x9_gps_handoff *, uint64_t,
    unsigned clock, uint64_t *);
int x9_gps_native_power_validate(const struct x9_gps_handoff *, uint64_t,
    uint64_t);
#ifdef __cplusplus
}
#endif
#endif
