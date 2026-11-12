#include "X9USBNativeRequestAdapter.h"
#include "X9USBNativeControlAdapter.h"
#include <stddef.h>
#include <kern/locks.h>
#include <sys/systm.h>

/* Field widths come from the original slot223 producer and completion
 * consumer, not a current SDK struct or a fabricated OSObject layout.
 * Unused words are opaque. In particular request.next@20 is not actual@48.
 */
struct X9USBStockRequest20H392 {
    uint32_t endpoint, opaque04;
    uint64_t memory, dma, opaque18, next;
    IOReturn status;
    uint32_t timeoutMS;
    uint8_t widePayload, reusable, opaque32[14];
    uint64_t requested, actual, target, action, parameter;
};
static_assert(sizeof(X9USBStockRequest20H392) == 0x68, "stock plain request size");
static_assert(offsetof(X9USBStockRequest20H392, memory) == 8, "stock retained memory");
static_assert(offsetof(X9USBStockRequest20H392, dma) == 0x10, "stock DMA command");
static_assert(offsetof(X9USBStockRequest20H392, next) == 0x20, "stock request link");
static_assert(offsetof(X9USBStockRequest20H392, status) == 0x28, "stock status32");
static_assert(offsetof(X9USBStockRequest20H392, timeoutMS) == 0x2c, "stock timeout32");
static_assert(offsetof(X9USBStockRequest20H392, widePayload) == 0x30, "stock union selector");
static_assert(offsetof(X9USBStockRequest20H392, reusable) == 0x31, "stock cleanup selector");
static_assert(offsetof(X9USBStockRequest20H392, requested) == 0x40, "stock requested64");
static_assert(offsetof(X9USBStockRequest20H392, actual) == 0x48, "stock actual64");
static_assert(offsetof(X9USBStockRequest20H392, target) == 0x50, "stock completion target");
static_assert(offsetof(X9USBStockRequest20H392, action) == 0x58, "stock completion action");
static_assert(offsetof(X9USBStockRequest20H392, parameter) == 0x60, "stock completion parameter");

