#include "X9USBControlBridge.h"
#include "X9USBNativeControlAdapter.h"
#include <IOKit/IOEventSource.h>
static LCK_GRP_DECLARE(x9_usb_control_group, "x9_usb_control");
static LCK_MTX_DECLARE(x9_usb_control_lock, &x9_usb_control_group);
static X9USBControlBridge *x9_usb_control_bridge;
static bool x9_usb_control_installing;
uint8_t X9USBControlBridge::out[X9_USB_EP0_CAPACITY] = {};
static X9USBControlBridge *usb_control_registry()
{
    lck_mtx_lock(&x9_usb_control_lock);
    auto *b = x9_usb_control_bridge;
    lck_mtx_unlock(&x9_usb_control_lock); return b; // permanent boot retain
}
class X9USBControlSource final : public IOEventSource {
    OSDeclareDefaultStructors(X9USBControlSource);
    X9USBControlBridge *bridge = nullptr;
public:
    bool initWithBridge(X9USBControlBridge *b)
    { if (!IOEventSource::init(b)) return false; bridge = b; return true; }
    bool checkForWork() APPLE_KEXT_OVERRIDE { return bridge && bridge->drain(); }
    void notify() { signalWorkAvailable(); }
};
OSDefineMetaClassAndStructors(X9USBControlSource, IOEventSource);
OSDefineMetaClassAndStructors(X9USBControlBridge, OSObject);
bool X9USBControlBridge::installedFor(IOWorkLoop *loop, void *controller)
{
    auto *b = usb_control_registry();
    return loop && controller && b && b->loop == loop && b->controller == controller;
}
IOReturn X9USBControlBridge::controlReady(IOWorkLoop *loop, bool rearm)
{
    if (!loop || !loop->inGate()) return kIOReturnNotPermitted;
    auto *b = usb_control_registry();
    if (!b) return kIOReturnSuccess;
    if (b->loop != loop) return kIOReturnNotReady;
    if (b->draining) return kIOReturnBusy;
    return rearm ? X9USBNativeControlAdapter::rearmReady(b->controller) : kIOReturnSuccess;
}
IOReturn X9USBControlBridge::install(IOWorkLoop *loop, void *controller, Handler handler)
{
    if (!loop || !controller || !handler) return kIOReturnBadArgument;
    if (!loop->inGate()) return kIOReturnNotPermitted;
    if (!X9USBRequestBridge::installedOn(loop)) return kIOReturnNotReady;
    lck_mtx_lock(&x9_usb_control_lock);
    if (x9_usb_control_bridge || x9_usb_control_installing) {
        lck_mtx_unlock(&x9_usb_control_lock); return kIOReturnExclusiveAccess;
    }
    x9_usb_control_installing = true;
    lck_mtx_unlock(&x9_usb_control_lock);
    auto *b = new X9USBControlBridge;
    IOReturn result = kIOReturnNoMemory;
    if (b && b->init()) {
        b->loop = loop; loop->retain(); b->controller = controller; b->handler = handler;
        b->source = new X9USBControlSource;
        if (b->source && b->source->initWithBridge(b)) result = loop->addEventSource(b->source);
    }
    lck_mtx_lock(&x9_usb_control_lock);
    if (result == kIOReturnSuccess) { b->registered = true; x9_usb_control_bridge = b; }
    x9_usb_control_installing = false;
    lck_mtx_unlock(&x9_usb_control_lock);
    if (result != kIOReturnSuccess && b) b->release();
    return result;
}
void X9USBControlBridge::free()
{
    if (registered) panic("X9 USB: freeing registered EP0 gate");
    if (source) { source->release(); source = nullptr; }
    if (loop) { loop->release(); loop = nullptr; }
    OSObject::free();
}
IOReturn X9USBControlBridge::reply(uint64_t ticket, unsigned kind, const void *bytes, uint32_t n)
{
    auto *b = usb_control_registry();
    if (!b) return kIOReturnNotReady;
    if (!b->loop->inGate()) return kIOReturnNotPermitted;
    return usb_return(x9_usb_native_ep0_reply(ticket, kind, bytes, n));
}
IOReturn X9USBControlBridge::cancel(uint64_t ticket, int error)
{
    auto *b = usb_control_registry();
    if (!b) return kIOReturnNotReady;
    if (!b->loop->inGate()) return kIOReturnNotPermitted;
    return usb_return(x9_usb_native_ep0_cancel(ticket, error));
}
bool X9USBControlBridge::drain()
{
    if (!loop->inGate()) panic("X9 USB: EP0 callback outside controller gate");
    if (draining) return false;
    x9_usb_ep0_event event = {};
    if (x9_usb_native_ep0_claim(&event, out, sizeof(out))) return false;
    draining = true;
    /* Neither usb_lock nor registry lock held. Snapshot remains pinned until
     * this callback returns, including reentrant gate calls and reset races. */
    if (!X9USBNativeControlAdapter::event(controller, event))
        handler(controller, event, event.kind == X9_EP0_OUT_DATA ? out : nullptr);
    if (x9_usb_native_ep0_ack(event.sequence)) panic("X9 USB: lost EP0 gate event");
    draining = false;
    return x9_usb_native_ep0_pending() != 0;
}
void X9USBControlBridge::notify()
{
    auto *b = usb_control_registry();
    if (b && x9_usb_native_ep0_pending()) b->source->notify();
}
extern "C" void x9_usb_native_control_notify(void) { X9USBControlBridge::notify(); }
