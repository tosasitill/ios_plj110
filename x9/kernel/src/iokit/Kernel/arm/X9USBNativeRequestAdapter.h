/* D22/20H392 ordinary IOUSBDeviceIORequest -> current-layout worker bridge.
 * This is not an Apple class declaration, a loader or a native-mode switch.
 */
#ifndef X9_USB_NATIVE_REQUEST_ADAPTER_H
#define X9_USB_NATIVE_REQUEST_ADAPTER_H
#include "X9USBRequestBridge.h"

class X9USBNativeRequestAdapter final {
public:
    using FamilyComplete = void (*)(void *controller, void *request);
    /* Bind once, after the native loader has qualified the original Family
     * completion entry/thunk, request allocator and current-layout gate.
     * The controller and thunk must stay pinned for this entire boot. No raw
     * address, stock numeric vslot, OSObject cast, or successful stub here.
     * Bridge must already be installed on EXACTLY this workloop.
     */
    static IOReturn bind(IOWorkLoop *, void *controller, FamilyComplete);
    static bool boundFor(IOWorkLoop *, void *controller, FamilyComplete);
    /* Called by the qualified native ioGated glue. request must be the live
     * 0x68-byte plain allocation from stock slot223/226, not arbitrary bytes.
     * memory must ALREADY have a qualified current-layout descriptor vptr and
     * geometry, and equal request+8. Do not cast request+8 to obtain it without
     * the loader's separate memory-object qualification.
     * A nonzero return leaves native wrapper release/free responsibility
     * unchanged and NEVER emits a callback. Success preserves the original
     * Family completion obligation until the original controller gate drains.
     */
    static IOReturn submit(void *controller, void *request, IOMemoryDescriptor *memory);
    // Typed targeted abort only; NOT the stock endpoint/pipe abort signature.
    static IOReturn abort(void *controller, void *request,
                          X9USBRequestBridge::AbortResult *);
    static IOReturn rearm(void *controller);

private:
    enum class Phase : uint8_t { Free, Submitting, Accepted, Delivering };
    struct Entry {
        Phase phase;
        void *request;
        IOMemoryDescriptor *memory;
        uint32_t endpoint;
        uint64_t requested, target, action, parameter;
        X9USBRequestOwner::Ticket ticket;
    } entries[X9USBRequestOwner::Capacity] = {};
    IOWorkLoop *loop = nullptr;
    void *controller = nullptr;
    FamilyComplete familyComplete = nullptr;
    static X9USBNativeRequestAdapter instance;
    IOReturn check(void *) const;
    IOReturn enqueue(void *, IOMemoryDescriptor *);
    static void completed(void *, void *, IOReturn, uint32_t);
};
#endif
