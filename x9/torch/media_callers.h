/* SPDX-License-Identifier: MIT
 * Exact return addresses in the UUID-gated 20H392 CMCapture image. Both own
 * media bridges use one list so chaining cannot widen/lose flashlight scope.
 */
#ifndef X9_MEDIA_CALLERS_H
#define X9_MEDIA_CALLERS_H
#include <stdbool.h>
#include <stdint.h>
static bool x9_media_flashlight_caller(uintptr_t return_address, uintptr_t slide)
{
    static const uintptr_t callers[] = {
        0x1a20007d0, /* IsAvailable */
        0x1a20009bc, /* NotifyForCurrentState */
        0x1a2000abc, 0x1a2000ae8, 0x1a2000b04, 0x1a2000b2c, /* setupDevice */
        0x1a1f3e5a8, /* teardownDevice */
    };
    uintptr_t caller = return_address - slide;
    for (unsigned i = 0; i < sizeof(callers) / sizeof(callers[0]); i++)
        if (caller == callers[i]) return true;
    return false;
}
#endif
