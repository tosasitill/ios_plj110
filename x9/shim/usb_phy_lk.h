#pragma once
#include <stdint.h>
/* Callbacks keep the exact stock LK recovery sequence host-testable. */
void x9_usb_phy_recover(void *, uint32_t (*)(void *, uintptr_t),
                        void (*)(void *, uintptr_t, uint32_t),
                        void (*)(void *, unsigned));
/* Captured X9 XS-PHY USB2 core power-on; refclk-sel=true, hwpll-mode=false
 * as in the captured DT. Preserve external repeater and calibration settings. */
void x9_usb_phy_power_on(void *, uint32_t (*)(void *, uintptr_t),
                         void (*)(void *, uintptr_t, uint32_t),
                         void (*)(void *, unsigned));
/* Reset only the UDC device module after PHY recovery, as stock LK does. */
void x9_usb_device_reset(void *, uint32_t (*)(void *, uintptr_t),
                          void (*)(void *, uintptr_t, uint32_t),
                          void (*)(void *, unsigned));
