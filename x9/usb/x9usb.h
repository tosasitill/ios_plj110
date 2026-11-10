/* Own-code user API; target iOS16 arm64, ABI1. */
#ifndef X9USB_USER_H
#define X9USB_USER_H
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include "../kernel/src/bsd/dev/x9_usb_device.h"
#ifdef __cplusplus
extern "C" {
#endif
kern_return_t x9usb_open(bool bulk, io_connect_t *);
kern_return_t x9usb_status(io_connect_t, struct x9_usb_status *);
/* Nonblocking chunks, not USB messages. Send success is enqueue, not wire ACK.
 * Capture generation from status. A reset invalidates earlier operations;
 * never retry an ambiguous write automatically in a different generation. */
kern_return_t x9usb_send(io_connect_t, uint32_t generation, const void *, size_t);
kern_return_t x9usb_receive(io_connect_t, uint32_t generation, void *, size_t *);
/* Run the original iOS IOKit controller functions with an explicitly chosen
 * X9 service. Never impersonates the native controller or reconfigures it. */
kern_return_t x9usb_controller_command(unsigned operation, uint32_t delay_ms);
/* Native cycle uses fixed50ms: delay_ms must be50 for CYCLE and0 otherwise.
 * Variable delays are explicitly X9-only, never attributed to native IOKit. */
kern_return_t x9usb_cycle_delayed(uint32_t delay_ms);
kern_return_t x9usb_get_description(io_connect_t, struct x9_usb_description_status *);
/* Original Create/GetInfo/SetDescription; only the current ECM/private bulk
 * profile is supported. Requires explicit bus-off + root + entitlement. */
kern_return_t x9usb_set_description(const struct x9_usb_description *);
struct x9usb_native_status {
    uint32_t controllers, interfaces, multiplexors;
    bool controller_api_available;
};
kern_return_t x9usb_native_status(struct x9usb_native_status *);
#ifdef __cplusplus
}
#endif
#endif
