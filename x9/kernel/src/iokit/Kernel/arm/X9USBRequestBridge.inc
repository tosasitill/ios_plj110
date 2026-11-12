/* Sole worker performs prepare/ownership; original workloop performs native
 * completion. No lock is held across descriptor methods or a native callback.
 * Default registry is empty; existing ECM/log/private paths remain unchanged.
 */
#include "X9USBRequestBridge.h"
#include <IOKit/IOEventSource.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <sys/systm.h>

static LCK_GRP_DECLARE(x9_usb_native_group, "x9_usb_native");
static LCK_MTX_DECLARE(x9_usb_native_registry_lock, &x9_usb_native_group);
static X9USBRequestBridge *x9_usb_native_bridge;
static bool x9_usb_native_installing;

class X9USBCompletionSource final : public IOEventSource {
    OSDeclareDefaultStructors(X9USBCompletionSource);
    X9USBRequestBridge *bridge = nullptr;
public:
    bool initWithBridge(X9USBRequestBridge *b)
    {
        if (!IOEventSource::init(b)) return false;
        bridge = b; return true;
    }
    bool checkForWork() APPLE_KEXT_OVERRIDE
    { return bridge && bridge->drainNativeCompletions(); }
    void notify() { signalWorkAvailable(); }
};
OSDefineMetaClassAndStructors(X9USBCompletionSource, IOEventSource);
OSDefineMetaClassAndStructors(X9USBRequestBridge, OSObject);

