#include "x9usb.h"
#include <string.h>
_Static_assert(sizeof(struct x9_usb_status) == 128, "X9 USB ABI v1 size");
_Static_assert(sizeof(struct x9_usb_description_status) == 240, "X9 USB description ABI size");
_Static_assert(sizeof(struct x9_usb_description) == 224, "X9 USB description size");
kern_return_t x9usb_get_description(io_connect_t c, struct x9_usb_description_status *out)
{
    if (!out) return kIOReturnBadArgument;
    memset(out, 0, sizeof(*out)); size_t n = sizeof(*out);
    kern_return_t e = IOConnectCallStructMethod(c, X9_USB_GET_DESCRIPTION, NULL, 0, out, &n);
    if (!e && (n != sizeof(*out) || out->version != X9_USB_DESCRIPTION_ABI ||
        out->description.version != X9_USB_DESCRIPTION_ABI)) return kIOReturnUnsupported;
    return e;
}
kern_return_t x9usb_open(bool bulk, io_connect_t *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = IO_OBJECT_NULL;
    io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(X9_USB_SERVICE));
    if (!s) return kIOReturnNotFound;
    kern_return_t e = IOServiceOpen(s, mach_task_self(), bulk ? 1 : 0, out);
    IOObjectRelease(s);
    if (!e) {
        struct x9_usb_status status;
        e = x9usb_status(*out, &status);
        if (e) { IOServiceClose(*out); *out = IO_OBJECT_NULL; }
    }
    return e;
}
kern_return_t x9usb_status(io_connect_t c, struct x9_usb_status *out)
{
    if (!out) return kIOReturnBadArgument;
    memset(out, 0, sizeof(*out)); size_t n = sizeof(*out);
    kern_return_t e = IOConnectCallStructMethod(c, X9_USB_STATUS, NULL, 0, out, &n);
    if (!e && (n != sizeof(*out) || out->version != X9_USB_ABI)) return kIOReturnUnsupported;
    return e;
}
kern_return_t x9usb_send(io_connect_t c, uint32_t generation, const void *bytes, size_t n)
{
    if (!bytes || !n || n > X9_USB_CHUNK) return kIOReturnBadArgument;
    uint64_t input = generation;
    return IOConnectCallMethod(c, X9_USB_SEND, &input, 1, bytes, n, NULL, NULL, NULL, NULL);
}
kern_return_t x9usb_receive(io_connect_t c, uint32_t generation, void *bytes, size_t *n)
{
    if (!bytes || !n || !*n || *n > X9_USB_CHUNK) return kIOReturnBadArgument;
    uint64_t input = generation;
    return IOConnectCallMethod(c, X9_USB_RECEIVE, &input, 1, NULL, 0, NULL, NULL, bytes, n);
}
