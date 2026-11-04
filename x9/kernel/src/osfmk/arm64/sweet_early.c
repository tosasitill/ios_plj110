/*
 * Bootstrap console, selected only after PE_init_platform initializes the ADT.
 * start.s maps the boot video block as Device memory; arm_vm_init retains that
 * block at a reserved kernel VA. No framebuffer or persistent-RAM pointer is
 * usable before explicit initialization. A small local buffer retains the
 * first messages until an optional console record can be mapped.
 */
#include <arm64/sweet_early.h>

#if defined(SWEET)
#include <stdint.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <mach/vm_param.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>

_Static_assert(__builtin_offsetof(boot_args, Video) == 40, "start.s boot video offset");

static volatile uint32_t *early_fb;
static uint64_t early_fb_pa, early_fb_block;
static unsigned int early_width, early_height, early_stride;
static int early_initialized, board_x676c, board_x9;
static volatile uint32_t *x9_wdt;
static const char x9_kernel_tag[] __attribute__((used)) = "HelioDarwin-X9-0186\n";

/*
 * Bring-up aid: reset once the persistent record is durable, instead of
 * running on. A candidate that hangs after the jump can only be recovered by
 * a cold reset, which destroys the ramoops record before Android can snapshot
 * it; resetting here makes the record survive a warm reset instead. The X9
 * has no UART, so this is the only way to see how far a hung kernel got.
 * Off by default: on a working kernel it turns every boot into a reset loop.
 */
#ifndef SWEET_X9_DIAG_RESET
#define SWEET_X9_DIAG_RESET 0
#endif

#define PSCI_SYSTEM_RESET 0x84000009u

/* persistent_ram header: signature, next write offset, valid byte count. */
#define PSTORE_SIG 0x43474244u
#define PSTORE_HDR 12u
#define EARLY_LOG_CAP 0x4000u
static volatile uint32_t *pstore;
static uint32_t pstore_cap, pstore_next;
/*
 * Declared up here, not with the console state: once set, sweet_early_mark()
 * writes nothing at all, so checkpoints need to consult it to leave a record.
 */
static unsigned int early_disabled;
static char early_log[EARLY_LOG_CAP];
static uint32_t early_log_size;

