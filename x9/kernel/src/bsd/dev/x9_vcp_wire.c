#include "x9_vcp_wire.h"
#include <sys/errno.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static int instance_matches(const struct x9_vcp_init *q, uint32_t service,
                             uint64_t token)
{
    return q->service_id == X9_VCP_VDEC_SERVICE && service == q->service_id &&
           q->instance_token && q->instance_token != UINT64_MAX &&
           token == q->instance_token;
}

static int output_extent_valid(const struct x9_vcp_output_extent *e)
{
    return e->iova && e->bytes && e->iova < (UINT64_C(1) << 36) &&
           e->bytes <= (UINT64_C(1) << 36) - e->iova;
}

/* Called only after each full range has passed the no-overflow bound above. */
static int output_extents_overlap(const struct x9_vcp_output_extent *a,
                                   const struct x9_vcp_output_extent *b)
{
    return a->iova < b->iova + b->bytes && b->iova < a->iova + a->bytes;
}

int x9_vcp_encode_output(uint32_t abi, const struct x9_vcp_init *instance,
                          const struct x9_vcp_output_request *request,
                          const struct x9_vcp_output_record *record,
                          uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    struct x9_vcp_output_request r;
    struct x9_vcp_output_record b;
    uint8_t *p;
    uint32_t i;
    if (!out) return EINVAL;
    if (!instance || !request || !record) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(&r, request, sizeof(r));
    memcpy(&b, record, sizeof(b));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (abi != X9_VCP_OUTPUT_ABI_MT6993_CAPTURE1) return ENOTSUP;
    if (!instance_matches(&q, X9_VCP_VDEC_SERVICE, b.instance_token) ||
        b.context_id != q.context_id || !b.generation ||
        r.generation != b.generation || !b.firmware_vsi_address ||
        r.firmware_vsi_address != b.firmware_vsi_address ||
        !b.buffer_token || b.buffer_token == UINT64_MAX ||
        r.buffer_token != b.buffer_token || r.buffer_index != b.buffer_index)
        return EINVAL;
    if (b.plane_count != 1 && b.plane_count != X9_VCP_OUTPUT_PLANES)
        return ENOTSUP;
    for (i = 0; i < b.plane_count; i++)
        if (!output_extent_valid(&b.plane[i])) return ERANGE;
    if (r.y_iova != b.plane[0].iova) return EINVAL;
    if (b.plane_count == 1) {
        if (b.plane[1].iova || b.plane[1].bytes || r.c_iova) return EINVAL;
    } else {
        if (r.c_iova != b.plane[1].iova) return EINVAL;
        if (output_extents_overlap(&b.plane[0], &b.plane[1])) return EINVAL;
    }
    if (!b.general.iova && !b.general.bytes) {
        if (r.general_iova != UINT64_MAX || r.general_bytes) return EINVAL;
    } else {
        if (!output_extent_valid(&b.general) || b.general.bytes > INT32_MAX)
            return ERANGE;
        if (r.general_iova != b.general.iova || r.general_bytes != b.general.bytes)
            return EINVAL;
        for (i = 0; i < b.plane_count; i++)
            if (output_extents_overlap(&b.plane[i], &b.general)) return EINVAL;
    }
    put32(out, X9_VCP_VDEC_OUTPUT_SERVICE);
    put32(out + 4, X9_VCP_FRAME_BUFFER_BYTES);
    p = out + 8;
    put32(p, X9_VCP_FRAME_BUFFER);
    put32(p + 4, q.context_id);
    put64(p + 8, b.firmware_vsi_address);
    put64(p + 24, b.buffer_token);
    put64(p + 32, r.y_iova);
    put64(p + 40, r.c_iova);
    put64(p + 48, r.general_iova);
    put32(p + 56, (uint32_t)r.general_bytes);
    put32(p + 60, b.buffer_index);
    return 0;
}

