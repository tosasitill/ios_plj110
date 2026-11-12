/* MT6993 USB2 EP0 PIO. Included by the sole-MMIO-worker translation unit.
 * Register sequence follows OPPO mtu3_gadget_ep0.c, not Synopsys/DWC3.
 * Unlike Linux giveback-on-DATAEND, our COMPLETE waits for DATAEND to clear.
 * No native controller installer exists yet; native_apple_stack stays zero.
 */
enum native_ep0_phase { NE_IDLE, NE_REPLY, NE_IN, NE_IN_LAST, NE_OUT,
                        NE_OUT_REPLY, NE_STATUS, NE_TERMINAL,
                        NE_IN_DATA_REPLY, NE_IN_ZLP, NE_IN_ZLP_REPLY };
static struct {
    enum native_ep0_phase phase;
    uint64_t serial, sequence, deadline;
    struct x9_usb_ep0_event event;
    unsigned claimed, pending, reply, interrupted;
    unsigned native_requests, needs_zlp, zlp_queued;
    int interruption;
    uint32_t wire_length, in_length, actual, in_flight;
    uint8_t setup[8], bytes[X9_USB_EP0_CAPACITY];
} native_ep0;

static uint32_t ne_csr(void) { return R(EP0) & ~EP0_W1C; }
int x9_usb_native_ep0_rearm_ready(void)
{
    if (current_thread() != usb_worker) return EPERM;
    lck_mtx_lock(&usb_lock);
    int busy = native_ep0.phase != NE_IDLE || native_ep0.pending ||
        native_ep0.claimed || native_ep0.interrupted || native_ep0.in_flight || native_ep0.reply;
    lck_mtx_unlock(&usb_lock);
    // The gate guard excludes native callbacks; this second worker guard
    // excludes a new SETUP observed while the caller waits with its gate held.
    return busy || (R(EP0) & (DATAEND | TX | RX | SETUP | STALL | SENTSTALL | BIT(23))) ? EBUSY : 0;
}
/* usb_lock held. Every notification survives until exact sequence ack. */
static void ne_event(unsigned kind, int error, unsigned qualified, unsigned status)
{
    if (native_ep0.pending || native_ep0.sequence == UINT64_MAX)
        panic("X9 USB: EP0 event ownership/exhaustion");
    native_ep0.event = (struct x9_usb_ep0_event) {
        .ticket = native_ep0.serial, .sequence = ++native_ep0.sequence,
        .kind = kind, .actual = native_ep0.actual,
        .actual_qualified = qualified, .status_complete = status, .error = error
    };
    memcpy(native_ep0.event.setup, native_ep0.setup, 8);
    native_ep0.pending = 1;
}
static void ne_finish(int error, unsigned qualified, unsigned status)
{
    native_ep0.reply = 0;
    native_ep0.phase = NE_TERMINAL;
    ne_event(X9_EP0_COMPLETE, error, qualified, status);
}
int x9_usb_native_ep0_pending(void)
{
    lck_mtx_lock(&usb_lock);
    int result = native_ep0.pending && !native_ep0.claimed;
    lck_mtx_unlock(&usb_lock); return result;
}
int x9_usb_native_ep0_claim(struct x9_usb_ep0_event *event, void *bytes, uint32_t capacity)
{
    if (!event) return EINVAL;
    lck_mtx_lock(&usb_lock);
    int error = 0;
    if (!native_ep0.pending || native_ep0.claimed) error = EAGAIN;
    else if (native_ep0.event.kind == X9_EP0_OUT_DATA &&
        (!bytes || capacity < native_ep0.actual)) error = EMSGSIZE;
    else {
        *event = native_ep0.event;
        if (event->kind == X9_EP0_OUT_DATA)
            memcpy(bytes, native_ep0.bytes, event->actual);
        native_ep0.claimed = 1;
    }
    lck_mtx_unlock(&usb_lock); return error;
}
int x9_usb_native_ep0_ack(uint64_t sequence)
{
    lck_mtx_lock(&usb_lock);
    int error = 0;
    if (!sequence || !native_ep0.pending || !native_ep0.claimed ||
        native_ep0.event.sequence != sequence) error = ESTALE;
    else native_ep0.pending = native_ep0.claimed = 0;
    lck_mtx_unlock(&usb_lock); return error;
}
int x9_usb_native_ep0_reply(uint64_t ticket, unsigned kind, const void *bytes, uint32_t n)
{
    if (!ticket || kind < X9_EP0_SEND_IN || kind > X9_EP0_SEND_IN_ZLP ||
        n > X9_USB_EP0_CAPACITY || (n && !bytes) ||
        (kind != X9_EP0_SEND_IN && kind != X9_EP0_SEND_IN_REQUEST && n)) return EINVAL;
    lck_mtx_lock(&usb_lock);
    int error = 0;
    if (ticket != native_ep0.serial || native_ep0.interrupted || failed ||
        !st.native_apple_stack) error = ESTALE;
    else if (kind == X9_EP0_SEND_IN_ZLP) {
        /* The stock Family queues this second ordinary request immediately
         * after data admission, possibly before SETUP callback returns. It
         * is not a STATUS reply and must not overwrite the first command. */
        if (!native_ep0.native_requests || !native_ep0.needs_zlp || native_ep0.zlp_queued ||
            (native_ep0.phase != NE_REPLY && native_ep0.phase != NE_IN &&
             native_ep0.phase != NE_IN_LAST && native_ep0.phase != NE_IN_DATA_REPLY)) error = EBUSY;
        else native_ep0.zlp_queued = 1;
    }
    else if ((native_ep0.phase != NE_REPLY && native_ep0.phase != NE_OUT_REPLY) ||
        native_ep0.reply || (native_ep0.pending && !native_ep0.claimed)) error = EBUSY;
    else if ((kind == X9_EP0_SEND_IN || kind == X9_EP0_SEND_IN_REQUEST) && (native_ep0.phase != NE_REPLY ||
        !(native_ep0.setup[0] & 0x80) || !native_ep0.wire_length ||
        n > native_ep0.wire_length)) error = EINVAL;
    else if (kind == X9_EP0_RECEIVE_OUT && (native_ep0.phase != NE_REPLY ||
        (native_ep0.setup[0] & 0x80) || !native_ep0.wire_length)) error = EINVAL;
    else if (kind == X9_EP0_STATUS && native_ep0.phase == NE_REPLY &&
        native_ep0.wire_length) error = EINVAL;
    else {
        if (kind == X9_EP0_SEND_IN || kind == X9_EP0_SEND_IN_REQUEST) {
            if (n) memcpy(native_ep0.bytes, bytes, n);
            native_ep0.in_length = n;
            native_ep0.native_requests = kind == X9_EP0_SEND_IN_REQUEST;
            native_ep0.needs_zlp = n && n < native_ep0.wire_length && !(n % 64);
        }
        native_ep0.reply = kind;
    }
    lck_mtx_unlock(&usb_lock); return error;
}
int x9_usb_native_ep0_cancel(uint64_t ticket, int error)
{
    if (!ticket || !error) return EINVAL;
    lck_mtx_lock(&usb_lock);
    int result = 0;
    if (ticket != native_ep0.serial || native_ep0.phase == NE_IDLE ||
        native_ep0.phase == NE_TERMINAL) result = ESTALE;
    else if (!native_ep0.interrupted) {
        native_ep0.interrupted = 1; native_ep0.interruption = error;
    }
    lck_mtx_unlock(&usb_lock); return result;
}
void x9_usb_native_ep0_invalidate(int error)
{
    if (!error || current_thread() != usb_worker)
        panic("X9 USB: invalid EP0 worker cancellation");
    lck_mtx_lock(&usb_lock);
    /* A status success already observed before reset remains authoritative.
     * Otherwise don't overwrite even a claimed SETUP/OUT snapshot. */
    if (native_ep0.phase != NE_IDLE && native_ep0.phase != NE_TERMINAL &&
        !native_ep0.interrupted) {
        native_ep0.interrupted = 1; native_ep0.interruption = error;
    }
    lck_mtx_unlock(&usb_lock);
}
/* usb_lock held, only after previous packet's real TX interrupt. Zero packet
 * terminates a short response whose length is exactly a multiple of 64. */
