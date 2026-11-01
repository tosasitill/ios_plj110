/* Native gauge -> IOPMPowerSource contract. All fields are one coherent sample.
 * Only the kernel gauge worker calls X9BatteryReport; callers serialize reports.
 * current_ma is positive into the battery; temperature is centidegrees Celsius.
 * status: 1 charging, 2 discharging, 3 not charging, 4 full (Linux power_supply).
 * Do not infer external power or full status from SOC/current alone.
 */
#ifndef X9_BATTERY_SAMPLE_H
#define X9_BATTERY_SAMPLE_H
#include <stdint.h>
struct X9BatterySample {
    uint32_t size, version;
    int32_t percent, voltage_mv, current_ma, temperature_centi;
    int32_t present, external, charge_capable, status;
    int32_t remaining_mah, full_mah, cycles;
};
#ifdef __cplusplus
extern "C" {
#endif
int X9BatteryReport(const struct X9BatterySample *, unsigned);
void X9BatteryInvalidate(void);
#ifdef __cplusplus
}
#endif
#endif
