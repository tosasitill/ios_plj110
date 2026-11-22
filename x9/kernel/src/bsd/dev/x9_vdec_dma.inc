/* Actual XNU wired allocation, PA lookup and private-memory/cache adapter.
 * Ordinary 4K SMMU pages are NOT XNU's 16K CPU allocation granule. */
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <arm/caches_internal.h>
#include <arm64/proc_reg.h>
#include "x9_vdec_dma.h"
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
/* Kernel-private signatures from osfmk/vm/pmap.h (not exported to kexts). */
extern unsigned int pmap_cache_attributes(ppnum_t);
extern void pmap_set_cache_attributes(ppnum_t, unsigned int);
#define VDMA_MAGIC UINT64_C(0x583956444d413031)
#define VDMA_CPU_BYTES 16384u
#define VDMA_RAW_PAGES (X9_MM_SMMU_MAX_CHECK_BYTES / VDMA_CPU_BYTES)
struct vdma_physical_page {
    uint64_t pa;
    uint32_t page, reserved;
};
struct vdma_slice_record {
    struct x9_vdec_dma_slice handle;
    uint32_t offset, state;
    /* Only native global SHM uses Normal NC. Records the transition before
     * the first pmap change, including a partially failed preparation. */
    bool uncached;
};
struct x9_vdec_dma {
    uint64_t magic;
    lck_mtx_t lock;
    struct x9_vdec_dma_request request;
    struct x9_mm_smmu_private_mapping map;
    struct x9_mm_smmu_dma_span *spans;
    struct x9_mm_smmu_table_page *pages;
    struct vdma_physical_page *raw_index; /* sorted PA -> retained CPU page */
    uint64_t *raw_pa, *pool_pa, cd_pa;
    vm_offset_t raw, pool, cd;
    uint64_t power_child;
    uint32_t raw_bytes, raw_pages, state, last_errno;
    uint32_t span_capacity, table_capacity, pool_bytes, pool_pages, bitmap_words;
    struct vdma_slice_record *slices;
    uint64_t *occupied;
    uint32_t slice_count, client_refs;
    bool sliced, legacy_staged, shared_claim;
};
LCK_MTX_DECLARE(vdma_namespace_lock, &vdec_group);
static struct x9_vdec_dma *vdma_domains[2]; /* HFRP a0, codec b0 */
static uint64_t vdma_next_slice_token = 1; /* protected by namespace_lock */
static unsigned vdma_domain_index(uint32_t sid)
{ return sid == X9_MM_SMMU_HFRP_SID ? 0u : 1u; }
static uint64_t vdma_take_token(void)
{
    uint64_t token = 0;
    lck_mtx_lock(&vdma_namespace_lock);
    if (vdma_next_slice_token && vdma_next_slice_token != UINT64_MAX)
        token = vdma_next_slice_token++;
    lck_mtx_unlock(&vdma_namespace_lock);
    return token;
}
struct vdma_io {
    struct x9_vdec_dma *dma;
    const struct x9_mm_smmu_install_io *platform;
    bool retire;
};
static void vdma_reclaim_retired_slices(struct x9_vdec_dma *);

static int vdma_phys(vm_offset_t va, unsigned bits, uint64_t *out)
{
    ppnum_t first, last;
    uint64_t pa, limit;
    if (!va || (va & (VDMA_CPU_BYTES - 1)) || PAGE_SHIFT != 14 ||
        bits < 32 || bits > 48) return EINVAL;
    first = pmap_find_phys(kernel_pmap, (addr64_t)va);
    last = pmap_find_phys(kernel_pmap, (addr64_t)(va + VDMA_CPU_BYTES - 1));
    if (!first || first != last) return EFAULT;
    pa = ptoa_64(first); limit = UINT64_C(1) << bits;
    if ((pa & (VDMA_CPU_BYTES - 1)) || pa >= limit || VDMA_CPU_BYTES > limit - pa)
        return ERANGE;
    *out = pa;
    return 0;
}

