/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * LK scanout preservation, adapted from UEFI-Dali DisplayDxe.c at
 * 58729eb922b608b3691fbc59c04b2f35e0de75d2 (localhosts-A/UEFI-Dali).
 * MT6993 addresses/fields are from the captured X9 DT and vendor driver.
 * No panel, PHY, DSC, clock, route, EXDMA or mutex configuration writes.
 */
#include <stdint.h>
#include "hw.h"
#include "fdt.h"
#include "plog.h"
#include "display_lk.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define FRAME_BYTES ((uint64_t)FB_STRIDE * FB_H)
#define SLOT_BYTES ((uint64_t)FB_STRIDE * ((FB_H + 31u) & ~31u))
#define BUSY 0x80000000u

static const uintptr_t banks[] = {
	0x3e320000UL, 0x3e720000UL, 0x32920000UL, 0x32c20000UL,
};
/* Only real OVL0/OVL1 EXDMA nodes from MT6993 DT. EXDMA1 and
 * OVL1 EXDMA0 are absent; never probe those holes. */
static const uintptr_t engines[] = {
	0x32930000UL, 0x32950000UL, 0x32960000UL, 0x32970000UL,
	0x32980000UL, 0x32990000UL, 0x329a0000UL,
	0x32c50000UL, 0x32c60000UL, 0x32c70000UL,
	0x32c80000UL, 0x32c90000UL, 0x32ca0000UL,
};
static const unsigned layer_offsets[] = {
	0x20, 0x40, 0x48, 0x50, 0x2f4, 0x300, 0xf40, 0xf4c,
	0x2f0, 0x44, 0x4c, 0x100,
};
struct layer {
	uint32_t en, layer, size, format, pitch, con, low, high;
	uint32_t pitch_high, offset, clip, rdma;
};
struct inherited {
	uint64_t scanouts[COUNT(engines)];
	unsigned count, mutex_id, mutex_mask, swap;
};

