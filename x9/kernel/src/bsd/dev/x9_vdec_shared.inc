/* Included after DMA+instance memory. Global SHM has a native Normal-NC CPU
 * mapping. Observation does not grant retirement or pixel completion. */
#include <pexpert/device_tree.h>
#include "x9_vcp_handoff.h"
#include "x9_vdec_shared.h"
#define VSHARED_MAGIC UINT64_C(0x583953484d303031)
struct x9_vdec_shared {
    uint64_t magic, epoch;
    lck_mtx_t lock;
    struct x9_vdec_dma *arena;
    struct x9_vdec_dma *codec;
    struct x9_vdec_dma_slice slice;
    struct x9_vdec_dma_slice vp[2][3];
    struct x9_vdec_dma_extent vp_extent[2][3];
    struct x9_vcp_shm_mapping service;
    struct x9_vcp_shm_region global, logger;
    int fault_errno;
    uint32_t control_refs, bound_slots;
    bool published, offered, closing;
};
static int vshared_current_layout(uint32_t *multiplier)
{
    struct x9_vcp_handoff h;
    DTEntry chosen;
    const void *p = NULL;
    unsigned int bytes = 0;
    if (SecureDTLookupEntry(NULL,"/chosen",&chosen) != kSuccess ||
        SecureDTGetProperty(chosen,"x9-vcp-handoff",&p,&bytes) != kSuccess ||
        !p || bytes != sizeof(h)) return ENXIO;
    memcpy(&h,p,sizeof(h));
    if (!x9_vcp_handoff_valid(&h)) return ENXIO;
    *multiplier = h.vp_multiplier;
    return 0;
}
static void vshared_put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8*i)); }
static void vshared_put64(uint8_t *p, uint64_t v)
{ for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8*i)); }
static void vshared_drop(struct x9_vdec_shared *s)
{
    struct x9_vdec_dma *d = s->arena;
    lck_mtx_lock(&d->lock);
    if (!d->shared_claim || !d->client_refs) panic("X9 VDEC: lost global SHM claim");
    d->shared_claim = false; d->client_refs--;
    lck_mtx_unlock(&d->lock);
    vmem_drop_arena(s->codec);
    s->magic = 0;
    lck_mtx_destroy(&s->lock,&vdec_group);
    kfree_type(struct x9_vdec_shared,s);
}
static int vshared_free_slices(struct x9_vdec_shared *s)
{
    int e;
    for (unsigned group = 0; group < 2; group++)
        for (unsigned kind = 0; kind < 3; kind++) {
            if (!s->vp[group][kind].token) continue;
            e = x9_vdec_dma_slice_free(s->codec,&s->vp[group][kind]);
            if (e) return e;
        }
    return s->slice.token ? x9_vdec_dma_slice_free(s->arena,&s->slice) : 0;
}
static int vshared_prepare_vp(struct x9_vdec_shared *s, uint32_t multiplier)
{
    struct x9_vcp_vp_layout plan;
    struct x9_vcp_vp_pattern pattern;
    uint8_t image[X9_VCP_VP_PATTERN_BYTES + 16];
    int e;
    /* CURRENT profile is multiplier1, so each auxiliary buffer is 208B.
     * Native slice allocation zeros ALL 16K capacity, including the stock
     * untouched 10-bit16B tail. Never import old heap bytes as a pattern. */
    if (multiplier != 1) return ENOTSUP;
    for (unsigned group = 0; group < 2; group++) {
        e = x9_vcp_vp_layout(X9_VCP_VP_ABI_MT6993_CAPTURE1,group ? 10 : 8,
            multiplier,&plan);
        if (e) return e;
        if (plan.buffer_bytes != sizeof(image)) return EINVAL;
        for (unsigned kind = 0; kind < 3; kind++) {
            memset(image,0,sizeof(image));
            e = x9_vcp_vp_pattern(X9_VCP_VP_ABI_MT6993_CAPTURE1,
                group ? 10 : 8,kind,&pattern);
            if (e) return e;
            if (pattern.bytes != plan.pattern_bytes[kind]) return EINVAL;
            for (unsigned n = 0; n < plan.repetitions[kind]; n++)
                memcpy(image+n*pattern.bytes,pattern.data,pattern.bytes);
            e = x9_vdec_dma_slice_alloc(s->codec,s->epoch,plan.buffer_bytes,128,
                &s->vp[group][kind]);
            if (!e) e = x9_vdec_dma_slice_write(s->codec,&s->vp[group][kind],
                0,image,sizeof(image));
            if (e) return e;
        }
    }
    return 0;
}
int x9_vdec_shared_create(struct x9_vdec_dma *d, struct x9_vdec_dma *codec,
    struct x9_vdec_shared **out)
{
    struct x9_vdec_shared *s;
    uint8_t common[X9_VCP_COMMON_PREFIX_BYTES], logger[32] = {0};
    uint32_t multiplier = 0;
    int e;
    if (!d || d->magic != VDMA_MAGIC || !codec || codec == d || !out || *out)
        return EINVAL;
    e = vshared_current_layout(&multiplier);
    if (e) return e;
    e = x9_vcp_common_cold_image(X9_VCP_COMMON_ABI_MT6993_CAPTURE1,common);
    if (e) return e;
    s = kalloc_type(struct x9_vdec_shared,Z_WAITOK | Z_ZERO);
    if (!s) return ENOMEM;
    e = vmem_hold_arena(codec,X9_MM_SMMU_CODEC_SID,d->request.epoch,
        d->request.power_token);
    if (e) { kfree_type(struct x9_vdec_shared,s); return e; }
    lck_mtx_init(&s->lock,&vdec_group,LCK_ATTR_NULL);
    lck_mtx_lock(&d->lock);
    e = d->request.sid != X9_MM_SMMU_HFRP_SID ? EINVAL :
        d->state != X9_MM_SMMU_UNEXPOSED || d->legacy_staged || d->shared_claim ? EBUSY :
        d->client_refs == UINT32_MAX ? EOVERFLOW : 0;
    if (!e) { d->shared_claim = true; d->client_refs++; }
    lck_mtx_unlock(&d->lock);
    if (e) {
        vmem_drop_arena(codec);
        lck_mtx_destroy(&s->lock,&vdec_group);
        kfree_type(struct x9_vdec_shared,s); return e;
    }
    s->magic = VSHARED_MAGIC; s->arena = d; s->codec = codec;
    s->epoch = d->request.epoch;
    /* 4MiB alignment keeps this 0x20e000-byte global block wholly within one
     * low27 firmware alias window, regardless of arena's starting offset. */
    e = x9_vdec_dma_slice_alloc(d,s->epoch,X9_VCP_GLOBAL_BYTES,0x400000,&s->slice);
    if (!e) {
        struct vdma_slice_record *r = NULL;
        lck_mtx_lock(&d->lock);
        e = vdma_slice_find(d,&s->slice,&r);
        if (!e) e = vdma_slice_make_nc(d,r);
        lck_mtx_unlock(&d->lock);
    }
    if (!e) e = x9_vdec_dma_slice_write(d,&s->slice,
        X9_VCP_GLOBAL_VDEC_OFFSET,common,sizeof(common));
    /* Paired KO vcp_logger_init aad4..ab10: magic, size24, enable0,
     * info_ofs24, buff_ofs32, buff_size1MiB, r/w indices0. This stages the
     * cold bytes only, not logger IPI registration or a live reset. */
    vshared_put32(logger,0x504c5402u); vshared_put32(logger+4,24);
    vshared_put32(logger+12,24); vshared_put32(logger+16,32);
    vshared_put32(logger+20,0x100000);
    if (!e) e = x9_vdec_dma_slice_write(d,&s->slice,
        X9_VCP_GLOBAL_LOGGER_OFFSET,logger,sizeof(logger));
    if (!e) e = vshared_prepare_vp(s,multiplier);
    if (e) {
        if (vshared_free_slices(s)) {
            s->fault_errno = e; *out = s; return e; /* retained cleanup handle */
        }
        vshared_drop(s); return e;
    }
    *out = s;
    return 0;
}
int x9_vdec_shared_publish(struct x9_vdec_shared *s,
    struct x9_vcp_shm_region *global, struct x9_vcp_shm_region *logger)
{
    struct x9_vdec_dma_extent extent = {0};
    struct x9_vcp_shm_request q = {X9_VCP_VDEC_SERVICE,0,UINT64_MAX};
    uint8_t reply[X9_VCP_WIRE_BYTES];
    uint8_t addresses[48] = {0}, flags[2] = {0};
    int e;
    if (!global || !logger || global == logger) return EINVAL;
    memset(global,0,sizeof(*global)); memset(logger,0,sizeof(*logger));
    if (!s || s->magic != VSHARED_MAGIC) return EINVAL;
    lck_mtx_lock(&s->lock);
    e = s->closing || s->published ? EBUSY : s->fault_errno;
    for (unsigned group = 0; group < 2 && !e; group++) {
        for (unsigned kind = 0; kind < 3 && !e; kind++) {
            e = x9_vdec_dma_slice_publish(s->codec,&s->vp[group][kind],
                &s->vp_extent[group][kind]);
            if (!e) vshared_put64(addresses+group*24+kind*8,
                s->vp_extent[group][kind].iova);
        }
        if (!e) flags[group] = 1;
    }
    /* No firmware sees the cold common yet. Address bytes precede flags;
     * all six PoC handoffs precede the global publication/boot GPR export. */
    if (!e) e = x9_vdec_dma_slice_write(s->arena,&s->slice,
        X9_VCP_GLOBAL_VDEC_OFFSET+X9_VCP_COMMON_VP_IOVA_OFFSET,addresses,sizeof(addresses));
    if (!e) e = x9_vdec_dma_slice_write(s->arena,&s->slice,
        X9_VCP_GLOBAL_VDEC_OFFSET+X9_VCP_COMMON_VP_FLAGS_OFFSET,flags,sizeof(flags));
    if (!e) e = x9_vdec_dma_slice_publish(s->arena,&s->slice,&extent);
    if (!e) {
        /* Persist ownership before deriving/exporting ANY hardware alias. */
        s->published = true;
        s->global = (struct x9_vcp_shm_region){extent.iova,X9_VCP_GLOBAL_BYTES};
        s->logger = (struct x9_vcp_shm_region){extent.iova+X9_VCP_GLOBAL_LOGGER_OFFSET,
            X9_VCP_GLOBAL_LOGGER_BYTES};
        s->service.vdec = (struct x9_vcp_shm_region){extent.iova+X9_VCP_GLOBAL_VDEC_OFFSET,
            X9_VCP_GLOBAL_VDEC_BYTES};
        s->service.prop = (struct x9_vcp_shm_region){extent.iova+X9_VCP_GLOBAL_PROP_OFFSET,
            X9_VCP_GLOBAL_TEXT_BYTES};
        s->service.log = (struct x9_vcp_shm_region){extent.iova+X9_VCP_GLOBAL_LOG_INFO_OFFSET,
            X9_VCP_GLOBAL_TEXT_BYTES};
        s->service.cpu_token = s->slice.token;
        e = x9_vcp_encode_shm_reply(&q,&s->service,reply); /* shape/bounds only */
        if (!e) { *global = s->global; *logger = s->logger; }
    }
    if (e && !s->fault_errno) s->fault_errno = e;
    lck_mtx_unlock(&s->lock);
    return e;
}
int x9_vdec_shared_reply(void *cookie, uint64_t epoch, uint64_t end,
    const struct x9_vcp_shm_request *request, struct x9_vcp_shm_mapping *out)
{
    struct x9_vdec_shared *s = cookie;
    struct x9_vcp_shm_request q;
    uint32_t state = 0;
    uint8_t reply[X9_VCP_WIRE_BYTES];
    int e;
    if (!out) return EINVAL;
    if (!request) { memset(out,0,sizeof(*out)); return EINVAL; }
    q = *request; memset(out,0,sizeof(*out));
    if (!s || s->magic != VSHARED_MAGIC) return EINVAL;
    lck_mtx_lock(&s->lock);
    e = epoch != s->epoch ? ESTALE : s->closing || !s->published || s->offered ? EBUSY :
        s->fault_errno;
    if (!e) e = vmem_deadline(end);
    if (!e) e = x9_vdec_dma_slice_state(s->arena,&s->slice,&state);
    if (!e && state != X9_VDEC_DMA_DEVICE) e = EBUSY;
    if (!e) e = x9_vcp_encode_shm_reply(&q,&s->service,reply);
    if (!e) e = vmem_deadline(end);
    if (!e) { s->offered = true; *out = s->service; }
    if (e && !s->fault_errno) s->fault_errno = e;
    lck_mtx_unlock(&s->lock);
    return e;
}
/* Internal, read-only and restricted to THIS published global allocation.
 * No generic DEVICE slice read API is exported. The complete global slice
 * was made Normal NC before publication, including all tracked CPU aliases.
 * No cacheline clean/invalidate against firmware-updated lines is needed.
 * It proves visibility, not atomicity of a multiword update or retirement.
 * Caller holds shared lock and active native bus transaction/lifetime. */
