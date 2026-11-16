/* Board-worker readiness and explicit ownership lifecycle for launch/BlueTool.
 * These calls never send userland HCI, reset or download controller firmware. */
#ifndef X9_BOOTSTRAP16_H
#define X9_BOOTSTRAP16_H
#include <stdint.h>
int x9_bootstrap16_status(void);
int x9_bootstrap16_wait(uint32_t seconds);
/* Synchronous worker request. Success is followed by authoritative readback;
 * permission errors, rejected sleeps and uncertain ownership stay failures.
 * sleep requires a closed/empty native transport, as enforced by the kernel. */
int x9_bootstrap16_resume(void);
int x9_bootstrap16_sleep(void);
/* stderr plus kern.x9_bt_note (launchd jobs' stderr is /dev/null). */
void x9_bt_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