static int vdma_alloc(vm_offset_t *va, uint32_t bytes)
{
    return kmem_alloc(kernel_map, va, bytes, KMA_DATA | KMA_ZERO,
                        VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS ? 0 : ENOMEM;
}

/* In-place heap sort of a PRIVATE native index only. No sorting/rewriting
 * ordered DMA spans, recursive kernel stack use or external sort dependency. */
static void vdma_index_sift(struct vdma_physical_page *a, uint32_t count, uint32_t root)
{
    while (root < count / 2) {
        uint32_t child = 2 * root + 1;
        struct vdma_physical_page tmp;
        if (child + 1 < count && a[child].pa < a[child + 1].pa) child++;
        if (a[root].pa >= a[child].pa) break;
        tmp = a[root]; a[root] = a[child]; a[child] = tmp;
        root = child;
    }
}
static void vdma_index_sort(struct x9_vdec_dma *d)
{
    uint32_t count = d->raw_pages;
    for (uint32_t i = count / 2; i; i--) vdma_index_sift(d->raw_index,count,i-1);
    for (uint32_t i = count; i > 1; i--) {
        struct vdma_physical_page tmp = d->raw_index[0];
        d->raw_index[0] = d->raw_index[i-1]; d->raw_index[i-1] = tmp;
        vdma_index_sift(d->raw_index,i-1,0);
    }
}
static const struct vdma_physical_page *vdma_index_find(const struct x9_vdec_dma *d,
    uint64_t pa)
{
    uint32_t lo = 0, hi = d->raw_pages;
    while (lo < hi) {
        uint32_t mid = lo + (hi-lo) / 2;
        if (d->raw_index[mid].pa <= pa) lo = mid+1;
        else hi = mid;
    }
    if (!lo || pa-d->raw_index[lo-1].pa >= VDMA_CPU_BYTES) return NULL;
    return &d->raw_index[lo-1];
}

static void vdma_free(struct x9_vdec_dma *d)
{
    unsigned domain = vdma_domain_index(d->request.sid);
    if (d->power_child) panic("X9 VDEC: attempted to free pinned MM child");
    if (d->slice_count || d->client_refs || d->shared_claim)
        panic("X9 VDEC: attempted to free referenced DMA arena");
    if (d->state != X9_MM_SMMU_UNEXPOSED && d->state != X9_MM_SMMU_RETIRED)
        panic("X9 VDEC: attempted to free exposed DMA storage");
    if (d->raw) kmem_free(kernel_map, d->raw, d->raw_bytes);
    if (d->pool) kmem_free(kernel_map, d->pool, d->pool_bytes);
    if (d->cd) kmem_free(kernel_map, d->cd, VDMA_CPU_BYTES);
    if (d->slices) kfree_type(struct vdma_slice_record, X9_VDEC_DMA_MAX_SLICES, d->slices);
    if (d->spans) kfree_type(struct x9_mm_smmu_dma_span,d->span_capacity,d->spans);
    if (d->pages) kfree_type(struct x9_mm_smmu_table_page,d->table_capacity,d->pages);
    if (d->raw_index) kfree_type(struct vdma_physical_page,d->raw_pages,d->raw_index);
    if (d->raw_pa) kfree_type(uint64_t,d->raw_pages,d->raw_pa);
    if (d->pool_pa) kfree_type(uint64_t,d->pool_pages,d->pool_pa);
    if (d->occupied) kfree_type(uint64_t,d->bitmap_words,d->occupied);
    lck_mtx_lock(&vdma_namespace_lock);
    if (vdma_domains[domain] == d) vdma_domains[domain] = NULL;
    lck_mtx_unlock(&vdma_namespace_lock);
    d->magic = 0;
    lck_mtx_destroy(&d->lock, &vdec_group);
    kfree_type(struct x9_vdec_dma, d);
}

static int vdma_backing(struct x9_vdec_dma *d)
{
    uint64_t pa = 0;
    uint32_t bytes = 0;
    uint32_t tables = 0;
    unsigned bits = d->map.context.table.output_bits;
    int e;
    if (d->magic != VDMA_MAGIC || !d->raw || !d->pool || !d->cd || !d->slices ||
        !d->spans || !d->pages || !d->raw_pa || !d->pool_pa || !d->occupied ||
        !d->raw_index ||
        !d->raw_pages || d->raw_pages > VDMA_RAW_PAGES ||
        d->raw_bytes != d->raw_pages * VDMA_CPU_BYTES ||
        d->raw_bytes != d->map.total_bytes || d->map.spans != d->spans ||
        d->map.pages != d->pages || !d->map.span_count ||
        d->span_capacity != (d->raw_pages < X9_MM_SMMU_MAX_DMA_SPANS ?
            d->raw_pages : X9_MM_SMMU_MAX_DMA_SPANS) ||
        d->map.span_count > d->span_capacity ||
        !d->map.page_count || d->map.page_count > X9_MM_SMMU_BUILD_MAX_TABLES ||
        d->map.context.request.cd_pa != d->cd_pa) return EFAULT;
    e = x9_mm_smmu_stage1_geometry(&d->map.query,d->raw_bytes,&tables);
    if (e || d->table_capacity != tables || d->map.page_count != tables ||
        d->pool_bytes != ((tables*4096+VDMA_CPU_BYTES-1) & ~(VDMA_CPU_BYTES-1)) ||
        d->pool_pages != d->pool_bytes / VDMA_CPU_BYTES ||
        d->bitmap_words != (d->raw_pages+63) / 64) return EFAULT;
    e = vdma_phys(d->cd, bits, &pa);
    if (!e && pa != d->cd_pa) e = ESTALE;
    for (unsigned i = 0; i < d->pool_pages && !e; i++) {
        e = vdma_phys(d->pool + (vm_offset_t)i * VDMA_CPU_BYTES, bits, &pa);
        if (!e && pa != d->pool_pa[i]) e = ESTALE;
        if (!e && pa == d->cd_pa) e = EFAULT;
        for (unsigned j = 0; j < i && !e; j++) if (pa == d->pool_pa[j]) e = EFAULT;
    }
    for (unsigned i = 0; i < d->table_capacity && !e; i++)
        if (d->pages[i].pa != d->pool_pa[i / 4] + (uint64_t)(i & 3) * 4096 ||
            d->pages[i].host != (uint8_t *)(d->pool + (vm_offset_t)i * 4096)) e = EFAULT;
    /* A strictly sorted, complete index proves uniqueness in linear time.
     * Validate its native page identities before using it for PA lookup. */
    for (unsigned i = 0; i < d->raw_pages && !e; i++) {
        const struct vdma_physical_page *p = &d->raw_index[i];
        if (p->reserved || p->page >= d->raw_pages || p->pa != d->raw_pa[p->page] ||
            (i && p->pa <= d->raw_index[i-1].pa)) e = EFAULT;
    }
    for (unsigned i = 0; i < d->raw_pages && !e; i++) {
        e = vdma_phys(d->raw + (vm_offset_t)i * VDMA_CPU_BYTES, bits, &pa);
        if (!e && pa != d->raw_pa[i]) e = ESTALE;
        if (!e && pa == d->cd_pa) e = EFAULT;
        for (unsigned j = 0; j < d->pool_pages && !e; j++)
            if (pa == d->pool_pa[j]) e = EFAULT;
    }
    /* Match each actual CPU page to the retained coalesced scatter list, not
     * merely to the first physical address of a virtual allocation. */
    for (unsigned s = 0; s < d->map.span_count && !e; s++) {
        const struct x9_mm_smmu_dma_span *span = &d->spans[s];
        if (span->reserved || !span->bytes || (span->bytes & (VDMA_CPU_BYTES - 1)) ||
            span->bytes > d->raw_bytes - bytes) { e = EFAULT; break; }
        for (uint32_t off = 0; off < span->bytes; off += VDMA_CPU_BYTES)
            if (d->raw_pa[(bytes + off) / VDMA_CPU_BYTES] != span->expected_pa + off) {
                e = EFAULT; break;
            }
        bytes += span->bytes;
    }
    return e ? e : bytes == d->raw_bytes ? 0 : EFAULT;
}

int x9_vdec_dma_create(const struct x9_vdec_dma_request *r, struct x9_vdec_dma **out)
{
    static const unsigned oas[] = {32, 36, 40, 42, 44, 48, 48};
    struct x9_vdec_dma *d;
    struct x9_mm_smmu_table_build tree;
    struct x9_mm_smmu_context_request context;
    unsigned encoding;
    int e;
    if (!r || !out || *out || !r->epoch || !r->power_token || !r->asid || !r->bytes ||
        r->bytes > X9_MM_SMMU_MAX_CHECK_BYTES || !r->iova ||
        (r->iova & (VDMA_CPU_BYTES - 1)))
        return EINVAL;
    if (r->sid != X9_MM_SMMU_HFRP_SID && r->sid != X9_MM_SMMU_CODEC_SID)
        return ENOTSUP;
    if (PAGE_SHIFT != 14 || !(r->regs.idr5 & 16)) return ENOTSUP;
    encoding = r->regs.idr5 & 7;
    if (encoding >= 7) return ENOTSUP;
    d = kalloc_type(struct x9_vdec_dma, Z_WAITOK | Z_ZERO);
    if (!d) return ENOMEM;
    d->magic = VDMA_MAGIC; d->request = *r;
    lck_mtx_init(&d->lock, &vdec_group, LCK_ATTR_NULL);
    /* Claim the SID before preparing any backing, not only when attempting
     * the STE store. All firmware allocations share the one arena's context.
     * This local namespace is not a substitute for the controller's SID lease. */
    lck_mtx_lock(&vdma_namespace_lock);
    if (vdma_domains[vdma_domain_index(r->sid)]) e = EBUSY;
    else {
        e = 0;
        for (unsigned i = 0; i < 2; i++)
            if (vdma_domains[i] && vdma_domains[i]->request.asid == r->asid) e = EBUSY;
        if (!e) vdma_domains[vdma_domain_index(r->sid)] = d;
    }
    lck_mtx_unlock(&vdma_namespace_lock);
    if (e) { vdma_free(d); return e; }
    /* Variable-sized typed storage: the full ledger must not inflate the
     * fixed arena object beyond XNU's KALLOC_SAFE_ALLOC_SIZE. */
    d->slices = kalloc_type(struct vdma_slice_record, X9_VDEC_DMA_MAX_SLICES,
        Z_WAITOK | Z_ZERO);
    if (!d->slices) { vdma_free(d); return ENOMEM; }
    d->raw_bytes = (r->bytes + VDMA_CPU_BYTES - 1) & ~(VDMA_CPU_BYTES - 1);
    d->raw_pages = d->raw_bytes / VDMA_CPU_BYTES;
    d->map.query = (struct x9_mm_smmu_query){ .sid = r->sid, .iova = r->iova };
    e = x9_mm_smmu_stage1_geometry(&d->map.query,d->raw_bytes,&d->table_capacity);
    if (e) { vdma_free(d); return e; }
    d->span_capacity = d->raw_pages < X9_MM_SMMU_MAX_DMA_SPANS ?
        d->raw_pages : X9_MM_SMMU_MAX_DMA_SPANS;
    d->pool_bytes = (d->table_capacity*4096+VDMA_CPU_BYTES-1) & ~(VDMA_CPU_BYTES-1);
    d->pool_pages = d->pool_bytes / VDMA_CPU_BYTES;
    d->bitmap_words = (d->raw_pages+63) / 64;
    /* All lengths are recorded before allocation for partial-error cleanup.
     * Even a large codec arena keeps the fixed owner small; no maximum-sized
     * typed object or table/page array lives on the native kernel stack. */
    d->spans = kalloc_type(struct x9_mm_smmu_dma_span,d->span_capacity,Z_WAITOK | Z_ZERO);
    d->pages = kalloc_type(struct x9_mm_smmu_table_page,d->table_capacity,Z_WAITOK | Z_ZERO);
    d->raw_pa = kalloc_type(uint64_t,d->raw_pages,Z_WAITOK | Z_ZERO);
    d->raw_index = kalloc_type(struct vdma_physical_page,d->raw_pages,Z_WAITOK | Z_ZERO);
    d->pool_pa = kalloc_type(uint64_t,d->pool_pages,Z_WAITOK | Z_ZERO);
    d->occupied = kalloc_type(uint64_t,d->bitmap_words,Z_WAITOK | Z_ZERO);
    if (!d->spans || !d->pages || !d->raw_pa || !d->raw_index ||
        !d->pool_pa || !d->occupied) { vdma_free(d); return ENOMEM; }
    d->map.spans = d->spans; d->map.pages = d->pages;
    d->map.total_bytes = d->raw_bytes; d->map.access = r->access;
    e = vdma_alloc(&d->raw, d->raw_bytes);
    if (!e) e = vdma_alloc(&d->pool, d->pool_bytes);
    if (!e) e = vdma_alloc(&d->cd, VDMA_CPU_BYTES);
    if (!e) e = vdma_phys(d->cd, oas[encoding], &d->cd_pa);
    for (unsigned i = 0; i < d->pool_pages && !e; i++)
        e = vdma_phys(d->pool + (vm_offset_t)i * VDMA_CPU_BYTES,
                        oas[encoding], &d->pool_pa[i]);
    for (unsigned i = 0; i < d->table_capacity && !e; i++) {
        d->pages[i].pa = d->pool_pa[i / 4] + (uint64_t)(i & 3) * 4096;
        d->pages[i].host = (uint8_t *)(d->pool + (vm_offset_t)i * 4096);
    }
    for (unsigned i = 0; i < d->raw_pages && !e; i++) {
        unsigned n = d->map.span_count;
        e = vdma_phys(d->raw + (vm_offset_t)i * VDMA_CPU_BYTES,
                        oas[encoding], &d->raw_pa[i]);
        if (e) break;
        d->raw_index[i] = (struct vdma_physical_page){d->raw_pa[i],i,0};
        if (n && d->spans[n - 1].expected_pa + d->spans[n - 1].bytes == d->raw_pa[i])
            d->spans[n - 1].bytes += VDMA_CPU_BYTES;
        else if (n < d->span_capacity) {
            d->spans[n] = (struct x9_mm_smmu_dma_span){ d->raw_pa[i], VDMA_CPU_BYTES, 0 };
            d->map.span_count++;
        } else e = ENOSPC; /* bounded scatter fragmentation, not an IOVA alias */
    }
    if (!e) vdma_index_sort(d);
    if (!e) e = x9_mm_smmu_build_stage1(X9_MM_SMMU_STAGE1_4K36_ABI, &d->map.query,
        d->spans, d->map.span_count, d->raw_bytes, r->access, oas[encoding], d->pages,
        d->table_capacity, &tree);
    context = (struct x9_mm_smmu_context_request){ r->epoch, d->cd_pa, r->sid, r->asid };
    if (!e) e = x9_mm_smmu_build_context(X9_MM_SMMU_CONTEXT_ABI, &r->regs,
                                           &context, &tree, &d->map.context);
    if (!e) { d->map.page_count = tree.table_count; e = vdma_backing(d); }
    if (e) { vdma_free(d); return e; }
    *out = d;
    return 0;
}

/* Resolve ONLY this wired object's PAs to its independent host mappings.
 * No cast from a physical address, and no inference of virtual contiguity. */
static int vdma_resolve(struct x9_vdec_dma *d, uint64_t pa, uint32_t bytes,
                          vm_offset_t *va, unsigned *kind)
{
    if (!bytes || pa > UINT64_MAX - bytes) return EINVAL;
    if (pa >= d->cd_pa && pa - d->cd_pa < VDMA_CPU_BYTES &&
        bytes <= VDMA_CPU_BYTES - (pa - d->cd_pa)) {
        *va = d->cd + (vm_offset_t)(pa - d->cd_pa); *kind = 1; return 0;
    }
    for (unsigned i = 0; i < d->pool_pages; i++)
        if (pa >= d->pool_pa[i] && pa - d->pool_pa[i] < VDMA_CPU_BYTES &&
            bytes <= VDMA_CPU_BYTES - (pa - d->pool_pa[i])) {
            *va = d->pool + (vm_offset_t)i * VDMA_CPU_BYTES +
                (vm_offset_t)(pa - d->pool_pa[i]); *kind = 2; return 0;
        }
    const struct vdma_physical_page *p = vdma_index_find(d,pa);
    if (p && bytes <= VDMA_CPU_BYTES - (pa-p->pa)) {
        *va = d->raw + (vm_offset_t)p->page * VDMA_CPU_BYTES +
            (vm_offset_t)(pa-p->pa); *kind = 3; return 0;
    }
    return ENOENT;
}

static int vdma_overlap(uint64_t start, uint64_t bytes, uint64_t pa)
{
    return start < pa + VDMA_CPU_BYTES && pa < start + bytes;
}
static int vdma_exclude_private(struct x9_vdec_dma *d, uint64_t start, uint64_t bytes)
{
    if (!bytes || start >= (UINT64_C(1) << 48) ||
        bytes > (UINT64_C(1) << 48) - start) return ERANGE;
    if (vdma_overlap(start, bytes, d->cd_pa)) return EACCES;
    for (unsigned i = 0; i < d->pool_pages; i++)
        if (vdma_overlap(start, bytes, d->pool_pa[i])) return EACCES;
    for (unsigned i = 0; i < d->raw_pages; i++)
        if (vdma_overlap(start, bytes, d->raw_pa[i])) return EACCES;
    return 0;
}
static int vdma_enter(void *cookie, const struct x9_mm_smmu_private_mapping *m)
{
    struct vdma_io *v = cookie;
    struct x9_mm_smmu_regs r = {0};
    int e = m != &v->dma->map ? EINVAL :
        x9_mm_power_validate_pin(v->dma->request.power_token,
                                  v->dma->power_child, X9_MM_POWER_VCP);
    if (!e) e = vdma_backing(v->dma);
    if (!e && (!v->platform->reader.range_count ||
        v->platform->reader.range_count > X9_MM_SMMU_MAX_RANGES)) e = EINVAL;
    for (unsigned i = 0; i < v->platform->reader.range_count && !e; i++)
        e = vdma_exclude_private(v->dma, v->platform->reader.ranges[i].start,
                                   v->platform->reader.ranges[i].bytes);
    if (e) return e;
    if (!e) e = v->platform->enter(v->platform->cookie, m);
    if (e) return e; /* platform enter must unwind its own failed acquisition */
    e = x9_mm_power_validate_pin(v->dma->request.power_token,
                                  v->dma->power_child, X9_MM_POWER_VCP);
    if (!e) e = v->platform->guard(v->platform->cookie, v->dma->request.epoch);
    if (!e) e = v->platform->read_regs(v->platform->cookie, &r);
    if (!e) {
        unsigned qbits = r.cmdq_base & 31;
        uint64_t queue = r.cmdq_base & UINT64_C(0x000fffffffffffe0);
        if (!qbits || qbits > 19) e = ENOTSUP;
        else e = vdma_exclude_private(v->dma, queue, UINT64_C(1) << (qbits + 4));
    }
    if (e) v->platform->leave(v->platform->cookie,
        v->retire ? X9_MM_SMMU_QUARANTINED : X9_MM_SMMU_UNEXPOSED);
    return e;
}
static int vdma_guard(void *cookie, uint64_t epoch)
{
    struct vdma_io *v = cookie;
    int e;
    if (epoch != v->dma->request.epoch) return ESTALE;
    e = x9_mm_power_validate_pin(v->dma->request.power_token,
                                      v->dma->power_child, X9_MM_POWER_VCP);
    if (e) return e;
    return v->platform->guard(v->platform->cookie, epoch);
}
static void vdma_leave(void *cookie, uint32_t state)
{
    struct vdma_io *v = cookie;
    v->platform->leave(v->platform->cookie, state);
}
static int vdma_regs(void *cookie, struct x9_mm_smmu_regs *r)
{
    struct vdma_io *v = cookie;
    return v->platform->read_regs(v->platform->cookie, r);
}
static int vdma_read(void *cookie, uint64_t pa, uint8_t *dst, uint32_t bytes)
{
    struct vdma_io *v = cookie;
    vm_offset_t va = 0; unsigned kind = 0;
    int e = vdma_resolve(v->dma, pa, bytes, &va, &kind);
    if (!e) {
        if (kind == 3) return EACCES; /* no DMA pixel/BS reads through table walker */
        memcpy(dst, (const void *)va, bytes); return 0;
    }
    if (e != ENOENT) return e;
    return v->platform->reader.read(v->platform->reader.cookie, pa, dst, bytes);
}
static int vdma_write(void *cookie, uint64_t pa, const uint8_t *src, uint32_t bytes)
{
    struct vdma_io *v = cookie;
    vm_offset_t va = 0; unsigned kind = 0;
    int e = vdma_resolve(v->dma, pa, bytes, &va, &kind);
    if (!e) {
        if (kind != 1) return EACCES; /* installer never rewrites private tree/BS */
        memcpy((void *)va, src, bytes); return 0;
    }
    if (e != ENOENT) return e;
    return v->platform->write(v->platform->cookie, pa, src, bytes);
}
static int vdma_store(void *cookie, uint64_t pa, uint64_t value)
{
    struct vdma_io *v = cookie;
    vm_offset_t va = 0; unsigned kind = 0;
    int e = vdma_resolve(v->dma, pa, 8, &va, &kind);
    if (!e) {
        if (kind != 1 || (va & 7) || (pa & 7)) return EACCES;
        __atomic_store_n((uint64_t *)va, value, __ATOMIC_RELEASE); return 0;
    }
    if (e != ENOENT) return e;
    return v->platform->store64(v->platform->cookie, pa, value);
}
static int vdma_clean(void *cookie, uint64_t pa, uint32_t bytes)
{
    struct vdma_io *v = cookie;
    vm_offset_t va = 0; unsigned kind = 0;
    int e = vdma_resolve(v->dma, pa, bytes, &va, &kind);
    if (!e) {
        if (kind == 3) return EACCES; /* raw initial cache sync is separate */
        clean_dcache64(pa, bytes, TRUE);
        __asm__ volatile("dsb sy" ::: "memory"); return 0;
    }
    if (e != ENOENT) return e;
    return v->platform->clean(v->platform->cookie, pa, bytes);
}
static int vdma_barrier(void *cookie)
{
    struct vdma_io *v = cookie;
    __asm__ volatile("dsb sy" ::: "memory");
    return v->platform->barrier(v->platform->cookie);
}
static int vdma_publish(void *cookie, uint32_t old, uint32_t next)
{
    struct vdma_io *v = cookie;
    return v->platform->publish(v->platform->cookie, old, next);
}
static uint64_t vdma_clock(void *cookie)
{
    struct vdma_io *v = cookie;
    return v->platform->clock_ns(v->platform->cookie);
}
static int vdma_pause(void *cookie, uint64_t deadline)
{
    struct vdma_io *v = cookie;
    return v->platform->pause(v->platform->cookie, deadline);
}

static int vdma_operation(struct x9_vdec_dma *d,
    const struct x9_mm_smmu_install_io *adapter, uint32_t deadline_ms, bool retire)
{
    struct x9_mm_smmu_install_result result = {0};
    struct vdma_io v = { d, adapter, retire };
    struct x9_mm_smmu_install_io io;
    int e;
    if (!d || d->magic != VDMA_MAGIC || !adapter || !adapter->enter ||
        !adapter->guard || !adapter->leave || !adapter->read_regs ||
        !adapter->write || !adapter->store64 || !adapter->clean ||
        !adapter->barrier || !adapter->publish || !adapter->clock_ns ||
        !adapter->pause || !adapter->reader.read || !deadline_ms || deadline_ms > 1000)
        return EINVAL;
    lck_mtx_lock(&d->lock);
    if (d->state != (retire ? X9_MM_SMMU_INSTALLED : X9_MM_SMMU_UNEXPOSED)) {
        e = EBUSY; goto done;
    }
    e = vdma_backing(d);
    if (e) {
        if (retire) d->state = X9_MM_SMMU_QUARANTINED;
        goto done;
    }
    /* Pin the actual native GET before the first PA/enter reaches any
     * platform callback. An exposed/uncertain mapping retains this child,
     * preventing a parent's PUT even between separate install/retire calls.
     * The reset/domain/endpoint authority remains the platform's job. */
    if (!retire && !d->power_child)
        e = x9_mm_power_pin(d->request.power_token, X9_MM_POWER_VCP, &d->power_child);
    else
        e = x9_mm_power_validate_pin(d->request.power_token, d->power_child,
                                      X9_MM_POWER_VCP);
    if (e) {
        if (retire) d->state = X9_MM_SMMU_QUARANTINED;
        goto done;
    }
    /* Eliminate dirty zero-filled/staged WB lines before the first device
     * mapping is published. Each real 16K PA is synced separately. */
    if (!retire) {
        for (unsigned i = 0; i < d->raw_pages; i++)
            flush_dcache64(d->raw_pa[i], VDMA_CPU_BYTES, TRUE);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    io = *adapter;
    io.cookie = &v; io.reader.cookie = &v; io.reader.read = vdma_read;
    io.enter = vdma_enter; io.guard = vdma_guard; io.leave = vdma_leave;
    io.read_regs = vdma_regs; io.write = vdma_write; io.store64 = vdma_store;
    io.clean = vdma_clean; io.barrier = vdma_barrier; io.publish = vdma_publish;
    io.clock_ns = vdma_clock; io.pause = vdma_pause;
    d->state = X9_MM_SMMU_QUARANTINED; /* pin before any PA reaches callbacks */
    e = retire ? x9_mm_smmu_retire_stage1(&d->map, &io, deadline_ms, &result) :
                 x9_mm_smmu_install_stage1(&d->map, &io, deadline_ms, &result);
    d->state = result.state;
    if (!e && retire && d->state == X9_MM_SMMU_RETIRED)
        vdma_reclaim_retired_slices(d);
    if (d->state == X9_MM_SMMU_UNEXPOSED || d->state == X9_MM_SMMU_RETIRED) {
        int off = x9_mm_power_unpin(d->request.power_token, &d->power_child,
                                     X9_MM_POWER_VCP);
        if (!e) e = off; /* preserve installer first error; retain failed pin */
    }
done:
    d->last_errno = (uint32_t)e;
    lck_mtx_unlock(&d->lock);
    return e;
}
int x9_vdec_dma_install(struct x9_vdec_dma *d, const struct x9_mm_smmu_install_io *io,
                         uint32_t deadline_ms)
{
    return vdma_operation(d, io, deadline_ms, false);
}
int x9_vdec_dma_retire(struct x9_vdec_dma *d, const struct x9_mm_smmu_install_io *io,
                        uint32_t deadline_ms)
{
    return vdma_operation(d, io, deadline_ms, true);
}
int x9_vdec_dma_write_private(struct x9_vdec_dma *d, uint32_t offset,
                               const uint8_t *src, uint32_t bytes)
{
    int e;
    if (!d || d->magic != VDMA_MAGIC || !src || !bytes) return EINVAL;
    lck_mtx_lock(&d->lock);
    e = d->state != X9_MM_SMMU_UNEXPOSED || d->sliced || d->client_refs ? EBUSY :
        offset > d->request.bytes || bytes > d->request.bytes - offset ? ERANGE : 0;
    if (!e) e = vdma_backing(d);
    if (!e) {
        d->legacy_staged = true;
        memcpy((void *)(d->raw + offset), src, bytes);
    }
    lck_mtx_unlock(&d->lock);
    return e;
}
int x9_vdec_dma_discard(struct x9_vdec_dma **slot)
{
    struct x9_vdec_dma *d;
    if (!slot || !(d = *slot) || d->magic != VDMA_MAGIC) return EINVAL;
    lck_mtx_lock(&d->lock);
    if (d->slice_count || d->client_refs || d->shared_claim ||
        (d->state != X9_MM_SMMU_UNEXPOSED && d->state != X9_MM_SMMU_RETIRED)) {
        lck_mtx_unlock(&d->lock); return EBUSY;
    }
    if (d->power_child) {
        int e = x9_mm_power_unpin(d->request.power_token, &d->power_child,
                                   X9_MM_POWER_VCP);
        if (e) { lck_mtx_unlock(&d->lock); return e; }
    }
    *slot = NULL;
    /* Kernel owner must hold its own pointer/lifetime serialization too;
     * no waiter may retain a discarded opaque pointer. */
    lck_mtx_unlock(&d->lock);
    vdma_free(d);
    return 0;
}

/* Slice bookkeeping is under d->lock. Addresses remain private until PoC
 * handoff; no new STE/CD is constructed for an allocation inside the arena. */
static bool vdma_page_busy(struct x9_vdec_dma *d, uint32_t page)
{ return (d->occupied[page / 64] & (UINT64_C(1) << (page % 64))) != 0; }
static void vdma_mark_pages(struct x9_vdec_dma *d, uint32_t offset,
    uint32_t bytes, bool busy)
{
    uint32_t start = offset / VDMA_CPU_BYTES, count = bytes / VDMA_CPU_BYTES;
    for (uint32_t i = start; i < start + count; i++) {
        uint64_t bit = UINT64_C(1) << (i % 64);
        if (busy) d->occupied[i / 64] |= bit;
        else d->occupied[i / 64] &= ~bit;
    }
}
static int vdma_slice_find(struct x9_vdec_dma *d, const struct x9_vdec_dma_slice *h,
    struct vdma_slice_record **out)
{
    if (!h->token || h->token == UINT64_MAX || h->epoch != d->request.epoch)
        return ESTALE;
    for (unsigned i = 0; i < X9_VDEC_DMA_MAX_SLICES; i++) {
        struct vdma_slice_record *r = &d->slices[i];
        if (r->handle.token != h->token) continue;
        if (r->handle.epoch != h->epoch || r->handle.bytes != h->bytes ||
            r->handle.capacity != h->capacity) return ESTALE;
        if (!r->handle.bytes || r->handle.bytes > r->handle.capacity ||
            !r->handle.capacity || (r->handle.capacity & (VDMA_CPU_BYTES - 1)) ||
            (r->offset & (VDMA_CPU_BYTES - 1)) || r->offset >= d->raw_bytes ||
            r->handle.capacity > d->raw_bytes - r->offset ||
            r->state < X9_VDEC_DMA_CPU_PRIVATE || r->state > X9_VDEC_DMA_CPU_RETIRED)
            return EFAULT;
        for (uint32_t p = r->offset / VDMA_CPU_BYTES;
             p < (r->offset + r->handle.capacity) / VDMA_CPU_BYTES; p++)
            if (!vdma_page_busy(d,p)) return EFAULT;
        *out = r;
        return 0;
    }
    return ENOENT;
}
static int vdma_slice_backing(struct x9_vdec_dma *d, uint32_t offset, uint32_t bytes)
{
    uint64_t pa = 0;
    unsigned first = offset / VDMA_CPU_BYTES, count = bytes / VDMA_CPU_BYTES;
    int e;
    if (!bytes || (offset & (VDMA_CPU_BYTES - 1)) ||
        (bytes & (VDMA_CPU_BYTES - 1)) || offset >= d->raw_bytes ||
        bytes > d->raw_bytes - offset) return EINVAL;
    for (unsigned p = first; p < first + count; p++) {
        e = vdma_phys(d->raw + (vm_offset_t)p * VDMA_CPU_BYTES,
            d->map.context.table.output_bits,&pa);
        if (e) return e;
        if (pa != d->raw_pa[p]) return ESTALE;
    }
    return 0;
}
static int vdma_slice_cpu_domain(struct x9_vdec_dma *d)
{
    if (d->state == X9_MM_SMMU_UNEXPOSED || d->state == X9_MM_SMMU_RETIRED) return 0;
    if (d->state != X9_MM_SMMU_INSTALLED) return EBUSY;
    return x9_mm_power_validate_pin(d->request.power_token,d->power_child,X9_MM_POWER_VCP);
}
static int vdma_nc_mair(void)
{
    uint64_t mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    /* XNU start.s installs 0x44 in this slot on every CPU. Check the live
     * local encoding too; VM_WIMG_WCOMB must not silently mean Device/WB. */
    return ((mair >> MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITECOMB)) & 0xff) ==
        MAIR_WRITECOMB ? 0 : ENOTSUP;
}
static int vdma_slice_nc_check(struct x9_vdec_dma *d, struct vdma_slice_record *r)
{
    int e = r->uncached ? vdma_nc_mair() : ENOTSUP;
    unsigned first = r->offset / VDMA_CPU_BYTES;
    unsigned count = r->handle.capacity / VDMA_CPU_BYTES;
    for (unsigned p = first; p < first + count && !e; p++)
        if ((pmap_cache_attributes((ppnum_t)atop_64(d->raw_pa[p])) & VM_WIMG_MASK) !=
            VM_WIMG_WCOMB) e = ESTALE;
    return e;
}
/* Internal only: the one global SHM claim owns all these complete 16K pages.
 * vcp_helper's normal allocation uses mtk_mm-uncached; system_heap_do_vmap
 * uses pgprot_writecombine for it. AP/FW fields share lines INSIDE a VSI.
 * Page isolation alone therefore does not permit WB live stores. Use XNU's
 * managed-page cache API: it updates the physical aperture and tracked PV
 * mappings and performs its TLB/cache synchronization. No second alias,
 * direct PTE edit, remap of live memory or device-pointer cast is created.
 * Caller holds d->lock; conversion happens before any address publication. */
static int vdma_slice_make_nc(struct x9_vdec_dma *d, struct vdma_slice_record *r)
{
    unsigned first = r->offset / VDMA_CPU_BYTES;
    unsigned count = r->handle.capacity / VDMA_CPU_BYTES;
    int e = vdma_nc_mair();
    if (!e && (d->state != X9_MM_SMMU_UNEXPOSED || !d->shared_claim ||
        r->state != X9_VDEC_DMA_CPU_PRIVATE || r->uncached)) e = EBUSY;
    if (!e) e = vdma_slice_backing(d,r->offset,r->handle.capacity);
    /* kmem_alloc supplies fresh default managed pages. Do not commandeer a
     * page with a preexisting nondefault mode; the inverse is exactly DEFAULT. */
    for (unsigned p = first; p < first + count && !e; p++)
        if ((pmap_cache_attributes((ppnum_t)atop_64(d->raw_pa[p])) & VM_WIMG_MASK) !=
            VM_WIMG_DEFAULT) e = ESTALE;
    if (e) return e;
    r->uncached = true; /* retain the restoration obligation before mutation */
    for (unsigned p = first; p < first + count; p++) {
        flush_dcache64(d->raw_pa[p],VDMA_CPU_BYTES,TRUE);
        __asm__ volatile("dsb sy" ::: "memory");
        pmap_set_cache_attributes((ppnum_t)atop_64(d->raw_pa[p]),VM_WIMG_WCOMB);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return vdma_slice_nc_check(d,r);
}
static int vdma_slice_restore_cache(struct x9_vdec_dma *d, struct vdma_slice_record *r)
{
    unsigned first = r->offset / VDMA_CPU_BYTES;
    unsigned count = r->handle.capacity / VDMA_CPU_BYTES;
    int e;
    if (!r->uncached) return 0;
    /* Free is allowed only while private or after physical whole-domain
     * retirement. A failed conversion may leave a DEFAULT/WCOMB mixture;
     * these are the only owned modes we may undo. Foreign changes retain it. */
    e = vdma_slice_backing(d,r->offset,r->handle.capacity);
    for (unsigned p = first; p < first + count && !e; p++) {
        unsigned attr = pmap_cache_attributes((ppnum_t)atop_64(d->raw_pa[p])) & VM_WIMG_MASK;
        if (attr != VM_WIMG_DEFAULT && attr != VM_WIMG_WCOMB) e = ESTALE;
    }
    if (e) return e;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned p = first; p < first + count; p++)
        pmap_set_cache_attributes((ppnum_t)atop_64(d->raw_pa[p]),VM_WIMG_DEFAULT);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned p = first; p < first + count; p++)
        if ((pmap_cache_attributes((ppnum_t)atop_64(d->raw_pa[p])) & VM_WIMG_MASK) !=
            VM_WIMG_DEFAULT) return ESTALE;
    r->uncached = false;
    return 0;
}
int x9_vdec_dma_slice_alloc(struct x9_vdec_dma *d, uint64_t epoch,
    uint32_t bytes, uint32_t align, struct x9_vdec_dma_slice *out)
{
    struct vdma_slice_record *r = NULL;
    uint32_t capacity, needed, page, found = VDMA_RAW_PAGES;
    uint64_t token = 0;
    int e;
    if (!out) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!d || d->magic != VDMA_MAGIC || !bytes || bytes > X9_MM_SMMU_MAX_CHECK_BYTES ||
        align < 16 || align > X9_MM_SMMU_MAX_CHECK_BYTES || (align & (align - 1)))
        return EINVAL;
    if (align < VDMA_CPU_BYTES) align = VDMA_CPU_BYTES;
    capacity = (bytes + VDMA_CPU_BYTES - 1) & ~(VDMA_CPU_BYTES - 1);
    needed = capacity / VDMA_CPU_BYTES;
    lck_mtx_lock(&d->lock);
    e = epoch != d->request.epoch ? ESTALE : 0;
    if (!e && (d->legacy_staged || d->state == X9_MM_SMMU_RETIRED)) e = EBUSY;
    if (!e) e = vdma_slice_cpu_domain(d);
    if (!e && (d->slice_count == X9_VDEC_DMA_MAX_SLICES || needed > d->raw_pages)) e = ENOSPC;
    if (e) goto done;
    for (unsigned i = 0; i < X9_VDEC_DMA_MAX_SLICES; i++)
        if (!d->slices[i].handle.token) { r = &d->slices[i]; break; }
    if (!r) { e = EFAULT; goto done; }
    for (page = 0; page <= d->raw_pages - needed; page++) {
        uint32_t n;
        if ((d->request.iova + (uint64_t)page * VDMA_CPU_BYTES) & (align - 1)) continue;
        for (n = 0; n < needed; n++) if (vdma_page_busy(d,page+n)) break;
        if (n == needed) { found = page; break; }
        page += n; /* skip the busy page, not a potentially free successor */
    }
    if (found == VDMA_RAW_PAGES) { e = ENOSPC; goto done; }
    e = vdma_slice_backing(d,found * VDMA_CPU_BYTES,capacity);
    if (e) goto done;
    token = vdma_take_token();
    if (!token) { e = EOVERFLOW; goto done; }
    /* No part of this previously unadvertised interval is owned by a device.
     * Clear rounded slack too, before making the durable handle observable. */
    memset((void *)(d->raw + (vm_offset_t)found * VDMA_CPU_BYTES),0,capacity);
    r->handle = (struct x9_vdec_dma_slice){epoch,token,bytes,capacity};
    r->offset = found * VDMA_CPU_BYTES; r->state = X9_VDEC_DMA_CPU_PRIVATE;
    vdma_mark_pages(d,r->offset,capacity,true);
    d->slice_count++; d->sliced = true;
    *out = r->handle;