/* shared lock AND arena lock held. Returns only the known NC window in this
 * retained DEVICE slice; never an exported pointer or firmware address cast.
 * The control layer, not this helper, restricts stores to AP-owned fields. */
static int vshared_live_window(struct x9_vdec_shared *s, uint32_t offset,
    uint32_t bytes, volatile uint8_t **out)
{
    struct x9_vdec_dma *d = s->arena;
    struct vdma_slice_record *r = NULL;
    uint32_t first, limit;
    int e;
    *out = NULL;
    if (!bytes || offset >= s->slice.bytes || bytes > s->slice.bytes-offset)
        return ERANGE;
    e = s->closing || !s->published || !s->offered ? EBUSY : s->fault_errno;
    if (!e) e = vdma_slice_find(d,&s->slice,&r);
    if (!e && (!d->shared_claim || d->state != X9_MM_SMMU_INSTALLED ||
        r->state != X9_VDEC_DMA_DEVICE)) e = EBUSY;
    if (!e) e = vdma_slice_cpu_domain(d);
    if (!e) e = vdma_slice_nc_check(d,r);
    if (!e) {
        first = (r->offset+offset) & ~(VDMA_CPU_BYTES-1);
        limit = (r->offset+offset+bytes+VDMA_CPU_BYTES-1) & ~(VDMA_CPU_BYTES-1);
        e = vdma_slice_backing(d,first,limit-first);
    }
    if (!e) *out = (volatile uint8_t *)(d->raw+r->offset+offset);
    return e;
}
static int vshared_observe(struct x9_vdec_shared *s, uint32_t offset,
    uint8_t *out, uint32_t bytes)
{
    struct x9_vdec_dma *d = s->arena;
    volatile uint8_t *window = NULL;
    int e;
    if (!out || !bytes || offset >= s->slice.bytes || bytes > s->slice.bytes-offset)
        return ERANGE;
    lck_mtx_lock(&d->lock);
    e = vshared_live_window(s,offset,bytes,&window);
    if (!e) {
        __asm__ volatile("dmb oshld" ::: "memory");
        /* Keep volatile NC loads explicit; snapshot consistency is checked
         * by control, never inferred from an optimized bulk copy. */
        for (uint32_t n = 0; n < bytes; n++) out[n] = window[n];
        __asm__ volatile("dmb oshld" ::: "memory");
    }
    lck_mtx_unlock(&d->lock);
    return e;
}
int x9_vdec_shared_discard(struct x9_vdec_shared **slot)
{
    struct x9_vdec_shared *s;
    int e;
    if (!slot || !(s = *slot) || s->magic != VSHARED_MAGIC) return EINVAL;
    lck_mtx_lock(&s->lock);
    if (s->control_refs) { lck_mtx_unlock(&s->lock); return EBUSY; }
    s->closing = true;
    e = vshared_free_slices(s);
    if (!e) *slot = NULL;
    lck_mtx_unlock(&s->lock);
    if (!e) vshared_drop(s);
    return e;
}