static void fence(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Independent CRC copies, never reconstruct ECC-damaged fields. phase:
 * 0=inherited register, 1=before submit, 2=after submit, 3=final/decision.
 * Decision records use small synthetic keys; all real MMIO keys >= 0x10000.
 */
static void record(unsigned phase, uint64_t address, uint32_t value)
{
	static const char hex[] = "0123456789abcdef";
	char s[43];
	s[0] = 'l'; s[1] = 'k'; s[2] = '5'; s[3] = '0';
	s[4] = s[6] = s[23] = s[32] = ':';
	s[5] = hex[phase & 15];
	for (unsigned i = 0; i < 16; i++)
		s[7 + i] = hex[(address >> ((15 - i) * 4)) & 15];
	for (unsigned i = 0; i < 8; i++)
		s[24 + i] = hex[(value >> ((7 - i) * 4)) & 15];
	uint32_t crc = 0xffffffffu;
	for (unsigned i = 0; i < 32; i++) {
		crc ^= (unsigned char)s[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	crc = ~crc;
	for (unsigned i = 0; i < 8; i++)
		s[33 + i] = hex[(crc >> ((7 - i) * 4)) & 15];
	s[41] = '\n'; s[42] = 0;
	for (unsigned i = 0; i < 4; i++)
		plog_str(s);
	wdt_kick();
}

static int framebuffer_contract(uintptr_t fdt)
{
	uint64_t low, high, size;
	return fdt_chosen_u64(fdt, "atag,videolfb-fb_base_l", &low) &&
	       fdt_chosen_u64(fdt, "atag,videolfb-fb_base_h", &high) &&
	       fdt_chosen_u64(fdt, "atag,videolfb-vramSize", &size) &&
	       low == FB_BASE && high == 0 && size == FB_SIZE;
}

static int valid_layer(const struct layer *l, uint64_t *address)
{
	*address = l->low | ((uint64_t)(l->high & 15u) << 32);
	/* L0_EN bit4=FBDC, bits9:8=layer source. CON2 bit9/10=flip.
	 * Accept 32-bit linear format 2, with its RGB-swap bit16 recorded.
	 * Reject nonzero pitch extension, offsets/clips and invalid address high bits.
	 */
	return (l->en & 1u) && (l->layer & 1u) &&
	       !(l->layer & 0x310u) && !(l->con & 0x600u) &&
	       (l->pitch & 0xffffu) == FB_STRIDE && !(l->pitch_high & 15u) &&
	       (l->size & 0x3fffu) == FB_W && ((l->size >> 16) & 0x3fffu) == FB_H &&
	       !l->offset && !l->clip && !(l->high & ~15u) &&
	       (l->format & 0x2031fu) == 2u &&
	       *address >= FB_BASE && *address <= FB_BASE + FB_SIZE - FRAME_BYTES &&
	       (*address - FB_BASE) % SLOT_BYTES == 0;
}

static int prepare_layers(struct inherited *state)
{
	unsigned canonical = 0;
	for (unsigned u = 0; u < COUNT(engines); u++) {
		uintptr_t base = engines[u];
		struct layer l;
		uint32_t words[COUNT(layer_offsets)];
		for (unsigned i = 0; i < COUNT(layer_offsets); i++)
			words[i] = rd32(base + layer_offsets[i]);
		/* Explicit assignment avoids strict-aliasing/struct-layout assumptions. */
		l.en = words[0]; l.layer = words[1]; l.size = words[2];
		l.format = words[3]; l.pitch = words[4]; l.con = words[5];
		l.low = words[6]; l.high = words[7]; l.pitch_high = words[8];
		l.offset = words[9]; l.clip = words[10]; l.rdma = words[11];
		for (unsigned i = 0; i < COUNT(layer_offsets); i++)
			record(0, base + layer_offsets[i], words[i]);
		uint64_t address;
		if (!valid_layer(&l, &address))
			continue;
		/* Matching formats across all buffers prevents mirrored channel swaps. */
		unsigned swap = (l.format >> 16) & 1u;
		if (state->count && swap != state->swap)
			return 0;
		state->swap = swap;
		unsigned duplicate = 0;
		for (unsigned i = 0; i < state->count; i++)
			duplicate |= state->scanouts[i] == address;
		if (!duplicate)
			state->scanouts[state->count++] = address;
		canonical |= address == FB_BASE;
	}
	return canonical && state->count;
}

static int prepare_mutex(struct inherited *state)
{
	unsigned matches = 0;
	/* MT6993 has 8 mutex slots per bank. Dali's generic loop uses 10. */
	for (unsigned id = 0; id < 8; id++) {
		unsigned mask = 0, single = 1;
		uint32_t mod0[4], mod1[4];
		for (unsigned i = 0; i < COUNT(banks); i++) {
			uintptr_t b = banks[i] + id * 0x20u;
			uint32_t sof = rd32(b + 0x30);
			mod0[i] = rd32(b + 0x34); mod1[i] = rd32(b + 0x38);
			record(0, b + 0x20, rd32(b + 0x20));
			record(0, b + 0x30, sof);
			record(0, b + 0x34, mod0[i]); record(0, b + 0x38, mod1[i]);
			if (mod0[i] || mod1[i]) {
				mask |= 1u << i;
				if (sof & 7u)
					single = 0;
			}
		}
		/* MT6993 side MOD1 bit0=DSI0; OVL0 MOD0 bits3/4=EXDMA3/4. */
		if (single && (mod1[1] & 1u) && (mod0[2] & 0x18u)) {
			state->mutex_id = id; state->mutex_mask = mask;
			matches++;
		}
	}
	record(3, 1, matches);
	return matches == 1 && !(rd32(DSI_BASE + 0x34) & 3u);
}

static void paint(struct inherited *state, unsigned frame)
{
	/* No GOP in the direct shim: canonical opaque color bars, moving white
	 * band; mirror byte-for-byte into every validated LK scanout buffer.
	 * start.S leaves EL1 D-cache OFF. dsb sy publishes stores for DMA.
	 */
	volatile uint32_t *p = (volatile uint32_t *)FB_BASE;
	static const uint32_t colors[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu};
	for (unsigned y = 0; y < FB_H; y++) {
		for (unsigned x = 0; x < FB_STRIDE / 4; x++) {
			uint32_t color = colors[(x * 3 / FB_W) % 3];
			if (y / 96 == frame % ((FB_H + 95) / 96))
				color = 0xffffffffu;
			if (state->swap)
				color = (color & 0xff00ff00u) | ((color & 255u) << 16) | ((color >> 16) & 255u);
			p[(uint64_t)y * (FB_STRIDE / 4) + x] = color;
		}
		if (!(y & 255u))
			wdt_kick();
	}
	fence();
}

static void mirror(struct inherited *state)
{
	volatile uint32_t *p = (volatile uint32_t *)FB_BASE;
	for (unsigned i = 0; i < state->count; i++) {
		if (state->scanouts[i] == FB_BASE)
			continue;
		volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)state->scanouts[i];
		for (uint64_t n = 0; n < FRAME_BYTES / 4; n++) {
			d[n] = p[n];
			if (!(n & 0x3ffffu))
				wdt_kick();
		}
	}
	fence();
}

static int refresh(struct inherited *state, unsigned frame)
{
	uint32_t status = rd32(DSI_BASE + 0x0c);
	record(1, DSI_BASE + 0x0c, status);
	if (status & BUSY)
		return 0;
	paint(state, frame);
	mirror(state);
	/* Do not restart a transfer that became busy during rendering. */
	if (rd32(DSI_BASE + 0x0c) & BUSY)
		return 0;
	for (unsigned i = 0; i < COUNT(banks); i++)
		if (state->mutex_mask & (1u << i))
			wr32(banks[i] + 0x20 + state->mutex_id * 0x20u, 1);
	wr32(DSI_BASE, 0);
	wr32(DSI_BASE, 1);
	fence();
	record(2, DSI_BASE + 0x0c, rd32(DSI_BASE + 0x0c));
	return 1;
}

static int initialize(struct inherited *state, uintptr_t dtb)
{
	state->count = state->mutex_id = state->mutex_mask = state->swap = 0;
	plog_str("x9 LK display: preserve initialized scanout; no reinitialization\n");
	if (!framebuffer_contract(dtb)) {
		plog_str("lk preserve: FDT framebuffer mismatch; disabled\n");
		return 0;
	}
	static const unsigned dsi_offsets[] = {0, 8, 0x0c, 0x30, 0x34, 0x38, 0x3c, 0x44, 0x48, 0x400};
	for (unsigned i = 0; i < COUNT(dsi_offsets); i++)
		record(0, DSI_BASE + dsi_offsets[i], rd32(DSI_BASE + dsi_offsets[i]));
	/* No display writes before both independent validations pass. */
	int layers = prepare_layers(state);
	int mutex = prepare_mutex(state);
	record(3, 2, state->count); record(3, 3, layers); record(3, 4, mutex);
	if (!layers || !mutex) {
		plog_str("lk preserve: no validated scanout/unique command mutex; disabled\n");
		return 0;
	}
	record(3, 5, state->mutex_id); record(3, 6, state->mutex_mask);
	return 1;
}

void display_lk_preserve(uintptr_t dtb)
{
	struct inherited state;
	if (!initialize(&state, dtb))
		return;
	unsigned submitted = 0;
	for (unsigned frame = 0; frame < 16; frame++) {
		submitted += refresh(&state, frame);
		wait_ms(500);
		wdt_kick();
	}
	record(3, DSI_BASE + 0x0c, rd32(DSI_BASE + 0x0c));
	record(3, 7, submitted);
	plog_str("lk preserve: refresh stopped; submissions are not frame-completion proof\n");
}

/* Persistent console ownership ends before xnu_enter; the kernel never calls
 * back into shim memory. No display logging inside present() (no recursion). */
static struct inherited console_state;
static unsigned console_active;

int display_lk_console_init(uintptr_t dtb)
{
 console_active = initialize(&console_state, dtb);
 return console_active;
}

int display_lk_present(void)
{
 if (!console_active || (rd32(DSI_BASE + 0x0c) & BUSY))
  return 0;
 mirror(&console_state);
 if (rd32(DSI_BASE + 0x0c) & BUSY)
  return 0;
 for (unsigned i = 0; i < COUNT(banks); i++)
  if (console_state.mutex_mask & (1u << i))
   wr32(banks[i] + 0x20 + console_state.mutex_id * 0x20u, 1);
 wr32(DSI_BASE, 0);
 wr32(DSI_BASE, 1);
 fence();
 return 1;
}

void display_lk_stop(void)
{
 /* Bounded drain for the final log frame. Idle is not scanout proof. */
 for (unsigned t = 0; t < 200 && console_active; t++) {
  if (!(rd32(DSI_BASE + 0x0c) & BUSY))
   break;
  wait_ms(1);
  wdt_kick();
 }
 console_active = 0;
}
