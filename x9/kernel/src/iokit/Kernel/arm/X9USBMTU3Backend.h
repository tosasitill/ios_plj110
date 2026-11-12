/* Current-layout ordinary bulk scheduler; not an Apple controller class. */
#ifndef X9_USB_MTU3_BACKEND_H
#define X9_USB_MTU3_BACKEND_H
#include "X9USBRequestBridge.h"
#include "../../../bsd/dev/x9_usb_native_qmu.h"
class X9USBMTU3Backend final {
public:
    /* Boot-static context, install at most once, only AFTER native ABI,
     * controller workloop and exclusive native endpoint control plane qualify.
     * Getting operations does not enable/install/configure anything. */
    static X9USBRequestBridge::Backend operations();
private:
    struct Entry {
        bool occupied, active;
        uint32_t endpoint, requested, progressed;
        X9USBRequestOwner::Ticket ticket;
        X9USBRequestOwner::Chunk chunk;
        x9_usb_qmu_ticket physical;
    } entries[X9USBRequestOwner::Capacity] = {};
    struct Commit {
        X9USBRequestOwner *owner;
        X9USBRequestOwner::Chunk chunk;
        uint64_t nowNS;
        IOReturn result;
    };
    static X9USBMTU3Backend instance;
    static int commit(void *);
    static IOReturn validate(void *, uint32_t, const X9USBRequestOwner::Input &);
    static void poll(void *, X9USBRequestBridge &, X9USBRequestOwner &, bool, uint64_t);
    static IOReturn rearm(void *, X9USBRequestOwner &);
    void run(X9USBRequestBridge &, X9USBRequestOwner &, bool, uint64_t);
};
#endif
