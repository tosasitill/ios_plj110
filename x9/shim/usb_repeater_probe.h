#pragma once
#include <stdint.h>
/* Fixed X9 PMIF-P / SWINF2 / SID14 peripheral configuration reads only.
 * No API for PMIC register writes, arbitrary SIDs, or channel recovery. */
int x9_usb_repeater_read(void *, uint16_t, uint8_t *, uint32_t *,
                         uint32_t (*)(void *, uintptr_t),
                         void (*)(void *, uintptr_t, uint32_t),
                         void (*)(void *, unsigned));
