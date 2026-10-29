#pragma once
#include <stdint.h>

/* A local RAM log is always available. Persistent output is opt-in at build. */
void plog_init(void);
/* Continue the record this boot already started instead of resetting it. The
 * shim and the stub it jumps to are two stages of one boot, so their lines
 * belong in one record. Falls back to a reset when no valid header is there. */
void plog_resume(void);
void plog_str(const char *s);
void plog_hex(const char *label, uint64_t v);
void plog_flush(void);

void plog_attach_console(void (*write)(char), void (*flush)(void));
void plog_detach_console(void);
