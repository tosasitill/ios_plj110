/* Host regression executes the SAME geometry/compositor as the kernel. */
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <climits>

static bool PE_parse_boot_argn(const char *, int *, size_t) { return false; }
#include "../kernel/src/iokit/Kernel/arm/X9DisplayGeometry.h"
#include "../kernel/src/iokit/Kernel/arm/X9DisplayBackend.inc"

static void mapping()
{
	uint16_t columns[X9_PANEL_W];
	x9_fill_columns(columns);
	unsigned seen[X9_CANVAS_W] = {};
	for (uint32_t x = 0; x < X9_PANEL_W; x++) {
		assert(columns[x] < X9_CANVAS_W);
		assert(!x || columns[x] >= columns[x - 1]);
		seen[columns[x]]++;
		// The true inverse maps the touched pixel centre into the displayed
		// logical texel. Allow only the normalized 16.16 quantization error.
		int64_t fixed = (int64_t)x9_fill_normalized(x, X9_PANEL_W) * X9_CANVAS_W;
		assert(fixed >= (int64_t)columns[x] * 65536 - X9_CANVAS_W);
		assert(fixed < (int64_t)(columns[x] + 1) * 65536);
		assert(columns[x] + columns[X9_PANEL_W - 1 - x] == X9_CANVAS_W - 1);
	}
	unsigned duplicated = 0;
	for (unsigned n : seen) { assert(n == 1 || n == 2); duplicated += n - 1; }
	assert(duplicated == 14);
	for (uint32_t y = 0; y < X9_PANEL_H; y++) {
		int64_t fixed = (int64_t)x9_fill_normalized(y, X9_PANEL_H) * X9_CANVAS_H;
		assert(fixed >= (int64_t)y * 65536);
		assert(fixed < (int64_t)(y + 1) * 65536);
	}
	assert(x9_fill_normalized(-20, X9_PANEL_W) == x9_fill_normalized(0, X9_PANEL_W));
	assert(x9_fill_normalized(INT_MAX, X9_PANEL_W) == x9_fill_normalized(X9_PANEL_W - 1, X9_PANEL_W));
	assert(x9_fill_normalized(0, 0) == 0);
	// For all 4 rotations, rotate the same normalized point AFTER inverse
	// stretch; no driver-side rotation or second scale is necessary.
	for (int x = 0; x < (int)X9_PANEL_W; x++) {
		double u = x9_fill_normalized(x, X9_PANEL_W) / 65536.0;
		double actual = (x + .5) / X9_PANEL_W;
		assert(actual - u >= 0 && actual - u < 1.0 / 65536);
		assert((1 - u) - (1 - actual) < 1.0 / 65536);
	}
}

static x9_display::Surface surface(std::vector<uint32_t> &pixels, unsigned w, unsigned h)
{
	x9_display::Surface s = {};
	s.kernel_address = (uintptr_t)pixels.data();
	s.memory_length = pixels.size() * 4;
	s.pixel_format = 0x42475241;
	s.bytes_per_element = 4;
	s.element_width = s.element_height = 1;
	s.width = w; s.height = h; s.bytes_per_row = w * 4;
	return s;
}

static void compositor()
{
	using namespace x9_display;
	std::vector<uint32_t> src(16), dst(16, 0x12345678);
	for (unsigned i = 0; i < 16; i++) src[i] = 0xff000000 | i;
	Surface s = surface(src, 4, 4);
	auto draw = [&](Surface q, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
	                Format fmt = FormatBGRA, bool blend = false) {
		return draw_canvas((uint8_t *)dst.data(), 16, 4, 4, q, sx, sy, sw, sh, dx, dy, dw, dh, fmt, blend);
	};
	assert(draw(s, 0, 0, 4, 4, 0, 0, 4, 4));
	assert(dst == src);
	std::fill(dst.begin(), dst.end(), 0x12345678);
	assert(draw(s, 1, 1, 3, 3, -1, -1, 3, 3));
	assert(dst[0] == src[10] && dst[1] == src[11] && dst[4] == src[14]);
	assert(dst[2] == 0x12345678 && dst[8] == 0x12345678);
	// 2x2 -> 4x4 and scaled negative/off-screen clipping.
	assert(draw(s, 1, 1, 2, 2, 0, 0, 4, 4));
	assert(dst[0] == src[5] && dst[2] == src[6] && dst[8] == src[9] && dst[15] == src[10]);
	assert(draw(s, 1, 1, 2, 2, -2, -2, 4, 4));
	assert(dst[0] == src[10]);
	auto before = dst;
	assert(!draw(s, -1, 0, 4, 4, 0, 0, 4, 4));
	assert(!draw(s, 0, 0, 5, 4, 0, 0, 5, 4));
	assert(!draw(s, 0, 0, 4, 4, INT_MAX, 0, 4, 4));
	assert(!draw(s, 0, 0, 4, 4, 0, INT_MAX, 4, 4));
	assert(!draw(s, 0, 0, 4, 4, 4, 4, 4, 4));
	Surface bad = s; bad.memory_length--;
	assert(!draw(bad, 0, 0, 4, 4, 0, 0, 4, 4));
	bad = s; bad.offset = UINT64_MAX;
	assert(!draw(bad, 0, 0, 4, 4, 0, 0, 4, 4));
	bad = s; bad.bytes_per_row = 15;
	assert(!draw(bad, 0, 0, 4, 4, 0, 0, 4, 4));
	bad = s; bad.element_width = 2;
	assert(!draw(bad, 0, 0, 4, 4, 0, 0, 4, 4));
	assert(!draw(s, 0, 0, 4, 4, 0, 0, 4, 4, FormatUnsupported));
	assert(before == dst); // invalid inputs never partially mutate a frame
	// Correct nonzero IOSurface byte offset, not merely rect offsets.
	std::vector<uint32_t> prefixed(20, 0xdeadbeef);
	std::copy(src.begin(), src.end(), prefixed.begin() + 4);
	Surface shifted = surface(prefixed, 4, 4); shifted.offset = 16;
	assert(draw(shifted, 0, 0, 4, 4, 0, 0, 4, 4) && dst == src);
	std::fill(src.begin(), src.end(), 0x80000040); // premultiplied RGBA red
	std::fill(dst.begin(), dst.end(), 0xff000080);
	assert(draw(s, 0, 0, 4, 4, 0, 0, 4, 4, FormatRGBA, true));
	assert(dst[0] == blend_pixel(to_bgra(src[0], FormatRGBA), 0xff000080));
}

