/* Included after native shared/memory owners. No borrowed Linux pointers. */
#include "x9_vdec_control.h"
#define VCONTROL_MAGIC UINT64_C(0x5839435452303031)
#define VCONTROL_SCRATCH (2u * X9_VCP_INSTANCE_SLOT_BYTES)
struct x9_vdec_control {
    uint64_t magic;
    lck_mtx_t lock;
    struct x9_vcp_bus *bus;
    struct x9_vdec_shared *shared;
    struct x9_vdec_memory *memory;
    struct x9_vcp_init init;
    struct x9_vcp_instance_slot slot;
    struct x9_vdec_control_hardware hardware;
    struct x9_vcp_bus_reverse_ops reverse;
    struct x9_vdec_control_status status;
    struct x9_vdec_dma_slice output_slice[X9_VCP_OUTPUT_PLANES];
    uint32_t output_bytes[X9_VCP_OUTPUT_PLANES];
    struct x9_vcp_output_request output_request;
    struct x9_vdec_dma_slice input_slice;
    const uint8_t *input_source; /* borrowed ONLY for the reserved synchronous call */
    uint32_t input_capacity;
    uint8_t *scratch;
    bool closing;
};
static int vcontrol_fault(struct x9_vdec_control *c, int e)
{
    if (e && !c->status.last_errno) c->status.last_errno = (uint32_t)e;
    return e;
}
static int vcontrol_shm(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_shm_request *q, struct x9_vcp_shm_mapping *out)
{
    struct x9_vdec_control *c = cookie;
    return x9_vdec_shared_reply(c->shared,epoch,end,q,out);
}
static int vcontrol_alloc(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_mem_request *q, struct x9_vcp_mem_allocation *out)
{
    struct x9_vdec_control *c = cookie;
    return x9_vdec_memory_alloc(c->memory,epoch,end,q,out);
}
static int vcontrol_clock(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_clock_request *q)
{
    struct x9_vdec_control *c = cookie;
    return c->hardware.clock(c->hardware.cookie,epoch,end,q);
}
static int vcontrol_wait(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_wait_request *q, enum x9_vcp_wait_outcome *out)
{
    struct x9_vdec_control *c = cookie;
    return c->hardware.wait(c->hardware.cookie,epoch,end,q,out);
}
/* These are native NC accesses, not calls into a platform mock writer. Slot
 * resolver/unique binding checks precede every use; no generic live write API
 * is public. All scalar addresses are aligned in captured 0x19e8-byte slots. */
static uint32_t vcontrol_load32(volatile uint8_t *p, uint32_t offset)
{ return *(volatile uint32_t *)(volatile void *)(p+offset); }
static void vcontrol_store32(volatile uint8_t *p, uint32_t offset, uint32_t v)
{ *(volatile uint32_t *)(volatile void *)(p+offset) = v; }
static void vcontrol_store64(volatile uint8_t *p, uint32_t offset, uint64_t v)
{ *(volatile uint64_t *)(volatile void *)(p+offset) = v; }
/* s->lock held, freshly bound INIT only. Captured decode producer performs
 * this AP fence initialization after b000; do not reset any FW-owned bytes. */
static int vcontrol_initialize_slot(struct x9_vdec_control *c)
{
    struct x9_vdec_shared *s = c->shared;
    volatile uint8_t *p = NULL;
    int e;
    if (!c->status.bound || c->status.slot_initialized ||
        !(s->bound_slots & (1u << c->slot.index))) return EBUSY;
    if (c->slot.offset & 7) return EFAULT;
    lck_mtx_lock(&s->arena->lock);
    e = vshared_live_window(s,X9_VCP_GLOBAL_VDEC_OFFSET+c->slot.offset,
        X9_VCP_INSTANCE_SLOT_BYTES,&p);
    if (!e) {
        vcontrol_store32(p,0x1228,0);
        __asm__ volatile("dsb oshst" ::: "memory");
        if (vcontrol_load32(p,0x1228)) e = EIO;
        else c->status.slot_initialized = 1;
    }
    lck_mtx_unlock(&s->arena->lock);
    return e;
}
/* Physical bus whole-call reservation and c->lock held. The ONLY published
 * AP fields are those in decoder_decode_evidence. a005 itself writes the FB
 * fields in firmware: do not duplicate those or clear fields beside errors.
 * Normal NC avoids overwriting FW-updated neighbouring bytes with WB lines.
 * Qualification of the real non-ACP main route still precedes these stores. */