int x9_vcp_parse_output_notification(const struct x9_vcp_init *instance,
                                      const uint8_t *input, uint32_t bytes,
                                      struct x9_vcp_output_notification *out)
{
    struct x9_vcp_init q;
    struct x9_vcp_output_notification r;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t status, service;
    if (!out) return EINVAL;
    if (!instance || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    p = snapshot + 8;
    r.service_id = get32(snapshot);
    r.message_id = get32(p);
    r.instance_token = get64(p + 8);
    if (r.message_id == X9_VCP_FRAME_BUFFER_DONE)
        service = X9_VCP_VDEC_OUTPUT_SERVICE;
    else if (r.message_id == X9_VCP_OUTPUT_ASYNC)
        service = X9_VCP_VDEC_SERVICE;
    else return ENOTSUP;
    if (!instance_matches(&q, X9_VCP_VDEC_SERVICE, r.instance_token) ||
        r.service_id != service || get32(snapshot + 4) != X9_VCP_OUTPUT_NOTIFY_BYTES)
        return EINVAL;
    status = get32(p + 16);
    if (r.message_id == X9_VCP_OUTPUT_ASYNC && status) return ENOTSUP;
    memcpy(&r.firmware_status, &status, sizeof(status));
    r.context_id = get32(p + 4);
    r.reserved = get32(p + 20);
    r.id = get32(p + 24);
    r.data = get32(p + 28);
    r.payload = get64(p + 32);
    memcpy(out, &r, sizeof(r));
    return 0;
}

static int clock_request_valid(const struct x9_vcp_clock_request *r)
{
    uint32_t hw, acquire, clock_only;
    if (r->message_id < X9_VCP_LOCK_CORE || r->message_id > X9_VCP_UNLOCK_LAT)
        return ENOTSUP;
    hw = r->message_id >= X9_VCP_LOCK_LAT ? X9_VCP_HW_LAT : X9_VCP_HW_CORE;
    acquire = r->message_id == X9_VCP_LOCK_CORE || r->message_id == X9_VCP_LOCK_LAT;
    clock_only = hw == X9_VCP_HW_CORE && r->payload != 0;
    if (r->id != hw || (hw == X9_VCP_HW_LAT && r->payload)) return ENOTSUP;
    if (r->hardware_id != hw || r->acquire != acquire || r->clock_only != clock_only)
        return EINVAL;
    return 0;
}

int x9_vcp_parse_clock_request(const struct x9_vcp_init *request,
                               const uint8_t *input, uint32_t bytes,
                               struct x9_vcp_clock_request *out)
{
    struct x9_vcp_init q;
    struct x9_vcp_clock_request r;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    int err;
    if (!out) return EINVAL;
    if (!request || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    p = snapshot + 8;
    r.service_id = get32(snapshot);
    r.message_id = get32(p);
    r.request_context_id = get32(p + 4);
    r.instance_token = get64(p + 8);
    if (!instance_matches(&q, r.service_id, r.instance_token) ||
        get32(snapshot + 4) != X9_VCP_CLOCK_BYTES) return EINVAL;
    if (get32(p + 16)) return ENOTSUP;
    r.reserved = get32(p + 20);
    r.id = get32(p + 24);
    r.data = get32(p + 28);
    r.payload = get64(p + 32);
    r.hardware_id = r.message_id >= X9_VCP_LOCK_LAT ? X9_VCP_HW_LAT : X9_VCP_HW_CORE;
    r.acquire = r.message_id == X9_VCP_LOCK_CORE || r.message_id == X9_VCP_LOCK_LAT;
    r.clock_only = r.hardware_id == X9_VCP_HW_CORE && r.payload != 0;
    err = clock_request_valid(&r);
    if (err) return err;
    memcpy(out, &r, sizeof(r));
    return 0;
}

int x9_vcp_encode_clock_reply(const struct x9_vcp_init *request,
                              const struct x9_vcp_clock_request *reverse,
                              uint32_t operation_complete,
                              uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    struct x9_vcp_clock_request r;
    uint8_t *p;
    int err;
    if (!out) return EINVAL;
    if (!request || !reverse) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(&r, reverse, sizeof(r));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (!instance_matches(&q, r.service_id, r.instance_token)) return EINVAL;
    err = clock_request_valid(&r);
    if (err) return err;
    if (operation_complete > 1) return EINVAL;
    if (!operation_complete) return EAGAIN;
    put32(out, r.service_id);
    put32(out + 4, X9_VCP_CLOCK_BYTES);
    p = out + 8;
    put32(p, r.message_id + 0x1000u);
    put32(p + 4, q.context_id);
    put64(p + 8, r.instance_token);
    put32(p + 20, r.reserved);
    put32(p + 24, r.id);
    put32(p + 28, r.data);
    put64(p + 32, r.payload);
    return 0;
}

int x9_vcp_parse_ring_snapshot(uint32_t abi, uint32_t kind,
                               const uint8_t *input, uint32_t bytes,
                               struct x9_vcp_ring_snapshot *out)
{
    struct x9_vcp_ring_snapshot r;
    uint8_t metadata[12], entry[24];
    uint32_t offset, stride, distance;
    if (!out) return EINVAL;
    if (abi != X9_VCP_RING_ABI_MT6993_CAPTURE1 || kind > X9_VCP_RING_DISPLAY_FB) {
        memset(out, 0, sizeof(*out));
        return ENOTSUP;
    }
    if (!input || bytes != X9_VCP_INSTANCE_SLOT_BYTES) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    offset = kind == X9_VCP_RING_FREE_BS ? 0u :
             kind == X9_VCP_RING_FREE_FB ? 0x210u : 0x820u;
    stride = kind == X9_VCP_RING_FREE_BS ? 8u : 24u;
    memcpy(metadata, input + offset + stride * X9_VCP_RING_CAPACITY, sizeof(metadata));
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.read_index = get32(metadata);
    r.write_index = get32(metadata + 4);
    r.count = get32(metadata + 8);
    if (r.read_index >= X9_VCP_RING_CAPACITY || r.write_index >= X9_VCP_RING_CAPACITY ||
        r.count > X9_VCP_RING_CAPACITY) {
        memset(out, 0, sizeof(*out));
        return ERANGE;
    }
    if (r.count == X9_VCP_RING_CAPACITY && kind != X9_VCP_RING_FREE_BS) {
        memset(out, 0, sizeof(*out));
        return EOVERFLOW;
    }
    distance = (r.write_index - r.read_index) & (X9_VCP_RING_CAPACITY - 1u);
    /* The captured FB publisher leaves one slot empty. FREE_BS instead uses
     * count==64 as its full guard and increments count after the token/index
     * stores: a full BS ring has equal indices but still owns all 64 entries.
     * Neither case confers live consumer exclusion or DMA retirement. */
    if (r.count == X9_VCP_RING_CAPACITY && kind == X9_VCP_RING_FREE_BS && !distance)
        distance = X9_VCP_RING_CAPACITY;
    if (r.count != distance) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    if (distance) {
        /* Capture the single valid entry before clearing possibly overlapping
         * output; no stale entries or full-slot copy on the kernel stack. */
        memcpy(entry, input + offset + stride * r.read_index, stride);
        r.entry_present = 1;
        r.buffer_token = get64(entry);
        if (kind != X9_VCP_RING_FREE_BS) {
            r.timestamp = get64(entry + 8);
            r.field = (uint16_t)((uint32_t)entry[16] | (uint32_t)entry[17] << 8);
            r.frame_type = (uint16_t)((uint32_t)entry[18] | (uint32_t)entry[19] << 8);
            r.raw_flags = get32(entry + 20);
        }
    }
    memcpy(out, &r, sizeof(r));
    return 0;
}

int x9_vcp_mem_layout(uint32_t abi, uint32_t type, uint32_t bytes,
                       struct x9_vcp_mem_layout *out)
{
    uint32_t allocation_bytes;
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (abi != X9_VCP_MEM_ABI_MT6993_CAPTURE1 || type > X9_VCP_MEM_UBE_HW)
        return ENOTSUP;
    if (!bytes || bytes > X9_VCP_CODEC_ALLOC_MAX_BYTES) return ERANGE;
    allocation_bytes = bytes;
    if (type == X9_VCP_MEM_SW)
        allocation_bytes = ((bytes + 127u) & ~127u) + 128u;
    out->requested_bytes = bytes;
    out->allocation_bytes = allocation_bytes;
    out->extra_bytes = allocation_bytes - bytes;
    return 0;
}

static int mem_request_valid(const struct x9_vcp_mem_request *r)
{
    struct x9_vcp_mem_layout layout;
    int err = x9_vcp_mem_layout(X9_VCP_MEM_ABI_MT6993_CAPTURE1, r->mem.type,
                                 r->mem.bytes, &layout);
    if (err) return err;
    if (r->message_id == X9_VCP_MEM_ALLOC) {
        if (r->mem.iova || r->mem.dma_address || r->mem.allocation_token)
            return EINVAL;
    } else if (r->message_id == X9_VCP_MEM_FREE) {
        if (!r->mem.iova || r->mem.iova >= (1ull << 36) ||
            r->mem.bytes > (1ull << 36) - r->mem.iova ||
            r->mem.dma_address != r->mem.iova ||
            !r->mem.allocation_token || r->mem.allocation_token == UINT64_MAX)
            return ERANGE;
    } else return EINVAL;
    return 0;
}

int x9_vcp_parse_mem_request(const struct x9_vcp_init *request, const uint8_t *input,
                              uint32_t bytes, struct x9_vcp_mem_request *out)
{
    struct x9_vcp_init q;
    struct x9_vcp_mem_request r;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    int err;
    if (!out) return EINVAL;
    if (!request || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    p = snapshot + 8;
    r.service_id = get32(snapshot);
    r.message_id = get32(p);
    r.request_context_id = get32(p + 4);
    r.instance_token = get64(p + 8);
    if (!instance_matches(&q, r.service_id, r.instance_token) ||
        get32(snapshot + 4) != X9_VCP_MEM_OP_BYTES) return EINVAL;
    if (get32(p + 16)) return ENOTSUP;
    r.reserved = get32(p + 20);
    r.mem.type = get32(p + 24);
    r.mem.bytes = get32(p + 28);
    r.mem.iova = get64(p + 32);
    r.mem.dma_address = get64(p + 40);
    r.mem.allocation_token = get64(p + 48);
    r.tail[0] = get32(p + 56);
    r.tail[1] = get32(p + 60);
    err = mem_request_valid(&r);
    if (err) return err;
    memcpy(out, &r, sizeof(r));
    return 0;
}

int x9_vcp_encode_mem_alloc_reply(const struct x9_vcp_init *request,
                                   const struct x9_vcp_mem_request *reverse,
                                   const struct x9_vcp_mem_allocation *allocation,
                                   uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    struct x9_vcp_mem_request r;
    struct x9_vcp_mem_allocation a;
    struct x9_vcp_mem_layout layout;
    uint8_t *p;
    int err;
    if (!out) return EINVAL;
    if (!request || !reverse || !allocation) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(&r, reverse, sizeof(r));
    memcpy(&a, allocation, sizeof(a));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (!instance_matches(&q, r.service_id, r.instance_token) ||
        r.message_id != X9_VCP_MEM_ALLOC || a.status > 0) return EINVAL;
    err = mem_request_valid(&r);
    if (err) return err;
    err = x9_vcp_mem_layout(X9_VCP_MEM_ABI_MT6993_CAPTURE1, r.mem.type,
                             r.mem.bytes, &layout);
    if (err) return err;
    if (!a.status && (!a.iova || a.iova >= (1ull << 36) ||
        a.allocation_bytes < layout.allocation_bytes ||
        a.allocation_bytes > (1ull << 36) - a.iova ||
        !a.allocation_token || a.allocation_token == UINT64_MAX)) return ERANGE;
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_MEM_OP_BYTES);
    p = out + 8;
    put32(p, X9_VCP_MEM_ALLOC_DONE);
    put32(p + 4, q.context_id);
    put64(p + 8, q.instance_token);
    put32(p + 16, (uint32_t)a.status);
    put32(p + 20, r.reserved);
    put32(p + 24, r.mem.type);
    put32(p + 28, r.mem.bytes);
    if (!a.status) {
        put64(p + 32, a.iova);
        put64(p + 40, a.iova); /* stock pa is the device DMA alias */
        put64(p + 48, a.allocation_token);
    }
    put32(p + 56, r.tail[0]);
    put32(p + 60, r.tail[1]);
    return 0;
}

int x9_vcp_match_mem_free(const struct x9_vcp_mem_request *r,
                          const struct x9_vcp_mem_record *record)
{
    int err;
    if (!r || !record || r->message_id != X9_VCP_MEM_FREE ||
        r->service_id != X9_VCP_VDEC_SERVICE || !r->instance_token ||
        r->instance_token == UINT64_MAX) return EINVAL;
    err = mem_request_valid(r);
    if (err) return err;
    /* Compare defined bytes, never host-structure padding or a Linux pointer. */
    if (r->instance_token != record->instance_token ||
        r->mem.type != record->mem.type || r->mem.bytes != record->mem.bytes ||
        r->mem.iova != record->mem.iova ||
        r->mem.dma_address != record->mem.dma_address ||
        r->mem.allocation_token != record->mem.allocation_token) return ENOENT;
    return 0;
}

int x9_vcp_parse_wait_request(const struct x9_vcp_init *request, const uint8_t *input,
                               uint32_t bytes, struct x9_vcp_wait_request *out)
{
    struct x9_vcp_init q;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t hardware_id;
    if (!out) return EINVAL;
    if (!request || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (!instance_matches(&q, get32(snapshot), get64(p + 8)) ||
        get32(snapshot + 4) != X9_VCP_WAITISR_BYTES ||
        get32(p) != X9_VCP_WAITISR) return EINVAL;
    hardware_id = get32(p + 16);
    if (hardware_id > X9_VCP_HW_LAT) return ENOTSUP;
    out->service_id = q.service_id;
    out->request_context_id = get32(p + 4);
    out->instance_token = q.instance_token;
    out->hardware_id = hardware_id;
    out->reserved = get32(p + 20);
    out->id = get32(p + 24);
    out->data = get32(p + 28);
    out->payload = get64(p + 32);
    return 0;
}

int x9_vcp_encode_wait_reply(const struct x9_vcp_init *request,
                              const struct x9_vcp_wait_request *reverse,
                              enum x9_vcp_wait_outcome outcome,
                              uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    struct x9_vcp_wait_request r;
    uint8_t *p;
    int32_t status;
    if (!out) return EINVAL;
    if (!request || !reverse) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(&r, reverse, sizeof(r));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (!instance_matches(&q, r.service_id, r.instance_token)) return EINVAL;
    if (r.hardware_id > X9_VCP_HW_LAT) return ENOTSUP;
    switch (outcome) {
    case X9_VCP_WAIT_PENDING: return EAGAIN;
    case X9_VCP_WAIT_IRQ_RECEIVED: status = 0; break;
    case X9_VCP_WAIT_TIMED_OUT: status = -1; break;
    case X9_VCP_WAIT_INTERRUPTED: status = -2; break;
    default: return EINVAL;
    }
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_WAITISR_BYTES);
    p = out + 8;
    put32(p, X9_VCP_WAITISR_DONE);
    put32(p + 4, q.context_id);
    put64(p + 8, q.instance_token);
    put32(p + 16, (uint32_t)status);
    put32(p + 20, r.reserved);
    put32(p + 24, r.id);
    put32(p + 28, r.data);
    put64(p + 32, r.payload);
    return 0;
}

int x9_vcp_encode_query(const struct x9_vcp_query *query,
                        uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_query q;
    uint8_t *payload;
    if (!out) return EINVAL;
    if (!query) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    /* Snapshot parameters before clearing out, including aliased callers. */
    memcpy(&q, query, sizeof(q));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token || !q.data_token)
        return EINVAL;
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_QUERY_BYTES);
    payload = out + 8;
    put32(payload, X9_VCP_QUERY_CAP);
    put32(payload + 4, q.context_id);
    put64(payload + 8, q.instance_token);
    put64(payload + 24, q.data_token);
    put32(payload + 32, q.capability_id);
    return 0;
}