/* Reserve a bounded, non-wrapping range. Safe even for nested console calls. */
static uint32_t
sweet_log_reserve(uint32_t *next, uint32_t cap, uint32_t *count)
{
	uint32_t at = __atomic_load_n(next, __ATOMIC_RELAXED);
	for (;;) {
		if (at >= cap) {
			*count = 0;
			return cap;
		}
		uint32_t n = *count < cap - at ? *count : cap - at;
		if (__atomic_compare_exchange_n(next, &at, at + n, 0,
		    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			*count = n;
			return at;
		}
	}
}

#include <arm64/sweet_diag_record.h>

#if SWEET_X9_DIAG_PMSG
/* Only the owner-captured ramoops pmsg zone; no search or fallback addresses. */
static volatile uint32_t *diag_pmsg;
static uint32_t diag_pmsg_next;
#define X9_DIAG_PMSG_CAP (0x80000u - PSTORE_HDR)
#endif

static void
sweet_diag_journal(unsigned int stage)
{
#if SWEET_X9_DIAG_PMSG
	volatile uint32_t *hdr = __atomic_load_n(&diag_pmsg, __ATOMIC_ACQUIRE);
	if (hdr == NULL || !board_x9 || stage > 99) {
		return;
	}
	uint32_t flags = 4u | (pstore != NULL ? 1u : 0u) |
	    (early_disabled ? 2u : 0u);
	char line[SWEET_DIAG_RECORD_SIZE];
	sweet_diag_record_encode(line, stage, flags);
	/* Reserve whole copies, so exhaustion cannot emit a truncated record. */
	uint32_t n = 5u * SWEET_DIAG_RECORD_SIZE;
	uint32_t at = sweet_log_reserve(&diag_pmsg_next, X9_DIAG_PMSG_CAP, &n);
	volatile char *data = (volatile char *)hdr + PSTORE_HDR;
	uint32_t written = n - n % SWEET_DIAG_RECORD_SIZE;
	for (uint32_t i = 0; i < written; i++) {
		data[at + i] = line[i % SWEET_DIAG_RECORD_SIZE];
	}
	if (!written) {
		return;
	}
	uint32_t end = at + written;
	uint32_t old = __atomic_load_n((uint32_t *)&hdr[2], __ATOMIC_RELAXED);
	while (old < end && !__atomic_compare_exchange_n((uint32_t *)&hdr[2],
	    &old, end, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
	}
	/* CPU0-only diagnostic build; keep the non-wrapping ring header valid. */
	uint32_t size = __atomic_load_n((uint32_t *)&hdr[2], __ATOMIC_ACQUIRE);
	__atomic_store_n((uint32_t *)&hdr[1], size,
	    __ATOMIC_RELEASE);
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)stage;
#endif
}

static void
sweet_diag_pmsg_init(uint64_t console_base, uint32_t console_size)
{
#if SWEET_X9_DIAG_PMSG
	if (!board_x9 || console_base != 0xaec0f000ULL ||
	    console_size != 0x200000u || diag_pmsg != NULL) {
		return;
	}
	/* Pmsg is 1 MiB at 0xaee0f000. Use only its lower half; avoid ECC tail. */
	uint64_t pa = console_base + console_size;
	vm_offset_t offset = pa & PAGE_MASK;
	vm_offset_t mapping = ml_io_map_wcomb(pa - offset,
	    round_page((vm_size_t)0x80000u + offset));
	if (!mapping) {
		return;
	}
	volatile uint32_t *hdr = (volatile uint32_t *)(mapping + offset);
	uint32_t size = hdr[2], start = hdr[1];
	/* Never erase the shim's evidence to manufacture a fresh record. */
	if (hdr[0] != PSTORE_SIG || size > X9_DIAG_PMSG_CAP ||
	    start > X9_DIAG_PMSG_CAP ||
	    start != size) {
		return;
	}
	diag_pmsg_next = size;
	__atomic_store_n(&diag_pmsg, hdr, __ATOMIC_RELEASE);
	sweet_diag_journal(21);
#else
	(void)console_base;
	(void)console_size;
#endif
}

static int
sweet_compatible_has(const char *list, unsigned int size, const char *want)
{
	unsigned int i = 0;
	while (i < size) {
		unsigned int end = i;
		while (end < size && list[end]) {
			end++;
		}
		if (end == size) {
			return 0;
		}
		unsigned int j = 0;
		while (i + j < end && want[j] && list[i + j] == want[j]) {
			j++;
		}
		if (i + j == end && !want[j]) {
			return 1;
		}
		i = end + 1;
	}
	return 0;
}

int
sweet_early_is_x676c(void)
{
	return board_x676c;
}

int
sweet_early_is_x9(void)
{
	return board_x9;
}

/*
 * X9 feature switches that are proven on the device are on by default:
 * absent -> def on X9 (0 on every other board), "name=N" -> N, so each one
 * can still be turned off with name=0. Keeps the 607-byte boot line free.
 */
int
sweet_x9_arg(const char *name, int def)
{
	int v = 0;
	if (PE_parse_boot_argn(name, &v, sizeof(v))) {
		return v;
	}
	return board_x9 ? def : 0;
}

uint64_t
sweet_early_fb_block(void)
{
	return early_fb_block;
}

void
sweet_early_init(const boot_args *args)
{
	if (early_initialized) {
		return;
	}
	DTEntry root;
	void const *prop;
	unsigned int size;
	if (SecureDTLookupEntry(NULL, "/", &root) == kSuccess &&
	    SecureDTGetProperty(root, "compatible", &prop, &size) == kSuccess) {
		board_x676c = sweet_compatible_has(prop, size, "infinix,x676c");
		/* The X9 DT LK hands over carries only this SoC string at "/";
		 * it has no "oppo,x9". Match what the hardware actually reports. */
		board_x9 = sweet_compatible_has(prop, size, "mediatek,MT6993");
	}
	/* Conflicting board identities must never enable a legacy MMIO path. */
	if (board_x9) {
		board_x676c = 0;
	}
	early_initialized = 1;
	if (board_x9) {
		/* Also identifies a kernel with the X9 isolation patch to host tools. */
		sweet_pstore_append(x9_kernel_tag, sizeof(x9_kernel_tag) - 1);
	}
	if (args == NULL) {
		return;
	}
	const Boot_Video *video = &args->Video;
	uint64_t pa = video->v_baseAddr, stride = video->v_rowBytes;
	if ((video->v_depth & kBootVideoDepthMask) != 32 ||
	    !sweet_fb_layout_valid(pa, UINT64_MAX, video->v_width,
	    video->v_height, stride)) {
		return;
	}
	uint64_t bytes = stride * video->v_height;
	uint64_t block = pa & ~((uint64_t)SWEET_EARLY_BLOCK_SIZE - 1);
	/* start.s only patches the first V=P L1 entry, and never a RAM slot. */
	if (!block || (pa >> 36) || bytes > SWEET_EARLY_BLOCK_SIZE - (pa - block) ||
	    args->physBase > UINT64_MAX - args->memSize ||
	    (block < args->physBase + args->memSize &&
	    args->physBase < block + SWEET_EARLY_BLOCK_SIZE)) {
		return;
	}
	if (board_x9 && (pa != 0xfc910000ULL || video->v_width != 1256 ||
	    video->v_height != 2760 || stride != 5120)) {
		return;
	}
	early_fb_pa = pa;
	early_fb_block = block;
	early_width = (unsigned int)video->v_width;
	early_height = (unsigned int)video->v_height;
	early_stride = (unsigned int)(stride / 4);
	early_fb = (volatile uint32_t *)(uintptr_t)pa;
}

void
sweet_pstore_append(const char *buf, int len)
{
	if (buf == NULL || len <= 0) {
		return;
	}
	uint32_t n = (uint32_t)len;
	volatile uint32_t *hdr = __atomic_load_n(&pstore, __ATOMIC_ACQUIRE);
	if (hdr == NULL) {
		uint32_t at = sweet_log_reserve(&early_log_size, EARLY_LOG_CAP, &n);
		for (uint32_t i = 0; i < n; i++) {
			early_log[at + i] = buf[i];
		}
		return;
	}
	uint32_t at = sweet_log_reserve(&pstore_next, pstore_cap, &n);
	volatile char *data = (volatile char *)hdr + PSTORE_HDR;
	for (uint32_t i = 0; i < n; i++) {
		data[at + i] = buf[i];
	}
	if (!n) {
		return;
	}
	/* Normal-NC mapping: atomics here must not target Device memory. */
	uint32_t end = at + n;
	uint32_t old_size = __atomic_load_n((uint32_t *)&hdr[2], __ATOMIC_RELAXED);
	while (old_size < end && !__atomic_compare_exchange_n((uint32_t *)&hdr[2],
	    &old_size, end, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
	}
	for (;;) {
		uint32_t size = __atomic_load_n((uint32_t *)&hdr[2], __ATOMIC_ACQUIRE);
		uint32_t start = size == pstore_cap ? 0 : size;
		uint32_t old_start = __atomic_load_n((uint32_t *)&hdr[1], __ATOMIC_RELAXED);
		if ((old_start == start || __atomic_compare_exchange_n((uint32_t *)&hdr[1],
		    &old_start, start, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) &&
		    __atomic_load_n((uint32_t *)&hdr[2], __ATOMIC_ACQUIRE) == size) {
			break;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void
sweet_pstore_putc(char c)
{
	sweet_pstore_append(&c, 1);
}

/*
 * noinline is load-bearing. When this was inlined into sweet_bootstrap_mark()
 * the stage comparison against SWEET_X9_CHECKPOINT_STAGE compiled to a test
 * against zero, because the stage comes from a table the compiler cannot fold
 * -- the resulting branch never fired, so a bootstrap checkpoint could not
 * reset even when it was the selected one. Keep the compare in one place.
 */
__attribute__((noinline)) void
sweet_diag_checkpoint(unsigned int stage, const char *label)
{
	sweet_diag_journal(stage);
	sweet_early_mark(stage, label);
	/*
	 * sweet_early_mark() goes silent after sweet_early_disable(), and on X9 that
	 * happens right after the kprintf milestone. Every record therefore stopped
	 * at the last mark before the disable, even though execution continued: a
	 * build that reset at 49 showed its final line as "SWEET EARLY 20". Write the
	 * label straight to the persistent record so each checkpoint proves itself.
	 */
	if ((board_x9 || SWEET_X9_DIAG_RESET) && early_disabled && __atomic_load_n(
	    (volatile uint32_t * volatile *)&pstore, __ATOMIC_ACQUIRE) != NULL) {
		int len = 0;
		while (label[len]) {
			len++;
		}
		sweet_pstore_append("CKPT ", sizeof("CKPT ") - 1);
		sweet_pstore_append(label, len);
		sweet_pstore_append("\n", 1);
	}
#if SWEET_X9_DIAG_RESET
	/*
	 * The reset has to come from the kernel. A candidate that hangs can only be
	 * recovered with a forced restart, and that is a cold reset: DRAM is
	 * reinitialized and the record is gone before Android can snapshot it. A
	 * build that only recorded milestones and kept running was tried for this
	 * reason and produced no record at all.
	 */
	if (stage != SWEET_X9_CHECKPOINT_STAGE) {
		return;
	}
	/*
	 * Refuse to touch the record before it is mapped: those milestones run out
	 * of local buffers, and sweet_early_mark already handled them.
	 */
	if (__atomic_load_n((volatile uint32_t * volatile *)&pstore,
	    __ATOMIC_ACQUIRE) == NULL) {
		return;
	}
	sweet_pstore_append("diag: pstore durable, requesting PSCI reset\n",
	    sizeof("diag: pstore durable, requesting PSCI reset\n") - 1);
	__asm__ volatile("dsb sy" ::: "memory");
	{
		register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
		__asm__ volatile("smc #0" : "+r"(x0) ::
		    "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		    "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		    "memory");
	}
#endif
}

/*
 * A kernel_bootstrap() stage, by name. Numbers are fixed per stage rather than
 * assigned by call order: kernel_bootstrap_log() is inlined at 40+ call sites
 * and is also reached from the startup-entry loop, so a counter would depend on
 * inlining and would drift between builds. An unknown name is ignored, which
 * keeps stages that are not interesting out of the record.
 */
struct sweet_bootstrap_stage {
	const char *name;
	unsigned int stage;
};

static const struct sweet_bootstrap_stage sweet_bootstrap_stages[] = {
	{ "vm_mem_bootstrap", 60 },
	{ "console_init", 61 },
	{ "sched_init", 62 },
	{ "PMAP_ACTIVATE_KERNEL", 63 },
	{ "machine_init", 64 },
	{ "clock_init", 65 },
	{ "task_init", 66 },
	{ "thread_init", 67 },
	{ "kernel_thread_create", 68 },
	{ "sched_startup", 69 },
	{ "thread_daemon_init", 70 },
	{ "clock_service_create", 71 },
	{ "device_service_create", 72 },
	{ "PE_init_iokit", 73 },
	{ "early_boot_ready", 74 },
	{ "interrupts_enabled", 75 },
	{ "screen_acquired", 76 },
	{ "commpage_ready", 77 },
	{ "trust_cache_init", 78 },
	{ "machine_lockdown", 79 },
	{ "bsd_init", 80 },
	{ "bsd_entered", 81 },
	{ "bsd_proc0_ready", 82 },
	{ "bsd_buffers_ready", 83 },
	{ "bsd_vfs_ready", 84 },
	{ "bsd_filesystems_registered", 85 },
	{ "bsd_mbuf_ready", 86 },
	{ "bsd_posix_ready", 87 },
	{ "bsd_network_ready", 88 },
	{ "bsd_sysctl_ready", 89 },
	{ "bsd_autoconf_enter", 90 },
	{ "bsd_autoconf_ready", 91 },
	{ "bsd_net_clients_ready", 92 },
	{ "bsd_calendar_ready", 93 },
	{ "bsd_root_select_enter", 99 },
};

/*
 * Diagnostic probe that skips sweet_early_mark(): the framebuffer text path is
 * the one thing a bootstrap checkpoint does that arm_init milestones do not do
 * at that point. Used to tell "the checkpoint hung" apart from "the checkpoint
 * was never reached".
 */
void
sweet_diag_probe(unsigned int stage, const char *label)
{
	(void)label;
#if SWEET_X9_DIAG_RESET
	if (stage != SWEET_X9_CHECKPOINT_STAGE) {
		return;
	}
	/*
	 * No pstore guard here, unlike sweet_diag_checkpoint(). The build that
	 * used the guarded version did not reset, which cannot distinguish "never
	 * reached this function" from "reached it with no mapped record". This
	 * variant resets either way: a reset proves control got here, and the
	 * presence or absence of the appended line tells the record's state.
	 */
	if (__atomic_load_n((volatile uint32_t * volatile *)&pstore,
	    __ATOMIC_ACQUIRE) != NULL) {
		sweet_pstore_append("diag: probe reached, requesting PSCI reset\n",
		    sizeof("diag: probe reached, requesting PSCI reset\n") - 1);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	{
		register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
		__asm__ volatile("smc #0" : "+r"(x0) ::
		    "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		    "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		    "memory");
	}
#else
	(void)stage;
#endif
}

/*
 * Bare entry probe: one compare against the selected milestone and an SMC, with
 * no call, no stack frame and no access to the persistent record. If a normal
 * checkpoint never fires at a function's first statement, this separates "the
 * function was entered" from "the checkpoint machinery itself failed".
 */
void
sweet_diag_entry_reset(unsigned int stage)
{
#if SWEET_X9_DIAG_RESET
	if (stage != SWEET_X9_CHECKPOINT_STAGE) {
		return;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	{
		register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
		__asm__ volatile("smc #0" : "+r"(x0) ::
		    "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		    "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		    "memory");
	}
#else
	(void)stage;
#endif
}

void
sweet_bootstrap_mark(const char *message)
{
	if (message == NULL) {
		return;
	}
	for (unsigned int i = 0;
	    i < sizeof(sweet_bootstrap_stages) / sizeof(sweet_bootstrap_stages[0]);
	    i++) {
		const char *want = sweet_bootstrap_stages[i].name;
		const char *got = message;
		while (*want && *want == *got) {
			want++;
			got++;
		}
		if (!*want && !*got) {
			/* Independent breadcrumb first; reset itself stays console-free. */
			if (sweet_bootstrap_stages[i].stage == SWEET_X9_CHECKPOINT_STAGE) {
				sweet_diag_journal(sweet_bootstrap_stages[i].stage);
			}
			sweet_diag_entry_reset(sweet_bootstrap_stages[i].stage);
			sweet_diag_checkpoint(sweet_bootstrap_stages[i].stage, message);
			return;
		}
	}
}

void
sweet_watchdog_pet(void)
{
	volatile uint32_t *wdt = __atomic_load_n(&x9_wdt, __ATOMIC_ACQUIRE);
	if (wdt != NULL) {
		wdt[0x08 / 4] = 0x1971;
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

void
sweet_early_late_init(void)
{
	if (!early_initialized) {
		return;
	}
	if (board_x9 && x9_wdt == NULL) {
		volatile uint32_t *wdt = (volatile uint32_t *)ml_io_map(0x1c010000, 0x1000);
		__atomic_store_n(&x9_wdt, wdt, __ATOMIC_RELEASE);
		sweet_watchdog_pet();
	}
	if (pstore != NULL) {
		return;
	}
	uint64_t pa = 0;
	uint32_t size = 0;
	if (board_x676c) {
		pa = SWEET_PSTORE_PA;
		size = SWEET_PSTORE_SIZE;
	} else if (board_x9) {
		DTEntry chosen;
		void const *base_prop, *size_prop;
		unsigned int base_len, size_len;
		if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
		    SecureDTGetProperty(chosen, "x9-console-base", &base_prop, &base_len) != kSuccess ||
		    SecureDTGetProperty(chosen, "x9-console-size", &size_prop, &size_len) != kSuccess ||
		    base_len != sizeof(uint64_t) || size_len != sizeof(uint32_t)) {
			return;
		}
		pa = *(const uint64_t *)base_prop;
		size = *(const uint32_t *)size_prop;
		if ((pa & 3) || pa < 0xaec00000ULL || pa >= 0xaef10000ULL ||
		    size < 16 || size > 0xaef10000ULL - pa) {
			return;
		}
	}
	if (!pa || size < 16) {
		return;
	}
	/* Normal-NC, not Device. Align the physical request to reserve every
	 * early-I/O VA page, including a partial first page on X676C. */
	vm_offset_t offset = pa & PAGE_MASK;
	vm_size_t map_size = round_page((vm_size_t)size + offset);
	vm_offset_t mapping = ml_io_map_wcomb(pa - offset, map_size);
	if (!mapping) {
		return;
	}
	volatile uint32_t *hdr = (volatile uint32_t *)(mapping + offset);
	pstore_cap = size - PSTORE_HDR;
	if (hdr[0] != PSTORE_SIG || hdr[2] > pstore_cap ||
	    (hdr[2] < pstore_cap && hdr[1] != hdr[2])) {
		hdr[0] = PSTORE_SIG;
		hdr[1] = 0;
		hdr[2] = 0;
	}
	if (hdr[2] == pstore_cap && hdr[1] >= pstore_cap) {
		hdr[1] = 0;
	}
	pstore_next = hdr[2];
	__atomic_store_n(&pstore, hdr, __ATOMIC_RELEASE);
	sweet_diag_pmsg_init(pa, size);
	/* Called on the boot CPU with interrupts masked, before SMP startup. */
	sweet_pstore_append(early_log, (int)early_log_size);

	/*
	 * The diagnostic reset is not here. sweet_diag_checkpoint() owns it, so
	 * the behavior is chosen in one place; a copy at this point would always
	 * run first and make every later milestone unreachable.
	 */
}
#define EARLY_FB early_fb
#define EARLY_WIDTH early_width
#define EARLY_HEIGHT early_height
#define EARLY_STRIDE early_stride
#define EARLY_SCALE 2u
#define EARLY_CELL_W (8u * EARLY_SCALE)
#define EARLY_CELL_H (16u * EARLY_SCALE)
#define EARLY_ROWS ((EARLY_HEIGHT - 32u) / EARLY_CELL_H)
#define EARLY_COLS (EARLY_WIDTH / EARLY_CELL_W)

extern unsigned char iso_font[256 * 16];
extern uint32_t fbcon_shadow[SWEET_FBCON_MAX_PIXELS];

/* Initialized data must remain writable until disable, including panics. */
static unsigned int early_row, early_col;
static unsigned int early_started, early_panicking;

static void
sweet_early_clear_row(void)
{
	for (unsigned int y = 0; y < EARLY_CELL_H; y++) {
		volatile uint32_t *line = EARLY_FB + (early_row * EARLY_CELL_H + y) * EARLY_STRIDE;
		uint32_t *line_shadow = fbcon_shadow + (early_row * EARLY_CELL_H + y) * EARLY_WIDTH;
		for (unsigned int x = 0; x < EARLY_WIDTH; x++) {
			line_shadow[x] = 0xff000000u;
			line[x] = 0xff000000u;
		}
	}
}

/* Shift the whole text area up one glyph row and clear the bottom using the
 * DRAM shadow buffer (zero uncached VRAM reads). */
static void
sweet_early_scroll(void)
{
	unsigned int top = (EARLY_ROWS - 1) * EARLY_CELL_H;
	for (unsigned int y = 0; y < top; y++) {
		volatile uint32_t *dst = EARLY_FB + y * EARLY_STRIDE;
		uint32_t *dst_shadow = fbcon_shadow + y * EARLY_WIDTH;
		const uint32_t *src_shadow = fbcon_shadow + (y + EARLY_CELL_H) * EARLY_WIDTH;
		for (unsigned int x = 0; x < EARLY_WIDTH; x++) {
			uint32_t px = src_shadow[x];
			dst_shadow[x] = px;
			dst[x] = px;
		}
	}
	early_row = EARLY_ROWS - 1;
	sweet_early_clear_row();
}

void
sweet_early_putc(char c)
{
	sweet_pstore_putc(c);
	if (early_disabled || early_fb == NULL) {
		return;
	}
	if (!early_started) {
		early_started = 1;
		/* Clear the whole panel once, so the console starts on a clean
		 * black screen instead of over the shim's boot splash. */
		for (unsigned int y = 0; y < EARLY_HEIGHT; y++) {
			volatile uint32_t *line = EARLY_FB + y * EARLY_STRIDE;
			uint32_t *line_shadow = fbcon_shadow + y * EARLY_WIDTH;
			for (unsigned int x = 0; x < EARLY_WIDTH; x++) {
				line_shadow[x] = 0xff000000u;
				line[x] = 0xff000000u;
			}
		}
	}
	if (c == '\r') {
		early_col = 0;
		return;
	}
	if (c == '\n' || early_col == EARLY_COLS) {
		early_col = 0;
		if (early_row + 1 >= EARLY_ROWS) {
			sweet_early_scroll();
		} else {
			early_row++;
			sweet_early_clear_row();
		}
		if (c == '\n') {
			return;
		}
	}
	const unsigned char *glyph = iso_font + (unsigned char)c * 16;
	for (unsigned int gy = 0; gy < 16; gy++) {
		for (unsigned int sy = 0; sy < EARLY_SCALE; sy++) {
			unsigned int y = early_row * EARLY_CELL_H + gy * EARLY_SCALE + sy;
			volatile uint32_t *line = EARLY_FB + y * EARLY_STRIDE + early_col * EARLY_CELL_W;
			uint32_t *line_shadow = fbcon_shadow + y * EARLY_WIDTH + early_col * EARLY_CELL_W;
			for (unsigned int gx = 0; gx < 8; gx++) {
				uint32_t pixel = (glyph[gy] & (1u << gx)) ? 0xffffffffu : 0xff000000u;
				for (unsigned int sx = 0; sx < EARLY_SCALE; sx++) {
					line_shadow[gx * EARLY_SCALE + sx] = pixel;
					line[gx * EARLY_SCALE + sx] = pixel;
				}
			}
		}
	}
	early_col++;
}

static void
sweet_early_string(const char *s)
{
	while (*s) {
		sweet_early_putc(*s++);
	}
}

void
sweet_early_mark(unsigned int stage, const char *label)
{
	if (early_disabled || stage > SWEET_X9_MAX_STAGE) {
		return;
	}
	if (early_fb != NULL) {
		uint32_t color = 0xff00ff00u | ((stage & 7u) * 32u << 16);
		for (unsigned int y = EARLY_HEIGHT - 32; y < EARLY_HEIGHT; y++) {
			for (unsigned int x = stage * 48;
			    x < (stage + 1) * 48 && x < EARLY_WIDTH; x++) {
				EARLY_FB[y * EARLY_STRIDE + x] = color;
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
	}
	sweet_early_string("SWEET EARLY ");
	if (stage >= 10) {
		sweet_early_putc((char)('0' + stage / 10));
	}
	sweet_early_putc((char)('0' + stage % 10));
	sweet_early_string(": ");
	sweet_early_string(label);
	sweet_early_putc('\n');
}

void
sweet_early_panic(const char *fmt, va_list *args)
{
	if (early_disabled || early_panicking) {
		return;
	}
	early_panicking = 1;
	sweet_early_string("\nSWEET EARLY PANIC: ");
	va_list copy;
	va_copy(copy, *args);
	_doprnt(fmt, &copy, sweet_early_putc, 16);
	va_end(copy);
	sweet_early_putc('\n');
	__asm__ volatile("dsb sy" ::: "memory");
}

void
sweet_early_relocate(uint64_t base)
{
	sweet_early_mark(8, "leaving bootstrap TTBR0");
	if (early_fb_block) {
		early_fb = (volatile uint32_t *)(uintptr_t)(base + early_fb_pa - early_fb_block);
	}
	/* Persistent console memory has no bootstrap alias; mapped in late_init. */
	__asm__ volatile("dsb sy" ::: "memory");
	sweet_early_mark(9, "kernel framebuffer VA live");
}

void
sweet_early_disable(void)
{
	early_disabled = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}
#endif /* SWEET */
