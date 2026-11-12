/* Qualified original control methods -> current-layout EP0 gate/worker.
 * No Apple class definition, hardcoded callable address or native-mode setter. */
#ifndef X9_USB_NATIVE_CONTROL_GLUE_H
#define X9_USB_NATIVE_CONTROL_GLUE_H
#include "X9USBNativeControlAdapter.h"
class X9USBNativeControlGlue final {
public:
    struct Snapshot {
        void *controller;
        uint32_t state, inEndpoint, outEndpoint, flags, familySpeed, packetSize;
        uint64_t requested, progress;
        bool hasDataMemory;
        uint8_t setup[8];
    };
    struct Operations {
        // Loader-qualified leaf thunks: decode an independently qualified
        // 20H392 control object, never cast it to a current OSObject. inspect
        // must qualify all fields, current descriptor geometry/nested prepare,
        // native targets and controller/gate identity before returning true.
        bool (*inspect)(void *control, Snapshot *);
        void (*inputSetup)(void *control, const uint8_t *setup8);
        IOReturn (*processSetup)(void *control);
        void (*inputData)(void *control, const uint8_t *packet, uint64_t bytes);
        void (*reset)(void *control);
    };
    // After ordinary adapter binding, BEFORE native USB can run. Every object,
    // thunk and patched controller method must be boot-pinned by the loader.
    // This installs the actual handler, not the missing Apple module loader,
    // configuration/endpoint backend, or native controller/profile switch.
    static IOReturn install(IOWorkLoop *, void *controller, void *control,
                            X9USBNativeControlAdapter::FamilyComplete, const Operations &);
    // Qualified controller slot213 thunk only. state5/6 arms packet OUT after
    // the original receiveControlOutData owns its descriptor. Other states
    // do not synthesize status or skip original ordinary-request callbacks.
    static IOReturn stateChanged(void *controller, uint32_t previous, uint32_t current);
    // Qualified controller slot210 thunk, ONLY packet-report event11/12. The
    // source is an ephemeral zeroed 0x68B STACK record, not a heap IO request;
    // never submit/retain/free it. Exact active SETUP/data/status snapshot only.
    static IOReturn acknowledgePacket(void *controller, const void *record,
                                      uint32_t eventCode, IOReturn status);
    // Qualified controller's EP0 halt-on thunk only. Idempotent for the two
    // OUT0/IN80 calls made by native stallSetupRequest. No clear-halt mapping.
    static IOReturn requestStall(void *controller, uint32_t endpoint);
    // ControlBridge observes the terminal BEFORE ordinary request callbacks,
    // so their native stall calls cannot turn an existing abort into new I/O.
    static void observe(void *controller, const x9_usb_ep0_event &);
    static IOReturn ready(void *controller);
private:
    enum class Phase : uint8_t { Idle, Setup, Running, Packet, Status, Reset, Fault } phase = Phase::Idle;
    IOWorkLoop *loop = nullptr;
    void *controller = nullptr, *control = nullptr;
    Operations ops = {};
    uint64_t ticket = 0;
    uint32_t wireLength = 0, outProgress = 0, activeBytes = 0;
    const uint8_t *activePacket = nullptr;
    bool usbIn = false, receiving = false, acknowledged = false, stalled = false, terminalError = false;
    uint8_t setup[8] = {};
    IOReturn failure = kIOReturnSuccess;
    static X9USBNativeControlGlue instance;
    static bool installing;
    static void handle(void *, const x9_usb_ep0_event &, const uint8_t *);
    IOReturn check(void *) const;
    bool inspect(Snapshot &) const;
    void fail(IOReturn);
    void command(unsigned);
    void process(const x9_usb_ep0_event &, const uint8_t *);
};
#endif