done:
    lck_mtx_unlock(&d->lock);
    return e;
}
static int vdma_slice_copy(struct x9_vdec_dma *d, const struct x9_vdec_dma_slice *slice,
    uint32_t offset, uint8_t *data, uint32_t bytes, bool write)
{
    struct x9_vdec_dma_slice h;
    struct vdma_slice_record *r = NULL;
    int e;
    if (!d || d->magic != VDMA_MAGIC || !slice || !data || !bytes) return EINVAL;
    h = *slice;
    lck_mtx_lock(&d->lock);
    e = vdma_slice_find(d,&h,&r);
    if (!e && (r->state == X9_VDEC_DMA_DEVICE ||
        (write && (r->state != X9_VDEC_DMA_CPU_PRIVATE || d->state == X9_MM_SMMU_RETIRED))))
        e = EBUSY;
    if (!e && (offset > h.bytes || bytes > h.bytes - offset)) e = ERANGE;
    if (!e) e = vdma_slice_cpu_domain(d);
    if (!e) e = vdma_slice_backing(d,r->offset,h.capacity);
    if (!e && r->uncached) e = vdma_slice_nc_check(d,r);
    if (!e) {
        void *address = (void *)(d->raw + r->offset + offset);
        if (write) memcpy(address,data,bytes);
        else memcpy(data,address,bytes);
    }
    lck_mtx_unlock(&d->lock);
    return e;
}
int x9_vdec_dma_slice_write(struct x9_vdec_dma *d, const struct x9_vdec_dma_slice *s,
    uint32_t offset, const uint8_t *src, uint32_t bytes)
{ return vdma_slice_copy(d,s,offset,__DECONST(uint8_t *,src),bytes,true); }
int x9_vdec_dma_slice_read(struct x9_vdec_dma *d, const struct x9_vdec_dma_slice *s,
    uint32_t offset, uint8_t *dst, uint32_t bytes)
{ return vdma_slice_copy(d,s,offset,dst,bytes,false); }
static void vdma_slice_flush(struct x9_vdec_dma *d, struct vdma_slice_record *r)
{
    unsigned first = r->offset / VDMA_CPU_BYTES, count = r->handle.capacity / VDMA_CPU_BYTES;
    for (unsigned p = first; p < first + count; p++)
        flush_dcache64(d->raw_pa[p],VDMA_CPU_BYTES,TRUE);
    __asm__ volatile("dsb sy" ::: "memory");
}
int x9_vdec_dma_slice_publish(struct x9_vdec_dma *d, const struct x9_vdec_dma_slice *slice,
    struct x9_vdec_dma_extent *out)
{
    struct x9_vdec_dma_slice h;
    struct vdma_slice_record *r = NULL;
    int e;
    if (!out) return EINVAL;
    if (!d || d->magic != VDMA_MAGIC || !slice) { memset(out,0,sizeof(*out)); return EINVAL; }
    h = *slice; memset(out,0,sizeof(*out));
    lck_mtx_lock(&d->lock);
    e = vdma_slice_find(d,&h,&r);
    if (!e && (d->state != X9_MM_SMMU_INSTALLED || r->state != X9_VDEC_DMA_CPU_PRIVATE)) e = EBUSY;
    if (!e) e = vdma_slice_cpu_domain(d);
    if (!e) e = vdma_slice_backing(d,r->offset,h.capacity);
    if (!e && r->uncached) e = vdma_slice_nc_check(d,r);
    if (!e) {
        vdma_slice_flush(d,r);
        r->state = X9_VDEC_DMA_DEVICE; /* journal before any IOVA escapes */
        *out = (struct x9_vdec_dma_extent){d->request.iova+r->offset,h.bytes,h.capacity};
    }
    lck_mtx_unlock(&d->lock);
    return e;
}
static void vdma_reclaim_retired_slices(struct x9_vdec_dma *d)
{
    /* Called ONLY after the existing whole-domain retire has proved endpoint
     * drain, removed the exact STE and consumed TLBI/SYNC. API exclusion kept
     * CPU dirty lines out throughout DEVICE ownership. A bare b001/IRQ/c006
     * or failure to send is never sufficient to reach this path. */
    for (unsigned i = 0; i < X9_VDEC_DMA_MAX_SLICES; i++) {
        struct vdma_slice_record *r = &d->slices[i];
        if (r->handle.token && r->state == X9_VDEC_DMA_DEVICE) {
            vdma_slice_flush(d,r);
            r->state = X9_VDEC_DMA_CPU_RETIRED;
        }
    }
}
int x9_vdec_dma_slice_state(struct x9_vdec_dma *d, const struct x9_vdec_dma_slice *slice,
    uint32_t *state)
{
    struct x9_vdec_dma_slice h;
    struct vdma_slice_record *r = NULL;
    int e;
    if (!state) return EINVAL;
    if (!d || d->magic != VDMA_MAGIC || !slice) { *state = 0; return EINVAL; }
    h = *slice; *state = 0;
    lck_mtx_lock(&d->lock);
    e = vdma_slice_find(d,&h,&r);
    if (!e) e = vdma_slice_cpu_domain(d);
    if (!e) *state = r->state;
    lck_mtx_unlock(&d->lock);
    return e;
}
int x9_vdec_dma_slice_free(struct x9_vdec_dma *d, struct x9_vdec_dma_slice *slice)
{
    struct vdma_slice_record *r = NULL;
    int e;
    if (!d || d->magic != VDMA_MAGIC || !slice) return EINVAL;
    lck_mtx_lock(&d->lock);
    e = vdma_slice_find(d,slice,&r);
    if (!e && r->state == X9_VDEC_DMA_DEVICE) e = EBUSY;
    if (!e) e = vdma_slice_cpu_domain(d);
    if (!e) e = vdma_slice_restore_cache(d,r);
    if (!e) {
        vdma_mark_pages(d,r->offset,r->handle.capacity,false);
        memset(r,0,sizeof(*r)); d->slice_count--;
        memset(slice,0,sizeof(*slice));
    }
    lck_mtx_unlock(&d->lock);
    return e;
}
