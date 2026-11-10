/* Normal live SID80/SSID0 attach: preserve installed table, queue and every
 * unrelated policy. OPPO arm-smmu-v3.c arm_smmu_write_entry/STE writer and
 * targeted CFGI/CD/TLBI commands; no SMMUv2/4K/QCOM or HYP debug authority.
 * All calls hold camera_smmu_lock and an independently acquired host lease.
 */
#define CAM_SHARED_ST_ADDR 0x000fffffffffffc0ull /* STRTAB bits51..6 */
#define CAM_SHARED_Q_ADDR  0x000fffffffffffe0ull /* queue bits51..5 */
#define CAM_SHARED_L1_SPAN 31ull
enum { CAM_SHARED_CAPTURED=1, CAM_SHARED_INSTALLED, CAM_SHARED_RETIRED };
static const unsigned cam_shared_reg32[]={0,4,0x14,0x20,0x24,0x28,0x2c,
    0x44,0x50,0x54,0x60,0x64,0x88,0x100a8,0x100ac};
static const unsigned cam_shared_reg64[]={0x80,0x90,0xa0};
static struct {
    struct x9_camera_smmu *owner;
    struct x9_camera_smmu_host host;
    uint32_t regs32[sizeof(cam_shared_reg32)/sizeof(cam_shared_reg32[0])];
    uint64_t regs64[sizeof(cam_shared_reg64)/sizeof(cam_shared_reg64[0])];
    uint64_t prior[CAM_SMMU_STES][CAM_SMMU_STE_WORDS],expected[CAM_SMMU_STE_WORDS];
    uint64_t l1_prior[X9_CAM_SMMU_HOST_L1_MAX];
    vm_offset_t ste,cmd,l1;
    uint32_t l1_entries;
    uint32_t shift,prod,cons,wait_cons,observed_prod,observed_cons;
    uint32_t batch_start,batch_count,phase,issued,samples,operation;
    struct cam_smmu_cmd batch[3];
    uint64_t deadline;
    bool captured,pending;
    int error;
} cam_shared;
static int cam_shared_fault(int e)
{
    if(!cam_shared.error)cam_shared.error=e;
    return cam_shared.error;
}
static bool cam_shared_private_overlap(struct x9_camera_smmu *,uint64_t,uint64_t);
/* Provider-owned resident CPU mappings only. Every 16K page is rechecked;
 * avoid aliases into private RAW/CD/PTE/queue/STE backing. Never map PA here. */
static int cam_shared_region(struct x9_camera_smmu *m,
    const struct x9_camera_smmu_host_region *r,uint32_t limit)
{
    if(!r->va || !r->pa || r->reserved || (r->va&(CAM_S1_PAGE-1)) ||
       (r->pa&(CAM_S1_PAGE-1)) || !r->bytes || r->bytes>limit ||
       (r->bytes&(CAM_S1_PAGE-1)) || r->bytes>UINTPTR_MAX-r->va ||
       r->pa>=(1ull<<m->map->oas) || r->bytes>(1ull<<m->map->oas)-r->pa)return EINVAL;
    /* Range-check private backing once, rather than rewalking every RAW
     * page for each page of a large inherited CMDQ. The VA->PA check below
     * still independently covers EVERY resident page of the whole region. */
    if(cam_shared_private_overlap(m,r->pa,r->bytes))return ESTALE;
    for(unsigned off=0;off<r->bytes;off+=CAM_S1_PAGE) {
        uint64_t pa=0;
        int e=cam_s1_phys(r->va+off,m->map->oas,&pa);if(e)return e;
        if(pa!=r->pa+off)return ESTALE;
    }
    return 0;
}
static int cam_shared_slice(const struct x9_camera_smmu_host_region *r,
    uint64_t pa,uint32_t bytes,vm_offset_t *out)
{
    if(pa<r->pa || pa-r->pa>=r->bytes || bytes>r->bytes-(pa-r->pa))return ERANGE;
    *out=r->va+(vm_offset_t)(pa-r->pa);return 0;
}
static bool cam_shared_region_empty(const struct x9_camera_smmu_host_region *r)
{ return !r->va && !r->pa && !r->bytes && !r->reserved; }
/* Regions have already passed overflow/PA/PTE validation before this call. */
static bool cam_shared_region_alias(const struct x9_camera_smmu_host_region *a,
    const struct x9_camera_smmu_host_region *b)
{
    return (a->va<b->va+b->bytes && b->va<a->va+a->bytes) ||
        (a->pa<b->pa+b->bytes && b->pa<a->pa+a->bytes);
}
static bool cam_shared_private_overlap(struct x9_camera_smmu *m,uint64_t pa,uint64_t bytes)
{
    const struct cam_s1_page *fixed[]={&m->ste,&m->cmd,&m->evt,&m->map->root,&m->map->cd};
    for(unsigned n=0;n<sizeof(fixed)/sizeof(fixed[0]);n++)
        if(pa<fixed[n]->pa+CAM_S1_PAGE && fixed[n]->pa<pa+bytes)return true;
    for(unsigned n=0;n<m->map->leaves;n++)
        if(pa<m->map->leaf[n].pa+CAM_S1_PAGE && m->map->leaf[n].pa<pa+bytes)return true;
    for(unsigned n=0;n<m->map->pages;n++)
        if(pa<m->map->raw_pa[n]+CAM_S1_PAGE && m->map->raw_pa[n]<pa+bytes)return true;
    return false;
}
/* Normal arm-smmu-v3-common.c: SPLIT8, full L2 descriptor SPAN=9,
 * L1 index SID/256, L2 index SID%256. Resolve existing storage only.
 * Every nonzero L1 target is range/alias checked before touching STE80;
 * foreign L2s stay unmapped and are protected by the mandatory host guard.
 * No write, allocation, descriptor repair, new L1 publication or CFGI_ALL.
 */
