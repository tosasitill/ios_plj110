#include "loader.h"
#include "hw.h"
#include "plog.h"

/* For loader bring-up only; no claim to be an XNU kernel or UI. */
void stub_main(uint64_t args)
{
	const volatile struct boot_args *ba = (const volatile struct boot_args *)args;
	/* Second stage of the same boot: keep the shim's lines, append ours. */
	plog_resume();
	plog_str("X9 STUB: direct kernel entry\n");
	plog_hex("boot_args", args);
	plog_hex("Revision", ba->Revision);
	plog_hex("Version", ba->Version);
	plog_hex("physBase", ba->physBase);
	plog_hex("memSize", ba->memSize);
	plog_hex("topOfKernelData", ba->topOfKernelData);
	plog_hex("framebuffer", ba->Video.v_baseAddr);
	plog_hex("stride", ba->Video.v_rowBytes);
	plog_hex("deviceTreeP", ba->deviceTreeP);
	plog_str("X9 STUB: reset\n");
	plog_flush();
	/* The warm reset is what exposes the ramoops record to the next Android
	 * boot; give the mirror time to settle first. */
	wait_ms(8000);
	reboot();
}
