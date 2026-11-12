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
#include <libkern/c++/OSArray.h>
#include <sys/errno.h>
#include "../../../bsd/dev/x9_usb_device.h"
#include "../../../bsd/dev/x9_usb_native_qmu.h"
#include "X9USBRequestOwner.inc"
#include "X9USBRequestBridge.inc"
extern "C" int sweet_early_is_x9(void);
extern "C" int sweet_x9_arg(const char *, int);
static IOReturn usb_return(int);
#include "X9USBMTU3Backend.inc"
#include "X9USBNativeRequestAdapter.inc"
#include "X9USBControlBridge.inc"
#include "X9USBNativeControlAdapter.inc"
#include "X9USBNativeControlGlue.inc"

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
        !setProperty("X9USBDescriptionMetadata", kOSBooleanTrue) ||
        !setProperty("X9USBTrackedTXABIVersion", X9_USB_TX_ABI, 32) ||
        !setProperty("X9USBSuperSpeed", kOSBooleanFalse)) return false;
    /* BSD starts the only MMIO worker after proc0 is ready. Service discovery
     * is not a statement that enumeration or a DMA transfer has succeeded. */
    registerService();
    IOLog("X9 USB: own ABI v1 registered; status=type0, admin bulk=type1; hardware starts at BSD init\n");
    return true;
}
static bool usb_ascii(OSObject *object, char *out)
{
    auto *s = OSDynamicCast(OSString, object);
    if (!s || !s->getLength() || s->getLength() >= X9_USB_STRING_CAPACITY) return false;
    const char *p = s->getCStringNoCopy();
    for (unsigned i = 0; i < s->getLength(); i++)
        if ((uint8_t)p[i] < 0x20 || (uint8_t)p[i] > 0x7e) return false;
    memcpy(out, p, s->getLength()); out[s->getLength()] = 0;
    return true;
}
static IOReturn usb_description(OSObject *parameter)
{
    /* Native20H392 createUSBDevice checks this entitlement. Keep that check
     * AND the X9 administrator policy; no daemon entitlement bypass. */
    OSObject *entitlement = IOUserClient::copyClientEntitlement(current_task(),
        "com.apple.private.usbdevice.setdescription");
    bool allowed = entitlement == kOSBooleanTrue;
    if (entitlement) entitlement->release();
    if (!allowed) return kIOReturnNotPrivileged;
    auto *d = OSDynamicCast(OSDictionary, parameter);
    if (!d || d->getCount() != 12) return kIOReturnBadArgument;
    x9_usb_description v = {}; v.version = X9_USB_DESCRIPTION_ABI;
    const char *keys[] = { "vendorID", "productID", "deviceID", "deviceClass",
        "deviceSubClass", "deviceProtocol", "BcdUSBVersion" };
    uint32_t *values[] = { &v.vendor_id, &v.product_id, &v.device_id, &v.device_class,
        &v.device_subclass, &v.device_protocol, &v.bcd_usb };
    for (unsigned i = 0; i < 7; i++) {
        auto *n = OSDynamicCast(OSNumber, d->getObject(keys[i]));
        if (!n || n->unsigned64BitValue() > 0xffff) return kIOReturnBadArgument;
        *values[i] = (uint32_t)n->unsigned64BitValue();
    }
    if (!usb_ascii(d->getObject("manufacturerString"), v.manufacturer) ||
        !usb_ascii(d->getObject("productString"), v.product) ||
        !usb_ascii(d->getObject("serialNumber"), v.serial)) return kIOReturnBadArgument;
    if (d->getObject("AllowMultipleCreates") != kOSBooleanTrue) return kIOReturnBadArgument;
    auto *configs = OSDynamicCast(OSArray, d->getObject("ConfigurationDescriptors"));
    if (!configs || configs->getCount() != 1) return kIOReturnUnsupported;
    auto *config = OSDynamicCast(OSDictionary, configs->getObject(0));
    if (!config || config->getCount() != 5 ||
        config->getObject("DefaultConfiguration") != kOSBooleanTrue) return kIOReturnUnsupported;
    auto *label = OSDynamicCast(OSString, config->getObject("Description"));
    if (!label || !label->isEqualTo("X9 ECM transport")) return kIOReturnUnsupported;
    auto *attributes = OSDynamicCast(OSNumber, config->getObject("Attributes"));
    auto *power = OSDynamicCast(OSNumber, config->getObject("MaxPower"));
    /* Existing ECM descriptor policy only. Do not silently accept configs
     * requiring mux, PTPSync, alternate endpoint allocation or USB3. */
    if (!attributes || attributes->unsigned64BitValue() != 0x80 ||
        !power || power->unsigned64BitValue() != 250) return kIOReturnUnsupported;
    auto *interfaces = OSDynamicCast(OSArray, config->getObject("Interfaces"));
    x9_usb_status status; x9_usb_get_status(&status);
    unsigned count = status.mode == 2 ? 2 : 1;
    if (!interfaces || interfaces->getCount() != count) return kIOReturnUnsupported;
    auto *ecm = OSDynamicCast(OSString, interfaces->getObject(0));
    if (!ecm || !ecm->isEqualTo("X9USBECM")) return kIOReturnUnsupported;
    if (count == 2) {
        auto *bulk = OSDynamicCast(OSString, interfaces->getObject(1));
        if (!bulk || !bulk->isEqualTo("X9USBBulk")) return kIOReturnUnsupported;
    }
    return usb_return(x9_usb_set_description(&v));
}
IOReturn X9USBDevice::setProperties(OSObject *properties)
{
    /* Exact 20H392 IOKit property-command envelope, not an Apple C++ vtable.
     * Original CreateWithService/ForceOffBus/GoOffAndOnBus can address this
     * explicitly selected service. Do not publish IOUSBDeviceController or
     * return success for an unimplemented mux configuration. */
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    auto *d = OSDynamicCast(OSDictionary, properties);
    auto *command = d ? OSDynamicCast(OSString, d->getObject("USBDeviceCommand")) : nullptr;
    if (!command || d->getCount() > 2) return kIOReturnBadArgument;
    auto *parameter = d->getObject("USBDeviceCommandParameter");
    if (d->getCount() != (parameter ? 2u : 1u)) return kIOReturnBadArgument;
    if (command->isEqualTo("SetDeviceDescription")) return usb_description(parameter);
    unsigned op;
    uint32_t delay = 0;
    if (command->isEqualTo("ForceOffBusEnable")) op = X9_USB_BUS_OFF;
    else if (command->isEqualTo("ForceOffBusDisable")) op = X9_USB_BUS_ON;
    else if (command->isEqualTo("GoOffAndOnBus") || command->isEqualTo("X9GoOffAndOnBus")) {
        op = X9_USB_BUS_CYCLE;
        auto *number = OSDynamicCast(OSNumber, parameter);
        if (!number) return kIOReturnBadArgument;
        /* 20H392 gated_setProperties ignores the integer; slot199 dispatches
         * a thread-call that sleeps exactly50ms. Custom delay is X9-only. */
        if (command->isEqualTo("GoOffAndOnBus")) delay = X9_USB_NATIVE_CYCLE_MS;
        else {
            if (number->unsigned64BitValue() > 1000) return kIOReturnBadArgument;
            delay = (uint32_t)number->unsigned64BitValue();
        }
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
    case ENOENT: return kIOReturnNotFound;
    case EOVERFLOW: return kIOReturnNoResources;
    default: return kIOReturnIOError;
    }
}
IOReturn X9USBUserClient::externalMethod(uint32_t sel, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch; (void)target; (void)reference;
    if (!a || isInactive()) return kIOReturnNotReady;
    if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
        (a->scalarOutputCount && (sel != X9_USB_SEND_TRACKED ||
         a->scalarOutputCount != 1 || !a->scalarOutput))) return kIOReturnBadArgument;
    if (sel == X9_USB_STATUS) {
        if (a->scalarInputCount || a->structureInputSize || !a->structureOutput ||
            a->structureOutputSize < sizeof(x9_usb_status)) return kIOReturnBadArgument;
        x9_usb_status s; x9_usb_get_status(&s);
        memcpy(a->structureOutput, &s, sizeof(s)); a->structureOutputSize = sizeof(s);
        return kIOReturnSuccess;
    }
    if (sel == X9_USB_GET_DESCRIPTION) {
        if (a->scalarInputCount || a->structureInputSize || !a->structureOutput ||
            a->structureOutputSize < sizeof(x9_usb_description_status)) return kIOReturnBadArgument;
        x9_usb_description_status s; x9_usb_get_description(&s);
        memcpy(a->structureOutput, &s, sizeof(s)); a->structureOutputSize = sizeof(s);
        return kIOReturnSuccess;
    }
    if (!raw || !token) return kIOReturnNotPrivileged;
    if (sel == X9_USB_TX_RESULT) {
        if (a->scalarInputCount != 3 || a->scalarInput[0] > UINT32_MAX ||
            !a->scalarInput[1] || a->scalarInput[2] > 1 || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(x9_usb_tx_result))
            return kIOReturnBadArgument;
        x9_usb_tx_result result = {};
        int error = x9_usb_get_tx_result(token, (uint32_t)a->scalarInput[0],
            a->scalarInput[1], (unsigned)a->scalarInput[2], &result);
        if (error) { a->structureOutputSize = 0; return usb_return(error); }
        memcpy(a->structureOutput, &result, sizeof(result));
        a->structureOutputSize = sizeof(result);
        return kIOReturnSuccess;
    }
    if (a->scalarInputCount != 1 || a->scalarInput[0] > UINT32_MAX) return kIOReturnBadArgument;
    if (sel == X9_USB_SEND_TRACKED) {
        if (!a->structureInput || !a->structureInputSize || a->structureInputSize > X9_USB_CHUNK ||
            a->structureOutputSize || a->scalarOutputCount != 1 || !a->scalarOutput)
            return kIOReturnBadArgument;
        uint64_t request = 0;
        int error = x9_usb_send_tracked(token, (uint32_t)a->scalarInput[0], a->structureInput,
            (uint32_t)a->structureInputSize, &request);
        a->scalarOutput[0] = request;
        return usb_return(error);
    }
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
