/* Included after native shared/memory owners. No borrowed Linux pointers. */
#include "x9_vdec_control.h"
#include <kern/thread_call.h>
#define VCONTROL_MAGIC UINT64_C(0x5839435452303031)
#define VCONTROL_SCRATCH (2u * X9_VCP_INSTANCE_SLOT_BYTES)
/* Scheduling policy only; not a producer fence, deadline extension or DMA
 * completion inference. Each batch retains the normal <=3s bus deadline. */
#define VCONTROL_SERVICE_IDLE_MS 10u
#define VCONTROL_SERVICE_BUSY_MS 1u
#define VCONTROL_SERVICE_TIMEOUT_MS 3000u
struct vcontrol_input_record {
    struct x9_vdec_control_input_journal journal;
    struct x9_vdec_dma_slice slice;
};
struct vcontrol_output_record {
    struct x9_vdec_control_output_journal journal;
    struct x9_vdec_dma_slice slice[X9_VCP_OUTPUT_PLANES];
    uint32_t bytes[X9_VCP_OUTPUT_PLANES];
    struct x9_vcp_output_request request;
};
struct x9_vdec_control {
    uint64_t magic;
    lck_mtx_t lock;
    struct x9_vcp_bus *bus;
    uint64_t bus_token;
    thread_call_t service_worker;
    struct x9_vdec_shared *shared;
    struct x9_vdec_memory *memory;
    struct x9_vcp_init init;
    struct x9_vcp_instance_slot slot;
    struct x9_vdec_control_hardware hardware;
    struct x9_vcp_bus_reverse_ops reverse;
    struct x9_vdec_control_status status;
    struct vcontrol_input_record *inputs, *active_input;
    struct vcontrol_output_record *outputs, *active_output;
    const uint8_t *input_source; /* borrowed ONLY for the reserved synchronous call */
    const struct x9_vcp_bus_call_log *active_start_call; /* same bounded borrow */
    uint32_t input_capacity;
    uint8_t *scratch;
    struct x9_vcp_query active_query;
    bool query_active, query_cap_staged;
    bool closing, route_registered;
};
static void vcontrol_journal_input(struct x9_vdec_control *c)
{
    struct x9_vdec_control_input_journal *j = &c->active_input->journal;
    j->resources = c->status.resources;
    j->input_prepared = c->status.input_prepared;
    j->vsi_attempted = c->status.vsi_attempted;
    j->vsi_staged = c->status.vsi_staged;
}
static void vcontrol_journal_output(struct x9_vdec_control *c)
{
    c->active_output->journal.resources = c->status.output;
    c->active_output->journal.prepared = c->status.output_prepared;
}
static int vcontrol_fault(struct x9_vdec_control *c, int e)
{
    if (e && !c->status.last_errno) c->status.last_errno = (uint32_t)e;
    return e;
}
/* The caller holds another control lock AND the one bus reservation. Every
 * competing control operation checks that reservation and returns before
 * running hardware callbacks. Never acquire a foreign mutex with an unbounded
 * blocking wait: short status/teardown/pre-reservation work may finish, but
 * no ABBA dependency can wait past this call's absolute deadline. Hardware,
 * bus-owner and DMA callbacks cannot reenter ANY control or wait for it while
 * holding lower locks. Current-caller callbacks do not use these hooks. */
static int vcontrol_route_enter(void *cookie, uint64_t epoch, uint64_t end)
{
    struct x9_vdec_control *c = cookie;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC) return EINVAL;
    for (unsigned n = 0; n < 300000; n++) {
        e = vmem_deadline(end);
        if (e) return e;
        if (lck_mtx_try_lock(&c->lock)) {
            e = epoch != c->status.epoch || !c->route_registered ||
                !c->status.init_attempted ? ESTALE : (int)c->status.last_errno;
            if (!e) e = vmem_deadline(end);
            /* closing is host admission, NOT FW stop. Its route continues
             * to service actual reverse work until the bus is stopped. */
            if (e) lck_mtx_unlock(&c->lock);
            return e;
        }
        delay(10);
    }
    return ETIMEDOUT;
}
static void vcontrol_route_leave(void *cookie, const struct x9_vcp_bus_route_journal *journal)
{
    struct x9_vdec_control *c = cookie;
    /* Bus counter was checked before entering; this subset cannot overflow. */
    c->status.foreign_packets++; c->status.foreign_call = *journal;
    vcontrol_fault(c,(int)journal->last_errno);
    lck_mtx_unlock(&c->lock);
}
static void vcontrol_worker(thread_call_param_t, thread_call_param_t);
/* c->lock held. ONCE defers self-resubmission until the callback returns;
 * closing/error stops future submission before cancel_wait can join it. */
