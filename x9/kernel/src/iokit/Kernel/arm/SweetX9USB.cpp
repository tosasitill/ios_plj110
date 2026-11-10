/* Own-code iOS-facing MTU3 transport. Does not masquerade as AppleUSBXDCI,
 * IOUSBDeviceController, or AppleUSBDeviceMux. ECM feeds native BSD networking.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <sys/errno.h>
#include "../../../bsd/dev/x9_usb_device.h"
extern "C" int sweet_early_is_x9(void);
extern "C" int sweet_x9_arg(const char *, int);
static IOReturn usb_return(int);

/* Only the device worker allocates these. Keep prepared pages wired until
 * reboot even if a QMU STOP times out. No other device's carve-out is borrowed. */
extern "C" int x9_usb_dma_alloc(void **va, uint64_t *pa)
{
    static IOBufferMemoryDescriptor *pinned[5];
    static unsigned count;
    if (!va || !pa || count >= 5) return EINVAL;
    auto *m = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        16384, 0xfffffc000ULL);
    if (!m) return ENOMEM;
    IOByteCount contiguous = 0;
    auto physical = m->getPhysicalSegment(0, &contiguous);
    if (!physical || (physical & 0x3fff) || contiguous < 16384 || physical >= (1ULL << 36) - 16383) {
        m->release(); return EFAULT;
    }
    if (m->prepare(kIODirectionInOut) != kIOReturnSuccess) { m->release(); return EIO; }
    *va = m->getBytesNoCopy(); *pa = physical;
    bzero(*va, 16384); pinned[count++] = m;
    return 0;
}
class X9USBDevice : public IOService {
    OSDeclareDefaultStructors(X9USBDevice);
public:
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(X9USBDevice, IOService);
IOService *X9USBDevice::probe(IOService *p, SInt32 *score)
{
    int mode = sweet_x9_arg("x9_usb", 0);
    if (!sweet_early_is_x9() || (mode != 1 && mode != 2)) return nullptr;
    return IOService::probe(p, score);
}
bool X9USBDevice::start(IOService *p)
{
    if (!IOService::start(p)) return false;
    setName(X9_USB_SERVICE);
    if (!setProperty(kIOUserClientClassKey, "X9USBUserClient") ||
        !setProperty("X9USBABIVersion", X9_USB_ABI, 32) ||
        !setProperty("X9USBPhysicalTransport", "MT6993-MTU3-QMU36") ||
        !setProperty("X9USBRole", "device") ||
        !setProperty("X9USBNetworkInterface", "ue0") ||
        !setProperty("X9USBAppleNativeStack", kOSBooleanFalse) ||
        !setProperty("X9USBControllerBusCommands", kOSBooleanTrue) ||
        !setProperty("X9USBSuperSpeed", kOSBooleanFalse)) return false;
    /* BSD starts the only MMIO worker after proc0 is ready. Service discovery
     * is not a statement that enumeration or a DMA transfer has succeeded. */
    registerService();
    IOLog("X9 USB: own ABI v1 registered; status=type0, admin bulk=type1; hardware starts at BSD init\n");
    return true;
}
IOReturn X9USBDevice::setProperties(OSObject *properties)
{
    /* Exact 20H392 IOKit property-command envelope, not an Apple C++ vtable.
     * Original CreateWithService/ForceOffBus/GoOffAndOnBus can address this
     * explicitly selected service. Do not publish IOUSBDeviceController or
     * return success for an unimplemented description/mux configuration. */
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    auto *d = OSDynamicCast(OSDictionary, properties);
    auto *command = d ? OSDynamicCast(OSString, d->getObject("USBDeviceCommand")) : nullptr;
    if (!command || d->getCount() > 2) return kIOReturnBadArgument;
    auto *parameter = d->getObject("USBDeviceCommandParameter");
    if (d->getCount() != (parameter ? 2u : 1u)) return kIOReturnBadArgument;
    unsigned op;
    uint32_t delay = 0;
    if (command->isEqualTo("ForceOffBusEnable")) op = X9_USB_BUS_OFF;
    else if (command->isEqualTo("ForceOffBusDisable")) op = X9_USB_BUS_ON;
    else if (command->isEqualTo("GoOffAndOnBus")) {
        op = X9_USB_BUS_CYCLE;
        auto *number = OSDynamicCast(OSNumber, parameter);
        if (!number || number->unsigned64BitValue() > 1000) return kIOReturnBadArgument;
        delay = (uint32_t)number->unsigned64BitValue();
    } else return kIOReturnUnsupported;
    if (op != X9_USB_BUS_CYCLE && parameter) return kIOReturnBadArgument;
    return usb_return(x9_usb_bus_command(op, delay));
}
class X9USBUserClient : public IOUserClient {
    OSDeclareDefaultStructors(X9USBUserClient);
    bool raw = false;
    uint64_t token = 0;
    void releaseLease() { if (token) { x9_usb_release(token); token = 0; } }
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *,
                           OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(X9USBUserClient, IOUserClient);
bool X9USBUserClient::initWithTask(task_t t, void *id, UInt32 type, OSDictionary *props)
{
    if (type > 1 || !IOUserClient::initWithTask(t, id, type, props)) return false;
    raw = type == 1;
    return !raw || clientHasPrivilege(t, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
}
bool X9USBUserClient::start(IOService *p)
{
    if (!IOUserClient::start(p)) return false;
    return !raw || x9_usb_claim(&token) == 0;
}
void X9USBUserClient::stop(IOService *p)
{ releaseLease(); IOUserClient::stop(p); }
IOReturn X9USBUserClient::clientClose(void)
{ releaseLease(); terminate(); return kIOReturnSuccess; }
static IOReturn usb_return(int e)
{
    switch (e) {
    case 0: return kIOReturnSuccess;
    case EINVAL: return kIOReturnBadArgument;
    case EPERM: return kIOReturnNotPermitted;
    case EAGAIN: return kIOReturnNotReady;
    case ESTALE: return kIOReturnAborted;
    case ENXIO: return kIOReturnOffline;
    case EMSGSIZE: return kIOReturnNoSpace;
    case EBUSY: return kIOReturnExclusiveAccess;
    case ETIMEDOUT: return kIOReturnTimeout;
    case ENOTSUP: return kIOReturnUnsupported;
    default: return kIOReturnIOError;
    }
}
IOReturn X9USBUserClient::externalMethod(uint32_t sel, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch; (void)target; (void)reference;
    if (!a || isInactive()) return kIOReturnNotReady;
    if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
        a->scalarOutputCount) return kIOReturnBadArgument;
    if (sel == X9_USB_STATUS) {
        if (a->scalarInputCount || a->structureInputSize || !a->structureOutput ||
            a->structureOutputSize < sizeof(x9_usb_status)) return kIOReturnBadArgument;
        x9_usb_status s; x9_usb_get_status(&s);
        memcpy(a->structureOutput, &s, sizeof(s)); a->structureOutputSize = sizeof(s);
        return kIOReturnSuccess;
    }
    if (!raw || !token) return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 1 || a->scalarInput[0] > UINT32_MAX) return kIOReturnBadArgument;
    if (sel == X9_USB_SEND) {
        if (!a->structureInput || !a->structureInputSize || a->structureInputSize > X9_USB_CHUNK ||
            a->structureOutputSize) return kIOReturnBadArgument;
        return usb_return(x9_usb_send(token, (uint32_t)a->scalarInput[0], a->structureInput,
                                     (uint32_t)a->structureInputSize));
    }
    if (sel == X9_USB_RECEIVE) {
        if (a->structureInputSize || !a->structureOutput || !a->structureOutputSize ||
            a->structureOutputSize > X9_USB_CHUNK) return kIOReturnBadArgument;
        uint32_t n = (uint32_t)a->structureOutputSize;
        int error = x9_usb_receive(token, (uint32_t)a->scalarInput[0], a->structureOutput, &n);
        a->structureOutputSize = n;
        return usb_return(error);
    }
    return kIOReturnUnsupported;
}