int x9_vcp_parse_query_reply(const struct x9_vcp_query *query, const uint8_t *input,
                              uint32_t bytes, struct x9_vcp_query_result *out)
{
    struct x9_vcp_query q;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t status;
    uint64_t address;
    if (!out) return EINVAL;
    if (!query || !input) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, query, sizeof(q));
    if (bytes != X9_VCP_WIRE_BYTES) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token || !q.data_token ||
        get32(snapshot) != q.service_id ||
        get32(snapshot + 4) != X9_VCP_QUERY_REPLY_BYTES ||
        get32(p) != X9_VCP_QUERY_CAP_DONE || get32(p + 4) != q.context_id ||
        get64(p + 8) != q.instance_token || get64(p + 24) != q.data_token ||
        get32(p + 40) != q.capability_id) return EINVAL;
    status = get32(p + 16);
    memcpy(&out->firmware_status, &status, sizeof(status));
    if (status) return EIO;
    address = get64(p + 32);
    if (!address || address > UINT32_MAX) return ERANGE;
    out->firmware_data_address = (uint32_t)address;
    return 0;
}

int x9_vcp_encode_init(const struct x9_vcp_init *request,
                       uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    uint8_t *p;
    if (!out) return EINVAL;
    if (!request) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token ||
        q.instance_token == UINT64_MAX) return EINVAL;
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_INIT_BYTES);
    p = out + 8;
    put32(p, X9_VCP_INIT);
    put32(p + 4, q.context_id);
    put64(p + 8, q.instance_token);
    return 0;
}

