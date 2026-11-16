/* SPDX-License-Identifier: MIT */
#ifndef X9_DISPLAY_GEOMETRY_H
#define X9_DISPLAY_GEOMETRY_H

/* Keep these equal to mkadt.py. UIKit remains 414x920 pt @3x. */
#define X9_PANEL_W 1256u
#define X9_PANEL_H 2760u
#define X9_CANVAS_W 1242u
#define X9_CANVAS_H 2760u

/* Pixel-centre inverse of a full-axis stretch. HID uses normalized 16.16
 * coordinates: backboardd applies the logical display dimensions afterwards.
 * No extra offset or second logical/panel ratio belongs in the HID event. */
static inline int32_t
x9_fill_normalized(int32_t pixel, uint32_t extent)
{
	if (!extent) return 0;
	if (pixel < 0) pixel = 0;
	if ((uint32_t)pixel >= extent) pixel = (int32_t)extent - 1;
	return (int32_t)((((uint64_t)(uint32_t)pixel * 2 + 1) << 15) / extent);
}

static inline void
x9_fill_columns(uint16_t columns[X9_PANEL_W])
{
	for (uint32_t x = 0; x < X9_PANEL_W; x++)
		columns[x] = (uint16_t)(((uint64_t)(2 * x + 1) * X9_CANVAS_W) / (2 * X9_PANEL_W));
}

/* Expand ONLY from a separate retained logical canvas into a physical slot.
 * Do not expand in place or ever seed the canvas from an expanded panel.
 * Nearest-neighbour, pixel-centred, leaves y unchanged (including the 20px
 * status bar offset). Padding is not fetched by the panel and is untouched. */
static inline void
x9_fill_expand(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
    uint32_t src_stride, const uint16_t columns[X9_PANEL_W])
{
	for (uint32_t y = 0; y < X9_PANEL_H; y++) {
		const uint8_t *s = src + (uint64_t)y * src_stride;
		uint8_t *d = dst + (uint64_t)y * dst_stride;
		for (uint32_t x = 0; x < X9_PANEL_W; x++) {
			uint32_t pixel;
			memcpy(&pixel, s + (uint64_t)columns[x] * 4, 4);
			memcpy(d + (uint64_t)x * 4, &pixel, 4);
		}
	}
}
#endif
