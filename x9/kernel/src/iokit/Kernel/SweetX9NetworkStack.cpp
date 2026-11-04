/*
 * X9 naming compatibility for configd's InterfaceNamer.
 * Adapted from hackios-xnu-kernel MiniIONetworkStack.cpp at
 * c84b4634277a70beac73921029b1fb6afc2530af (XNU 8796.121.2 / iOS 16.5).
 * Current target: XNU 8796.141.3 / iOS 16.7.16, 16 KiB pages.
 *
 * InterfaceNamer opens IONetworkStack before publishing Plugin:InterfaceNamer
 * completion. SCNetworkInterfaceCopyAll waits for that publication.
 * Without x9_wifi_fake=1 this service creates no interface and naming
 * requests stay unsupported. With it, configd's first open starts the Wi-Fi
 * iOS side (X9WifiNet.inc: en0 + IO80211Interface over a fake radio) and
 * naming requests are accepted, en0 being named when it is made.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#if defined(ARM64_BOARD_CONFIG_SWEET)
extern "C" int sweet_early_is_x9(void);
extern "C" int sweet_x9_arg(const char *name, int def);

#include "X9WifiNet.inc"

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
    enabled = sweet_x9_arg("x9_network_stack", 1);
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
    IOLog("X9 NETWORK: IONetworkStack registered under IOResources; %s\n",
        sweet_x9_arg("x9_wifi_fake", 0) == 1 ? "Wi-Fi FAKE radio armed (x9_wifi_fake=1)" : "naming only");
    x9_wifi_print_lk_dt();
    x9_wifi_probe_hw();
    x9_wifi_power_test();
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
    x9_wifi_maybe_start();
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
    static unsigned logged;
    if (x9wifi::gIf != NULL) {
        if (logged++ < 16) {
            char text[200] = "?";
            OSSerialize *s = OSSerialize::withCapacity(512);
            if (s != NULL) {
                if (properties != NULL && properties->serialize(s)) {
                    strlcpy(text, s->text(), sizeof(text));
                }
                s->release();
            }
            IOLog("X9 NETWORK: naming command accepted (en0 named at creation): %s\n", text);
        }
        return kIOReturnSuccess;
    }
    (void)properties;
    if (logged++ < 4) {
        IOLog("X9 NETWORK: interface naming request unsupported; no native interface\n");
    }
    return kIOReturnUnsupported;
}
#endif
