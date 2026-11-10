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
typedef CFTypeRef (*description_create_fn)(CFAllocatorRef);
typedef CFMutableDictionaryRef (*description_info_fn)(CFTypeRef);
typedef kern_return_t (*description_set_fn)(CFTypeRef, CFTypeRef);
typedef kern_return_t (*set_properties_fn)(io_registry_entry_t, CFTypeRef);

static io_service_t own_service(const char *key)
{
    io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(X9_USB_SERVICE));
    if (!s) return IO_OBJECT_NULL;
    CFTypeRef p = IORegistryEntryCreateCFProperty(s,
        !strcmp(key, "description") ? CFSTR("X9USBDescriptionMetadata") : CFSTR("X9USBControllerBusCommands"),
        kCFAllocatorDefault, 0);
    bool allowed = p && CFGetTypeID(p) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)p);
    if (p) CFRelease(p);
    if (!allowed) { IOObjectRelease(s); return IO_OBJECT_NULL; }
    return s;
}

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
    if (operation > X9_USB_BUS_CYCLE ||
        delay_ms != (operation == X9_USB_BUS_CYCLE ? X9_USB_NATIVE_CYCLE_MS : 0u)) return kIOReturnBadArgument;
    if (geteuid()) return kIOReturnNotPrivileged;
    create_with_service_fn create = (create_with_service_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerCreateWithService");
    force_off_fn force = (force_off_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerForceOffBus");
    cycle_fn cycle = (cycle_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerGoOffAndOnBus");
    if (!create || !force || !cycle) return kIOReturnUnsupported;
    io_service_t s = own_service("bus");
    if (!s) return kIOReturnNotFound;
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
kern_return_t x9usb_cycle_delayed(uint32_t delay_ms)
{
    if (delay_ms > 1000) return kIOReturnBadArgument;
    if (geteuid()) return kIOReturnNotPrivileged;
    /* The SDK marks this unavailable on iOS, but the pinned20H392 IOKit
     * exports and uses it at18e5caf50. Resolve, don't change SDK annotations. */
    set_properties_fn set_properties = (set_properties_fn)dlsym(RTLD_DEFAULT, "IORegistryEntrySetCFProperties");
    if (!set_properties) return kIOReturnUnsupported;
    io_service_t s = own_service("bus");
    if (!s) return kIOReturnNotFound;
    int delay = (int)delay_ms;
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &delay);
    if (!n) { IOObjectRelease(s); return kIOReturnNoMemory; }
    const void *keys[] = { CFSTR("USBDeviceCommand"), CFSTR("USBDeviceCommandParameter") };
    const void *values[] = { CFSTR("X9GoOffAndOnBus"), n };
    CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    kern_return_t e = d ? set_properties(s, d) : kIOReturnNoMemory;
    if (d) CFRelease(d);
    CFRelease(n); IOObjectRelease(s);
    return e;
}
static bool put_number(CFMutableDictionaryRef d, CFStringRef key, uint32_t value)
{
    int64_t v = value;
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &v);
    if (!n) return false;
    CFDictionarySetValue(d, key, n); CFRelease(n); return true;
}
static bool ascii_string(const char *p)
{
    if (!p[0]) return false;
    for (unsigned i = 0; i < X9_USB_STRING_CAPACITY; i++) {
        if (!p[i]) return true;
        if ((uint8_t)p[i] < 0x20 || (uint8_t)p[i] > 0x7e) return false;
    }
    return false;
}
static bool put_string(CFMutableDictionaryRef d, CFStringRef key, const char *value)
{
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, value, kCFStringEncodingASCII);
    if (!s) return false;
    CFDictionarySetValue(d, key, s); CFRelease(s); return true;
}
kern_return_t x9usb_set_description(const struct x9_usb_description *v)
{
    if (!v || v->version != X9_USB_DESCRIPTION_ABI || !v->vendor_id || v->vendor_id > 0xffff ||
        !v->product_id || v->product_id > 0xffff || v->device_id > 0xffff || v->bcd_usb != 0x200 ||
        v->device_class != 0xef || v->device_subclass != 2 || v->device_protocol != 1 ||
        !ascii_string(v->manufacturer) || !ascii_string(v->product) || !ascii_string(v->serial)) return kIOReturnBadArgument;
    if (geteuid()) return kIOReturnNotPrivileged;
    create_with_service_fn create = (create_with_service_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerCreateWithService");
    description_create_fn make = (description_create_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceDescriptionCreate");
    /* Exported GetInfo returns the mutable dictionary owned by the original
     * CF description. Do not manufacture its private CFRuntime layout. */
    description_info_fn info = (description_info_fn)dlsym(RTLD_DEFAULT, "_IOUSBDeviceDescriptionGetInfo");
    description_set_fn set = (description_set_fn)dlsym(RTLD_DEFAULT, "IOUSBDeviceControllerSetDescription");
    if (!create || !make || !info || !set) return kIOReturnUnsupported;
    io_service_t s = own_service("description");
    if (!s) return kIOReturnNotFound;
    io_connect_t c = IO_OBJECT_NULL;
    kern_return_t e = IOServiceOpen(s, mach_task_self(), 0, &c);
    struct x9_usb_description_status status;
    if (!e) { e = x9usb_get_description(c, &status); IOServiceClose(c); }
    if (e) { IOObjectRelease(s); return e; }
    if (!status.forced_off || (status.mode != 1 && status.mode != 2)) {
        IOObjectRelease(s); return kIOReturnNotReady;
    }
    CFTypeRef controller = NULL, description = NULL;
    CFMutableDictionaryRef cfg = NULL;
    CFArrayRef interfaces = NULL, configs = NULL;
    e = create(kCFAllocatorDefault, s, &controller); IOObjectRelease(s);
    if (e) { if (controller) CFRelease(controller); return e; }
    if (!controller) return kIOReturnInternalError;
    e = kIOReturnNoMemory;
    description = make(kCFAllocatorDefault);
    if (!description) goto out;
    CFMutableDictionaryRef d = info(description);
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) { e = kIOReturnInternalError; goto out; }
    cfg = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!cfg) goto out;
    const void *names[] = { CFSTR("X9USBECM"), CFSTR("X9USBBulk") };
    interfaces = CFArrayCreate(kCFAllocatorDefault, names, status.mode == 2 ? 2 : 1, &kCFTypeArrayCallBacks);
    if (!interfaces) goto out;
    CFDictionarySetValue(cfg, CFSTR("Interfaces"), interfaces);
    CFDictionarySetValue(cfg, CFSTR("DefaultConfiguration"), kCFBooleanTrue);
    CFDictionarySetValue(cfg, CFSTR("Description"), CFSTR("X9 ECM transport"));
    if (!put_number(cfg, CFSTR("Attributes"), 0x80) || !put_number(cfg, CFSTR("MaxPower"), 250)) goto out;
    const void *one[] = { cfg };
    configs = CFArrayCreate(kCFAllocatorDefault, one, 1, &kCFTypeArrayCallBacks);
    if (!configs) goto out;
    CFDictionarySetValue(d, CFSTR("ConfigurationDescriptors"), configs);
    CFDictionarySetValue(d, CFSTR("AllowMultipleCreates"), kCFBooleanTrue);
    if (!put_number(d, CFSTR("vendorID"), v->vendor_id) ||
        !put_number(d, CFSTR("productID"), v->product_id) ||
        !put_number(d, CFSTR("deviceID"), v->device_id) ||
        !put_number(d, CFSTR("deviceClass"), v->device_class) ||
        !put_number(d, CFSTR("deviceSubClass"), v->device_subclass) ||
        !put_number(d, CFSTR("deviceProtocol"), v->device_protocol) ||
        !put_number(d, CFSTR("BcdUSBVersion"), v->bcd_usb) ||
        !put_string(d, CFSTR("manufacturerString"), v->manufacturer) ||
        !put_string(d, CFSTR("productString"), v->product) ||
        !put_string(d, CFSTR("serialNumber"), v->serial)) goto out;
    e = set(controller, description); /* exactly one original IOKit call */
out:
    if (configs) CFRelease(configs);
    if (interfaces) CFRelease(interfaces);
    if (cfg) CFRelease(cfg);
    if (description) CFRelease(description);
    CFRelease(controller);
    return e;
}
