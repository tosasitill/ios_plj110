/* SPDX-License-Identifier: MIT */
#ifndef X9_DISPLAY_CADENCE_H
#define X9_DISPLAY_CADENCE_H

#include <stdint.h>

/* Software notification/LK-submit rates only. These do not describe panel
 * timing or prove that a requested DSI mode was applied. */
static inline bool
x9_display_cadence_supported(uint32_t hz)
{
	return hz == 60 || hz == 120;
}

/* An explicit unsupported rate is an error, not an implicit 60 Hz request.
 * Automatic selection retains the existing measured-TE policy: only a period
 * below 9100 us selects 120 Hz; every other result selects 60 Hz. */
static inline uint32_t
x9_display_cadence_select(uint32_t configured_hz, bool explicit_hz,
    uint32_t te_period_us)
{
	if (explicit_hz) {
		return x9_display_cadence_supported(configured_hz) ? configured_hz : 0;
	}
	return te_period_us != 0 && te_period_us < 9100 ? 120 : 60;
}

static inline uint64_t
x9_display_cadence_period_ns(uint32_t hz)
{
	return (1000000000ULL + hz / 2) / hz;
}

#endif
