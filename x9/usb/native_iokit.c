/* Original iOS16 controller property-command API. The signatures are traced
 * from 20H392 IOKit; no private kernel C++ layout or Apple bytes are linked.
 * Discovery is nonblocking. Bus commands target X9USBDevice explicitly,
 * require root, and are issued once, including after an ambiguous timeout. */
#include "x9usb.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

typedef kern_return_t (*create_with_service_fn)(CFAllocatorRef, io_service_t, CFTypeRef *);
typedef kern_return_t (*force_off_fn)(CFTypeRef, int);
typedef kern_return_t (*cycle_fn)(CFTypeRef, int);

static uint32_t service_count(const char *name, kern_return_t *error)
{
    io_iterator_t it = IO_OBJECT_NULL;
    *error = IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(name), &it);
    if (*error) return 0;
    uint32_t count = 0;
    io_service_t s;
    while ((s = IOIteratorNext(it))) { if (count != UINT32_MAX) count++; IOObjectRelease(s); }
    IOObjectRelease(it);
    return count;
}
kern_return_t x9usb_native_status(struct x9usb_native_status *out)
{
    if (!out) return kIOReturnBadArgument;
    memset(out, 0, sizeof(*out));
    out->controller_api_available = dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerCreateWithService") &&
        dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerForceOffBus") &&
        dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerGoOffAndOnBus");
    kern_return_t e;
    out->controllers = service_count("IOUSBDeviceController", &e);
    if (e) return e;
    out->interfaces = service_count("IOUSBDeviceInterface", &e);
    if (e) return e;
    out->multiplexors = service_count("AppleUSBDeviceMux", &e);
    return e;
}
kern_return_t x9usb_controller_command(unsigned operation, uint32_t delay_ms)
{
    if (operation > X9_USB_BUS_CYCLE || delay_ms > 1000 ||
        (operation != X9_USB_BUS_CYCLE && delay_ms)) return kIOReturnBadArgument;
    if (geteuid()) return kIOReturnNotPrivileged;
    create_with_service_fn create = (create_with_service_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerCreateWithService");
    force_off_fn force = (force_off_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerForceOffBus");
    cycle_fn cycle = (cycle_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerGoOffAndOnBus");
    if (!create || !force || !cycle) return kIOReturnUnsupported;
    io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(X9_USB_SERVICE));
    if (!s) return kIOReturnNotFound;
    CFTypeRef capability = IORegistryEntryCreateCFProperty(s, CFSTR("X9USBControllerBusCommands"), kCFAllocatorDefault, 0);
    bool supported = capability && CFGetTypeID(capability) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)capability);
    if (capability) CFRelease(capability);
    if (!supported) { IOObjectRelease(s); return kIOReturnUnsupported; }
    CFTypeRef controller = NULL;
    kern_return_t e = create(kCFAllocatorDefault, s, &controller);
    IOObjectRelease(s);
    if (e) { if (controller) CFRelease(controller); return e; }
    if (!controller) return kIOReturnInternalError;
    /* This is the real IOKit entrypoint -> IORegistryEntrySetCFProperties ->
     * X9USBDevice::setProperties -> the sole MTU3 worker, not a new selector. */
    e = operation == X9_USB_BUS_CYCLE ? cycle(controller, (int)delay_ms) :
        force(controller, operation == X9_USB_BUS_OFF);
    CFRelease(controller);
    return e;
}