static void ne_transmit(void)
{
    uint32_t n = native_ep0.in_length - native_ep0.actual;
    if (n > 64) n = 64;
    if (n) x9_mtu3_fifo_write(native_ep0.bytes + native_ep0.actual, n);
    native_ep0.in_flight = n;
    native_ep0.phase = (n < 64 || native_ep0.actual + n == native_ep0.wire_length ||
        (native_ep0.native_requests && native_ep0.actual + n == native_ep0.in_length))
        ? NE_IN_LAST : NE_IN;
    W(EP0, ne_csr() | TX);
    /* actual does NOT advance on a FIFO write/TXPKTRDY publication. */
}
void x9_usb_native_ep0_poll(unsigned available)
{
    if (current_thread() != usb_worker) panic("X9 USB: EP0 outside sole worker");
    if (!available) x9_usb_native_ep0_invalidate(ENXIO);
    lck_mtx_lock(&usb_lock);
    /* Preserve a TX completion latched during suspend for the resume poll. */
    unsigned sample = available && !suspended;
    uint32_t isr = sample ? R(0x80) & 0x10001u : 0;
    uint32_t csr = sample ? R(EP0) : 0;
    if (isr) W(0x80, isr); /* only EP0/SETUPEND W1C, never bulk ISR */
    if (native_ep0.phase != NE_IDLE && native_ep0.phase != NE_TERMINAL &&
        ((isr & BIT(16)) || (csr & SETUP))) {
        /* No-data SETUP remains asserted until our STATUS/STALL response. */
        if ((isr & BIT(16)) || native_ep0.phase != NE_REPLY || native_ep0.wire_length) {
            native_ep0.interrupted = 1; native_ep0.interruption = ECANCELED;
        }
    }
    /* Gate callback can enqueue but worker must not consume its reply or reuse
     * the snapshot until it has returned and acknowledged this exact event. */
    if (native_ep0.pending) goto out;
    if (native_ep0.phase == NE_TERMINAL) {
        native_ep0.phase = NE_IDLE; native_ep0.interrupted = 0;
        native_ep0.reply = native_ep0.in_flight = 0;
    }
    if (native_ep0.interrupted) {
        ne_finish(native_ep0.interruption, !native_ep0.in_flight, 0); goto out;
    }
    if (!available || suspended) goto out;
    if (csr & SENTSTALL) {
        W(EP0, (csr & ~(EP0_W1C | STALL)) | SENTSTALL);
        csr = R(EP0);
        if (native_ep0.phase != NE_IDLE) { ne_finish(EPIPE, !native_ep0.in_flight, 0); goto out; }
    }
    if (native_ep0.phase == NE_IDLE) {
        if (!(csr & SETUP)) goto out;
        if (R(0x108) != 8 || native_ep0.serial == UINT64_MAX) {
            W(EP0, ne_csr() | STALL | SETUP); goto out;
        }
        x9_mtu3_fifo_read(native_ep0.setup, 8);
        native_ep0.serial++; native_ep0.actual = native_ep0.in_length = 0;
        native_ep0.native_requests = native_ep0.needs_zlp = native_ep0.zlp_queued = 0;
        native_ep0.wire_length = native_ep0.setup[6] | ((uint32_t)native_ep0.setup[7] << 8);
        clock_interval_to_deadline(5000, NSEC_PER_MSEC, &native_ep0.deadline);
        if (native_ep0.wire_length) {
            /* Data-stage direction is latched while acknowledging SETUP. */
            uint32_t next = ne_csr() & ~BIT(20);
            if (native_ep0.setup[0] & 0x80) next |= BIT(20);
            W(EP0, next | SETUP);
        }
        native_ep0.phase = NE_REPLY;
        ne_event(X9_EP0_SETUP, 0, 1, 0); goto out;
    }
    if (mach_absolute_time() >= native_ep0.deadline) {
        W(EP0, ne_csr() | STALL | ((csr & SETUP) ? SETUP : 0));
        ne_finish(ETIMEDOUT, !native_ep0.in_flight, 0); goto out;
    }
    if (native_ep0.phase == NE_IN_DATA_REPLY || native_ep0.phase == NE_IN_ZLP_REPLY) {
        /* Event already acked: original Family data/ZLP callback and the
         * adapter's descriptor complete have returned in the controller gate.
         * Never publish the terminator/status before those obligations finish. */
        if (native_ep0.phase == NE_IN_DATA_REPLY && native_ep0.needs_zlp) {
            if (!native_ep0.zlp_queued || (csr & (TX | BIT(23)))) goto out;
            native_ep0.in_flight = 0;
            W(EP0, ne_csr() | TX); native_ep0.phase = NE_IN_ZLP;
        } else {
            W(EP0, ne_csr() | DATAEND); native_ep0.phase = NE_STATUS;
        }
        goto out;
    }
    if (native_ep0.reply) {
        unsigned reply = native_ep0.reply; native_ep0.reply = 0;
        if (reply == X9_EP0_STALL) {
            W(EP0, ne_csr() | STALL | ((csr & SETUP) ? SETUP : 0));
            ne_finish(EPIPE, 1, 0);
        } else if (reply == X9_EP0_SEND_IN || reply == X9_EP0_SEND_IN_REQUEST) {
            if (csr & (TX | BIT(23))) {
                native_ep0.reply = reply; goto out; /* don't overwrite busy FIFO */
            }
            ne_transmit();
        } else if (reply == X9_EP0_RECEIVE_OUT) native_ep0.phase = NE_OUT;
        else {
            /* OPPO programs DEV_ADDR before arming status. Preserve that
             * register order; the live address latch is not yet verified.
             * Do not apply configuration or enable endpoints on admission. */
            if (native_ep0.setup[0] == 0 && native_ep0.setup[1] == 5 &&
                !native_ep0.wire_length && !native_ep0.setup[3] &&
                !native_ep0.setup[4] && !native_ep0.setup[5] && native_ep0.setup[2] <= 127)
                W(0x800, (R(0x800) & ~(0x7fu << 24)) | ((uint32_t)native_ep0.setup[2] << 24));
            W(EP0, ne_csr() | DATAEND | (native_ep0.phase == NE_OUT_REPLY ? RX : SETUP));
            native_ep0.phase = NE_STATUS;
        }
        goto out; /* current ISR predates this publication, cannot retire it */
    }
    if ((native_ep0.phase == NE_IN || native_ep0.phase == NE_IN_LAST) &&
        (isr & BIT(0)) && !(csr & (TX | BIT(23)))) {
        native_ep0.actual += native_ep0.in_flight; native_ep0.in_flight = 0;
        if (native_ep0.phase == NE_IN_LAST) {
            if (native_ep0.native_requests) {
                native_ep0.phase = NE_IN_DATA_REPLY;
                ne_event(X9_EP0_IN_DATA, 0, 1, 0);
            } else { W(EP0, ne_csr() | DATAEND); native_ep0.phase = NE_STATUS; }
        } else ne_transmit();
    } else if (native_ep0.phase == NE_IN_ZLP && (isr & BIT(0)) && !(csr & (TX | BIT(23)))) {
        native_ep0.phase = NE_IN_ZLP_REPLY;
        ne_event(X9_EP0_IN_ZLP, 0, 1, 0);
    } else if (native_ep0.phase == NE_OUT && (csr & RX)) {
        uint32_t n = R(0x108);
        if (n > 64 || n > native_ep0.wire_length - native_ep0.actual) {
            W(EP0, ne_csr() | STALL | RX); ne_finish(EOVERFLOW, 0, 0); goto out;
        }
        if (n) x9_mtu3_fifo_read(native_ep0.bytes + native_ep0.actual, n);
        native_ep0.actual += n;
        if (n < 64 || native_ep0.actual == native_ep0.wire_length) {
            /* Hold final RX until the actual gate handler accepts status. */
            native_ep0.phase = NE_OUT_REPLY;
            ne_event(X9_EP0_OUT_DATA, 0, 1, 0);
        } else W(EP0, ne_csr() | RX);
    } else if (native_ep0.phase == NE_STATUS &&
        !(csr & (DATAEND | TX | RX | SETUP | STALL | SENTSTALL | BIT(23)))) {
        /* Separate poll, no SETUPEND/reset: hardware retired status, not just
         * a DATAEND write, TX IRQ, descriptor fill, or callback return. */
        ne_finish(0, 1, 1);
    }
out:
    lck_mtx_unlock(&usb_lock);
}
