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
#ifdef __cplusplus
}
#endif
#endif