int x9_vcp_parse_init_reply(const struct x9_vcp_init *request, const uint8_t *input,
                            uint32_t bytes, struct x9_vcp_init_result *out)
{
    struct x9_vcp_init q;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t status;
    uint64_t address;
    if (!out) return EINVAL;
    if (!request || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token ||
        q.instance_token == UINT64_MAX || get32(snapshot) != q.service_id ||
        get32(snapshot + 4) != X9_VCP_INIT_REPLY_BYTES ||
        get32(p) != X9_VCP_INIT_DONE || get64(p + 8) != q.instance_token)
        return EINVAL;
    status = get32(p + 16);
    memcpy(&out->firmware_status, &status, sizeof(status));
    out->reply_context_id = get32(p + 4);
    if (status) return EIO;
    address = get64(p + 24);
    if (!address || address > UINT32_MAX) return ERANGE;
    out->firmware_vsi_address = (uint32_t)address;
    return 0;
}

int x9_vcp_parse_start_reply(const struct x9_vcp_init *instance, const uint8_t *input,
    uint32_t bytes, struct x9_vcp_start_result *out)
{
    struct x9_vcp_init q;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t status;
    if (!out) return EINVAL;
    if (!instance || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (!instance_matches(&q, get32(snapshot), get64(p + 8)) ||
        get32(snapshot + 4) != X9_VCP_START_REPLY_BYTES ||
        get32(p) != X9_VCP_START_DONE) return EINVAL;
    status = get32(p + 16);
    memcpy(&out->firmware_status, &status, sizeof(status));
    out->reply_context_id = get32(p + 4);
    memcpy(out->opaque, p + 20, sizeof(out->opaque));
    return status ? EIO : 0;
}

int x9_vcp_parse_codec_check(const struct x9_vcp_init *instance, const uint8_t *input,
                             uint32_t bytes, struct x9_vcp_codec_check *out)
{
    struct x9_vcp_init q;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t svp, codec;
    if (!out) return EINVAL;
    if (!instance || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token ||
        q.instance_token == UINT64_MAX || get32(snapshot) != q.service_id ||
        get32(snapshot + 4) != X9_VCP_CHECK_CODEC_BYTES ||
        get32(p) != X9_VCP_CHECK_CODEC || get64(p + 8) != q.instance_token)
        return EINVAL;
    svp = get32(p + 16);
    codec = get32(p + 24);
    memcpy(&out->candidate_svp, &svp, sizeof(svp));
    memcpy(&out->codec_id, &codec, sizeof(codec));
    out->service_id = q.service_id;
    out->request_context_id = get32(p + 4);
    out->instance_token = q.instance_token;
    out->reserved = get32(p + 20);
    out->data = get32(p + 28);
    out->payload = get64(p + 32);
    return 0;
}

int x9_vcp_encode_hevc_codec_reply(const struct x9_vcp_init *instance, uint32_t fourcc,
                                   const struct x9_vcp_codec_check *request,
                                   uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    struct x9_vcp_codec_check r;
    uint8_t *p;
    uint32_t codec;
    if (!out) return EINVAL;
    if (!instance || !request) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(&r, request, sizeof(r));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token ||
        q.instance_token == UINT64_MAX || r.service_id != q.service_id ||
        r.instance_token != q.instance_token) return EINVAL;
    if (fourcc != X9_VCP_FOURCC_HEVC) return ENOTSUP;
    memcpy(&codec, &r.codec_id, sizeof(codec));
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_CHECK_CODEC_BYTES);
    p = out + 8;
    put32(p, X9_VCP_CHECK_CODEC_DONE);
    put32(p + 4, q.context_id);
    put64(p + 8, q.instance_token);
    put32(p + 16, codec == X9_VCP_CODEC_HEVC && r.candidate_svp == 0 ? 0 : UINT32_MAX);
    put32(p + 20, r.reserved);
    put32(p + 24, codec);
    put32(p + 28, r.data);
    put64(p + 32, r.payload);
    return 0;
}

