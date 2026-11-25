/* Normal MT6993 camera IAS34/16K Stage-1 backing store.
 * Source facts: include/linux/io-pgtable-arm.h and
 * drivers/iommu/io-pgtable-arm-common.c::{arm_lpae_prot_to_pte,
 * arm_lpae_init_pgtable_s1}; arm-smmu-v3.c::arm_smmu_make_s1_cd.
 * Independently implemented bounded builder, not Linux allocator code.
 * This is NOT the native controller attach path. No inherited table walks,
 * MMIO, queue commands, SMC/HVC, route reservation or READY publication.
 */
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include "x9_camera_s1.h"
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
LCK_MTX_DECLARE(camera_s1_lock, &camera_group);

enum { CAM_S1_SHIFT=14, CAM_S1_PAGE=1u<<CAM_S1_SHIFT,
    CAM_S1_ENTRIES=2048, CAM_S1_ROOT_ENTRIES=512,
    CAM_S1_L2_SHIFT=25, CAM_S1_MAX_LEAVES=2,
    CAM_S1_MAX_PAGES=X9_CAMERA_MAX_BYTES/CAM_S1_PAGE };
#define CAM_S1_IOVA_LIMIT (1ull<<34)
#define CAM_S1_PA_MASK 0x0000ffffffffc000ull
/* RAW uses MAIR[0] Normal NC, outer-shareable, AP unprivileged RW,
 * nG/AF/PXN/UXN. CPU mapping stays normal WB and needs explicit PoC sync.
 * Stage-1 cannot express write-only: AP RW necessarily permits reads.
 * No NS quirk: the nonsecure controller context supplies security state.
 */
#define CAM_S1_PAGE_ATTR (3ull|(1ull<<6)|(2ull<<8)|(1ull<<10)| \
    (1ull<<11)|(3ull<<53))
#define CAM_S1_MAIR 0x00000000f404ff44ull
#define CAM_S1_MAGIC 0x583943414d533131ull
static const uint32_t cam_s1_oas[]={32,36,40,42,44,48};
#include "x9_camera_cache.inc"

enum { CAM_RAW_PRIVATE, CAM_RAW_DEVICE, CAM_RAW_CPU, CAM_RAW_IDLE,
    CAM_RAW_RETAINED, CAM_RAW_CANCELLED };

struct cam_s1_page { vm_offset_t va; uint64_t pa; };
struct x9_camera_s1 {
    uint64_t magic, iova;
    vm_offset_t raw;
    uint32_t bytes, mapped_bytes, pages, leaves, first_root, oas, ips, asid;
    struct cam_s1_page root, leaf[CAM_S1_MAX_LEAVES], cd;
    uint64_t raw_pa[CAM_S1_MAX_PAGES];
    int last_errno;
    bool published; /* may be visible to HW, NOT attached/ready */
    bool verified;  /* diag44: full check passed after publish; tables/pages immutable since */
    /* CPU pin is independent of userspace staging frame_readers. Only the
     * CAMSV qualified-completion path changes this cache ownership record.
     * One immutable slot/context per backing; no cross-slot alias/rebind. */
    unsigned raw_state;
    int raw_errno; /* RAW uncertainty must not masquerade as broken PTEs */
    const struct x9_camera_camsv_slot *raw_slot;
    void *raw_context;
    int (*raw_check)(void *,uint64_t,const struct x9_camera_camsv_request *);
    uint64_t raw_lease;
    struct x9_camera_camsv_request raw_request;
};
_Static_assert(CAM_S1_ENTRIES*sizeof(uint64_t)==CAM_S1_PAGE,"16K table");
_Static_assert(CAM_S1_MAX_PAGES==1024,"bounded RAW allocation");

static int cam_s1_phys(vm_offset_t va, uint32_t oas, uint64_t *out)
{
    ppnum_t first,last;
    uint64_t pa,limit;
    if(!va || (va&(CAM_S1_PAGE-1)) || oas<32 || oas>48)return CAM_FAIL(EINVAL);
    first=pmap_find_phys(kernel_pmap,(addr64_t)va);
    last=pmap_find_phys(kernel_pmap,(addr64_t)(va+CAM_S1_PAGE-1));
    if(!first || first!=last)return CAM_FAIL(EFAULT);
    pa=ptoa_64(first);limit=1ull<<oas;
    if((pa&(CAM_S1_PAGE-1)) || pa>=limit || CAM_S1_PAGE>limit-pa ||
        (pa&~CAM_S1_PA_MASK))return CAM_FAIL(ERANGE);
    *out=pa;return 0;
}

