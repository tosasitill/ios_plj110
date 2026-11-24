#include "plog.h"
#include "config.h"

#define PRZ_SIG 0x43474244U	/* PERSISTENT_RAM_SIG, "DBGC" */
#define PRZ_HDR 12U
#define LOCAL_SIZE 0x4000

static char logbuf[LOCAL_SIZE];
static uint32_t pos;
#define USB_RING_SIZE (128u * 1024u)
static char usb_ring[USB_RING_SIZE];
static uint32_t usb_total;
static void (*screen_put)(char);
static void (*screen_flush)(void);
static unsigned screen_hidden;

static int diagnostic_line(const char *s)
{
 return s[0] == 'l' && s[1] == 'k' && s[2] == '5' && s[3] == '0' && s[4] == ':';
}


#if X9_PSTORE_MIRROR
/* Triple-redundant record: the console record's head is overwritten by
 * Android-side writers on some boots and one bad header byte drops the whole
 * file, so every character also lands in the pmsg zone (appended through the
 * pstore layer by Android, never observed losing its header) and in the first
 * dmesg dump record (a different zone the TEE does not touch). */
static volatile uint32_t *const prz = (volatile uint32_t *)X9_PSTORE_BASE;
#define PRZ_CAP (X9_PSTORE_SIZE - PRZ_HDR)
static uint32_t prz_pos;
/* pmsg sits right after the console zone: ramoops allocates dump, console,
 * pmsg, then ftrace last. */
static volatile uint32_t *const pmsg =
    (volatile uint32_t *)(X9_CONSOLE_BASE + X9_CONSOLE_SIZE);
#define PMSG_CAP (0x100000UL - PRZ_HDR)
static uint32_t pmsg_pos;
/* First dmesg dump record: the carveout starts 0xf000 before the console. */
static volatile uint32_t *const drec =
    (volatile uint32_t *)(X9_CONSOLE_BASE - 0xf000UL);
#define DREC_CAP (0x1000UL - PRZ_HDR)
static uint32_t drec_pos;
#endif
#if X9_CONSOLE_BASE
static volatile uint32_t *const con = (volatile uint32_t *)X9_CONSOLE_BASE;
#define CON_CAP (X9_CONSOLE_SIZE - PRZ_HDR)
static uint32_t con_pos;
#endif

static void put(char c)
{
 usb_ring[usb_total % USB_RING_SIZE]=c;
 if (usb_total != UINT32_MAX) usb_total++;
	/* Verbose bytes stay in RAM/USB/pstore; screen is stage-only. */
	if (pos != LOCAL_SIZE - 1) {
		logbuf[pos++] = c;
		logbuf[pos] = 0;
	}
#if X9_PSTORE_MIRROR
	if (prz_pos < PRZ_CAP) {
		*(volatile char *)(X9_PSTORE_BASE + PRZ_HDR + prz_pos) = c;
		prz_pos++;
		prz[1] = prz_pos;
		prz[2] = prz_pos;
	}
	if (pmsg_pos < PMSG_CAP) {
		*(volatile char *)((volatile char *)pmsg + PRZ_HDR + pmsg_pos) = c;
		pmsg_pos++;
		pmsg[1] = pmsg_pos;
		pmsg[2] = pmsg_pos;
	}
	if (drec_pos < DREC_CAP) {
		*(volatile char *)((volatile char *)drec + PRZ_HDR + drec_pos) = c;
		drec_pos++;
		drec[1] = drec_pos;
		drec[2] = drec_pos;
	}
#endif
#if X9_CONSOLE_BASE
	if (con_pos < CON_CAP) {
		*(volatile char *)(X9_CONSOLE_BASE + PRZ_HDR + con_pos) = c;
		con_pos++;
		con[1] = con_pos;
		con[2] = con_pos;
	}
#endif
}

#if X9_CONSOLE_BASE
/* The kernel continues this zone as a wrapping ramoops ring (sweet_early.c:
 * hdr[1]=start/next write, hdr[2]=valid size). A warm reset (TOPRGU timeout,
 * PSCI) keeps DRAM, so before this boot discards it, keep the newest bytes:
 * the USB console loses roughly the last KiB of a boot that hangs. */
#define PREV_TAIL 6144u
static char prev_tail[PREV_TAIL];
static uint32_t prev_len, prev_size, prev_start, prev_sig;
static void keep_previous_tail(void)
{
	prev_sig = con[0]; prev_start = con[1]; prev_size = con[2]; prev_len = 0;
	if (prev_sig != PRZ_SIG || prev_size > CON_CAP || prev_start > CON_CAP)
		return;
	uint32_t end = prev_size < CON_CAP ? prev_size : prev_start;
	uint32_t n = prev_size < PREV_TAIL ? prev_size : PREV_TAIL;
	uint32_t at = (end + CON_CAP - n) % CON_CAP;
	for (uint32_t i = 0; i < n; i++) {
		char c = *(volatile char *)(X9_CONSOLE_BASE + PRZ_HDR + at);
		prev_tail[i] = (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) ? c : '.';
		if (++at == CON_CAP) at = 0;
	}
	prev_len = n;
}
#endif
void plog_previous_boot(void)
{
	/* MT6993 TOPRGU (mt6993.dts watchdog@1c010000; hangdet.c NONRST_REG2
	 * 0x24 = boot mode[3:0], reboot flag[11], stage[31:29]). */
	volatile uint32_t *rgu = (volatile uint32_t *)0x1c010000UL;
	plog_hex("x9 prior reset TOPRGU MODE", rgu[0x00 / 4]);
	plog_hex("x9 prior reset TOPRGU LENGTH", rgu[0x04 / 4]);
	plog_hex("x9 prior reset TOPRGU STA", rgu[0x0c / 4]);
	plog_hex("x9 prior reset TOPRGU NONRST", rgu[0x20 / 4]);
	plog_hex("x9 prior reset TOPRGU NONRST2", rgu[0x24 / 4]);
#if 0 /* log diag8: the zone header did not change across two boots (shim and
       kernel writes not observed), so its tail is stale data, not the prior boot */
	plog_hex("x9 previous boot ring sig", prev_sig);
	plog_hex("x9 previous boot ring start", prev_start);
	plog_hex("x9 previous boot ring size", prev_size);
	if (!prev_len) {
		plog_str("x9 previous boot tail: none (cold reset or no record)\n");
		return;
	}
	plog_str("x9 previous boot tail BEGIN (DRAM kept across warm reset; lines are from the PREVIOUS boot)\n");
	for (uint32_t i = 0; i < prev_len; i++) put(prev_tail[i]);
	plog_str("\nx9 previous boot tail END\n");