int x9_vcp_encode_start(const struct x9_vcp_start *request,
                        uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_start q;
    uint8_t *p;
    if (!out) return EINVAL;
    if (!request) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.firmware_vsi_address ||
        !q.bytes_used || q.bytes_used > q.buffer_bytes) return EINVAL;
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_START_BYTES);
    p = out + 8;
    put32(p, X9_VCP_START);
    put32(p + 4, q.context_id);
    put64(p + 8, q.firmware_vsi_address);
    put32(p + 16, q.bytes_used);
    put32(p + 20, q.buffer_bytes);
    put32(p + 24, q.flags);
    return 0;
}

int x9_vcp_parse_decode_snapshot(uint32_t abi, const uint8_t *input,
                                uint32_t bytes, struct x9_vcp_decode_snapshot *out)
{
    /* Only evidenced byte ranges. Keep kernel stack use bounded even though
     * input is a full slot; preserve overlap without snapshotting 6.5 KiB. */
    struct {
        uint8_t addresses[0x5c], tokens_errors[0x24], timestamp[8];
        uint8_t modes[5], bitdepth[4], general[16], blocked[4];
    } s;
    uint32_t planes, fd, i;
    if (!out) return EINVAL;
    if (abi != X9_VCP_DECODE_ABI_MT6993_CAPTURE1) {
        memset(out, 0, sizeof(*out));
        return ENOTSUP;
    }
    if (!input || bytes != X9_VCP_INSTANCE_SLOT_BYTES) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(s.addresses, input + 0xe7c, sizeof(s.addresses));
    memcpy(s.tokens_errors, input + 0xf18, sizeof(s.tokens_errors));
    memcpy(s.timestamp, input + 0xf48, sizeof(s.timestamp));
    memcpy(s.bitdepth, input + 0xf80, sizeof(s.bitdepth));
    memcpy(s.modes, input + 0x1000, sizeof(s.modes));
    memcpy(s.general, input + 0x1208, sizeof(s.general));
    memcpy(s.blocked, input + 0x1228, sizeof(s.blocked));
    memset(out, 0, sizeof(*out));
    planes = get32(s.tokens_errors + 0x10);
    if (planes > X9_VCP_DECODE_MAX_PLANES) return ERANGE;
    out->changed_info = get32(s.addresses);
    out->bs_iova = get64(s.addresses + 4);
    out->bs_non_acp_iova = get64(s.addresses + 0x14);
    for (i = 0; i < X9_VCP_DECODE_MAX_PLANES; i++)
        out->fb_iova[i] = get64(s.addresses + 0x1c + 8 * i);
    out->bs_token = get64(s.tokens_errors);
    out->fb_token = get64(s.tokens_errors + 8);
    out->plane_count = planes;
    out->fb_index = get32(s.tokens_errors + 0x14);
    out->error_map = get32(s.tokens_errors + 0x18);
    out->error_code[0] = get32(s.tokens_errors + 0x1c);
    out->error_code[1] = get32(s.tokens_errors + 0x20);
    out->timestamp = get64(s.timestamp);
    out->input_driven = s.modes[0];
    out->output_async = s.modes[1];
    out->low_power_mode = s.modes[3];
    out->dynamic_low_latency = s.modes[4];
    out->bitdepth = get32(s.bitdepth);
    out->general_iova = get64(s.general);
    fd = get32(s.general + 8);
    memcpy(&out->general_fd, &fd, sizeof(fd));
    out->general_bytes = get32(s.general + 12);
    out->ipi_blocked = get32(s.blocked);
    return 0;
}

int x9_vcp_parse_vp_request(const struct x9_vcp_init *instance, const uint8_t *input,
                           uint32_t bytes, struct x9_vcp_vp_request *out)
{
    struct x9_vcp_init q;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    if (!out) return EINVAL;
    if (!instance || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token ||
        q.instance_token == UINT64_MAX || get32(snapshot) != q.service_id ||
        get32(snapshot + 4) != X9_VCP_KERNEL_PARAM_BYTES ||
        get32(p) != X9_VCP_GET_KERNEL_PARAM || get64(p + 8) != q.instance_token)
        return EINVAL;
    if (get32(p + 16) || get32(p + 24) != X9_VCP_KPARAM_VP_BUFFER) return ENOTSUP;
    out->service_id = q.service_id;
    out->request_context_id = get32(p + 4);
    out->instance_token = q.instance_token;
    out->reserved = get32(p + 20);
    out->data = get32(p + 28);
    out->payload = get64(p + 32);
    return 0;
}

