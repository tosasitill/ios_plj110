/*
 * X9 naming compatibility for configd's InterfaceNamer.
 * Adapted from hackios-xnu-kernel MiniIONetworkStack.cpp at
 * c84b4634277a70beac73921029b1fb6afc2530af (XNU 8796.121.2 / iOS 16.5).
 * Current target: XNU 8796.141.3 / iOS 16.7.16, 16 KiB pages.
 *
 * InterfaceNamer opens IONetworkStack before publishing Plugin:InterfaceNamer
 * completion. SCNetworkInterfaceCopyAll waits for that publication. X9 has
 * no native IONetworkInterface producer yet, so naming requests remain
 * unsupported. This service does not create an interface or a MAC address.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#if defined(ARM64_BOARD_CONFIG_SWEET)
extern "C" int sweet_early_is_x9(void);

class IONetworkStack : public IOService
{
    OSDeclareDefaultStructors(IONetworkStack);
public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);

IOService *
IONetworkStack::probe(IOService *provider, SInt32 *score)
{
    int enabled = 1;
    if (!sweet_early_is_x9()) {
        return NULL;
    }
    (void)PE_parse_boot_argn("x9_network_stack", &enabled, sizeof(enabled));
    if (enabled != 1) {
        IOLog("X9 NETWORK: naming compatibility service disabled\n");
        return NULL;
    }
    return IOService::probe(provider, score);
}

bool
IONetworkStack::start(IOService *provider)
{
    if (!sweet_early_is_x9() || !IOService::start(provider)) {
        return false;
    }
    if (!setProperty(kIOUserClientClassKey, "IONetworkStackUserClient")) {
        IOLog("X9 NETWORK: cannot set user-client class\n");
        return false;
    }
    setName("IONetworkStack");
    registerService();
    IOLog("X9 NETWORK: IONetworkStack registered under IOResources; naming only\n");
    return true;
}

class IONetworkStackUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(IONetworkStackUserClient);
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);

bool
IONetworkStackUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    char who[32] = "?";
    (void)proc_selfname(who, sizeof(who));
    IOService *root = IOService::getServiceRoot();
    if (root) {
        IOLog("X9 NETWORK: IONetworkStack opened by %s; root busy=%u\n",
            who, (unsigned)root->getBusyState());
    } else {
        IOLog("X9 NETWORK: IONetworkStack opened by %s; service root unavailable\n", who);
    }
    return true;
}

IOReturn
IONetworkStackUserClient::clientClose(void)
{
    if (!isInactive()) {
        terminate();
    }
    return kIOReturnSuccess;
}

IOReturn
IONetworkStackUserClient::setProperties(OSObject *properties)
{
    (void)properties;
    static unsigned logged;
    if (logged++ < 4) {
        IOLog("X9 NETWORK: interface naming request unsupported; no native interface\n");
    }
    return kIOReturnUnsupported;
}
#endif
