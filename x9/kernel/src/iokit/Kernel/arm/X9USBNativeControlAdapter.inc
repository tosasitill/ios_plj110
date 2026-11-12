/* Stock request geometry is defined above in X9USBNativeRequestAdapter.inc.
 * Its original callback record and Family release/free remain untouched. */
#include "X9USBNativeControlAdapter.h"
// Share the ordinary adapter's binding publication lock. Included .inc files
// cannot declare startup entries on colliding __LINE__ values in one TU.
X9USBNativeControlAdapter X9USBNativeControlAdapter::instance;
uint8_t X9USBNativeControlAdapter::in[X9_USB_EP0_CAPACITY] = {};
IOReturn X9USBNativeControlAdapter::bind(IOWorkLoop *loop, void *controller, FamilyComplete complete,
    uint32_t packetSize, uint32_t controlFlags)
{
    if (!loop || !controller || !complete) return kIOReturnBadArgument;
    if (!loop->inGate()) return kIOReturnNotPermitted;
    if (packetSize != 64 || (controlFlags & 1)) return kIOReturnUnsupported;
    if (!X9USBRequestBridge::installedOn(loop) || !X9USBControlBridge::installedFor(loop, controller))
        return kIOReturnNotReady;
    if (!X9USBNativeRequestAdapter::boundFor(loop, controller, complete)) return kIOReturnNotReady;
    loop->retain();
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    auto &a = instance;
    if (a.loop) {
        lck_mtx_unlock(&x9_usb_adapter_binding_lock); loop->release();
        return kIOReturnExclusiveAccess;
    }
    a.controller = controller; a.familyComplete = complete; a.loop = loop;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock); return kIOReturnSuccess;
}
IOReturn X9USBNativeControlAdapter::check(void *controller) const
{
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    auto *workloop = loop; auto *owner = this->controller;
    bool bound = workloop && familyComplete;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock);
    if (!bound) return kIOReturnNotReady;
    if (owner != controller) return kIOReturnBadArgument;
    if (!workloop->inGate()) return kIOReturnNotPermitted;
    return X9USBControlBridge::installedFor(workloop, controller) ? kIOReturnSuccess : kIOReturnNotReady;
}
IOReturn X9USBNativeControlAdapter::submit(void *controller, void *request, IOMemoryDescriptor *memory)
{
    auto &a = instance;
    IOReturn status = a.check(controller);
    return status == kIOReturnSuccess ? a.enqueue(request, memory) : status;
}
IOReturn X9USBNativeControlAdapter::rearmReady(void *controller)
{
    auto &a = instance;
    IOReturn status = a.check(controller);
    if (status != kIOReturnSuccess) return status;
    if (a.failure != kIOReturnSuccess) return a.failure;
    if (a.inOperation || a.ticket || a.stage != Stage::Idle) return kIOReturnBusy;
    for (const auto &e : a.entries) if (e.phase != Phase::Free) return kIOReturnBusy;
    return kIOReturnSuccess;
}
IOReturn X9USBNativeControlAdapter::isolate(void *controller, IOReturn reason)
{
    auto &a = instance;
    IOReturn status = a.check(controller);
    if (status != kIOReturnSuccess) return status;
    if (reason == kIOReturnSuccess) return kIOReturnBadArgument;
    if (a.failure == kIOReturnSuccess) a.failure = reason;
    for (auto &e : a.entries)
        if (e.phase == Phase::Accepted || e.phase == Phase::Preparing) e.phase = Phase::Quarantined;
    (void)X9USBControlBridge::cancel(a.ticket, EIO);
    return kIOReturnSuccess;
}
void X9USBNativeControlAdapter::quarantine(Entry &e, IOReturn reason)
{
    e.phase = Phase::Quarantined;
    if (failure == kIOReturnSuccess) failure = reason;
    // Keep original request, extra retain and successful prepare indefinitely.
    // Cancellation does NOT fabricate an actual count or run Family cleanup.
    (void)X9USBControlBridge::cancel(ticket, EIO);
}
IOReturn X9USBNativeControlAdapter::rejectPrepared(Entry &e, IOReturn reason)
{
    IOReturn result = e.prepared ? e.memory->complete(kIODirectionOut) : kIOReturnSuccess;
    if (result != kIOReturnSuccess) {
        quarantine(e, result);
        // A reject would let the native wrapper free a still-prepared object.
        // Take ownership explicitly, although no PIO packet was published.
        inOperation = false; return kIOReturnSuccess;
    }
    auto *memory = e.memory; e = {}; memory->release();
    inOperation = false; return reason;
}
IOReturn X9USBNativeControlAdapter::enqueue(void *request, IOMemoryDescriptor *memory)
{
    if (!request || !memory) return kIOReturnBadArgument;
    if (inOperation) return kIOReturnBusy;
    if (failure != kIOReturnSuccess) return failure;
    if (!ticket || stage == Stage::Idle) return kIOReturnNotReady;
    X9USBStockRequest20H392 raw;
    memcpy(&raw, request, sizeof(raw)); // independently qualified plain 0x68B
    if (raw.endpoint != 0x80 || raw.memory != reinterpret_cast<uintptr_t>(memory) || !raw.action ||
        raw.widePayload || raw.reusable || raw.dma || raw.next || raw.status || raw.actual ||
        raw.requested > X9_USB_EP0_CAPACITY || raw.timeoutMS)
        return kIOReturnUnsupported;
    for (auto &e : entries)
        if (e.phase != Phase::Free && (e.request == request ||
            (raw.requested && e.memory == memory))) return kIOReturnExclusiveAccess;
    // The stock zero request uses control+70, which can also be the data
    // request's fallback descriptor. Zero has no prepare/DMA; separate retains
    // and completion records make this shared descriptor safe, not a duplicate.
    Kind kind;
    unsigned reply;
    Entry *room = nullptr;
    if (stage == Stage::WaitData && usbIn && wireLength && raw.requested <= wireLength) {
        kind = Kind::Data; reply = X9_EP0_SEND_IN_REQUEST; room = &entries[0];
    } else if ((stage == Stage::DataAccepted || stage == Stage::DataRetired) && needsZLP && !raw.requested) {
        kind = Kind::ZLP; reply = X9_EP0_SEND_IN_ZLP; room = &entries[1];
    } else if (stage == Stage::WaitStatus && !raw.requested && !usbIn) {
        kind = Kind::Status; reply = X9_EP0_STATUS; room = &entries[0];
    } else return kIOReturnNotReady;
    if (room->phase != Phase::Free) return kIOReturnExclusiveAccess;
    inOperation = true;
    if (raw.requested > memory->getLength()) { inOperation = false; return kIOReturnBadArgument; }
    Entry &e = *room;
    e = { Phase::Preparing, kind, request, memory, raw.requested, raw.target, raw.action, raw.parameter, false };
    memory->retain(); // separate from original Family request+8 retain
    if (raw.requested) {
        // Family also owns its control+40 prepare/complete pair; this is an
        // additional nested prepare, never a replacement for native cleanup.
        // Current-layout descriptor qualification includes nested directions.
        IOReturn status = memory->prepare(kIODirectionOut);
        if (status != kIOReturnSuccess) return rejectPrepared(e, status);
        e.prepared = true;
        if (memory->readBytes(0, in, raw.requested) != raw.requested)
            return rejectPrepared(e, kIOReturnUnderrun);
    }
    IOReturn status = X9USBControlBridge::reply(ticket, reply,
        kind == Kind::Data && raw.requested ? in : nullptr, static_cast<uint32_t>(raw.requested));
    if (status != kIOReturnSuccess) return rejectPrepared(e, status);
    e.phase = Phase::Accepted;
    if (kind == Kind::Data) {
        needsZLP = raw.requested && raw.requested < wireLength && !(raw.requested % 64);
        stage = Stage::DataAccepted;
    } else if (kind == Kind::Status) stage = Stage::StatusAccepted;
    inOperation = false; return kIOReturnSuccess;
}
bool X9USBNativeControlAdapter::complete(Entry &e, IOReturn status, uint32_t actual)
{
    if (e.phase != Phase::Accepted || actual > e.requested)
        panic("X9 USB: EP0 completion without accepted request/count");
    e.phase = Phase::Delivering;
    auto *memory = e.memory;
    if (e.prepared) {
        IOReturn result = memory->complete(kIODirectionOut);
        if (result != kIOReturnSuccess) { quarantine(e, result); return false; }
        e.prepared = false;
    }
    X9USBStockRequest20H392 raw;
    memcpy(&raw, e.request, sizeof(raw));
    if (raw.endpoint != 0x80 || raw.memory != reinterpret_cast<uintptr_t>(memory) ||
        raw.requested != e.requested || raw.target != e.target || raw.action != e.action ||
        raw.parameter != e.parameter || raw.timeoutMS || raw.widePayload || raw.reusable ||
        raw.dma || raw.next || raw.status || raw.actual)
        panic("X9 USB: accepted EP0 native request mutated");
    void *request = e.request;
    uint64_t count = actual;
    memcpy(static_cast<uint8_t *>(request) + 0x28, &status, sizeof(status));
    memcpy(static_cast<uint8_t *>(request) + 0x48, &count, sizeof(count));
    familyComplete(controller, request); // actual callback + original release/free
    // Original request may be freed. Access only extra current-layout retain.
    memory->release(); e = {}; return true;
}
bool X9USBNativeControlAdapter::event(void *controller, const x9_usb_ep0_event &event)
{
    auto &a = instance;
    if (a.check(controller) != kIOReturnSuccess) return false; // dormant/unbound
    return a.process(event);
}
bool X9USBNativeControlAdapter::process(const x9_usb_ep0_event &event)
{
    if (event.kind == X9_EP0_SETUP) {
        if (failure != kIOReturnSuccess) {
            ticket = event.ticket; // only cancellation target; old requests stay quarantined
            (void)X9USBControlBridge::cancel(event.ticket, EIO); return true;
        }
        for (auto &e : entries) if (e.phase != Phase::Free)
            panic("X9 USB: new EP0 setup before original request callbacks returned");
        ticket = event.ticket;
        wireLength = event.setup[6] | ((uint32_t)event.setup[7] << 8);
        outProgress = 0;
        usbIn = (event.setup[0] & 0x80) != 0; needsZLP = false;
        stage = usbIn && wireLength ? Stage::WaitData : wireLength ? Stage::WaitOut : Stage::WaitStatus;
        return false; // qualified loader still delivers actual setup to Family
    }
    if (!ticket || event.ticket != ticket) panic("X9 USB: EP0 adapter ticket changed");
    if (event.kind == X9_EP0_OUT_DATA) {
        if (usbIn || stage != Stage::WaitOut) panic("X9 USB: EP0 OUT event state");
        stage = Stage::WaitStatus; return false; // Family inputDataPacket can queue IN status
    }
    if (event.kind == X9_EP0_OUT_PACKET) {
        if (usbIn || stage != Stage::WaitOut || !event.actual_qualified || event.error ||
            event.status_complete || event.packet_offset != outProgress || event.packet_length > 64 ||
            event.packet_length > wireLength - outProgress ||
            event.actual != outProgress + event.packet_length ||
            event.packet_final != (event.packet_length < 64 || event.actual == wireLength))
            panic("X9 USB: EP0 OUT packet geometry/progress");
        outProgress = event.actual;
        if (event.packet_final) stage = Stage::WaitStatus;
        return false; // original Family sees each actual packet, not aggregate
    }
    if (event.kind == X9_EP0_IN_DATA || event.kind == X9_EP0_IN_ZLP) {
        unsigned index = event.kind == X9_EP0_IN_DATA ? 0 : 1;
        auto &e = entries[index];
        if (!event.actual_qualified || event.error || event.status_complete ||
            e.phase != Phase::Accepted || e.kind != (index ? Kind::ZLP : Kind::Data) ||
            (!index && event.actual != e.requested))
            panic("X9 USB: unqualified EP0 stage event");
        stage = Stage::DataRetired;
        (void)complete(e, kIOReturnSuccess, index ? 0 : event.actual);
        return true; // never call the setup/status handler a second time
    }
    if (event.kind != X9_EP0_COMPLETE) panic("X9 USB: unknown EP0 event");
    IOReturn status = event.error == ECANCELED ? kIOReturnAborted : usb_return(event.error);
    if (!event.error && (!event.status_complete || !event.actual_qualified))
        panic("X9 USB: EP0 terminal success without status evidence");
    for (auto &e : entries) {
        if (e.phase == Phase::Free || e.phase == Phase::Quarantined) continue;
        if (!event.actual_qualified) { quarantine(e, kIOReturnIOError); continue; }
        if (event.error && e.kind == Kind::ZLP) {
            // Stock zlpCompletion ignores its error argument and announces
            // state8 unconditionally. Never feed it a failed/unsent ZLP and
            // transiently report data-stage success. Retain the obligation;
            // the qualified control glue still receives the terminal error.
            quarantine(e, status); continue;
        }
        if (!event.error && e.kind != Kind::Status)
            panic("X9 USB: status passed an undelivered EP0 data/ZLP callback");
        uint32_t actual = e.kind == Kind::Data ? event.actual : 0;
        (void)complete(e, status, actual);
    }
    if (failure == kIOReturnSuccess) { stage = Stage::Idle; ticket = 0; }
    return false; // IN status OUT notification or terminal error belongs to native control glue
}
