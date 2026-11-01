#include "hw.h"

uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

uint64_t counter_frequency(void)
{
	uint64_t freq = rsr("cntfrq_el0");
	return freq ? freq : CNTFRQ_FALLBACK;
}

extern void x9_shim_usb_poll(void);
void wdt_kick(void)
{
	wr32(WDT_RESTART, WDT_RESTART_KEY);
	x9_shim_usb_poll();
	__asm__ volatile("dsb sy" ::: "memory");
}

void wait_ms(unsigned ms)
{
	uint64_t freq = counter_frequency();
	uint64_t ticks = (freq / 1000) * ms;
	uint64_t ctl = rsr("cntkctl_el1");
	/* Select a counter event around 1 ms; EVNTI is limited to bits 0..15. */
	unsigned bit = 0;
	while (bit < 15 && (1UL << (bit + 1)) < freq / 1000)
		bit++;
	ctl = (ctl & ~(0xfUL << 4)) | ((uint64_t)bit << 4) | (1UL << 2);
	__asm__ volatile("msr cntkctl_el1, %0; isb" :: "r"(ctl) : "memory");
	isb();
	uint64_t start = rsr("cntpct_el0");
	while (rsr("cntpct_el0") - start < ticks) {
		wdt_kick();
		__asm__ volatile("wfe");
	}
}

void reboot(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	/* If reset is refused, stop feeding the watchdog. */
	for (;;)
		__asm__ volatile("wfe");
}