#endif
}

void plog_init(void)
{
#if X9_CONSOLE_BASE
	keep_previous_tail();
#endif
	screen_put = 0; screen_flush = 0; screen_hidden = 0;
	pos = 0; usb_total = 0;
	logbuf[0] = 0;
#if X9_PSTORE_MIRROR
	/* Discards whatever Android left in the console zone. That record is the
	 * previous boot's log, so only enable this for a deliberate attempt. */
	prz_pos = 0;
	prz[0] = PRZ_SIG;
	prz[1] = 0;
	prz[2] = 0;
	pmsg_pos = 0;
	pmsg[0] = PRZ_SIG;
	pmsg[1] = 0;
	pmsg[2] = 0;
	drec_pos = 0;
	drec[0] = PRZ_SIG;
	drec[1] = 0;
	drec[2] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
#endif
#if X9_CONSOLE_BASE
	con_pos = 0;
	con[0] = PRZ_SIG;
	con[1] = con[2] = 0;
#endif
}

#if X9_PSTORE_MIRROR || X9_CONSOLE_BASE
/* Adopt the record the previous stage left in the zone. Both stages write the
 * same header, so appending keeps one boot in one record; a stale or implausible
 * header resets instead of appending onto someone else's bytes. */
static uint32_t resume_zone(volatile uint32_t *h, uint32_t cap)
{
	if (h[0] != PRZ_SIG || h[1] > cap || h[2] > cap || h[2] < h[1])
		return 0;
	return h[2];
}
#endif

void plog_resume(void)
{
	screen_put = 0; screen_flush = 0; screen_hidden = 0;
	pos = 0; usb_total = 0;
	logbuf[0] = 0;
#if X9_PSTORE_MIRROR
	prz_pos = resume_zone(prz, PRZ_CAP);
	prz[0] = PRZ_SIG;
	prz[1] = prz[2] = prz_pos;
	pmsg_pos = resume_zone(pmsg, PMSG_CAP);
	pmsg[0] = PRZ_SIG;
	pmsg[1] = pmsg[2] = pmsg_pos;
	drec_pos = resume_zone(drec, DREC_CAP);
	drec[0] = PRZ_SIG;
	drec[1] = drec[2] = drec_pos;
	__asm__ volatile("dsb sy" ::: "memory");
#endif
#if X9_CONSOLE_BASE
	con_pos = resume_zone(con, CON_CAP);
	con[0] = PRZ_SIG;
	con[1] = con[2] = con_pos;
#endif
}

void plog_attach_console(void (*write)(char), void (*flush)(void))
{
 screen_put = write; screen_flush = flush;

}

void plog_detach_console(void)
{
 screen_put = 0; screen_flush = 0;
}

void plog_str(const char *s)
{
 unsigned saved = screen_hidden, newline = 0;
 screen_hidden |= diagnostic_line(s);
 while (*s) {
  newline |= *s == '\n';
  put(*s++);
 }
 (void)newline;
 screen_hidden = saved;
}

void plog_hex(const char *label, uint64_t v)
{
	plog_str(label);
	plog_str("=0x");
	for (int i = 15; i >= 0; i--) {
		unsigned d = (v >> (i * 4)) & 0xf;
		put(d < 10 ? '0' + d : 'a' + d - 10);
	}
	put('\n');
}

void plog_flush(void)
{
	if (screen_flush) screen_flush();
	__asm__ volatile("dsb sy" ::: "memory");
}

uint64_t plog_position(void) { return usb_total; }
uint32_t plog_copy(uint64_t *cursor,char *out,uint32_t max)
{
 if(!cursor||!out||*cursor>usb_total)return 0;
 uint64_t earliest=usb_total>USB_RING_SIZE?usb_total-USB_RING_SIZE:0;
 if(*cursor<earliest)*cursor=earliest;
 uint32_t n=usb_total-(uint32_t)*cursor;if(n>max)n=max;
 for(uint32_t i=0;i<n;i++)out[i]=usb_ring[(*cursor+i)%USB_RING_SIZE];
 *cursor+=n;return n;
}

/* Explicit short stage messages are the only screen text. No replay/scroll
 * of verbose logs; keep the same message in every normal log channel. */
void plog_stage(const char *message)
{
 plog_str(message);
 if (screen_put) {
  const char *p = message;
  while (*p) screen_put(*p++);
  if (screen_flush) screen_flush();
 }
}