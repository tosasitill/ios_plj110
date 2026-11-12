#include "X9USBNativeControlGlue.h"
// One boot-pinned binding, published with the ordinary adapter lock. No new
// startup lock macros in this shared .inc translation unit.
X9USBNativeControlGlue X9USBNativeControlGlue::instance;
bool X9USBNativeControlGlue::installing;
IOReturn X9USBNativeControlGlue::install(IOWorkLoop *loop, void *controller, void *control,
    X9USBNativeControlAdapter::FamilyComplete complete, const Operations &ops)
{
    if (!loop || !controller || !control || !complete || !ops.inspect || !ops.inputSetup ||
        !ops.processSetup || !ops.inputData || !ops.reset) return kIOReturnBadArgument;
    if (!loop->inGate()) return kIOReturnNotPermitted;
    if (!X9USBNativeRequestAdapter::boundFor(loop, controller, complete)) return kIOReturnNotReady;
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    if (instance.loop || installing) {
        lck_mtx_unlock(&x9_usb_adapter_binding_lock); return kIOReturnExclusiveAccess;
    }
    installing = true;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock);
    Snapshot s = {};
    bool qualified = ops.inspect(control, &s) && s.controller == controller && s.state == 1 &&
        s.inEndpoint == 0x80 && !s.outEndpoint && !(s.flags & 1) && s.familySpeed <= 2 &&
        s.packetSize == 64 && !s.requested && !s.progress && !s.hasDataMemory;
    IOReturn status = qualified ? X9USBControlBridge::install(loop, controller, handle, ready) : kIOReturnUnsupported;
    bool installed = status == kIOReturnSuccess;
    if (status == kIOReturnSuccess)
        status = X9USBNativeControlAdapter::bind(loop, controller, complete, s.packetSize, s.flags);
    if (installed) loop->retain();
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    // Once the permanent bridge is installed it cannot be rolled back. A bind
    // failure publishes a faulted, inert handler, never a partially live glue.
    if (installed) {
        instance.loop = loop; instance.controller = controller;
        instance.control = control; instance.ops = ops;
        if (status != kIOReturnSuccess) { instance.failure = status; instance.phase = Phase::Fault; }
    }
    installing = false;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock);
    return status;
}
IOReturn X9USBNativeControlGlue::check(void *owner) const
{
    lck_mtx_lock(&x9_usb_adapter_binding_lock);
    auto *gate = loop; auto *bound = controller;
    lck_mtx_unlock(&x9_usb_adapter_binding_lock);
    if (!gate) return kIOReturnNotReady;
    if (owner != bound) return kIOReturnBadArgument;
    if (!gate->inGate()) return kIOReturnNotPermitted;
    return kIOReturnSuccess;
}
bool X9USBNativeControlGlue::inspect(Snapshot &s) const
{
    s = {};
    return ops.inspect(control, &s) && s.controller == controller && s.state >= 1 && s.state <= 9 &&
        s.inEndpoint == 0x80 && !s.outEndpoint && !(s.flags & 1) && s.familySpeed <= 2 &&
        s.packetSize == 64 && s.progress <= s.requested && s.requested <= X9_USB_EP0_CAPACITY;
}
void X9USBNativeControlGlue::fail(IOReturn reason)
{
    if (reason == kIOReturnSuccess) reason = kIOReturnIOError;
    if (failure == kIOReturnSuccess) failure = reason;
    phase = Phase::Fault;
    // No reset/free of a control object whose layout or pending request count
    // is uncertain. Original descriptor/targets stay boot-pinned; block rearm.
    (void)X9USBNativeControlAdapter::isolate(controller, reason);
    (void)X9USBControlBridge::cancel(ticket, EIO);
}
void X9USBNativeControlGlue::command(unsigned kind)
{
    IOReturn result = X9USBControlBridge::reply(ticket, kind, nullptr, 0);
    // A reset/new SETUP can race a gate callback. ESTALE means await that
    // ticket's real COMPLETE, not a reason to retry or declare ABI failure.
    if (result != kIOReturnSuccess && result != kIOReturnAborted) fail(result);
}
IOReturn X9USBNativeControlGlue::ready(void *controller)
{
    auto &g = instance;
    IOReturn status = g.check(controller);
    if (status != kIOReturnSuccess) return status;
    if (g.failure != kIOReturnSuccess) return g.failure;
    if (g.phase != Phase::Idle || g.ticket) return kIOReturnBusy;
    Snapshot s;
    if (!g.inspect(s) || s.state != 1 || s.requested || s.progress || s.hasDataMemory) {
        g.fail(kIOReturnIOError); return g.failure;
    }
    return kIOReturnSuccess;
}
void X9USBNativeControlGlue::observe(void *controller, const x9_usb_ep0_event &event)
{
    auto &g = instance;
    if (g.check(controller) != kIOReturnSuccess) return; // generic bridge baseline
    g.terminalError = event.kind == X9_EP0_COMPLETE && event.error != 0;
}
IOReturn X9USBNativeControlGlue::requestStall(void *controller, uint32_t endpoint)
{
    auto &g = instance;
    IOReturn status = g.check(controller);
    if (status != kIOReturnSuccess) return status;
    if (g.failure != kIOReturnSuccess) return g.failure;
    if (endpoint != 0 && endpoint != 0x80) return kIOReturnUnsupported;
    if (!g.ticket || g.phase == Phase::Idle) return kIOReturnNotReady;
    if (!g.stalled) {
        g.stalled = true;
        if (!g.terminalError) g.command(X9_EP0_STALL);
    }
    return g.failure;
}
IOReturn X9USBNativeControlGlue::stateChanged(void *controller, uint32_t previous, uint32_t current)
{
    auto &g = instance;
    IOReturn status = g.check(controller);
    if (status != kIOReturnSuccess) return status;
    if (g.failure != kIOReturnSuccess) return g.failure;
    Snapshot s;
    if (!g.inspect(s) || s.state != current || previous < 1 || previous > 9) {
        g.fail(kIOReturnBadArgument); return g.failure;
    }
    if (g.phase == Phase::Idle) {
        if (current != 1) { g.fail(kIOReturnNotReady); return g.failure; }
        return kIOReturnSuccess;
    }
    if (current == 9) return requestStall(controller, 0x80);
    if ((current == 5 || current == 6) && !g.usbIn && g.wireLength) {
        if (!s.hasDataMemory || s.requested != g.wireLength || s.progress != g.outProgress) {
            g.fail(kIOReturnUnsupported); return g.failure;
        }
        if (!g.receiving) {
            g.receiving = true;
            g.command(X9_EP0_RECEIVE_OUT_PACKETS);
        }
    }
    return g.failure;
}
IOReturn X9USBNativeControlGlue::acknowledgePacket(void *controller, const void *record,
    uint32_t eventCode, IOReturn status)
{
    auto &g = instance;
    IOReturn result = g.check(controller);
    if (result != kIOReturnSuccess) return result;
    if (g.failure != kIOReturnSuccess) return g.failure;
    if (!record || (eventCode != 11 && eventCode != 12)) return kIOReturnUnsupported;
    X9USBStockRequest20H392 raw;
    memcpy(&raw, record, sizeof(raw)); // separately qualified ephemeral stack geometry
    bool setupEvent = g.phase == Phase::Setup && eventCode == 12;
    bool dataEvent = (g.phase == Phase::Packet || g.phase == Phase::Status) && eventCode == 11;
    if ((!setupEvent && !dataEvent) || g.acknowledged || status != kIOReturnSuccess ||
        raw.endpoint || raw.opaque04 || raw.memory || raw.dma || raw.next || raw.status || raw.timeoutMS ||
        raw.widePayload || raw.reusable || raw.target || raw.action || raw.parameter ||
        raw.opaque18 != reinterpret_cast<uintptr_t>(g.activePacket) ||
        raw.requested != 64 || raw.actual != g.activeBytes) {
        g.fail(kIOReturnBadArgument); return g.failure;
    }
    g.acknowledged = true;
    return kIOReturnSuccess; // event reporting only, no heap ownership or MMIO
}
void X9USBNativeControlGlue::handle(void *controller, const x9_usb_ep0_event &event, const uint8_t *out)
{
    auto &g = instance;
    if (g.check(controller) != kIOReturnSuccess) panic("X9 USB: control glue outside bound gate");
    g.process(event, out);
}
void X9USBNativeControlGlue::process(const x9_usb_ep0_event &event, const uint8_t *out)
{
    if (failure != kIOReturnSuccess) {
        // Adapter may consume subsequent SETUP itself after quarantine. Never
        // dereference or reset the old native control on those new terminals.
        (void)X9USBControlBridge::cancel(event.ticket, EIO); return;
    }
    Snapshot s;
    if (event.kind == X9_EP0_SETUP) {
        if (phase != Phase::Idle || ticket || !inspect(s) || s.state != 1 ||
            !s.familySpeed || s.progress || s.requested || s.hasDataMemory) {
            ticket = event.ticket; fail(kIOReturnNotReady); return;
        }
        ticket = event.ticket; memcpy(setup, event.setup, sizeof(setup));
        wireLength = setup[6] | ((uint32_t)setup[7] << 8);
        usbIn = (setup[0] & 0x80) != 0; receiving = false; stalled = false; outProgress = 0;
        phase = Phase::Running;
        if (usbIn && !wireLength) { command(X9_EP0_STALL); return; }
        phase = Phase::Setup; activePacket = setup; activeBytes = 8; acknowledged = false;
        ops.inputSetup(control, setup);
        activePacket = nullptr; activeBytes = 0;
        if (failure != kIOReturnSuccess) return;
        if (!acknowledged || !inspect(s) || s.state != 2 || memcmp(s.setup, setup, sizeof(setup))) {
            fail(kIOReturnIOError); return;
        }
        phase = Phase::Running;
        IOReturn result = ops.processSetup(control); // actual original setup dispatch
        if (failure == kIOReturnSuccess && result != kIOReturnSuccess) command(X9_EP0_STALL);
        return;
    }
    if (!ticket || event.ticket != ticket) { fail(kIOReturnAborted); return; }
    if (event.kind == X9_EP0_OUT_PACKET) {
        if (stalled) return; // preserve/ack the copied packet, never feed state9 to native data
        if (phase != Phase::Running || usbIn || !receiving || !out || !inspect(s) ||
            (s.state != 5 && s.state != 6) || !s.hasDataMemory || s.requested != wireLength ||
            s.progress != event.packet_offset || event.packet_offset != outProgress ||
            !event.actual_qualified || event.error || event.packet_length > 64 ||
            event.packet_length > wireLength - outProgress || event.actual != outProgress + event.packet_length) {
            fail(kIOReturnIOError); return;
        }
        // Advance our expected progress before original state notification;
        // the native method advances control+20 before announcing state6/7.
        outProgress = event.actual;
        phase = Phase::Packet; activePacket = out; activeBytes = event.packet_length; acknowledged = false;
        ops.inputData(control, out, event.packet_length);
        activePacket = nullptr; activeBytes = 0;
        if (failure != kIOReturnSuccess) return;
        if (stalled) { phase = Phase::Running; return; } // legitimate native callback rejection
        if (!acknowledged || !inspect(s) || s.progress != outProgress ||
            (event.packet_final ? s.state != 7 : (s.state != 5 && s.state != 6))) {
            fail(kIOReturnIOError); return;
        }
        phase = Phase::Running;
        if (!event.packet_final) command(X9_EP0_CONTINUE_OUT);
        // Final status is submitted by the ORIGINAL control method/user
        // callback via the ordinary adapter. Never manufacture another ZLP.
        return;
    }
    if (event.kind != X9_EP0_COMPLETE) { fail(kIOReturnUnsupported); return; }
    IOReturn settled = X9USBNativeControlAdapter::rearmReady(controller);
    if (settled != kIOReturnSuccess || !event.actual_qualified) {
        fail(settled == kIOReturnSuccess ? kIOReturnIOError : settled); return;
    }
    if (!inspect(s)) { fail(kIOReturnIOError); return; }
    if (event.error) {
        // Only after adapter callbacks/descriptor completes have settled. The
        // stock reset reports abort with original confirmed progress; never
        // invoke it over a quarantined request with unqualified actual.
        if (s.state != 1) {
            phase = Phase::Reset; ops.reset(control);
            if (failure != kIOReturnSuccess) return;
            if (!inspect(s) || s.state != 1) { fail(kIOReturnIOError); return; }
        }
    } else if (usbIn) {
        if (!event.status_complete || s.state != 8) { fail(kIOReturnIOError); return; }
        phase = Phase::Status; activePacket = nullptr; activeBytes = 0; acknowledged = false;
        ops.inputData(control, nullptr, 0); // hardware-retired status OUT, not data callback
        if (failure != kIOReturnSuccess) return;
        if (!acknowledged || !inspect(s) || s.state != 1) { fail(kIOReturnIOError); return; }
    } else if (!event.status_complete || s.state != 1) {
        fail(kIOReturnIOError); return;
    }
    // OUT/no-data's statusStageWriteCompletion already ran in the adapter;
    // don't call inputDataPacket a second time or duplicate final callbacks.
    ticket = 0; phase = Phase::Idle; receiving = false; outProgress = 0;
}