int x9_vcp_encode_vp_reply(const struct x9_vcp_init *instance,
                          const struct x9_vcp_vp_request *request,
                          const struct x9_vcp_common_snapshot *common,
                          uint32_t bitdepth, uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_init q;
    struct x9_vcp_vp_request r;
    struct x9_vcp_common_snapshot c;
    uint32_t group, i;
    uint8_t *p;
    if (!out) return EINVAL;
    if (!instance || !request || !common) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, instance, sizeof(q));
    memcpy(&r, request, sizeof(r));
    memcpy(&c, common, sizeof(c));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (q.service_id != X9_VCP_VDEC_SERVICE || !q.instance_token ||
        q.instance_token == UINT64_MAX || r.service_id != q.service_id ||
        r.instance_token != q.instance_token) return EINVAL;
    if (bitdepth != 8 && bitdepth != 10) return ENOTSUP;
    group = bitdepth == 8 ? 0 : 1;
    if (c.enable_smmu != 1 || c.has_fault || c.vp_published[group] != 1)
        return EAGAIN;
    for (i = 0; i < X9_VCP_VP_BUFFER_COUNT; i++)
        if (!c.vp_iova[group][i] || c.vp_iova[group][i] >= (UINT64_C(1) << 36))
            return ERANGE;
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_KERNEL_PARAM_BYTES);
    p = out + 8;
    put32(p, X9_VCP_GET_KERNEL_PARAM_DONE);
    put32(p + 4, r.request_context_id);
    put64(p + 8, q.instance_token);
    put32(p + 20, r.reserved);
    put32(p + 24, X9_VCP_KPARAM_VP_BUFFER);
    put32(p + 28, r.data);
    put64(p + 32, r.payload);
    return 0;
}

int x9_vcp_vp_layout(uint32_t abi, uint32_t bitdepth, uint32_t multiplier,
                     struct x9_vcp_vp_layout *out)
{
    uint32_t i, pattern;
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (abi != X9_VCP_VP_ABI_MT6993_CAPTURE1 || (bitdepth != 8 && bitdepth != 10))
        return ENOTSUP;
    if (!multiplier) return EINVAL;
    if (multiplier > (X9_VCP_CODEC_ALLOC_MAX_BYTES - 16) / X9_VCP_VP_PATTERN_BYTES)
        return ERANGE;
    out->group_index = bitdepth == 8 ? 0 : 1;
    out->memory_type = X9_VCP_VP_MEMORY_TYPE;
    out->buffer_bytes = X9_VCP_VP_PATTERN_BYTES * multiplier + 16;
    for (i = 0; i < X9_VCP_VP_BUFFER_COUNT; i++) {
        pattern = bitdepth == 10 && i != X9_VCP_VP_LENGTH ? X9_VCP_VP_PATTERN_BYTES : 4;
        out->pattern_bytes[i] = pattern;
        out->repetitions[i] = out->buffer_bytes / pattern;
        out->written_bytes[i] = out->repetitions[i] * pattern;
        out->untouched_bytes[i] = out->buffer_bytes - out->written_bytes[i];
    }
    return 0;
}

int x9_vcp_vp_pattern(uint32_t abi, uint32_t bitdepth, uint32_t kind,
                      struct x9_vcp_vp_pattern *out)
{
    uint8_t sample;
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (abi != X9_VCP_VP_ABI_MT6993_CAPTURE1 || (bitdepth != 8 && bitdepth != 10))
        return ENOTSUP;
    if (kind >= X9_VCP_VP_BUFFER_COUNT) return EINVAL;
    if (kind == X9_VCP_VP_LENGTH) {
        out->bytes = 4;
        out->data[0] = out->data[2] = bitdepth == 10 ? 0xcf : 0xbf;
        out->data[1] = out->data[3] = bitdepth == 10 ? 0x27 : 0x1f;
    } else {
        sample = kind == X9_VCP_VP_Y ? 0x10 : 0x80;
        if (bitdepth == 8) {
            out->bytes = 4;
            memset(out->data, sample, out->bytes);
        } else {
            /* Captured .data+0x25/0xe5, 192B each: 0*16, sample*64,
             * 0*16, sample*64, 0*32. Not a linear 10-bit pixel layout. */
            out->bytes = X9_VCP_VP_PATTERN_BYTES;
            memset(out->data + 16, sample, 64);
            memset(out->data + 96, sample, 64);
        }
    }
    return 0;
}