static int cam_s1_alloc_page(struct cam_s1_page *p, uint32_t oas)
{
    if(kmem_alloc(kernel_map,&p->va,CAM_S1_PAGE,KMA_DATA|KMA_ZERO,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return CAM_FAIL(ENOMEM);
    return cam_s1_phys(p->va,oas,&p->pa);
}

static void cam_s1_free_page(struct cam_s1_page *p)
{
    if(p->va)kmem_free(kernel_map,p->va,CAM_S1_PAGE);
    p->va=0;p->pa=0;
}

/* All storage is private until export. Partial allocation failures can
 * unwind without hardware/TLBI. No externally supplied PA or LK carveout.
 */
static void cam_s1_free_unpublished(struct x9_camera_s1 *m)
{
    if(m->published)panic("X9 CAMERA: free of published Stage-1 map");
    cam_s1_free_page(&m->cd);
    for(unsigned i=0;i<CAM_S1_MAX_LEAVES;i++)cam_s1_free_page(&m->leaf[i]);
    cam_s1_free_page(&m->root);
    if(m->raw)kmem_free(kernel_map,m->raw,m->mapped_bytes);
    m->magic=0;
    kfree_type(struct x9_camera_s1,m);
}

static uint64_t cam_s1_cd_word0(const struct x9_camera_s1 *m)
{
    /* CD TCR layout is NOT CPU TCR_EL1. T0SZ30/TG0=2, NC walk,
     * SH0=outer, EPD1, V, IPS, AA64, R/A/ASET; stall and HA/HD=0.
     */
    return 30ull|(2ull<<6)|(2ull<<12)|(1ull<<30)|(1ull<<31)|
        ((uint64_t)m->ips<<32)|(1ull<<41)|(1ull<<45)|(1ull<<46)|
        (1ull<<47)|((uint64_t)m->asid<<48);
}

static int cam_s1_check_private(struct x9_camera_s1 *m)
{
    const struct cam_s1_page *table[4];
    const volatile uint64_t *root,*cd;
    unsigned count;
    if(!m || m->magic!=CAM_S1_MAGIC)return CAM_FAIL(EINVAL);
    if(m->last_errno)return m->last_errno;
    /* Validate full address/size geometry before indexing any PTE. */
    if(PAGE_SHIFT!=CAM_S1_SHIFT || m->ips>=6 || m->oas!=cam_s1_oas[m->ips] ||
        m->asid>255 || (m->asid && !m->published) ||
        !m->bytes || m->bytes>X9_CAMERA_MAX_BYTES ||
        m->mapped_bytes!=((m->bytes+CAM_S1_PAGE-1)&~(CAM_S1_PAGE-1)) ||
        m->pages!=m->mapped_bytes/CAM_S1_PAGE || m->pages>CAM_S1_MAX_PAGES ||
        !m->iova || (m->iova&(CAM_S1_PAGE-1)) || m->iova>=CAM_S1_IOVA_LIMIT ||
        m->mapped_bytes>CAM_S1_IOVA_LIMIT-m->iova ||
        m->first_root!=m->iova>>CAM_S1_L2_SHIFT ||
        m->leaves!=1+((m->iova+m->mapped_bytes-1)>>CAM_S1_L2_SHIFT)-m->first_root ||
        !m->leaves || m->leaves>CAM_S1_MAX_LEAVES ||
        m->first_root+m->leaves>CAM_S1_ROOT_ENTRIES ||
        !m->raw || !m->root.va || !m->cd.va)return CAM_FAIL(EFAULT);
    table[0]=&m->root;table[1]=&m->cd;table[2]=&m->leaf[0];table[3]=&m->leaf[1];
    count=2+m->leaves;
    for(unsigned i=m->leaves;i<CAM_S1_MAX_LEAVES;i++)
        if(m->leaf[i].va || m->leaf[i].pa)return CAM_FAIL(EFAULT);
    for(unsigned i=m->pages;i<CAM_S1_MAX_PAGES;i++)if(m->raw_pa[i])return CAM_FAIL(EFAULT);
    for(unsigned i=0;i<count;i++) {
        uint64_t pa=0;
        int e=cam_s1_phys(table[i]->va,m->oas,&pa);
        if(e)return e;
        if(pa!=table[i]->pa)return CAM_FAIL(ESTALE);
        for(unsigned j=0;j<i;j++)if(pa==table[j]->pa)return CAM_FAIL(EFAULT);
    }
    for(unsigned i=0;i<m->pages;i++) {
        uint64_t pa=0;
        int e=cam_s1_phys(m->raw+(vm_offset_t)i*CAM_S1_PAGE,m->oas,&pa);
        if(e)return e;
        if(pa!=m->raw_pa[i])return CAM_FAIL(ESTALE);
        for(unsigned t=0;t<count;t++)if(pa==table[t]->pa)return CAM_FAIL(EFAULT);
        for(unsigned j=0;j<i;j++)if(pa==m->raw_pa[j])return CAM_FAIL(EFAULT);
    }
    /* Entire tables, including inactive root entries and all unmapped
     * leaf slots, must match. No aliases, block/contiguous entries or
     * translations outside the page-rounded buffer are accepted.
     */
    root=(const volatile uint64_t *)m->root.va;
    for(unsigned i=0;i<CAM_S1_ENTRIES;i++) {
        uint64_t expected=0;
        if(i>=m->first_root && i<m->first_root+m->leaves)
            expected=m->leaf[i-m->first_root].pa|3ull;
        if(root[i]!=expected)return CAM_FAIL(ESTALE);
    }
    for(unsigned t=0;t<m->leaves;t++) {
        const volatile uint64_t *leaf=(const volatile uint64_t *)m->leaf[t].va;
        uint64_t base=(uint64_t)(m->first_root+t)<<CAM_S1_L2_SHIFT;
        for(unsigned i=0;i<CAM_S1_ENTRIES;i++) {
            uint64_t va=base+((uint64_t)i<<CAM_S1_SHIFT),expected=0;
            if(va>=m->iova && va-m->iova<m->mapped_bytes)
                expected=m->raw_pa[(va-m->iova)>>CAM_S1_SHIFT]|CAM_S1_PAGE_ATTR;
            if(leaf[i]!=expected)return CAM_FAIL(ESTALE);
        }
    }
    cd=(const volatile uint64_t *)m->cd.va;
    for(unsigned i=0;i<CAM_S1_ENTRIES;i++) {
        uint64_t expected=0;
        if(m->asid) {
            if(i==0)expected=cam_s1_cd_word0(m);
            else if(i==1)expected=m->root.pa;
            else if(i==3)expected=CAM_S1_MAIR;
        }
        if(cd[i]!=expected)return CAM_FAIL(ESTALE);
    }
    __asm__ volatile("dsb sy":::"memory");
    return 0;
}

int x9_camera_s1_create(uint32_t idr5,uint64_t iova,uint32_t bytes,
    struct x9_camera_s1 **out)
{
    /* IDR5.GRAN16K bit5. 52-bit OA has different descriptor packing;
     * reject it rather than truncate its high bits into a 48-bit table.
     */
    unsigned ips=idr5&7u;
    uint32_t mapped;
    struct x9_camera_s1 *m;
    int e=0;
    if(!out || *out || !bytes || bytes>X9_CAMERA_MAX_BYTES || !iova ||
        (iova&(CAM_S1_PAGE-1)) || iova>=CAM_S1_IOVA_LIMIT)return CAM_FAIL(EINVAL);
    if(PAGE_SHIFT!=CAM_S1_SHIFT || !(idr5&(1u<<5)) || ips>=6)return CAM_FAIL(ENOTSUP);
    mapped=(bytes+CAM_S1_PAGE-1)&~(CAM_S1_PAGE-1);
    if(mapped>CAM_S1_IOVA_LIMIT-iova)return CAM_FAIL(ERANGE);
    m=kalloc_type(struct x9_camera_s1,Z_WAITOK|Z_ZERO);
    if(!m)return CAM_FAIL(ENOMEM);
    m->magic=CAM_S1_MAGIC;m->iova=iova;m->bytes=bytes;m->mapped_bytes=mapped;
    m->pages=mapped/CAM_S1_PAGE;m->oas=cam_s1_oas[ips];m->ips=ips;
    m->first_root=(uint32_t)(iova>>CAM_S1_L2_SHIFT);
    m->leaves=(uint32_t)((iova+mapped-1)>>CAM_S1_L2_SHIFT)-m->first_root+1;
    if(kmem_alloc(kernel_map,&m->raw,mapped,KMA_DATA|KMA_ZERO,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)e=CAM_FAIL(ENOMEM);
    if(!e)e=cam_s1_alloc_page(&m->root,m->oas);
    for(unsigned i=0;i<m->leaves && !e;i++)e=cam_s1_alloc_page(&m->leaf[i],m->oas);
    if(!e)e=cam_s1_alloc_page(&m->cd,m->oas);
    for(unsigned i=0;i<m->pages && !e;i++)
        e=cam_s1_phys(m->raw+(vm_offset_t)i*CAM_S1_PAGE,m->oas,&m->raw_pa[i]);
    if(!e) {
        uint64_t *root=(uint64_t *)m->root.va;
        for(unsigned i=0;i<m->pages;i++) {
            uint64_t va=iova+((uint64_t)i<<CAM_S1_SHIFT);
            unsigned t=(unsigned)(va>>CAM_S1_L2_SHIFT)-m->first_root;
            unsigned slot=(unsigned)(va>>CAM_S1_SHIFT)&(CAM_S1_ENTRIES-1);
            uint64_t *leaf=(uint64_t *)m->leaf[t].va;
            leaf[slot]=m->raw_pa[i]|CAM_S1_PAGE_ATTR;
        }
        for(unsigned i=0;i<m->leaves;i++)root[m->first_root+i]=m->leaf[i].pa|3ull;
        e=cam_s1_check_private(m);
    }
    if(e){cam_s1_free_unpublished(m);return e;}
    /* Not a camera lease, hardware state, or valid zero-filled frame. */
    *out=m;return 0;
}

int x9_camera_s1_validate(struct x9_camera_s1 *m)
{
    int e;
    lck_mtx_lock(&camera_s1_lock);
    if(cam_progress_step)cam_progress("s1:check-private");
    /* diag44: a published map is permanent and immutable (wired pages, our
     * own PTEs; free/discard panic or clear magic). The full walk is O(pages^2)
     * plus ~1200 pmap lookups and ran on every lease check (~40 per frame),
     * so after one full pass only re-read the CD words that attach it. */
    if(m && m->magic==CAM_S1_MAGIC && m->published && m->verified && !m->last_errno) {
        const volatile uint64_t *cd=(const volatile uint64_t *)m->cd.va;
        e=(cd[0]==cam_s1_cd_word0(m) && cd[1]==m->root.pa && cd[3]==CAM_S1_MAIR)?0:CAM_FAIL(ESTALE);
        if(e)m->verified=false;
    } else {
        e=cam_s1_check_private(m);
        if(!e && m->published)m->verified=true;
    }
    if(e && m && m->magic==CAM_S1_MAGIC && !m->last_errno)m->last_errno=e;
    lck_mtx_unlock(&camera_s1_lock);return e;
}

int x9_camera_s1_export(struct x9_camera_s1 *m,uint32_t asid,
    struct x9_camera_s1_export *out)
{
    int e;
    uint64_t *cd;
    if(!out || !asid || asid>255)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_check_private(m);
    if(!e && m->published)e=CAM_FAIL(EBUSY);
    if(!e) {
        /* No address escapes before the permanent retention bit is set.
         * Failed hardware attach is NOT grounds to discard these pages.
         */
        m->published=true;m->asid=asid;
        cd=(uint64_t *)m->cd.va;
        cd[1]=m->root.pa;cd[3]=CAM_S1_MAIR;
        __asm__ volatile("dsb sy":::"memory");
        cd[0]=cam_s1_cd_word0(m);
        e=cam_s1_check_private(m);
        if(!e) {
            /* Flush WB CPU RAW before any NC device writes. Every physical
             * page is distinct; never sync raw[0] PA plus a virtual length.
             */
            for(unsigned i=0;i<m->pages && !e;i++)
                e=cam_cache_range(m->raw+(vm_offset_t)i*CAM_S1_PAGE,
                    CAM_S1_PAGE,CAM_CACHE_DEVICE);
            for(unsigned i=0;i<m->leaves && !e;i++)
                e=cam_cache_range(m->leaf[i].va,CAM_S1_PAGE,CAM_CACHE_CLEAN);
            if(!e)e=cam_cache_range(m->root.va,CAM_S1_PAGE,CAM_CACHE_CLEAN);
            if(!e)e=cam_cache_range(m->cd.va,CAM_S1_PAGE,CAM_CACHE_CLEAN);
            if(!e)e=cam_s1_check_private(m);
        }
        if(!e) {
            *out=(struct x9_camera_s1_export){.backing=m,.raw=(const void *)m->raw,
                .iova=m->iova,.root_pa=m->root.pa,.cd_pa=m->cd.pa,
                .bytes=m->bytes,.mapped_bytes=m->mapped_bytes,
                .pages=m->pages,.oas=m->oas};
        }
    }
    if(e && m && m->magic==CAM_S1_MAGIC && e!=EBUSY && !m->last_errno)m->last_errno=e;
    lck_mtx_unlock(&camera_s1_lock);return e;
}

int x9_camera_s1_discard(struct x9_camera_s1 **map)
{
    struct x9_camera_s1 *m;
    int e=0;
    if(!map)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_s1_lock);
    m=*map;
    if(!m || m->magic!=CAM_S1_MAGIC)e=CAM_FAIL(EINVAL);
    else if(m->published)e=CAM_FAIL(EBUSY);
    else {
        /* Even a failed software validation cannot have exposed this
         * opaque object's addresses. Unpublished allocation can unwind.
         */
        *map=NULL;cam_s1_free_unpublished(m);
    }
    lck_mtx_unlock(&camera_s1_lock);return e;
}

/* Identity Stage-1 map for an EXTERNAL physical range (path-b VCP carve-out).
 * Unlike x9_camera_s1_create, the data pages are NOT allocated here: leaf
 * PTEs translate IOVA iova+off to a caller-supplied PA pa+off (identity when
 * pa==iova). Only the root/leaf/CD table pages are kernel-allocated. This is
 * pure table construction: no MMIO, queue, SMC/HVC, attach or READY. IDR5 is
 * a runtime capability gate (16K granule, IAS34, OAS<=48), not authority to
 * use the shared MM SMMU. The caller (future host adapter) reserves the SID
 * and installs the STE; nothing here touches the controller.
 */
#define CAM_S1_ID_MAGIC 0x583943414d534944ull
struct x9_camera_s1_id {
    uint64_t magic, iova, pa;
    uint32_t bytes, mapped_bytes, pages, leaves, first_root, oas, ips, asid;
    struct cam_s1_page root, leaf[CAM_S1_MAX_LEAVES], cd;
    bool published;
    int last_errno;
};

static uint64_t cam_s1_id_cd_word0(const struct x9_camera_s1_id *m)
{
    return 30ull|(2ull<<6)|(2ull<<12)|(1ull<<30)|(1ull<<31)|
        ((uint64_t)m->ips<<32)|(1ull<<41)|(1ull<<45)|(1ull<<46)|
        (1ull<<47)|((uint64_t)m->asid<<48);
}

static void cam_s1_id_free_unpublished(struct x9_camera_s1_id *m)
{
    if(m->published)panic("X9 CAMERA: free of published identity Stage-1 map");
    cam_s1_free_page(&m->cd);
    for(unsigned i=0;i<CAM_S1_MAX_LEAVES;i++)cam_s1_free_page(&m->leaf[i]);
    cam_s1_free_page(&m->root);
    m->magic=0;
    kfree_type(struct x9_camera_s1_id,m);
}

static int cam_s1_id_check(struct x9_camera_s1_id *m)
{
    const struct cam_s1_page *table[4];
    unsigned count;
    uint64_t pa_limit;
    if(!m || m->magic!=CAM_S1_ID_MAGIC)return CAM_FAIL(EINVAL);
    if(m->last_errno)return m->last_errno;
    if(PAGE_SHIFT!=CAM_S1_SHIFT || m->ips>=6 || m->oas!=cam_s1_oas[m->ips] ||
        m->asid>255 || (m->asid && !m->published) ||
        !m->bytes || m->bytes>X9_CAMERA_MAX_BYTES ||
        m->mapped_bytes!=((m->bytes+CAM_S1_PAGE-1)&~(CAM_S1_PAGE-1)) ||
        m->pages!=m->mapped_bytes/CAM_S1_PAGE || m->pages>CAM_S1_MAX_PAGES ||
        !m->iova || (m->iova&(CAM_S1_PAGE-1)) || m->iova>=CAM_S1_IOVA_LIMIT ||
        m->mapped_bytes>CAM_S1_IOVA_LIMIT-m->iova ||
        !m->pa || (m->pa&(CAM_S1_PAGE-1)) ||
        m->first_root!=m->iova>>CAM_S1_L2_SHIFT ||
        m->leaves!=1+((m->iova+m->mapped_bytes-1)>>CAM_S1_L2_SHIFT)-m->first_root ||
        !m->leaves || m->leaves>CAM_S1_MAX_LEAVES ||
        m->first_root+m->leaves>CAM_S1_ROOT_ENTRIES ||
        !m->root.va || !m->cd.va)return CAM_FAIL(EFAULT);
    pa_limit=1ull<<m->oas;
    if(m->pa>=pa_limit || m->mapped_bytes>pa_limit-m->pa ||
        (m->pa&~CAM_S1_PA_MASK))return CAM_FAIL(ERANGE);
    table[0]=&m->root;table[1]=&m->cd;table[2]=&m->leaf[0];table[3]=&m->leaf[1];
    count=2+m->leaves;
    for(unsigned i=m->leaves;i<CAM_S1_MAX_LEAVES;i++)
        if(m->leaf[i].va || m->leaf[i].pa)return CAM_FAIL(EFAULT);
    for(unsigned i=0;i<count;i++) {
        uint64_t pa=0;
        int e=cam_s1_phys(table[i]->va,m->oas,&pa);
        if(e)return e;
        if(pa!=table[i]->pa)return CAM_FAIL(ESTALE);
        for(unsigned j=0;j<i;j++)if(pa==table[j]->pa)return CAM_FAIL(EFAULT);
    }
    {
        const volatile uint64_t *root=(const volatile uint64_t *)m->root.va;
        for(unsigned i=0;i<CAM_S1_ENTRIES;i++) {
            uint64_t expected=0;
            if(i>=m->first_root && i<m->first_root+m->leaves)
                expected=m->leaf[i-m->first_root].pa|3ull;
            if(root[i]!=expected)return CAM_FAIL(ESTALE);
        }
    }
    for(unsigned t=0;t<m->leaves;t++) {
        const volatile uint64_t *leaf=(const volatile uint64_t *)m->leaf[t].va;
        uint64_t base=(uint64_t)(m->first_root+t)<<CAM_S1_L2_SHIFT;
        for(unsigned i=0;i<CAM_S1_ENTRIES;i++) {
            uint64_t va=base+((uint64_t)i<<CAM_S1_SHIFT),expected=0;
            if(va>=m->iova && va-m->iova<m->mapped_bytes)
                expected=(m->pa+(va-m->iova))|CAM_S1_PAGE_ATTR;
            if(leaf[i]!=expected)return CAM_FAIL(ESTALE);
        }
    }
    {
        const volatile uint64_t *cd=(const volatile uint64_t *)m->cd.va;
        for(unsigned i=0;i<CAM_S1_ENTRIES;i++) {
            uint64_t expected=0;
            if(m->asid) {
                if(i==0)expected=cam_s1_id_cd_word0(m);
                else if(i==1)expected=m->root.pa;
                else if(i==3)expected=CAM_S1_MAIR;
            }
            if(cd[i]!=expected)return CAM_FAIL(ESTALE);
        }
    }
    __asm__ volatile("dsb sy":::"memory");
    return 0;
}

int x9_camera_s1_id_create(uint32_t idr5,uint64_t iova,uint64_t pa,
    uint32_t bytes,struct x9_camera_s1_id **out)
{
    unsigned ips=idr5&7u;
    uint32_t mapped;
    struct x9_camera_s1_id *m;
    int e=0;
    if(!out || *out || !bytes || bytes>X9_CAMERA_MAX_BYTES || !iova ||
        (iova&(CAM_S1_PAGE-1)) || iova>=CAM_S1_IOVA_LIMIT ||
        !pa || (pa&(CAM_S1_PAGE-1)))return CAM_FAIL(EINVAL);
    if(PAGE_SHIFT!=CAM_S1_SHIFT || !(idr5&(1u<<5)) || ips>=6)return CAM_FAIL(ENOTSUP);
    mapped=(bytes+CAM_S1_PAGE-1)&~(CAM_S1_PAGE-1);
    if(mapped>CAM_S1_IOVA_LIMIT-iova)return CAM_FAIL(ERANGE);
    m=kalloc_type(struct x9_camera_s1_id,Z_WAITOK|Z_ZERO);
    if(!m)return CAM_FAIL(ENOMEM);
    m->magic=CAM_S1_ID_MAGIC;m->iova=iova;m->pa=pa;m->bytes=bytes;
    m->mapped_bytes=mapped;m->pages=mapped/CAM_S1_PAGE;
    m->oas=cam_s1_oas[ips];m->ips=ips;
    m->first_root=(uint32_t)(iova>>CAM_S1_L2_SHIFT);
    m->leaves=(uint32_t)((iova+mapped-1)>>CAM_S1_L2_SHIFT)-m->first_root+1;
    if(m->leaves>CAM_S1_MAX_LEAVES || m->first_root+m->leaves>CAM_S1_ROOT_ENTRIES)
        e=CAM_FAIL(ERANGE);
    if(!e)e=cam_s1_alloc_page(&m->root,m->oas);
    for(unsigned i=0;i<m->leaves && !e;i++)e=cam_s1_alloc_page(&m->leaf[i],m->oas);
    if(!e)e=cam_s1_alloc_page(&m->cd,m->oas);
    if(!e) {
        uint64_t *root=(uint64_t *)m->root.va;
        for(unsigned i=0;i<m->pages;i++) {
            uint64_t va=iova+((uint64_t)i<<CAM_S1_SHIFT);
            unsigned t=(unsigned)(va>>CAM_S1_L2_SHIFT)-m->first_root;
            unsigned slot=(unsigned)(va>>CAM_S1_SHIFT)&(CAM_S1_ENTRIES-1);
            uint64_t *leaf=(uint64_t *)m->leaf[t].va;
            leaf[slot]=(pa+((uint64_t)i<<CAM_S1_SHIFT))|CAM_S1_PAGE_ATTR;
        }
        for(unsigned i=0;i<m->leaves;i++)root[m->first_root+i]=m->leaf[i].pa|3ull;
        e=cam_s1_id_check(m);
    }
    if(e){cam_s1_id_free_unpublished(m);return e;}
    *out=m;return 0;
}

int x9_camera_s1_id_validate(struct x9_camera_s1_id *m)
{
    int e;
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_id_check(m);
    if(e && m && m->magic==CAM_S1_ID_MAGIC && !m->last_errno)m->last_errno=e;
    lck_mtx_unlock(&camera_s1_lock);return e;
}

int x9_camera_s1_id_export(struct x9_camera_s1_id *m,uint32_t asid,
    struct x9_camera_s1_export *out)
{
    int e;
    uint64_t *cd;
    if(!out || !asid || asid>255)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_id_check(m);
    if(!e && m->published)e=CAM_FAIL(EBUSY);
    if(!e) {
        m->published=true;m->asid=asid;
        cd=(uint64_t *)m->cd.va;
        cd[1]=m->root.pa;cd[3]=CAM_S1_MAIR;
        __asm__ volatile("dsb sy":::"memory");
        cd[0]=cam_s1_id_cd_word0(m);
        e=cam_s1_id_check(m);
        /* External data pages are not kernel-owned WB memory; only the
         * kernel-allocated table pages need PoC cleaning before NC walk.
         */
        for(unsigned i=0;i<m->leaves && !e;i++)
            e=cam_cache_range(m->leaf[i].va,CAM_S1_PAGE,CAM_CACHE_CLEAN);
        if(!e)e=cam_cache_range(m->root.va,CAM_S1_PAGE,CAM_CACHE_CLEAN);
        if(!e)e=cam_cache_range(m->cd.va,CAM_S1_PAGE,CAM_CACHE_CLEAN);
        if(!e)e=cam_s1_id_check(m);
        if(!e) {
            *out=(struct x9_camera_s1_export){.backing=NULL,.raw=NULL,
                .iova=m->iova,.root_pa=m->root.pa,.cd_pa=m->cd.pa,
                .bytes=m->bytes,.mapped_bytes=m->mapped_bytes,
                .pages=m->pages,.oas=m->oas};
        }
    }
    if(e && m && m->magic==CAM_S1_ID_MAGIC && e!=EBUSY && !m->last_errno)m->last_errno=e;
    lck_mtx_unlock(&camera_s1_lock);return e;
}

int x9_camera_s1_id_discard(struct x9_camera_s1_id **map)
{
    struct x9_camera_s1_id *m;
    int e=0;
    if(!map)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_s1_lock);
    m=*map;
    if(!m || m->magic!=CAM_S1_ID_MAGIC)e=CAM_FAIL(EINVAL);
    else if(m->published)e=CAM_FAIL(EBUSY);
    else { *map=NULL;cam_s1_id_free_unpublished(m); }
    lck_mtx_unlock(&camera_s1_lock);return e;
}
