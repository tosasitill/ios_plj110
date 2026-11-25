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

/* Copy one horizontally contiguous run of pixels. XNU has no NEON
 * intrinsics in C, so use constant-size memcpy: clang lowers 32-byte and
 * 16-byte copies to paired ldp/stp of q registers, which is what makes this
 * faster than one 4-byte copy per output pixel. */
static inline void
x9_fill_copy_run(uint8_t *dst, const uint8_t *src, uint32_t pixels)
{
	while (pixels >= 8) {
		memcpy(dst, src, 32);
		src += 32; dst += 32; pixels -= 8;
	}
	while (pixels >= 4) {
		memcpy(dst, src, 16);
		src += 16; dst += 16; pixels -= 4;
	}
	while (pixels--) {
		uint32_t pixel;
		memcpy(&pixel, src, 4);
		memcpy(dst, &pixel, 4);
		src += 4; dst += 4;
	}
}

/* Expand ONLY from a separate retained logical canvas into a physical slot.
 * Do not expand in place or ever seed the canvas from an expanded panel.
 * Nearest-neighbour, pixel-centred, leaves y unchanged (including the 20px
 * status bar offset). Padding is not fetched by the panel and is untouched.
 *
 * x9_fill_columns() is monotone with per-step increments of 0 or 1, so a row
 * is 15 contiguous copy runs (average 84 px) separated by 14 duplicated
 * columns. Scan the runs once per call (never per row) and copy each run
 * with fixed-size movers; host benchmark of the 1256x2760 frame:
 * 1.24-2.00 ms per-pixel vs 0.93-1.12 ms run copy. A columns[] map with more
 * than 64 runs (step of 2 or a step back) keeps the pixel-exact path. */
struct x9_fill_run {
	uint16_t dst_x;
	uint16_t src_x;
	uint16_t len;
};

static inline void
x9_fill_expand(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
    uint32_t src_stride, const uint16_t columns[X9_PANEL_W])
{
	struct x9_fill_run runs[64];
	uint32_t nruns = 0;
	uint32_t x = 0;

	while (x < X9_PANEL_W && nruns < 64) {
		uint32_t run = 1;
		while (x + run < X9_PANEL_W &&
		    columns[x + run] == columns[x] + run &&
		    columns[x] + run < X9_CANVAS_W)
			run++;
		runs[nruns].dst_x = (uint16_t)x;
		runs[nruns].src_x = columns[x];
		runs[nruns].len = (uint16_t)run;
		nruns++;
		x += run;
	}

	if (x < X9_PANEL_W) {
		for (uint32_t y = 0; y < X9_PANEL_H; y++) {
			const uint8_t *s = src + (uint64_t)y * src_stride;
			uint8_t *d = dst + (uint64_t)y * dst_stride;
			for (x = 0; x < X9_PANEL_W; x++) {
				uint32_t pixel;
				memcpy(&pixel, s + (uint64_t)columns[x] * 4, 4);
				memcpy(d + (uint64_t)x * 4, &pixel, 4);
			}
		}
		return;
	}

	for (uint32_t y = 0; y < X9_PANEL_H; y++) {
		const uint8_t *s = src + (uint64_t)y * src_stride;
		uint8_t *d = dst + (uint64_t)y * dst_stride;
		for (uint32_t i = 0; i < nruns; i++)
			x9_fill_copy_run(d + (uint64_t)runs[i].dst_x * 4,
			    s + (uint64_t)runs[i].src_x * 4, runs[i].len);
	}
}
#endif
