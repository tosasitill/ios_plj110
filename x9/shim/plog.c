#include "plog.h"
#include "config.h"

#define PRZ_SIG 0x43474244U	/* PERSISTENT_RAM_SIG, "DBGC" */
#define PRZ_HDR 12U
#define LOCAL_SIZE 0x4000

static char logbuf[LOCAL_SIZE];
static uint32_t pos;
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
	if (screen_put && !screen_hidden) screen_put(c);
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

void plog_init(void)
{
	screen_put = 0; screen_flush = 0; screen_hidden = 0;
	pos = 0;
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
	pos = 0;
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
 /* Replay early human-readable lines, keeping CRC MMIO records in pstore. */
 for (unsigned i = 0; i < pos;) {
  unsigned hide = diagnostic_line(logbuf + i);
  do {
   char c = logbuf[i++];
   if (!hide && screen_put) screen_put(c);
   if (c == '\n') break;
  } while (i < pos);
 }
 if (screen_flush) screen_flush();
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
 if (newline && !screen_hidden && screen_flush) screen_flush();
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
	if (screen_flush) screen_flush();
}

void plog_flush(void)
{
	if (screen_flush) screen_flush();
	__asm__ volatile("dsb sy" ::: "memory");
}
