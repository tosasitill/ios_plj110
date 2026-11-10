/* X9 torch ABI v1. Values describe confirmed hardware state, not UI state. */
#ifndef X9_TORCH_H
#define X9_TORCH_H
#include <stdint.h>
#include <sys/ioccom.h>
#define X9_TORCH_DEVICE "/dev/x9-torch"
#define X9_TORCH_READY       1u
#define X9_TORCH_LATCHED     2u
#define X9_TORCH_NEEDS_ZERO  4u
#define X9_TORCH_OFF_FAILED  8u
#define X9_TORCH_LEVEL_UNKNOWN 16u
struct x9_torch_status {
    uint32_t version, mode, flags, channel;
    uint32_t level, current_ua, max_current_ua, max_seconds;
    uint32_t device_info, enable, fault, last_errno;
};
#define X9_TORCH_GET_STATUS _IOR('T', 0, struct x9_torch_status)
/* write: exactly one native uint32_t, 0..1000. read: one uint32_t confirmed
 * selected-channel level, or EIO when unknown. Probe mode reports a startup
 * snapshot; inherited currents above our cap read 1000, with exact uA in
 * GET_STATUS. ioctl GET_STATUS: full snapshot; no arbitrary register access. */
#endif