static X9USBRequestBridge *usb_native_registry()
{
    lck_mtx_lock(&x9_usb_native_registry_lock);
    X9USBRequestBridge *b = x9_usb_native_bridge;
    lck_mtx_unlock(&x9_usb_native_registry_lock);
    return b; // successful installation owns a permanent registry reference
}
bool X9USBRequestBridge::installedOn(IOWorkLoop *workloop)
{
    X9USBRequestBridge *b = usb_native_registry();
    return workloop && b && b->loop == workloop;
}
IOReturn X9USBRequestBridge::install(IOWorkLoop *workloop, const Backend &ops)
{
    if (!workloop || !ops.validateEndpoint || !ops.poll || !ops.rearm)
        return kIOReturnBadArgument;
    lck_mtx_lock(&x9_usb_native_registry_lock);
    if (x9_usb_native_bridge || x9_usb_native_installing) {
        lck_mtx_unlock(&x9_usb_native_registry_lock); return kIOReturnExclusiveAccess;
    }
    x9_usb_native_installing = true;
    lck_mtx_unlock(&x9_usb_native_registry_lock);
    auto *b = new X9USBRequestBridge;
    IOReturn result = kIOReturnNoMemory;
    if (b && b->init()) {
        b->lock = IOLockAlloc();
        b->loop = workloop; workloop->retain(); b->backend = ops;
        b->source = new X9USBCompletionSource;
        if (b->lock && b->source && b->source->initWithBridge(b))
            result = workloop->addEventSource(b->source);
    }
    lck_mtx_lock(&x9_usb_native_registry_lock);
    if (result == kIOReturnSuccess) {
        b->registered = true;
        x9_usb_native_bridge = b; // initial OSObject retain becomes registry's
    }
    x9_usb_native_installing = false;
    lck_mtx_unlock(&x9_usb_native_registry_lock);
    if (result != kIOReturnSuccess && b) b->release();
    return result;
}
void X9USBRequestBridge::free()
{
    if (registered) panic("X9 USB: freeing registered native request bridge");
    if (source) { source->release(); source = nullptr; }
    if (loop) { loop->release(); loop = nullptr; }
    if (lock) { IOLockFree(lock); lock = nullptr; }
    OSObject::free();
}
IOReturn X9USBRequestBridge::submit(uint32_t endpoint,
    const X9USBRequestOwner::Input &input, X9USBRequestOwner::Ticket *ticket)
{
    X9USBRequestBridge *b = usb_native_registry();
    return b ? b->enqueueRequest(endpoint, input, ticket) : kIOReturnNotReady;
}
IOReturn X9USBRequestBridge::abort(X9USBRequestOwner::Ticket ticket, AbortResult *result)
{
    if (!result) return kIOReturnBadArgument;
    *result = {}; // actualQualified=false, never assert zero wire on error
    if (!ticket.generation || !ticket.serial) return kIOReturnBadArgument;
    X9USBRequestBridge *b = usb_native_registry();
    return b ? b->enqueueControl(ControlOp::Abort, ticket, result) : kIOReturnNotReady;
}
IOReturn X9USBRequestBridge::rearm()
{
    X9USBRequestBridge *b = usb_native_registry();
    return b ? b->enqueueControl(ControlOp::Rearm, {}, nullptr) : kIOReturnNotReady;
}
IOReturn X9USBRequestBridge::enqueueControl(ControlOp operation,
    X9USBRequestOwner::Ticket ticket, AbortResult *result)
{
    if (!loop->inGate()) return kIOReturnNotPermitted;
    uint64_t deadline; clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    IOLockLock(lock);
    if (worker == THREAD_NULL) { IOLockUnlock(lock); return kIOReturnNotReady; }
    if (current_thread() == worker) { IOLockUnlock(lock); return kIOReturnNotPermitted; }
    if (control.phase != ControlPhase::Free) { IOLockUnlock(lock); return kIOReturnBusy; }
    control = {}; control.operation = operation; control.ticket = ticket;
    control.phase = ControlPhase::Queued;
    while (control.phase != ControlPhase::Done) {
        if (control.phase == ControlPhase::Queued) {
            if (mach_absolute_time() >= deadline) {
                control = {}; // lock proves no cancellation/reopen reached worker
                IOLockUnlock(lock); return kIOReturnTimeout;
            }
            (void)IOLockSleepDeadline(lock, &control, deadline, THREAD_UNINT);
        } else {
            // Once claimed, a deadline cannot revoke STOP/complete/reopen.
            // No caller stack pointer or native callback is retained by C.
            (void)IOLockSleep(lock, &control, THREAD_UNINT);
        }
    }
    IOReturn status = control.result;
    if (result) *result = control.abortResult;
    control = {};
    IOLockUnlock(lock);
    return status;
}
void X9USBRequestBridge::finishControl(IOReturn status, const AbortResult &result)
{
    IOLockLock(lock);
    if (control.phase != ControlPhase::Claimed)
        panic("X9 USB: native control finished without worker claim");
    control.result = status; control.abortResult = result;
    control.phase = ControlPhase::Done;
    IOLockWakeup(lock, &control, false);
    IOLockUnlock(lock);
}
void X9USBRequestBridge::startControl(bool available)
{
    IOLockLock(lock);
    if (control.phase != ControlPhase::Queued) { IOLockUnlock(lock); return; }
    control.phase = ControlPhase::Claimed;
    ControlOp operation = control.operation;
    auto ticket = control.ticket;
    bool emptyExchange = true;
    Entry *target = nullptr;
    for (auto &e : entries) {
        if (e.phase != Phase::Free) emptyExchange = false;
        if (e.phase != Phase::Free && e.ticket.generation == ticket.generation &&
            e.ticket.serial == ticket.serial) target = &e;
    }
    Phase phase = target ? target->phase : Phase::Free;
    bool acknowledged = target && target->acknowledged;
    IOLockUnlock(lock);
    if (operation == ControlOp::Rearm) {
        IOReturn status = completionFailure != kIOReturnSuccess ? completionFailure :
            !available ? kIOReturnOffline : !emptyExchange ? kIOReturnBusy :
            backend.rearm(backend.context, requests);
        finishControl(status, {});
        return;
    }
    if (phase == Phase::CompletionQueued) { resolveAbort(); return; }
    if (phase != Phase::Accepted || !acknowledged) {
        finishControl(phase == Phase::Free ? kIOReturnNotFound : kIOReturnBusy, {});
        return;
    }
    X9USBRequestOwner::State state; IOReturn reason;
    IOReturn status = requests.inspect(ticket, &state, &reason);
    if (status != kIOReturnSuccess) { finishControl(status, {}); return; }
    if (state == X9USBRequestOwner::State::Quarantined) {
        finishControl(requests.failure(), {}); return;
    }
    if (state != X9USBRequestOwner::State::Terminal &&
        state != X9USBRequestOwner::State::Stopping) {
        status = requests.cancel(ticket, kIOReturnAborted);
        if (status != kIOReturnSuccess) { finishControl(status, {}); return; }
    }
    // Claimed remains pinned until backend STOP/retire and descriptor complete.
    // The matching native completion still runs only after this gate returns.
}
void X9USBRequestBridge::resolveAbort()
{
    IOLockLock(lock);
    if (control.phase != ControlPhase::Claimed || control.operation != ControlOp::Abort) {
        IOLockUnlock(lock); return;
    }
    auto ticket = control.ticket;
    Entry *target = nullptr;
    for (auto &e : entries)
        if (e.phase != Phase::Free && e.ticket.generation == ticket.generation &&
            e.ticket.serial == ticket.serial) target = &e;
    Phase phase = target ? target->phase : Phase::Free;
    AbortResult result = {};
    if (phase == Phase::CompletionQueued)
        result = { target->completionResult, target->actual, true, true };
    IOLockUnlock(lock);
    if (phase == Phase::CompletionQueued) { finishControl(kIOReturnSuccess, result); return; }
    if (phase != Phase::Accepted) {
        finishControl(phase == Phase::Free ? kIOReturnNotFound : kIOReturnBusy, {}); return;
    }
    X9USBRequestOwner::State state; IOReturn reason;
    IOReturn status = requests.inspect(ticket, &state, &reason);
    if (status != kIOReturnSuccess) { finishControl(status, {}); return; }
    if (state == X9USBRequestOwner::State::Quarantined)
        finishControl(requests.failure(), {});
    // Terminal but no completion (e.g. complete still pending) is NOT success.
    // Wait for the next bounded worker poll, not for a callback on this gate.
}
IOReturn X9USBRequestBridge::enqueueRequest(uint32_t endpoint,
    const X9USBRequestOwner::Input &input, X9USBRequestOwner::Ticket *ticket)
{
    if (!loop->inGate()) return kIOReturnNotPermitted;
    if (!ticket || !input.nativeRequest || !input.memory || !input.completion ||
        input.requested > UINT32_MAX) return kIOReturnBadArgument;
    if (input.isochronous || input.reusable) return kIOReturnUnsupported;
    // One additional retain belongs to this exchange THROUGH native callback.
    // It replaces the worker-owner retain when completion is deferred to loop.
    input.memory->retain();
    uint64_t deadline; clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    IOLockLock(lock);
    if (worker == THREAD_NULL) {
        IOLockUnlock(lock); input.memory->release(); return kIOReturnNotReady;
    }
    if (current_thread() == worker) {
        IOLockUnlock(lock); input.memory->release(); return kIOReturnNotPermitted;
    }
    if (nextSerial == UINT64_MAX) {
        IOLockUnlock(lock); input.memory->release(); return kIOReturnNoResources;
    }
    Entry *room = nullptr;
    for (auto &e : entries) {
        if (e.phase == Phase::Free) { if (!room) room = &e; }
        else if (e.input.nativeRequest == input.nativeRequest || e.input.memory == input.memory) {
            IOLockUnlock(lock); input.memory->release(); return kIOReturnExclusiveAccess;
        }
    }
    if (!room) { IOLockUnlock(lock); input.memory->release(); return kIOReturnNoResources; }
    *room = {};
    room->bridge = this; room->input = input; room->endpoint = endpoint;
    room->serial = ++nextSerial; room->phase = Phase::Queued;
    // Worker is polled at 1ms; no new thread, MMIO writer or mutable stack
    // pointer is passed to it. The persistent entry owns the copied inputs.
    while (!room->admissionDone) {
        if (room->phase == Phase::Queued) {
            if (mach_absolute_time() >= deadline) {
                *room = {}; // lock proves worker has not claimed this request
                IOLockUnlock(lock); input.memory->release(); return kIOReturnTimeout;
            }
            (void)IOLockSleepDeadline(lock, room, deadline, THREAD_UNINT);
        } else {
            // No ambiguous timeout/error once the worker could have accepted.
            (void)IOLockSleep(lock, room, THREAD_UNINT);
        }
    }
    IOReturn result = room->admissionResult;
    if (result == kIOReturnSuccess) {
        *ticket = room->ticket;
        room->acknowledged = true;
        bool completionPending = room->phase == Phase::CompletionQueued;
        // Worker may now schedule DMA. Native completion source remains
        // blocked by this caller's original workloop gate until action returns.
        IOLockUnlock(lock);
        if (completionPending) source->notify(); // offline completion may predate ack
    } else {
        *room = {};
        IOLockUnlock(lock); input.memory->release();
    }
    return result;
}
void X9USBRequestBridge::completed(void *parameter, void *nativeRequest,
                                  IOReturn status, uint32_t actual)
{
    auto *e = static_cast<Entry *>(parameter);
    X9USBRequestBridge *b = e->bridge;
    IOLockLock(b->lock);
    if (e->phase != Phase::Accepted || e->input.nativeRequest != nativeRequest ||
        !e->admissionDone || e->admissionResult != kIOReturnSuccess)
        panic("X9 USB: duplicate/foreign native worker completion");
    e->completionResult = status; e->actual = actual;
    e->phase = Phase::CompletionQueued;
    IOLockUnlock(b->lock);
    // Nonblocking; NEVER call the original gate/Family action from worker.
    b->source->notify();
}
IOReturn X9USBRequestBridge::workerNext(uint32_t *endpoint,
    X9USBRequestOwner::Ticket *ticket, X9USBRequestOwner::Input *input)
{
    if (!endpoint || !ticket || !input) return kIOReturnBadArgument;
    IOLockLock(lock);
    if (worker != current_thread()) { IOLockUnlock(lock); return kIOReturnNotPermitted; }
    Entry *next = nullptr;
    for (auto &e : entries)
        if (e.phase == Phase::Accepted && e.acknowledged && !e.scheduled &&
            (!next || e.serial < next->serial)) next = &e;
    if (!next) { IOLockUnlock(lock); return kIOReturnNotFound; }
    next->scheduled = true;
    *endpoint = next->endpoint; *ticket = next->ticket; *input = next->input;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}
