/* Compiled in the sole MTU3 C worker. OPPO 6.12 mtu3_qmu.c/mtu3_core.c
 * register semantics; no native layout assumptions or fabricated DMA result.
 * The existing five boot-pinned allocations are exclusively profile-owned.
 */
enum { NQ_FREE, NQ_STAGED, NQ_ACTIVE, NQ_RETIRED, NQ_QUARANTINED };
static struct {
    unsigned owned, state, zlp_issued;
    struct x9_usb_qmu_ticket ticket;
} native_qmu[5];
static uint64_t native_qmu_serial;

static int native_index(uint32_t address)
{
    if (address & ~0x8fu) return -1;
    for (unsigned i = 0; i < 5; i++)
        if (!eps[i].interrupt && address == (eps[i].number | (eps[i].in ? 0x80u : 0)))
            return (int)i;
    return -1; /* no EP0, iso, interrupt or guessed endpoint numbering */
}
static int native_ticket_index(struct x9_usb_qmu_ticket t)
{
    int i = native_index(t.endpoint);
    if (i < 0 || !t.serial || native_qmu[i].state == NQ_FREE ||
        native_qmu[i].ticket.serial != t.serial ||
        native_qmu[i].ticket.length != t.length) return -1;
    return i;
}
int x9_usb_native_qmu_enable(uint32_t endpoint, unsigned usb_in)
{
    if (usb_worker != current_thread()) return EPERM;
    int i = native_index(endpoint);
    if (i < 0 || usb_in > 1 || eps[i].in != usb_in) return EINVAL;
    if (!st.native_apple_stack || failed || !eps[i].va ||
        (st.speed != 1 && st.speed != 3)) return ENXIO;
    for (unsigned j = 0; j < 5; j++)
        if (eps[j].enabled && !native_qmu[j].owned) return EBUSY;
    if (native_qmu[i].owned || eps[i].enabled) return EBUSY;
    if (native_qmu[i].state != NQ_FREE || (R(qcsr(&eps[i])) & ACTIVE)) return EBUSY;
    /* Fixed, non-overlapping per-direction 512-byte slots, same allocation
     * geometry as legacy. A future native controller must select this subset,
     * not accept arbitrary FIFO/type/maxpacket from a request. */
    unsigned fifo = eps[i].number == 1 ? 0 : eps[i].in ? 1024 : 512;
    if (R(eps[i].in ? 0xc08 : 0xc0c) < fifo + 512) return ENODEV;
    uint32_t cap = R(0xc10);
    if ((eps[i].in ? cap & 31 : (cap >> 8) & 31) < eps[i].number) return ENODEV;
    maxpacket = st.speed == 3 ? 512 : 64;
    configure_endpoint(&eps[i], fifo);
    native_qmu[i].owned = 1;
    return 0;
}
int x9_usb_native_qmu_validate(uint32_t endpoint, unsigned usb_in)
{
    if (usb_worker != current_thread()) return EPERM;
    int i = native_index(endpoint);
    if (i < 0 || usb_in > 1 || eps[i].in != usb_in) return EINVAL;
    if (!st.native_apple_stack || failed || suspended ||
        (st.reserved & X9_USB_BUS_FORCED_OFF) || !st.configuration ||
        !native_qmu[i].owned || !eps[i].enabled || !eps[i].va) return ENXIO;
    if (eps[i].halted) return EPIPE;
    if (native_qmu[i].state == NQ_QUARANTINED) return EIO;
    return 0; /* busy endpoints may queue; stage alone checks FREE */
}
int x9_usb_native_qmu_buffer(uint32_t endpoint, void **bounce)
{
    if (!bounce) return EINVAL;
    *bounce = NULL;
    int error = x9_usb_native_qmu_validate(endpoint, !!(endpoint & 0x80));
    if (error) return error;
    int i = native_index(endpoint);
    if (native_qmu[i].state != NQ_FREE || eps[i].busy) return EBUSY;
    *bounce = eps[i].va + DMA_BUFFER;
    return 0;
}
int x9_usb_native_qmu_stage(uint32_t endpoint, uint32_t n,
                          struct x9_usb_qmu_ticket *ticket)
{
    if (!ticket || n > DMA_BUFFER || (!n && !(endpoint & 0x80))) return EINVAL;
    /* Ordinary IN ZLP still publishes a real zero-length GPD. The matching
     * QMU length-error path below may emit via BMU; neither is completion. */
    void *bounce = NULL;
    int error = x9_usb_native_qmu_buffer(endpoint, &bounce);
    if (error) return error;
    if (native_qmu_serial == UINT64_MAX) return EOVERFLOW;
    int i = native_index(endpoint); struct endpoint *e = &eps[i];
    error = stop_endpoint(e); if (error) return error;
    prepare_gpd(e, bounce, n, 0); /* no ZLP between 4096-byte chunks */
    native_qmu[i].ticket = (struct x9_usb_qmu_ticket){ ++native_qmu_serial, endpoint, n };
    native_qmu[i].state = NQ_STAGED;
    native_qmu[i].zlp_issued = 0;
    e->requested = n;
    *ticket = native_qmu[i].ticket;
    return 0;
}
int x9_usb_native_qmu_publish(struct x9_usb_qmu_ticket t,
                             int (*commit)(void *), void *context)
{
    if (usb_worker != current_thread()) return EPERM;
    int i = native_ticket_index(t);
    if (i < 0) return ESTALE;
    if (native_qmu[i].state != NQ_STAGED || !commit) return EINVAL;
    int error = x9_usb_native_qmu_validate(t.endpoint, eps[i].in);
    if (error) return error;
    if (R(qcsr(&eps[i])) & ACTIVE) return EBUSY;
    error = commit(context);
    if (error) return error; /* owner did NOT publish; staged can be discarded */
    publish_gpd(&eps[i]); /* immediate HWO/cache/MMIO; no owner call here */
    native_qmu[i].state = NQ_ACTIVE; eps[i].busy = 1;
    return 0; /* no post-publication rejection */
}
int x9_usb_native_qmu_discard(struct x9_usb_qmu_ticket t)
{
    if (usb_worker != current_thread()) return EPERM;
    int i = native_ticket_index(t);
    if (i < 0) return ESTALE;
    if (native_qmu[i].state != NQ_STAGED) return EBUSY;
    native_qmu[i].state = NQ_FREE; native_qmu[i].ticket = (struct x9_usb_qmu_ticket){0};
    return 0;
}
static int native_tx_zlp(unsigned i, uint64_t current)
{
    struct endpoint *e = &eps[i];
    struct gpd *g = descriptor(e, e->slot);
    /* The outer worker's availability flag is coarse: suspend/configuration
     * and hardware speed/connection must be requalified before a NEW packet.
     * A failed guard falls through to STOP/quarantine, never deferred replay. */
    int error = x9_usb_native_qmu_validate(native_qmu[i].ticket.endpoint, e->in);
    if (error) return error;
    if ((st.reserved & X9_USB_BUS_CYCLING) || !(R(0x2404) & BIT(6)) ||
        (R(0x800) & 7) != st.speed || (st.speed != 1 && st.speed != 3)) return ENXIO;
    /* Caller sampled this endpoint's sole LEN_ERR with no CS_ERR. Requalify
     * actual current GPD, direction/count and pinned state before BMU. Never
     * bypass a nonzero request or publish a second ZLP for the same ticket. */
    if (!e->in || native_qmu[i].state != NQ_ACTIVE || native_qmu[i].ticket.length ||
        native_qmu[i].zlp_issued || current != e->pa + e->slot * DESC_STRIDE ||
        (g->length & (gen2 ? 0xfffffu : 0xffffu))) return EIO;
    uint32_t txcsr = R(csr0(e));
    if (!(txcsr & BIT(24)) || (txcsr & (BIT(25) | BIT(16) | BIT(22)))) return EIO;
    /* OPPO qmu_tx_zlp_error_handler: DMAREQEN off -> TXPKTRDY -> GPD BPS/HWO
     * -> DMAREQEN on -> RESUME. No TX buffer write for a zero-byte packet.
     * Require idle FIFO, rather than waiting/publishing against another packet.
     * No owner call here: the accepted chunk was already marked published. */
    W(csr0(e), txcsr & ~(BIT(29) | BIT(22)));
    native_qmu[i].zlp_issued = 1; // irreversible packet publication obligation
    W(csr0(e), (R(csr0(e)) & ~BIT(22)) | BIT(16));
    g->flags |= BIT(2) | HWO; cache(g, DESC_STRIDE, 0); // bypass the zero GPD
    W(csr0(e), (R(csr0(e)) & ~BIT(22)) | BIT(29));
    W(qcsr(e), BIT(1)); // one RESUME, not a second transfer or automatic replay
    W(0x780, BIT(e->number + 16)); // acknowledge ONLY this observed LEN_ERR
    return 0;
}
int x9_usb_native_qmu_probe(struct x9_usb_qmu_ticket t, unsigned stop,
                           struct x9_usb_qmu_evidence *r)
{
    if (usb_worker != current_thread()) return EPERM;
    if (!r || stop > 1) return EINVAL;
    bzero(r, sizeof(*r));
    int i = native_ticket_index(t);
    if (i < 0) return ESTALE;
    if (native_qmu[i].state != NQ_ACTIVE) return EBUSY;
    struct endpoint *e = &eps[i];
    struct gpd *g = descriptor(e, e->slot);
    cache(g, DESC_STRIDE, 1);
    uint64_t current = R(qcsr(e) + 8) | ((uint64_t)((R(qhi(e)) >> 8) & 15) << 32);
    uint64_t start = e->pa + e->slot * DESC_STRIDE;
    uint64_t end = e->pa + (e->slot ^ 1) * DESC_STRIDE;
    /* Check only this endpoint's bits; a stale DONE bit is never completion. */
    unsigned hw_error = e->in ? R(0x780) & (BIT(e->number) | BIT(e->number + 16)) :
        R(0x790) & (BIT(e->number) | BIT(e->number + 16));
    int zlp_error = 0;
    if (!stop && e->in && !t.length && hw_error == BIT(e->number + 16)) {
        zlp_error = native_tx_zlp((unsigned)i, current);
        if (!zlp_error) return EAGAIN; // publication is NOT wire/QMU retirement
    }
    uint32_t txcsr = e->in ? R(csr0(e)) : 0;
    unsigned fifo_drained = !e->in || ((txcsr & BIT(24)) && !(txcsr & BIT(16)));
    if (!stop && !hw_error && ((g->flags & HWO) || current == start ||
        (current == end && !fifo_drained))) return EAGAIN;
    r->error = zlp_error ? zlp_error : hw_error ? EIO : 0;
    int error = stop_endpoint(e); /* does not erase GPD/CPR */
    r->stopped = !error && !(R(qcsr(e)) & ACTIVE);
    if (error) r->error = error;
    cache(g, DESC_STRIDE, 1);
    current = R(qcsr(e) + 8) | ((uint64_t)((R(qhi(e)) >> 8) & 15) << 32);
    r->hwo_clear = !(g->flags & HWO); r->cpr_at_terminator = current == end;
    unsigned actual = g->length & (gen2 ? 0xfffffu : 0xffffu);
    /* A stopped but unretired GPD has no qualified partial-wire count. IN's
     * programmed length alone is NOT evidence that those bytes escaped. */
    r->actual_qualified = r->stopped && r->hwo_clear && r->cpr_at_terminator &&
        !hw_error && fifo_drained && actual <= t.length && (!e->in || actual == t.length);
    if (r->actual_qualified) r->actual = actual;
    else if (!r->error) r->error = EIO;
    if (!r->actual_qualified)
        printf("X9 USB NATIVE: ep=%#x serial=%llu STOP=%u HWO-clear=%u CPR-end=%u FIFO-drained=%u raw-length=%u UNQUALIFIED error=%d\n",
            t.endpoint, t.serial, r->stopped, r->hwo_clear, r->cpr_at_terminator,
            fifo_drained, actual, r->error);
    if (!e->in && r->actual_qualified) {
        cache(e->va + DMA_BUFFER, DMA_BUFFER, 1); r->out_bytes = e->va + DMA_BUFFER;
    }
    native_qmu[i].state = NQ_RETIRED; /* retains until explicit owner decision */
    return 0;
}
int x9_usb_native_qmu_finish(struct x9_usb_qmu_ticket t, unsigned quarantine)
{
    if (usb_worker != current_thread()) return EPERM;
    int i = native_ticket_index(t);
    if (i < 0) return ESTALE;
    if (native_qmu[i].state != NQ_RETIRED || quarantine > 1) return EINVAL;
    if (quarantine) { native_qmu[i].state = NQ_QUARANTINED; return 0; }
    eps[i].slot ^= 1; eps[i].busy = 0;
    native_qmu[i].state = NQ_FREE; native_qmu[i].ticket = (struct x9_usb_qmu_ticket){0};
    lck_mtx_lock(&usb_lock); st.dma_completions++; lck_mtx_unlock(&usb_lock);
    return 0;
}
int x9_usb_native_qmu_barrier(void)
{
    if (usb_worker != current_thread()) return EPERM;
    for (unsigned i = 0; i < 5; i++)
        if (native_qmu[i].state != NQ_FREE) return EIO;
    for (unsigned i = 0; i < 5; i++) native_qmu[i].owned = 0;
    return 0; /* reset_data disables the now-drained endpoints next */
}
int x9_usb_native_qmu_rearm_ready(void)
{
    if (usb_worker != current_thread()) return EPERM;
    int control_error = x9_usb_native_ep0_rearm_ready();
    if (control_error) return control_error;
    if (!st.native_apple_stack || failed || suspended || !st.configuration ||
        (st.reserved & (X9_USB_BUS_FORCED_OFF | X9_USB_BUS_CYCLING))) return ENXIO;
    if (!(R(0x2404) & BIT(6)) || (R(0x800) & 7) != st.speed ||
        (st.speed != 1 && st.speed != 3)) return ENXIO;
    unsigned pairs = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (native_qmu[i].state != NQ_FREE) return EBUSY;
        if (!native_qmu[i].owned) {
            if (eps[i].enabled) return EBUSY; /* never borrow a private endpoint */
            continue;
        }
        unsigned endpoint = eps[i].number | (eps[i].in ? 0x80u : 0);
        int error = x9_usb_native_qmu_validate(endpoint, eps[i].in);
        if (error) return error;
        if (eps[i].busy || (R(qcsr(&eps[i])) & ACTIVE)) return EBUSY;
        pairs |= BIT((eps[i].number == 1 ? 0 : 2) + eps[i].in);
    }
    /* Native bulk control plane must have at least one complete IN/OUT pair,
     * and no half-enabled pair. No profile/configuration flag is written. */
    return pairs && ((pairs & 3) == 0 || (pairs & 3) == 3) &&
        ((pairs & 12) == 0 || (pairs & 12) == 12) ? 0 : ENXIO;
}
void x9_usb_native_qmu_isolate(void)
{
    if (usb_worker != current_thread()) panic("X9 USB: native isolate on second worker");
    W(0x2404, R(0x2404) & ~BIT(6));
    lck_mtx_lock(&usb_lock);
    if (!failed) { st.dma_errors++; st.generation++; }
    failed = 1; st.state = X9_USB_FAULT; st.error = EIO;
    lck_mtx_unlock(&usb_lock);
    /* Backend closes all requests and STOPs all in-flight endpoints this poll.
     * Do not recurse into fault/reset or reset quarantined descriptor state. */
}
