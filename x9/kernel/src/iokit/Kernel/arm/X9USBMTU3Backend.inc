#include "X9USBMTU3Backend.h"

X9USBMTU3Backend X9USBMTU3Backend::instance;
X9USBRequestBridge::Backend X9USBMTU3Backend::operations()
{ return { &instance, validate, poll }; }
IOReturn X9USBMTU3Backend::validate(void *, uint32_t endpoint,
                                  const X9USBRequestOwner::Input &input)
{
    /* No qualified EP0/zero-packet/BMU or iso/reusable contract yet. Refuse
     * before prepare/accept, rather than accepting a request we cannot send.
     * The future Family adapter must separately qualify native request flags;
     * this contract implements ordinary bulk with no implicit appended ZLP. */
    if (!input.requested || input.isochronous || input.reusable)
        return kIOReturnUnsupported;
    return usb_return(x9_usb_native_qmu_validate(endpoint, input.usbIn));
}
int X9USBMTU3Backend::commit(void *parameter)
{
    auto &c = *static_cast<Commit *>(parameter);
    c.result = c.owner->publishChunk(c.chunk, c.nowNS);
    return c.result == kIOReturnSuccess ? 0 : ECANCELED;
}
void X9USBMTU3Backend::poll(void *parameter, X9USBRequestBridge &bridge,
                          X9USBRequestOwner &owner, bool available, uint64_t nowNS)
{ static_cast<X9USBMTU3Backend *>(parameter)->run(bridge, owner, available, nowNS); }
void X9USBMTU3Backend::run(X9USBRequestBridge &bridge, X9USBRequestOwner &owner,
                         bool available, uint64_t nowNS)
{
    /* Exchange owns descriptor/context through Family completion. Scheduler
     * copies only opaque ticket/counts, never raw native fields or vslots. */
    for (unsigned n = 0; n < X9USBRequestOwner::Capacity; n++) {
        Entry *room = nullptr;
        for (auto &e : entries) if (!e.occupied) { room = &e; break; }
        if (!room) break;
        uint32_t endpoint = 0; X9USBRequestOwner::Ticket t = {};
        X9USBRequestOwner::Input input = {};
        IOReturn result = bridge.workerNext(&endpoint, &t, &input);
        if (result == kIOReturnNotFound) break;
        if (result != kIOReturnSuccess) {
            (void)owner.close(kIOReturnOffline); x9_usb_native_qmu_isolate();
            available = false; break;
        }
        *room = {}; room->occupied = true; room->endpoint = endpoint;
        room->ticket = t; room->requested = static_cast<uint32_t>(input.requested);
    }
    if (!available) (void)owner.close(kIOReturnOffline);
    /* One request at a time per endpoint, FIFO by non-reused owner serial.
     * One chunk per endpoint per poll, bounded by exchange capacity. */
    for (auto &e : entries) {
        if (!e.occupied) continue;
        X9USBRequestOwner::State state; IOReturn reason;
        IOReturn result = owner.inspect(e.ticket, &state, &reason);
        if (result != kIOReturnSuccess) {
            x9_usb_native_qmu_isolate(); (void)owner.close(kIOReturnOffline);
            available = false; continue; // retain physical record on disagreement
        }
        if (state == X9USBRequestOwner::State::Quarantined) continue;
        if (state == X9USBRequestOwner::State::Terminal && !e.active) { e = {}; continue; }
        bool earlier = false;
        for (const auto &other : entries)
            if (other.occupied && other.endpoint == e.endpoint &&
                other.ticket.serial < e.ticket.serial) earlier = true;
        if (earlier) continue;
        if (e.active) {
            x9_usb_qmu_evidence evidence = {};
            int error = x9_usb_native_qmu_probe(e.physical,
                !available || state == X9USBRequestOwner::State::Stopping, &evidence);
            if (error == EAGAIN) continue;
            if (error) {
                x9_usb_native_qmu_isolate(); (void)owner.close(kIOReturnOffline);
                available = false; continue; // no manufactured retirement on stale token
            }
            X9USBRequestOwner::Retirement r = {
                evidence.stopped != 0, evidence.hwo_clear != 0,
                evidence.cpr_at_terminator != 0, evidence.actual_qualified != 0,
                evidence.actual, usb_return(evidence.error)
            };
            result = owner.retire(e.chunk, r, evidence.out_bytes);
            bool quarantine = result != kIOReturnSuccess;
            if (x9_usb_native_qmu_finish(e.physical, quarantine) || quarantine) {
                x9_usb_native_qmu_isolate(); (void)owner.close(kIOReturnOffline);
                available = false;
                continue; // both owner and physical record pinned through reboot
            }
            e.active = false; e.progressed += evidence.actual;
            result = owner.inspect(e.ticket, &state, &reason);
            if (result == kIOReturnSuccess && state == X9USBRequestOwner::State::Terminal) e = {};
            continue; // never append another chunk in this retirement turn
        }
        if (!available || state != X9USBRequestOwner::State::Ready) continue;
        void *bounce = nullptr;
        int error = x9_usb_native_qmu_buffer(e.endpoint, &bounce);
        if (error == EBUSY) continue;
        if (error) { (void)owner.cancel(e.ticket, usb_return(error)); continue; }
        uint32_t length = e.requested - e.progressed;
        if (length > 4096) length = 4096;
        result = owner.beginChunk(e.ticket, bounce, length, nowNS, &e.chunk);
        if (result != kIOReturnSuccess) {
            (void)owner.cancel(e.ticket, result); continue;
        }
        error = x9_usb_native_qmu_stage(e.endpoint, length, &e.physical);
        if (error) {
            (void)owner.rejectUnpublished(e.chunk, usb_return(error));
            if (error == ETIMEDOUT) {
                x9_usb_native_qmu_isolate(); (void)owner.close(kIOReturnOffline); available = false;
            }
            continue;
        }
        Commit c = { &owner, e.chunk, nowNS, kIOReturnSuccess };
        error = x9_usb_native_qmu_publish(e.physical, commit, &c);
        if (error) {
            /* publish guarantees nonzero is pre-HWO. publishChunk may itself
             * have already terminalized timeout; never mask its real error. */
            if (x9_usb_native_qmu_discard(e.physical)) {
                x9_usb_native_qmu_isolate(); (void)owner.close(kIOReturnOffline); available = false;
            }
            (void)owner.rejectUnpublished(e.chunk,
                c.result != kIOReturnSuccess ? c.result : usb_return(error));
            continue;
        }
        e.active = true;
    }
    /* A failure may be discovered after an earlier endpoint was visited.
     * Close and STOP those earlier endpoints too, before returning to C.
     * No scheduling or callbacks in this bounded second pass. */
    if (!available) {
        (void)owner.close(kIOReturnOffline);
        for (auto &e : entries) {
            if (!e.occupied || !e.active) continue;
            X9USBRequestOwner::State state; IOReturn reason;
            if (owner.inspect(e.ticket, &state, &reason) != kIOReturnSuccess ||
                state == X9USBRequestOwner::State::Quarantined) continue;
            x9_usb_qmu_evidence evidence = {};
            if (x9_usb_native_qmu_probe(e.physical, 1, &evidence)) continue;
            X9USBRequestOwner::Retirement r = {
                evidence.stopped != 0, evidence.hwo_clear != 0,
                evidence.cpr_at_terminator != 0, evidence.actual_qualified != 0,
                evidence.actual, usb_return(evidence.error)
            };
            bool quarantine = owner.retire(e.chunk, r, evidence.out_bytes) != kIOReturnSuccess;
            if (x9_usb_native_qmu_finish(e.physical, quarantine) || quarantine) {
                x9_usb_native_qmu_isolate(); continue;
            }
            e = {}; // stopped terminal; native completion remains on original gate
        }
    }
}
