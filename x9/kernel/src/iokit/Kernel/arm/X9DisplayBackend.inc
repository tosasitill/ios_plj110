/* SPDX-License-Identifier: MIT */
/*
 * Task 7 display backend boundary, included by SweetFramebuffer.cpp after the
 * IOKit headers. It owns display-buffer description/validation and the legacy
 * CPU copy primitives. It performs no MMIO, takes no locks, allocates nothing
 * and changes no caller order; SweetFramebuffer keeps swap, holds, vsync and
 * the page-flip policy.
 *
 * Validation mirrors the conditions directPresent() already applied, so a
 * surface accepted before is still accepted and a refusal reason is reported
 * separately. No new acceptance rule is introduced here.
 */
#ifndef X9_DISPLAY_BACKEND_INC
#define X9_DISPLAY_BACKEND_INC

namespace x9_display {

enum Format { FormatBGRA, FormatRGBA, FormatARGB, FormatUnsupported };

struct Surface {
	uint64_t scanout_phys;      /* display-controller fetch base, 0 = not registered */
	uint64_t offset;
	uint64_t memory_length;
	uintptr_t kernel_address;
	uint32_t pixel_format;
	uint32_t bytes_per_element;
	uint32_t element_width;
	uint32_t element_height;
	uint32_t width;
	uint32_t height;
	uint32_t bytes_per_row;
};

struct Scanout {
	uint64_t phys;              /* first fetched byte */
	uintptr_t kernel_address;   /* same byte in the kernel's CPU mapping */
	uintptr_t cache_address;    /* window base directPresent cleaned before */
	uint32_t stride;
	uint32_t offset_x;
	uint32_t offset_y;
};

enum Reject {
	RejectNone,
	RejectNoController,
	RejectNotScanoutMemory,
	RejectElementSize,
	RejectTooSmall,
	RejectAlignment,
	RejectAddressWindow,
};

inline Format
format(uint32_t pixel_format)
{
	switch (pixel_format) {
	case 0:                 /* unset: what the copy path always assumed */
	case 0x42475241:        /* 'BGRA' */
		return FormatBGRA;
	case 0x52474241:        /* 'RGBA' */
		return FormatRGBA;
	case 0x41524742:        /* 'ARGB' */
		return FormatARGB;
	default:
		return FormatUnsupported;
	}
}

/* sweetfb_blend=0 restores the old opaque copies. Parsed once per caller. */
inline bool
blend_enabled()
{
	static int enabled = -1;
	if (enabled < 0) {
		int arg = 1;
		PE_parse_boot_argn("sweetfb_blend", &arg, sizeof(arg));
		enabled = arg != 0;
	}
	return enabled != 0;
}

/* Little-endian words: BGRA bytes read as 0xAARRGGBB. */
inline uint32_t
to_bgra(uint32_t v, Format format)
{
	if (format == FormatRGBA) {
		return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
	}
	if (format == FormatARGB) {
		return __builtin_bswap32(v);
	}
	return v;
}

/* Premultiplied source-over: d = s + d * (255 - a) / 255, per channel. */
inline uint32_t
blend_pixel(uint32_t s, uint32_t d)
{
	uint32_t a = s >> 24;
	if (a == 0xff) {
		return s;
	}
	if (s == 0) {
		return d;
	}
	uint32_t inv = 255 - a;
	uint32_t rb = (d & 0x00ff00ffu) * inv + 0x00800080u;
	rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	uint32_t ag = ((d >> 8) & 0x00ff00ffu) * inv + 0x00800080u;
	ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
	return s + (rb | ag);
}

inline void
row(uint8_t *dst, const uint8_t *src, uint32_t count, Format format, bool blend)
{
	if (format == FormatBGRA && !blend) {
		memcpy(dst, src, (size_t)count * 4);
		return;
	}
	for (uint32_t x = 0; x < count; x++) {
		uint32_t s, d;
		memcpy(&s, src + (uint64_t)x * 4, 4);
		s = to_bgra(s, format);
		if (blend) {
			memcpy(&d, dst + (uint64_t)x * 4, 4);
			s = blend_pixel(s, d);
		}
		memcpy(dst + (uint64_t)x * 4, &s, 4);
	}
}

inline void
rows(uint8_t *dst, uint32_t dst_stride, const uint8_t *src, uint32_t src_stride,
    uint32_t width, uint32_t height, Format format, bool blend)
{
	for (uint32_t y = 0; y < height; y++) {
		row(dst + (uint64_t)y * dst_stride, src + (uint64_t)y * src_stride, width, format, blend);
	}
}

/*
 * Nearest-neighbour scaled layer: source rect (sx,sy,sw,sh) onto panel rect
 * (dx,dy,dw,dh), clipped to the panel. The caller has bounds-checked the
 * whole source rect against the surface memory.
 */
inline void
scaled(uint8_t *dst, uint32_t dst_stride, uint32_t panel_w, uint32_t panel_h,
    const uint8_t *src, uint32_t src_stride, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t dx, int32_t dy, int32_t dw, int32_t dh, Format format, bool blend)
{
	int32_t x0 = dx < 0 ? 0 : dx, y0 = dy < 0 ? 0 : dy;
	int32_t x1 = dx + dw > (int32_t)panel_w ? (int32_t)panel_w : dx + dw;
	int32_t y1 = dy + dh > (int32_t)panel_h ? (int32_t)panel_h : dy + dh;
	if (x0 >= x1 || y0 >= y1) {
		return;
	}
	uint64_t xstep = ((uint64_t)sw << 16) / (uint64_t)dw;
	uint64_t ystep = ((uint64_t)sh << 16) / (uint64_t)dh;
	for (int32_t y = y0; y < y1; y++) {
		uint64_t r = ((uint64_t)(y - dy) * ystep) >> 16;
		if (r >= (uint64_t)sh) {
			r = (uint64_t)sh - 1;
		}
		const uint8_t *srow = src + (uint64_t)(sy + (int32_t)r) * src_stride + (uint64_t)sx * 4;
		uint8_t *drow = dst + (uint64_t)y * dst_stride;
		uint64_t fx = (uint64_t)(x0 - dx) * xstep;
		for (int32_t x = x0; x < x1; x++, fx += xstep) {
			uint64_t col = fx >> 16;
			if (col >= (uint64_t)sw) {
				col = (uint64_t)sw - 1;
			}
			uint32_t s, d;
			memcpy(&s, srow + col * 4, 4);
			s = to_bgra(s, format);
			if (blend) {
				memcpy(&d, drow + (uint64_t)x * 4, 4);
				s = blend_pixel(s, d);
			}
			memcpy(drow + (uint64_t)x * 4, &s, 4);
		}
	}
}

/*
 * Validate one full-panel scanout candidate and compute the panel window.
 * Conditions are exactly the ones directPresent() checked before it asked for
 * the kernel mapping: a registered scanout address, 4-byte elements, a surface
 * at least panel-sized, and a 16-byte aligned fetch inside the 32-bit display
 * address window. The caller performs the kernelAddress() check afterwards in
 * the same order as before; kernelAddress() may create a mapping lazily, so it
 * must not run for candidates that fail these checks. Reject is diagnostic.
 */
inline bool
validate(const Surface &s, bool controller_present, uint32_t panel_w, uint32_t panel_h,
    uint32_t logical_w, uint32_t logical_h, int32_t off_x, int32_t off_y,
    Scanout *out, Reject *reject)
{
	if (!out) {
		if (reject) {
			*reject = RejectNotScanoutMemory;
		}
		return false;
	}
	if (!controller_present) {
		if (reject) {
			*reject = RejectNoController;
		}
		return false;
	}
	if (s.scanout_phys == 0) {
		if (reject) {
			*reject = RejectNotScanoutMemory;
		}
		return false;
	}
	if (s.bytes_per_element != 4) {
		if (reject) {
			*reject = RejectElementSize;
		}
		return false;
	}
	if (s.width < panel_w || s.height < panel_h) {
		if (reject) {
			*reject = RejectTooSmall;
		}
		return false;
	}
	/* full-screen surface: panel window at the logical offset */
	uint32_t ox = (s.width - panel_w) / 2, oy = (s.height - panel_h) / 2;
	if (s.width == logical_w && s.height == logical_h) {
		ox = (uint32_t)off_x;
		oy = (uint32_t)off_y;
	}
	uint64_t pa = s.scanout_phys + s.offset + (uint64_t)oy * s.bytes_per_row + (uint64_t)ox * 4;
	if ((pa & 0xf) != 0) {
		if (reject) {
			*reject = RejectAlignment;
		}
		return false;
	}
	if (!(pa + (uint64_t)panel_h * s.bytes_per_row < (1ull << 32))) {
		if (reject) {
			*reject = RejectAddressWindow;
		}
		return false;
	}
	out->phys = pa;
	out->kernel_address = 0;
	out->cache_address = 0;
	out->stride = s.bytes_per_row;
	out->offset_x = ox;
	out->offset_y = oy;
	if (reject) {
		*reject = RejectNone;
	}
	return true;
}

/* Fill the CPU windows once the caller has obtained a non-zero mapping. */
inline void
mapped_addresses(const Surface &s, uintptr_t base, Scanout *out)
{
	out->kernel_address = base + s.offset + (uint64_t)out->offset_y * s.bytes_per_row + (uint64_t)out->offset_x * 4;
	out->cache_address = base + s.offset + (uint64_t)out->offset_y * s.bytes_per_row;
}

} /* namespace x9_display */

#endif /* X9_DISPLAY_BACKEND_INC */
