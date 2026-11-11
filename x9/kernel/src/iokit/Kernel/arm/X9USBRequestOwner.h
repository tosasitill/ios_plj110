/* MTU3 native-request ownership component; NOT a stock Apple class/layout.
 * One instance must live for the sole USB worker's entire lifetime. No MMIO,
 * Apple vtable calls, request allocation/free or automatic DMA retry here.
 */
#ifndef X9_USB_REQUEST_OWNER_H
#define X9_USB_REQUEST_OWNER_H
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>

class X9USBRequestOwner final {
public:
    static constexpr unsigned Capacity = 16;
    struct Ticket { uint64_t generation, serial; };
    struct Chunk { Ticket request; uint64_t serial; uint32_t length; };
    enum class State : uint8_t { Free, Ready, Staged, Active, Stopping, Terminal,
                                Delivering, Quarantined };
    /* Ordinary requests only. The eventual ABI-qualified Family adapter owns
     * nativeRequest and calls Family completion (including release/free).
     * This owner holds an EXTRA descriptor retain through that callback.
     * Never cast nativeRequest to OSObject. Iso payloads are frame pointers,
     * not byte counts; flag31/reusable requests are not supported here.
     */
    using Completion = void (*)(void *context, void *nativeRequest,
                               IOReturn status, uint32_t actual);
    struct Input {
        void *nativeRequest;
        IOMemoryDescriptor *memory;
        uint64_t requested;
        uint32_t timeoutMS; // zero disables; do not guess stock's 5000ms branch
        bool usbIn;        // USB IN reads memory; USB OUT writes memory
        bool isochronous, reusable; // adapter must propagate native flags30/31
        void *context;
        Completion completion;
    };
    /* Worker-sampled evidence, not a manufactured acknowledgement. Normal
     * retirement requires HWO clear + CPR at terminator + STOP inactive.
     * Abort requires STOP inactive AND a separately qualified actual count;
     * STOP alone never establishes that zero bytes escaped.
     */
    struct Retirement {
        bool stopped, hwoClear, cprAtTerminator, actualQualified;
        uint32_t actual;
        IOReturn status;
    };

    X9USBRequestOwner();
    ~X9USBRequestOwner(); // live/quarantined ownership is a fatal lifetime bug
    IOReturn bindWorker();
    /* Synchronous prepare before acceptance. Nonzero: caller still owns its
     * native request, no completion will follow. Current-layout memory objects
     * only; an unadapted stock descriptor vptr must NEVER reach this API.
     * Success: completion obligation
     * transfers here, but callbacks only run in explicit drainCompletions().
     * Caller must finish returning gated-submit success before that drain.
     */
    IOReturn accept(const Input &, Ticket *);
    /* Prepare one bounce chunk BEFORE QMU publication. Caller keeps its pinned
     * bounce buffer alive through retirement/quarantine; no physical-address
     * mapping from the native descriptor is assumed. 0-byte requests still
     * need a real EP0/QMU ZLP retirement, never synthetic success.
     * nowNS must be the worker's monotonic clock, not user/host wall time.
     */
    IOReturn beginChunk(Ticket, void *bounce, uint32_t length,
                        uint64_t nowNS, Chunk *);
    /* Worker linearization point immediately BEFORE HWO publication. Nothing
     * may call this owner between success and that MMIO/cache publication.
     * Once marked published, rejectUnpublished is prohibited; any failure
     * needs STOP plus retirement evidence instead. Deadline starts here.
     */
    IOReturn publishChunk(Chunk, uint64_t nowNS);
    /* After beginChunk but before publication: a definitively unsubmitted
     * chunk can be cancelled with known zero delta (earlier actual survives).
     */
    IOReturn rejectUnpublished(Chunk, IOReturn);
    IOReturn retire(Chunk, const Retirement &, const void *outBounce);
    IOReturn cancel(Ticket, IOReturn reason);
    IOReturn expire(uint64_t nowNS); // marks Active -> Stopping, never callbacks
    IOReturn inspect(Ticket, State *, IOReturn *stopReason) const;
    /* Close the admission gate first. Ready/Staged -> Terminal, Active -> Stopping;
     * the worker must physically STOP/drain each Stopping chunk via retire().
     * Old generation tickets remain usable for retirement, not new admission.
     */
    IOReturn close(IOReturn reason);
    IOReturn reopen(); // only with no slots, including callbacks/quarantine
    IOReturn drainCompletions(); // bounded batch; returns first complete failure
    bool empty() const;

private:
    struct Slot {
        State state;
        Ticket ticket;
        Input input;
        uint64_t chunkSerial, deadlineNS;
        uint32_t chunkLength, actual;
        IOReturn status;
        bool prepared, started;
    };
    Slot slots[Capacity] = {};
    thread_t worker = THREAD_NULL;
    uint64_t generation = 1, nextSerial = 0;
    bool closed = true, inOperation = false, draining = false;
    X9USBRequestOwner(const X9USBRequestOwner &) = delete;
    X9USBRequestOwner &operator=(const X9USBRequestOwner &) = delete;
    bool onWorker() const;
    Slot *find(Ticket);
    const Slot *find(Ticket) const;
    Slot *find(Chunk);
    void terminal(Slot &, IOReturn);
    void quarantine(Slot &, IOReturn);
    IOReturn cancelSlot(Slot &, IOReturn);
};
#endif