static int vcontrol_stage_start(struct x9_vdec_control *c, uint64_t end,
    const struct x9_vcp_decode_snapshot *observed)
{
    struct x9_vdec_shared *s = c->shared;
    const struct x9_vdec_control_start_resources *r = &c->status.resources;
    volatile uint8_t *p = NULL;
    int e;
    lck_mtx_lock(&s->lock);
    lck_mtx_lock(&s->arena->lock);
    e = !c->status.bound || !c->status.slot_initialized || c->status.vsi_attempted ||
        !(s->bound_slots & (1u << c->slot.index)) ? EBUSY : vmem_deadline(end);
    if (!e && (c->slot.offset & 7)) e = EFAULT;
    if (!e) e = vshared_live_window(s,X9_VCP_GLOBAL_VDEC_OFFSET+c->slot.offset,
        X9_VCP_INSTANCE_SLOT_BYTES,&p);
    if (!e) {
        __asm__ volatile("dmb oshld" ::: "memory");
        /* Two equal snapshots do not license a live store. These gates only
         * reject changed negotiation; AP ownership comes from the matched
         * ABI's precise producer fields and the real owner qualification. */
        if (p[0x1000] != 2 || p[0x1000] != observed->input_driven ||
            p[0x1001] != observed->output_async || p[0x1003] != observed->low_power_mode ||
            vcontrol_load32(p,0xf80) != observed->bitdepth ||
            vcontrol_load32(p,0xf28) != observed->plane_count ||
            vcontrol_load32(p,0x1228)) e = ESTALE;
    }
    if (!e) e = vmem_deadline(end);
    if (!e) {
        c->status.vsi_attempted = 1; /* retain before first live store */
        vcontrol_store64(p,0xe80,r->input.iova);
        vcontrol_store64(p,0xe90,0); /* qualified ABSENT attachment, NOT main alias */
        vcontrol_store64(p,0xf18,r->input_token);
        vcontrol_store64(p,0xf48,r->timestamp);
        p[0x1004] = (uint8_t)r->dynamic_low_latency;
        __asm__ volatile("dsb oshst" ::: "memory");
        c->status.vsi_staged = 1;
    }
    lck_mtx_unlock(&s->arena->lock);
    lck_mtx_unlock(&s->lock);
    return e;
}
/* Two fresh PoC copies, not cached Android bytes. Equal copies do NOT prove
 * firmware-wide serialization: this only observes fields while the bus owns
 * the current command/reverse request. Ring results are never consumption or
 * completion authority. No CPU writer, flag reset or shared index store. */