static int cam_shared_table_capture(struct x9_camera_smmu *m,uint64_t base,
    uint32_t cfg,uint64_t evt_pa,uint64_t evt_bytes)
{
    const struct x9_camera_smmu_host_region *s=&cam_shared.host.strtab;
    const struct x9_camera_smmu_host_region *q=&cam_shared.host.cmdq;
    const struct x9_camera_smmu_host_region *l=&cam_shared.host.strtab_l2;
    uint64_t pa=base&CAM_SHARED_ST_ADDR,desc,target;
    unsigned fmt=(cfg>>16)&3u,split=(cfg>>6)&31u,bits=cfg&63u;
    uint32_t bytes,align;
    int e;
    if(!fmt) {
        if(cfg!=8 || !cam_shared_region_empty(l))return ENOTSUP;
        if(pa&(CAM_S1_PAGE-1))return ERANGE;
        return cam_shared_slice(s,pa,CAM_S1_PAGE,&cam_shared.ste);
    }
    if(fmt!=1 || ((m->idr0>>27)&3u)!=1 || split!=8 || bits<8 || bits>20 ||
       bits>(m->idr1&63u) || (cfg&~((3u<<16)|(31u<<6)|63u)))return ENOTSUP;
    e=cam_shared_region(m,l,2*CAM_S1_PAGE);if(e)return e;
    if(cam_shared_region_alias(s,l) || cam_shared_region_alias(q,l))return EINVAL;
    if(evt_pa<l->pa+l->bytes && l->pa<evt_pa+evt_bytes)return ESTALE;
    cam_shared.l1_entries=1u<<(bits-8);
    bytes=cam_shared.l1_entries*8;align=bytes<64?64:bytes;
    if(cam_shared.l1_entries>X9_CAM_SMMU_HOST_L1_MAX || (pa&(align-1)))return ERANGE;
    e=cam_shared_slice(s,pa,bytes,&cam_shared.l1);if(e)return e;
    desc=*(volatile uint64_t *)cam_shared.l1;
    /* SID80 lies in the first leaf. A zero/partial leaf cannot be installed
     * by this engine; the native table owner must establish it separately. */
    if(!desc)return ENXIO;
    if((desc&CAM_SHARED_L1_SPAN)!=9 ||
       (desc&~(CAM_SHARED_ST_ADDR|CAM_SHARED_L1_SPAN)))return ENOTSUP;
    target=desc&CAM_SHARED_ST_ADDR;
    if(!target || (target&(CAM_S1_PAGE-1)))return ERANGE;
    e=cam_shared_slice(l,target,CAM_S1_PAGE,&cam_shared.ste);if(e)return e;
    for(unsigned n=0;n<cam_shared.l1_entries;n++) {
        if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
        desc=((const volatile uint64_t *)cam_shared.l1)[n];
        cam_shared.l1_prior[n]=desc;
        if(!desc)continue; /* exact zero-invalid, not a fabricated ABORT leaf */
        if((desc&CAM_SHARED_L1_SPAN)!=9 ||
           (desc&~(CAM_SHARED_ST_ADDR|CAM_SHARED_L1_SPAN)))return ENOTSUP;
        pa=desc&CAM_SHARED_ST_ADDR;
        if(!pa || (pa&(CAM_S1_PAGE-1)) || pa>=(1ull<<m->map->oas) ||
           CAM_S1_PAGE>(1ull<<m->map->oas)-pa)return ERANGE;
        if((pa<s->pa+s->bytes && s->pa<pa+CAM_S1_PAGE) ||
           (pa<q->pa+q->bytes && q->pa<pa+CAM_S1_PAGE) ||
           (pa<evt_pa+evt_bytes && evt_pa<pa+CAM_S1_PAGE) ||
           cam_shared_private_overlap(m,pa,CAM_S1_PAGE))return ESTALE;
        /* Another SID range must not alias any part of our pinned leaf;
         * otherwise an STE80 write would also change an unrelated stream. */
        if(n && pa<l->pa+l->bytes && l->pa<pa+CAM_S1_PAGE)return ESTALE;
        if(!n && pa!=target)return ESTALE;
    }
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static int cam_shared_table_guard(void)
{
    if(cam_shared.l1_entries) {
        if(!cam_shared.l1 || cam_shared.l1_entries>X9_CAM_SMMU_HOST_L1_MAX)return ESTALE;
        for(unsigned n=0;n<cam_shared.l1_entries;n++)
            if(((const volatile uint64_t *)cam_shared.l1)[n]!=cam_shared.l1_prior[n])return ESTALE;
    } else if(cam_shared.l1 || !cam_shared_region_empty(&cam_shared.host.strtab_l2))return ESTALE;
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static int cam_shared_lease(struct x9_camera_smmu *m)
{
    if(cam_shared.error)return cam_shared.error;
    if(!cam_shared.captured || cam_shared.owner!=m || !m->shared_attempted ||
       !cam_shared.host.lease)return ESTALE;
    return cam_shared.host.guard(cam_shared.host.context,cam_shared.host.lease,
        cam_shared.operation,&m->view);
}
/* Capture each actual consumer observation before the next guard. Only our
 * serialized batch can advance the inherited queue; no CONSumption writes,
 * event/error acknowledgements or foreign producer normalization. */
static int cam_shared_guard(struct x9_camera_smmu *m)
{
    uint32_t mask=(2u<<cam_shared.shift)-1u;
    int e=cam_shared_lease(m);if(e)return e;
    e=cam_smmu_storage(m);if(e)return e;
    e=cam_shared_region(m,&cam_shared.host.strtab,2*CAM_S1_PAGE);if(e)return e;
    e=cam_shared_region(m,&cam_shared.host.cmdq,16u<<X9_CAM_SMMU_HOST_CMDQ_SHIFT);if(e)return e;
    if(cam_shared.l1_entries) {
        e=cam_shared_region(m,&cam_shared.host.strtab_l2,2*CAM_S1_PAGE);if(e)return e;
    }
    for(unsigned n=0;n<sizeof(cam_shared_reg32)/sizeof(cam_shared_reg32[0]);n++)
        if(cam_smmu_rr(m,cam_shared_reg32[n])!=cam_shared.regs32[n])return ESTALE;
    for(unsigned n=0;n<sizeof(cam_shared_reg64)/sizeof(cam_shared_reg64[0]);n++)
        if(cam_smmu_rr64(m,cam_shared_reg64[n])!=cam_shared.regs64[n])return ESTALE;
    cam_shared.observed_prod=cam_smmu_rr(m,0x98);
    cam_shared.observed_cons=cam_smmu_rr(m,0x9c);
    __asm__ volatile("dsb sy":::"memory");
    if(cam_shared.observed_prod!=cam_shared.prod || (cam_shared.observed_cons&~mask))return ESTALE;
    if(cam_shared.pending) {
        uint32_t left=(cam_shared.prod-cam_shared.wait_cons)&mask;
        uint32_t moved=(cam_shared.observed_cons-cam_shared.wait_cons)&mask;
        if(moved>left)return ESTALE;
        cam_shared.wait_cons=cam_shared.observed_cons;
    } else if(cam_shared.observed_cons!=cam_shared.cons)return ESTALE;
    e=cam_shared_table_guard();if(e)return e;
    for(unsigned sid=0;sid<CAM_SMMU_STES;sid++) {
        const volatile uint64_t *p=(const volatile uint64_t *)cam_shared.ste+sid*8;
        const uint64_t *expected=sid==CAM_SMMU_SID?cam_shared.expected:cam_shared.prior[sid];
        for(unsigned w=0;w<8;w++)if(p[w]!=expected[w])return ESTALE;
    }
    return cam_shared_lease(m);
}
static int cam_shared_batch_check(void)
{
    uint32_t ring=1u<<cam_shared.shift,mask=2*ring-1;
    if(!cam_shared.batch_count || cam_shared.batch_count>3 ||
       (cam_shared.batch_start&~mask) ||
       cam_shared.batch[cam_shared.batch_count-1].word[0]!=CAM_SMMU_CMD_SYNC ||
       cam_shared.batch[cam_shared.batch_count-1].word[1])return ESTALE;
    for(unsigned n=0;n<cam_shared.batch_count;n++) {
        const volatile uint64_t *p=(const volatile uint64_t *)cam_shared.cmd+
            ((cam_shared.batch_start+n)&(ring-1))*2;
        if(p[0]!=cam_shared.batch[n].word[0] || p[1]!=cam_shared.batch[n].word[1])return ESTALE;
    }
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static int cam_shared_submit(struct x9_camera_smmu *m,unsigned op)
{
    struct cam_smmu_cmd cmd[2]={0};
    uint32_t ring=1u<<cam_shared.shift,mask=2*ring-1,count;
    int e=cam_shared_guard(m);if(e)return e;
    if(cam_shared.pending || cam_shared.prod!=cam_shared.cons)return EBUSY;
    if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
    cmd[0]=(struct cam_smmu_cmd){{CAM_SMMU_CFGI_STE|((uint64_t)CAM_SMMU_SID<<32),1}};
    count=1;
    if(op==1) {
        cmd[1]=(struct cam_smmu_cmd){{CAM_SMMU_CFGI_CD|((uint64_t)CAM_SMMU_SID<<32),1}};
        count=2;
    } else if(op==2)cmd[0]=(struct cam_smmu_cmd){{CAM_SMMU_TLBI_NH_ASID|
        ((uint64_t)CAM_SMMU_ASID<<48),0}};
    cam_shared.batch_start=cam_shared.prod;cam_shared.batch_count=count+1;
    for(unsigned n=0;n<=count;n++) {
        vm_offset_t va=cam_shared.cmd+((cam_shared.prod+n)&(ring-1))*16;
        e=cam_shared_guard(m);if(e)return e;
        if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
        cam_shared.batch[n]=n<count?cmd[n]:(struct cam_smmu_cmd){{CAM_SMMU_CMD_SYNC,0}};
        *(volatile uint64_t *)va=cam_shared.batch[n].word[0];
        *(volatile uint64_t *)(va+8)=cam_shared.batch[n].word[1];
        e=cam_cache_range(va,16,CAM_CACHE_CLEAN);if(e)return e;
    }
    e=cam_shared_batch_check();if(!e)e=cam_shared_guard(m);if(e)return e;
    if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
    cam_shared.wait_cons=cam_shared.cons;
    cam_shared.prod=(cam_shared.prod+count+1)&mask;cam_shared.pending=true;
    /* issued/pending BEFORE the only actual CMDQ_PROD publication. */
    cam_shared.issued++;
    cam_smmu_rw(m,0x98,cam_shared.prod);
    cam_shared.samples=0;
    for(unsigned poll=0;poll<100000;poll++) {
        if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
        e=cam_shared_batch_check();if(!e)e=cam_shared_guard(m);if(e)return e;
        cam_shared.samples=cam_shared.observed_cons==cam_shared.prod?cam_shared.samples+1:0;
        if(cam_shared.samples==2) {
            e=cam_shared_lease(m);if(e)return e;
            if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
            e=cam_shared_batch_check();if(!e)e=cam_shared_guard(m);if(e)return e;
            if(cam_shared.observed_cons!=cam_shared.prod)return ESTALE;
            cam_shared.cons=cam_shared.prod;cam_shared.pending=false;return 0;
        }
        delay(1);
    }
    return ETIMEDOUT;
}
static int cam_shared_store(struct x9_camera_smmu *m,unsigned word,uint64_t value)
{
    vm_offset_t va=cam_shared.ste+CAM_SMMU_SID*64+word*8;
    int e=cam_shared_guard(m);if(e)return e;
    if(word>=8 || cam_shared.pending)return ESTALE;
    if(mach_absolute_time()>=cam_shared.deadline)return ETIMEDOUT;
    /* Immutable expected journal records possible exposure before atomic64.
     * No uncertain store/cache failure is blindly rolled back or retried. */
    cam_shared.expected[word]=value;
    *(volatile uint64_t *)va=value;
    e=cam_cache_range(va,8,CAM_CACHE_CLEAN);if(e)return e;
    return cam_shared_guard(m);
}
static int cam_shared_capture(struct x9_camera_smmu *m)
{
    struct x9_camera_smmu_host_region *s=&cam_shared.host.strtab,*q=&cam_shared.host.cmdq;
    uint64_t strtab,cmd,evt,evt_pa,evt_bytes;
    uint32_t cr0,cr1,cr2,gbpa,ge,gn,evt_prod,evt_cons,shift,evt_shift,mask;
    int e=cam_shared_region(m,s,2*CAM_S1_PAGE);
    if(!e)e=cam_shared_region(m,q,16u<<X9_CAM_SMMU_HOST_CMDQ_SHIFT);if(e)return e;
    if(cam_shared_region_alias(s,q))return EINVAL;
    for(unsigned n=0;n<sizeof(cam_shared_reg32)/sizeof(cam_shared_reg32[0]);n++)
        cam_shared.regs32[n]=cam_smmu_rr(m,cam_shared_reg32[n]);
    for(unsigned n=0;n<sizeof(cam_shared_reg64)/sizeof(cam_shared_reg64[0]);n++)
        cam_shared.regs64[n]=cam_smmu_rr64(m,cam_shared_reg64[n]);
    cr0=cam_smmu_rr(m,0x20);cr1=cam_smmu_rr(m,0x28);cr2=cam_smmu_rr(m,0x2c);
    gbpa=cam_smmu_rr(m,0x44);ge=cam_smmu_rr(m,0x60);gn=cam_smmu_rr(m,0x64);
    evt_prod=cam_smmu_rr(m,0x100a8);evt_cons=cam_smmu_rr(m,0x100ac);
    strtab=cam_shared.regs64[0];cmd=cam_shared.regs64[1];evt=cam_shared.regs64[2];
    shift=cmd&31;evt_shift=evt&31;
    if(cam_smmu_rr(m,0)!=m->idr0 || cam_smmu_rr(m,4)!=m->idr1 ||
       cam_smmu_rr(m,0x14)!=m->idr5 ||
       cr0!=CAM_SMMU_CR0 || cam_smmu_rr(m,0x24)!=cr0 || (cr1&~0xfffu) ||
       (cr2&~7u) || (gbpa&(1u<<31)) || cam_smmu_rr(m,0x50) || cam_smmu_rr(m,0x54) ||
       (ge^gn) || evt_prod!=evt_cons || (evt_prod&(1u<<31)) ||
       shift<2 || shift>((m->idr1>>21)&31u) || shift>X9_CAM_SMMU_HOST_CMDQ_SHIFT || evt_shift<2 ||
       evt_shift>((m->idr1>>16)&31u) || evt_shift>19 ||
       (evt_prod&~((2u<<evt_shift)-1u)) ||
       (strtab&~(CAM_SHARED_ST_ADDR|(1ull<<62))) ||
       (cmd&~(CAM_SHARED_Q_ADDR|31ull|(1ull<<62))) ||
       (evt&~(CAM_SHARED_Q_ADDR|31ull|(1ull<<62))))return ENOTSUP;
    evt_pa=evt&CAM_SHARED_Q_ADDR;evt_bytes=32ull<<evt_shift;
    if(!evt_pa || (evt_pa&(evt_bytes-1)) || evt_pa>=(1ull<<m->map->oas) ||
       evt_bytes>(1ull<<m->map->oas)-evt_pa ||
       (evt_pa<s->pa+s->bytes && s->pa<evt_pa+evt_bytes) ||
       (evt_pa<q->pa+q->bytes && q->pa<evt_pa+evt_bytes) ||
       cam_shared_private_overlap(m,evt_pa,evt_bytes))return ESTALE;
    e=cam_shared_table_capture(m,strtab,cam_smmu_rr(m,0x88),evt_pa,evt_bytes);
    if(!e)e=cam_shared_slice(q,cmd&CAM_SHARED_Q_ADDR,16u<<shift,&cam_shared.cmd);
    if(e)return e;
    /* Architected bases aligned to their complete structures; no queue size
     * reduction, inherited producer reset or short-region prefix accepted. */
    if((cmd&CAM_SHARED_Q_ADDR)&((16u<<shift)-1))return ERANGE;
    cam_shared.shift=shift;mask=(2u<<shift)-1;
    cam_shared.prod=cam_smmu_rr(m,0x98);cam_shared.cons=cam_smmu_rr(m,0x9c);
    if(cam_shared.prod!=cam_shared.cons || (cam_shared.prod&~mask))return EBUSY;
    for(unsigned sid=0;sid<CAM_SMMU_STES;sid++) {
        const volatile uint64_t *p=(const volatile uint64_t *)cam_shared.ste+sid*8;
        for(unsigned w=0;w<8;w++)cam_shared.prior[sid][w]=p[w];
    }
    /* Existing live/bypass/translated STE80 cannot be taken over. All other
     * contexts remain exact raw prior, not converted to a private template. */
    if(cam_shared.prior[CAM_SMMU_SID][0]>1)return EBUSY;
    for(unsigned w=1;w<8;w++)if(cam_shared.prior[CAM_SMMU_SID][w])return EBUSY;
    memcpy(cam_shared.expected,cam_shared.prior[CAM_SMMU_SID],sizeof(cam_shared.expected));
    cam_shared.captured=true;cam_shared.phase=CAM_SHARED_CAPTURED;
    return cam_shared_guard(m);
}
static void cam_shared_log(int e)
{
    printf("X9 CAMERA SID80: operation=%u phase=%u captured=%u l1_entries=%u batches=%u pending=%u "
        "prod=%x cons=%x actual=%x/%x samples=%u errno=%d "
        "(prior table/queue retained; no global reset or READY grant)\n",
        cam_shared.operation,cam_shared.phase,cam_shared.captured,cam_shared.l1_entries,cam_shared.issued,
        cam_shared.pending,cam_shared.prod,cam_shared.cons,cam_shared.observed_prod,
        cam_shared.observed_cons,cam_shared.samples,e);
}
int x9_camera_smmu_attach(struct x9_camera_smmu *m,const struct x9_camera_smmu_host *host)
{
    uint64_t span;
    bool entered=false;
    int e;
    if(!host || !host->context || !host->lease || !host->enter || !host->guard || !host->leave)return EINVAL;
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e && (!m->exported || m->shared_attempted || cam_shared.owner || m->commands ||
       m->pending || m->close_attempted))e=EBUSY;
    if(!e)e=cam_smmu_storage(m);
    if(!e) {
        e=host->enter(host->context,host->lease,X9_CAM_SMMU_INSTALL,&m->view);
        entered=!e;
    }
    if(!e) {
        m->shared_attempted=true;cam_shared.owner=m;cam_shared.host=*host;
        cam_shared.operation=X9_CAM_SMMU_INSTALL;
        nanoseconds_to_absolutetime(100000000,&span);cam_shared.deadline=mach_absolute_time()+span;
        e=host->guard(host->context,host->lease,X9_CAM_SMMU_INSTALL,&m->view);
        if(!e)e=cam_shared_capture(m);
        /* Break-before-make. Clear V, synchronize the invalid STE before
         * touching remaining words. Even zero-invalid gets targeted CFGI. */
        if(!e)e=cam_shared_store(m,0,0);
        if(!e)e=cam_shared_submit(m,0);
        for(unsigned w=1;w<8 && !e;w++)
            e=cam_shared_store(m,w,((const uint64_t *)m->ste.va)[CAM_SMMU_SID*8+w]);
        if(!e)e=cam_shared_store(m,0,((const uint64_t *)m->ste.va)[CAM_SMMU_SID*8]);
        if(!e)e=cam_shared_submit(m,1);
        if(!e)e=cam_shared_submit(m,2);
        if(!e)e=cam_shared_guard(m);
        if(!e && mach_absolute_time()>=cam_shared.deadline)e=ETIMEDOUT;
        if(!e)cam_shared.phase=CAM_SHARED_INSTALLED;
        else {cam_shared_fault(e);cam_smmu_latch(m,e);}
        cam_shared_log(e);
    }
    if(entered)host->leave(host->context,host->lease,X9_CAM_SMMU_INSTALL);
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
static int cam_smmu_shared_check_locked(struct x9_camera_smmu *m)
{
    bool entered=false;
    int e;
    if(cam_shared.owner!=m || cam_shared.phase!=CAM_SHARED_INSTALLED ||
       cam_shared.error || cam_shared.pending)return cam_shared.error?cam_shared.error:ESTALE;
    cam_shared.operation=X9_CAM_SMMU_CHECK;
    e=cam_shared.host.enter(cam_shared.host.context,cam_shared.host.lease,
        X9_CAM_SMMU_CHECK,&m->view);entered=!e;
    if(!e)e=cam_shared_guard(m);
    if(e)cam_shared_fault(e);
    if(entered)cam_shared.host.leave(cam_shared.host.context,cam_shared.host.lease,
        X9_CAM_SMMU_CHECK);
    return e;
}
int x9_camera_smmu_attached_check(struct x9_camera_smmu *m)
{
    int e;
    lck_mtx_lock(&camera_smmu_lock);e=cam_smmu_identity(m);
    if(!e)e=cam_smmu_shared_check_locked(m);
    if(e && m && m==cam_smmu_namespace && m->shared_attempted)cam_smmu_latch(m,e);
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
int x9_camera_smmu_detach(struct x9_camera_smmu *m)
{
    uint64_t span;
    bool entered=false;
    int e;
    lck_mtx_lock(&camera_smmu_lock);e=cam_smmu_identity(m);
    if(!e && (cam_shared.owner!=m || cam_shared.phase!=CAM_SHARED_INSTALLED ||
       cam_shared.pending || cam_shared.error))e=ESTALE;
    if(!e) {
        cam_shared.operation=X9_CAM_SMMU_RETIRE;
        e=cam_shared.host.enter(cam_shared.host.context,cam_shared.host.lease,
            X9_CAM_SMMU_RETIRE,&m->view);entered=!e;
    }
    if(!e) {
        nanoseconds_to_absolutetime(100000000,&span);cam_shared.deadline=mach_absolute_time()+span;
        e=cam_shared_guard(m);
        /* CPU ownership is necessary, never a substitute for the independent
         * host RETIRE proof of full stopped RAW/AXI/IRQ/worker history. */
        lck_mtx_lock(&camera_s1_lock);
        if(!e && (m->map->raw_errno || (m->map->raw_state!=CAM_RAW_PRIVATE &&
           m->map->raw_state!=CAM_RAW_IDLE)))e=EBUSY;
        lck_mtx_unlock(&camera_s1_lock);
        if(!e)e=cam_shared_store(m,0,cam_shared.expected[0]&~1ull);
        if(!e)e=cam_shared_submit(m,1);
        if(!e)e=cam_shared_submit(m,2);
        for(unsigned w=1;w<8 && !e;w++)e=cam_shared_store(m,w,cam_shared.prior[CAM_SMMU_SID][w]);
        if(!e)e=cam_shared_store(m,0,cam_shared.prior[CAM_SMMU_SID][0]);
        if(!e)e=cam_shared_submit(m,0);
        if(!e)e=cam_shared_guard(m);
        if(!e && mach_absolute_time()>=cam_shared.deadline)e=ETIMEDOUT;
        if(!e)cam_shared.phase=CAM_SHARED_RETIRED;
    }
    if(e && m && m==cam_smmu_namespace && m->shared_attempted) {
        cam_shared_fault(e);cam_smmu_latch(m,e);
    }
    if(entered)cam_shared.host.leave(cam_shared.host.context,cam_shared.host.lease,
        X9_CAM_SMMU_RETIRE);
    if(m && m==cam_shared.owner)cam_shared_log(e);
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