X9USBNativeRequestAdapter X9USBNativeRequestAdapter::instance;
static LCK_GRP_DECLARE(x9_usb_adapter_group, "x9_usb_adapter");
static LCK_MTX_DECLARE(x9_usb_adapter_binding_lock, &x9_usb_adapter_group);
bool X9USBNativeRequestAdapter::boundFor(IOWorkLoop *loop, void *controller, FamilyComplete complete)
{
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    const auto &a = instance;
    bool result = loop && controller && complete && a.loop == loop &&
        a.controller == controller && a.familyComplete == complete;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock); return result;
}
IOReturn X9USBNativeRequestAdapter::bind(IOWorkLoop *loop, void *controller,
                                        FamilyComplete complete)
{
    if (!loop || !controller || !complete) return kIOReturnBadArgument;
    if (!loop->inGate()) return kIOReturnNotPermitted;
    if (!X9USBRequestBridge::installedOn(loop)) return kIOReturnNotReady;
    auto &a = instance;
    loop->retain();
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    if (a.loop) {
        lck_mtx_unlock(&x9_usb_adapter_binding_lock);
        loop->release(); return kIOReturnExclusiveAccess;
    }
    a.controller = controller; a.familyComplete = complete; a.loop = loop;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock);
    return kIOReturnSuccess;
}
IOReturn X9USBNativeRequestAdapter::check(void *nativeController) const
{
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    IOWorkLoop *workloop = loop;
    void *owner = controller;
    bool bound = workloop && familyComplete;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock);
    if (!bound) return kIOReturnNotReady;
    if (nativeController != owner) return kIOReturnBadArgument;
    if (!workloop->inGate()) return kIOReturnNotPermitted;
    return X9USBRequestBridge::installedOn(workloop) ? kIOReturnSuccess : kIOReturnNotReady;
}
IOReturn X9USBNativeRequestAdapter::submit(void *controller, void *request,
                                          IOMemoryDescriptor *memory)
{
    auto &a = instance;
    IOReturn result = a.check(controller);
    return result == kIOReturnSuccess ? a.enqueue(request, memory) : result;
}
IOReturn X9USBNativeRequestAdapter::enqueue(void *request, IOMemoryDescriptor *memory)
{
    if (!request || !memory) return kIOReturnBadArgument;
    X9USBStockRequest20H392 routing;
    memcpy(&routing, request, sizeof(routing));
    if (routing.endpoint == 0x80)
        return X9USBNativeControlAdapter::submit(controller, request, memory);
    Entry *room = nullptr;
    for (auto &e : entries) {
        if (e.phase == Phase::Free) { if (!room) room = &e; }
        else if (e.request == request || e.memory == memory) return kIOReturnExclusiveAccess;
    }
    if (!room) return kIOReturnNoResources;
    X9USBStockRequest20H392 raw;
    memcpy(&raw, request, sizeof(raw)); // qualified plain allocation, never OSObject
    if (raw.widePayload || raw.reusable) return kIOReturnUnsupported;
    if (raw.endpoint != 0x01 && raw.endpoint != 0x81 &&
        raw.endpoint != 0x03 && raw.endpoint != 0x83) return kIOReturnUnsupported;
    if (raw.memory != reinterpret_cast<uintptr_t>(memory) || !raw.action ||
        raw.requested > UINT32_MAX) return kIOReturnBadArgument;
    // Replace ioGated before a legacy DMA command/queue acquires the request.
    // Reject stale/partially submitted input rather than silently resetting it.
    if (raw.dma || raw.next || raw.actual || raw.status != kIOReturnSuccess)
        return kIOReturnNotReady;
    *room = {};
    room->phase = Phase::Submitting; room->request = request; room->memory = memory;
    room->endpoint = raw.endpoint; room->requested = raw.requested;
    room->target = raw.target; room->action = raw.action; room->parameter = raw.parameter;
    X9USBRequestOwner::Input input = { request, memory, raw.requested, raw.timeoutMS,
        (raw.endpoint & 0x80) != 0, false, false, room, completed };
    IOReturn result = X9USBRequestBridge::submit(raw.endpoint, input, &room->ticket);
    if (result == kIOReturnSuccess) room->phase = Phase::Accepted;
    else *room = {}; // no native writes/release/free/callback on rejection
    return result; // same gate excludes real completion until the action returns
}
void X9USBNativeRequestAdapter::completed(void *context, void *request,
                                          IOReturn status, uint32_t actual)
{
    auto &a = instance;
    auto &e = *static_cast<Entry *>(context);
    if (!a.loop || !a.loop->inGate() || e.phase != Phase::Accepted ||
        e.request != request || actual > e.requested)
        panic("X9 USB: invalid stock request completion ownership");
    X9USBStockRequest20H392 raw;
    memcpy(&raw, request, sizeof(raw));
    if (raw.memory != reinterpret_cast<uintptr_t>(e.memory) ||
        raw.endpoint != e.endpoint || raw.requested != e.requested ||
        raw.widePayload || raw.reusable || raw.dma || raw.next || raw.actual ||
        raw.status != kIOReturnSuccess || raw.target != e.target ||
        raw.action != e.action || raw.parameter != e.parameter)
        panic("X9 USB: accepted stock request mutated before Family completion");
    e.phase = Phase::Delivering;
    uint64_t count = actual; // actual wire bytes, NOT residual or copied bytes
    memcpy(static_cast<uint8_t *>(request) + 0x28, &status, sizeof(status));
    memcpy(static_cast<uint8_t *>(request) + 0x48, &count, sizeof(count));
    FamilyComplete complete = a.familyComplete;
    void *controller = a.controller;
    // Native Family owns callback target/action/parameter and ORIGINAL
    // descriptor release/free. Bridge holds its independent descriptor retain.
    // NEVER dereference request, memory or raw callback target after this call.
    complete(controller, request);
    e = {}; // only our boot-static record; request may already have been freed
}
IOReturn X9USBNativeRequestAdapter::abort(void *controller, void *request,
    X9USBRequestBridge::AbortResult *result)
{
    if (!result) return kIOReturnBadArgument;
    *result = {};
    auto &a = instance;
    IOReturn status = a.check(controller);
    if (status != kIOReturnSuccess) return status;
    if (!request) return kIOReturnBadArgument;
    for (const auto &e : a.entries) {
        if (e.phase == Phase::Free || e.request != request) continue;
        if (e.phase != Phase::Accepted) return kIOReturnBusy;
        return X9USBRequestBridge::abort(e.ticket, result);
    }
    return kIOReturnNotFound; // no raw read or guessed request ticket
}
IOReturn X9USBNativeRequestAdapter::rearm(void *controller)
{
    auto &a = instance;
    IOReturn status = a.check(controller);
    if (status != kIOReturnSuccess) return status;
    for (const auto &e : a.entries) if (e.phase != Phase::Free) return kIOReturnBusy;
    return X9USBRequestBridge::rearm();
}