static int vcontrol_snapshot(struct x9_vdec_control *c, uint64_t end,
    struct x9_vcp_common_snapshot *out)
{
    struct x9_vdec_shared *s = c->shared;
    uint8_t common[2][X9_VCP_COMMON_PREFIX_BYTES];
    int e;
    if (!c->status.bound) return ENXIO;
    lck_mtx_lock(&s->lock);
    e = vmem_deadline(end);
    for (unsigned n = 0; n < 2 && !e; n++) {
        e = vshared_observe(s,X9_VCP_GLOBAL_VDEC_OFFSET,common[n],sizeof(common[n]));
        if (!e) e = vshared_observe(s,X9_VCP_GLOBAL_VDEC_OFFSET+c->slot.offset,
            c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES,X9_VCP_INSTANCE_SLOT_BYTES);
    }
    if (!e && (memcmp(common[0],common[1],sizeof(common[0])) ||
        memcmp(c->scratch,c->scratch+X9_VCP_INSTANCE_SLOT_BYTES,
            X9_VCP_INSTANCE_SLOT_BYTES))) e = EAGAIN;
    if (!e) e = x9_vcp_parse_common_snapshot(X9_VCP_COMMON_ABI_MT6993_CAPTURE1,
        common[1],sizeof(common[1]),out);
    if (!e && (out->has_fault || out->enable_smmu != 1)) e = EIO;
    if (!e) e = vmem_deadline(end);
    lck_mtx_unlock(&s->lock);
    return e;
}
static int vcontrol_vp(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_vp_request *q, struct x9_vcp_common_snapshot *out,
    uint32_t *bitdepth)
{
    struct x9_vdec_control *c = cookie;
    struct x9_vcp_decode_snapshot decoded;
    struct x9_vcp_common_snapshot common;
    uint32_t group, state = 0;
    uint8_t reply[X9_VCP_WIRE_BYTES];
    int e;
    if (!out || !bitdepth || !q) return EINVAL;
    memset(out,0,sizeof(*out)); *bitdepth = 0;
    if (epoch != c->status.epoch || q->instance_token != c->init.instance_token ||
        q->service_id != c->init.service_id) return ESTALE;
    e = vcontrol_snapshot(c,end,&common);
    if (!e) e = x9_vcp_parse_decode_snapshot(X9_VCP_DECODE_ABI_MT6993_CAPTURE1,
        c->scratch,X9_VCP_INSTANCE_SLOT_BYTES,&decoded);
    if (!e && decoded.bitdepth != 8 && decoded.bitdepth != 10) e = ENOTSUP;
    if (e) return e;
    group = decoded.bitdepth == 8 ? 0u : 1u;
    /* Publication flags/address bytes are observed, not invented from the
     * cold image. Compare to the six durable native slice handoffs too. */
    lck_mtx_lock(&c->shared->lock);
    for (unsigned g = 0; g < 2 && !e; g++) {
        if (common.vp_published[g] != 1) { e = EIO; break; }
        for (unsigned k = 0; k < 3 && !e; k++) {
            if (common.vp_iova[g][k] != c->shared->vp_extent[g][k].iova) e = ESTALE;
            if (!e) e = x9_vdec_dma_slice_state(c->shared->codec,
                &c->shared->vp[g][k],&state);
            if (!e && state != X9_VDEC_DMA_DEVICE) e = EBUSY;
        }
    }
    lck_mtx_unlock(&c->shared->lock);
    if (!e && common.vp_published[group] != 1) e = EIO;
    if (!e) e = x9_vcp_encode_vp_reply(&c->init,q,&common,decoded.bitdepth,reply);
    if (!e) e = vmem_deadline(end);
    if (!e) { *out = common; *bitdepth = decoded.bitdepth; }
    return e;
}
static int vcontrol_recheck(struct x9_vdec_control *c, uint64_t end)
{
    struct x9_vcp_common_snapshot common;
    struct x9_vcp_ring_snapshot rings[3];
    int e = vcontrol_snapshot(c,end,&common);
    for (unsigned kind = 0; kind < 3 && !e; kind++)
        e = x9_vcp_parse_ring_snapshot(X9_VCP_RING_ABI_MT6993_CAPTURE1,kind,
            c->scratch,X9_VCP_INSTANCE_SLOT_BYTES,&rings[kind]);
    if (!e) {
        memcpy(c->status.rings,rings,sizeof(rings));
        c->status.recheck_pending = 0;
    }
    /* A concurrent firmware update is retained as a pending observation,
     * not a completed frame or a dropped/replied notification. */
    return e == EAGAIN ? 0 : e;
}
static int vcontrol_notification(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_output_notification *q)
{
    struct x9_vdec_control *c = cookie;
    int e;
    if (epoch != c->status.epoch || q->instance_token != c->init.instance_token)
        return ESTALE;
    e = vmem_deadline(end);
    if (e) return e;
    if (c->status.notifications == UINT64_MAX) return EOVERFLOW;
    c->status.last_notification = *q; c->status.notifications++;
    c->status.recheck_pending = 1;
    memset(c->status.rings,0,sizeof(c->status.rings));
    if (q->firmware_status) return EIO;
    /* During INIT there is no bound VSI yet: retain the actual event until
     * a subsequent reverse notification or reserved START preparation. */
    return c->status.bound ? vcontrol_recheck(c,end) : 0;
}
static int vcontrol_prepare_start(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_init *init, const struct x9_vcp_init_result *result,
    const struct x9_vcp_start *q)
{
    struct x9_vdec_control *c = cookie;
    struct x9_vdec_control_start_resources *resources = &c->status.resources;
    struct x9_vdec_dma_extent extent;
    struct x9_vcp_common_snapshot common;
    struct x9_vcp_decode_snapshot decoded;
    uint32_t input_state;
    int e;
    if (epoch != c->status.epoch || !c->status.start_attempted ||
        !c->status.input_attempted || c->status.input_prepared ||
        !c->input_source || c->input_slice.token || !c->status.output_prepared ||
        init->instance_token != c->init.instance_token ||
        result->firmware_vsi_address != c->status.init.firmware_vsi_address ||
        q->bytes_used != resources->bytes_used ||
        q->buffer_bytes != ((c->input_capacity + VDMA_CPU_BYTES - 1) &
            ~(VDMA_CPU_BYTES - 1))) return ESTALE;
    e = c->status.recheck_pending ? vcontrol_recheck(c,end) : 0;
    /* a005's notification is NOT output acceptance. Revalidate our actual
     * DEVICE storage, and leave VSI/layout/IRQ qualification to the real
     * qualify_start owner. No fresh addresses are inferred from b005. */
    if (!e && c->status.output_attempted) {
        if (!c->status.output_prepared) e = EBUSY;
        for (unsigned n = 0; n < c->status.output.plane_count && !e; n++) {
            uint32_t state;
            e = x9_vdec_dma_slice_state(c->shared->codec,&c->output_slice[n],&state);
            if (!e && state != X9_VDEC_DMA_DEVICE) e = EBUSY;
        }
    }
    if (!e) e = vmem_deadline(end);
    if (!e) {
        resources->input_token = vdma_take_token();
        if (!resources->input_token) e = EOVERFLOW;
    }
    if (!e) e = x9_vdec_dma_slice_alloc(c->shared->codec,epoch,
        c->input_capacity,128,&c->input_slice);
    if (!e) {
        resources->input_slice_token = c->input_slice.token;
        /* Initial wire capacity used the native allocator's exact rounding.
         * Verify the actual handle before either copying or exposing input. */
        if (q->buffer_bytes != c->input_slice.capacity) e = ESTALE;
    }
    if (!e) e = vmem_deadline(end);
    if (!e) e = x9_vdec_dma_slice_write(c->shared->codec,&c->input_slice,
        0,c->input_source,resources->bytes_used);
    if (!e) e = vmem_deadline(end);
    if (!e) e = x9_vdec_dma_slice_state(c->shared->codec,&c->input_slice,&input_state);
    if (!e && input_state != X9_VDEC_DMA_CPU_PRIVATE) e = EBUSY;
    if (!e) e = x9_vdec_dma_slice_publish(c->shared->codec,&c->input_slice,&extent);
    if (!e) {
        /* Record the real exposure BEFORE any subsequent deadline/owner error.
         * A failed VSI preparation must not lose or recycle DEVICE backing. */
        resources->input = extent;
        c->status.input_prepared = 1;
    }
    if (!e) e = vmem_deadline(end);
    if (!e) e = vcontrol_snapshot(c,end,&common);
    if (!e) e = x9_vcp_parse_decode_snapshot(X9_VCP_DECODE_ABI_MT6993_CAPTURE1,
        c->scratch,X9_VCP_INSTANCE_SLOT_BYTES,&decoded);
    if (!e && (decoded.input_driven != 2 || decoded.ipi_blocked ||
        decoded.plane_count != resources->output.plane_count ||
        (decoded.bitdepth != 8 && decoded.bitdepth != 10))) e = ENOTSUP;
    /* Firmware/layout/routing and IRQ ownership require the real hardware
     * owner; it receives immutable host records, not a live writable VSI. */
    if (!e) e = c->hardware.qualify_start(c->hardware.cookie,epoch,end,init,result,q,
        resources,&decoded);
    if (!e) e = vcontrol_stage_start(c,end,&decoded);
    return e;
}
static int vcontrol_prepare_output(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_init *init, const struct x9_vcp_init_result *ready,
    struct x9_vcp_output_request *q, struct x9_vcp_output_record *record)
{
    struct x9_vdec_control *c = cookie;
    struct x9_vcp_common_snapshot common;
    struct x9_vcp_output_record *r = &c->status.output;
    int e;
    if (epoch != c->status.epoch || !c->status.output_attempted ||
        c->status.output_prepared || c->status.start_attempted ||
        init->instance_token != c->init.instance_token ||
        ready->firmware_vsi_address != c->status.init.firmware_vsi_address)
        return ESTALE;
    e = vcontrol_snapshot(c,end,&common);
    for (unsigned n = 0; n < r->plane_count && !e; n++) {
        e = vmem_deadline(end);
        if (!e) e = x9_vdec_dma_slice_alloc(c->shared->codec,epoch,
            c->output_bytes[n],128,&c->output_slice[n]);
    }
    /* Obtain/journal ALL backing before exposing Y: C capacity failure
     * must not unnecessarily turn a still-private Y slice into DEVICE. */
    for (unsigned n = 0; n < r->plane_count && !e; n++) {
        struct x9_vdec_dma_extent extent;
        if (!e) e = vmem_deadline(end);
        if (!e) e = x9_vdec_dma_slice_publish(c->shared->codec,&c->output_slice[n],&extent);
        if (!e) {
            /* Journal EACH real handoff immediately, including padding.
             * Partial Y success/C failure never loses Y's DEVICE handle. */
            r->plane[n] = (struct x9_vcp_output_extent){extent.iova,extent.capacity};
            if (n) c->output_request.c_iova = extent.iova;
            else c->output_request.y_iova = extent.iova;
        }
    }
    if (!e) e = vmem_deadline(end);
    if (!e) {
        c->status.output_prepared = 1;
        *record = *r; *q = c->output_request;
    }
    return e;
}
int x9_vdec_control_create(struct x9_vcp_bus *bus, struct x9_vdec_shared *s,
    struct x9_vdec_memory *m, const struct x9_vdec_control_hardware *hardware,
    struct x9_vdec_control **out)
{
    struct x9_vdec_control *c;
    int e;
    if (!bus || !s || s->magic != VSHARED_MAGIC || !m || m->magic != VMEM_MAGIC ||
        !hardware || !hardware->clock || !hardware->wait || !out || *out) return EINVAL;
    e = x9_vcp_bus_check_shared(bus,s->epoch,&s->global);
    if (e) return e;
    c = kalloc_type(struct x9_vdec_control,Z_WAITOK | Z_ZERO);
    if (!c) return ENOMEM;
    c->scratch = kalloc_data(VCONTROL_SCRATCH,Z_WAITOK | Z_ZERO);
    if (!c->scratch) { kfree_type(struct x9_vdec_control,c); return ENOMEM; }
    lck_mtx_lock(&s->lock); lck_mtx_lock(&m->lock);
    e = s->closing || !s->published || m->closing || m->control_refs ? EBUSY :
        s->fault_errno ? s->fault_errno : m->fault_errno;
    if (!e && (m->epoch != s->epoch || m->arena[0] != s->arena ||
        m->arena[1] != s->codec)) e = ESTALE;
    if (!e && s->control_refs == UINT32_MAX) e = EOVERFLOW;
    if (!e) { s->control_refs++; m->control_refs++; c->init = m->init; }
    lck_mtx_unlock(&m->lock); lck_mtx_unlock(&s->lock);
    if (e) {
        kfree_data(c->scratch,VCONTROL_SCRATCH); kfree_type(struct x9_vdec_control,c);
        return e;
    }
    c->magic = VCONTROL_MAGIC; c->bus = bus; c->shared = s; c->memory = m;
    c->hardware = *hardware; c->status.epoch = s->epoch;
    c->reverse = (struct x9_vcp_bus_reverse_ops){ .cookie = c,
        .shm = vcontrol_shm, .alloc = vcontrol_alloc, .clock = vcontrol_clock,
        .wait = vcontrol_wait, .vp = vcontrol_vp, .notification = vcontrol_notification,
        .prepare_output = vcontrol_prepare_output };
    if (hardware->qualify_start) c->reverse.prepare_start = vcontrol_prepare_start;
    lck_mtx_init(&c->lock,&vdec_group,LCK_ATTR_NULL);
    *out = c;
    return 0;
}
int x9_vdec_control_init(struct x9_vdec_control *c, uint32_t ms,
    struct x9_vcp_init_result *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vdec_shared *s;
    uint64_t iova;
    uint32_t alias;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !out || !log) return EINVAL;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    if (!ms || ms > 3000) return EINVAL;
    lck_mtx_lock(&c->lock); s = c->shared;
    e = c->closing || c->status.init_attempted ? EBUSY : (int)c->status.last_errno;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&s->global);
    if (!e) {
        c->status.init_attempted = 1;
        e = x9_vcp_bus_init(c->bus,&c->init,&c->reverse,ms,out,log);
        c->status.init = *out; /* retain even a firmware/transport error */
        if (!e) {
            iova = s->service.vdec.iova;
            /* Captured RV55 payload10fe0..11026 and16 rows at1c220:
             * [0x140000000,0x1c0000000) -> [0x30000000,0xb0000000).
             * Owner must already prove this firmware's residency/pairing. */
            if (iova < UINT64_C(0x140000000) || iova >= UINT64_C(0x1c0000000)) e = ERANGE;
            else {
                alias = (uint32_t)(UINT64_C(0x30000000)+iova-UINT64_C(0x140000000));
                e = x9_vcp_resolve_instance_slot(X9_VCP_INSTANCE_ABI_MT6993_CAPTURE1,
                    &s->service.vdec,alias,out->firmware_vsi_address,&c->slot);
            }
            if (!e) {
                lck_mtx_lock(&s->lock);
                if (s->closing || s->fault_errno) e = EBUSY;
                else if (s->bound_slots & (1u << c->slot.index)) e = EEXIST;
                else {
                    s->bound_slots |= 1u << c->slot.index; c->status.bound = 1;
                    e = vcontrol_initialize_slot(c);
                }
                lck_mtx_unlock(&s->lock);
            }
        }
        vcontrol_fault(c,e);
    }
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_query(struct x9_vdec_control *c, const struct x9_vcp_query *q,
    uint32_t ms, struct x9_vcp_query_result *out, struct x9_vcp_bus_call_log *log)
{
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !q || !out || !log) return EINVAL;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    if (!ms || ms > 3000) return EINVAL;
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || c->status.start_attempted ? EBUSY :
        (int)c->status.last_errno;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) e = x9_vcp_bus_query(c->bus,&c->init,q,&c->reverse,ms,out,log);
    if (log->request_attempted) vcontrol_fault(c,e);
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_get_status(struct x9_vdec_control *c, struct x9_vdec_control_status *out)
{
    if (!c || c->magic != VCONTROL_MAGIC || !out) return EINVAL;
    lck_mtx_lock(&c->lock); *out = c->status; lck_mtx_unlock(&c->lock);
    return 0;
}
int x9_vdec_control_register_output(struct x9_vdec_control *c, uint32_t planes,
    const uint32_t bytes[X9_VCP_OUTPUT_PLANES], uint32_t ms,
    struct x9_vcp_output_notification *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_output_record *r;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !bytes || !out || !log) return EINVAL;
    if (!ms || ms > 3000 || !planes || planes > X9_VCP_OUTPUT_PLANES ||
        !bytes[0] || (planes == 1 ? bytes[1] != 0 : !bytes[1])) return EINVAL;
    for (unsigned n = 0; n < planes; n++)
        if (bytes[n] > X9_MM_SMMU_MAX_CHECK_BYTES) return EMSGSIZE;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || c->status.output_attempted ||
        c->status.start_attempted ? EBUSY : (int)c->status.last_errno;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) {
        c->status.output_attempted = 1;
        r = &c->status.output;
        r->buffer_token = vdma_take_token();
        if (!r->buffer_token) e = EOVERFLOW;
        r->instance_token = c->init.instance_token; r->context_id = c->init.context_id;
        r->generation = c->status.epoch; r->plane_count = planes;
        r->firmware_vsi_address = c->status.init.firmware_vsi_address;
        c->output_request = (struct x9_vcp_output_request){
            .firmware_vsi_address = r->firmware_vsi_address, .generation = r->generation,
            .buffer_token = r->buffer_token, .general_iova = UINT64_MAX };
        for (unsigned n = 0; n < planes; n++) c->output_bytes[n] = bytes[n];
        if (!e) e = x9_vcp_bus_register_output(c->bus,&c->init,&c->status.init,
            &c->reverse,ms,out,log);
        c->status.output_notified = out->message_id == X9_VCP_FRAME_BUFFER_DONE;
        vcontrol_fault(c,e);
    }
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_decode_start(struct x9_vdec_control *c, const uint8_t *annexb,
    uint32_t used, uint32_t capacity, uint32_t flags, uint64_t timestamp,
    uint32_t dynamic_low_latency, uint32_t ms,
    struct x9_vcp_start_result *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_start q;
    struct x9_vdec_control_start_resources *resources;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !annexb || !out || !log) return EINVAL;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    if (!ms || ms > 3000 || !used || used > capacity ||
        capacity > X9_MM_SMMU_MAX_CHECK_BYTES || dynamic_low_latency > UINT8_MAX)
        return EINVAL;
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || c->status.start_attempted ||
        c->status.input_attempted || !c->status.output_prepared ? EBUSY :
        (int)c->status.last_errno;
    if (!e && !c->reverse.prepare_start) e = ENOTSUP;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) {
        c->status.start_attempted = 1;
        c->status.input_attempted = 1;
        resources = &c->status.resources;
        resources->epoch = c->status.epoch;
        resources->instance_token = c->init.instance_token;
        resources->slot = c->slot;
        resources->bytes_used = used;
        resources->timestamp = timestamp;
        resources->dynamic_low_latency = dynamic_low_latency;
        resources->output = c->status.output;
        c->input_source = annexb;
        c->input_capacity = capacity;
        q = (struct x9_vcp_start){ .service_id = c->init.service_id,
            .context_id = c->init.context_id,
            .firmware_vsi_address = c->status.init.firmware_vsi_address,
            .bytes_used = used,
            .buffer_bytes = (capacity + VDMA_CPU_BYTES - 1) & ~(VDMA_CPU_BYTES - 1),
            .flags = flags };
        e = x9_vcp_bus_decode_start(c->bus,&c->init,&c->status.init,&q,&c->reverse,
            ms,out,log);
        c->input_source = NULL; /* never retained after return, including error */
        c->status.start = *out; c->status.start_replied = log->ack_matched;
        vcontrol_fault(c,e);
    }
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_discard(struct x9_vdec_control **slot)
{
    struct x9_vdec_control *c;
    struct x9_vcp_bus_status bus;
    int e = 0;
    if (!slot || !(c = *slot) || c->magic != VCONTROL_MAGIC) return EINVAL;
    lck_mtx_lock(&c->lock); c->closing = true;
    if (c->status.init_attempted) {
        e = x9_vcp_bus_get_status(c->bus,&bus);
        if (!e && (bus.epoch != c->status.epoch || bus.phase != X9_VCP_BUS_STOPPED)) e = EBUSY;
        for (unsigned n = 0; n < 2 && !e; n++) {
            struct x9_vdec_dma *d = c->memory->arena[n];
            lck_mtx_lock(&d->lock);
            if (d->state != X9_MM_SMMU_RETIRED) e = EBUSY;
            lck_mtx_unlock(&d->lock);
        }
    }
    for (unsigned n = 0; n < X9_VCP_OUTPUT_PLANES && !e; n++)
        if (c->output_slice[n].token)
            e = x9_vdec_dma_slice_free(c->shared->codec,&c->output_slice[n]);
    if (!e && c->input_slice.token)
        e = x9_vdec_dma_slice_free(c->shared->codec,&c->input_slice);
    if (!e) {
        lck_mtx_lock(&c->shared->lock); lck_mtx_lock(&c->memory->lock);
        if (!c->shared->control_refs || !c->memory->control_refs)
            panic("X9 VDEC: lost control owner reference");
        if (c->status.bound) c->shared->bound_slots &= ~(1u << c->slot.index);
        c->shared->control_refs--; c->memory->control_refs--;
        lck_mtx_unlock(&c->memory->lock); lck_mtx_unlock(&c->shared->lock);
        c->magic = 0; *slot = NULL;
    }
    lck_mtx_unlock(&c->lock);
    if (!e) {
        lck_mtx_destroy(&c->lock,&vdec_group);
        kfree_data(c->scratch,VCONTROL_SCRATCH); kfree_type(struct x9_vdec_control,c);
    }
    return e;
}
