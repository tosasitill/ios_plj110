/* Direct LK -> SWEETXNU payload -> XNU; no firmware-volume or EFI dependency. */
#include <stddef.h>
#include "display_lk.h"
#include "fbconsole.h"
#include "hw.h"
#include "loader.h"
#include "plog.h"
#if X9_USB_RECEIVER
#include "resident.h"
#endif

extern char _head[], _payload_start[], _payload_end[];
#if X9_UFS_ROOT
const char *x9_ufs_root_boot(uintptr_t,const void *,size_t);
const char *x9_ufs_native_boot(uintptr_t,const void *,size_t);
#endif
#if X9_UFS_PROBE
int x9_ufs_probe(uintptr_t);
int x9_ufs_dma_pending(void);
#endif

void shim_main(uint64_t dtb, uint64_t boot_el)
{
	plog_init();
	plog_str("x9 shim: direct XNU loader\n");
	plog_hex("boot EL", boot_el);
	plog_hex("load", (uintptr_t)_head);
	plog_hex("dtb", dtb);
	plog_hex("cntfrq", rsr("cntfrq_el0"));
	plog_hex("mpidr", rsr("mpidr_el1"));
	plog_hex("midr", rsr("midr_el1"));
	plog_hex("sctlr", rsr("sctlr_el1"));
	plog_hex("mmfr0", rsr("id_aa64mmfr0_el1"));
	plog_hex("t0", rsr("cntpct_el0"));
	wdt_kick();

	/* Preserve LK scanout; legacy diagnostics stay in display.c, unlinked. */
	if (display_lk_console_init((uintptr_t)dtb)) {
		fbconsole_init();
		plog_attach_console(fbconsole_putc, fbconsole_flush);
		plog_str("pan51: LK console active; proceeding to XNU checkpoint80\n");
	}

#if X9_UFS_PROBE && !X9_UFS_ROOT
    int storage_result=x9_ufs_probe((uintptr_t)dtb);
    plog_hex("UFS qualification result",(uint64_t)(int64_t)storage_result);
    if(x9_ufs_dma_pending()) {
        plog_str("UFS: DMA still pending; retaining buffer, XNU handoff blocked\n");
        plog_flush();
        for(;;) {wdt_kick();__asm__ volatile("yield");}
    }
#endif
	const char *why;
	unsigned granule = (rsr("id_aa64mmfr0_el1") >> 20) & 0xf;
	if (granule != 1 && granule != 2)
		why = "CPU does not support the kernel's 16K granule";
	else
#if X9_UFS_NATIVE
        why = x9_ufs_native_boot((uintptr_t)dtb,_payload_start,(size_t)(_payload_end-_payload_start));
#elif X9_UFS_ROOT
        why = x9_ufs_root_boot((uintptr_t)dtb,_payload_start,(size_t)(_payload_end-_payload_start));
#elif X9_USB_RECEIVER
		why = usb_receiver_boot((uintptr_t)dtb, _payload_start,
				 (size_t)(_payload_end - _payload_start));
#else
		why = loader_boot((uintptr_t)dtb, _payload_start,
				 (size_t)(_payload_end - _payload_start));

#endif
	plog_str("loader: ");
	plog_str(why);
#if X9_USB_RECEIVER || X9_UFS_ROOT
    plog_str("\nresident root stopped; keeping screen/log visible\n");
    plog_flush();
    for (;;) { wdt_kick(); __asm__ volatile("yield"); }
#else
	plog_str("\nreset\n");
	plog_flush();
	/* Long enough for the pstore mirror to settle before the warm reset that
	 * exposes it, short enough that a failed attempt is not a long wait. */
	wait_ms(8000);
	reboot();
#endif
}

void shim_exception(uint64_t esr, uint64_t elr, uint64_t far)
{
	plog_str("x9 shim EXCEPTION\n");
	plog_hex("esr", esr);
	plog_hex("elr", elr);
	plog_hex("far", far);
	plog_flush();
	/* No MMIO probes or exception skipping on the recovery path. */
	reboot();
}

/* Reached only if the EL2 setup in start.S traps before the ERET to EL1. */
void shim_el2_exception(uint64_t esr, uint64_t elr, uint64_t far)
{
	plog_str("x9 shim EL2 EXCEPTION\n");
	plog_hex("esr", esr);
	plog_hex("elr", elr);
	plog_hex("far", far);
	plog_flush();
	reboot();
}