IOReturn X9USBRequestBridge::workerFailure()
{
    IOLockLock(lock); bool correct = worker == current_thread(); IOLockUnlock(lock);
    return correct ? completionFailure : kIOReturnNotPermitted;
}
void X9USBRequestBridge::workerPoll(bool available, uint64_t nowNS)
{
    IOLockLock(lock);
    bool first = worker == THREAD_NULL;
    if (first) worker = current_thread();
    bool correct = worker == current_thread();
    IOLockUnlock(lock);
    if (!correct) panic("X9 USB: native request poll on a second worker");
    if (first && requests.bindWorker() != kIOReturnSuccess)
        panic("X9 USB: native request worker binding failed");
    if (completionFailure != kIOReturnSuccess) available = false;
    if (!available) (void)requests.close(kIOReturnOffline);
    // Explicit control command only; a bus-on cannot clear quarantine/reopen.
    startControl(available);
    for (unsigned n = 0; n < X9USBRequestOwner::Capacity; n++) {
        IOLockLock(lock);
        Entry *e = nullptr;
        for (auto &candidate : entries)
            if (candidate.phase == Phase::Queued &&
                (!e || candidate.serial < e->serial)) e = &candidate;
        if (!e) { IOLockUnlock(lock); break; }
        e->phase = Phase::Claimed;
        auto input = e->input; uint32_t endpoint = e->endpoint;
        IOLockUnlock(lock);
        // validate/prepare can sleep, but never while holding exchange lock.
        IOReturn result = available ? backend.validateEndpoint(backend.context,
            endpoint, input) : kIOReturnOffline;
        X9USBRequestOwner::Ticket ticket = {};
        if (result == kIOReturnSuccess) {
            input.context = e; input.completion = completed;
            result = requests.accept(input, &ticket);
        }
        IOLockLock(lock);
        e->ticket = ticket; e->admissionResult = result; e->admissionDone = true;
        e->phase = result == kIOReturnSuccess ? Phase::Accepted : Phase::Rejected;
        IOLockWakeup(lock, e, false);
        IOLockUnlock(lock);
    }
    (void)requests.expire(nowNS);
    // The eventual MTU3 scheduler consumes acknowledged entries via workerNext
    // and invokes owner begin/publish/retire with real hardware evidence.
    backend.poll(backend.context, *this, requests, available, nowNS);
    IOReturn error = requests.drainCompletions(); // only queues workloop completions
    if (error != kIOReturnSuccess && completionFailure == kIOReturnSuccess)
        completionFailure = error; // native request remains retained/quarantined
    if (completionFailure == kIOReturnSuccess) completionFailure = requests.failure();
    resolveAbort(); // only after descriptor complete/latched quarantine
}
bool X9USBRequestBridge::drainNativeCompletions()
{
    if (!loop->inGate()) panic("X9 USB: native completion outside controller gate");
    if (draining) return false;
    draining = true;
    unsigned batch[X9USBRequestOwner::Capacity], count = 0;
    IOLockLock(lock);
    for (unsigned n = 0; n < X9USBRequestOwner::Capacity; n++) {
        Entry &e = entries[n];
        if (e.phase == Phase::CompletionQueued && e.acknowledged) {
            e.phase = Phase::Delivering; batch[count++] = n;
        }
    }
    IOLockUnlock(lock);
    for (unsigned n = 0; n < count; n++) {
        Entry &e = entries[batch[n]];
        auto input = e.input;
        input.completion(input.context, input.nativeRequest, e.completionResult, e.actual);
        // Family adapter may have freed request. Only exchange's separate
        // descriptor retain is accessed after callback; no request dereference.
        input.memory->release();
        IOLockLock(lock); e = {}; IOLockUnlock(lock);
    }
    IOLockLock(lock);
    bool more = false;
    for (auto &e : entries)
        more |= e.phase == Phase::CompletionQueued && e.acknowledged;
    IOLockUnlock(lock);
    draining = false;
    return more;
}
extern "C" void x9_usb_native_worker_poll(unsigned available, uint64_t nowNS)
{
    if (X9USBRequestBridge *b = usb_native_registry()) b->workerPoll(available != 0, nowNS);
}
extern "C" int x9_usb_native_worker_quiesce(uint64_t nowNS)
{
    X9USBRequestBridge *b = usb_native_registry();
    if (!b) return 0;
    b->workerPoll(false, nowNS); // close admission, physical STOP before reset
    return b->workerFailure() == kIOReturnSuccess ? 0 : EIO;
}
