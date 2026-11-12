/* Kernel-private gate/worker exchange. Not an Apple class or user ABI. */
#ifndef X9_USB_REQUEST_BRIDGE_H
#define X9_USB_REQUEST_BRIDGE_H
#include "X9USBRequestOwner.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>

class X9USBCompletionSource;
class X9USBRequestBridge final : public OSObject {
    OSDeclareDefaultStructors(X9USBRequestBridge);
public:
    struct Backend {
        void *context;
        // Must qualify the actual endpoint before descriptor prepare/admission.
        IOReturn (*validateEndpoint)(void *, uint32_t endpoint,
                                    const X9USBRequestOwner::Input &);
        // Sole worker only. available=false permits drain/STOP, never publish.
        // A real MTU3 endpoint backend must implement this, not a success stub.
        void (*poll)(void *, X9USBRequestBridge &, X9USBRequestOwner &,
                     bool available, uint64_t nowNS);
        // Explicit gate command only, with ALL exchange callbacks finished.
        // Qualify real endpoint readiness, then owner.reopen. Never enable USB.
        IOReturn (*rearm)(void *, X9USBRequestOwner &);
    };
    /* Install once AFTER native ABI, endpoint backend and controller workloop
     * qualify (workloop must use current XNU layout/ABI). Backend/context and
     * callback contexts must be pinned for this boot; opaque native pointers
     * are NOT cast to OSObject or automatically retained. No current
     * caller exists; MTU3 bulk backend remains dormant. Installation DOES NOT enable
     * native mode, change the USB profile or set native_apple_stack.
     * The registry retains this object/workloop/event source for this boot.
     */
    static IOReturn install(IOWorkLoop *, const Backend &);
    // Identity check for the ABI adapter, not a USB readiness/profile setter.
    static bool installedOn(IOWorkLoop *);
    /* Must be called with the SAME native controller workloop gate held.
     * Timeout cancels only an unclaimed Queued entry. Once worker claims it,
     * wait uninterruptibly for definitive accept/reject; NEVER return timeout
     * while native Family might still free an accepted request as a reject.
     */
    static IOReturn submit(uint32_t endpoint, const X9USBRequestOwner::Input &,
                           X9USBRequestOwner::Ticket *);
    struct AbortResult {
        IOReturn completionStatus = kIOReturnNotReady;
        uint32_t actual = 0;
        bool actualQualified = false, completionQueued = false;
    };
    /* Same controller gate, never worker. A claimed abort waits for descriptor
     * complete + queued Family completion OR definitive quarantine. Every
     * return preserves the accepted request's original completion obligation:
     * caller MUST NOT free/release/re-submit it on abort error. Success does
     * not mean callback ran; this gate still excludes callback execution.
     * Timeout only retracts a command the worker has not claimed. This is our
     * typed contract, NOT a qualified stock Family AbortPipe signature.
     */
    static IOReturn abort(X9USBRequestOwner::Ticket, AbortResult *);
    /* Requires no Queued/Accepted/Delivering/CompletionQueued entries, including
     * native callbacks and exchange descriptor release. Not an automatic
     * bus-on/reset recovery or a way to clear quarantine. */
    static IOReturn rearm();
    // Scheduler transfers each acknowledged request to backend exactly once.
    IOReturn workerNext(uint32_t *, X9USBRequestOwner::Ticket *,
                        X9USBRequestOwner::Input *);
    IOReturn workerFailure(); // latched complete error, not discarded
    void workerPoll(bool available, uint64_t nowNS);
    bool drainNativeCompletions();
    void free() APPLE_KEXT_OVERRIDE;

private:
    enum class Phase : uint8_t { Free, Queued, Claimed, Accepted, Rejected,
                                CompletionQueued, Delivering };
    struct Entry {
        Phase phase;
        X9USBRequestBridge *bridge;
        X9USBRequestOwner::Input input;
        X9USBRequestOwner::Ticket ticket;
        uint64_t serial;
        uint32_t endpoint, actual;
        IOReturn admissionResult, completionResult;
        bool admissionDone, acknowledged, scheduled;
    };
    Entry entries[X9USBRequestOwner::Capacity] = {};
    enum class ControlOp : uint8_t { Abort, Rearm };
    enum class ControlPhase : uint8_t { Free, Queued, Claimed, Done };
    struct Control {
        ControlPhase phase;
        ControlOp operation;
        X9USBRequestOwner::Ticket ticket;
        IOReturn result;
        AbortResult abortResult;
    } control = {};
    X9USBRequestOwner requests;
    IOWorkLoop *loop = nullptr;
    X9USBCompletionSource *source = nullptr;
    IOLock *lock = nullptr;
    Backend backend = {};
    thread_t worker = THREAD_NULL;
    uint64_t nextSerial = 0;
    IOReturn completionFailure = kIOReturnSuccess;
    bool registered = false, draining = false;
    IOReturn enqueueRequest(uint32_t, const X9USBRequestOwner::Input &,
                     X9USBRequestOwner::Ticket *);
    IOReturn enqueueControl(ControlOp, X9USBRequestOwner::Ticket, AbortResult *);
    void startControl(bool available);
    void resolveAbort();
    void finishControl(IOReturn, const AbortResult &);
    static void completed(void *, void *, IOReturn, uint32_t);
};
#endif
