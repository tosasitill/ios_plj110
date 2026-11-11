/* Compiled with SweetX9USB.cpp, but not instantiated or advertised as a native
 * controller until native ABI/bootstrap and MTU3 endpoint dispatch qualify.
 * Uses current XNU IOMemoryDescriptor methods, never old numeric vslots.
 */
#include "X9USBRequestOwner.h"
#include <sys/systm.h>

X9USBRequestOwner::X9USBRequestOwner() = default;
X9USBRequestOwner::~X9USBRequestOwner()
{
    for (const auto &s : slots)
        if (s.state != State::Free)
            panic("X9 USB: destroying live native request ownership");
}
bool X9USBRequestOwner::onWorker() const
{ return worker != THREAD_NULL && worker == current_thread(); }
IOReturn X9USBRequestOwner::bindWorker()
{
    if (worker != THREAD_NULL) return kIOReturnExclusiveAccess;
    worker = current_thread();
    closed = false;
    return kIOReturnSuccess;
}
const X9USBRequestOwner::Slot *X9USBRequestOwner::find(Ticket t) const
{
    if (!t.serial || !t.generation) return nullptr;
    for (const auto &s : slots)
        if (s.state != State::Free && s.ticket.serial == t.serial &&
            s.ticket.generation == t.generation) return &s;
    return nullptr;
}
X9USBRequestOwner::Slot *X9USBRequestOwner::find(Ticket t)
{ return const_cast<Slot *>(static_cast<const X9USBRequestOwner *>(this)->find(t)); }
X9USBRequestOwner::Slot *X9USBRequestOwner::find(Chunk c)
{
    Slot *s = find(c.request);
    return s && c.serial && s->chunkSerial == c.serial &&
        s->chunkLength == c.length ? s : nullptr;
}
void X9USBRequestOwner::terminal(Slot &s, IOReturn status)
{
    s.status = status;
    s.deadlineNS = 0;
    s.state = State::Terminal;
}
void X9USBRequestOwner::quarantine(Slot &s, IOReturn status)
{
    s.status = status == kIOReturnSuccess ? kIOReturnIOError : status;
    s.deadlineNS = 0;
    s.state = State::Quarantined;
    closed = true;
    for (auto &other : slots) (void)cancelSlot(other, kIOReturnOffline);
    /* Deliberately no complete/release/callback/reuse. Worker must also retain
     * its QMU allocation. There is no implicit quarantine recovery API. */
}
IOReturn X9USBRequestOwner::accept(const Input &i, Ticket *ticket)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (closed) return kIOReturnOffline;
    if (!ticket || !i.nativeRequest || !i.memory || !i.completion ||
        i.requested > UINT32_MAX) return kIOReturnBadArgument;
    if (i.isochronous || i.reusable) return kIOReturnUnsupported;
    // getLength/prepare/complete are serialized even against callback reentry.
    inOperation = true;
    if (i.requested > i.memory->getLength()) {
        inOperation = false; return kIOReturnBadArgument;
    }
    Slot *room = nullptr;
    for (auto &s : slots) {
        if (s.state == State::Free) { if (!room) room = &s; }
        else if (s.input.nativeRequest == i.nativeRequest || s.input.memory == i.memory) {
            inOperation = false; return kIOReturnExclusiveAccess;
        }
    }
    if (!room || nextSerial == UINT64_MAX) {
        inOperation = false; return kIOReturnNoResources;
    }
    i.memory->retain(); // separate from Family's descriptor@8 retain
    IOReturn result = i.requested ? i.memory->prepare(
        i.usbIn ? kIODirectionOut : kIODirectionIn) : kIOReturnSuccess;
    if (result != kIOReturnSuccess) {
        // A failed prepare is not a successful prepare/complete pair.
        i.memory->release(); inOperation = false; return result;
    }
    *room = {};
    room->input = i;
    room->ticket = { generation, ++nextSerial };
    room->prepared = i.requested != 0;
    room->state = State::Ready;
    *ticket = room->ticket;
    inOperation = false;
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::beginChunk(Ticket t, void *bounce, uint32_t length,
                                     uint64_t nowNS, Chunk *chunk)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (closed) return kIOReturnOffline;
    Slot *s = find(t);
    if (!s) return kIOReturnNotFound;
    if (s->state != State::Ready) return kIOReturnNotReady;
    if (!chunk || (length && !bounce) || length > 4096 ||
        length > s->input.requested - s->actual ||
        (!length && s->input.requested)) return kIOReturnBadArgument;
    if (s->chunkSerial == UINT64_MAX) {
        terminal(*s, kIOReturnNoResources); return kIOReturnNoResources;
    }
    if (s->started && s->deadlineNS && nowNS >= s->deadlineNS) {
        terminal(*s, kIOReturnTimeout); return kIOReturnTimeout;
    }
    inOperation = true;
    if (s->input.usbIn && length &&
        s->input.memory->readBytes(s->actual, bounce, length) != length) {
        terminal(*s, kIOReturnVMError);
        inOperation = false; return kIOReturnVMError;
    }
    s->chunkLength = length;
    s->chunkSerial++;
    s->state = State::Staged;
    *chunk = { s->ticket, s->chunkSerial, length };
    inOperation = false;
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::publishChunk(Chunk c, uint64_t nowNS)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (closed) return kIOReturnOffline;
    Slot *s = find(c);
    if (!s) return kIOReturnNotFound;
    if (s->state != State::Staged) return kIOReturnNotReady;
    if (s->started && s->deadlineNS && nowNS >= s->deadlineNS) {
        terminal(*s, kIOReturnTimeout); return kIOReturnTimeout;
    }
    if (!s->started) {
        uint64_t interval = (uint64_t)s->input.timeoutMS * 1000000;
        s->deadlineNS = interval ? (nowNS > UINT64_MAX - interval ?
                                    UINT64_MAX : nowNS + interval) : 0;
        s->started = true;
    }
    s->state = State::Active;
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::rejectUnpublished(Chunk c, IOReturn reason)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (reason == kIOReturnSuccess) return kIOReturnBadArgument;
    Slot *s = find(c);
    if (!s) return kIOReturnNotFound;
    if (s->state != State::Staged) return kIOReturnNotReady;
    terminal(*s, reason);
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::cancelSlot(Slot &s, IOReturn reason)
{
    if (s.state == State::Ready || s.state == State::Staged) terminal(s, reason);
    else if (s.state == State::Active) {
        s.state = State::Stopping; s.status = reason; s.deadlineNS = 0;
    } else return kIOReturnNotReady; // first terminal/stop cause wins
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::cancel(Ticket t, IOReturn reason)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (reason == kIOReturnSuccess) return kIOReturnBadArgument;
    Slot *s = find(t);
    return s ? cancelSlot(*s, reason) : kIOReturnNotFound;
}
IOReturn X9USBRequestOwner::expire(uint64_t nowNS)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    for (auto &s : slots)
        if ((s.state == State::Active || s.state == State::Ready ||
             s.state == State::Staged) &&
            s.deadlineNS && nowNS >= s.deadlineNS)
            (void)cancelSlot(s, kIOReturnTimeout);
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::retire(Chunk c, const Retirement &r, const void *outBounce)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    Slot *s = find(c);
    if (!s) return kIOReturnNotFound;
    if (s->state != State::Active && s->state != State::Stopping)
        return kIOReturnNotReady;
    bool stopping = s->state == State::Stopping;
    bool normal = !stopping && r.status == kIOReturnSuccess;
    if (!r.stopped || !r.actualQualified || r.actual > s->chunkLength ||
        (normal && (!r.hwoClear || !r.cprAtTerminator ||
                    (s->input.usbIn && r.actual != s->chunkLength)))) {
        quarantine(*s, kIOReturnIOError); return kIOReturnIOError;
    }
    IOReturn status = stopping ? s->status : r.status;
    inOperation = true;
    // Actual is wire bytes, NOT copied bytes or residual. Preserve it on error.
    uint32_t offset = s->actual;
    s->actual += r.actual; // bounded by chunkLength and remaining in beginChunk
    if (!s->input.usbIn && r.actual && (!outBounce ||
        s->input.memory->writeBytes(offset, outBounce, r.actual) != r.actual)) {
        if (status == kIOReturnSuccess) status = kIOReturnVMError;
    }
    if (status != kIOReturnSuccess || r.actual < s->chunkLength ||
        s->actual == s->input.requested) terminal(*s, status);
    else s->state = State::Ready;
    inOperation = false;
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::inspect(Ticket t, State *state, IOReturn *reason) const
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (!state || !reason) return kIOReturnBadArgument;
    const Slot *s = find(t);
    if (!s) return kIOReturnNotFound;
    *state = s->state; *reason = s->status;
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::close(IOReturn reason)
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation) return kIOReturnBusy;
    if (reason == kIOReturnSuccess) return kIOReturnBadArgument;
    closed = true;
    for (auto &s : slots) (void)cancelSlot(s, reason);
    return kIOReturnSuccess;
}
bool X9USBRequestOwner::empty() const
{
    if (!onWorker() || inOperation) return false;
    for (const auto &s : slots) if (s.state != State::Free) return false;
    return true;
}
IOReturn X9USBRequestOwner::reopen()
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation || draining || !empty()) return kIOReturnBusy;
    if (!closed) return kIOReturnExclusiveAccess;
    if (generation == UINT64_MAX) return kIOReturnNoResources;
    generation++; closed = false;
    return kIOReturnSuccess;
}
IOReturn X9USBRequestOwner::drainCompletions()
{
    if (!onWorker()) return kIOReturnNotPermitted;
    if (inOperation || draining) return kIOReturnBusy;
    draining = true;
    IOReturn firstError = kIOReturnSuccess;
    unsigned batch[Capacity], count = 0;
    // Detach the entire batch before any callback. Reentrant cancel/submit can
    // only create the NEXT batch; no repeat callback or reused slot this turn.
    for (unsigned n = 0; n < Capacity; n++) if (slots[n].state == State::Terminal) {
        slots[n].state = State::Delivering; batch[count++] = n;
    }
    for (unsigned n = 0; n < count; n++) {
        Slot &s = slots[batch[n]];
        inOperation = true;
        IOReturn status = s.prepared ? s.input.memory->complete(
            s.input.usbIn ? kIODirectionOut : kIODirectionIn) : kIOReturnSuccess;
        if (status != kIOReturnSuccess) {
            // DMA stopped, but unpin/complete failed: keep the sole ownership
            // record and do not call Family (which would release/free it).
            quarantine(s, status);
            if (firstError == kIOReturnSuccess) firstError = status;
            inOperation = false; continue;
        }
        s.prepared = false;
        Input input = s.input;
        IOReturn result = s.status;
        uint32_t actual = s.actual;
        inOperation = false;
        input.completion(input.context, input.nativeRequest, result, actual);
        // Callback may have freed nativeRequest. Never dereference it again.
        inOperation = true;
        input.memory->release();
        s = {};
        inOperation = false;
    }
    draining = false;
    return firstError;
}
