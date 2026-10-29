#pragma once
#include <stdint.h>

/* MT6993 addresses from the X9 captures; never use the MT6789 register map. */
#define TOPRGU_BASE	0x1c010000UL
#define WDT_RESTART	(TOPRGU_BASE + 0x08)
#define WDT_RESTART_KEY	0x1971U
#define GICD_BASE	0x06800000UL
#define GICR_BASE	0x06880000UL
#define UFSHCI_BASE	0x16810000UL
#define DSI_BASE	0x3e730000UL

/* LK's linear buffer, not proof that the DSC command-mode panel is updating. */
#define FB_BASE		0xfc910000UL
#define FB_W		1256
#define FB_H		2760
#define FB_STRIDE	5120
#define FB_SIZE		0x03660000UL

/* One captured contiguous System RAM run. Do not hand reserved holes to XNU. */
#define KERNEL_RAM_LO	0x140000000UL
#define KERNEL_RAM_HI	0x160000000UL
/* Experimental 4GiB subrange of captured 0x200000000..0x33fe00000 System RAM. */
#define KERNEL_LARGE_RAM_LO 0x200000000UL
#define KERNEL_LARGE_RAM_HI 0x33e000000UL
#define PAGE_16K	0x4000UL

#define PSTORE_BASE	0xaec00000UL
#define PSTORE_SIZE	0x00310000UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define CNTFRQ_FALLBACK		1000000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t counter_frequency(void);
void wdt_kick(void);
void wait_ms(unsigned ms);
void __attribute__((noreturn)) reboot(void);
void __attribute__((noreturn)) xnu_enter(uint64_t entry, uint64_t boot_args);