static void vcontrol_arm_worker(struct x9_vdec_control *c, uint32_t ms)
{
    uint64_t at;
    if (c->closing || c->status.last_errno || !c->status.bound ||
        !c->status.slot_initialized || !c->status.worker_enabled) return;
    if (ms) {
        clock_interval_to_deadline(ms,UINT32_C(1000000),&at);
        (void)thread_call_enter_delayed(c->service_worker,at);
    } else (void)thread_call_enter(c->service_worker);
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
/* Current control lock + whole bus reservation. HFRP's captured converter
 * determines an alias DOMAIN, not authority to read firmware/private DRAM.
 * Authority is this retained current DEVICE-owned native NC shared arena. */
static int vcontrol_observe_query(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_query *q, const struct x9_vcp_query_result *reply)
{
    struct x9_vdec_control *c = cookie;
    struct x9_vdec_shared *s = c->shared;
    uint64_t iova, alias64;
    uint32_t offset = 0;
    int e;
    if (!c->query_active || epoch != c->status.epoch || !c->status.bound ||
        q->service_id != c->init.service_id || q->context_id != c->init.context_id ||
        q->instance_token != c->init.instance_token ||
        q->capability_id != c->active_query.capability_id ||
        q->data_token != c->active_query.data_token || reply->firmware_status)
        return ESTALE;
    if (q->capability_id != X9_VCP_FORMAT_CAPABILITY_ID) return 0;
    lck_mtx_lock(&s->lock);
    e = vmem_deadline(end);
    if (!e && (s->magic != VSHARED_MAGIC || s->epoch != epoch ||
        s->global.iova > UINT64_MAX-X9_VCP_GLOBAL_VDEC_OFFSET ||
        s->service.vdec.iova != s->global.iova+X9_VCP_GLOBAL_VDEC_OFFSET ||
        s->service.vdec.bytes != X9_VCP_GLOBAL_VDEC_BYTES)) e = ESTALE;
    iova = s->service.vdec.iova;
    if (!e && (iova < UINT64_C(0x140000000) ||
        iova >= UINT64_C(0x1c0000000) ||
        s->service.vdec.bytes > UINT64_C(0x1c0000000)-iova)) e = ERANGE;
    if (!e) {
        alias64 = UINT64_C(0x30000000)+iova-UINT64_C(0x140000000);
        if (alias64 > UINT32_MAX-X9_VCP_FORMAT_TABLE_OFFSET ||
            reply->firmware_data_address != alias64+X9_VCP_FORMAT_TABLE_OFFSET)
            e = ERANGE;
        if (!e) e = x9_vcp_shared_offset(iova,(uint32_t)alias64,
            s->service.vdec.bytes,reply->firmware_data_address,
            X9_VCP_FORMAT_TABLE_BYTES,&offset);
        if (!e && offset != X9_VCP_FORMAT_TABLE_OFFSET) e = ERANGE;
    }
    for (unsigned n = 0; n < 2 && !e; n++) {
        e = vshared_observe(s,X9_VCP_GLOBAL_VDEC_OFFSET+offset,
            c->scratch+n*X9_VCP_FORMAT_TABLE_BYTES,X9_VCP_FORMAT_TABLE_BYTES);
        if (!e) e = vmem_deadline(end);
    }
    if (!e && memcmp(c->scratch,c->scratch+X9_VCP_FORMAT_TABLE_BYTES,
        X9_VCP_FORMAT_TABLE_BYTES)) e = EAGAIN;
    if (!e) e = x9_vcp_parse_format_snapshot(X9_VCP_FORMAT_ABI_MT6993_CAPTURE1,
        c->scratch+X9_VCP_FORMAT_TABLE_BYTES,X9_VCP_FORMAT_TABLE_BYTES,
        &c->status.formats);
    if (!e) {
        c->query_cap_staged = true;
        c->status.formats_observed = 1;
        c->status.formats_valid = 0; /* call_end still owns final success */
    }
    lck_mtx_unlock(&s->lock);
    return e;
}
/* Status-only exact lookup. Even a unique FRAME entry with a legal raw plane
 * count says nothing about linear/tiled layout, required capacities or DMA.
 * Unknown/unsupported format must not suppress independently stable rings. */
static void vcontrol_picture_format(struct x9_vdec_control *c)
{
    int e;
    c->status.picture_format_matched = 0;
    memset(&c->status.picture_format,0,sizeof(c->status.picture_format));
    if (!c->status.picture_observed) e = EAGAIN;
    else if (!c->status.formats_valid) e = ENXIO;
    else e = x9_vcp_find_frame_format(&c->status.formats,c->status.picture.fourcc,
        &c->status.picture_format);
    c->status.picture_format_errno = (uint32_t)e;
    c->status.picture_format_matched = !e;
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
    if (!e && r->picture_capacity_checked) {
        const struct x9_vcp_picture_snapshot *pic = &r->picture;
        /* Recheck consumed size/selector fields before AP publication. This
         * is only stale-negotiation rejection, not producer exclusion or a
         * promise that an equal picture describes a selected frame. */
        if (vcontrol_load32(p,0xf50) != pic->pic_w ||
            vcontrol_load32(p,0xf54) != pic->pic_h ||
            vcontrol_load32(p,0xf58) != pic->buf_w ||
            vcontrol_load32(p,0xf5c) != pic->buf_h ||
            vcontrol_load32(p,0xf60) != pic->fb_bytes[0] ||
            vcontrol_load32(p,0xf64) != pic->fb_bytes[1] ||
            vcontrol_load32(p,0xf80) != pic->bitdepth ||
            vcontrol_load32(p,0xf84) != pic->layout_mode ||
            vcontrol_load32(p,0xf88) != pic->fourcc) e = ESTALE;
    }
    if (!e) e = vmem_deadline(end);
    if (!e) {
        c->status.vsi_attempted = 1; /* retain before first live store */
        vcontrol_journal_input(c);
        vcontrol_store64(p,0xe80,r->input.iova);
        vcontrol_store64(p,0xe90,0); /* qualified ABSENT attachment, NOT main alias */
        vcontrol_store64(p,0xf18,r->input_token);
        vcontrol_store64(p,0xf48,r->timestamp);
        p[0x1004] = (uint8_t)r->dynamic_low_latency;
        __asm__ volatile("dsb oshst" ::: "memory");
        c->status.vsi_staged = 1;
        vcontrol_journal_input(c);
    }
    lck_mtx_unlock(&s->arena->lock);
    lck_mtx_unlock(&s->lock);
    return e;
}
enum vcontrol_snapshot_kind {
    VCONTROL_COMMON_ONLY, VCONTROL_DECODE_FIELDS, VCONTROL_RING_FIELDS
};
/* Two fresh NC copies, not cached Android bytes. Equal copies do NOT prove
 * firmware-wide serialization: this only observes fields while the bus owns
 * the current command/reverse request. Ring results are never consumption or
 * completion authority. No CPU writer, flag reset or shared index store. */
static int vcontrol_snapshot(struct x9_vdec_control *c, uint64_t end,
    enum vcontrol_snapshot_kind kind, struct x9_vcp_common_snapshot *out)
{
    struct x9_vdec_shared *s = c->shared;
    uint8_t common[2][X9_VCP_COMMON_PREFIX_BYTES];
    int e;
    if (!c->status.bound) return ENXIO;
    lck_mtx_lock(&s->lock);
    e = vmem_deadline(end);
    for (unsigned n = 0; n < 2 && !e; n++) {
        e = vshared_observe(s,X9_VCP_GLOBAL_VDEC_OFFSET,common[n],sizeof(common[n]));
        if (!e && kind != VCONTROL_COMMON_ONLY)
            e = vshared_observe(s,X9_VCP_GLOBAL_VDEC_OFFSET+c->slot.offset,
            c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES,X9_VCP_INSTANCE_SLOT_BYTES);
    }
    if (!e && memcmp(common[0],common[1],sizeof(common[0]))) e = EAGAIN;
    if (!e && kind == VCONTROL_DECODE_FIELDS) {
        /* Negotiation/VP qualification does not consume a ring. Compare
         * ONLY the evidenced decode fields; unrelated asynchronous FB/BS
         * updates must not turn a fresh control preparation into a fault.
         * The whole copies are immutable host data, not an atomic live VSI. */
        struct x9_vcp_decode_snapshot decoded[2];
        for (unsigned n = 0; n < 2 && !e; n++)
            e = x9_vcp_parse_decode_snapshot(X9_VCP_DECODE_ABI_MT6993_CAPTURE1,
                c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES,X9_VCP_INSTANCE_SLOT_BYTES,
                &decoded[n]);
        if (!e && memcmp(&decoded[0],&decoded[1],sizeof(decoded[0]))) e = EAGAIN;
    }
    if (!e) e = x9_vcp_parse_common_snapshot(X9_VCP_COMMON_ABI_MT6993_CAPTURE1,
        common[1],sizeof(common[1]),out);
    if (!e && (out->has_fault || out->enable_smmu != 1)) e = EIO;
    if (!e) e = vmem_deadline(end);
    lck_mtx_unlock(&s->lock);
    return e;
}
/* shared->lock and arena->lock held, after the current physical call check.
 * The live-window check can wait on lower ownership/backing operations, so
 * check the original deadline AGAIN before any live load. A completed copy
 * is journaled even if its subsequent deadline check fails. */
static int vcontrol_postreply_copy(struct x9_vdec_shared *s, uint64_t end,
    uint32_t offset, uint8_t *out, uint32_t bytes, uint32_t *bitmap, uint32_t bit)
{
    volatile uint8_t *p = NULL;
    int e = vmem_deadline(end);
    if (!e) e = vshared_live_window(s,offset,bytes,&p);
    if (!e) e = vmem_deadline(end);
    if (!e) {
        __asm__ volatile("dmb oshld" ::: "memory");
        for (uint32_t n = 0; n < bytes; n++) out[n] = p[n];
        __asm__ volatile("dmb oshld" ::: "memory");
        *bitmap |= bit;
        e = vmem_deadline(end);
    }
    return e;
}
/* Actual status0 START parser branch calls this while c->lock and the whole
 * physical reservation are held. Fresh copies only: scratch from a previous
 * preparation/recheck is never promoted by a failed read. No live store,
 * header admission, negotiated layout or firmware-generation inference. */
static int vcontrol_observe_start_reply(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_init *init, const struct x9_vcp_init_result *initialized,
    const struct x9_vcp_start *q, const struct x9_vcp_start_result *reply,
    const struct x9_vcp_bus_call_log *call)
{
    struct x9_vdec_control *c = cookie;
    struct x9_vdec_shared *s = c->shared;
    struct x9_vdec_control_input_journal *j;
    const struct x9_vdec_control_start_resources *r;
    uint32_t state, observation_errno = 0;
    int e;
    if (!c->active_input || !c->status.input_records ||
        c->status.input_records > X9_VDEC_CONTROL_MAX_RECORDS ||
        c->active_input != &c->inputs[c->status.input_records-1]) return ESTALE;
    j = &c->active_input->journal; r = &j->resources;
    if (j->postreply_attempted) return EALREADY;
    j->postreply_attempted = 1;
    e = vmem_deadline(end);
    if (!e && (call != c->active_start_call || !call || call->epoch != epoch ||
        !call->request_attempted || !call->prepared || !call->ack_matched ||
        epoch != c->status.epoch || epoch != s->epoch || epoch != r->epoch ||
        !c->status.bound || !c->status.slot_initialized ||
        !c->status.start_attempted || c->status.start_replied ||
        !c->status.input_prepared || !c->status.vsi_staged ||
        !j->input_prepared || !j->vsi_staged || reply->firmware_status ||
        init->service_id != c->init.service_id || init->context_id != c->init.context_id ||
        init->instance_token != c->init.instance_token ||
        r->instance_token != c->init.instance_token ||
        initialized->firmware_status || c->status.init.firmware_status ||
        initialized->firmware_vsi_address != c->status.init.firmware_vsi_address ||
        q->firmware_vsi_address != initialized->firmware_vsi_address ||
        q->service_id != init->service_id || q->context_id != init->context_id ||
        q->service_id != j->request.service_id || q->context_id != j->request.context_id ||
        q->firmware_vsi_address != j->request.firmware_vsi_address ||
        q->bytes_used != j->request.bytes_used || q->buffer_bytes != j->request.buffer_bytes ||
        q->flags != j->request.flags || q->bytes_used != r->bytes_used ||
        r->submission_index != c->status.input_records-1 ||
        r->slot.index != c->slot.index || r->slot.offset != c->slot.offset ||
        r->slot.bytes != c->slot.bytes || c->slot.index >= X9_VCP_INSTANCE_SLOT_COUNT ||
        c->slot.bytes != X9_VCP_INSTANCE_SLOT_BYTES ||
        c->slot.offset != X9_VCP_COMMON_PREFIX_BYTES +
            c->slot.index*X9_VCP_INSTANCE_SLOT_BYTES ||
        !r->input_token || r->input_token != c->status.resources.input_token ||
        r->input_slice_token != c->active_input->slice.token ||
        c->active_input->slice.epoch != epoch ||
        q->buffer_bytes != c->active_input->slice.capacity ||
        r->input.capacity != c->active_input->slice.capacity ||
        r->input.bytes != c->active_input->slice.bytes ||
        r->input.iova != c->status.resources.input.iova)) e = ESTALE;
    if (!e) e = x9_vdec_dma_slice_state(s->codec,&c->active_input->slice,&state);
    if (!e && state != X9_VDEC_DMA_DEVICE) e = EBUSY;
    if (e) { j->postreply_errno = (uint32_t)e; return e; }
    lck_mtx_lock(&s->lock);
    lck_mtx_lock(&s->arena->lock);
    e = vmem_deadline(end);
    if (!e && !(s->bound_slots & (1u << c->slot.index))) e = ESTALE;
    for (unsigned n = 0; n < 2 && !e; n++) {
        struct x9_vdec_control_postreply_snapshot *p = &j->postreply[n];
        uint8_t *slot = c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES;
        uint32_t bit = 1u << n;
        e = vcontrol_postreply_copy(s,end,X9_VCP_GLOBAL_VDEC_OFFSET,
            p->common_raw,sizeof(p->common_raw),&j->postreply_common_bitmap,bit);
        if (!e) e = vcontrol_postreply_copy(s,end,
            X9_VCP_GLOBAL_VDEC_OFFSET+c->slot.offset,slot,X9_VCP_INSTANCE_SLOT_BYTES,
            &j->postreply_sample_bitmap,bit);
        if (j->postreply_sample_bitmap & bit) {
            /* These loads are ONLY from this completed immutable host copy.
             * Preserve raw values before any parser may reject the sample. */
            p->raw_1364 = vcontrol_load32(slot,0x1364);
            p->raw_1374 = vcontrol_load32(slot,0x1374);
            p->raw_plane_count = vcontrol_load32(slot,0xf28);
            p->raw_input_driven = slot[0x1000];
            p->raw_bitdepth = vcontrol_load32(slot,0xf80);
            memcpy(p->decode_raw.addresses,slot+0xe7c,sizeof(p->decode_raw.addresses));
            memcpy(p->decode_raw.tokens_errors,slot+0xf18,sizeof(p->decode_raw.tokens_errors));
            memcpy(p->decode_raw.timestamp,slot+0xf48,sizeof(p->decode_raw.timestamp));
            memcpy(p->decode_raw.modes,slot+0x1000,sizeof(p->decode_raw.modes));
            memcpy(p->decode_raw.bitdepth,slot+0xf80,sizeof(p->decode_raw.bitdepth));
            memcpy(p->decode_raw.general,slot+0x1208,sizeof(p->decode_raw.general));
            memcpy(p->decode_raw.blocked,slot+0x1228,sizeof(p->decode_raw.blocked));
            memcpy(p->picture_raw,slot+0xf50,sizeof(p->picture_raw));
            memcpy(p->crop_raw,slot+0xfd8,sizeof(p->crop_raw));
            memcpy(p->dpb_raw,slot+0xe78,sizeof(p->dpb_raw));
            p->decode_errno = (uint32_t)x9_vcp_parse_decode_snapshot(
                X9_VCP_DECODE_ABI_MT6993_CAPTURE1,slot,X9_VCP_INSTANCE_SLOT_BYTES,&p->decode);
            p->picture_errno = (uint32_t)x9_vcp_parse_picture_snapshot(
                X9_VCP_PICTURE_ABI_MT6993_CAPTURE1,slot,X9_VCP_INSTANCE_SLOT_BYTES,&p->picture);
        }
    }
    lck_mtx_unlock(&s->arena->lock);
    lck_mtx_unlock(&s->lock);
    for (unsigned n = 0; n < 2; n++) {
        struct x9_vdec_control_postreply_snapshot *p = &j->postreply[n];
        uint32_t bit = 1u << n;
        if (j->postreply_common_bitmap & bit) {
            p->common_errno = (uint32_t)x9_vcp_parse_common_snapshot(
                X9_VCP_COMMON_ABI_MT6993_CAPTURE1,p->common_raw,sizeof(p->common_raw),&p->common);
            /* These are the same real common owner/fault checks consumed by
             * START preparation, not a new post-only PIC readiness gate. */
            if (!e && p->common_errno) e = (int)p->common_errno;
            if (!e && (p->common.has_fault || p->common.enable_smmu != 1)) e = EIO;
        }
        if ((j->postreply_sample_bitmap & bit) && !observation_errno)
            observation_errno = p->decode_errno ? p->decode_errno : p->picture_errno;
        if ((j->postreply_common_bitmap & j->postreply_sample_bitmap & bit) &&
            !p->common_errno && !p->decode_errno && !p->picture_errno)
            j->postreply_parsed_bitmap |= bit;
    }
    if (!e) e = vmem_deadline(end);
    if (!e && j->postreply_parsed_bitmap == 3) {
        const struct x9_vdec_control_postreply_snapshot *a = &j->postreply[0], *b = &j->postreply[1];
        if (memcmp(&a->common,&b->common,sizeof(a->common)) ||
            memcmp(&a->decode,&b->decode,sizeof(a->decode)) ||
            memcmp(&a->picture,&b->picture,sizeof(a->picture)) ||
            a->raw_1364 != b->raw_1364 || a->raw_1374 != b->raw_1374)
            observation_errno = EAGAIN;
        else j->postreply_observed = 1;
    }
    j->postreply_errno = e ? (uint32_t)e : observation_errno;
    /* Changing/post-only unparseable fields merely make the observation
     * unavailable; never add a normal START admission/completion gate. Real
     * handle/window/power/epoch/deadline errors retain the existing fault. */
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
    e = vcontrol_snapshot(c,end,VCONTROL_DECODE_FIELDS,&common);
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
static int vcontrol_match_ring(struct x9_vdec_control *c,
    const struct x9_vcp_ring_snapshot *ring)
{
    if (!ring->entry_present) return 0;
    /* Exact native identity lookup, never a cast of an FW token to a pointer.
     * A matched observation is durable bookkeeping ONLY. It cannot return a
     * DEVICE slice to CPU ownership, release references or produce pixels. */
    if (ring->kind == X9_VCP_RING_FREE_BS) {
        for (unsigned i = 0; i < c->status.input_records; i++) {
            struct x9_vdec_control_input_journal *j = &c->inputs[i].journal;
            if (j->input_prepared && j->resources.input_token == ring->buffer_token) {
                j->free_bs = *ring; j->free_bs_observed = 1; return 0;
            }
        }
    } else {
        for (unsigned i = 0; i < c->status.output_records; i++) {
            struct x9_vdec_control_output_journal *j = &c->outputs[i].journal;
            if (j->prepared && j->resources.buffer_token == ring->buffer_token) {
                if (ring->kind == X9_VCP_RING_FREE_FB) {
                    j->free_fb = *ring; j->free_fb_observed = 1;
                } else {
                    j->display_fb = *ring; j->display_fb_observed = 1;
                }
                return 0;
            }
        }
    }
    return ESTALE;
}
/* Each ring is sampled twice from immutable NC host copies. Only THAT ring's
 * parsed metadata/head need agree: a BS publication or unrelated VSI field
 * must not suppress an otherwise stable FB observation. The ring parser
 * zeroes structure padding, so comparison is deterministic. This is still
 * just a peek; neither equality nor known tokens license read/count stores,
 * DMA reclamation or pixels. No attempt to repair a transient count. */
static int vcontrol_ring_snapshot(struct x9_vdec_control *c, unsigned kind,
    struct x9_vcp_ring_snapshot *out)
{
    struct x9_vcp_ring_snapshot rings[2];
    bool pending = false;
    int e = 0;
    for (unsigned n = 0; n < 2 && !e; n++) {
        e = x9_vcp_parse_ring_snapshot(X9_VCP_RING_ABI_MT6993_CAPTURE1,kind,
            c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES,X9_VCP_INSTANCE_SLOT_BYTES,
            &rings[n]);
        /* All arguments above are fixed and valid: EINVAL is precisely the
         * captured index/count publication transient. Structural corruption
         * (ERANGE/invalid FB full) still fails closed, even in one sample. */
        if (e == EINVAL) { pending = true; e = 0; }
    }
    if (!e && (pending || memcmp(&rings[0],&rings[1],sizeof(rings[0])))) e = EAGAIN;
    if (!e) *out = rings[1];
    return e;
}
static int vcontrol_recheck(struct x9_vdec_control *c, uint64_t end)
{
    struct x9_vcp_common_snapshot common;
    bool pending = false;
    int e = vcontrol_snapshot(c,end,VCONTROL_RING_FIELDS,&common);
    c->status.ring_valid_bitmap = 0;
    c->status.picture_observed = 0;
    if (!e) {
        struct x9_vcp_picture_snapshot picture[2];
        for (unsigned n = 0; n < 2 && !e; n++)
            e = x9_vcp_parse_picture_snapshot(X9_VCP_PICTURE_ABI_MT6993_CAPTURE1,
                c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES,X9_VCP_INSTANCE_SLOT_BYTES,
                &picture[n]);
        /* Equal independent NC copies reject a visible update, but are not
         * a firmware seqlock, matched frame or valid negotiation. Even zero
         * and unknown picture/crop/DPB fields remain raw observations. Keep
         * rechecking a changing picture without suppressing stable rings. */
        if (!e && memcmp(&picture[0],&picture[1],sizeof(picture[0])))
            pending = true;
        else if (!e) {
            c->status.picture = picture[1];
            c->status.picture_observed = 1;
        }
    }
    vcontrol_picture_format(c);
    for (unsigned kind = 0; kind < 3 && !e; kind++) {
        struct x9_vcp_ring_snapshot ring;
        e = vcontrol_ring_snapshot(c,kind,&ring);
        if (e == EAGAIN) { pending = true; e = 0; continue; }
        if (!e) {
            c->status.rings[kind] = ring;
            c->status.ring_valid_bitmap |= 1u << kind;
            e = vcontrol_match_ring(c,&ring);
        }
    }
    if (!e) e = vmem_deadline(end);
    if (!e) {
        c->status.recheck_pending = pending;
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
    c->status.ring_valid_bitmap = 0;
    c->status.picture_observed = 0;
    vcontrol_picture_format(c);
    memset(c->status.rings,0,sizeof(c->status.rings));
    if (q->firmware_status) return EIO;
    /* During INIT there is no bound VSI yet: retain the actual event until
     * a subsequent reverse notification or reserved START preparation. */
    return c->status.bound ? vcontrol_recheck(c,end) : 0;
}
/* Native observer, not a synthetic notification or a completion callback.
 * c->lock and the physical bus reservation remain held by service(). */
static int vcontrol_service_recheck(void *cookie, uint64_t epoch, uint64_t end)
{
    struct x9_vdec_control *c = cookie;
    int e = epoch == c->status.epoch && c->status.bound ? 0 : ESTALE;
    if (!e) e = vmem_deadline(end);
    if (!e && c->status.recheck_pending) e = vcontrol_recheck(c,end);
    return e;
}
/* Captured FW30ad94..ade2 publishes total or separate capacities into PIC
 * fb_sz0/fb_sz1; vendor plane-selection/packing remains unresolved. Compare
 * only their 64-bit SUM with each retained output's owned aggregate. Being
 * large enough cannot prove Y/C partition, offsets, packing, stride, headers,
 * DPB scratch or hardware write layout. This only rejects an insufficient
 * candidate before exposing a new input or sending a001. */
static int vcontrol_output_capacity(struct x9_vdec_control *c, uint64_t end)
{
    struct x9_vdec_control_start_resources *resources = &c->status.resources;
    struct x9_vcp_common_snapshot common;
    struct x9_vcp_picture_snapshot pic[2];
    uint64_t required;
    int e = vcontrol_snapshot(c,end,VCONTROL_DECODE_FIELDS,&common);
    for (unsigned n = 0; n < 2 && !e; n++)
        e = x9_vcp_parse_picture_snapshot(X9_VCP_PICTURE_ABI_MT6993_CAPTURE1,
            c->scratch+n*X9_VCP_INSTANCE_SLOT_BYTES,X9_VCP_INSTANCE_SLOT_BYTES,&pic[n]);
    resources->picture_capacity_checked = 0;
    if (e) return e;
    /* Only consumed fields need agree. Unused array entries/crop/DPB may be
     * stale or update independently; do not infer a plane count from them. */
    if (pic[0].pic_w != pic[1].pic_w || pic[0].pic_h != pic[1].pic_h ||
        pic[0].buf_w != pic[1].buf_w || pic[0].buf_h != pic[1].buf_h ||
        pic[0].fb_bytes[0] != pic[1].fb_bytes[0] ||
        pic[0].fb_bytes[1] != pic[1].fb_bytes[1] ||
        pic[0].bitdepth != pic[1].bitdepth ||
        pic[0].layout_mode != pic[1].layout_mode ||
        pic[0].fourcc != pic[1].fourcc) return EAGAIN;
    resources->picture = pic[1];
    required = (uint64_t)pic[1].fb_bytes[0]+pic[1].fb_bytes[1];
    resources->picture_required_bytes = required;
    vcontrol_journal_input(c);
    /* Initial header: no published sizes and NO capacity assertion. The real
     * owner still has to qualify START. Reject partial dimensions instead of
     * treating them as valid zero-byte output or inventing sizes by fourcc. */
    if (!pic[1].pic_w && !pic[1].pic_h && !pic[1].buf_w &&
        !pic[1].buf_h && !required) return 0;
    if (!pic[1].pic_w || !pic[1].pic_h || !pic[1].buf_w ||
        !pic[1].buf_h || !required || pic[1].pic_w > pic[1].buf_w ||
        pic[1].pic_h > pic[1].buf_h) return EINVAL;
    /* RV32 producer MUL buf_w*buf_h: reject wrapped products, without claiming
     * all further vendor packing/rounding operations are overflow-free. */
    if ((uint64_t)pic[1].buf_w*pic[1].buf_h > UINT32_MAX) return EOVERFLOW;
    if (!c->status.output_records) return ENXIO;
    for (unsigned i = 0; i < c->status.output_records; i++) {
        const struct x9_vcp_output_record *r = &c->outputs[i].journal.resources;
        uint64_t total = 0;
        if (!c->outputs[i].journal.prepared || !r->plane_count ||
            r->plane_count > X9_VCP_OUTPUT_PLANES) return EBUSY;
        for (unsigned n = 0; n < r->plane_count; n++) {
            if (!r->plane[n].iova || !r->plane[n].bytes) return ESTALE;
            if (r->plane[n].bytes > UINT64_MAX-total) return EOVERFLOW;
            total += r->plane[n].bytes;
        }
        if (total < required) return ENOSPC;
    }
    e = vmem_deadline(end);
    if (!e) {
        resources->picture_capacity_checked = 1;
        vcontrol_journal_input(c);
    }
    return e;
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
        !c->active_input || !c->input_source || c->active_input->slice.token ||
        !c->status.output_prepared ||
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
        for (unsigned i = 0; i < c->status.output_records && !e; i++)
            for (unsigned n = 0; n < c->outputs[i].journal.resources.plane_count && !e; n++) {
                uint32_t state;
                e = x9_vdec_dma_slice_state(c->shared->codec,&c->outputs[i].slice[n],&state);
                if (!e && state != X9_VDEC_DMA_DEVICE) e = EBUSY;
            }
    }
    /* Previous b001 permits another CONTROL request, not storage reuse.
     * Revalidate every earlier input's retained DEVICE handle as well. */
    for (unsigned i = 0; i+1 < c->status.input_records && !e; i++) {
        uint32_t state;
        e = x9_vdec_dma_slice_state(c->shared->codec,&c->inputs[i].slice,&state);
        if (!e && state != X9_VDEC_DMA_DEVICE) e = EBUSY;
    }
    if (!e) e = vcontrol_output_capacity(c,end);
    if (!e) e = vmem_deadline(end);
    if (!e) {
        resources->input_token = vdma_take_token();
        if (!resources->input_token) e = EOVERFLOW;
        else vcontrol_journal_input(c);
    }
    if (!e) e = x9_vdec_dma_slice_alloc(c->shared->codec,epoch,
        c->input_capacity,128,&c->active_input->slice);
    if (!e) {
        resources->input_slice_token = c->active_input->slice.token;
        vcontrol_journal_input(c);
        /* Initial wire capacity used the native allocator's exact rounding.
         * Verify the actual handle before either copying or exposing input. */
        if (q->buffer_bytes != c->active_input->slice.capacity) e = ESTALE;
    }
    if (!e) e = vmem_deadline(end);
    if (!e) e = x9_vdec_dma_slice_write(c->shared->codec,&c->active_input->slice,
        0,c->input_source,resources->bytes_used);
    if (!e) e = vmem_deadline(end);
    if (!e) e = x9_vdec_dma_slice_state(c->shared->codec,&c->active_input->slice,&input_state);
    if (!e && input_state != X9_VDEC_DMA_CPU_PRIVATE) e = EBUSY;
    if (!e) e = x9_vdec_dma_slice_publish(c->shared->codec,&c->active_input->slice,&extent);
    if (!e) {
        /* Record the real exposure BEFORE any subsequent deadline/owner error.
         * A failed VSI preparation must not lose or recycle DEVICE backing. */
        resources->input = extent;
        c->status.input_prepared = 1;
        vcontrol_journal_input(c);
    }
    if (!e) e = vmem_deadline(end);
    if (!e) e = vcontrol_snapshot(c,end,VCONTROL_DECODE_FIELDS,&common);
    if (!e) e = x9_vcp_parse_decode_snapshot(X9_VCP_DECODE_ABI_MT6993_CAPTURE1,
        c->scratch,X9_VCP_INSTANCE_SLOT_BYTES,&decoded);
    /* plane_count is AP-published f28: this only rejects an AP record
     * mismatch. It does not qualify FW-derived1374 or its current layout. */
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
/* Called only by prepare_output within its physical whole-call reservation,
 * after every actual output handoff has been journaled. Captured enum5's
 * 26c00..26c18 case stores the AP input u32 to VSI+f28 (store at26c10).
 * This is that AP producer, not a write to FW1364/1374/mode/depth or evidence
 * that firmware has consumed/refreshed its derived count before a005. */
static int vcontrol_stage_output_planes(struct x9_vdec_control *c,
    uint64_t epoch, uint64_t end)
{
    struct x9_vdec_shared *s = c->shared;
    struct x9_vcp_output_record *r = &c->status.output;
    struct vcontrol_output_record *o = c->active_output;
    volatile uint8_t *p = NULL;
    int e;
    if (epoch != c->status.epoch || !o || !c->status.output_attempted ||
        c->status.output_prepared || o->journal.ap_plane_count_store_attempted ||
        r->generation != epoch || r->instance_token != c->init.instance_token ||
        r->context_id != c->init.context_id ||
        r->firmware_vsi_address != c->status.init.firmware_vsi_address ||
        r->buffer_index >= c->status.output_records ||
        r->buffer_index >= X9_VDEC_CONTROL_MAX_RECORDS ||
        o != &c->outputs[r->buffer_index] ||
        !r->plane_count || r->plane_count > X9_VCP_OUTPUT_PLANES) return ESTALE;
    for (unsigned n = 0; n < r->plane_count; n++) {
        uint32_t state;
        if (o->slice[n].epoch != epoch) return ESTALE;
        e = x9_vdec_dma_slice_state(s->codec,&o->slice[n],&state);
        if (e) return e;
        if (state != X9_VDEC_DMA_DEVICE) return EBUSY;
        if (!r->plane[n].iova || r->plane[n].bytes != o->slice[n].capacity)
            return ESTALE;
    }
    lck_mtx_lock(&s->lock);
    lck_mtx_lock(&s->arena->lock);
    e = epoch != s->epoch || epoch != s->arena->request.epoch ? ESTALE : 0;
    if (!e && (!c->status.bound || !c->status.slot_initialized ||
        c->slot.index >= X9_VCP_INSTANCE_SLOT_COUNT)) e = EBUSY;
    if (!e && (!(s->bound_slots & (1u << c->slot.index)) ||
        c->slot.bytes != X9_VCP_INSTANCE_SLOT_BYTES ||
        c->slot.offset != X9_VCP_COMMON_PREFIX_BYTES +
            c->slot.index * X9_VCP_INSTANCE_SLOT_BYTES)) e = ESTALE;
    if (!e && (c->slot.offset & 7)) e = EFAULT;
    if (!e) e = vmem_deadline(end);
    /* Revalidates the retained global DEVICE slice, live power pin,
     * Normal-NC MAIR/pmap attributes and every backing PA before this store. */
    if (!e) e = vshared_live_window(s,X9_VCP_GLOBAL_VDEC_OFFSET+c->slot.offset,
        X9_VCP_INSTANCE_SLOT_BYTES,&p);
    if (!e) e = vmem_deadline(end);
    if (!e) {
        o->journal.ap_plane_count_store_attempted = 1;
        vcontrol_store32(p,0xf28,r->plane_count);
        __asm__ volatile("dsb oshst" ::: "memory");
        o->journal.ap_plane_count_readback = vcontrol_load32(p,0xf28);
        if (o->journal.ap_plane_count_readback != r->plane_count) e = EIO;
        else o->journal.ap_plane_count_store_observed = 1;
        /* A later failure retains both this AP-store journal and all DEVICE
         * handles. Never roll back a live store or grant FW/DMA readiness. */
        if (!e) e = vmem_deadline(end);
    }
    lck_mtx_unlock(&s->arena->lock);
    lck_mtx_unlock(&s->lock);
    return e;
}
static int vcontrol_prepare_output(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_init *init, const struct x9_vcp_init_result *ready,
    struct x9_vcp_output_request *q, struct x9_vcp_output_record *record)
{
    struct x9_vdec_control *c = cookie;
    struct x9_vcp_common_snapshot common;
    struct x9_vcp_output_record *r = &c->status.output;
    struct vcontrol_output_record *o = c->active_output;
    int e;
    if (epoch != c->status.epoch || !c->status.output_attempted ||
        !o || c->status.output_prepared ||
        init->instance_token != c->init.instance_token ||
        ready->firmware_vsi_address != c->status.init.firmware_vsi_address)
        return ESTALE;
    e = vcontrol_snapshot(c,end,VCONTROL_COMMON_ONLY,&common);
    for (unsigned n = 0; n < r->plane_count && !e; n++) {
        e = vmem_deadline(end);
        if (!e) e = x9_vdec_dma_slice_alloc(c->shared->codec,epoch,
            o->bytes[n],128,&o->slice[n]);
    }
    /* Obtain/journal ALL backing before exposing Y: C capacity failure
     * must not unnecessarily turn a still-private Y slice into DEVICE. */
    for (unsigned n = 0; n < r->plane_count && !e; n++) {
        struct x9_vdec_dma_extent extent;
        if (!e) e = vmem_deadline(end);
        if (!e) e = x9_vdec_dma_slice_publish(c->shared->codec,&o->slice[n],&extent);
        if (!e) {
            /* Journal EACH real handoff immediately, including padding.
             * Partial Y success/C failure never loses Y's DEVICE handle. */
            r->plane[n] = (struct x9_vcp_output_extent){extent.iova,extent.capacity};
            if (n) o->request.c_iova = extent.iova;
            else o->request.y_iova = extent.iova;
            vcontrol_journal_output(c);
        }
    }
    if (!e) e = vmem_deadline(end);
    if (!e) e = vcontrol_stage_output_planes(c,epoch,end);
    if (!e) {
        c->status.output_prepared = 1;
        vcontrol_journal_output(c);
        *record = *r; *q = o->request;
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
    c->inputs = kalloc_type(struct vcontrol_input_record,X9_VDEC_CONTROL_MAX_RECORDS,
        Z_WAITOK | Z_ZERO);
    c->outputs = kalloc_type(struct vcontrol_output_record,X9_VDEC_CONTROL_MAX_RECORDS,
        Z_WAITOK | Z_ZERO);
    /* Not HIGH/default or SIGNAL/interrupt context: hardware callbacks may
     * block within their existing absolute deadline. Never parallel callbacks. */
    c->service_worker = thread_call_allocate_with_options(vcontrol_worker,c,
        THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
    if (!c->scratch || !c->inputs || !c->outputs || !c->service_worker) {
        if (c->scratch) kfree_data(c->scratch,VCONTROL_SCRATCH);
        if (c->inputs) kfree_type(struct vcontrol_input_record,X9_VDEC_CONTROL_MAX_RECORDS,c->inputs);
        if (c->outputs) kfree_type(struct vcontrol_output_record,X9_VDEC_CONTROL_MAX_RECORDS,c->outputs);
        if (c->service_worker && !thread_call_free(c->service_worker))
            panic("X9 VDEC: pending unpublished service worker");
        kfree_type(struct x9_vdec_control,c); return ENOMEM;
    }
    e = x9_vcp_bus_retain_shared(bus,s->epoch,&s->global,&c->bus_token);
    if (e) goto fail;
    lck_mtx_lock(&s->lock); lck_mtx_lock(&m->lock);
    e = s->closing || !s->published || m->closing || m->control_refs ? EBUSY :
        s->fault_errno ? s->fault_errno : m->fault_errno;
    if (!e && (m->epoch != s->epoch || m->arena[0] != s->arena ||
        m->arena[1] != s->codec)) e = ESTALE;
    if (!e && s->control_refs == UINT32_MAX) e = EOVERFLOW;
    if (!e) { s->control_refs++; m->control_refs++; c->init = m->init; }
    lck_mtx_unlock(&m->lock); lck_mtx_unlock(&s->lock);
    if (e) goto fail;
    c->magic = VCONTROL_MAGIC; c->bus = bus; c->shared = s; c->memory = m;
    c->hardware = *hardware; c->status.epoch = s->epoch;
    c->reverse = (struct x9_vcp_bus_reverse_ops){ .cookie = c,
        .route_enter = vcontrol_route_enter, .route_leave = vcontrol_route_leave,
        .shm = vcontrol_shm, .alloc = vcontrol_alloc, .clock = vcontrol_clock,
        .wait = vcontrol_wait, .vp = vcontrol_vp, .notification = vcontrol_notification,
        .observe_start_reply = vcontrol_observe_start_reply,
        .observe_query_reply = vcontrol_observe_query,
        .recheck = vcontrol_service_recheck,
        .prepare_output = vcontrol_prepare_output };
    if (hardware->qualify_start) c->reverse.prepare_start = vcontrol_prepare_start;
    lck_mtx_init(&c->lock,&vdec_group,LCK_ATTR_NULL);
    /* Everything reachable from a registry cookie is now initialized. No
     * worker is armed, and the unexposed route cannot receive foreign work. */
    lck_mtx_lock(&c->lock);
    e = x9_vcp_bus_register_instance(bus,c->status.epoch,c->bus_token,&c->init,&c->reverse);
    if (!e) { c->route_registered = true; *out = c; }
    lck_mtx_unlock(&c->lock);
    if (e) {
        lck_mtx_lock(&s->lock); lck_mtx_lock(&m->lock);
        if (!s->control_refs || !m->control_refs)
            panic("X9 VDEC: lost unpublished control references");
        s->control_refs--; m->control_refs--;
        lck_mtx_unlock(&m->lock); lck_mtx_unlock(&s->lock);
        c->magic = 0; lck_mtx_destroy(&c->lock,&vdec_group);
        goto fail;
    }
    return 0;
fail:
    if (c->bus_token && x9_vcp_bus_release_shared(bus,s->epoch,&c->bus_token))
        panic("X9 VDEC: lost unpublished bus lifetime hold");
    if (!thread_call_free(c->service_worker))
        panic("X9 VDEC: pending unpublished service worker");
    kfree_type(struct vcontrol_input_record,X9_VDEC_CONTROL_MAX_RECORDS,c->inputs);
    kfree_type(struct vcontrol_output_record,X9_VDEC_CONTROL_MAX_RECORDS,c->outputs);
    kfree_data(c->scratch,VCONTROL_SCRATCH); kfree_type(struct x9_vdec_control,c);
    return e;
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
        if (e == EBUSY && !log->epoch) {
            /* A competing instance may reserve between check_shared and
             * call_begin. No request/callback ran, so INIT remains fresh. */
            c->status.init_attempted = 0;
            lck_mtx_unlock(&c->lock); return e;
        }
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
        if (!e) {
            c->status.worker_enabled = 1;
            vcontrol_arm_worker(c,0);
        }
    }
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_query(struct x9_vdec_control *c, const struct x9_vcp_query *q,
    uint32_t ms, struct x9_vcp_query_result *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_query query;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !q || !out || !log) return EINVAL;
    query = *q;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    if (!ms || ms > 3000) return EINVAL;
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || c->status.start_attempted ||
        c->status.reset_attempted ? EBUSY :
        (int)c->status.last_errno;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) {
        c->active_query = query;
        c->query_active = true; c->query_cap_staged = false;
        e = x9_vcp_bus_query(c->bus,&c->init,&query,&c->reverse,ms,out,log);
        c->query_active = false;
        /* A pre-reservation refusal has not attempted this query; retain any
         * prior valid observation. Every reserved cap0 transaction replaces
         * validity, including routed-baseline/callback/call_end failures. */
        if (log->epoch && query.capability_id == X9_VCP_FORMAT_CAPABILITY_ID) {
            c->status.format_query = query;
            c->status.format_reply = *out;
            c->status.format_call = *log;
            c->status.formats_observed = c->query_cap_staged;
            c->status.formats_valid = !e && log->ack_matched && c->query_cap_staged;
            c->status.formats_last_errno = (uint32_t)e;
            vcontrol_picture_format(c);
        }
    }
    if (log->epoch) vcontrol_fault(c,e); /* includes a failed routed RX baseline */
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_decode_reset(struct x9_vdec_control *c, uint32_t drain_type,
    uint32_t ms, struct x9_vcp_reset_result *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_reset q;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !out || !log) return EINVAL;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    if (!ms || ms > 3000 || drain_type > 2) return EINVAL;
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || !c->status.slot_initialized ||
        c->status.reset_attempted ||
        (c->status.start_attempted && !c->status.start_replied) ? EBUSY :
        (int)c->status.last_errno;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) {
        q = (struct x9_vcp_reset){ .service_id = c->init.service_id,
            .context_id = c->init.context_id,
            .firmware_vsi_alias = c->status.init.firmware_vsi_address,
            .drain_type = drain_type };
        c->status.reset_attempted = 1; c->status.reset_drain_type = drain_type;
        e = x9_vcp_bus_decode_reset(c->bus,&c->init,&c->status.init,&q,
            &c->reverse,ms,out,log);
        if (e == EBUSY && !log->epoch) {
            /* No reservation, send, callbacks or state exposure. The one-shot
             * reset slot may still be used; no DMA record was touched. */
            c->status.reset_attempted = 0; c->status.reset_drain_type = 0;
            lck_mtx_unlock(&c->lock); return e;
        }
        c->status.reset = *out; c->status.reset_call = *log;
        c->status.reset_replied = log->ack_matched;
        vcontrol_fault(c,e);
    }
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_get_status(struct x9_vdec_control *c, struct x9_vdec_control_status *out)
{
    if (!c || c->magic != VCONTROL_MAGIC || !out) return EINVAL;
    lck_mtx_lock(&c->lock); *out = c->status; lck_mtx_unlock(&c->lock);
    return 0;
}
int x9_vdec_control_get_route(struct x9_vdec_control *c, struct x9_vcp_bus_instance_status *out)
{
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !out) return EINVAL;
    lck_mtx_lock(&c->lock);
    e = x9_vcp_bus_get_instance_status(c->bus,c->status.epoch,c->bus_token,out);
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_get_input(struct x9_vdec_control *c, uint32_t index,
    struct x9_vdec_control_input_journal *out)
{
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !out) return EINVAL;
    memset(out,0,sizeof(*out));
    lck_mtx_lock(&c->lock);
    e = index >= c->status.input_records ? ENOENT : 0;
    if (!e) *out = c->inputs[index].journal;
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_get_output(struct x9_vdec_control *c, uint32_t index,
    struct x9_vdec_control_output_journal *out)
{
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !out) return EINVAL;
    memset(out,0,sizeof(*out));
    lck_mtx_lock(&c->lock);
    e = index >= c->status.output_records ? ENOENT : 0;
    if (!e) *out = c->outputs[index].journal;
    lck_mtx_unlock(&c->lock);
    return e;
}
int x9_vdec_control_register_output(struct x9_vdec_control *c, uint32_t planes,
    const uint32_t bytes[X9_VCP_OUTPUT_PLANES], uint32_t ms,
    struct x9_vcp_output_notification *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_output_record *r;
    struct x9_vcp_output_record previous_output;
    uint32_t previous_attempted, previous_prepared, previous_notified;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !bytes || !out || !log) return EINVAL;
    if (!ms || ms > 3000 || !planes || planes > X9_VCP_OUTPUT_PLANES ||
        !bytes[0] || (planes == 1 ? bytes[1] != 0 : !bytes[1])) return EINVAL;
    for (unsigned n = 0; n < planes; n++)
        if (bytes[n] > X9_MM_SMMU_MAX_CHECK_BYTES) return EMSGSIZE;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || c->status.reset_attempted ? EBUSY :
        (int)c->status.last_errno;
    if (!e && c->status.start_attempted && !c->status.start_replied) e = EBUSY;
    if (!e && c->status.output_records == X9_VDEC_CONTROL_MAX_RECORDS) e = ENOSPC;
    if (!e && c->status.output_records) {
        const struct vcontrol_output_record *first = &c->outputs[0];
        if (planes != first->journal.resources.plane_count ||
            memcmp(bytes,first->bytes,sizeof(first->bytes))) e = ENOTSUP;
    }
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) {
        previous_output = c->status.output;
        previous_attempted = c->status.output_attempted;
        previous_prepared = c->status.output_prepared;
        previous_notified = c->status.output_notified;
        c->status.output_attempted = 1;
        c->status.output_prepared = 0; c->status.output_notified = 0;
        c->active_output = &c->outputs[c->status.output_records++];
        r = &c->status.output;
        memset(r,0,sizeof(*r));
        r->buffer_token = vdma_take_token();
        if (!r->buffer_token) e = EOVERFLOW;
        r->instance_token = c->init.instance_token; r->context_id = c->init.context_id;
        r->generation = c->status.epoch; r->plane_count = planes;
        r->buffer_index = c->status.output_records-1;
        r->firmware_vsi_address = c->status.init.firmware_vsi_address;
        c->active_output->request = (struct x9_vcp_output_request){
            .firmware_vsi_address = r->firmware_vsi_address, .generation = r->generation,
            .buffer_token = r->buffer_token, .buffer_index = r->buffer_index,
            .general_iova = UINT64_MAX };
        for (unsigned n = 0; n < planes; n++) c->active_output->bytes[n] = bytes[n];
        vcontrol_journal_output(c);
        if (!e) e = x9_vcp_bus_register_output(c->bus,&c->init,&c->status.init,
            &c->reverse,ms,out,log);
        if (e == EBUSY && !log->epoch) {
            /* Only an unreserved host attempt: no prepare_output callback,
             * allocation, NC store or physical send is possible here. The
             * burned unique token is never reused; no buffer was exposed. */
            for (unsigned n = 0; n < X9_VCP_OUTPUT_PLANES; n++)
                if (c->active_output->slice[n].token)
                    panic("X9 VDEC: exposed output without bus reservation");
            memset(c->active_output,0,sizeof(*c->active_output));
            c->active_output = NULL; c->status.output_records--;
            c->status.output = previous_output;
            c->status.output_attempted = previous_attempted;
            c->status.output_prepared = previous_prepared;
            c->status.output_notified = previous_notified;
            lck_mtx_unlock(&c->lock); return e;
        }
        c->status.output_notified = out->message_id == X9_VCP_FRAME_BUFFER_DONE;
        vcontrol_journal_output(c);
        c->active_output->journal.notification = *out;
        c->active_output->journal.notified = c->status.output_notified;
        c->active_output->journal.call = *log;
        c->active_output->journal.last_errno = (uint32_t)e;
        c->active_output = NULL;
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
    struct x9_vdec_control_start_resources previous_resources;
    struct x9_vcp_start_result previous_start;
    uint32_t previous_start_attempted, previous_start_replied;
    uint32_t previous_input_attempted, previous_input_prepared;
    uint32_t previous_vsi_attempted, previous_vsi_staged;
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !annexb || !out || !log) return EINVAL;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    if (!ms || ms > 3000 || !used || used > capacity ||
        capacity > X9_MM_SMMU_MAX_CHECK_BYTES || dynamic_low_latency > UINT8_MAX)
        return EINVAL;
    lck_mtx_lock(&c->lock);
    e = c->closing || !c->status.bound || c->status.reset_attempted ? EBUSY :
        (int)c->status.last_errno;
    if (!e && ((c->status.start_attempted && !c->status.start_replied) ||
        !c->status.output_prepared)) e = EBUSY;
    if (!e && c->status.input_records == X9_VDEC_CONTROL_MAX_RECORDS) e = ENOSPC;
    if (!e && !c->reverse.prepare_start) e = ENOTSUP;
    if (!e) e = x9_vcp_bus_check_shared(c->bus,c->status.epoch,&c->shared->global);
    if (!e) {
        previous_resources = c->status.resources; previous_start = c->status.start;
        previous_start_attempted = c->status.start_attempted;
        previous_start_replied = c->status.start_replied;
        previous_input_attempted = c->status.input_attempted;
        previous_input_prepared = c->status.input_prepared;
        previous_vsi_attempted = c->status.vsi_attempted;
        previous_vsi_staged = c->status.vsi_staged;
        c->status.start_attempted = 1;
        c->status.input_attempted = 1;
        c->status.start_replied = 0; c->status.input_prepared = 0;
        c->status.vsi_attempted = 0; c->status.vsi_staged = 0;
        c->active_input = &c->inputs[c->status.input_records++];
        resources = &c->status.resources;
        memset(resources,0,sizeof(*resources));
        resources->epoch = c->status.epoch;
        resources->instance_token = c->init.instance_token;
        resources->slot = c->slot;
        resources->bytes_used = used;
        resources->timestamp = timestamp;
        resources->dynamic_low_latency = dynamic_low_latency;
        resources->output = c->status.output;
        resources->submission_index = c->status.input_records-1;
        resources->registered_outputs = c->status.output_records;
        vcontrol_journal_input(c);
        c->input_source = annexb;
        c->input_capacity = capacity;
        q = (struct x9_vcp_start){ .service_id = c->init.service_id,
            .context_id = c->init.context_id,
            .firmware_vsi_address = c->status.init.firmware_vsi_address,
            .bytes_used = used,
            .buffer_bytes = (capacity + VDMA_CPU_BYTES - 1) & ~(VDMA_CPU_BYTES - 1),
            .flags = flags };
        c->active_input->journal.request = q; /* before reservation/preparation */
        c->active_start_call = log;
        e = x9_vcp_bus_decode_start(c->bus,&c->init,&c->status.init,&q,&c->reverse,
            ms,out,log);
        c->active_start_call = NULL;
        c->input_source = NULL; /* never retained after return, including error */
        if (e == EBUSY && !log->epoch) {
            if (c->active_input->slice.token)
                panic("X9 VDEC: exposed input without bus reservation");
            memset(c->active_input,0,sizeof(*c->active_input));
            c->active_input = NULL; c->status.input_records--;
            c->status.resources = previous_resources; c->status.start = previous_start;
            c->status.start_attempted = previous_start_attempted;
            c->status.start_replied = previous_start_replied;
            c->status.input_attempted = previous_input_attempted;
            c->status.input_prepared = previous_input_prepared;
            c->status.vsi_attempted = previous_vsi_attempted;
            c->status.vsi_staged = previous_vsi_staged;
            lck_mtx_unlock(&c->lock); return e;
        }
        c->status.start = *out; c->status.start_replied = log->ack_matched;
        vcontrol_journal_input(c);
        c->active_input->journal.result = *out;
        c->active_input->journal.control_replied = log->ack_matched;
        c->active_input->journal.call = *log;
        c->active_input->journal.last_errno = (uint32_t)e;
        c->active_input->journal.postreply_valid = !e && log->ack_matched &&
            c->active_input->journal.postreply_observed &&
            !c->active_input->journal.postreply_errno;
        c->active_input = NULL;
        vcontrol_fault(c,e);
    }
    lck_mtx_unlock(&c->lock);
    return e;
}
/* Manual service and automatic pump use exactly the same physical batch,
 * under c->lock, not two competing OUT0/IN1 consumers. */
