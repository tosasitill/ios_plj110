/* X9 BT discovery and explicitly versioned bootstrap IOUserClient.
 * UniXNU/hackios reference supplies IOResources service registration patterns,
 * not an MT6653 Bluetooth driver. Do not masquerade as AppleBluetoothModule:
 * the real 20H392 AppleBT shared-memory/pipe ABI has not been implemented.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include "../../../bsd/dev/x9_bluetooth.h"

#if defined(ARM64_BOARD_CONFIG_SWEET)
extern "C" int sweet_early_is_x9(void);
extern "C" int sweet_x9_arg(const char *, int);

class X9BluetoothTransport : public IOService {
    OSDeclareDefaultStructors(X9BluetoothTransport);
public:
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(X9BluetoothTransport, IOService);
IOService *X9BluetoothTransport::probe(IOService *p, SInt32 *score)
{
    if (!sweet_early_is_x9() || sweet_x9_arg("x9_bt", 0) != 1) return nullptr;
    return IOService::probe(p, score);
}
bool X9BluetoothTransport::start(IOService *p)
{
    if (!IOService::start(p)) return false;
    if (!setProperty(kIOUserClientClassKey, "X9BluetoothUserClient") ||
        !setProperty("X9BluetoothABIVersion", X9_BT_ABI_VERSION, 32) ||
        !setProperty("X9BluetoothStage", "bootstrap-only") ||
        !setProperty("X9BluetoothPhysicalTransport", "MT6653-UART3-UARTHUB") ||
        !setProperty("X9BluetoothFirmwareReady", kOSBooleanFalse)) return false;
    /* A native-name discovery experiment is separate from custom H4 access.
     * 172.txt confirms SpringBoard matches IONameMatch=bluetooth. Default name
     * cannot fool native clients into treating this as an AppleBT controller. */
    setName(sweet_x9_arg("x9_bt_discovery", 0) == 1 ? "bluetooth" : "X9BluetoothBootstrap");
    x9_bluetooth_start();
    registerService();
    IOLog("X9 BT: %s registered; bootstrap ABI v%u, not AppleBT, not radio-ready\n", getName(), X9_BT_ABI_VERSION);
    return true;
}

class X9BluetoothUserClient : public IOUserClient {
    OSDeclareDefaultStructors(X9BluetoothUserClient);
    bool administrator = false;
    bool exclusive = false;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *,
                           OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(X9BluetoothUserClient, IOUserClient);
bool X9BluetoothUserClient::initWithTask(task_t t, void *id, UInt32 type, OSDictionary *props)
{
    /* Unknown AppleBT open types are rejected; type zero is our documented ABI. */
    if (type != 0 || !IOUserClient::initWithTask(t, id, type, props)) return false;
    administrator = clientHasPrivilege(t, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    char who[32] = "?"; proc_selfname(who, sizeof(who));
    IOLog("X9 BT: bootstrap client opened by %s admin=%u\n", who, administrator);
    return true;
}
IOReturn X9BluetoothUserClient::clientClose(void)
{ terminate(); return kIOReturnSuccess; }
bool X9BluetoothUserClient::start(IOService *p)
{
    if (!IOUserClient::start(p)) return false;
    if (administrator) {
        if (!p->open(this)) return false;
        exclusive = true;
    }
    return true;
}
void X9BluetoothUserClient::stop(IOService *p)
{
    if (exclusive) { p->close(this); exclusive = false; }
    IOUserClient::stop(p);
}
static IOReturn bt_return(int error)
{
    switch (error) {
    case 0: return kIOReturnSuccess;
    case EINVAL: return kIOReturnBadArgument;
    case ENOENT: return kIOReturnNotFound;
    case ENXIO: return kIOReturnNotReady;
    case EPERM: return kIOReturnNotPermitted;
    case EAGAIN: return kIOReturnNotReady;
    case EMSGSIZE: return kIOReturnNoSpace;
    default: return kIOReturnIOError;
    }
}
IOReturn X9BluetoothUserClient::externalMethod(uint32_t sel, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch; (void)target; (void)reference;
    if (!a || isInactive()) return kIOReturnNotReady;
    // Only small inband structures in v1. Reject descriptors/async explicitly
    // rather than trusting pointers/sizes supplied for another Apple ABI.
    if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
        a->scalarOutputCount) return kIOReturnBadArgument;
    switch (sel) {
    case X9_BT_GET_STATUS: {
        if (a->scalarInputCount || a->structureInputSize || !a->structureOutput ||
            a->structureOutputSize < sizeof(x9_bt_status)) return kIOReturnBadArgument;
        x9_bt_status s; x9_bluetooth_status(&s);
        memcpy(a->structureOutput, &s, sizeof(s)); a->structureOutputSize = sizeof(s);
        return kIOReturnSuccess;
    }
    case X9_BT_GET_SECTION: {
        if (a->scalarInputCount != 1 || a->scalarInput[0] >= X9_BT_SECTION_MAX ||
            a->structureInputSize || !a->structureOutput ||
            a->structureOutputSize < sizeof(x9_bt_section)) return kIOReturnBadArgument;
        x9_bt_section section;
        int error = x9_bluetooth_section((uint32_t)a->scalarInput[0], &section);
        if (error) return bt_return(error);
        memcpy(a->structureOutput, &section, sizeof(section)); a->structureOutputSize = sizeof(section);
        return kIOReturnSuccess;
    }
    case X9_BT_SEND_H4:
        if (!administrator) return kIOReturnNotPrivileged;
        if (a->scalarInputCount || !a->structureInput || !a->structureInputSize ||
            a->structureInputSize > X9_BT_FRAME_MAX || a->structureOutputSize) return kIOReturnBadArgument;
        return bt_return(x9_bluetooth_send(a->structureInput, (uint32_t)a->structureInputSize));
    case X9_BT_RECV_H4: {
        if (!administrator) return kIOReturnNotPrivileged;
        if (a->scalarInputCount || a->structureInputSize || !a->structureOutput ||
            !a->structureOutputSize || a->structureOutputSize > X9_BT_FRAME_MAX) return kIOReturnBadArgument;
        uint32_t n = (uint32_t)a->structureOutputSize;
        int error = x9_bluetooth_receive(a->structureOutput, &n);
        a->structureOutputSize = n; return bt_return(error);
    }
    default:
        static unsigned logged;
        if (logged++ < 16) IOLog("X9 BT: unsupported bootstrap selector %u (native ABI not assumed)\n", sel);
        return kIOReturnUnsupported;
    }
}
#endif
