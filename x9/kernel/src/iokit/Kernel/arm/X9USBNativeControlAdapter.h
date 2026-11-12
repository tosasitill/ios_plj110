/* Qualified plain EP0-IN requests, separate from the QMU bulk owner. */
#ifndef X9_USB_NATIVE_CONTROL_ADAPTER_H
#define X9_USB_NATIVE_CONTROL_ADAPTER_H
#include "X9USBControlBridge.h"
class X9USBNativeControlAdapter final {
public:
    using FamilyComplete = void (*)(void *controller, void *request);
    /* Same current-layout gate, after request/control bridges install. Future
     * loader must qualify and boot-pin controller, native callback targets,
     * plain allocator and FamilyComplete; the control object's packet size
     * must remain 64 and flag88 bit0 clear (USB2 PIO, not Family speed enum3).
     * Does not enable native USB/profile. */
    static IOReturn bind(IOWorkLoop *, void *controller, FamilyComplete,
                         uint32_t qualifiedPacketSize, uint32_t qualifiedControlFlags);
    /* Reached by the ordinary adapter only for endpoint 0x80. memory must
     * independently qualify as current-layout and equal original request+8.
     * IN data/explicit ZLP and OUT/no-data IN status have distinct ownership.
     * Reject = original wrapper cleanup unchanged; success = Family callback
     * obligation or explicit quarantine, NEVER permission to free/resubmit. */
    static IOReturn submit(void *controller, void *request, IOMemoryDescriptor *memory);
    // ControlBridge invokes in its exact gate. Consumes only internal IN
    // stage events (or a SETUP rejected by a latched quarantine). SETUP/OUT/
    // terminal events still go to the loader's qualified control-object glue.
    static bool event(void *controller, const x9_usb_ep0_event &);
    static IOReturn rearmReady(void *controller);
    // Exact gate, boot-long failure. Keep accepted/prepared request obligations
    // when the qualified control glue observes an invalid native transition.
    static IOReturn isolate(void *controller, IOReturn reason);
private:
    enum class Phase : uint8_t { Free, Preparing, Accepted, Delivering, Quarantined };
    enum class Kind : uint8_t { Data, ZLP, Status };
    struct Entry {
        Phase phase;
        Kind kind;
        void *request;
        IOMemoryDescriptor *memory;
        uint64_t requested, target, action, parameter;
        bool prepared;
    } entries[2] = {};
    enum class Stage : uint8_t { Idle, WaitData, DataAccepted, DataRetired,
                                WaitOut, WaitStatus, StatusAccepted } stage = Stage::Idle;
    IOWorkLoop *loop = nullptr;
    void *controller = nullptr;
    FamilyComplete familyComplete = nullptr;
    uint64_t ticket = 0;
    uint32_t wireLength = 0, outProgress = 0;
    bool usbIn = false, needsZLP = false, inOperation = false;
    IOReturn failure = kIOReturnSuccess;
    static X9USBNativeControlAdapter instance;
    static uint8_t in[X9_USB_EP0_CAPACITY];
    IOReturn check(void *) const;
    IOReturn enqueue(void *, IOMemoryDescriptor *);
    bool process(const x9_usb_ep0_event &);
    bool complete(Entry &, IOReturn, uint32_t);
    IOReturn rejectPrepared(Entry &, IOReturn);
    void quarantine(Entry &, IOReturn);
};
#endif