static int vcontrol_service_locked(struct x9_vdec_control *c, uint32_t ms,
    struct x9_vcp_bus_call_log *log)
{
    int e;
    memset(log,0,sizeof(*log));
    e = c->closing || !c->status.bound || !c->status.slot_initialized ? EBUSY :
        (int)c->status.last_errno;
    if (!e && (c->status.service_batches == UINT64_MAX ||
        c->status.service_packets > UINT64_MAX-64)) e = vcontrol_fault(c,EOVERFLOW);
    if (!e) {
        e = x9_vcp_bus_service(c->bus,&c->init,&c->status.init,&c->reverse,64,ms,log);
        /* epoch is set only after actual bus reservation. Pre-reservation
         * contention does not latch a fault or claim a serviced batch. */
        if (log->epoch) {
            c->status.service_batches++;
            c->status.service_packets += log->received;
            c->status.service_call = *log;
            vcontrol_fault(c,e);
        } else if (e != EBUSY) {
            c->status.service_call = *log;
            vcontrol_fault(c,e);
        }
    }
    return e;
}
int x9_vdec_control_service(struct x9_vdec_control *c, uint32_t ms,
    struct x9_vcp_bus_call_log *log)
{
    int e;
    if (!c || c->magic != VCONTROL_MAGIC || !log) return EINVAL;
    memset(log,0,sizeof(*log));
    if (!ms || ms > 3000) return EINVAL;
    lck_mtx_lock(&c->lock);
    e = vcontrol_service_locked(c,ms,log);
    lck_mtx_unlock(&c->lock);
    return e;
}
static void vcontrol_worker(thread_call_param_t param, __unused thread_call_param_t unused)
{
    struct x9_vdec_control *c = param;
    struct x9_vcp_bus_status bus;
    struct x9_vcp_bus_call_log log;
    uint32_t delay = VCONTROL_SERVICE_IDLE_MS;
    int e;
    /* The native bus hold and control/shared/memory handles remain retained
     * until closing + ONCE cancel_wait has joined this exact callback. */
    lck_mtx_lock(&c->lock);
    if (c->closing || !c->status.worker_enabled || c->status.last_errno) {
        c->status.worker_enabled = 0;
        lck_mtx_unlock(&c->lock); return;
    }
    if (c->status.worker_wakeups == UINT64_MAX) {
        c->status.worker_enabled = 0;
        c->status.worker_last_errno = (uint32_t)vcontrol_fault(c,EOVERFLOW);
        lck_mtx_unlock(&c->lock); return;
    }
    c->status.worker_wakeups++;
    e = x9_vcp_bus_get_status(c->bus,&bus);
    if (!e && bus.epoch != c->status.epoch) e = ESTALE;
    if (!e) c->status.worker_bus_phase = bus.phase;
    if (!e && bus.phase != X9_VCP_BUS_RUNNING)
        e = bus.last_errno ? (int)bus.last_errno : ECANCELED;
    if (e) {
        /* A STOPPING/STOPPED bus is not transient transaction contention.
         * Stop scheduling; neither its phase nor this CPU stop retires DMA. */
        c->status.worker_enabled = 0; c->status.worker_last_errno = (uint32_t)e;
        if (e != ECANCELED) vcontrol_fault(c,e);
        lck_mtx_unlock(&c->lock); return;
    }
    e = vcontrol_service_locked(c,VCONTROL_SERVICE_TIMEOUT_MS,&log);
    c->status.worker_last_errno = (uint32_t)e;
    if (e == EBUSY && !log.epoch && !c->status.last_errno) {
        /* Actual pre-reservation contention only. A reserved/failed reverse
         * call is never retried, and its packet log remains owned in status. */
        if (c->status.worker_contentions == UINT64_MAX) vcontrol_fault(c,EOVERFLOW);
        else c->status.worker_contentions++;
        delay = VCONTROL_SERVICE_BUSY_MS;
    } else if (e) c->status.worker_enabled = 0;
    else if (log.batch_limit_reached || c->status.recheck_pending)
        delay = VCONTROL_SERVICE_BUSY_MS;
    if (c->status.last_errno) c->status.worker_enabled = 0;
    vcontrol_arm_worker(c,delay);
    lck_mtx_unlock(&c->lock);
}
int x9_vdec_control_quiesce(struct x9_vdec_control *c)
{
    if (!c || c->magic != VCONTROL_MAGIC) return EINVAL;
    lck_mtx_lock(&c->lock);
    c->closing = true; c->status.closing = 1; c->status.worker_enabled = 0;
    lck_mtx_unlock(&c->lock);
    /* Never join holding the control lock needed by a queued callback. The
     * actual XNU ONCE implementation cancels reschedule AND waits in-flight;
     * closing prevents any later entry from this object or manual INIT. */
    (void)thread_call_cancel_wait(c->service_worker);
    lck_mtx_lock(&c->lock); c->status.worker_joined = 1; lck_mtx_unlock(&c->lock);
    return 0;
}
int x9_vdec_control_discard(struct x9_vdec_control **slot)
{
    struct x9_vdec_control *c;
    struct x9_vcp_bus_status bus;
    int e = 0;
    if (!slot || !(c = *slot) || c->magic != VCONTROL_MAGIC) return EINVAL;
    e = x9_vdec_control_quiesce(c);
    if (e) return e;
    lck_mtx_lock(&c->lock);
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
    /* Before touching any callback-visible slice/ref, prove that no bus call
     * still uses this cookie. Host quiesce alone never removes a live route.
     * A failed later cleanup retains this closed object and its tombstone. */
    if (!e && c->route_registered) {
        e = x9_vcp_bus_unregister_instance(c->bus,c->status.epoch,c->bus_token);
        if (!e) c->route_registered = false;
    }
    for (unsigned i = 0; i < c->status.output_records && !e; i++)
        for (unsigned n = 0; n < X9_VCP_OUTPUT_PLANES && !e; n++)
            if (c->outputs[i].slice[n].token)
                e = x9_vdec_dma_slice_free(c->shared->codec,&c->outputs[i].slice[n]);
    for (unsigned i = 0; i < c->status.input_records && !e; i++)
        if (c->inputs[i].slice.token)
            e = x9_vdec_dma_slice_free(c->shared->codec,&c->inputs[i].slice);
    if (!e) e = x9_vcp_bus_release_shared(c->bus,c->status.epoch,&c->bus_token);
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
        if (!thread_call_free(c->service_worker))
            panic("X9 VDEC: joined service worker still pending");
        lck_mtx_destroy(&c->lock,&vdec_group);
        kfree_type(struct vcontrol_input_record,X9_VDEC_CONTROL_MAX_RECORDS,c->inputs);
        kfree_type(struct vcontrol_output_record,X9_VDEC_CONTROL_MAX_RECORDS,c->outputs);
        kfree_data(c->scratch,VCONTROL_SCRATCH); kfree_type(struct x9_vdec_control,c);
    }
    return e;
}
