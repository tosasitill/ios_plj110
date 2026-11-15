/* Included after the native DMA arena implementation; never a raw PA API. */
#include <kern/clock.h>
#include "x9_vdec_memory.h"
#define VMEM_MAGIC UINT64_C(0x58394d454d303031)
struct vmem_record {
    struct x9_vcp_mem_record wire;
    struct x9_vdec_dma_slice slice;
    uint32_t arena;
};
struct x9_vdec_memory {
    uint64_t magic, epoch;
    lck_mtx_t lock;
    struct x9_vcp_init init;
    struct x9_vdec_dma *arena[2];
    struct vmem_record *records;
    int fault_errno;
    uint32_t control_refs;
    bool closing;
};
static int vmem_deadline(uint64_t end)
{
    uint64_t ticks, now;
    clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks,&now);
    return !end || now >= end ? ETIMEDOUT : 0;
}
static int vmem_hold_arena(struct x9_vdec_dma *d, uint32_t sid,
    uint64_t epoch, uint64_t parent)
{
    int e;
    if (!d || d->magic != VDMA_MAGIC) return EINVAL;
    lck_mtx_lock(&d->lock);
    e = d->request.sid != sid || d->request.epoch != epoch ||
        d->request.power_token != parent ? ESTALE : 0;
    if (!e && (d->state != X9_MM_SMMU_UNEXPOSED && d->state != X9_MM_SMMU_INSTALLED)) e = EBUSY;
    if (!e && d->legacy_staged) e = EBUSY;
    if (!e && d->client_refs == UINT32_MAX) e = EOVERFLOW;
    if (!e) d->client_refs++;
    lck_mtx_unlock(&d->lock);
    return e;
}
static void vmem_drop_arena(struct x9_vdec_dma *d)
{
    lck_mtx_lock(&d->lock);
    if (!d->client_refs) panic("X9 VDEC: lost memory arena reference");
    d->client_refs--;
    lck_mtx_unlock(&d->lock);
}
int x9_vdec_memory_create(struct x9_vdec_dma *hfrp, struct x9_vdec_dma *codec,
    uint32_t context, struct x9_vdec_memory **out, struct x9_vcp_init *init)
{
    struct x9_vdec_memory *m;
    uint64_t epoch, parent, token;
    int e;
    if (!out || *out || !init || !hfrp || !codec || hfrp == codec ||
        hfrp->magic != VDMA_MAGIC || codec->magic != VDMA_MAGIC) return EINVAL;
    memset(init,0,sizeof(*init));
    epoch = hfrp->request.epoch; parent = hfrp->request.power_token;
    e = vmem_hold_arena(hfrp,X9_MM_SMMU_HFRP_SID,epoch,parent);
    if (e) return e;
    e = vmem_hold_arena(codec,X9_MM_SMMU_CODEC_SID,epoch,parent);
    if (e) { vmem_drop_arena(hfrp); return e; }
    m = kalloc_type(struct x9_vdec_memory,Z_WAITOK | Z_ZERO);
    if (!m) { vmem_drop_arena(codec); vmem_drop_arena(hfrp); return ENOMEM; }
    m->records = kalloc_type(struct vmem_record,X9_VDEC_DMA_MAX_SLICES,Z_WAITOK | Z_ZERO);
    if (!m->records) {
        kfree_type(struct x9_vdec_memory,m);
        vmem_drop_arena(codec); vmem_drop_arena(hfrp); return ENOMEM;
    }
    token = vdma_take_token();
    if (!token) {
        kfree_type(struct vmem_record,X9_VDEC_DMA_MAX_SLICES,m->records);
        kfree_type(struct x9_vdec_memory,m);
        vmem_drop_arena(codec); vmem_drop_arena(hfrp); return EOVERFLOW;
    }
    m->magic = VMEM_MAGIC; m->epoch = epoch;
    m->arena[0] = hfrp; m->arena[1] = codec;
    m->init = (struct x9_vcp_init){X9_VCP_VDEC_SERVICE,context,token};
    lck_mtx_init(&m->lock,&vdec_group,LCK_ATTR_NULL);
    *init = m->init; *out = m;
    return 0;
}
int x9_vdec_memory_alloc(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_mem_request *request, struct x9_vcp_mem_allocation *out)
{
    struct x9_vdec_memory *m = cookie;
    struct x9_vcp_mem_request q;
    struct x9_vcp_mem_layout layout;
    struct x9_vdec_dma_extent extent = {0};
    struct vmem_record *r = NULL;
    struct x9_vdec_dma *d;
    uint32_t arena;
    int e;
    if (!out) return EINVAL;
    if (!request) { memset(out,0,sizeof(*out)); return EINVAL; }
    q = *request; memset(out,0,sizeof(*out));
    if (!m || m->magic != VMEM_MAGIC) return EINVAL;
    lck_mtx_lock(&m->lock);
    e = m->closing ? EBUSY : m->fault_errno;
    if (!e) e = vmem_deadline(end);
    if (!e && (epoch != m->epoch || q.instance_token != m->init.instance_token ||
        q.service_id != m->init.service_id)) e = ESTALE;
    if (!e && (q.message_id != X9_VCP_MEM_ALLOC || q.mem.iova ||
        q.mem.dma_address || q.mem.allocation_token)) e = EINVAL;
    if (!e) e = x9_vcp_mem_layout(X9_VCP_MEM_ABI_MT6993_CAPTURE1,q.mem.type,q.mem.bytes,&layout);
    /* UBE's preferred devices/fallback are a route, not the type3 constant. */
    if (!e && q.mem.type == X9_VCP_MEM_UBE_HW) e = ENOTSUP;
    if (e) goto done;
    arena = q.mem.type == X9_VCP_MEM_SW ? 0u : 1u;
    d = m->arena[arena];
    lck_mtx_lock(&d->lock);
    e = d->state != X9_MM_SMMU_INSTALLED ? ENXIO : vdma_slice_cpu_domain(d);
    lck_mtx_unlock(&d->lock);
    if (e) goto done;
    for (unsigned i = 0; i < X9_VDEC_DMA_MAX_SLICES; i++)
        if (!m->records[i].slice.token) { r = &m->records[i]; break; }
    if (!r || layout.allocation_bytes > X9_MM_SMMU_MAX_CHECK_BYTES) {
        out->status = -12; goto done; /* explicit Linux ENOMEM wire status */
    }
    e = x9_vdec_dma_slice_alloc(d,epoch,layout.allocation_bytes,128,&r->slice);
    if (e == ENOSPC || e == ENOMEM) { out->status = -12; e = 0; goto done; }
    if (e) goto done;
    /* This record survives all later errors, including an uncertain send.
     * Install it BEFORE exposing the IOVA or returning allocation success. */
    r->arena = arena; r->wire.instance_token = m->init.instance_token;
    r->wire.mem = q.mem;
    e = vmem_deadline(end);
    if (!e) e = x9_vdec_dma_slice_publish(d,&r->slice,&extent);
    if (!e) {
        r->wire.mem.iova = extent.iova;
        r->wire.mem.dma_address = extent.iova;
        r->wire.mem.allocation_token = r->slice.token;
        e = vmem_deadline(end);
    }
    if (!e) {
        out->iova = extent.iova;
        out->allocation_token = r->slice.token;
        out->allocation_bytes = extent.capacity;
    }
done:
    /* A partial allocation is not a recoverable ENOMEM response. Preserve
     * its journal AND stop new allocations even if a caller ignores the
     * error; only the existing teardown/drain path may dispose of it. */
    if (e && r && r->slice.token && !m->fault_errno) m->fault_errno = e;
    lck_mtx_unlock(&m->lock);
    return e;
}
int x9_vdec_memory_match_free(struct x9_vdec_memory *m,
    const struct x9_vcp_mem_request *request, struct x9_vdec_dma_slice *out)
{
    struct x9_vcp_mem_request q;
    int e = ENOENT;
    if (!out) return EINVAL;
    if (!request) { memset(out,0,sizeof(*out)); return EINVAL; }
    q = *request; memset(out,0,sizeof(*out));
    if (!m || m->magic != VMEM_MAGIC) return EINVAL;
    lck_mtx_lock(&m->lock);
    if (m->closing) e = EBUSY;
    else if (q.instance_token != m->init.instance_token) e = ESTALE;
    else for (unsigned i = 0; i < X9_VDEC_DMA_MAX_SLICES; i++) {
        struct vmem_record *r = &m->records[i];
        if (r->slice.token && !x9_vcp_match_mem_free(&q,&r->wire)) {
            *out = r->slice; e = 0; break;
        }
    }
    lck_mtx_unlock(&m->lock);
    return e;
}
int x9_vdec_memory_discard(struct x9_vdec_memory **slot)
{
    struct x9_vdec_memory *m;
    int e = 0;
    if (!slot || !(m = *slot) || m->magic != VMEM_MAGIC) return EINVAL;
    lck_mtx_lock(&m->lock);
    if (m->control_refs) { lck_mtx_unlock(&m->lock); return EBUSY; }
    m->closing = true;
    for (unsigned i = 0; i < X9_VDEC_DMA_MAX_SLICES; i++) {
        struct vmem_record *r = &m->records[i];
        if (!r->slice.token) continue;
        e = x9_vdec_dma_slice_free(m->arena[r->arena],&r->slice);
        if (e) break; /* retain partial cleanup state and both arena refs */
        memset(r,0,sizeof(*r));
    }
    if (!e) {
        vmem_drop_arena(m->arena[1]); vmem_drop_arena(m->arena[0]);
        m->magic = 0; *slot = NULL;
    }
    lck_mtx_unlock(&m->lock);
    if (!e) {
        lck_mtx_destroy(&m->lock,&vdec_group);
        kfree_type(struct vmem_record,X9_VDEC_DMA_MAX_SLICES,m->records);
        kfree_type(struct x9_vdec_memory,m);
    }
    return e;
}
