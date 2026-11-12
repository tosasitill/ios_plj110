/* Typed control event gate. No native class layout or callback ABI assertion. */
#ifndef X9_USB_CONTROL_BRIDGE_H
#define X9_USB_CONTROL_BRIDGE_H
#include "X9USBRequestBridge.h"
#include "../../../bsd/dev/x9_usb_native_ep0.h"
class X9USBControlSource;
class X9USBControlBridge final : public OSObject {
    OSDeclareDefaultStructors(X9USBControlBridge);
public:
    using Handler = void (*)(void *controller, const x9_usb_ep0_event &, const uint8_t *out);
    using Ready = IOReturn (*)(void *controller);
    /* Once only, called in the same already-qualified request bridge gate. Future loader
     * must qualify and boot-pin controller/thunk; a nonnull pointer is NOT ABI
     * proof. install does not configure/connect USB or publish an Apple class. */
    static IOReturn install(IOWorkLoop *, void *controller, Handler, Ready = nullptr);
    static bool installedFor(IOWorkLoop *, void *controller);
    // Exact gate only. Abort cannot wait inside an EP0 callback; rearm also
    // requires the control adapter idle and no quarantined native ownership.
    // No control bridge = bulk-only baseline, not native control readiness.
    static IOReturn controlReady(IOWorkLoop *, bool rearm);
    /* Same gate, ticket from SETUP/OUT event, copied IN payload. Admission only.
     * Complete event remains obligatory on reset/stall/timeout too. */
    static IOReturn reply(uint64_t ticket, unsigned kind, const void *, uint32_t);
    static IOReturn cancel(uint64_t ticket, int error);
    static void notify(); // worker notification only, never executes native code
    bool drain();
    void free() APPLE_KEXT_OVERRIDE;
private:
    IOWorkLoop *loop = nullptr;
    X9USBControlSource *source = nullptr;
    void *controller = nullptr;
    Handler handler = nullptr;
    Ready ready = nullptr;
    bool registered = false, draining = false;
    // One boot-static snapshot; keep the metaclass allocation below 16 KiB.
    static uint8_t out[X9_USB_EP0_CAPACITY];
};
#endif
