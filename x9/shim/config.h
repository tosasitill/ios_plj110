#pragma once

/* No guessed console-zone writes. Override both values only for a known zone. */
#ifndef X9_CONSOLE_BASE
#define X9_CONSOLE_BASE 0UL
#endif
#ifndef X9_CONSOLE_SIZE
#define X9_CONSOLE_SIZE 0UL
#endif

#if X9_CONSOLE_BASE && X9_CONSOLE_SIZE < 16
#error "X9_CONSOLE_SIZE must include a persistent_ram header and data"
#endif

/* Bring-up aid: validate the payload, log everything, then halt without
 * copying a single byte into kernel RAM or jumping. Separates "shim reached"
 * from "the copy corrupted something" without needing a working console. */
#ifndef X9_DIAG_ONLY
#define X9_DIAG_ONLY 0
#endif

/* Bring-up aid: mirror the log into the ramoops console zone, which survives
 * the warm reset, so a boot that never reaches Android's pstore driver can
 * still be read back with adb. Off by default: this writes DRAM the running
 * Android pstore driver owns. Enable only for a deliberate attempt.
 *
 * Layout, from the live X9 kernel parameters (mem 0xaec00000 + 0x310000,
 * console 0x200000, ftrace 0x1000, pmsg 0x100000, record 0x1000):
 * ramoops puts its dump records first, then the console zone. The same
 * arithmetic reproduces the proven X676C console at 0x4811f000, so the zone
 * here is 0xaec0f000..0xaee0f000 -- not the carveout base, which holds the
 * dump records the preloader and kernel keep for crash capture. */
#ifndef X9_PSTORE_MIRROR
#define X9_PSTORE_MIRROR 0
#endif
#ifndef X9_PSTORE_BASE
#define X9_PSTORE_BASE 0xaec0f000UL
#endif
#ifndef X9_PSTORE_SIZE
#define X9_PSTORE_SIZE 0x00200000UL
#endif