int x9_vcp_parse_shm_request(const uint8_t *input,
                            uint32_t bytes, struct x9_vcp_shm_request *out)
{
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    if (!out) return EINVAL;
    if (!input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    p = snapshot + 8;
    if (get32(snapshot) != X9_VCP_VDEC_SERVICE || get32(snapshot + 4) != X9_VCP_SHM_BYTES ||
        get32(p) != X9_VCP_SHM_ALLOC || get32(p + 24) != X9_VCP_MEM_SHM)
        return EINVAL;
    /* Only the observed initial-service variant is understood. Firmware
     * zeroes the 64-byte payload before setting ID/sentinel/type. */
    if (get32(p + 4) || get64(p + 8) != UINT64_MAX || get64(p + 16) ||
        get32(p + 28) || get64(p + 32) || get64(p + 40) ||
        get64(p + 48) || get64(p + 56)) return ENOTSUP;
    out->service_id = get32(snapshot);
    out->context_id = get32(p + 4);
    out->instance_token = get64(p + 8);
    return 0;
}

static int shm_region_valid(const struct x9_vcp_shm_region *r)
{
    /* Captured HFRP aperture; size is protocol size, not CPU-page rounding. */
    const uint64_t start = UINT64_C(0x140000000), end = UINT64_C(0x1c0000000);
    return r->bytes && !(r->iova & 15) && r->iova >= start &&
        r->iova < end && r->bytes <= end - r->iova &&
        r->bytes <= (UINT64_C(1) << 27) - (r->iova & 0x7ffffff);
}

static int shm_regions_overlap(const struct x9_vcp_shm_region *a,
                                const struct x9_vcp_shm_region *b)
{
    return a->iova < b->iova + b->bytes && b->iova < a->iova + a->bytes;
}

int x9_vcp_shm_work_layout(uint32_t bytes, struct x9_vcp_shm_layout *out)
{
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (bytes < X9_VCP_SHM_MIN_BYTES) return ERANGE;
    out->common.bytes = X9_VCP_COMMON_PREFIX_BYTES;
    out->instance_pool.offset = X9_VCP_COMMON_PREFIX_BYTES;
    out->instance_pool.bytes = 0x33d50u - X9_VCP_COMMON_PREFIX_BYTES;
    out->auxiliary[0].offset = 0x33d50u;
    out->auxiliary[0].bytes = 0x800u;
    out->auxiliary[1].offset = 0x34550u;
    out->auxiliary[1].bytes = 0x1400u;
    out->remainder.offset = X9_VCP_SHM_MIN_BYTES;
    out->remainder.bytes = bytes - X9_VCP_SHM_MIN_BYTES;
    return 0;
}

int x9_vcp_resolve_instance_slot(uint32_t abi, const struct x9_vcp_shm_region *region,
                                 uint32_t alias, uint32_t address,
                                 struct x9_vcp_instance_slot *out)
{
    struct x9_vcp_shm_region r;
    uint32_t offset, relative;
    int error;
    if (!out) return EINVAL;
    if (!region) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&r, region, sizeof(r));
    memset(out, 0, sizeof(*out));
    if (abi != X9_VCP_INSTANCE_ABI_MT6993_CAPTURE1) return ENOTSUP;
    if (r.bytes < X9_VCP_SHM_MIN_BYTES || !shm_region_valid(&r)) return ERANGE;
    error = x9_vcp_shared_offset(r.iova, alias, r.bytes, address,
                                X9_VCP_INSTANCE_SLOT_BYTES, &offset);
    if (error) return error;
    if (offset < X9_VCP_COMMON_PREFIX_BYTES) return ERANGE;
    relative = offset - X9_VCP_COMMON_PREFIX_BYTES;
    if (relative % X9_VCP_INSTANCE_SLOT_BYTES ||
        relative / X9_VCP_INSTANCE_SLOT_BYTES >= X9_VCP_INSTANCE_SLOT_COUNT)
        return ERANGE;
    out->index = relative / X9_VCP_INSTANCE_SLOT_BYTES;
    out->offset = offset;
    out->bytes = X9_VCP_INSTANCE_SLOT_BYTES;
    return 0;
}

int x9_vcp_encode_shm_reply(const struct x9_vcp_shm_request *request,
                            const struct x9_vcp_shm_mapping *mapping,
                            uint8_t out[X9_VCP_WIRE_BYTES])
{
    struct x9_vcp_shm_request q;
    struct x9_vcp_shm_mapping m;
    uint32_t prop, log;
    uint8_t *p;
    if (!out) return EINVAL;
    if (!request || !mapping) {
        memset(out, 0, X9_VCP_WIRE_BYTES);
        return EINVAL;
    }
    memcpy(&q, request, sizeof(q));
    memcpy(&m, mapping, sizeof(m));
    memset(out, 0, X9_VCP_WIRE_BYTES);
    if (q.service_id != X9_VCP_VDEC_SERVICE || q.context_id ||
        q.instance_token != UINT64_MAX || !m.cpu_token)
        return EINVAL;
    if (m.vdec.bytes < X9_VCP_SHM_MIN_BYTES ||
        !shm_region_valid(&m.vdec) || !shm_region_valid(&m.prop) ||
        !shm_region_valid(&m.log) || shm_regions_overlap(&m.vdec, &m.prop) ||
        shm_regions_overlap(&m.vdec, &m.log) || shm_regions_overlap(&m.prop, &m.log))
        return ERANGE;
    if (x9_vcp_pack_iova(m.prop.iova, &prop) || x9_vcp_pack_iova(m.log.iova, &log))
        return ERANGE;
    put32(out, q.service_id);
    put32(out + 4, X9_VCP_SHM_BYTES);
    p = out + 8;
    put32(p, X9_VCP_SHM_ALLOC_DONE);
    put32(p + 4, q.context_id);
    put64(p + 8, q.instance_token);
    put32(p + 24, X9_VCP_MEM_SHM);
    put32(p + 28, m.vdec.bytes);
    put64(p + 32, m.vdec.iova);
    put64(p + 40, m.vdec.iova); /* actual stock DMA address semantics */
    put64(p + 48, m.cpu_token);
    put32(p + 56, prop);
    put32(p + 60, log);
    return 0;
}

int x9_vcp_common_cold_image(uint32_t abi,
                            uint8_t out[X9_VCP_COMMON_PREFIX_BYTES])
{
    if (!out) return EINVAL;
    memset(out, 0, X9_VCP_COMMON_PREFIX_BYTES);
    if (abi != X9_VCP_COMMON_ABI_MT6993_CAPTURE1) return ENOTSUP;
    /* Fresh host-owned bytes only: this is not a retained-state reset. */
    out[X9_VCP_COMMON_SMMU_OFFSET] = 1;
    return 0;
}

