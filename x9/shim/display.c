/*
 * MT6993 display keep-alive — see wiki/MT6993-Display.md.
 *
 * The panel is command-mode DSC: LK pushes frames over DSI command packets
 * (DCS 0x2c) and the panel holds the image in its GRAM. After LK jumps away
 * nobody triggers frames. LK leaves its pipeline configured; the registers
 * whose values point into DRAM identify the pixel source for a later fill.
 */
#include <stdint.h>
#include "display.h"
#include "hw.h"
#include "plog.h"
#include "panel_init_table.h"

#define DSI_START	(DSI_BASE + 0x00)
#define DSI_INTSTA	(DSI_BASE + 0x0c)	/* bit31 BUSY, bit11 */

/* New-generation DISP pipeline blocks (Android DT); DSI0 feeds from the
 * ovl2 group. The exdma blocks read the framebuffer. */
/* SPM MTCMOS API (LK 0xb6100/0xb61a0): SET word 0x1002D100+((idx>>1)&0x70),
 * CLR at +4, bit idx&31; STATUS word 0x1002D200+((idx>>1)&0x70), bit idx&31.
 * Domain names/indices from the SSPM_SPM_DBG table in the handover DT. */
#define SPM_SET_BASE	0x1002D100UL
#define SPM_CLR_BASE	0x1002D104UL
#define SPM_STA_BASE	0x1002D200UL

static void dump_line(const char *name, uint32_t off, uint32_t v);
static void display_mml_shadow_dump(const char *tag);
static uint32_t gce_event_get(uint32_t ev);
static void gce_event_clr(uint32_t ev);

static void dump_win(uint64_t base, const char *tag)
{
	for (uint32_t o = 0; o < 0x40; o += 4)
		dump_line(tag, o, rd32(base + o));
	wdt_kick();
}

static void mtcmos_set(unsigned idx, int on)
{
	uint32_t word = (idx >> 1) & 0x70u;
	wr32((on ? SPM_SET_BASE : SPM_CLR_BASE) | word, 1u << (idx & 31));
	__asm__ volatile("dsb sy" ::: "memory");
}

static unsigned mtcmos_status(unsigned idx)
{
	return (rd32(SPM_STA_BASE + ((idx >> 1) & 0x70u)) >> (idx & 31)) & 1u;
}

static const struct { uint64_t idx; char name[12]; } power_domains[] = {
	{ 11, { 'd','i','s','p','_','v','c','o','r','e',0 } },
	{ 12, { 'd','i','s','0',0 } },
	{ 13, { 'd','i','s','1',0 } },
	{ 25, { 'm','m','i','n','f','r','a','0',0 } },
	{ 28, { 'm','m','_','p','r','o','c',0 } },
	{ 22, { 'o','v','l','0',0 } },
	{ 23, { 'o','v','l','1',0 } },
	/* LK's mutex power-on (lk.bin 0x6dccc/0x6ddcc) pulls SPM domains
	 * 0x31/0x32 before touching the mutex units. */
	{ 49, { 'm','4','9',0 } },
	{ 50, { 'm','5','0',0 } },
};

void display_power_diag(void)
{
	/* Bisect: power one candidate domain at a time and re-read EXDMA2's
	 * config word — the register leaving 0xffffffff names the domain that
	 * gates the display pipeline. */
	for (unsigned i = 0; i < sizeof(power_domains) / sizeof(power_domains[0]); i++) {
		unsigned idx = power_domains[i].idx;
		mtcmos_set(idx, 1);
		for (unsigned t = 0; t < 50; t++) {
			if (mtcmos_status(idx))
				break;
			wait_ms(1);
		}
		dump_line(power_domains[i].name, mtcmos_status(idx), 0);
		uint32_t probe = rd32(0x3e050000UL + 0x0c);
		dump_line("bis", idx, probe);
		wdt_kick();
	}
	plog_str("power diag done\n");
}
/* Every line is written twice: the pmsg zone's ECC "corrects" our raw
 * writes at deterministic offsets, so two copies at different offsets let
 * the reader reconstruct each line by majority. */
static void dump_line(const char *name, uint32_t off, uint32_t v)
{
	for (int copy = 0; copy < 2; copy++) {
		plog_str(name);
		plog_hex("+off", ((uint64_t)off << 32) | v);
	}
}

static void dump_block(uint64_t base, const char *name, uint32_t words)
{
	for (uint32_t off = 0; off < words * 4; off += 4) {
		dump_line(name, off, rd32(base + off));
	}
	wdt_kick();
}

/* Every line is written twice: the pmsg zone's ECC "corrects" our raw
 * writes at deterministic offsets, so two copies at different offsets let
 * the reader reconstruct each line by majority. */
void display_early_diag(void)
{
	/* The kernel's checkpoint lines must land beyond the ~2 KiB head zone
	 * that Android-side writers clobber, so the record head is padded with
	 * a full dump of the blocks that answer (DSI, mutex). */
	/* Deep dumps: the EXDMA source address register lives beyond 0x40, and
	 * LK's logo path used the ovl0-group EXDMAs (0x3295xxxx). */
	dump_block(DSI_BASE, "dsi", 0x40);
	/* 0x32960000 = disp-ovl0-exdma3 (LK DT), driven from mediatek-drm.ko's
	 * mtk_ovl_exdma_* code. mml1-boot hardware facts: +0xf40 holds LK's
	 * 0xfc910000 (the videolfb base in this block's own layout), +0x20 the
	 * panel size. Dump the whole 0xf00 shadow region plus the +0x110..0x150
	 * twin zone; the mml0/mml1 mutexes are logged only to document that
	 * they stay cold and are not this block's synchroniser. */
	dump_block(0x3e050000UL, "ex2", 0x100);
	dump_block(0x3e060000UL, "ex3", 0x100);
	dump_block(0x3e070000UL, "ex4", 0x100);
	dump_block(0x3e080000UL, "ex5", 0x100);
	dump_block(0x32950000UL, "o0e2", 0x100);
	dump_block(0x32960000UL, "o0e3", 0x100);
	display_mml_shadow_dump("shd");
	dump_block(0x32020000UL, "mm0", 0x40);
	dump_block(0x32320000UL, "mm1", 0x40);
	/* ovlsys_config (0x32900000) is the ovl0 domain's own clock controller
	 * (DT phandle 0x21a: every disp-ovl0-exdma/dlo-async node's clocks
	 * reference it). mmsys0's gate words live at +0xa18/+0xaa8 (0xfffffbff
	 * when on); this block has never been observed. ovl2's config block
	 * (0x3e000000) covers the dl-side feed. */
	dump_block(0x32900000UL, "ovc", 0x400);
	dump_block(0x3e000000UL, "ov2", 0x100);
	dump_block(0x3e7f0000UL, "dscw", 0x40);
	dump_block(0x3e320000UL, "mux", 0x40);
	/* The ovlsys mout select slots (0xec8..0xf38, one per EXDMA) and the
	 * routing registers around them name the pixel path LK configured. */
	dump_block(0x3e300000UL, "mms", 0x3d1);
	/* The dlo/dli-async routing blocks (DSI-side pixel path) share the
	 * 0x3e700000 regmap per the handover DT; LK touched it 12 times. */
	dump_block(0x3e700000UL, "dl", 0x400);
	plog_str("dsi dump done\n");
}


/* o0e3 = 0x32960000 = disp-ovl0-exdma3 (LK DT 'disp-ovl0-exdma3@32960000',
 * compatible mediatek,mt6993-disp-ovl-exdma; driver mtk_ovl_exdma_* lives in
 * mediatek-drm.ko). Hardware facts from the mml1 boot: LK left 0xfc910000 at
 * +0xf40 — the videolfb pointer in this block's own layout (the mtk-mml RDMA
 * pairs +0x118/+0xf44 read 0 here, so +0xf40 is the populated base slot);
 * +0x00 reads 0x5, +0x20 the panel size 0x0ac804e8; writes land with field
 * masking (+0x2c: wrote 0x1400, read 0x400). The mml0-mutex0 at 0x32020000
 * is an unrelated dead block (EN write does not stick) — o0e3 belongs to the
 * ovl0 domain synchronised by the ovlsys mutex we already arm. */
static void display_mml_shadow_dump(const char *tag)
{
	static const uint32_t offs[] = {
		0xf00, 0xf08, 0xf10, 0xf20, 0xf28, 0xf30, 0xf34,
		0xf38, 0xf3c, 0xf40, 0xf44, 0xf48, 0xf4c, 0xf50,
		0xf58, 0xf60, 0xf64, 0xf70, 0xf74,
		0x110, 0x118, 0x120, 0x128, 0x148, 0x150,
	};
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		dump_line(tag, offs[i], rd32(0x32960000UL + offs[i]));
	wdt_kick();
}

/* Clock gates: each dispsys regmap is its own clock controller (DT phandles:
 * 0x3e300000 mmsys0, 0x32900000 ovlsys_config for the ovl0-exdma domain,
 * 0x3e700000 mmsys1). mmsys0 hardware shows the gate words at +0xa18/+0xaa8
 * reading 0xfffffbff while its engine runs, and the +0x100 zone is dead.
 * The ovl0 domain's gates were never raised — raise the same offsets in
 * 0x32900000 (covering con/set/clr triplets), read back, and log. */
void display_clock_enable(void)
{
	const uint64_t ovc = 0x32900000UL;
	static const uint32_t offs[] = {
		0xa18, 0xa1c, 0xa20, 0xa68, 0xa6c, 0xa70, 0xa74,
		0xa78, 0xaa8, 0xaac, 0xab0,
	};
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		dump_line("cgb", offs[i], rd32(ovc + offs[i]));
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		wr32(ovc + offs[i], 0xffffffffu);
	__asm__ volatile("dsb sy" ::: "memory");
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		dump_line("cga", offs[i], rd32(ovc + offs[i]));
	plog_str("clock enable done\n");
}

/* Static LK config is intact and the ovl0 gates are open, yet the pulse
 * sweep (cg2, bits on o0e3 +0x00/+0x24..+0x38) never moved a counter: the
 * per-frame start is not an EXDMA register bit. LK's display flow is
 * token-driven through the GCE (LK DT: dispsys-config 'mboxes' reserves
 * GCE threads 0/1/2/24 CFG, 3/5/25 TRIG_LOOP, 7 EVENT_LOOP; the embedded
 * FDT carries 'mml_stream_config'/'mml_stop' as GCE events). So: refresh
 * LK's base, confirm +0xf40 is writable, keep the ovl2 mout slot, and let
 * display_gce_signal() find the missing start among the GCE events. */
void display_replay_config(void)
{
	const uint64_t oe3 = 0x32960000UL;
	display_clock_enable();
	/* Refresh LK's base with the same value — confirms +0xf40 is writable. */
	wr32(oe3 + 0xf40, 0xfc910000UL);
	dump_line("wB4", 0xf40, rd32(oe3 + 0xf40));
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(0x3e300000UL + 0xee8, 2);
	__asm__ volatile("dsb sy" ::: "memory");
	plog_str("replay config done\n");
}

/* LK's display power-up path (lk.bin 0x6dc54): mmsys0/mmsys1 gate words
 * +0xa68/+0xa74/+0x1a8 and +0xa6c/+0xa78/+0x1a8, then the two ovlsys
 * clock domains — 0x32900000 (ovlsys_config, ovl0) and 0x32c00000
 * (ovl1_ovlsys_config) — whose gate words are +0xad0/+0xdc, then bits
 * 12-15 of each domain's +0x38 are cleared. cg2 raised +0xa18..+0xab0 in
 * 0x32900000 and saw +0xa68 move, but LK's bank is +0xad0/+0xdc, and the
 * second (ovl1) domain was never touched at all. Also dumps the blocks no
 * build has observed yet: postmask0 0x3e3d0000, splitter0 0x3e7d0000, and
 * four mutex-domain candidates (the disp DT declares only
 * disp1-mutex0@3e320000; LK keeps two runtime mutex pointers — mmsys1/
 * ovl0+0x2000/ovl1+0x2000 are the by-layout candidates). */
void display_lk_ungate(void)
{
	dump_win(0x32c00000UL, "c1");
	dump_win(0x3e3d0000UL, "pm");
	dump_win(0x3e7d0000UL, "sp");
	dump_win(0x3e720000UL, "m1");
	dump_win(0x32902000UL, "m2");
	dump_win(0x32c02000UL, "m3");
	dump_line("g1", 0xad0, rd32(0x32900000UL + 0xad0));
	dump_line("g1", 0xadc, rd32(0x32900000UL + 0xadc));
	dump_line("g2", 0xad0, rd32(0x32c00000UL + 0xad0));
	dump_line("g2", 0xadc, rd32(0x32c00000UL + 0xadc));
	dump_line("g3", 0x38, rd32(0x32900000UL + 0x38));
	dump_line("g4", 0x38, rd32(0x32c00000UL + 0x38));

	/* pan40 retained status-word writes here. Correction from the clock
	 * driver (clk-mt6993-mmsys.c): +0xAC8/+0xAD4 are STATUS, while
	 * +0xAD0/+0xADC are the corresponding CLR aliases. The earlier CLR
	 * addresses were not one-word-off mistakes. An inherited zero STATUS
	 * is not evidence that these writes ungated anything. pan44 keeps
	 * this sequence unchanged to test only the PQ input branch below. */
	wr32(0x3e300000UL + 0xa68, 0xffffffffu);
	wr32(0x3e300000UL + 0xa74, 0xffffffffu);
	wr32(0x3e300000UL + 0x1a8, 0xffffffffu);
	wr32(0x3e700000UL + 0xa6c, 0xffffffffu);
	wr32(0x3e700000UL + 0xa78, 0xffffffffu);
	wr32(0x3e700000UL + 0x1a8, 0xffffffffu);
	wr32(0x32900000UL + 0xac8, 0x0u);
	wr32(0x32900000UL + 0xad4, 0x0u);
	wr32(0x32c00000UL + 0xac8, 0x0u);
	wr32(0x32c00000UL + 0xad4, 0x0u);
	wr32(0x32900000UL + 0x38, rd32(0x32900000UL + 0x38) & 0xffff0fffu);
	wr32(0x32c00000UL + 0x38, rd32(0x32c00000UL + 0x38) & 0xffff0fffu);
	__asm__ volatile("dsb sy" ::: "memory");

	dump_line("g5", 0xac8, rd32(0x32900000UL + 0xac8));
	dump_line("g5", 0xad4, rd32(0x32900000UL + 0xad4));
	dump_line("g6", 0xac8, rd32(0x32c00000UL + 0xac8));
	dump_line("g6", 0xad4, rd32(0x32c00000UL + 0xad4));
	dump_line("g7", 0x38, rd32(0x32900000UL + 0x38));
	plog_str("lk ungate done\n");
}

/* The mutex software trigger: LK writes 1 then 0 to [mutex + id*0x20 +
 * 0x2c] (lk 0x6da38) on both of its mutex domains. Every earlier build
 * armed EN (+0x20) and restored SOF/MOD but never pulsed +0x2c — the one
 * physical LK write the replay was missing. postmask0's enable idiom
 * (lk 0x6dea4) is base |= 1 and +8 = 0x917. */
static const uint64_t mutex_cand[4] = {
	0x3e320000UL, 0x3e720000UL, 0x32902000UL, 0x32c02000UL,
};

