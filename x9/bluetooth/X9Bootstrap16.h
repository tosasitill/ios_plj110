/* Read-only readiness for launch/BlueTool. No HCI command or power request. */
#ifndef X9_BOOTSTRAP16_H
#define X9_BOOTSTRAP16_H
#include <stdint.h>
int x9_bootstrap16_status(void);
int x9_bootstrap16_wait(uint32_t seconds);
#endif