int x9_vcp_parse_common_snapshot(uint32_t abi, const uint8_t *input,
                                uint32_t bytes, struct x9_vcp_common_snapshot *out)
{
    uint8_t snapshot[X9_VCP_COMMON_PREFIX_BYTES];
    uint32_t has_fault, i, j;
    if (!out) return EINVAL;
    if (abi != X9_VCP_COMMON_ABI_MT6993_CAPTURE1) {
        memset(out, 0, sizeof(*out));
        return ENOTSUP;
    }
    if (!input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    /* Caller supplies a coherent host copy. Snapshot before clearing aliased
     * output; copying a live SHM pointer here would not provide coherency. */
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    has_fault = get32(snapshot + 16);
    if (has_fault > 1 || snapshot[X9_VCP_COMMON_SMMU_OFFSET] > 1 ||
        snapshot[X9_VCP_COMMON_VP_FLAGS_OFFSET] > 1 ||
        snapshot[X9_VCP_COMMON_VP_FLAGS_OFFSET + 1] > 1) return EINVAL;
    out->fault_hw_id = get32(snapshot);
    out->fault_port = get32(snapshot + 4);
    out->fault_iova = get64(snapshot + 8);
    out->has_fault = has_fault;
    out->enable_smmu = snapshot[X9_VCP_COMMON_SMMU_OFFSET];
    for (i = 0; i < 2; i++) {
        out->vp_published[i] = snapshot[X9_VCP_COMMON_VP_FLAGS_OFFSET + i];
        for (j = 0; j < 3; j++)
            out->vp_iova[i][j] = get64(snapshot + X9_VCP_COMMON_VP_IOVA_OFFSET +
                                      i * 24 + j * 8);
    }
    return 0;
}

int x9_vcp_pack_iova(uint64_t iova, uint32_t *packed)
{
    if (!packed) return EINVAL;
    *packed = 0;
    if (!iova || iova >= (UINT64_C(1) << 36) || (iova & 15)) return ERANGE;
    *packed = (uint32_t)(iova | ((iova >> 32) & 15));
    return 0;
}

uint64_t x9_vcp_unpack_iova(uint32_t packed)
{
    return (uint64_t)(packed & ~15u) | (uint64_t)(packed & 15u) << 32;
}

int x9_vcp_parse_ready_event(uint32_t abi, uint32_t controller,
                            uint32_t channel, uint32_t core_count,
                            uint32_t expected_sram_bytes, const uint8_t *payload,
                            uint32_t payload_bytes, struct x9_vcp_ready_event *out)
{
    struct x9_vcp_ready_event r;
    uint8_t p[X9_VCP_READY_BYTES];
    if (!out) return EINVAL;
    if (!payload || payload_bytes != sizeof(p)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(p, payload, sizeof(p));
    memset(out, 0, sizeof(*out));
    if (abi != X9_VCP_READY_ABI_MT6993_CAPTURE1 || core_count != 2)
        return ENOTSUP;
    if (controller > X9_VCP_CONTROLLER_MMUP ||
        channel != (controller == X9_VCP_CONTROLLER_VCP ?
                    X9_VCP_READY_CHANNEL : X9_MMUP_READY_CHANNEL)) return ENOTSUP;
    if (!expected_sram_bytes || get32(p) != expected_sram_bytes) return EINVAL;
    r.controller = controller;
    r.channel = channel;
    r.ready_index = controller;
    r.sram_bytes = get32(p);
    memcpy(out, &r, sizeof(r));
    return 0;
}

int x9_vcp_parse_boot_gprs(uint32_t abi,
                          const struct x9_vcp_shm_region *shared,
                          const struct x9_vcp_shm_region *logger,
                          const uint8_t *mmup, uint32_t mmup_bytes,
                          const uint8_t *vcp, uint32_t vcp_bytes,
                          struct x9_vcp_boot_snapshot *out)
{
    struct x9_vcp_shm_region s, l;
    struct x9_vcp_boot_snapshot r;
    uint8_t m[X9_VCP_BOOT_GPR_BYTES], v[X9_VCP_BOOT_GPR_BYTES];
    uint64_t offset;
    if (!out) return EINVAL;
    if (!shared || !logger || !mmup || !vcp || mmup_bytes != sizeof(m) ||
        vcp_bytes != sizeof(v)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    memcpy(&s, shared, sizeof(s));
    memcpy(&l, logger, sizeof(l));
    memcpy(m, mmup, sizeof(m));
    memcpy(v, vcp, sizeof(v));
    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    if (abi != X9_VCP_BOOT_ABI_MT6993_CAPTURE1) return ENOTSUP;
    if (!s.bytes || (s.iova & 15) || s.iova < UINT64_C(0x140000000) ||
        s.iova >= UINT64_C(0x1c0000000) ||
        s.bytes > UINT64_C(0x1c0000000) - s.iova) return ERANGE;
    if (!l.bytes || (l.bytes & 15) || (l.iova & 15) || l.iova < s.iova)
        return ERANGE;
    offset = l.iova - s.iova;
    /* VCP logger GPR is base+stride, not an advertised second extent.
     * Reject one-past-end even when the first logger extent itself fits. */
    if (offset >= s.bytes || l.bytes >= s.bytes - offset) return ERANGE;
    r.shared_iova = x9_vcp_unpack_iova(get32(m));
    r.shared_bytes = get32(m + 4);
    r.mmup_logger_iova = x9_vcp_unpack_iova(get32(m + 8));
    r.vcp_logger_iova = x9_vcp_unpack_iova(get32(v + 8));
    if (r.shared_iova != s.iova || r.shared_bytes != s.bytes ||
        x9_vcp_unpack_iova(get32(v)) != s.iova || get32(v + 4) != s.bytes ||
        r.mmup_logger_iova != l.iova || r.vcp_logger_iova != l.iova + l.bytes)
        return EINVAL;
    memcpy(out, &r, sizeof(r));
    return 0;
}

int x9_vcp_shared_offset(uint64_t iova, uint32_t alias, uint32_t size,
                          uint32_t address, uint32_t bytes, uint32_t *offset)
{
    uint64_t delta;
    if (!offset) return EINVAL;
    *offset = 0;
    if (!iova || iova >= (UINT64_C(1) << 36) || !alias || !size || !bytes ||
        (iova & 0x7ffffff) != (alias & 0x7ffffff) ||
        size > (UINT64_C(1) << 27) - (iova & 0x7ffffff) ||
        size > (UINT64_C(1) << 36) - iova ||
        size > (UINT64_C(1) << 32) - alias || address < alias) return ERANGE;
    delta = (uint64_t)address - alias;
    if (delta >= size || bytes > size - delta) return ERANGE;
    *offset = (uint32_t)delta;
    return 0;
}