static void mutex_pulse(uint64_t base)
{
	wr32(base + 0x2c, 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(base + 0x2c, 0u);
	__asm__ volatile("dsb sy" ::: "memory");
}

void display_mutex_kick(void)
{
	wr32(0x3e3d0000UL, rd32(0x3e3d0000UL) | 1u);
	wr32(0x3e3d0008UL, 0x917u);
	__asm__ volatile("dsb sy" ::: "memory");
	dump_line("pk", 0x0, rd32(0x3e3d0000UL));
	dump_line("pk", 0x8, rd32(0x3e3d0008UL));

	/* A: pulse the mmsys0 mutex alone. */
	mutex_pulse(mutex_cand[0]);
	wait_ms(150);
	wdt_kick();
	dump_win(mutex_cand[0], "mA");
	dump_line("dA", 0x0c, rd32(DSI_BASE + 0x0c));

	/* B: pulse, then replay the LK DSI trigger. */
	mutex_pulse(mutex_cand[0]);
	for (int i = 0; i < 2; i++) {
		dsi_trigger_lk();
		dsi_wait_idle();
		wdt_kick();
	}
	dump_win(mutex_cand[0], "mB");
	dump_line("dB", 0x0c, rd32(DSI_BASE + 0x0c));

	/* C: re-write the full LK enable set (SOF/MOD/EN), then pulse, then
	 * trigger. */
	wr32(mutex_cand[0] + 0x30, 0x00000080u);
	wr32(mutex_cand[0] + 0x34, 0x030001ffu);
	wr32(mutex_cand[0] + 0x38, 0x003de300u);
	wr32(mutex_cand[0] + 0x20, 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	mutex_pulse(mutex_cand[0]);
	for (int i = 0; i < 2; i++) {
		dsi_trigger_lk();
		dsi_wait_idle();
		wdt_kick();
	}
	dump_win(mutex_cand[0], "mC");
	dump_line("dC", 0x0c, rd32(DSI_BASE + 0x0c));

	/* D: pulse every mutex-domain candidate. */
	for (unsigned i = 0; i < 4; i++)
		mutex_pulse(mutex_cand[i]);
	wait_ms(150);
	wdt_kick();
	for (unsigned i = 0; i < 4; i++) {
		char tag[3] = { 'm', '0' + (char)i, 0 };

		dump_line(tag, 0x2c, rd32(mutex_cand[i] + 0x2c));
		dump_line(tag, 0x20, rd32(mutex_cand[i] + 0x20));
		dump_line(tag, 0x24, rd32(mutex_cand[i] + 0x24));
	}
	dump_line("dD", 0x0c, rd32(DSI_BASE + 0x0c));
	plog_str("mutex kick done\n");
}


/* Kernel-exact mt6993 command send (mtk_dsi_cmdq + mtk_dsi_host_send_cmd,
 * OPPO mt6993 source, cloned at oppo-source/modules): the FIFO window is
 * +0x400 — +0x200 is DSI_TX_TIMECON0 on this VER_N3 layout, so every
 * pre-pan31 send wrote timing registers instead of packets, which is why
 * no command ever reached the DDIC and every read served stale FIFO
 * residue (pan28). Shorts: header (type<<8)|config, payload bytes at
 * +0x402. Longs: header (len<<16)|(type<<8)|config, payload words from
 * +0x404. config = SHORT(0)/LONG(2), BTA(4) for reads, +HSTX(8) only for
 * HS sends. +0x44 = size|SIZE_SEL(0x8000). START is the kernel's
 * full-word 0->1 with a trailing 0, and completion is BUSY (bit31)
 * CLEARING — the LK-style wait-for-bit31-HIGH waits for busy to START,
 * not for the packet to finish. */
static int dsi_cmdq_send_k(uint64_t dsi, unsigned char type,
			   const unsigned char *data, unsigned char count,
			   unsigned char flags)
{
	unsigned config = (count > 2) ? 0x2u : 0x0u;
	unsigned size = (count > 2) ? 1u + (count + 3) / 4 : 1u;
	unsigned t;

	if (flags & 0x1u)		/* HSTX */
		config |= 0x8u;
	if (flags & 0x2u)		/* BTA (reads) */
		config = 0x4u;

	wr32(dsi + 0x400, 0);
	if (count > 2) {
		for (unsigned w = 0; w < size - 1; w++) {
			uint32_t v = 0;

			for (unsigned b = 0; b < 4; b++) {
				unsigned i = w * 4 + b;

				if (i < count)
					v |= (uint32_t)data[i] << (b * 8);
			}
			wr32(dsi + 0x404 + w * 4, v);
		}
		wr32(dsi + 0x400, ((uint32_t)count << 16)
				  | ((uint32_t)type << 8) | config);
	} else {
		uint32_t m;

		wr32(dsi + 0x400, ((uint32_t)type << 8) | config);
		for (unsigned i = 0; i < count; i++) {
			m = 0xffu << ((2 + i) * 8);
			wr32(dsi + 0x400, (rd32(dsi + 0x400) & ~m)
					  | ((uint32_t)data[i] << ((2 + i) * 8)));
		}
	}
	/* kernel does two field-masked writes (CMDQ_SIZE=0x3ff, then
	 * CMDQ_SIZE_SEL=BIT15); a full-word write would clobber the page
	 * bits and anything else live in +0x44. */
	wr32(dsi + 0x44, (rd32(dsi + 0x44) & ~(0x3ffu | 0x8000u))
			  | size | 0x8000u);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(dsi + 0x00, 0);
	wr32(dsi + 0x00, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(dsi + 0x00, 0);
	for (t = 0; t < 200; t++) {
		if (!(rd32(dsi + 0x0c) & 0x80000000u))
			break;
		wait_ms(1);
	}
	return t == 200;
}

/* V2-exact DCS read. mt6993 runs DSI_CMD_V2 (MMSYS_MT6993 + cmd mode +
 * dsi_cmd_v2_en), so the authoritative path is mtk_dsi_cmd_transfer +
 * mtk_setup_dsi_cmdq + mtk_dsi_send_cmd_trigger + mtk_dsi_read_data_by_cpu:
 * 1. RX responses reach the CMDQ SRAM (page 3) only with RX_DATA_SRAM_MODE
 *    (DSI_RX_CON +0xa0 BIT0) — mtk_dsi_ddp_config sets it once at probe.
 *    pan31/32 never touched +0xa0, which is why page 3 read static garbage
 *    while the legacy +0xa4 window held the live response.
 * 2. Both packets queue in ONE FIFO pass and ONE trigger: max-return
 *    (0x37 short, payload = rx_len) at +0x400, DCS read (0x06, config
 *    BTA=4) at +0x404, total size 2 | SIZE_SEL.
 * 3. Order: poll LPRX_RD_RDY (INTSTA bit0), clear bit0, READ the data,
 *    RACK last. pan31/32 acked RACK before reading.
 * 4. Byte order (mtk_dsi_read_data_by_cpu): read_data[0] = byte @+0x5FC =
 *    the LOW byte of the 32-bit read. pan31 had it reversed.
 * The legacy +0xa4 words are dumped alongside. Returns read_data[1]
 * (first payload byte — for GET_POWER_MODE the power state) or 0. */
static uint32_t dsi_dcs_read_k(uint64_t dsi, uint8_t cmd, uint8_t ret_words)
{
	uint32_t w, trig, rx1, rx2, rx3;
	unsigned t;
	unsigned char type;

	wr32(dsi + 0xa0, rd32(dsi + 0xa0) | 0x1u);
	wr32(dsi + 0x400, ((uint32_t)ret_words << 16) | (0x37u << 8));
	wr32(dsi + 0x404, ((uint32_t)cmd << 16) | (0x06u << 8) | 0x4u);
	wr32(dsi + 0x44, (rd32(dsi + 0x44) & ~(0x3ffu | 0x8000u))
			  | 2u | 0x8000u);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(dsi + 0x00, 0);
	wr32(dsi + 0x00, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(dsi + 0x00, 0);

	for (t = 0; t < 100; t++) {
		if (rd32(dsi + 0x0c) & 0x1u)
			break;
		wait_ms(1);
	}
	wr32(dsi + 0x0c, rd32(dsi + 0x0c) & ~0x1u);
	trig = rd32(dsi + 0xb8);
	wr32(dsi + 0x44, (rd32(dsi + 0x44) & ~0x70000u) | 0x30000u);
	w = rd32(dsi + 0x5fc);
	wr32(dsi + 0x44, rd32(dsi + 0x44) & ~0x70000u);
	wr32(dsi + 0xb4, rd32(dsi + 0xb4) | 0x1u);
	for (t = 0; t < 200; t++) {
		if (!(rd32(dsi + 0x0c) & 0x80000000u))
			break;
		wait_ms(1);
	}

	type = w & 0xffu;
	rx1 = (w >> 8) & 0xffu;
	rx2 = (w >> 16) & 0xffu;
	rx3 = (w >> 24) & 0xffu;
	dump_line("rK0", cmd, type);
	dump_line("rK1", cmd, rx1);
	dump_line("rK2", cmd, rx2);
	dump_line("rK3", cmd, rx3);
	dump_line("rKt", cmd, (trig >> 8) & 0xfffu);
	/* legacy RX window, for continuity with pan2-32 evidence */
	dump_line("rx0", cmd, rd32(dsi + 0xa4));
	dump_line("rx1", cmd, rd32(dsi + 0xa8));
	dump_line("rx2", cmd, rd32(dsi + 0xac));
	dump_line("rx3", cmd, rd32(dsi + 0xb0));
	return rx1;
}

static const uint64_t dsi_pair[2] = { 0x3e730000UL, 0x3e740000UL };

/* pan35: backlight on. The panel is alive the whole boot — 0x0A reads
 * 0x9C, which the panel dtsi's own ESD config declares the HEALTHY
 * value (esd_check_cmd 0A, esd_check_value 9C, mask FF). The glass is
 * dark because the backlight is a DDIC register, not a PWM rail: the
 * dtsi's off sequence is a 5-byte `51 00 00 00 00` long write, the
 * on levels are 3-byte `51 hi lo` long writes, and the init table
 * only sets 0x53 0x20 (BC on) — the LEVEL is the kernel backlight
 * driver's job, which 35 rounds never did. LK's exit path is the only
 * writer that ever dimmed it.
 * pan35 hardware result: the HS variant (config LONG|HSTX) FAILED —
 * the read right after returned DI=0x02 Ack&Error with error bits
 * 0x0003 (SoT error / SoT sync error) and 0x52 was unchanged: the
 * clock lane is not running, so HS transmissions die. pan36 sends the
 * level in LP (config LONG only) — the panel's own init-table longs
 * are LP and all verified landing. Read 0x52 before/after for the
 * A/B, and re-read 0x0A to confirm the panel stayed alive. */
void display_backlight_on(void)
{
	static const unsigned char bl_on[3] = { 0x51, 0x0e, 0x7a };

	plog_str("bl0\n");
	dsi_dcs_read_k(dsi_pair[0], 0x52, 2);
	dsi_cmdq_send_k(dsi_pair[0], 0x39, bl_on, 3, 0);
	wait_ms(30);
	dsi_dcs_read_k(dsi_pair[0], 0x52, 2);
	dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
	plog_str("bl done\n");
}

/* Wake the panel and PROVE the link. DCS read get-power-mode (0x0A)
 * answers with sleep/display/booster state bits, before and after the
 * wake sequence: a response means panel and DSI link are alive and the
 * problem is upstream in the pixel pipeline; no response means the
 * panel side never hears us. Encode [15:8]=type, [23:16]=data0 — the
 * byte order that makes LK's 0x002c3909 a well-formed long write. */
void display_panel_wake(int minimal)
{
	plog_str("ck0\n");
	dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);	/* get power mode, baseline */

	if (minimal) {
		/* pan28 stale-FIFO discriminator, now through the kernel's
		 * own read path (page-3 RX at +0x5FF): 0x0D and 0x0E carry
		 * different payloads on a live link, and the rK0 response
		 * type (0x21/0x22) distinguishes a real response from
		 * garbage. The wake shorts go out kernel-exact: type 0x05,
		 * SHORT_PACKET config 0, LP (the dtsi's dsi_lp_mode). */
		plog_str("ckE\n");
		dsi_dcs_read_k(dsi_pair[0], 0x0d, 1);
		dsi_dcs_read_k(dsi_pair[0], 0x0e, 1);
		int wa = dsi_cmdq_send_k(dsi_pair[0], 0x05,
					 (const unsigned char *) "\x11", 1, 0);
		wait_ms(150);
		int wb = dsi_cmdq_send_k(dsi_pair[0], 0x05,
					 (const unsigned char *) "\x29", 1, 0);
		dump_line("wE", (unsigned)wa, (unsigned)wb);
		plog_str("ckE2\n");
		dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
		dsi_dcs_read_k(dsi_pair[0], 0x0d, 1);
		dsi_dcs_read_k(dsi_pair[0], 0x0e, 1);
		plog_str("ckE done\n");
		return;
	}

	/* pan31: the full oplus,dsi-on-command table through the kernel's
	 * own transport — FIFO at +0x400 (pan2-30 wrote the TX TIMECON
	 * bank at +0x200 instead; the DDIC never saw a single packet),
	 * SHORT_PACKET/LONG_PACKET config, LP mode per the dtsi's
	 * dsi_lp_mode state, kernel START and BUSY-clear completion.
	 * post_wait_ms honored per entry; per-entry pI log keeps the
	 * wedge evidence. Checkpoint reads use the kernel read path. */
	{
		unsigned wedges = 0;

		wr32(dsi_pair[0] + 0x0c, 0x80000000u);
		__asm__ volatile("dsb sy" ::: "memory");
		for (unsigned n = 0; n < sizeof(panel_init_table) /
					   sizeof(panel_init_table[0]); n++) {
			const struct panel_init_entry *e =
				&panel_init_table[n];
			const unsigned char *data = &panel_init_pool[e->off];
			unsigned char type = (e->count == 1) ? 0x05u
					   : (e->count == 2) ? 0x15u
							     : e->type;
			int to = dsi_cmdq_send_k(dsi_pair[0], type, data,
						 e->count, 0);

			wedges += (unsigned)to;
			dump_line("pI", n, ((uint32_t)type << 24)
					  | ((uint32_t)to << 16) | e->count);
			if (e->wait_ms)
				wait_ms(e->wait_ms);
			wdt_kick();
			switch (n) {
			case 10:
				plog_str("ck1\n");
				dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
				break;
			case 50:
				plog_str("ck2\n");
				dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
				break;
			case 200:
				plog_str("ck3\n");
				dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
				break;
			case 380:
				plog_str("ck4\n");
				dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
				break;
			case 381:
				plog_str("ck5\n");
				dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
				break;
			case 382:
				plog_str("ck6\n");
				dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
				break;
			}
		}
		plog_hex("pi wedges", wedges);
	}
	dump_line("pi", 0, (uint32_t)(sizeof(panel_init_table) /
				     sizeof(panel_init_table[0])));
	dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
	plog_str("panel wake done\n");
}

/* pan13->pan14: kernel-mirror video start. The OPPO mt6993 kernel source
 * (v1/mtk_dsi.c, driver data confirmed against the running .ko) uses a
 * register layout pan1-12 never touched: CON_CTRL +0x30, MODE_CTRL +0x34,
 * TXRX_CTRL +0x38, PSCTRL +0x3c, SIZE_CON +0x2c, VSA/VBP/VFP/VACT
 * +0x60/64/68/6c, HSA/HBP/HFP_WC +0x80/84/88, PHY bank +0x600, LCCON
 * +0x1d0, LCPAT +0x610. The old writes hit +0x10..0x2c/+0x110 (the dump
 * table's legacy names) - +0x110 is actually VM_CMD_CON, not a timing
 * shadow. Sequence mirrors mtk_preconfig_dsi_enable +
 * mtk_output_dsi_enable for the 60 Hz mode:
 *   lanes 4 RGB888 SYNC_PULSE NON_CONT_CLK, DSC slice_mode 1
 *   chunk 628 -> ps_wc 1256, PS_SEL 5 (DSC)
 *   hsa 2 hbp 21 hfp 9 vsa 14 vbp 22 vfp 0x34 (live DT timing@sa_fhd_60)
 *   data_rate 828 (LK leaves the MIPITX PLL at ~830, source comment
 *   "lk mipi setting is 830"; the PLL itself is not retouched here)
 * Porch bytes follow mtk_dsi_calc_vdo_timing exactly, SPR packed-8bpp
 * (bpp 2): hsa_byte wraps (2*2-10, u32), hbp 32; HFP_WC takes the VER_N3
 * minimum-clamp plus lane-alignment fine-tune (pan13 wrote 4; no frame
 * ever completed - the kernel value is 410).
 * pan14 adds what the pan13 log proved missing:
 *   - mtk_dsi_exit_ulps (runs unconditionally before every kernel video
 *     start): MIPITX lane sw-ctl park/unpark to LP11, DSI PHY_RESET
 *     pulse, LD0CON LDX_ULPM_AS_L0, MODE_CTRL SLEEP_MODE, TIME_CON0
 *     (+0x200, not the PHY bank) wake-up period, SLEEPOUT_START pulse
 *     and SLEEPOUT_DONE wait.
 *   - INTSTA is write-1-to-clear: the kernel writes ~status; pan13's
 *     write of 0 cleared nothing, so its final 0x10800 could be stale
 *     LK-era flags and its frame_done=0 was unverifiable.
 *   - post-START CON_CTRL/MODE/LCCON/LD0CON/TIME_CON readbacks, so the
 *     pan13 ECC ambiguity (CON 0x30 read back 0-or-2) cannot recur. */

/* MIPITX lane sw-control banks (mtk_mipi_tx.c MT6983-style offsets from
 * 0x130a0000): per lane {sw_ctl_en, pre_oe, oe, dp, dn} then the
 * complement lane's {pre_oe, oe}. D2/D0/CK/D1/D3/CK1. */
static const uint32_t mipi_lane_banks[6][7] = {
	{ 0x15c, 0x160, 0x164, 0x168, 0x16c, 0x180, 0x184 },
	{ 0x25c, 0x260, 0x264, 0x268, 0x26c, 0x280, 0x284 },
	{ 0x35c, 0x360, 0x364, 0x368, 0x36c, 0x380, 0x384 },
	{ 0x45c, 0x460, 0x464, 0x468, 0x46c, 0x480, 0x484 },
	{ 0x55c, 0x560, 0x564, 0x568, 0x56c, 0x580, 0x584 },
	{ 0x75c, 0x760, 0x764, 0x768, 0x76c, 0x780, 0x784 },
};

static void mipi_lane_pre_oe(int on)
{
	for (unsigned l = 0; l < 6; l++)
		wr32(0x130a0000UL + mipi_lane_banks[l][5], on ? 1u : 0u);
}

static void mipi_lane_oe(int on)
{
	for (unsigned l = 0; l < 6; l++)
		wr32(0x130a0000UL + mipi_lane_banks[l][6], on ? 1u : 0u);
}

static void mipi_lane_dpn(int on)
{
	for (unsigned l = 0; l < 6; l++) {
		wr32(0x130a0000UL + mipi_lane_banks[l][3], on ? 1u : 0u);
		wr32(0x130a0000UL + mipi_lane_banks[l][4], on ? 1u : 0u);
	}
}

static void mipi_lane_sw_ctl(int on)
{
	for (unsigned l = 0; l < 6; l++)
		wr32(0x130a0000UL + mipi_lane_banks[l][0], on ? 0x101u : 0u);
}

/* pan15: mt6993 mutex on the real bases, from the ddp node's reg list
 * (disp1-mutex0@3e320000: main 0x3e320000, side 0x3e720000, ovlsys0
 * 0x32920000, ovlsys1 0x32c20000 - the ovl bases pan1-14 guessed as
 * +0x2000 were off by 0x1e000). Layout per mutex n, stride 0x20
 * (mtk_drm_ddp.h): INTEN +0x00, EN +0x20, RST +0x2c, SOF +0x30,
 * MOD0 +0x34, MOD1 +0x38. mutex0/DSI0 (mt6993_ddp_driver_data):
 * SOF is MODE-DEPENDENT and this is the pan41 correction. pan15-40
 * wrote 0x81 = SOF_DSI0 | EOF_DSI0, the *video-mode* value. mt6993
 * sets qos_sideband_use_eof = true, so mtk_disp_mutex_src_set()
 * (mtk_drm_ddp.c:36580) takes the is_cmd_mode branch and selects
 * DDP_MUTEX_SOF_SINGLE_EOF_DSI0 => mutex_sof[] = SINGLE_MODE(0) |
 * EOF_DSI0(0x80) = 0x80. 0x81 asks the mutex to wait for a DSI0 SOF
 * event that command mode never produces, so the frame never gets
 * its release; LK's own leftover at +0x30 was 0x80 for exactly this
 * reason. main MOD0 DLO_ASYNC0 BIT(24), MOD1
 * DSI0_MAC0 BIT(0)|BIT(31); ovlsys MOD0 EXDMA0 BIT(0)|BLENDER0 BIT(8);
 * INTEN = (1<<0)|(1<<8) (DISP_MUTEX_TOTAL_v2 = 8). mtk_disp_mutex_enable
 * also writes EN=1 to side and ovlsys1 without mods. */
static void display_mutex_kernel(void)
{
	static const uint64_t base[4] = {
		0x3e320000UL, 0x3e720000UL, 0x32920000UL, 0x32c20000UL,
	};
	static const char pre[4] = { 'A', 'B', 'C', 'D' };
	static const uint32_t offs[6] = { 0x00, 0x20, 0x2c, 0x30, 0x34, 0x38 };
	char tag[4] = "mk";

	for (unsigned b = 0; b < 4; b++) {
		tag[2] = pre[b];
		for (unsigned j = 0; j < 6; j++)
			dump_line(tag, offs[j], rd32(base[b] + offs[j]));
	}
	wr32(0x3e320000UL + 0x30, 0x80u);			/* SOF: cmd-mode */
	/* pan19: every bank's MOD set now names the modules this shim
	 * actually runs (mt6993_mutex_mod bit table, v1/mtk_drm_ddp.c):
	 *   main  MOD0 = DLI_ASYNC0(bit8) | DLO_ASYNC0(bit24), MOD1 = 0.
	 *     pan16 wrote DSI0's 0x80000001 into MAIN MOD1 - the table puts
	 *     DSI0_MAC0 in the SIDE bank, so that write armed a module the
	 *     main mutex can never see.
	 *   ovlsys0 MOD0 = EXDMA0(0) | BLENDER0(8) | OUTPROC0(16); MOD1 =
	 *     OVL_DLO_ASYNC12 = BIT(16)|BIT(31). LK's 0x17e18 armed
	 *     EXDMA3/4 + BLENDER0-5 + OUTPROC0 - a set our EXDMA0 layer was
	 *     never part of, so the ovlsys mutex waited on engines that do
	 *     not run (pan18: BUSY, no frame).
	 *   side  MOD0 = DLI_ASYNC20(bit0); MOD1 = DSI0_MAC0 = BIT(0)|BIT(31).
	 *     Never configured before (the EN write did not even stick in
	 *     pan16) - DSI0's frame-done lives here, so no release was
	 *     ever possible.
	 * Historical baseline below wrote BIT31 into MOD1. pan46 stage 4
	 * corrects that: BIT31 is a SOFTWARE register-selector flag, stripped
	 * by mtk_disp_mutex_add_comp; it is not a hardware commit/valid bit. */
	wr32(0x3e320000UL + 0x34, 0x01000100u);		/* MOD0: DLI0|DLO0 */
	wr32(0x3e320000UL + 0x38, 0);				/* MOD1: none */
	wr32(0x3e320000UL + 0x20, 0);
	wr32(0x3e320000UL + 0x20, 1);				/* EN */
	wr32(0x3e320000UL + 0x00, 0x101u);			/* INTEN */
	wr32(0x32920000UL + 0x30, 0x80u);
	/* Retained pan47 baseline for the staged comparison. Compiled stock
	 * assigns bit15 to BLENDER7, while virtual EXDMA_OUT_CB3 has no bit.
	 * Stage 3 replaces this historical membership with the actual six-stage
	 * single-source graph (0x17e08); stage 4 separately strips MOD1 BIT31. */
	wr32(0x32920000UL + 0x34, 0x0001fe18u);		/* historical MOD0: EXDMA3/4, BL1..7, OUTPROC0 */
	wr32(0x32920000UL + 0x38, 0x00010000u);		/* MOD1: DLO_ASYNC12 */
	wr32(0x32920000UL + 0x20, 0);
	wr32(0x32920000UL + 0x20, 1);
	wr32(0x32920000UL + 0x00, 0x101u);
	wr32(0x3e720000UL + 0x30, 0x80u);			/* side: DSI0 lives here */
	wr32(0x3e720000UL + 0x34, 0x00000001u);		/* MOD0: DLI_ASYNC20 */
	/* pan20: MOD1 = DSI0_MAC0 | DSC_WRAP0_CORE0 (BIT(14)|BIT(31) in the
	 * table). LK's leftover 0x4b81 carried bit14 - the CORE0 mutex bit,
	 * matching the config+PPS LK left in core0 - and no SPLITTER0(bit12):
	 * follow that set, not the fuller table. */
	wr32(0x3e720000UL + 0x38, 0x80004001u);		/* MOD1: DSI0_MAC0|DSC_CORE0 */
	wr32(0x3e720000UL + 0x20, 0);
	wr32(0x3e720000UL + 0x20, 1);
	wr32(0x3e720000UL + 0x00, 0x101u);
	wr32(0x32c20000UL + 0x20, 1);				/* ovlsys1: enable only */
	__asm__ volatile("dsb sy" ::: "memory");
	/* pan15 logged all four banks under one shared "mL" tag, so bank
	 * attribution was lost to ECC; per-bank tags from pan16 on. */
	for (unsigned b = 0; b < 4; b++) {
		tag[1] = 'n';
		tag[2] = pre[b];	/* pan44: formerly all four banks logged as mnD */
		for (unsigned j = 0; j < 6; j++)
			dump_line(tag, offs[j], rd32(base[b] + offs[j]));
	}
	plog_str("mutex kernel done\n");
}

/* pan16: mtk_dsi_tx_buf_rw, the one preconfig piece pan13-15 never
 * replicated (mt6993 dsi_buffer=true; both enable paths call it right
 * after rxtx_control, before video mode is set). Offsets from
 * dsi_buf_con_base 0x300 + con_offset 4: BUF_CON0 0x300, BUF_CON1
 * 0x304, BUF_SIZE 0x30c, RW_TIMES 0x314, SODI 0x318/0x31c, PREULTRA
 * 0x328/0x32c, ULTRA 0x330/0x334, URGENT 0x338/0x33c, PREURGENT_HIGH
 * 0x340; DEBUG_SEL 0x274, RESERVED 0x3f8. Every threshold derives from
 * the live BUF_SIZE readback (DSI_BUF_REAL_SIZE [14:0]) with the
 * kernel's exact u32 math - hsa_byte's 0xfffffffa wrap included.
 * fill_rate/compress_rate/mmsys_clk are computed upstream but never
 * consumed in the MT6993 branch, so they are dropped here. */
static void display_tx_buf(uint64_t dsi)
{
	const uint32_t ps_wc = 1256u, lanes = 4u;
	uint32_t buf_con = rd32(dsi + 0x30c) & 0x7fffu;

	plog_hex("buf con", buf_con);
	wr32(dsi + 0x300, rd32(dsi + 0x300) | 0x1u);		/* BUF_BUF_EN */
	wr32(dsi + 0x274, rd32(dsi + 0x274) | 0x400u);		/* MM_RST_SEL */
	wr32(dsi + 0x3f8, rd32(dsi + 0x3f8) & ~(1u << 29));	/* VDE_BLOCK_ULTRA */

	/* line_time (get_line_time_vdo, DPHY sync pulse, +data_phy_cycle:
	 * non-cont clock panel, so vdo_keep_hs_perline is 0). */
	uint32_t hsa_byte = 2u * 2u - 10u;	/* 0xfffffffa, wrap intended */
	uint32_t line_time = 32u + hsa_byte + 32u + ps_wc + 410u;
	line_time = (line_time + lanes - 1u) / lanes + 51u;
	uint32_t image_time = (ps_wc + lanes - 1u) / lanes;
	uint32_t consume_rate =
		828u * lanes * image_time * 1000u / (8u * 32u * line_time);
	plog_hex("consume rt", consume_rate);

	uint32_t ultra_lo_us = (buf_con * 7000u + consume_rate * 10u - 1u)
			       / (consume_rate * 10u);
	if (ultra_lo_us < 35u)
		ultra_lo_us = 35u;
	uint32_t ultra_hi_us = ultra_lo_us + 1u;
	uint32_t v;
	v = (ultra_hi_us * consume_rate + 999u) / 1000u;
	uint32_t ultra_hi = v < buf_con ? v : buf_con;
	v = (ultra_lo_us * consume_rate + 999u) / 1000u;
	uint32_t ultra_lo = v < buf_con ? v : buf_con;
	v = (31u * consume_rate + 999u) / 1000u;
	uint32_t urgent_hi = v < buf_con ? v : buf_con;
	v = (30u * consume_rate + 999u) / 1000u;
	uint32_t urgent_lo = v < buf_con ? v : buf_con;

	wr32(dsi + 0x304, rd32(dsi + 0x304) & ~0x7fffu);	/* output_valid 0 in vdo */
	wr32(dsi + 0x318, buf_con + 1u);			/* sodi_hi */
	wr32(dsi + 0x31c, buf_con);				/* sodi_lo */
	wr32(dsi + 0x328, buf_con + 1u);			/* preultra_hi */
	wr32(dsi + 0x32c, buf_con);				/* preultra_lo */
	wr32(dsi + 0x330, ultra_hi);
	wr32(dsi + 0x334, ultra_lo);
	wr32(dsi + 0x338, urgent_hi);
	wr32(dsi + 0x33c, urgent_lo);
	wr32(dsi + 0x314, ps_wc * 2760u / 16u);			/* rw_times, in_width 16 */

	/* preurgent, vdo line-counter mode (PREURGENT_SUPPORT_VDO). Both
	 * non-degenerate kernel branches assign prefetch - buf_preurgent. */
	uint32_t line_time_ns = line_time * 100000u / (828u * 100u / 8u);
	uint32_t buf_pu = (31000u + line_time_ns - 1u) / line_time_ns;
	int prefetch = 36;	/* vtotal 2848 - vsync_start 2812 (vsa14+vbp22) */
	if (prefetch - (int)buf_pu > 0)
		buf_pu = (uint32_t)(prefetch - (int)buf_pu);
	else
		buf_pu = 0;
	wr32(dsi + 0x300, rd32(dsi + 0x300) & ~(1u << 21));	/* line counter */
	wr32(dsi + 0x340, buf_pu);
	wr32(dsi + 0x300, rd32(dsi + 0x300) & ~(1u << 3));
	wr32(dsi + 0x300, rd32(dsi + 0x300) | (1u << 20));	/* PREURGENT_EN */
	for (unsigned i = 0; i < 17; i++)
		dump_line("tb", 0x300 + i * 4, rd32(dsi + 0x300 + i * 4));
	dump_line("tb", 0x274, rd32(dsi + 0x274));
	dump_line("tb", 0x3f8, rd32(dsi + 0x3f8));
}

/* pan37: the cmd-mode frame trigger.
 *
 * MT6993 has no video mode at all — mtk_dsi_probe (mtk_dsi.c:17103)
 * forces cmd mode and DSI_CMD_V2 for mmsys_id == MMSYS_MT6993. Every
 * earlier pan drove SYNC_PULSE video mode and called mtk_dsi_start(),
 * which is the branch the kernel takes only when NOT in cmd mode
 * (mtk_dsi.c:6561). That is the root cause of "backlight on, no
 * colour": the DSI engine was never running the path this panel has.
 *
 * The kernel's cmd-mode per-frame trigger (mtk_dsi.c:8576,
 * MTK_TRIG_FLAG_SOF / trigger_without_cmdq) is exactly:
 *   MEM_CONTI (+0x48) = 0x3c          already done in display_video_start
 *   CMDQ0     (+0x400) = 0x002c3909   short packet, HS, BTA, len 0x2c
 *   CMDQ_CON  (+0x44) = 1 | SIZE_SEL
 *   START     (+0x00) = 0 -> 1 -> 0
 * and the payload is fetched by the engine from the internal
 * "memory continuous" buffer (MEM_CONTI). The DDP pipeline
 * (EXDMA -> blender -> outproc -> DLO -> splitter -> DSC -> merge ->
 * DSI) is what fills that buffer; the trigger only asks the DSI to
 * push what is already there.
 *
 * 0x002c3909 = config 0x09 (SHORT|HSTX|BTA) | type 0x39 (DCS long
 * write) | count 0x2c. The identical literal appears in the BDG
 * self-test bridge (mtk_disp_bdg.c:1913), so it is a fixed hardware
 * idiom, not a value derived from this panel.
 */
/* pan46: one final image, bounded and cumulative mutex experiments. The
 * baseline stage does not re-arm. Later stages opt into the actual per-frame
 * command-mode enable contract (mtk_disp_mutex_enable_cmdq), not RESET +0x2c. */
static unsigned frame_mutex_rearm, frame_good_count;
static uint32_t frame_last_status, frame_last_wait;

static void display_mutex_frame_enable(void)
{
	static const uint64_t base[] = {
		0x3e320000UL, 0x32920000UL, 0x3e720000UL, 0x32c20000UL,
	};
	for (unsigned i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
		wr32(base[i] + 0x08, rd32(base[i] + 0x08) & ~0x1u);
		wr32(base[i] + 0x20, 0);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(base[i] + 0x20, 1);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void display_cmd_frame_trigger(uint64_t dsi)
{
	uint32_t first_status, last_status;
	unsigned t;
	/* The engine must be in CMD mode (MODE_CTRL.MODE == 0), not the
	 * SYNC_PULSE video mode the earlier pans wrote. */
	wr32(dsi + 0x34, rd32(dsi + 0x34) & ~0x3u);
	/* MEM_CONTI: "need to config for cmd mode to transmit frame data
	 * to DDIC" (mtk_dsi_rxtx_control, mtk_dsi.c:2815). */
	wr32(dsi + 0x48, 0x3cu);
	/* Frame packet. Same V2 encoding dsi_cmdq_send_k uses, but the
	 * count field is the fixed 0x2c the kernel writes, not a real
	 * payload length — the engine streams from MEM_CONTI. */
	wr32(dsi + 0x400, 0x002c3909u);
	wr32(dsi + 0x44, (rd32(dsi + 0x44) & ~(0x3ffu | 0x8000u))
			  | 1u | 0x8000u);
	/* Status clearing is common to every stage. The driver's ~status write
	 * clears old latched interrupt bits; an old FRAME_DONE cannot win a test. */
	wr32(dsi + 0x0c, ~rd32(dsi + 0x0c));
	if (frame_mutex_rearm)
		display_mutex_frame_enable();
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(dsi + 0x00, rd32(dsi + 0x00) & ~0x1u);
	wr32(dsi + 0x00, rd32(dsi + 0x00) | 0x1u);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(dsi + 0x00, rd32(dsi + 0x00) & ~0x1u);
	/* An idle read alone does not establish that START was accepted.
	 * Capture the first/final status and bounded wait length without
	 * logging in the timing-sensitive START pulse. */
	first_status = rd32(dsi + 0x0c);
	last_status = first_status;
	for (t = 0; t < 200; t++) {
		last_status = rd32(dsi + 0x0c);
		if (!(last_status & 0x80000000u))
			break;
		wait_ms(1);
	}
	dump_line("tr0", 0x0c, first_status);
	dump_line("tr1", 0x0c, last_status);
	dump_line("trw", 0x00, t);
	frame_last_status = last_status;
	frame_last_wait = t;
}

/* pan17: the ovlsys pixel source. Bases from mt6993.dts: EXDMA0
 * 0x32930000, BLENDER0 0x329b0000, ovl0_dlo_async12 0x32900000
 * (mmsys0 routing is 0x3e300000). Offsets from
 * platform/mtk_ovl_exdma_reg.h / mtk_ovl_blender_reg.h /
 * mtk_disp_dlo_async.c: EN +0x20, RST +0x24, ROI +0x30, L0 block
 * +0x40..0x54, L0_PITCH 0x2f4, L0_ADDR 0xf40; DLO relay size
 * w|h<<16 at 0xc00 + alias*4. LK's ovlsys0 mutex MOD0 leftover
 * (0x17e18) says its logo pipeline ran much wider than our minimal
 * EXDMA0|BLENDER0 - these dumps show what it left behind. */
static void display_ovlsys_probe(void)
{
	static const uint64_t ex = 0x32930000UL, bl = 0x329b0000UL,
			      dl = 0x32900000UL;

	for (unsigned i = 0; i < 22; i++)
		dump_line("ex", 0x00 + i * 4, rd32(ex + i * 4));
	for (unsigned i = 0; i < 7; i++)
		dump_line("ex", 0x2ec + i * 4, rd32(ex + 0x2ec + i * 4));
	dump_line("ex", 0xf40, rd32(ex + 0xf40));
	for (unsigned i = 0; i < 22; i++)
		dump_line("bl", 0x00 + i * 4, rd32(bl + i * 4));
	dump_line("bl", 0x100, rd32(bl + 0x100));
	dump_line("bl", 0x104, rd32(bl + 0x104));
	dump_line("bl", 0x208, rd32(bl + 0x208));
	dump_line("dl", 0xbd0, rd32(dl + 0xbd0));
	dump_line("dl", 0xbd4, rd32(dl + 0xbd4));
	for (unsigned i = 0; i < 3; i++)
		dump_line("dl", 0xc00 + i * 4, rd32(dl + 0xc00 + i * 4));
	dump_line("dl", 0xc30, rd32(dl + 0xc30));
	/* ovlsys0 routing words (mtk_disp_ovlsys_reg.h mt6993): AID
	 * 0xE0C+, OUTPROC crossbars 0xEB8+, EXDMA crossbars 0xEC8+,
	 * BLENDER crossbars 0xF40+. LK's leftovers here decide whether
	 * pan19 must write routing or can keep standing on them. */
	for (unsigned i = 0; i < 9; i++)
		dump_line("or", 0xe0c + i * 0x10, rd32(dl + 0xe0c + i * 0x10));
	for (unsigned i = 0; i < 8; i++)
		dump_line("or", 0xeb8 + i * 4, rd32(dl + 0xeb8 + i * 4));
	for (unsigned i = 0; i < 29; i++)
		dump_line("or", 0xec8 + i * 4, rd32(dl + 0xec8 + i * 4));
	for (unsigned i = 0; i < 12; i++)
		dump_line("or", 0xf40 + i * 4, rd32(dl + 0xf40 + i * 4));
	/* pan19: the shadow/crossbar-con words every kernel connection
	 * writes first (MT6993_OVLSYS_BYPASS_MUX_SHADOW 0x400,
	 * MT6993_OVLSYS_CROSSBAR_CON 0x408). */
	dump_line("or", 0x400, rd32(dl + 0x400));
	dump_line("or", 0x408, rd32(dl + 0x408));

	/* pan19: LK's own chain leftovers. Its ovlsys0 mutex MOD0 0x17e18
	 * names EXDMA3/4 + BLENDER0-5 + OUTPROC0 (not our EXDMA0), so its
	 * layer config may still sit in EXDMA3 0x32960000 - including the
	 * 0xfc910000 L0_ADDR. OUTPROC0 0x32a30000 has never been touched by
	 * any pan; BLENDER2 0x329d0000 / BLENDER6 0x32a10000 carry the
	 * routes LK left (0xef0=EXDMA4->BLENDER2, 0xf58=BLENDER6->OUTPROC0). */
	{
		static const uint64_t e3 = 0x32960000UL, op = 0x32a30000UL,
				      b2 = 0x329d0000UL, b6 = 0x32a10000UL;
		static const uint32_t e3o[8] = {
			0x000, 0x008, 0x020, 0x030, 0x048, 0x050, 0x2f4, 0xf40,
		};

		for (unsigned i = 0; i < 8; i++)
			dump_line("e3", e3o[i], rd32(e3 + e3o[i]));
		dump_line("b2", 0x020, rd32(b2 + 0x020));
		dump_line("b2", 0x030, rd32(b2 + 0x030));
		dump_line("b6", 0x020, rd32(b6 + 0x020));
		dump_line("b6", 0x030, rd32(b6 + 0x030));
		{
			static const uint32_t opo[7] = {
				0x000, 0x008, 0x010, 0x020, 0x024, 0x028, 0x030,
			};

			for (unsigned i = 0; i < 7; i++)
				dump_line("op", opo[i], rd32(op + opo[i]));
		}
	}
	/* pan19: main-side routing words, never dumped before. dispsys0
	 * 0x3e300000 (shadow 0x240, DLI0 size 0xb90, DLO0 size 0xbd0,
	 * PQ_IN_CROSSBAR0 0xf34, PQ_OUT_CROSSBAR3 0xee4) and dispsys1
	 * 0x3e700000 (shadow: active-file 0x2e0 / v1-file 0xcf8 - dump
	 * both, the fork is unresolved; DLI20 size 0xb98, SPLITTER_IN_CB0
	 * 0xe2c, SPLITTER_OUT_CB0 0xdc4, COMP_OUT_CB0 0xee4,
	 * MERGE_OUT_CB0 0xe6c). */
	{
		static const uint64_t m0 = 0x3e300000UL, m1 = 0x3e700000UL;
		static const uint32_t m0o[5] = {
			0x240, 0xb90, 0xbd0, 0xf34, 0xee4,
		};
		static const uint32_t m1o[9] = {
			0x2e0, 0xcf8, 0xb98, 0xe2c, 0xdc4, 0xee4, 0xe6c,
			0xefc, 0xf00,
		};

		for (unsigned i = 0; i < 5; i++)
			dump_line("mr0", m0o[i], rd32(m0 + m0o[i]));
		for (unsigned i = 0; i < 9; i++)
			dump_line("mr1", m1o[i], rd32(m1 + m1o[i]));
	}
	/* pan19: the DSC wrap 0x3e7f0000 (disp1-dsc-wrap0). The panel is
	 * DSC-compressed (ext-params: 2x628 slices, bpp 8.0); pan19 hardware
	 * found core0 fully configured AND enabled by LK (CON 0x000 =
	 * 0x00010081 - DSC_EN BIT(0) set, MODE 0x030 = 0x00010001 - the
	 * dsc_param_load_mode==1 encoding with slice_mode 1) with the panel
	 * PPS at 0x80, core1 (+0x400) blank. LK's side-mutex leftover
	 * 0x4b81 carries BIT(14) = DSC_WRAP0_CORE0 (not CORE1 - 0x4b81's
	 * set bits are 0,7,8,9,11,14). pan20 routes through this core. */
	{
		static const uint64_t dsc = 0x3e7f0000UL;
		static const uint32_t co[11] = {
			0x000, 0x004, 0x008, 0x018, 0x01c,
			0x020, 0x024, 0x028, 0x030, 0x034, 0x228,
		};

		for (unsigned c = 0; c < 2; c++) {
			char tag[4] = "ds";

			tag[2] = '0' + (char)c;
			for (unsigned i = 0; i < 11; i++)
				dump_line(tag, co[i], rd32(dsc + c * 0x400 + co[i]));
			for (unsigned i = 0; i < 6; i++)
				dump_line(tag, 0x80 + i * 4,
					  rd32(dsc + c * 0x400 + 0x80 + i * 4));
		}
	}
}

/* pan40: the SMI LARB that sits between EXDMA3 and DRAM.
 *
 * pan39 finally wrote the kernel's three fetch-enable functions and the
 * log proves they landed: RDMA0_CTRL (0x100) = 1, DUMMY_REG (0x200) =
 * 0xC, GDRDY_PRD (0x208) = 0xFFFFFF, GMC_SETTING1 = 0x3ff03ff,
 * FIFO_CTRL = 0x06000000. And the mutex is demonstrably producing
 * frames: GCE events disp_mutex0_sof0 (0x11e), disp_mutex0_eof (0x12e)
 * and disp_ovlsys0_sof0 (0x75) all read 1.
 *
 * Yet GREQ_LAYER_CNT (0x234) is still 0 and RDMA0_CTRL bits 27:16
 * (FIFO_USED_SIZE) are still 0 — the read buffer never holds a byte.
 * A G-REQ that is enabled but never appears means the request is being
 * dropped before it reaches the fabric. The master for EXDMA3 is
 * `mediatek,larb = <&smi_larb1 SMMU_L1_P2_OVL_EXDMA3>` (mt6993.dts
 * disp-ovl0-exdma3@32960000), and this shim has never written a single
 * SMI/LARB register (0 occurrences of "larb" or 0x32ab0000 anywhere in
 * display.c). That is the missing layer.
 *
 * What the kernel does on larb resume (drivers/memory/mtk-smi.c):
 *   mtk_smi_larb_resume() -> larb_gen->config_port(dev)
 *     = mtk_smi_larb_config_port_gen2_general() (:813)
 *       for_each_set_bit(i, larb->mmu):
 *         NONSEC_CON(i) |= F_MMU_EN(BIT0) | BANK_SEL(bank[i])
 *       then the mt6993 misc table for larb1:
 *         CMD_THRT_CON 0x24 = 0x370456
 *         SW_FLAG      0x40 = 0x1
 *   The SW_FLAG write is the "dummy write" the driver reads back
 *   (:4262); if the LARB clock is off the write silently fails.
 *
 * Register offsets, same file:
 *   SMI_LARB_NONSEC_CON(id) = 0x380 + id*4
 *   SMI_LARB_CMD_THRT_CON   = 0x24
 *   SMI_LARB_SW_FLAG        = 0x40
 *   F_MMU_EN                = BIT(0)
 *   BANK_SEL(id)            = (id&3)<<8 | (id&3)<<10 | (id&3)<<12 | (id&3)<<14
 *
 * EXDMA3's ports in larb1 (include/dt-bindings/memory/mt6993-larb-port.h):
 *   P0 = OVL_EXDMA3_HDR, P2 = OVL_EXDMA3 (the pixel read), P4/P6 = stash.
 * We only drive the main plane, so P2 is the one that must be non-secure
 * and MMU-enabled; P0/P4/P6 are set too because a half-configured LARB
 * can stall the whole port group.
 */
static void display_smi_larb1_enable(void)
{
	const uint64_t larb1 = 0x32ab0000UL;	/* smi-larb1@32ab0000 */
	static const unsigned ports[4] = { 0, 2, 4, 6 };

	/* Pre-state: the "dummy write" check the kernel performs reads
	 * SW_FLAG back after writing it, so log it first. */
	dump_line("sm0", 0x40, rd32(larb1 + 0x40));
	dump_line("sm0", 0x24, rd32(larb1 + 0x24));
	for (unsigned i = 0; i < 4; i++)
		dump_line("sm0", 0x380 + ports[i] * 4, rd32(larb1 + 0x380 + ports[i] * 4));

	/* config_port_gen2_general: F_MMU_EN | BANK_SEL(bank). larb1 has
	 * bank 0 for these ports (single-bank LARB), so BANK_SEL(0) = 0. */
	for (unsigned i = 0; i < 4; i++) {
		uint32_t off = 0x380 + ports[i] * 4;
		wr32(larb1 + off, rd32(larb1 + off) | 0x1u);
	}

	/* The mt6993 misc table for LARB1, verbatim. */
	wr32(larb1 + 0x24, 0x370456u);		/* SMI_LARB_CMD_THRT_CON */
	wr32(larb1 + 0x40, 0x1u);		/* SMI_LARB_SW_FLAG (dummy) */
	__asm__ volatile("dsb sy" ::: "memory");

	/* The kernel treats SW_FLAG reading back 0 as a hard failure
	 * ("larb%u write dummy fail"). Report the same way. */
	dump_line("sm1", 0x40, rd32(larb1 + 0x40));
	dump_line("sm1", 0x24, rd32(larb1 + 0x24));
	for (unsigned i = 0; i < 4; i++)
		dump_line("sm1", 0x380 + ports[i] * 4, rd32(larb1 + 0x380 + ports[i] * 4));
	plog_hex("larb1 swflag ok", rd32(larb1 + 0x40) ? 1u : 0u);
}

/* pan39: the kernel's three DRAM-fetch enable steps, verbatim.
 *
 * pan1..pan38 configured EXDMA3's layer and set EN|FORCE_RELAY, then
 * triggered the DSI. GREQ_LAYER_CNT (0x234) never left 0. The cause is
 * not topology, SMI, MTCMOS or the DSI — it is that none of these three
 * functions ever ran:
 *
 *   mtk_ovl_exdma_start()          mtk_disp_ovl_exdma.c:825
 *   mtk_ovl_exdma_golden_setting() mtk_disp_ovl_exdma.c:3902 (!is_dc)
 *   mtk_ovl_exdma_layer_on()       mtk_disp_ovl_exdma.c:1084
 *
 * pan38's own log proves it: start() writes GDRDY_PRD (0x208) =
 * 0xFFFFFFFF unconditionally, and pan38 read back 0x00000000. At the
 * same time RDMA0_BUF_HIGH (0x220) read 0x80000000, which is exactly
 * golden_setting's !is_dc result (PRE_ULTRA_HIGH_DIS set, threshold 0)
 * — LK had run the golden setting on this engine; we had not.
 *
 * RDMA0_CTRL (0x100) = 1 in layer_on() is the real fetch enable.
 * L0_EN (0x40) only gates the layer into the blender; it does not start
 * a G-REQ. That is why 38 rounds of correct-looking layer config never
 * moved a single byte from DRAM.
 *
 * Values are the mt6993 driver data (mtk_disp_ovl_exdma.c:5912,
 * mt6993_ovl_exdma_driver_data) run through the !is_dc branch of
 * mtk_ovl_exdma_cal_golden_setting() (:3862), and the mt6993 register
 * table (platform/mtk_ovl_exdma_reg.h, ovl_exdma_regs_mt6993):
 *   fifo_size 1536, issue_req_th_dl 511, issue_req_th_urg_dl 255,
 *   greq_num_dl 0xFF, apb_mcyc_rd 0.
 */
static void display_exdma_start_kernel(uint64_t ex)
{
	/* --- mtk_ovl_exdma_start() --- */
	wr32(ex + 0x024, rd32(ex + 0x024) | 0x1u);	/* RST bit0 */
	wr32(ex + 0x024, rd32(ex + 0x024) & ~0x1u);
	wr32(ex + 0x008, 0);				/* INTSTA clear */
	wr32(ex + 0x020, rd32(ex + 0x020) | 0x1u);	/* EN: OVL_EN */
	wr32(ex + 0x020, rd32(ex + 0x020) | 0x10u);	/* EN: FORCE_RELAY */

	/* RDMA_BURST_CON1 0x1F4: BURST16_EN(28) | DDR_EN(30) | DDR_ACK_EN(31).
	 * disp_helper stage is NORMAL for us, so both DDR bits are 1. Without
	 * DDR_ACK_EN the read master never gets a grant acknowledgement and
	 * the G-REQ sits unserved. */
	wr32(ex + 0x1f4, rd32(ex + 0x1f4) | (1u << 28) | (1u << 30) | (1u << 31));

	/* DUMMY_REG 0x200 = DISP_OVL_EXT_DDR_EN_OPT(BIT2) |
	 * DISP_OVL_FORCE_EXT_DDR_EN(BIT3). Written with mask ~0. */
	wr32(ex + 0x200, 0x0u | (1u << 2) | (1u << 3));

	/* DATAPATH_CON 0x014: LAYER_SMI_ID_EN(BIT0) | GCLAST_EN(BIT24) |
	 * HDR_GCLAST_EN(BIT25), masked write. */
	wr32(ex + 0x014, rd32(ex + 0x014) | 0x1u | (1u << 24) | (1u << 25));

	/* MOUT 0xFF0 = 2 (mask 0x3). The non-addon branch of start(). */
	wr32(ex + 0xff0, (rd32(ex + 0xff0) & ~0x3u) | 0x2u);

	/* GDRDY_PRD 0x208 = 0xFFFFFFFF — the register pan38 read back as 0. */
	wr32(ex + 0x208, 0xffffffffu);

	/* --- mtk_ovl_exdma_golden_setting(), !is_dc branch --- */
	/* GMC_SETTING1 0x108: ULTRA_TH 0x3ff | PRE_ULTRA_TH 0x3ff<<16 */
	wr32(ex + 0x108, 0x3ffu | (0x3ffu << 16));
	/* FIFO_CTRL 0x110: FIFO_THRD 0 | FIFO_SIZE 1536<<16 */
	wr32(ex + 0x110, 0u | (1536u << 16));
	/* GMC_SETTING2 0x1e0: ISSUE_REQ_TH 511 | URG 255<<16 |
	 * REQ_TH_PRE_ULTRA 0<<28 | REQ_TH_ULTRA 1<<29 | FORCE_REQ_TH 0<<30 */
	wr32(ex + 0x1e0, 511u | (255u << 16) | (1u << 29));
	/* GREQ_NUM 0x1F8 = 0xF1FF00FF | greq_num_dl(0xFF) */
	wr32(ex + 0x1f8, 0xf1ff00ffu | 0xffu);
	/* GREQ_URG_NUM 0x1FC = greq_num_dl */
	wr32(ex + 0x1fc, 0xffu);
	/* ULTRA_SRC 0x20C = 0x8040 (!is_dc) */
	wr32(ex + 0x20c, 0x8040u);
	/* BUF_LOW 0x210: ULTRA_LOW_TH 0 | PRE_ULTRA_LOW_TH 0<<12 */
	wr32(ex + 0x210, 0);
	/* BUF_HIGH 0x220: PRE_ULTRA_HIGH_TH 0<<12 | PRE_ULTRA_HIGH_DIS 1<<31.
	 * pan38 read exactly this value — LK's own, not ours. */
	wr32(ex + 0x220, (0u << 12) | (1u << 31));
	/* EN_CON 0x00C: BLOCK_EXT_ULTRA(18) | BLOCK_EXT_PRE_ULTRA(19) |
	 * APB_MCYC_RD(23), all 0 in !is_dc; mask 0x23<<18. */
	wr32(ex + 0x00c, rd32(ex + 0x00c) & ~(0x23u << 18));

	/* --- mtk_ovl_exdma_layer_on() --- */
	/* L0_EN 0x40 = layer_src in FLD_L0_LAYER_SRC (bits 9:8). A DRAM
	 * layer has layer_src 0, so this leaves the field clear and only
	 * the EN bit matters. Kept explicit because the kernel writes the
	 * field, not the bit. */
	wr32(ex + 0x40, rd32(ex + 0x40) & ~(0x3u << 8));
	/* RDMA0_CTRL 0x100 = 1 — THE fetch enable every earlier pan missed. */
	wr32(ex + 0x100, 1u);

	__asm__ volatile("dsb sy" ::: "memory");
}

/* pan18: the full kernel-exact EXDMA0/BLENDER0 layer config, every
 * value from mtk_disp_ovl_exdma.c / mtk_disp_ovl_blender.c and the
 * mt6991 reg_fld tables (mt6993 data uses them):
 * EXDMA0 0x32930000 - SHADOW 0x28 bit2 bypass (power-on idiom);
 * ROI 0x30 (h<<16)|w; PITCH 0x2f4 = pitch|CONST_BLD bit28 (PIXEL_NONE
 * constant blend); PITCH_MSB 0x2f0 0; ADDR 0xf40 + ADDR_MSB 0xf4c
 * (34-bit, ours is 0); SRC_SIZE 0x48 (h<<16)|w; CLIP 0x4c 0; CLRFMT
 * 0x50 = 2 (RGBA8888 encoding of DRM_FORMAT_ARGB8888 LE, no swaps);
 * CON2 0x300 = 0x1ff ((alpha_con 1<<8)|alpha 0xff); OFFSET 0x44 0;
 * CLR 0x25c 0xff000000; UNDO_ALPHA 0xff8 clear bits0|4 (no
 * premultiply); EN 0x20 bits0|4 (kernel .config enables EARLY); L0_EN
 * 0x40 bit0 (LAYER_SRC bits 9:8 stay 0 - memory source). AID: ovlsys0
 * 0x32900000 + 0xE0C bit0 clear = non-secure read path.
 * BLENDER0 0x329b0000 - ROI 0x30; BGCLR 0x104 0xff000000; layer
 * mirror: PITCH 0x208, SRC_SIZE 0x48, CLIP 0x4c, CLRFMT 0x50,
 * L0_CON 0x54 CONST_BLD bit24, OFFSET 0x44, L0_EN 0x40 bit0; start:
 * RST 0x24 pulse, INTSTA 0x8 clear, RELAY_MODE_OPT 0x1c 1, EN 0x20
 * bit0 (bld_enable_mt6993 - pan17 missed the relay word). */
/* pan47: mirror mtk_ovl_blender_connect() across the stock BL1..BL6
 * cascade. Intermediate blenders feed the next blender; BL6 is the
 * final output to OUTPROC0. */
static void display_blender_path_connect(uint64_t first, uint32_t size)
{
	static const uint32_t offsets[] = {
		0x00000u, 0x10000u, 0x20000u,
		0x30000u, 0x40000u, 0x50000u,
	};

	for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		uint64_t bl = first + offsets[i];
		unsigned last = i == sizeof(offsets) / sizeof(offsets[0]) - 1;

		wr32(bl + 0x30, size);
		wr32(bl + 0x24, 1u);
		wr32(bl + 0x24, 0u);
		wr32(bl + 0x08, 0u);
		wr32(bl + 0x1c, 1u);			/* RELAY_MODE_OPT */
		/* Stock connect(): non-first stages consume the previous BG input. */
		wr32(bl + 0x10, (rd32(bl + 0x10) & ~0x31u) |
			(i ? 0x1u : 0u) | (last ? 0x10u : 0x20u));
		wr32(bl + 0x20, rd32(bl + 0x20) | 1u);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void display_ovlsys_layer_config(uint64_t dsi)
{
	/* pan38: the source engine is EXDMA3, not EXDMA0.
	 *
	 * mt6993_mtk_ovlsys_main_bringup (mtk_drm_drv.c:4187) is the real
	 * chain this chip's pixels take:
	 *   EXDMA3 -> BLENDER1 -> EXDMA4 -> BLENDER2 -> EXDMA5 ->
	 *   BLENDER3 -> EXDMA6 -> BLENDER4 -> EXDMA7 -> BLENDER5 ->
	 *   EXDMA2 -> EXDMA_OUT_CB3 -> BLENDER6 -> OUTPROC0 ->
	 *   DLO_ASYNC12 -> EXDMA0
	 * EXDMA0 is the TAIL of that chain (fed by OUTPROC0), which is why
	 * pan37's EXDMA0 counters stayed at zero with every register
	 * correct: it was never the thing reading DRAM.
	 *
	 * LK agrees twice: its surviving ovlsys mutex MOD0 is 0x00017e18
	 * = EXDMA3|EXDMA4|BLENDER0-5|OUTPROC0, and its log says
	 * "videolfb - firstEXDMA = 3".
	 *
	 * Addresses below are read straight out of the LK device tree in
	 * lk_a.img (disp-ovl0-exdmaN@/blenderN@/outprocN@), which is the
	 * authoritative map for this board:
	 *   EXDMA0 0x32930000  EXDMA2 0x32950000  EXDMA3 0x32960000
	 *   EXDMA4 0x32970000  EXDMA5 0x32980000  EXDMA6 0x32990000
	 *   EXDMA7 0x329a0000  BLENDER0 0x329b0000 .. BLENDER6 0x32a10000
	 *   OUTPROC0 0x32a30000  ovl0_dlo_async12 0x32900000
	 * (MT6991 numbers differ by 0x10000 — do not borrow them.)
	 */
	static const uint64_t ex = 0x32960000UL,	/* EXDMA3, the real source */
			      bl = 0x329c0000UL,	/* BLENDER1, EXDMA3's sink */
			      ov = 0x32900000UL, dl = 0x32900000UL;
	const uint32_t size = 1256u | (2760u << 16);	/* 0x0ac804e8 */
	/* pan39: LK's live EXDMA3 pitch is 0x1400 = 5120, with CONST_BLD
	 * (bit 28) CLEAR. pan38 wrote 5120|BIT28; the kernel sets CONST_BLD
	 * only when pixel_blend_mode == DRM_MODE_BLEND_PIXEL_NONE
	 * (mtk_disp_ovl_exdma.c:2973), and LK — which demonstrably drove
	 * this engine to produce the boot logo — did not. Follow the
	 * hardware, not the default. */
	const uint32_t pitch = 5120u;

	/* pan40: SMI LARB1 first. EXDMA3's master port lives here, and a
	 * disabled/secure LARB port drops the G-REQ before the fabric sees
	 * it — which is exactly the pan39 symptom (fetch enabled, mutex
	 * producing SOF/EOF, zero G-REQ). */
	display_smi_larb1_enable();

	/* EXDMA3: shadow bypass, config, layer, early enable. */
	wr32(ex + 0x28, rd32(ex + 0x28) | 0x4u);
	wr32(ex + 0x30, size);
	wr32(ex + 0x20, rd32(ex + 0x20) | 0x1u | 0x10u);
	/* pan39: AID (address-ID / secure-bit) select for EXDMA3 layer 0.
	 * pan38 wrote ovlsys0+0xE2C, which is EXDMA2's AID register — it
	 * was clearing the secure bit on the wrong engine for 38 rounds.
	 * The correct mt6993 value is ovlsys_regs_mt6993
	 * [OVL_EXDMA3_L0_AID_SETTING] = 0xE3C (platform/
	 * mtk_disp_ovlsys_reg.h:117), plus MT6985_OVL_LAYER_OFFEST 0x4 per
	 * layer because mt6993 sets aid_per_layer_setting = true
	 * (mtk_disp_ovl_exdma.c:5931). Layer 0 => 0xE3C + 0, bit0 clear =
	 * non-secure DRAM read. NOTE: the mt6991 table in the same header
	 * uses 0xB30 for this; do not borrow it — the two tables differ. */
	wr32(ov + 0xe3c, rd32(ov + 0xe3c) & ~0x1u);	/* EXDMA3 L0 non-sec */
	wr32(ex + 0x2f0, 0);
	wr32(ex + 0x2f4, pitch);
	wr32(ex + 0xf40, 0xfc910000u);			/* L0_ADDR */
	wr32(ex + 0xf4c, 0);				/* L0_ADDR_MSB */
	wr32(ex + 0x48, size);
	wr32(ex + 0x4c, 0);				/* CLIP */
	wr32(ex + 0x50, 2u);				/* CLRFMT RGBA8888 */
	wr32(ex + 0x300, 0x1ffu);			/* CON2 alpha */
	wr32(ex + 0x44, 0);				/* OFFSET */
	wr32(ex + 0x25c, 0xff000000u);			/* L0_CLR dim */
	wr32(ex + 0xff8, rd32(ex + 0xff8) & ~0x11u);	/* UNDO_ALPHA */
	wr32(ex + 0x40, rd32(ex + 0x40) | 0x1u);	/* L0_EN */

	/* pan39: the three kernel functions that actually put this engine
	 * into DRAM fetch. Every pan through 38 configured the layer and
	 * enabled the block, then triggered the DSI — and GREQ_LAYER_CNT
	 * stayed 0. The reason is that none of them ran:
	 *
	 *   mtk_ovl_exdma_start()        (mtk_disp_ovl_exdma.c:825)
	 *   mtk_ovl_exdma_golden_setting() (:3902, !is_dc branch)
	 *   mtk_ovl_exdma_layer_on()     (:1084)
	 *
	 * The proof is in pan38's own log: start() writes
	 * GDRDY_PRD (0x208) = 0xFFFFFFFF unconditionally, and pan38 read
	 * back 0. Simultaneously RDMA0_BUF_HIGH (0x220) read back
	 * 0x80000000, which is exactly golden_setting's !is_dc value
	 * (PRE_ULTRA_HIGH_DIS set, PRE_ULTRA_HIGH_TH 0) — so LK had run
	 * the golden setting on this very engine, and we had not.
	 *
	 * RDMA0_CTRL (0x100) = 1 in layer_on() is the actual fetch
	 * enable; L0_EN alone only gates the layer into the blender.
	 * Nothing upstream of DRAM (routing, mutex, DSI) can make the
	 * G-REQ counter move without it. */
	display_exdma_start_kernel(ex);

	/* pan39: snapshot the fetch state after start() and layer_on() but
	 * BEFORE any DSI trigger. Comparing this against the "c1" read
	 * after the triggers separates "fetch never enabled" from "fetch
	 * enabled but not triggered": RDMA0_CTRL(0x100) bit0 is static once
	 * layer_on() sets it, while GREQ_LAYER_CNT(0x234) and FIFO_USED
	 * (0x100 bits 27:16) only move if DRAM is actually being pulled. */
	dump_line("c0", 0x100, rd32(ex + 0x100));
	dump_line("c0", 0x234, rd32(ex + 0x234));
	dump_line("c0", 0x208, rd32(ex + 0x208));
	dump_line("c0", 0x1f4, rd32(ex + 0x1f4));
	dump_line("c0", 0x200, rd32(ex + 0x200));
	dump_line("c0", 0x0ff0, rd32(ex + 0xff0));
	dump_line("c0", 0x1e0, rd32(ex + 0x1e0));
	dump_line("c0", 0x110, rd32(ex + 0x110));

	/* BLENDER1 @0x329c0000: config + layer mirror + mt6993 start.
	 * pan39: CONST_BLD (bit 24 of L0_PITCH 0x208 / L0_CON 0x54) is
	 * CLEAR here, matching EXDMA3 and matching the hardware: the
	 * kernel only sets it for DRM_MODE_BLEND_PIXEL_NONE, and our
	 * source layer is an opaque RGBA8888 fill, not a constant-blend
	 * layer. Setting it made the blender treat the layer as constant
	 * colour and discard the fetched pixels. */
	wr32(bl + 0x30, size);
	wr32(bl + 0x104, 0xff000000u);
	wr32(bl + 0x208, pitch & 0xffffu);
	wr32(bl + 0x48, size);
	wr32(bl + 0x4c, 0);
	wr32(bl + 0x50, 2u);
	wr32(bl + 0x54, 0);
	wr32(bl + 0x200, 0x10ffu);			/* BL1 L0_CON2: alpha_en|0xff */
	wr32(bl + 0x44, 0);
	wr32(bl + 0x40, rd32(bl + 0x40) | 0x1u);
	display_blender_path_connect(bl, size);

	wr32(dl + 0xc00, size & 0x0fffffffu);		/* DLO relay */
	wr32(dl + 0xc30, size & 0x0fffffffu);
	__asm__ volatile("dsb sy" ::: "memory");
	wait_ms(200);
	wdt_kick();
	dump_line("e1", 0x000, rd32(ex + 0x000));
	dump_line("e1", 0x008, rd32(ex + 0x008));
	dump_line("e1", 0x020, rd32(ex + 0x020));
	dump_line("e1", 0x030, rd32(ex + 0x030));
	dump_line("e1", 0x048, rd32(ex + 0x048));
	dump_line("e1", 0x050, rd32(ex + 0x050));
	dump_line("e1", 0xf40, rd32(ex + 0xf40));
	dump_line("b1", 0x000, rd32(bl + 0x000));
	dump_line("b1", 0x008, rd32(bl + 0x008));
	dump_line("b1", 0x020, rd32(bl + 0x020));
	dump_line("b1", 0x01c, rd32(bl + 0x01c));
	dump_line("b1", 0x030, rd32(bl + 0x030));
	dump_line("d1", 0xc00, rd32(dl + 0xc00));
	dump_line("d1", 0xc30, rd32(dl + 0xc30));
	dump_line("v5", 0x0c, rd32(dsi + 0x0c));
	dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
}

/* pan19: the routing fabric - the link every pan so far left out. All
 * values from the mt6993 connect path in mtk_drm_ddp.c (writes are
 * read|value; the kernel sets each space's bypass shadow before any
 * route word):
 *   ovlsys0 0x32900000: BYPASS_MUX_SHADOW 0x400 |= 1, CROSSBAR_CON
 *     0x408 |= 0x1FE0000; EXDMA0 out CB 0xEC8 |= BIT0 (->BLENDER0),
 *     BLENDER0 out 0xF40 |= BIT0 (->OUTPROC0), BLENDER6 out 0xF58
 *     route bit cleared (LK's stale route would fight ours on
 *     OUTPROC0's input; its unknown bit16 kept), OUTPROC0 out 0xEB8
 *     |= BIT0 (->DLO_ASYNC12).
 *   OUTPROC0 0x32a30000 start+config (mtk_disp_ovl_outproc.c, mt6991
 *     reg table): RST 0x24 pulse, INTSTA 0x8 clear, EN 0x20 = 1
 *     (relay bit5 clear), DATAPATH_CON 0x10 |= BIT26 (OUTPUT_CLAMP),
 *     ROI 0x30.
 *   dispsys0 0x3e300000: shadow 0x240 |= 0xFF0001; DLI_ASYNC0 size
 *     0xb90, DLO_ASYNC0 size 0xbd0 (both h<<16|w, the async bridges'
 *     only config - mtk_disp_dli/dlo_async.c); PQ_IN_CROSSBAR0 0xf34
 *     |= BIT1 (DLI_ASYNC0 -> PQ0_OUT_CB3, the PQ bypass route of the
 *     DRM_BYPASS_PQ_MT6993 bringup), PQ_OUT_CROSSBAR3 0xee4 |= BIT0
 *     (->DLO_ASYNC0).
 *   dispsys1 0x3e700000: shadow 0x2e0 |= 0xFF0001 (the actively-built
 *     ddp.c uses 0x2e0; the v1 copy says 0xcf8 - dumped, unwritten);
 *     DLI_ASYNC20 size 0xb98; SPLITTER_IN_CB0 0xe2c |= BIT0
 *     (->SPLITTER0_OUT_CB0), then pan20's DSC tail (see below).
 * Every hop between the CBs is a wire: no splitter/comp/merge module
 * config exists in this route. */
/* pan44: disconnect the inherited MDP_RSZ0 output on PQ_IN_CB0.
 * pan43 reads +0xF34 = 0x00030003. BIT0 feeds MDP_RSZ0, which this
 * shim neither configures nor includes in its mutex; BIT1 is our bypass
 * input to PQ_OUT_CB3. Preserve all other fields and routes for this
 * single-register experiment. PQ_OUT_CB3's inherited DLO1 route is
 * deliberately unchanged and is recorded in the path snapshots. */
static void display_pq_bypass_source_select(uint64_t m0)
{
	dump_line("pq0", 0xf34, rd32(m0 + 0xf34));
	wr32(m0 + 0xf34, (rd32(m0 + 0xf34) & ~0x1u) | 0x2u);
	__asm__ volatile("dsb sy" ::: "memory");
	dump_line("pq1", 0xf34, rd32(m0 + 0xf34));
}

/* pan45: in the stock full-PQ graph PQ_OUT_CB3 -> DLO1 -> DLI21
 * feeds CHIST1 (mtk_drm_drv.c). This shim instead sends the bypass
 * frame through DLO0 -> DLI20 and does not configure/synchronise DLO1.
 * pan44 reads +0xEE4 = 3 twice. Disconnect only that inherited consumer;
 * keep every other field, including the pan44 PQ input selection. */
static void display_pq_bypass_output_select(uint64_t m0)
{
	dump_line("po0", 0xee4, rd32(m0 + 0xee4));
	wr32(m0 + 0xee4, (rd32(m0 + 0xee4) & ~0x2u) | 0x1u);
	__asm__ volatile("dsb sy" ::: "memory");
	dump_line("po1", 0xee4, rd32(m0 + 0xee4));
}

static void display_routing_config(uint64_t dsi)
{
	static const uint64_t ov = 0x32900000UL, op = 0x32a30000UL,
			      m0 = 0x3e300000UL, m1 = 0x3e700000UL;
	const uint32_t size = 1256u | (2760u << 16);	/* 0x0ac804e8 */

	/* ovlsys0: shadow words first, then the route hops.
	 * pan39: follow LK's own leftovers for source fetch setup. pan38's pre-write probe (display_ovlsys_probe runs
	 * before any write of ours) read these values out of the live
	 * hardware LK handed over:
	 *   0xEE8 = 0x2        EXDMA3 -> BLENDER1
	 *   0xEF0 = 0x4        EXDMA4 -> BLENDER2
	 *   0xF58 = 0x00010001 BLENDER6 -> OUTPROC0
	 *   0xF40 = 0x0        BLENDER0 out unused
	 * pan38 then wrote 0xF40 |= BIT0 and 0xF58 &= ~BIT0 — it wired
	 * BLENDER0 to OUTPROC0 and DELETED the exit LK was actually using,
	 * while configuring BLENDER0 even though EXDMA3's pixels go to
	 * BLENDER1. Two independent breaks in one boot.
	 *
	 * pan47 follows the stock cascade: EXDMA3 enters BL1, BL1..BL6
	 * relay in sequence, and BL6 alone feeds OUTPROC0. Remove the
	 * stale direct BL1->OUTPROC0 hop before enabling BL6's output. */
	wr32(ov + 0x400, rd32(ov + 0x400) | 0x1u);
	wr32(ov + 0x408, rd32(ov + 0x408) | 0x1fe0000u);
	wr32(ov + 0xee8, rd32(ov + 0xee8) | 0x2u);	/* EXDMA3 -> BLENDER1 */
	wr32(ov + 0xf44, rd32(ov + 0xf44) & ~0x1u);	/* BLENDER1 is not final */
	wr32(ov + 0xf58, rd32(ov + 0xf58) | 0x1u);	/* BLENDER6 -> OUTPROC0 */
	wr32(ov + 0xeb8, rd32(ov + 0xeb8) | 0x1u);	/* OUTPROC0 -> DLO12 */

	/* OUTPROC0: kernel start + config. */
	wr32(op + 0x24, 0x1u);
	wr32(op + 0x24, 0x0u);				/* RST pulse */
	wr32(op + 0x08, 0);				/* INTSTA clear */
	wr32(op + 0x20, 1u);				/* EN, relay off */
	wr32(op + 0x10, rd32(op + 0x10) | (1u << 26));	/* OUTPUT_CLAMP */
	wr32(op + 0x30, size);				/* ROI */

	/* dispsys0: shadow, bridge sizes, PQ bypass crossbars. */
	wr32(m0 + 0x240, rd32(m0 + 0x240) | 0xff0001u);
	wr32(m0 + 0xb90, size);				/* DLI_ASYNC0 */
	wr32(m0 + 0xbd0, size);				/* DLO_ASYNC0 */
	display_pq_bypass_source_select(m0);
	display_pq_bypass_output_select(m0);

	/* dispsys1: shadow, bridge size, splitter in-select, then the DSC
	 * path. pan19 routed the uncompressed tail (0xdc4|BIT0 -> COMP,
	 * 0xee4|BIT0 -> MERGE); pan20 replaces it with the kernel's own
	 * insert recipe - mtk_ddp_insert_dsc_prim_MT6993(), three full
	 * writes (mask ~0) into the side map:
	 *   0xdc4 = BIT(6)   SPLITTER_OUT_CB0 -> DSC0_0
	 *   0xefc = BIT(0)   COMP_OUT_CB6 (DSC0's out) -> MERGE_OUT_CB0
	 *   0xee4 = 0        clear the COMP bypass route
	 * LK's leftover 0xdc4 was exactly 0x40 (BIT(6) alone) - same choice.
	 * DSC core0 itself stays LK's live config: CON 0x000 = 0x00010081
	 * (DSC_EN BIT(0) already set), MODE 0x030 = 0x00010001 (slice_mode 1
	 * + init_delay_height 1 + the load-mode-1 bit), full PPS at 0x80.
	 * The only core0 write pan20 adds is mtk_dsc_start's shadow kick:
	 * SHADOW_CTRL 0x228 = DSC_FORCE_COMMIT BIT(0) (mt6993 data has
	 * need_bypass_shadow = false, so nothing else goes there). */
	wr32(m1 + 0x2e0, rd32(m1 + 0x2e0) | 0xff0001u);
	wr32(m1 + 0xb98, size);				/* DLI_ASYNC20 */
	wr32(m1 + 0xe2c, rd32(m1 + 0xe2c) | 0x1u);
	wr32(m1 + 0xdc4, 0x00000040u);			/* -> DSC0_0 */
	wr32(m1 + 0xefc, 0x00000001u);			/* DSC0 out -> MERGE */
	wr32(m1 + 0xee4, 0x00000000u);			/* clear COMP bypass */
	wr32(m1 + 0xe6c, rd32(m1 + 0xe6c) | 0x1u);	/* MERGE -> DSI0_0 */
	wr32(0x3e7f0000UL + 0x228,
	     rd32(0x3e7f0000UL + 0x228) | 0x1u);	/* DSC FORCE_COMMIT */
	__asm__ volatile("dsb sy" ::: "memory");

	/* Post-write evidence: every word above, then the GCE frame events
	 * (disp_mutex0_sof0 0x11e / disp_mutex0_eof 0x12e /
	 * disp_ovlsys0_sof0 0x75 / stream_eof0 0x281) - a set mutex0_eof
	 * is the first direct "a frame completed" signal any pan can see. */
	{
		static const uint32_t poo[7] = {
			0x400, 0x408, 0xeb8, 0xee8, 0xf44, 0xf58, 0xe3c,
		};
		static const uint32_t m0o[5] = {
			0x240, 0xb90, 0xbd0, 0xf34, 0xee4,
		};
		static const uint32_t m1o[9] = {
			0x2e0, 0xcf8, 0xb98, 0xe2c, 0xdc4, 0xee4, 0xe6c,
			0xefc, 0xf00,
		};

		for (unsigned i = 0; i < 7; i++)
			dump_line("po", poo[i], rd32(ov + poo[i]));
		dump_line("pp", 0x000, rd32(op + 0x000));
		dump_line("pp", 0x008, rd32(op + 0x008));
		dump_line("pp", 0x010, rd32(op + 0x010));
		dump_line("pp", 0x020, rd32(op + 0x020));
		dump_line("pp", 0x030, rd32(op + 0x030));
		for (unsigned i = 0; i < 5; i++)
			dump_line("pr0", m0o[i], rd32(m0 + m0o[i]));
		for (unsigned i = 0; i < 9; i++)
			dump_line("pr1", m1o[i], rd32(m1 + m1o[i]));
	}
	/* DSC core0 live state: INTSTA's DSC_DONE (BIT0, from the driver's
	 * define) latching here is the encoder-side "a frame went through". */
	dump_line("dv", 0x000, rd32(0x3e7f0000UL));
	dump_line("dv", 0x008, rd32(0x3e7f0000UL + 0x008));
	dump_line("dv", 0x030, rd32(0x3e7f0000UL + 0x030));
	dump_line("dv", 0x228, rd32(0x3e7f0000UL + 0x228));
	for (unsigned t = 0; t < 3; t++) {
		wait_ms(200);
		wdt_kick();
		dump_line("v6", 0x0c, rd32(dsi + 0x0c));
		dump_line("v7", 0x008, rd32(0x3e7f0000UL + 0x008));
		dump_line("v7", 0x030, rd32(0x3e7f0000UL + 0x030));
	}
	/* pan41: the GCE event reads moved OUT of here. They used to sit in
	 * this function, which runs BEFORE display_mutex_kernel() arms the
	 * mutexes, so pan39/pan40's "ge 11e/12e/75 = 1" was LK's sticky
	 * boot-logo completion, not evidence of anything we did. They are
	 * now taken in display_video_start() after an explicit clear. */
	dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);
}

/* pan42: mtk_dsi_ulps_exit_end() (mtk_dsi.c:4043). The synchronous
 * exit path calls this even after a timeout. Clearing MODE alone does
 * not finish the wake: clear its interrupt, LD0 override and START,
 * then reset the engine while preserving the other CON_CTRL bits.
 * display_tx_buf() below supplies the existing golden-setting path. */
static void display_ulps_exit_end(uint64_t dsi)
{
	wr32(dsi + 0x08, rd32(dsi + 0x08) & ~(1u << 6));
	wr32(dsi + 0x1d4, rd32(dsi + 0x1d4) & ~(1u << 3));
	wr32(dsi + 0x34, rd32(dsi + 0x34) & ~(1u << 20));
	wr32(dsi + 0x00, rd32(dsi + 0x00) & ~(1u << 2));
	wr32(dsi + 0x30, rd32(dsi + 0x30) | 0x1u);
	wr32(dsi + 0x30, rd32(dsi + 0x30) & ~0x1u);
	__asm__ volatile("dsb sy" ::: "memory");
	dump_line("ue", 0x08, rd32(dsi + 0x08));
	dump_line("ue", 0x1d4, rd32(dsi + 0x1d4));
	dump_line("ue", 0x34, rd32(dsi + 0x34));
	dump_line("ue", 0x00, rd32(dsi + 0x00));
	dump_line("ue", 0x30, rd32(dsi + 0x30));
}

/* pan42: read-only snapshots. CON_CTRL bit1 was zero even after the
 * enable write in pan41; do not assume that write enabled the engine.
 * The analog PLL lives at +0x28/+0x2c/+0x30, not the old +0x24 peek. */
static void display_dsi_snapshot(uint64_t dsi, const char *tag)
{
	static const uint32_t offs[] = {
		0x00, 0x08, 0x0c, 0x30, 0x34, 0x38, 0x1d0, 0x1d4,
		0x230, 0x234, 0x244, 0x274, 0x300, 0x304, 0x30c,
	};
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		dump_line(tag, offs[i], rd32(dsi + offs[i]));
	for (uint32_t off = 0x28; off <= 0x30; off += 4)
		dump_line("ap", off, rd32(0x130a0000UL + off));
	wdt_kick();
}

/* pan43: read-only producer-to-consumer evidence, not guessed writes.
 * MT6993's three CONFIG banks have DIFFERENT VALID/READY offsets.
 * BLENDER1 DBG_STATUS5 describes layer/background input and both outputs.
 * No DCS transaction here: it would overwrite the blocked frame CMDQ. */
/* pan45: a compact second encoding of the decisive path reads. For these
 * kA0..kP1 tags ONLY, the +off payload is value<<32 | ~value, not an MMIO
 * offset. Reject a damaged complement; never repair it. This is detection,
 * not ECC correction or proof against equal corruption in both halves.
 * Read all words before logging, outside the timing-sensitive START pulse.
 * The address/tag map is mirrored and tested in decode_display_checks.py. */
static void display_checked_path_snapshot(int after)
{
	static const uint64_t addr[] = {
		0x3e300ee4UL, 0x3e300f34UL,	/* A/B: PQ output/input MOUT */
		0x32900aacUL, 0x32900ab8UL,	/* C/D: OVL VALID/READY1 */
		0x32900ab0UL, 0x32900abcUL,	/* E/F: OVL VALID/READY2 */
		0x3e300a3cUL, 0x3e300a4cUL,	/* G/H: main VALID/READY1 */
		0x3e300a44UL, 0x3e300a54UL,	/* I/J: main VALID/READY3 */
		0x3e700a3cUL, 0x3e700a4cUL,	/* K/L: side VALID/READY0 */
		0x3e730274UL, 0x3e730244UL,	/* M/N: DSI FSM/position */
		0x32900bbcUL, 0x32960100UL,	/* O/P: DLO12 counter/source FIFO */
	};
	uint32_t value[sizeof(addr) / sizeof(addr[0])];
	char tag[4] = "kA0";

	for (unsigned i = 0; i < sizeof(addr) / sizeof(addr[0]); i++)
		value[i] = rd32(addr[i]);
	tag[2] = after ? '1' : '0';
	for (unsigned i = 0; i < sizeof(addr) / sizeof(addr[0]); i++) {
		tag[1] = 'A' + i;
		dump_line(tag, value[i], ~value[i]);
	}
}

static void display_frame_path_snapshot(int after)
{
	static const uint32_t bo[] = {
		0x10, 0x20, 0x28, 0x30, 0x40, 0x7a0, 0x7ac, 0x7b4,
	};
	/* pan44: OUTPROC and async-bridge state alongside the DL handshakes.
	 * Clock STATUS offsets come from clk-mt6993-mmsys.c; relay status
	 * offsets come from mtk_disp_dli/dlo_async.c and mtk_drm_ddp.c. */
	static const uint32_t op[] = {
		0x10, 0x20, 0x28, 0x30, 0x108, 0x150, 0x154, 0x158, 0x15c,
	};
	static const uint32_t oc[] = { 0x38, 0xac8, 0xad4, 0xc30, 0xbbc, 0xeb8 };
	static const uint32_t mc[] = {
		0x34, 0x240, 0xa60, 0xa6c, 0xb90, 0xabc, 0xbd0, 0xb3c,
		0xbd4, 0xb40, 0xf34, 0xee4,
	};
	static const uint32_t sc[] = { 0xa64, 0xa70, 0xb98, 0xae0 };
	static const uint32_t mu[] = { 0x04, 0x20, 0x2c, 0x30, 0x34, 0x38 };
	const char *bt = after ? "bp1" : "bp0";
	const char *ot = after ? "ov1" : "ov0";
	const char *mt = after ? "m01" : "m00";
	const char *st = after ? "m11" : "m10";
	for (unsigned i = 0; i < sizeof(bo) / sizeof(bo[0]); i++)
		dump_line(bt, bo[i], rd32(0x329c0000UL + bo[i]));
	for (uint32_t off = 0xaa8; off <= 0xabc; off += 4)
		dump_line(ot, off, rd32(0x32900000UL + off));
	for (uint32_t off = 0xa38; off <= 0xa54; off += 4)
		dump_line(mt, off, rd32(0x3e300000UL + off));
	for (uint32_t off = 0xa3c; off <= 0xa58; off += 4)
		dump_line(st, off, rd32(0x3e700000UL + off));
	for (unsigned i = 0; i < sizeof(op) / sizeof(op[0]); i++)
		dump_line(after ? "op1" : "op0", op[i], rd32(0x32a30000UL + op[i]));
	for (unsigned i = 0; i < sizeof(oc) / sizeof(oc[0]); i++)
		dump_line(after ? "oc1" : "oc0", oc[i], rd32(0x32900000UL + oc[i]));
	for (unsigned i = 0; i < sizeof(mc) / sizeof(mc[0]); i++)
		dump_line(after ? "mc1" : "mc0", mc[i], rd32(0x3e300000UL + mc[i]));
	for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++)
		dump_line(after ? "sc1" : "sc0", sc[i], rd32(0x3e700000UL + sc[i]));
	for (unsigned i = 0; i < sizeof(mu) / sizeof(mu[0]); i++) {
		dump_line(after ? "ma1" : "ma0", mu[i], rd32(0x3e320000UL + mu[i]));
		dump_line(after ? "mb1" : "mb0", mu[i], rd32(0x3e720000UL + mu[i]));
		dump_line(after ? "mo1" : "mo0", mu[i], rd32(0x32920000UL + mu[i]));
	}
	wdt_kick();
}

/* pan46: apply one additional mutex property at each stage. No route,
 * framebuffer, clock, PLL, DSC PPS, dimensions or DSI threshold changes.
 * Stage 1: release the acquired software mutex, as the CPU driver does.
 * Stage 2: per-frame CFG/EN sequence, enabled by frame_mutex_rearm.
 * Stage 3: EXDMA3 -> BL1..BL6 -> OUTPROC0 from the compiled mutex table.
 * Stage 4: BIT31 in the software tables selects MOD1; the driver strips it
 * before writing hardware. It is NOT a hardware commit/valid flag. */
static void display_final_mutex_stage(unsigned stage)
{
	static const uint64_t base[] = {
		0x3e320000UL, 0x32920000UL, 0x3e720000UL, 0x32c20000UL,
	};
	if (stage == 1) {
		for (unsigned i = 0; i < sizeof(base) / sizeof(base[0]); i++)
			wr32(base[i] + 0x24, 0);
	} else if (stage == 2) {
		frame_mutex_rearm = 1;
	} else if (stage == 3) {
		wr32(0x32920000UL + 0x20, 0);
		/* Stock table: EXDMA3 | BL1..BL6 | OUTPROC0; no BL7/EXDMA4. */
		wr32(0x32920000UL + 0x34, 0x00017e08u);
	} else if (stage == 4) {
		wr32(0x32920000UL + 0x20, 0);
		wr32(0x3e720000UL + 0x20, 0);
		wr32(0x32920000UL + 0x38,
		     rd32(0x32920000UL + 0x38) & ~0x80000000u);
		wr32(0x3e720000UL + 0x38,
		     rd32(0x3e720000UL + 0x38) & ~0x80000000u);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t display_record_crc(const char *data, unsigned len)
{
	uint32_t crc = 0xffffffffu;
	for (unsigned i = 0; i < len; i++) {
		crc ^= (unsigned char)data[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

/* pan48: read all six cascade selectors once; CRC rejects damaged evidence. */
static void display_cascade_snapshot(unsigned after)
{
	static const char hex[] = "0123456789abcdef";
	uint32_t value[6];
	for (unsigned i = 0; i < 6; i++)
		value[i] = rd32(0x329c0010UL + (uint64_t)i * 0x10000UL);
	char line[27];
	line[0] = 'q'; line[1] = '4'; line[2] = '8'; line[3] = '0';
	line[4] = after ? '1' : '0'; line[5] = '0';
	line[7] = line[16] = ':'; line[25] = '\n'; line[26] = 0;
	for (unsigned copy = 0; copy < 8; copy++) {
		for (unsigned j = 0; j < 6; j++) {
			unsigned i = (j + copy) % 6;
			line[6] = hex[i];
			for (unsigned n = 0; n < 8; n++)
				line[8 + n] = hex[(value[i] >> ((7 - n) * 4)) & 15];
			uint32_t crc = display_record_crc(line, 16);
			for (unsigned n = 0; n < 8; n++)
				line[17 + n] = hex[(crc >> ((7 - n) * 4)) & 15];
			plog_str(line);
		}
		wdt_kick();
	}
}

/* pan49: group-correct handshakes plus per-blender inputs; read-only. */
static void display_graph_snapshot(unsigned stage, unsigned after)
{
	static const uint64_t addr[] = {
		0x32900aa8UL, 0x32900ab4UL, 0x32900aacUL, 0x32900ab8UL,
		0x32900ab0UL, 0x32900abcUL, 0x329c0040UL, 0x329d0040UL,
		0x329e0040UL, 0x329f0040UL, 0x32a00040UL, 0x32a10040UL,
		0x329c07b4UL, 0x329d07b4UL, 0x329e07b4UL, 0x329f07b4UL,
		0x32a007b4UL, 0x32a107b4UL, 0x32920034UL, 0x32920038UL,
		0x3e720038UL, 0x3e730274UL, 0x32900bbcUL,
	};
	static const char hex[] = "0123456789abcdef";
	uint32_t value[sizeof(addr) / sizeof(addr[0])];
	const unsigned count = sizeof(value) / sizeof(value[0]);
	for (unsigned i = 0; i < count; i++)
		value[i] = rd32(addr[i]);
	char line[27];
	line[0] = 'q'; line[1] = '4'; line[2] = '9';
	line[3] = '0' + stage; line[4] = after ? '1' : '0';
	line[7] = line[16] = ':'; line[25] = '\n'; line[26] = 0;
	for (unsigned copy = 0; copy < 8; copy++) {
		for (unsigned j = 0; j < count; j++) {
			unsigned i = (j + copy * 5) % count;
			line[5] = hex[i >> 4]; line[6] = hex[i & 15];
			for (unsigned n = 0; n < 8; n++)
				line[8 + n] = hex[(value[i] >> ((7 - n) * 4)) & 15];
			uint32_t crc = display_record_crc(line, 16);
			for (unsigned n = 0; n < 8; n++)
				line[17 + n] = hex[(crc >> ((7 - n) * 4)) & 15];
			plog_str(line);
		}
		wdt_kick();
	}
}

/* q46<stage><pre/post><index>:<value>:<CRC32> is a new, independent
 * diagnostic protocol. CRC covers all 16 prefix bytes, including the stage,
 * phase, index and value. A corrupted but plausible tag cannot silently
 * reattribute a register. No repair or majority-vote reconstruction.
 * Eight interleaved copies all represent ONE read snapshot. Logging and
 * watchdog servicing are outside the timing-sensitive START pulse. */
static void display_final_path_snapshot(unsigned stage, unsigned after)
{
	static const uint64_t addr[] = {
		0x3e300ee4UL, 0x3e300f34UL, 0x32900aacUL, 0x32900ab8UL,
		0x32900ab0UL, 0x32900abcUL, 0x3e300a3cUL, 0x3e300a4cUL,
		0x3e300a44UL, 0x3e300a54UL, 0x3e700a3cUL, 0x3e700a4cUL,
		0x3e730274UL, 0x3e730244UL, 0x32900bbcUL, 0x32960100UL,
		0x3e320020UL, 0x3e320024UL, 0x3e320034UL, 0x3e320038UL,
		0x3e720020UL, 0x3e720024UL, 0x3e720034UL, 0x3e720038UL,
		0x32920020UL, 0x32920024UL, 0x32920034UL, 0x32920038UL,
		0x3e320008UL, 0x3e720008UL, 0x32920008UL, 0x3e73000cUL,
		0x32960240UL, 0x32a30108UL, 0x3e7f0008UL,
	};
	static const char hex[] = "0123456789abcdef";
	uint32_t value[sizeof(addr) / sizeof(addr[0]) + 3];
	const unsigned regs = sizeof(addr) / sizeof(addr[0]);
	const unsigned count = sizeof(value) / sizeof(value[0]);
	/* Explicit sparse stores: a string-array initializer makes Apple clang
	 * emit a libc memcpy in the freestanding arm64 build. No libc here. */
	char line[27];
	line[0] = 'q';
	line[1] = '4';
	line[2] = '6';
	line[7] = line[16] = ':';
	line[25] = '\n';
	line[26] = 0;
	for (unsigned i = 0; i < regs; i++)
		value[i] = rd32(addr[i]);
	value[regs] = frame_last_status;
	value[regs + 1] = frame_last_wait;
	value[regs + 2] = frame_good_count;
	line[3] = '0' + stage;
	line[4] = after ? '1' : '0';
	for (unsigned copy = 0; copy < 8; copy++) {
		for (unsigned j = 0; j < count; j++) {
			unsigned i = (j + copy * 5) % count;
			line[5] = hex[i >> 4];
			line[6] = hex[i & 15];
			for (unsigned n = 0; n < 8; n++)
				line[8 + n] = hex[(value[i] >> ((7 - n) * 4)) & 15];
			uint32_t crc = display_record_crc(line, 16);
			for (unsigned n = 0; n < 8; n++)
				line[17 + n] = hex[(crc >> ((7 - n) * 4)) & 15];
			plog_str(line);
		}
		wdt_kick();
	}
}

static int display_normal_frame_ok(uint32_t status)
{
	/* FRAME_DONE BIT4 is not CMD_DONE BIT1. Never accept idle alone,
	 * BUSY, underrun BIT12 or input-unfinished BIT18 as frame success. */
	return (status & 0x10u) && !(status & 0x80041000u);
}

void display_video_start(void)
{
	const uint64_t dsi = dsi_pair[0];
	dump_line("pan", 0x46, 1);
	frame_mutex_rearm = frame_good_count = 0;
	frame_last_status = frame_last_wait = 0;
	/* LK's ovlsys source state, before any write of ours. */
	display_ovlsys_probe();
	/* Handover state, before any write of ours. */
	for (unsigned i = 0; i < 12; i++)
		dump_line("v0", 0x30 + i * 4, rd32(dsi + 0x30 + i * 4));
	for (unsigned i = 0; i < 4; i++)
		dump_line("v0", 0x80 + i * 4, rd32(dsi + 0x80 + i * 4));
	dump_line("v0", 0x1d0, rd32(dsi + 0x1d0));
	dump_line("v0", 0x1d4, rd32(dsi + 0x1d4));
	dump_line("v0", 0x200, rd32(dsi + 0x200));
	for (unsigned i = 0; i < 6; i++)
		dump_line("v0", 0x600 + i * 4, rd32(dsi + 0x600 + i * 4));
	/* MIPITX PLL @0x130a0000 (mipi-tx-config@130A0000): read-only peek
	 * at what LK locked. Offsets from mtk_mipi_tx.c MT6983-style defines. */
	dump_line("mtx", 0x00, rd32(0x130a0000UL));
	dump_line("mtx", 0x04, rd32(0x130a0000UL + 0x04));
	dump_line("mtx", 0x08, rd32(0x130a0000UL + 0x08));
	dump_line("mtx", 0x24, rd32(0x130a0000UL + 0x24));

	/* mtk_dsi_enable: engine on. */
	wr32(dsi + 0x30, rd32(dsi + 0x30) | 0x2u);
	dump_line("vce", 0x30, rd32(dsi + 0x30));

	/* mtk_dsi_exit_ulps(false), verbatim order. */
	mipi_lane_pre_oe(0);
	mipi_lane_oe(0);
	mipi_lane_dpn(0);
	mipi_lane_sw_ctl(1);
	wr32(dsi + 0x38, (rd32(dsi + 0x38) & ~0x3cu) | (0xfu << 2));
	/* mtk_dsi_phy_reset: CON_CTRL PHY_RESET 0 -> 1 -> 0. */
	wr32(dsi + 0x30, rd32(dsi + 0x30) & ~0x4u);
	wr32(dsi + 0x30, rd32(dsi + 0x30) | 0x4u);
	wr32(dsi + 0x30, rd32(dsi + 0x30) & ~0x4u);
	mipi_lane_pre_oe(1);
	wr32(dsi + 0x08, rd32(dsi + 0x08) | (1u << 6));	/* sleepout_done int */
	wr32(dsi + 0x1d4, rd32(dsi + 0x1d4) | (1u << 3));	/* ulpm as l0 */
	wr32(dsi + 0x34, rd32(dsi + 0x34) | (1u << 20));	/* sleep mode */
	/* TIME_CON0 at +0x200 (not the PHY bank): wake-up period 102 =
	 * 828*1000/8192 + 1 for DPHY at data_rate 828. */
	wr32(dsi + 0x200, (rd32(dsi + 0x200) & ~0xffffu) | 102u);
	mipi_lane_sw_ctl(0);
	mipi_lane_oe(1);
	mipi_lane_dpn(1);
	wr32(dsi + 0x00, rd32(dsi + 0x00) & ~0x4u);	/* sleepout start */
	wr32(dsi + 0x00, rd32(dsi + 0x00) | 0x4u);
	{
		int done = 0;
		for (int i = 0; i < 200 && !done; i++) {
			done = rd32(dsi + 0x0c) & (1u << 6);
			wait_ms(1);
		}
		plog_hex("ulps done", done ? 1u : 0u);
		dump_line("ul", 0x0c, rd32(dsi + 0x0c));
	}
	display_ulps_exit_end(dsi);

	/* mtk_dsi_phy_timconfig (dphy v1, data_rate 828: ui 1, cycle 9). */
	wr32(dsi + 0x600, 0x0a0f090au);	/* lpx|hs_prep|hs_zero|hs_trail */
	wr32(dsi + 0x604, 0x0f320f28u);	/* ta_go|ta_sure|ta_get|da_hs_exit */
	wr32(dsi + 0x608, 0x091f0103u);	/* cont_det|da_hs_sync|clk_zero|clk_trail */
	wr32(dsi + 0x60c, 0x000f1008u);	/* clk_hs_prep|clk_hs_post|clk_hs_exit */
	/* mtk_dsi_rxtx_control: 4 lanes, non-continuous clock; MEM_CONTI. */
	wr32(dsi + 0x38, (0xfu << 2) | (1u << 6));
	wr32(dsi + 0x48, 0x3cu);
	/* mtk_dsi_ps_control_vact, DSC branch (PSCTRL masked like kernel). */
	wr32(dsi + 0x6c, 2760);		/* VACT */
	wr32(dsi + 0x3c, (rd32(dsi + 0x3c) & ~0xf7fffu)
			 | (5u << 16) | 1256);	/* PS_SEL DSC, ps_wc 628*2 */
	wr32(dsi + 0x2c, (2760u << 16) | 419);	/* SIZE: h=ceil(1256/3) */
	/* mtk_dsi_set_vm_cmd: VM_CMD_EN | TS_VFP_EN. */
	wr32(dsi + 0x110, 0x21u);
	/* mtk_dsi_calc_vdo_timing + config_vdo_timing, u32 arithmetic kept
	 * verbatim: hsa wraps; hfp takes the VER_N3 non-cont-clock minimum
	 * clamp then the lane-alignment fine-tune. */
	{
		uint32_t hsa = 2u * 2u - 10u;		/* 0xfffffffa */
		uint32_t hbp = 21u * 2u - 10u;		/* 32 */
		uint32_t hfp = 9u * 2u - 12u;		/* 6 */
		const uint32_t dpc = 51u, lanes = 4u, ps_wc = 1256u;
		const uint32_t hfp_min =
		    (10u + 1u + 16u + 1u + 9u + 15u + 1u + 10u + 8u + 31u)
		    * lanes - 2u;			/* 406 */
		uint32_t total;

		if (hfp > dpc * lanes)
			hfp -= dpc * lanes;
		else
			hfp = 1;
		if (hfp < hfp_min)
			hfp = hfp_min;
		total = 4u + (4u + hsa + 2u) + 4u + (4u + hbp + 2u)
		      + (4u + ps_wc + 2u) + (4u + hfp + 2u) + dpc * lanes;
		hfp = hfp + lanes * 2u - total % (lanes * 2u);
		wr32(dsi + 0x60, 14);		/* VSA */
		wr32(dsi + 0x64, 22);		/* VBP */
		wr32(dsi + 0x68, 52);		/* VFP: live DT 60 Hz value 0x34 */
		wr32(dsi + 0x80, hsa);		/* HSA_WC */
		wr32(dsi + 0x84, hbp);		/* HBP_WC */
		wr32(dsi + 0x88, hfp);		/* HFP_WC */
		plog_hex("hfp wc", hfp);
	}
	/* mtk_dsi_cmdq_size_sel + set_interrupt_enable. INTSTA is W1C:
	 * clear latched flags with ~status like the kernel, not with 0. */
	wr32(dsi + 0x44, rd32(dsi + 0x44) | 0x8000u);
	wr32(dsi + 0x0c, ~rd32(dsi + 0x0c));
	wr32(dsi + 0x08, 0x45010u);	/* underrun|inp_unfinish|frame_done|target_line */
	__asm__ volatile("dsb sy" ::: "memory");
	for (unsigned i = 0; i < 12; i++)
		dump_line("v1", 0x30 + i * 4, rd32(dsi + 0x30 + i * 4));
	for (unsigned i = 0; i < 4; i++)
		dump_line("v1", 0x80 + i * 4, rd32(dsi + 0x80 + i * 4));

	/* mtk_preconfig_dsi_enable order: ... rxtx_control, then tx_buf. */
	display_tx_buf(dsi);

	/* pan37: CMD mode, not video. MT6993 has no video mode
	 * (mtk_dsi.c:17103 "mt6993 only support cmd mode"); the
	 * SYNC_PULSE + mtk_dsi_start() sequence every earlier pan ran is
	 * the non-cmd branch. MODE_CTRL.MODE = CMD_MODE (0), then the
	 * per-frame trigger below pushes whatever the DDP pipeline has
	 * filled into the DSI buffer. */
	wr32(dsi + 0x34, rd32(dsi + 0x34) & ~0x3u);	/* CMD_MODE */
	wr32(dsi + 0x610, 0x55);	/* LCPAT: DPHY pattern */
	wr32(dsi + 0x1d0, rd32(dsi + 0x1d0) | 0x1u);	/* LCCON: HS clock */
	__asm__ volatile("dsb sy" ::: "memory");
	wait_ms(10);
	wdt_kick();
	dump_line("s1", 0x0c, rd32(dsi + 0x0c));
	wait_ms(30);
	wdt_kick();
	for (unsigned i = 0; i < 12; i++)
		dump_line("v2", 0x30 + i * 4, rd32(dsi + 0x30 + i * 4));
	dump_line("v2", 0x1d0, rd32(dsi + 0x1d0));
	dump_line("v2", 0x1d4, rd32(dsi + 0x1d4));
	dump_line("v2", 0x200, rd32(dsi + 0x200));
	dump_line("v2", 0x0c, rd32(dsi + 0x0c));
	/* pan37: fill the framebuffer, configure the whole DDP chain, arm
	 * the mutexes, then run the cmd-mode trigger. The glass is the
	 * verdict channel: a wrong register in this chain now shows up as
	 * "still no colour" rather than being masked by a video mode the
	 * panel never runs. */
	fb_fill(0xff0000ffu);
	display_ovlsys_layer_config(dsi);
	display_routing_config(dsi);
	display_mutex_kernel();
	wait_ms(120);
	wdt_kick();
	dump_line("v3", 0x30, rd32(dsi + 0x30));
	dump_line("v3", 0x34, rd32(dsi + 0x34));
	dump_line("v3", 0x0c, rd32(dsi + 0x0c));
	dump_line("v3", 0x1d0, rd32(dsi + 0x1d0));
	/* pan41: clear the sticky GCE display events BEFORE the trigger
	 * loop, so whatever reads back afterwards was produced by these
	 * triggers and by nothing earlier. pan39/pan40 read these same
	 * events without clearing and after the fact, then reported them
	 * as "the mutex is demonstrably producing frames"; that inference
	 * did not hold. */
	gce_event_clr(0x11e);	/* disp_mutex0_sof0 */
	gce_event_clr(0x12e);	/* disp_mutex0_eof  */
	gce_event_clr(0x75);	/* disp_ovlsys0_sof0 */
	gce_event_clr(0x281);	/* stream_eof0 */
	/* pan42: compare source FIFO and DSI state around the frame loop.
	 * A zero latched GCE event is not proof the source never started. */
	dump_line("nf0", 0x100, rd32(0x32960000UL + 0x100));
	dump_line("nf0", 0x240, rd32(0x32960000UL + 0x240));
	display_dsi_snapshot(dsi, "ds0");
	display_checked_path_snapshot(0);
	display_frame_path_snapshot(0);
	/* One manual flash covers five bounded, cumulative tests. Do not mix
	 * a new route into a mutex experiment. Stop at the first two consecutive
	 * fresh, error-free normal FRAME_DONEs, not a pattern or stale GCE bit.
	 * A previously pending command can drain on the first repaired frame;
	 * requiring another cleared-status completion avoids calling that alone
	 * a repeatable new frame. Glass visibility still requires owner feedback. */
	display_cascade_snapshot(0);
	int winning_stage = -1;
	for (unsigned stage = 0; stage < 5; stage++) {
		frame_good_count = 0;
		display_final_mutex_stage(stage);
		display_final_path_snapshot(stage, 0);
		display_graph_snapshot(stage, 0);
		for (unsigned f = 0; f < 4; f++) {
			display_cmd_frame_trigger(dsi);
			wait_ms(40);
			wdt_kick();
			frame_last_status = rd32(dsi + 0x0c);
			if (display_normal_frame_ok(frame_last_status))
				frame_good_count++;
			else
				frame_good_count = 0;
			if (frame_good_count >= 2) {
				winning_stage = (int)stage;
				break;
			}
		}
		display_final_path_snapshot(stage, 1);
		display_graph_snapshot(stage, 1);
		if (winning_stage >= 0)
			break;
	}
	display_cascade_snapshot(1);
	display_dsi_snapshot(dsi, "ds1");
	display_checked_path_snapshot(1);
	display_frame_path_snapshot(1);
	if (winning_stage >= 0) {
		plog_hex("normal frame stage", (unsigned)winning_stage);
		/* Preserve the working normal image instead of overwriting it with
		 * a BTA power read or DSC-ambiguous self-pattern. */
		for (unsigned i = 0; i < 4; i++) {
			wait_ms(1000);
			wdt_kick();
		}
		plog_str("normal frame hold done\n");
		return;
	}
	/* Events were cleared before the triggers; these reads cannot be
	 * cited as frame completion if they remain zero. */
	plog_hex("ge 11e", gce_event_get(0x11e));
	plog_hex("ge 12e", gce_event_get(0x12e));
	plog_hex("ge 075", gce_event_get(0x75));
	plog_hex("ge 281", gce_event_get(0x281));
	/* Mutex state: EN, acquire/ownership (0x24), SOF source, MOD0/1.
	 * RESET is +0x2c; neither offset is the kernel frame trigger, which
	 * writes EN=1 across the participating domains. */
	dump_line("mz", 0x20, rd32(0x3e320000UL + 0x20));
	dump_line("mz", 0x24, rd32(0x3e320000UL + 0x24));
	dump_line("mz", 0x30, rd32(0x3e320000UL + 0x30));
	dump_line("mz", 0x34, rd32(0x3e320000UL + 0x34));
	dump_line("mz", 0x38, rd32(0x3e320000UL + 0x38));
	dump_line("my", 0x20, rd32(0x32920000UL + 0x20));
	dump_line("my", 0x24, rd32(0x32920000UL + 0x24));
	dump_line("my", 0x30, rd32(0x32920000UL + 0x30));
	dump_line("my", 0x34, rd32(0x32920000UL + 0x34));
	dump_line("my", 0x38, rd32(0x32920000UL + 0x38));
	/* EXDMA3 source state. pan41's RDMA0_CTRL = 0x05cd0001 means
	 * FIFO_USED_SIZE = 1485, and FLOW_CTRL_DBG = 0x08040420 means
	 * eng_act with out_ready = 0. Zero instantaneous G-REQ readings
	 * do not negate the buffered data or prove the source is idle. */
	display_exdma_counters("c1");
	dump_line("c1", 0x020, rd32(0x32960000UL + 0x020));	/* EXDMA3 EN */
	dump_line("c1", 0x040, rd32(0x32960000UL + 0x040));	/* L0_EN */
	dump_line("c1", 0xf40, rd32(0x32960000UL + 0xf40));	/* L0_ADDR */
	dump_line("c1", 0x030, rd32(0x32960000UL + 0x030));	/* ROI */
	dump_line("v4", 0x0c, rd32(dsi + 0x0c));
	dump_line("v4", 0x00, rd32(dsi + 0x00));
	dump_line("v4", 0x44, rd32(dsi + 0x44));
	dump_line("v4", 0x48, rd32(dsi + 0x48));
	/* The verdict read: a single power-mode read settles whether the
	 * panel stayed awake (0x9c = dtsi ESD healthy value). */
	dsi_dcs_read_k(dsi_pair[0], 0x0a, 1);

	/* DSI self-pattern, using the runtime PATGEN_EN value (mtk_dsi.c:
	 * 8689). pan41 confirmed CON0 = 0xc11 but showed no colour.
	 * That is NOT a decisive panel/link test: the runtime option runs
	 * inside the ordinary CRTC frame configuration, and the shim's
	 * engine enable, wake completion and frame timing still need
	 * verification. In particular, this panel expects DSC data; a
	 * register write alone does not prove a valid frame was sent. */
	dump_line("sp0", 0x230, rd32(dsi + 0x230));
	dump_line("sp0", 0x234, rd32(dsi + 0x234));
	wr32(dsi + 0x230, 0xc11u);
	/* CON1 has no value in the debugfs path — only CON0 does. The one
	 * place the kernel does define CON1 is the disabled
	 * mtk_dsi_self_pattern() (mtk_dsi.c:3727, 0x0fff0fff), so use that
	 * rather than inventing a value, and log it either way. */
	wr32(dsi + 0x234, 0x0fff0fffu);
	__asm__ volatile("dsb sy" ::: "memory");
	wait_ms(50);
	wdt_kick();
	dump_line("sp1", 0x230, rd32(dsi + 0x230));
	dump_line("sp1", 0x234, rd32(dsi + 0x234));
	/* Keep the pattern up through the post-trigger waits so the owner
	 * has time to see it before the warm reset. The self-pattern path
	 * still needs the cmd-mode frame trigger to move a frame. */
	for (int f = 0; f < 6; f++) {
		display_cmd_frame_trigger(dsi);
		wait_ms(120);
		wdt_kick();
	}
	dump_line("sp2", 0x230, rd32(dsi + 0x230));
	dump_line("sp2", 0x0c, rd32(dsi + 0x0c));
	dump_line("sp2", 0x00, rd32(dsi + 0x00));
	display_dsi_snapshot(dsi, "ds2");
	/* Leave the pattern enabled for observation. main.c restores the
	 * backlight after this returns; whether pixels reach the glass
	 * is a hardware result, not implied by the enable register. */
	plog_str("video start done\n");
}

/* GCE (Global Command Engine, 0x30100000, mminfra domain). Register
 * interface recovered from mtk-cmdq-drv-ext.ko:
 *   +0x60 write event id (masked 0x3ff) = query port, value at +0x64
 *   +0x68 event command: 0x10000|id sets the event, id alone clears it
 *   thread CSRs at +0x100 + n*0x80, 32 threads (cmdq_probe loop, stride
 *   0x80): +0x00 CURR_ADDR, +0x04 END_ADDR, +0x10 IRQ status (read/ack),
 *   +0x20 PC, +0x24 END (cmdq_thread_get_pc/get_end).
 * Display event ids from the handover DT (dispsys-config gce-event-names
 * -> gce-events): 0x280 disp_token_stream_dirty0, 0x281 stream_eof0,
 * 0x282 esd_eof0, 0x283 stream_block0, 0x284 cabc_eof0, 0x285 vfp_period0,
 * 0x288 mml_stream_config, 0x27b mml_stop, 0x2bb mml_disp_done_event,
 * 0x11e disp_mutex0_sof0, 0x12e disp_mutex0_eof, 0x166
 * disp_cmd_mode_trig_start, 0x167 vdo_mode_trig_start, 0x164 wait_dsi0_te,
 * 0x75 disp_ovlsys0_sof0; 'default-tokens' pairs 0x2bc/0x2bd, 0x2be/0x2bf,
 * 0x2c0/0x2c6, 0x2c7/0x2aa, 0x2ad/0x2b3, 0x287/0x27b, 0x2bb/0x288. */
#define GCE_BASE	0x30100000UL
#define GCE_THR(n)	(GCE_BASE + 0x100UL + (n) * 0x80UL)

static uint32_t gce_event_get(uint32_t ev)
{
	wr32(GCE_BASE + 0x60, ev & 0x3ffu);
	__asm__ volatile("dsb sy" ::: "memory");
	return rd32(GCE_BASE + 0x64);
}

static void gce_event_set(uint32_t ev)
{
	wr32(GCE_BASE + 0x68, 0x10000u | (ev & 0xffffu));
	__asm__ volatile("dsb sy" ::: "memory");
}

/* pan41: writing the event id alone CLEARS it (+0x68, the same port
 * gce_event_set writes 0x10000|id to). Without this the reads below
 * are worthless as evidence: GCE events are sticky, so a "1" read
 * after our triggers may be LK's boot-logo completion still latched
 * from seconds earlier. pan39/pan40 treated exactly such a read as
 * proof the mutex was framing — it was not proof of anything. */
static void gce_event_clr(uint32_t ev)
{
	wr32(GCE_BASE + 0x68, ev & 0xffffu);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Inventory before touching anything: the GCE global window, every thread
 * CSR (a thread LK parked mid-packet shows PC != 0 — its WFE operand names
 * the waited token), and the live value of every display/token event. */
void display_gce_diag(void)
{
	static const uint32_t regs[6] = { 0x00, 0x04, 0x08, 0x10, 0x20, 0x24 };

	dump_block(GCE_BASE, "gcG", 0x20);
	for (unsigned t = 0; t < 32; t++) {
		char tag[4] = { 'g', '0' + (char)(t / 10), '0' + (char)(t % 10), 0 };
		for (unsigned r = 0; r < 6; r++)
			dump_line(tag, regs[r], rd32(GCE_THR(t) + regs[r]));
		wdt_kick();
	}
	static const uint32_t evs[] = {
		0x280, 0x281, 0x282, 0x283, 0x284, 0x285,
		0x288, 0x27b, 0x2bb,
		0x2bc, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c6, 0x2c7,
		0x2aa, 0x2ad, 0x2b3, 0x287,
		0x11e, 0x12e, 0x166, 0x167, 0x164, 0x165, 0x168,
		0x75, 0x2c1, 0x2c2, 0x2c3, 0x2c4,
	};
	for (unsigned i = 0; i < sizeof(evs) / sizeof(evs[0]); i++)
		dump_line("ev", evs[i], gce_event_get(evs[i]));
	plog_str("gce diag done\n");
}

/* Signal each candidate token once and watch what answers. Per try, the
 * synthetic dump offsets are: 0xf0/0xf8 o0e3 pixel counters, 0x100 the
 * event's value after the set (0 = consumed by a waiting thread),
 * 0x200 DSI INTSTA, 0x300+thread a PC that moved since the last look. */
void display_gce_signal(void)
{
	static const struct { uint32_t ev; char tag; } tries[] = {
		{ 0x280, 'D' },	/* disp_token_stream_dirty0 */
		{ 0x166, 'T' },	/* disp_cmd_mode_trig_start */
		{ 0x11e, 'S' },	/* disp_mutex0_sof0 */
		{ 0x288, 'M' },	/* mml_stream_config */
		{ 0x281, 'E' },	/* disp_token_stream_eof0 */
		{ 0x75,  'O' },	/* disp_ovlsys0_sof0 */
		{ 0x283, 'B' },	/* disp_token_stream_block0 */
		{ 0x2c3, 'I' },	/* disp_token_disp_v_idle_power_on0 */
		{ 0x2bb, 'd' },	/* mml_disp_done_event */
		{ 0x27b, 'm' },	/* mml_stop */
	};
	static uint32_t pc0[32];
	const uint64_t oe3 = 0x32960000UL;

	for (unsigned t = 0; t < 32; t++)
		pc0[t] = rd32(GCE_THR(t) + 0x20);
	for (unsigned s = 0; s < sizeof(tries) / sizeof(tries[0]); s++) {
		char tag[2] = { tries[s].tag, 0 };

		gce_event_set(tries[s].ev);
		wait_ms(80);
		wdt_kick();
		dump_line(tag, 0x0f0, rd32(oe3 + 0xf0));
		dump_line(tag, 0x0f8, rd32(oe3 + 0xf8));
		dump_line(tag, 0x100, gce_event_get(tries[s].ev));
		dump_line(tag, 0x200, rd32(DSI_BASE + 0x0c));
		for (unsigned t = 0; t < 32; t++) {
			uint32_t pc = rd32(GCE_THR(t) + 0x20);

			if (pc != pc0[t]) {
				dump_line(tag, 0x300 + t, pc);
				pc0[t] = pc;
			}
		}
		wait_ms(40);
		wdt_kick();
	}
	plog_str("gce signal done\n");
}

/* Final state: full CSR re-dump of every thread that holds an address. */
void display_gce_post(void)
{
	for (unsigned t = 0; t < 32; t++) {
		char tag[4] = { 'h', '0' + (char)(t / 10), '0' + (char)(t % 10), 0 };

		if (rd32(GCE_THR(t) + 0x00) | rd32(GCE_THR(t) + 0x20)) {
			dump_line(tag, 0x00, rd32(GCE_THR(t) + 0x00));
			dump_line(tag, 0x04, rd32(GCE_THR(t) + 0x04));
			dump_line(tag, 0x10, rd32(GCE_THR(t) + 0x10));
			dump_line(tag, 0x20, rd32(GCE_THR(t) + 0x20));
			dump_line(tag, 0x24, rd32(GCE_THR(t) + 0x24));
		}
		wdt_kick();
	}
	plog_str("gce post done\n");
}


void display_mutex_arm(void)
{
	wr32(0x3e300000UL + 0xa68, 0xffffffffu);
	wr32(0x3e300000UL + 0xa74, 0xffffffffu);
	wr32(0x3e300000UL + 0x1a8, 0xffffffffu);
	wr32(0x3e700000UL + 0xa6c, 0xffffffffu);
	wr32(0x3e700000UL + 0xa78, 0xffffffffu);
	__asm__ volatile("dsb sy" ::: "memory");

	const uint64_t mux = 0x3e320000UL;
	wr32(mux + 0x08, 0);
	/*
	 * Re-assert the MOD/SOF values LK left in OVLSYS MUTEX0. This is a
	 * no-op when they are intact, and a repair when they are not: LK's own
	 * disable path (lk 0x6dab0/0x6db60) clears MOD/SOF before pulsing +0x2c,
	 * and the hardware only latches an EXDMA shadow register (0xf00 and the
	 * rest) for modules whose MOD bit is set. Values from the mutex1.log
	 * hardware readback:
	 *   +0x30 (SOF)  = 0x00000080  (DSI0 SOF source, bit 7)
	 *   +0x34 (MOD0) = 0x030001ff
	 *   +0x38 (MOD1) = 0x003de300
	 * Whether EXDMA5 (comp 652) has a MOD bit in either word is still
	 * unresolved; this restores LK's set, it does not add to it.
	 */
	wr32(mux + 0x30, 0x00000080u);
	wr32(mux + 0x34, 0x030001ffu);
	wr32(mux + 0x38, 0x003de300u);
	wr32(mux + 0x20, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	plog_hex("mux en", rd32(mux + 0x20));
	dump_line("mxM0", 0x30, rd32(mux + 0x30));
	dump_line("mxM1", 0x34, rd32(mux + 0x34));
	dump_line("mxM2", 0x38, rd32(mux + 0x38));
	plog_str("mutex armed\n");
}

/* Final-state dump after the trigger loop: what stuck, what the DSI saw. */
void display_post_diag(void)
{
	/* The latch verdict: did the shadow BASE_0 take the framebuffer
	 * pointer, and did mml0-mutex0 record frame completions? */
	display_mml_shadow_dump("psh");
	dump_block(0x32020000UL, "pm0", 0x40);
	dump_block(0x32960000UL, "pe3", 0x40);
	dump_block(0x3e320000UL, "pmx", 0x40);
	dump_block(DSI_BASE, "pds", 0x8);
	plog_str("post diag done\n");
}


/* Hold the configured pipeline alive and watch it: re-trigger the LK DSI
 * sequence every 500 ms for `loops` rounds, sampling a few state registers
 * every 10th round. Green that survives this loop is a working pixel path;
 * a decay shows exactly when and into what the pipeline falls over. */
void display_green_hold(int loops)
{
	const uint64_t oe3 = 0x32960000UL;
	static const struct { uint64_t base; uint32_t off; } watch[] = {
		{ oe3, 0x000 }, { oe3, 0x020 }, { oe3, 0x0f40 },
		{ oe3, 0x0f0 }, { oe3, 0x0f4 }, { oe3, 0x0f8 }, { oe3, 0x0fc },
		{ 0x3e320000UL, 0x000 }, { 0x3e320000UL, 0x024 },
		{ DSI_BASE, 0x00c },
	};
	for (int i = 0; i < loops; i++) {
		dsi_trigger_lk();
		dsi_wait_idle();
		wdt_kick();
		if (i % 10 == 0) {
			char tag[2] = { '0' + (char)((i / 10) % 10), 0 };
			for (unsigned w = 0; w < sizeof(watch) / sizeof(watch[0]); w++)
				dump_line(tag, watch[w].off, rd32(watch[w].base + watch[w].off));
			plog_str("hold ");
			plog_str(tag);
			plog_str("\n");
		}
		wait_ms(500);
	}
	plog_str("hold done\n");
}


/* DSC-wrap enable + first observation of the ovl2 blend/outproc/dlo12
 * blocks (bases from the handover DT: blender0 0x3e0b0000, outproc0
 * 0x3e130000, ovl2-dlo-async12 0x3e000000). LK's module-enable idiom
 * (lk 0x6deec) is [base] |= 1; dscw+0x0 read 0 in every dump so far —
 * the DSC has config (0x24=0x80890013 ...) but no enable. LK's mutex MOD
 * contains no blender/outproc at all: the logo path likely runs
 * EXDMA5 -> DLO_ASYNC0 (dl mout 0xef8 bit 0x100, already written and
 * sticking) -> DSC -> DSI, so the DSC enable is the prime suspect. */
/* Pixel-movement proof: the RDMA-style debug counters (named dump table:
 * 0xf0 R_IN_PXL_CNT, 0xf4 R_IN_LINE_CNT, 0xf8 R_OUT_PXL_CNT, 0xfc
 * R_OUT_LINE_CNT) tell whether EXDMA5 actually moves pixels. If
 * OUT_PXL_CNT stays 0 the block never emits — fetch fault (SMMU?) — and
 * every downstream fix is moot. Sampled before/after triggers. */
void display_exdma_counters(const char *tag)
{
	/* pan38: +0xf0..0xfc are NOT counters — the mt6991/mt6993 register
	 * table (platform/mtk_ovl_exdma_reg.h) puts L0_Y2R_PARA_* there,
	 * which stay zero whether or not the engine runs. pan37 read that
	 * range and wrongly concluded "the fetch never started".
	 * The real evidence registers are:
	 *   0x234 GREQ_LAYER_CNT  outstanding requests (not total fetches)
	 *   0x230 SMI_DBG         SMI path state
	 *   0x208 GDRDY_PRD       GDRDY period
	 *   0x210/0x220 RDMA0_BUF_LOW/HIGH   read buffer watermark
	 *   0x270 CRC             raw checksum; zero alone is inconclusive
	 *   0x23C DEBUG_FLAG
	 * o0e3 = 0x32960000 = disp-ovl0-exdma3, the configured source. */
	uint64_t oe3 = 0x32960000UL;
	/* pan39: the fetch-enable state. RDMA0_CTRL bit0 (FLD_RDMA_EN) is
	 * the bit layer_on() sets; if it is 0 nothing else here can move.
	 * FIFO_USED_SIZE in bits 27:16 of the same word is the live read
	 * buffer occupancy — the single most direct "DRAM bytes are
	 * arriving" signal this engine exposes. */
	dump_line(tag, 0x100, rd32(oe3 + 0x100));	/* RDMA0_CTRL */
	dump_line(tag, 0x1f4, rd32(oe3 + 0x1f4));	/* BURST_CON1 DDR bits */
	dump_line(tag, 0x200, rd32(oe3 + 0x200));	/* DUMMY_REG EXT_DDR */
	dump_line(tag, 0x014, rd32(oe3 + 0x014));	/* DATAPATH_CON */
	dump_line(tag, 0x0ff0, rd32(oe3 + 0xff0));	/* MOUT */
	dump_line(tag, 0x108, rd32(oe3 + 0x108));	/* GMC_SETTING1 */
	dump_line(tag, 0x110, rd32(oe3 + 0x110));	/* FIFO_CTRL */
	dump_line(tag, 0x1e0, rd32(oe3 + 0x1e0));	/* GMC_SETTING2 */
	dump_line(tag, 0x1f8, rd32(oe3 + 0x1f8));	/* GREQ_NUM */
	dump_line(tag, 0x20c, rd32(oe3 + 0x20c));	/* ULTRA_SRC */
	dump_line(tag, 0x234, rd32(oe3 + 0x234));
	dump_line(tag, 0x230, rd32(oe3 + 0x230));
	dump_line(tag, 0x208, rd32(oe3 + 0x208));
	dump_line(tag, 0x210, rd32(oe3 + 0x210));
	dump_line(tag, 0x220, rd32(oe3 + 0x220));
	dump_line(tag, 0x270, rd32(oe3 + 0x270));
	dump_line(tag, 0x23c, rd32(oe3 + 0x23c));
	dump_line(tag, 0x010, rd32(oe3 + 0x010));
	/* pan40: the kernel's own "did a G-REQ reach the fabric" counters.
	 * mtk_drm_ddp.c:2477 defines them in the ovlsys CONFIG block
	 * (0x32900000, the same block the crossbars live in):
	 *   MT6993_DISP_REG_OVLSYS_SMI_LARB0_GREQ = 0x330
	 *   MT6993_DISP_REG_OVLSYS_SMI_LARB1_GREQ = 0x334
	 * and its ovlsys dump (:42697) prints exactly these. EXDMA3 hangs
	 * off smi_larb1, so 0x334 is the one that must move. Also the
	 * DL_VALID/DL_READY handshake pair (:2469/:2473), which is how the
	 * kernel decides the ovlsys-to-dispsys link is live. */
	dump_line(tag, 0x330, rd32(0x32900000UL + 0x330));
	dump_line(tag, 0x334, rd32(0x32900000UL + 0x334));
	dump_line(tag, 0xaa8, rd32(0x32900000UL + 0xaa8));
	dump_line(tag, 0xab4, rd32(0x32900000UL + 0xab4));
	/* ovlsys clock-gate words: a gated module cannot issue a request.
	 * MT6993: CG_CON0 0xAC8, CG_CON1 0xAD4 (mtk_drm_ddp.c:2403). The
	 * shim's own display_lk_ungate() wrote 0xAD0/0xADC — one word off
	 * each — so these two reads say whether the real gates are open. */
	dump_line(tag, 0xac8, rd32(0x32900000UL + 0xac8));
	dump_line(tag, 0xad4, rd32(0x32900000UL + 0xad4));
	/* pan41: OVL_EXDMA_RDMA0_DBG (0x24c). This is the register the
	 * kernel's own dump decodes (mtk_disp_ovl_exdma.c:5192
	 * ovl_print_ovl_rdma_status), and it answers "is the read master
	 * issuing requests" in one word:
	 *   bits 2:0   warm_rst_cs
	 *   bit  3     layer_greq     <- the layer is asking for data
	 *   bits 27:4  out_data
	 *   bit  28    out_ready
	 *   bit  29    out_valid
	 *   bit  30    smi_busy
	 *   bit  31    smi_greq      <- a G-REQ is on the SMI bus
	 * These are instantaneous handshake bits, not cumulative activity
	 * counters. pan41 has smi_greq = 0 with FIFO_USED_SIZE = 1485:
	 * the engine can have buffered data and no outstanding request.
	 * Use the FIFO and FSM snapshots together, not zero G-REQ alone. */
	dump_line(tag, 0x24c, rd32(oe3 + 0x24c));
	/* pan41: OVL_EXDMA_FLOW_CTRL_DBG (0x240) — the engine's own FSM.
	 * This is the register that separates "the trigger path never
	 * starts a frame" from "a frame runs but no DRAM request leaves".
	 * Decode is ovl_exdma_printf_status() (mtk_disp_ovl_exdma.c:5132):
	 *   bits 9:0    ovl_fms_state  1=idle 2=wait_SOF 4=prepare
	 *                              8=reg_update 0x10=eng_clr
	 *                              0x20=eng_act(processing)
	 *   bit 10/11   addcon_idle / blend_idle
	 *   bit 12/13   out_valid / out_ready
	 *   bit 15      out_idle
	 *   bits 19:16  rdma_idle3..0
	 *   bit 20      rst
	 *   bit 21      trig
	 *   bit 23/24   frame_hwrst_done / frame_swrst_done
	 *   bit 25/26   frame_underrun / frame_done
	 *   bit 27/28   ovl_running / ovl_start
	 *   bit 30/31   reg_update / ovl_upd_reg
	 * A single idle snapshot cannot tell whether an earlier frame ran.
	 * pan41's eng_act state with out_ready = 0 is consistent with
	 * downstream backpressure, not proof of a missing source trigger. */
	dump_line(tag, 0x240, rd32(oe3 + 0x240));	/* FLOW_CTRL_DBG */
	dump_line(tag, 0x244, rd32(oe3 + 0x244));	/* ADDCON_DBG */
	/* Raw pure-frame monitors. The register table names these but does
	 * not establish their enable/reset/counting semantics; do not use
	 * a zero here as proof that no frame ran. */
	dump_line(tag, 0xa10, rd32(oe3 + 0xa10));	/* RDMA0_PURE_FRAME_MON0 */
	dump_line(tag, 0xa14, rd32(oe3 + 0xa14));	/* RDMA0_PURE_FRAME_MON1 */
	/* Security / address-domain path. A secure-marked layer, or a domain
	 * register that disagrees with the AID we cleared at 0xE3C, makes
	 * the SMMU drop the read before it reaches DRAM — which looks
	 * exactly like "enabled, framed, zero G-REQ". */
	dump_line(tag, 0xf00, rd32(oe3 + 0xf00));	/* SECURE_MISC */
	dump_line(tag, 0xfe4, rd32(oe3 + 0xfe4));	/* DOMAIN */
	dump_line(tag, 0xfec, rd32(oe3 + 0xfec));	/* SECURITY_DIS */
	dump_line(tag, 0x8f0, rd32(oe3 + 0x8f0));	/* SMI_2ND_CFG */
	dump_line(tag, 0x29c, rd32(oe3 + 0x29c));	/* BANK_CON */
	/* The layer-0 read master's own enable/status pair. RDMA0_CTRL
	 * bit0 is the enable; bits 27:16 are the live FIFO occupancy. */
	dump_line(tag, 0x028, rd32(oe3 + 0x028));	/* SHADOW_CTRL */
	dump_line(tag, 0x02c, rd32(oe3 + 0x02c));	/* SRC_CON */
	wdt_kick();
}

void display_dsc_enable(void)
{
	wr32(0x3e7f0000UL, rd32(0x3e7f0000UL) | 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	plog_hex("dsc con", rd32(0x3e7f0000UL));
	plog_str("dsc enabled\n");
}

void display_route_chain(int hold_ms)
{
	display_dsc_enable();
	display_exdma_counters("cta");
	/* The three never-observed ovl2 blocks. */
	dump_block(0x3e0b0000UL, "bld", 0x100);
	dump_block(0x3e130000UL, "opc", 0x100);
	dump_block(0x3e000000UL, "d12", 0x40);
	for (int i = 0; i < 6; i++) {
		dsi_trigger_lk();
		dsi_wait_idle();
		wdt_kick();
	}
	for (int t = 0; t < 4; t++) {
		wait_ms(hold_ms / 4);
		wdt_kick();
	}
	display_exdma_counters("ctb");
	plog_str("chain done\n");
}


void display_route_sweep2(int ms_per_try)
{
	static const uint32_t slots[2] = { 0xef8, 0xed8 };
	static const uint32_t bits[14] = {
		0x100, 0x10, 0x80, 0x4000, 0x8, 0x4, 0x40, 0x200,
		0x2, 0x2000, 0x400, 0x20, 0x800, 0x1000,
	};
	/* sweep2 on mmsys (0x3e300000) showed 0xef8/0xed8 writes do not stick
	 * (read back 0) while 0xee8 does — the ovl2/dl-side mout slots live in
	 * the second dispsys regmap at 0x3e700000 (the dli-async20/21/29/30
	 * nodes' reg), so sweep that base too. */
	static const uint64_t bases[2] = { 0x3e300000UL, 0x3e700000UL };
	for (unsigned base_i = 0; base_i < 2; base_i++) {
		for (unsigned s = 0; s < 2; s++) {
			for (unsigned b = 0; b < 14; b++) {
				uint32_t reg = slots[s];
				char tag[6] = { 'q', '0' + (char)base_i,
						'0' + (char)s, 'a' + (char)b, 0 };
				wr32(bases[base_i] + reg, bits[b]);
				__asm__ volatile("dsb sy" ::: "memory");
				for (int i = 0; i < 3; i++) {
					dsi_trigger_lk();
					dsi_wait_idle();
					wdt_kick();
				}
				dump_line(tag, reg,
					  rd32(bases[base_i] + reg));
				wait_ms(ms_per_try);
				wr32(bases[base_i] + reg, 0);
				__asm__ volatile("dsb sy" ::: "memory");
			}
		}
	}
	plog_str("sweep2 done\n");
}

void display_route_sweep(int ms_per_slot)
{
	for (unsigned k = 0; k < 14; k++) {
		uint32_t reg = 0xec8 + k * 8;
		char tag[8] = { 's', 'w', '0' + (char)(k / 10), '0' + (char)(k % 10), 0 };
		wr32(0x3e300000UL + reg, 2);
		__asm__ volatile("dsb sy" ::: "memory");
		for (int i = 0; i < 3; i++) {
			dsi_trigger_lk();
			dsi_wait_idle();
			wdt_kick();
		}
		dump_line(tag, reg, rd32(0x3e300000UL + reg));
		dump_line("sta", rd32(DSI_BASE + 0x0c), rd32(0x3e7f0000UL));
		wait_ms(ms_per_slot);
		wr32(0x3e300000UL + reg, 0);
		__asm__ volatile("dsb sy" ::: "memory");
	}
	plog_str("route sweep done\n");
}

void fb_fill(uint32_t color)
{
	/* The DT-declared videolfb: atag,videolfb-fb_base = 0xfc910000. */
	volatile uint64_t *p = (volatile uint64_t *)FB_BASE;
	uint64_t v = (uint64_t)color << 32 | color;
	unsigned words_per_line = FB_STRIDE / 8;

	for (unsigned y = 0; y < FB_H; y++) {
		volatile uint64_t *row = p + (uint64_t)y * words_per_line;
		for (unsigned x = 0; x < FB_STRIDE / 8; x++)
			row[x] = v;
		if (!(y & 0x1ff))
			wdt_kick();
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void dsi_trigger_lk(void)
{
	/* ddp_dsi_trigger sequence, reverse-engineered from lk_a.img.
	 * The +0x200 RMW from LK's trigger is GONE: the mt6993 kernel
	 * driver data puts dsi_time_con at +0x200 (TIMECON0, TIMECON1
	 * at +0x204) — LK's "frame packet literal 0x002c3909" was a TX
	 * timing word, and replaying it corrupted the timing every
	 * kernel-exact send below depends on. */
	wr32(DSI_BASE + 0x1d8, 0x00aaffffu);
	wr32(DSI_BASE + 0x1d8, rd32(DSI_BASE + 0x1d8) | 0x01000000u);
	wr32(DSI_BASE + 0x608,
	     (rd32(DSI_BASE + 0x608) & 0xffff00ffu) | 0x200u);
	wr32(DSI_INTSTA, rd32(DSI_INTSTA) & ~0x800u);
	wr32(DSI_START, rd32(DSI_START) & ~0x10u);
	wr32(DSI_START, rd32(DSI_START) | 0x10u);
}

uint32_t dsi_intsta(void)
{
	return rd32(DSI_INTSTA);
}

int dsi_wait_idle(void)
{
	unsigned t = 200;

	while (rd32(DSI_INTSTA) & 0x80000000u) {
		if (--t == 0)
			return -1;
		wait_ms(10);
	}
	return 0;
}