static void fullscreen()
{
	using namespace x9_display;
	constexpr unsigned stride = 5120, words = stride / 4;
	std::vector<uint32_t> canvas(words * X9_CANVAS_H, 0xfeedbeef);
	std::vector<uint32_t> panel(words * X9_PANEL_H, 0xdeadbeef);
	for (unsigned y = 0; y < X9_CANVAS_H; y++)
		for (unsigned x = 0; x < X9_CANVAS_W; x++) canvas[y * words + x] = 0xff000000 | (y << 12) | x;
	uint16_t columns[X9_PANEL_W]; x9_fill_columns(columns);
	auto baseline = canvas;
	for (unsigned frame = 0; frame < 8; frame++) {
		x9_fill_expand((uint8_t *)panel.data(), stride, (const uint8_t *)canvas.data(), stride, columns);
		assert(canvas == baseline); // no feedback/cumulative stretching
		for (unsigned y = 0; y < X9_PANEL_H; y++) {
			for (unsigned x = 0; x < X9_PANEL_W; x++)
				assert(panel[y * words + x] == canvas[y * words + columns[x]]);
			for (unsigned x = X9_PANEL_W; x < words; x++) assert(panel[y * words + x] == 0xdeadbeef);
		}
	}
	// Keyboard-sized bottom layer + small island-shaped top overlay share the
	// logical canvas; preserved background and both physical edge pixels.
	std::vector<uint32_t> keyboard(X9_CANVAS_W * 600, 0xffaabbcc);
	Surface k = surface(keyboard, X9_CANVAS_W, 600);
	assert(draw_canvas((uint8_t *)canvas.data(), stride, X9_CANVAS_W, X9_CANVAS_H,
	    k, 0, 0, X9_CANVAS_W, 600, 0, 2160, X9_CANVAS_W, 600, FormatBGRA, false));
	std::vector<uint32_t> island(360 * 100, 0xff112233);
	Surface i = surface(island, 360, 100);
	assert(draw_canvas((uint8_t *)canvas.data(), stride, X9_CANVAS_W, X9_CANVAS_H,
	    i, 0, 0, 360, 100, 441, 20, 360, 100, FormatBGRA, true));
	x9_fill_expand((uint8_t *)panel.data(), stride, (const uint8_t *)canvas.data(), stride, columns);
	assert(panel[2160 * words] == 0xffaabbcc && panel[2760 * words - (words - 1255)] == 0xffaabbcc);
	assert(panel[20 * words + X9_PANEL_W / 2] == 0xff112233);
	assert(panel[19 * words + X9_PANEL_W / 2] == baseline[19 * words + columns[X9_PANEL_W / 2]]);
	assert(canvas[2159 * words] == baseline[2159 * words]);
	// Explicit removal clears logical pixels; output must not retain side strips.
	memset(canvas.data(), 0, canvas.size() * 4);
	x9_fill_expand((uint8_t *)panel.data(), stride, (const uint8_t *)canvas.data(), stride, columns);
	for (unsigned y = 0; y < X9_PANEL_H; y++)
		for (unsigned x = 0; x < X9_PANEL_W; x++) assert(panel[y * words + x] == 0);
}

int main()
{
	mapping(); compositor(); fullscreen();
	puts("PASS: exhaustive inverse/14 duplicate pixels, clipping/bounds/offset/alpha, full panel/retention/padding/keyboard-overlay/20px");
}
