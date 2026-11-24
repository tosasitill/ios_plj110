/* Native cold normal-bank host. Register order/CFG/TLB commands follow
 * OPPO arm_smmu_device_reset, but without resetting an inherited controller,
 * touching secure registers, HYP sharing, or changing other-stream policy.
 * Ordinary wrapper configuration follows mtk_smmu_hw_init/smmu_init_wpcfg
 * and this board's live DT; its priors are restored only after normal OFF.
 * XNU is the sole normal software producer in this repository: both native
 * boot and subsequent CFGI/TLBI use camera_smmu_lock and self-owned queues.
 * Live comparisons reject firmware interception; they are not a universal
 * firmware exclusivity proof. An active/inherited bank must use another host.
 */
#include "x9_camera_native_smmu.h"
enum { CAM_N_IDR0,CAM_N_IDR1,CAM_N_IDR5,CAM_N_CR0,CAM_N_ACK,CAM_N_CR1,
    CAM_N_CR2,CAM_N_GBPA,CAM_N_IRQ,CAM_N_IRQACK,CAM_N_GE,CAM_N_GEN,
    CAM_N_CFG,CAM_N_CP,CAM_N_CC,CAM_N_EP,CAM_N_EC,CAM_N_WORDS };
static const unsigned cam_native_off[CAM_N_WORDS]={0,4,0x14,0x20,0x24,0x28,
    0x2c,0x44,0x50,0x54,0x60,0x64,0x88,0x98,0x9c,0x100a8,0x100ac};
struct cam_native_snapshot {
    uint32_t word[CAM_N_WORDS];
    uint32_t wp[4];
    uint64_t base[3];
};
_Static_assert(CAM_N_WORDS==17,"native prior normal snapshot size");
static int cam_native_producer_guard_locked(uint64_t,struct x9_camera_smmu *);
static const unsigned cam_native_base_off[3]={0x80,0x90,0xa0};
/* live.dts resource1 is 309e0000, selected by arm_smmu_device_probe.
 * Do not use the fallback base+1ff000 or the secure configuration aperture.
 * The four ordinary RW words are GLB_CTL0, TCU_CTL1, TCU_CTL10, TCU_CTL0.
 * This guard does not access IRQ, monitor/reset/clear, DVM or MPAM registers. */
static const unsigned cam_native_wp_off[4]={0,0x204,0x228,0x200};
static vm_offset_t cam_native_mmio,cam_native_wp_mmio;
static void cam_native_prior(const struct x9_camera_smmu *m,struct cam_native_snapshot *s)
{
    /* memcmp also covers structure alignment padding. Always initialize it;
     * copying only word/base arrays left four stack bytes indeterminate. */
    bzero(s,sizeof(*s));
    memcpy(s->word,m->native_prior_word,sizeof(s->word));
    memcpy(s->wp,m->native_prior_wp,sizeof(s->wp));
    memcpy(s->base,m->native_prior_base,sizeof(s->base));
}
static int cam_native_wp_guard(struct x9_camera_smmu *m)
{
    struct x9_camera_smmu_policy policy;
    int e=cam_smmu_policy_read(&policy);if(e)return e;
    if(memcmp(&policy,&m->native_policy,sizeof(policy)))return CAM_FAIL(ESTALE);
    if(!m->native_wp)return CAM_FAIL(ENXIO);
    /* smmu_init_wpcfg never verifies wrapper words; do not refuse on them. */
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static void cam_native_read(struct x9_camera_smmu *m,struct cam_native_snapshot *s)
{
    bzero(s,sizeof(*s));
    cam_mmio_region=1;
    for(unsigned i=0;i<CAM_N_WORDS;i++){cam_mmio_off=cam_native_off[i];s->word[i]=cam_smmu_rr(m,cam_native_off[i]);cam_mmio_reads++;}
    cam_mmio_region=2;
    for(unsigned i=0;i<4;i++){cam_mmio_off=cam_native_wp_off[i];s->wp[i]=cam_smmu_wp_rr(m,cam_native_wp_off[i]);cam_mmio_reads++;}
    cam_mmio_region=3;
    for(unsigned i=0;i<3;i++){cam_mmio_off=cam_native_base_off[i];s->base[i]=cam_smmu_rr64(m,cam_native_base_off[i]);cam_mmio_reads++;}
    cam_mmio_region=0;
    __asm__ volatile("dsb sy":::"memory");
}
static const char *cam_native_match_step="-";
static int cam_native_match(struct x9_camera_smmu *m,const struct cam_native_snapshot *s)
{
    if(cam_smmu_power_lost)return CAM_FAIL(ENXIO);
    struct cam_native_snapshot now;
    const char *outer=cam_progress_step;
    cam_progress_step="match:mm-power-validate (mm_power_lock)";cam_progress_t0=mach_absolute_time();
    int e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);
    cam_progress_step=outer;
    if(e)return e;
    if(!m->native_wp)return CAM_FAIL(ENXIO);
    cam_native_read(m,&now);
    memcpy(now.wp,s->wp,sizeof(now.wp)); /* Android: wrapper is write-only config */
    if(!memcmp(s,&now,sizeof(now)))return 0;
    { /* 400: native retire failed ESTALE after CR0=0 with no detail. */
        static unsigned logs;
        if(logs<12) {
            logs++;
            for(unsigned i=0;i<CAM_N_WORDS;i++)if(s->word[i]!=now.word[i])
                printf("X9 CAMERA NATIVE MATCH DIFF: step=%s word=%u off=%x expected=%x actual=%x\n",
                    cam_native_match_step,i,cam_native_off[i],s->word[i],now.word[i]);
            for(unsigned i=0;i<3;i++)if(s->base[i]!=now.base[i])
                printf("X9 CAMERA NATIVE MATCH DIFF: step=%s base=%u off=%x expected=%llx actual=%llx\n",
                    cam_native_match_step,i,cam_native_base_off[i],s->base[i],now.base[i]);
        }
    }
    return CAM_FAIL(ESTALE);
}
static int cam_native_wp_write(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    unsigned field,uint32_t mask,uint32_t value)
{
    int e=cam_native_match(m,s);if(e)return e;
    uint32_t cur=cam_smmu_wp_rr(m,cam_native_wp_off[field]);
    uint32_t next=(cur&~mask)|(value&mask),got;
    s->wp[field]=cur;
    if(next==cur)return 0;
    /* Record even an uncertain store before issuing it. Partial wrapper or
     * normal startup cannot use the never-hardware-exposed retirement path. */
    m->native_wp_issued|=1u<<field;
    m->native_wp_expected[field]=s->wp[field]=next;
    cam_smmu_wp_rw(m,cam_native_wp_off[field],next);
    got=cam_smmu_wp_rr(m,cam_native_wp_off[field]);
    if(got!=next)printf("X9 CAMERA SMMU WRAPPER ADOPT: field=%u off=%x before=%x wrote=%x read=%x "
        "(smmu_write_field has no readback check)\n",field,cam_native_wp_off[field],cur,next,got);
    m->native_wp_expected[field]=s->wp[field]=got;
    return cam_native_match(m,s);
}
static int cam_native_wp_init(struct x9_camera_smmu *m,struct cam_native_snapshot *s)
{
    /* mt6993_data_mm: EXTRA_DCM, MM read-overtaking; live DT: axslc,
     * slc-gid8, tcu-qos5, stash-mode3. No CPU_PARTID disable for MM.
     * Each masked writer follows the normal Android order. The second
     * STD_AXI_MODE_DIS from axslc is already1 and needs no repeated store.
     * Secure WPCFG is a separate unsupported 0514 service; this implements
     * only the source's ordinary wrapper writes, not a firmware grant. */
    static const struct { unsigned field; uint32_t mask,value; } steps[]={
        {0,1u<<2,1u<<2},{0,1u<<11,1u<<11},
        {0,7u<<18,7u<<18},{0,1,1},
        {1,0x1f0,0xe0},{1,2,2},
        {2,0x7f,8},{3,0xf,5},{0,(1u<<10)|(1u<<13),(1u<<10)|(1u<<13)}
    };
    int e=0;
    for(unsigned i=0;i<sizeof(steps)/sizeof(steps[0]) && !e;i++)
        e=cam_native_wp_write(m,s,steps[i].field,steps[i].mask,steps[i].value);
    printf("X9 CAMERA SMMU WRAPPER: issued=%x GLB=%x TCU1=%x GID=%x QOS=%x errno=%d "
        "(Android normal config; secure policy unchanged)\n",m->native_wp_issued,
        s->wp[0],s->wp[1],s->wp[2],s->wp[3],e);
    return e;
}
/* diag32: reopen on reused queues. device_reset writes CMDQ PROD/CONS=0;
 * if the controller keeps its old (equal, empty) cursor, continue from it. */
static int cam_native_cursor_reset(struct x9_camera_smmu *m,struct cam_native_snapshot *s,unsigned field)
{
    int e=cam_native_match(m,s);if(e)return e;
    uint32_t before=s->word[field],got;
    cam_smmu_rw(m,cam_native_off[field],0);got=cam_smmu_rr(m,cam_native_off[field]);
    if(got && got==before && m->native_queues_adopted)
        printf("X9 CAMERA NATIVE QUEUE ADOPT: off=%x kept=%x (write ignored; same queue continues)\n",
            cam_native_off[field],got);
    else got=0;
    s->word[field]=got;
    return cam_native_match(m,s);
}
static int cam_native_write32(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    unsigned field,uint32_t value)
{
    int e=cam_native_match(m,s);if(e)return e;
    /* Exported backing and the actual MM token are retained before store. */
    cam_smmu_rw(m,cam_native_off[field],value);s->word[field]=value;
    return cam_native_match(m,s);
}
static int cam_native_write64(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    unsigned field,uint64_t value)
{
    int e=cam_native_match(m,s);if(e)return e;
    cam_smmu_rw64(m,cam_native_base_off[field],value);s->base[field]=value;
    return cam_native_match(m,s);
}
/* With CR0=CR0ACK=0, STRTAB/CMDQ/EVTQ bases and the CR1/CR2/STRTAB_BASE_CFG
 * reset configuration retained the values written by this host after zero-prior
 * stores. Android device_reset rewrites all of them before enabling their
 * consumers, and a disabled SMMU does not fetch any of them. Keep only an
 * unchanged, explicitly observed host value as inert state; never adopt a
 * changed register or an enabled controller. */
static uint64_t cam_native_inert_base[3];
static uint32_t cam_native_inert_config[CAM_N_WORDS];
static int cam_native_restore_base(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    unsigned field,uint64_t value)
{
    unsigned off=cam_native_base_off[field];
    uint64_t was,now;
    int e=cam_native_match(m,s);if(e)return e;
    was=s->base[field];
    cam_smmu_rw64(m,off,value);
    now=cam_smmu_rr64(m,off);
    if(now!=value) {
        cam_smmu_rw(m,off,(uint32_t)value);cam_smmu_rw(m,off+4,(uint32_t)(value>>32));
        delay(10);now=cam_smmu_rr64(m,off);
    }
    if(now!=value) {
        printf("X9 CAMERA NATIVE SMMU BASE RESTORE: field=%u off=%x prior=%llx written_by_host=%llx now=%llx cr0=%x ack=%x\n",
            field,off,value,was,now,s->word[CAM_N_CR0],s->word[CAM_N_ACK]);
        if(now!=was || s->word[CAM_N_CR0] || s->word[CAM_N_ACK])return CAM_FAIL(ESTALE);
        cam_native_inert_base[field]=now;
        printf("X9 CAMERA NATIVE SMMU BASE ADOPT: field=%u base=%llx stays with SMMUEN=0 (Android rewrites before enable)\n",field,now);
        value=now;
    }
    s->base[field]=value;
    return cam_native_match(m,s);
}
static int cam_native_restore_config(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    unsigned field,uint32_t value,uint32_t host_value)
{
    unsigned off=cam_native_off[field];
    uint32_t was=s->word[field],now;
    int e=cam_native_match(m,s);if(e)return e;
    cam_smmu_rw(m,off,value);
    now=cam_smmu_rr(m,off);
    if(now!=value) {
        printf("X9 CAMERA NATIVE SMMU CONFIG RESTORE: field=%u off=%x prior=%x written_by_host=%x now=%x cr0=%x ack=%x\n",
            field,off,value,was,now,s->word[CAM_N_CR0],s->word[CAM_N_ACK]);
        if(now!=was || now!=host_value || s->word[CAM_N_CR0] || s->word[CAM_N_ACK])
            return CAM_FAIL(ESTALE);
        cam_native_inert_config[field]=now;
        printf("X9 CAMERA NATIVE SMMU CONFIG ADOPT: field=%u value=%x stays with SMMUEN=0 (Android rewrites before enable)\n",
            field,now);
        value=now;
    }
    s->word[field]=value;
    return cam_native_match(m,s);
}
static int cam_native_enable(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    uint32_t value)
{
    int e=cam_native_match(m,s);if(e)return e;
    uint32_t before=s->word[CAM_N_ACK];
    m->native_cr0=value;cam_smmu_rw(m,0x20,value);s->word[CAM_N_CR0]=value;
    uint64_t span;nanoseconds_to_absolutetime(1000000000ull,&span); /* ARM_SMMU_POLL_TIMEOUT_US */
    uint64_t deadline=mach_absolute_time()+span;
    for(unsigned n=0;n<1000000;n++) {
        struct cam_native_snapshot now;
        e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);if(e)return e;
        cam_native_read(m,&now);
        uint32_t ack=now.word[CAM_N_ACK];
        if(ack!=before && ack!=value)return CAM_FAIL(ESTALE);
        now.word[CAM_N_ACK]=s->word[CAM_N_ACK];
        memcpy(now.wp,s->wp,sizeof(now.wp));
        if(memcmp(s,&now,sizeof(now)))return CAM_FAIL(ESTALE);
        if(ack==value){s->word[CAM_N_ACK]=value;return 0;}
        if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
        delay(1);
    }
    return CAM_FAIL(ETIMEDOUT);
}
static int cam_native_full_table(struct x9_camera_smmu *m,unsigned shift,bool bypass)
{
    vm_offset_t va=0;uint64_t pa=0;unsigned bytes=64u<<shift;
    if(shift<8 || shift>16 || m->exported)return CAM_FAIL(EINVAL);
    cam_progress(bytes!=m->ste_bytes?"ft:alloc-contig":"ft:fill");
    if(bytes!=m->ste_bytes && m->native_queues_adopted)return CAM_FAIL(EINVAL);
    if(bytes!=m->ste_bytes) {
        if(kmem_alloc_contig(kernel_map,&va,bytes,PAGE_MASK,0,0,
            KMA_DATA|KMA_ZERO,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return CAM_FAIL(ENOMEM);
        int e=cam_s1_phys(va,cam_s1_oas[m->idr5&7u],&pa);
        for(unsigned off=CAM_S1_PAGE;off<bytes && !e;off+=CAM_S1_PAGE) {
            uint64_t next=0;e=cam_s1_phys(va+off,cam_s1_oas[m->idr5&7u],&next);
            if(!e && next!=pa+off)e=CAM_FAIL(EFAULT);
        }
        if(e){kmem_free(kernel_map,va,bytes);return e;}
        kmem_free(kernel_map,m->ste.va,m->ste_bytes);m->ste.va=va;m->ste.pa=pa;
    }
    m->native=true;m->native_bypass=bypass;m->ste_shift=shift;m->ste_bytes=bytes;
    uint64_t *ste=(uint64_t *)m->ste.va;
    bzero(ste,bytes);
    for(unsigned sid=0;sid<(1u<<shift);sid++) {
        ste[sid*8]=1;
        if(bypass && sid!=CAM_SMMU_SID) {
            ste[sid*8]=1ull|(4ull<<1);
            if(m->idr1&(1u<<27))ste[sid*8+1]=1ull<<44;
        }
    }
    cam_progress("ft:storage");
    return cam_smmu_storage(m);
}
static int cam_native_smmu_start_impl(uint64_t token,uint64_t iova,uint32_t bytes,
    struct x9_camera_smmu **out,struct x9_camera_smmu_export *view,
    struct x9_camera_smmu *previous)
{
    vm_offset_t mmio;
    struct cam_native_snapshot entry,again;
    struct x9_camera_smmu_policy policy;
    struct x9_camera_smmu probe={0},*m=NULL;
    int e;
    if(!out || *out || !view || !token)return CAM_FAIL(EINVAL);
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=4)return CAM_FAIL(ENOTSUP); /* normal EL1 host, not a guessed EL2 handoff */
    /* This boot's qualified controller/master policy, BEFORE MMIO or
     * exporting any storage. No generated/live.dts fallback is a grant. */
    cam_progress("smmu:policy");
    e=cam_smmu_policy_read(&policy);if(e)return e;
    cam_progress("smmu:power-validate");
    e=x9_mm_power_validate(token,X9_MM_POWER_CAMERA);if(e)return e;
    /* Android: arm_smmu_rpm_get (SMI common / mminfra in use) BEFORE the
     * first SMMU register access of a delayed hw_init. */
    cam_progress("smmu:power-up (mminfra AO/0/1/2)");
    e=cam_smmu_power_up(token);if(e)return e;
    cam_progress("smmu:io-map");
    lck_mtx_lock(&camera_smmu_lock);
    if(!cam_native_mmio)cam_native_mmio=ml_io_map(0x30800000,0x20000);
    if(!cam_native_wp_mmio)cam_native_wp_mmio=ml_io_map(0x309e0000,0x1000);
    mmio=cam_native_mmio;
    lck_mtx_unlock(&camera_smmu_lock);
    if(!mmio || !cam_native_wp_mmio)return CAM_FAIL(ENOMEM);
    probe.mmio=mmio;probe.native_wp=cam_native_wp_mmio;probe.native_power=token;
    printf("X9 CAMERA SMMU MAP: shared=%lx wp=%lx\n",(unsigned long)mmio,(unsigned long)cam_native_wp_mmio);
    cam_progress("smmu:first-mmio-read 0x30800000/0x309e0000");
    cam_native_read(&probe,&entry);
    printf("X9 CAMERA SMMU ENTRY: idr0=%x idr1=%x idr5=%x cr0=%x ack=%x gbpa=%x cr1=%x cr2=%x cfg=%x ge=%x/%x bases=%llx/%llx/%llx\n",
        entry.word[CAM_N_IDR0],entry.word[CAM_N_IDR1],entry.word[CAM_N_IDR5],entry.word[CAM_N_CR0],
        entry.word[CAM_N_ACK],entry.word[CAM_N_GBPA],entry.word[CAM_N_CR1],entry.word[CAM_N_CR2],
        entry.word[CAM_N_CFG],entry.word[CAM_N_GE],entry.word[CAM_N_GEN],
        entry.base[0],entry.base[1],entry.base[2]);cam_native_read(&probe,&again);
    if(memcmp(&entry,&again,sizeof(entry)))return CAM_FAIL(EBUSY);
    unsigned sidbits=entry.word[CAM_N_IDR1]&63u;
    if(!cam_smmu_capable(entry.word[CAM_N_IDR0],entry.word[CAM_N_IDR1],
         entry.word[CAM_N_IDR5]) || sidbits<8 || sidbits>16)return CAM_FAIL(ENOTSUP);
    /* No normal bases/stream-table config/queue/interface are inherited. A
     * base or STRTAB config may remain only when this host observed it unchanged
     * under CR0=ACK=0 and Android's next reset rewrites it before enable. CR1/CR2
     * retain their original inherited-prior semantics. */
    for(unsigned i=0;i<3;i++)if(entry.base[i] && entry.base[i]!=cam_native_inert_base[i])
        return CAM_FAIL(EBUSY);
    if((entry.word[CAM_N_CFG] && entry.word[CAM_N_CFG]!=cam_native_inert_config[CAM_N_CFG]) ||
       entry.word[CAM_N_CR0] || entry.word[CAM_N_ACK] ||
       entry.word[CAM_N_IRQ] || entry.word[CAM_N_IRQACK] ||
       /* diag27: equal CMDQ cursors with CR0=ACK=0 are an empty, inert queue
        * (kept by our own retire); start rewrites both to 0 like device_reset. */
       entry.word[CAM_N_CP]!=entry.word[CAM_N_CC] ||
       entry.word[CAM_N_EP] || entry.word[CAM_N_EC] ||
       (entry.word[CAM_N_GBPA]&(1u<<31)))return CAM_FAIL(EBUSY);
    if(entry.word[CAM_N_GE]^entry.word[CAM_N_GEN]) {
        printf("X9 CAMERA SMMU GERROR ADOPT: cold GERROR=%x GERRORN=%x acked before reset "
            "(Android device_reset does not refuse)\n",entry.word[CAM_N_GE],entry.word[CAM_N_GEN]);
        cam_smmu_rw(&probe,0x64,entry.word[CAM_N_GE]);
        cam_native_read(&probe,&entry);
        if(entry.word[CAM_N_GE]^entry.word[CAM_N_GEN])return CAM_FAIL(EIO);
    }
    cam_progress("smmu:prepare-replacement");
    e=cam_smmu_prepare_replacement(entry.word[CAM_N_IDR0],entry.word[CAM_N_IDR1],
        entry.word[CAM_N_IDR5],iova,bytes,previous,&m);if(e)return e;
    lck_mtx_lock(&camera_smmu_lock);
    m->native_power=token;m->native_gbpa=entry.word[CAM_N_GBPA];
    m->native_wp=cam_native_wp_mmio;
    m->native_policy=policy;
    memcpy(m->native_prior_word,entry.word,sizeof(entry.word));
    memcpy(m->native_prior_wp,entry.wp,sizeof(entry.wp));
    memcpy(m->native_wp_expected,entry.wp,sizeof(entry.wp));
    memcpy(m->native_prior_base,entry.base,sizeof(entry.base));
    if(m->native_gbpa&(1u<<20))
        printf("X9 CAMERA SMMU GBPA INHERITED: gbpa=%x ABORT set; non-camera STEs still bypass "
            "(MTK arm_smmu_init_initial_stes)\n",m->native_gbpa);
    cam_progress("smmu:full-table");
    e=cam_native_full_table(m,sidbits,true);
    printf("X9 CAMERA SMMU FULL TABLE: errno=%d\n",e);
    if(!e)printf("X9 CAMERA SMMU MAP: m->mmio=%lx m->wp=%lx (prepare_replacement maps its own)\n",
        (unsigned long)m->mmio,(unsigned long)m->native_wp);
    if(!e)cam_progress("smmu:entry-match");
    if(!e)e=cam_native_match(m,&entry);
    lck_mtx_unlock(&camera_smmu_lock);
    if(e){(void)x9_camera_smmu_discard(&m);return e;}
    cam_progress("smmu:export");
    e=x9_camera_smmu_export(m,view);
    /* Addresses permanently escaped even when export's cache work fails. */
    if(m->exported)*out=m;
    if(e) {
        if(!m->exported)(void)x9_camera_smmu_discard(&m);
        return e;
    }
    lck_mtx_lock(&camera_smmu_lock);
    /* Keep the first failing step: the caller's prepare stage also covers
     * allocation and export, so a bare errno cannot identify which SMMU
     * register or invalidation failed. No extra MMIO reads on failure. */
    const char *step="entry-match";
    cam_progress("smmu:entry-match2");
    e=cam_native_match(m,&entry);
    if(!e)m->native_setup_issued=true; /* BEFORE first wrapper/normal store */
#define CAM_NATIVE_STEP(name, expr) do { if(!e) { step=(name); cam_progress("smmu:" name); e=(expr); } } while(0)
    CAM_NATIVE_STEP("wrapper",cam_native_wp_init(m,&entry));
    /* Android arm_smmu_device_reset starts by synchronizing CR0=0.
     * Admission above requires a cold, unowned bank; do not reset a live
     * controller whose other stream owners cannot be accounted for. */
    CAM_NATIVE_STEP("disable-ack",cam_native_enable(m,&entry,0));
    CAM_NATIVE_STEP("cr1",cam_native_write32(m,&entry,CAM_N_CR1,m->view.cr1));
    CAM_NATIVE_STEP("cr2",cam_native_write32(m,&entry,CAM_N_CR2,m->view.cr2));
    CAM_NATIVE_STEP("strtab-base",cam_native_write64(m,&entry,0,m->view.strtab_base));
    CAM_NATIVE_STEP("strtab-cfg",cam_native_write32(m,&entry,CAM_N_CFG,m->view.strtab_cfg));
    CAM_NATIVE_STEP("cmdq-base",cam_native_write64(m,&entry,1,m->view.cmdq_base));
    CAM_NATIVE_STEP("cmdq-prod",cam_native_cursor_reset(m,&entry,CAM_N_CP));
    CAM_NATIVE_STEP("cmdq-cons",cam_native_cursor_reset(m,&entry,CAM_N_CC));
    if(!e && entry.word[CAM_N_CP]!=entry.word[CAM_N_CC]){step="cmdq-adopt";e=CAM_FAIL(ESTALE);}
    if(!e && entry.word[CAM_N_CP]) {
        m->prod=m->cons=m->last_prod=m->last_cons=m->wait_cons=entry.word[CAM_N_CP];
    }
    /* As in Android, enable CMDQ before invalidations, then initialize EVTQ.
     * All four initial queue cursors were confirmed zero at admission. */
    CAM_NATIVE_STEP("cmdq-enable-ack",cam_native_enable(m,&entry,CAM_SMMU_CR0_CMDQEN));
    if(!e) {
        const struct cam_smmu_cmd cfg={{4,31}};
        CAM_NATIVE_STEP("cfgi-all",cam_smmu_submit(m,&cfg,1));
    }
    if(!e && (m->idr0&(1u<<9))) {
        const struct cam_smmu_cmd hyp={{0x20,0}};
        CAM_NATIVE_STEP("tlbi-el2-all",cam_smmu_submit(m,&hyp,1));
    }
    if(!e) {
        const struct cam_smmu_cmd tlbi={{0x30,0}}; /* TLBI_NSNH_ALL */
        CAM_NATIVE_STEP("tlbi-nsnh-all",cam_smmu_submit(m,&tlbi,1));
    }
    /* The owned queue advanced through SYNC; use its actual consumed
     * cursors in subsequent complete register comparisons. */
    if(!e){entry.word[CAM_N_CP]=m->prod;entry.word[CAM_N_CC]=m->cons;}
    CAM_NATIVE_STEP("evtq-base",cam_native_write64(m,&entry,2,m->view.evtq_base));
    if(!e)m->native_evtq_programmed=true;
    CAM_NATIVE_STEP("evtq-prod",cam_native_write32(m,&entry,CAM_N_EP,0));
    CAM_NATIVE_STEP("evtq-cons",cam_native_write32(m,&entry,CAM_N_EC,0));
    CAM_NATIVE_STEP("evtq-enable-ack",cam_native_enable(m,&entry,CAM_SMMU_CR0_CMDQEN|CAM_SMMU_CR0_EVTQEN));
    CAM_NATIVE_STEP("combined-irq",cam_smmu_irq_bind_locked(m,&entry));
    CAM_NATIVE_STEP("smmu-enable-ack",cam_native_enable(m,&entry,CAM_SMMU_CR0));
    CAM_NATIVE_STEP("live-check",cam_smmu_live(m));
#undef CAM_NATIVE_STEP
    if(!e){m->native_started=true;step="started";}
    else {cam_smmu_latch(m,e);if(e==ETIMEDOUT)cam_smmu_retire_timeout(m);}
    printf("X9 CAMERA NATIVE SMMU: step=%s sidbits=%u bypass=%u cr0=%x started=%u tcu_pf=%u "
        "prod=%x cons=%x errno=%d (own normal storage, retain MM power/backing)\n",
        step,sidbits,m->native_bypass,m->native_cr0,m->native_started,
        m->native_policy.tcu_prefetch,m->prod,m->cons,e);
    lck_mtx_unlock(&camera_smmu_lock);
    if(e) {
        int irq_cleanup=cam_smmu_irq_shutdown(m);
        if(irq_cleanup)printf("X9 CAMERA SMMU IRQ: startup cleanup errno=%d original=%d\n",irq_cleanup,e);
    }
    return e;
}
int x9_camera_native_smmu_start(uint64_t token,uint64_t iova,uint32_t bytes,
    struct x9_camera_smmu **out,struct x9_camera_smmu_export *view)
{ return cam_native_smmu_start_impl(token,iova,bytes,out,view,NULL); }

int x9_camera_native_smmu_check(struct x9_camera_smmu *m)
{
    if(!m || !m->native || !m->native_started || m->native_retire_attempted)return CAM_FAIL(ENXIO);
    return x9_camera_smmu_check(m);
}
/* Called only from native_retire.inc after its real, independently checked
 * source/CPU/worker/GIC/FBC/A0-bus admission. Control + IRQ mutex held.
 * The private guard performs NO DMA-owner callback or SMMU reentry. */
static int cam_native_smmu_retire_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    struct cam_native_snapshot prior,now;
    int e;
    {cam_native_match_step="retire:entry";cam_drain_mark(cam_native_match_step);}
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e && (!m->native || !m->native_started || m->native_retire_attempted ||
       m->native_restored || m->pending || m->shared_attempted ||
       m->native_cr0!=CAM_SMMU_CR0))e=CAM_FAIL(ESTALE);
    {cam_native_match_step="retire:storage";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_smmu_storage(m);
    {cam_native_match_step="retire:live";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_smmu_live(m);
    {cam_native_match_step="retire:admit";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    if(e)goto out;
    m->native_retire_attempted=true;m->native_retire_epoch=epoch;
    cam_native_prior(m,&prior);
    volatile uint64_t *ste=(volatile uint64_t *)m->ste.va+CAM_SMMU_SID*8;
    /* Actual BBM first breaks ONLY SID80. Non-camera STE policy is intact.
     * Record the intended phase before any uncertain cache publication. */
    {cam_native_match_step="retire:bbm";cam_drain_mark(cam_native_match_step);}
    m->native_ste_phase=1;ste[0]=0;__asm__ volatile("dsb sy":::"memory");
    {cam_native_match_step="retire:bbm-cache";cam_drain_mark(cam_native_match_step);}
    e=cam_cache_range(m->ste.va+CAM_SMMU_SID*64,64,CAM_CACHE_CLEAN);
    {cam_native_match_step="retire:bbm-guard";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    {cam_native_match_step="retire:cfgi-cd";cam_drain_mark(cam_native_match_step);}
    if(!e) {
        const struct cam_smmu_cmd cfg[2]={
            {{CAM_SMMU_CFGI_STE|((uint64_t)CAM_SMMU_SID<<32),1}},
            {{CAM_SMMU_CFGI_CD|((uint64_t)CAM_SMMU_SID<<32),1}}};
        e=cam_smmu_submit(m,cfg,2); /* actual CFGI80/CD80 plus trailing SYNC */
    }
    {cam_native_match_step="retire:cfgi-guard";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    {cam_native_match_step="retire:tlbi";cam_drain_mark(cam_native_match_step);}
    if(!e) {
        const struct cam_smmu_cmd tlbi={{CAM_SMMU_TLBI_NH_ASID|((uint64_t)CAM_SMMU_ASID<<48),0}};
        e=cam_smmu_submit(m,&tlbi,1); /* actual ASID1 invalidation + SYNC */
    }
    {cam_native_match_step="retire:tlbi-guard";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    {cam_native_match_step="retire:ste-bypass";cam_drain_mark(cam_native_match_step);}
    if(!e) {
        m->native_ste_phase=2;
        for(unsigned w=1;w<8;w++)ste[w]=w==1 && m->native_bypass && (m->idr1&(1u<<27))?1ull<<44:0;
        __asm__ volatile("dsb sy":::"memory");
        {cam_native_match_step="retire:ste-bypass-cache";cam_drain_mark(cam_native_match_step);}
        e=cam_cache_range(m->ste.va+CAM_SMMU_SID*64,64,CAM_CACHE_CLEAN);
    }
    {cam_native_match_step="retire:ste-bypass-guard";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    {cam_native_match_step="retire:ste-stream";cam_drain_mark(cam_native_match_step);}
    if(!e) {
        m->native_ste_phase=3;ste[0]=m->native_bypass?1ull|(4ull<<1):1ull;
        __asm__ volatile("dsb sy":::"memory");
        {cam_native_match_step="retire:ste-stream-cache";cam_drain_mark(cam_native_match_step);}
        e=cam_cache_range(m->ste.va+CAM_SMMU_SID*64,64,CAM_CACHE_CLEAN);
    }
    {cam_native_match_step="retire:ste-stream-cfgi";cam_drain_mark(cam_native_match_step);}
    if(!e) {
        const struct cam_smmu_cmd cfg={{CAM_SMMU_CFGI_STE|((uint64_t)CAM_SMMU_SID<<32),1}};
        e=cam_smmu_submit(m,&cfg,1);
    }
    {cam_native_match_step="retire:ste-stream-guard";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    {cam_native_match_step="retire:ste-live";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_smmu_storage(m);if(!e)e=cam_smmu_live(m);
    if(!e) {
        /* The IRQ worker takes camera_smmu_lock. Drop it while disabling and
         * draining the source, then revalidate the original producer proof. */
        {cam_native_match_step="retire:irq-shutdown";cam_drain_mark(cam_native_match_step);}
        lck_mtx_unlock(&camera_smmu_lock);
        e=cam_smmu_irq_shutdown(m);
        lck_mtx_lock(&camera_smmu_lock);
        if(!e)e=cam_smmu_identity(m);
        if(!e)e=cam_native_producer_guard_locked(epoch,m);
        if(!e)e=cam_smmu_live(m);
    }
    if(!e) {
        cam_native_read(m,&now);
        /* No untranslated source is left: exact OFF/gates and bus idle
         * precede SMMU/queue disable. GBPA is NEVER changed to hide traffic. */
        {cam_native_match_step="retire:disable";cam_drain_mark(cam_native_match_step);}
        e=cam_native_enable(m,&now,0);
    }
    for(unsigned i=0;i<3 && !e;i++) {
        {cam_native_match_step="retire:base";cam_drain_mark(cam_native_match_step);}
        e=cam_native_producer_guard_locked(epoch,m);
        if(!e)e=cam_native_restore_base(m,&now,i,prior.base[i]);
        if(!e && now.base[i]!=prior.base[i])prior.base[i]=now.base[i]; /* observed inert base */
    }
    const unsigned fields[]={CAM_N_CFG,CAM_N_CR1,CAM_N_CR2,CAM_N_CP,CAM_N_CC};
    const char *const field_steps[]={"retire:strtab-cfg","retire:cr1","retire:cr2",
        "retire:cmdq-prod","retire:cmdq-cons"};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]) && !e;i++) {
        {cam_native_match_step=field_steps[i];cam_drain_mark(cam_native_match_step);}
        e=cam_native_producer_guard_locked(epoch,m);
        /* diag16: split guard / pre-match / store / post-match (diag15 reset
         * twice right after the retire:cmdq-prod mark). */
        printf("X9 CAMERA RETIRE FIELD: %s guard=%d snapshot=%x prior=%x\n",field_steps[i],e,
            now.word[fields[i]],prior.word[fields[i]]);
        delay(20000);
        if(!e)x9_mm_power_smmu_dump(field_steps[i]);
        delay(20000);
        /* diag16 reset right after this point with CMDQ_BASE already restored
         * to 0 and PROD=CONS=0xd: storing PROD=0 alone makes the queue look
         * non-empty at PA 0. Linux device_reset never leaves PROD!=CONS with an
         * invalid base. Equal cursors are an empty, inert queue; the next owner
         * rewrites them before enabling CMDQ, so keep them and never store. */
        if(!e && (fields[i]==CAM_N_CP || fields[i]==CAM_N_CC)) {
            if(now.word[CAM_N_CP]!=now.word[CAM_N_CC]){e=CAM_FAIL(EBUSY);break;}
            printf("X9 CAMERA RETIRE FIELD: %s kept %x (PROD==CONS empty queue, no store)\n",
                field_steps[i],now.word[fields[i]]);
            prior.word[fields[i]]=now.word[fields[i]];
            continue;
        }
        /* CMDQ cursors are restored ONLY AFTER full-target real SYNC
         * consumption AND CR0/ACK CMDQEN=0. OPPO device_reset writes both
         * PROD/CONS. This is own disabled-queue restoration, not a fake
         * consumer advance or cancellation of a still pending command. */
        if(!e && (fields[i]==CAM_N_CFG || fields[i]==CAM_N_CR1 || fields[i]==CAM_N_CR2)) {
            uint32_t host_value=fields[i]==CAM_N_CFG?m->view.strtab_cfg:
                fields[i]==CAM_N_CR1?m->view.cr1:m->view.cr2;
            e=cam_native_restore_config(m,&now,fields[i],prior.word[fields[i]],host_value);
        } else if(!e) {
            e=cam_native_match(m,&now);
            printf("X9 CAMERA RETIRE FIELD: %s pre-match=%d\n",field_steps[i],e);delay(20000);
            if(!e) {
                cam_smmu_rw(m,cam_native_off[fields[i]],prior.word[fields[i]]);now.word[fields[i]]=prior.word[fields[i]];
                printf("X9 CAMERA RETIRE FIELD: %s stored %x\n",field_steps[i],prior.word[fields[i]]);delay(20000);
                e=cam_native_match(m,&now);
                printf("X9 CAMERA RETIRE FIELD: %s post-match=%d\n",field_steps[i],e);delay(20000);
            }
        }
        if(!e && (fields[i]==CAM_N_CFG || fields[i]==CAM_N_CR1 || fields[i]==CAM_N_CR2) &&
           now.word[fields[i]]!=prior.word[fields[i]])prior.word[fields[i]]=now.word[fields[i]];
    }
    /* No queue/table fetch or live translated source remains. Restore only
     * wrapper words changed by this host, keeping inherited bits exact.
     * A partial/failed store stays terminal with MM and all backing held. */
    for(unsigned i=4;i && !e;i--)if(m->native_wp_issued&(1u<<(i-1))) {
        {cam_native_match_step="retire:wrapper";cam_drain_mark(cam_native_match_step);}
        e=cam_native_producer_guard_locked(epoch,m);
        if(!e)e=cam_native_wp_write(m,&now,i-1,UINT32_MAX,prior.wp[i-1]);
    }
    if(!e){cam_native_match_step="retire:final-prior";cam_drain_mark(cam_native_match_step);}
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    if(!e)e=cam_native_match(m,&prior);
    if(!e) {
        delay(1);e=cam_native_producer_guard_locked(epoch,m);
        if(!e)e=cam_native_match(m,&prior);
    }
    if(!e) {
        m->native_restored=true;m->native_started=false;
        /* diag27: retire matched the bank against its observed-inert priors
         * (adopted bases, CR1/CR2/STRTAB_CFG, equal CMDQ cursors). Make that the
         * baseline for the retirement check and the next reopen; the boot-time
         * zeros it replaced failed both with ESTALE (MATCH DIFF word 5/6/12/13/14). */
        memcpy(m->native_prior_word,prior.word,sizeof(prior.word));
        memcpy(m->native_prior_base,prior.base,sizeof(prior.base));
        printf("X9 CAMERA NATIVE PRIOR UPDATE: cr1=%x cr2=%x cfg=%x cmdq=%x/%x bases=%llx/%llx/%llx\n",
            prior.word[CAM_N_CR1],prior.word[CAM_N_CR2],prior.word[CAM_N_CFG],
            prior.word[CAM_N_CP],prior.word[CAM_N_CC],prior.base[0],prior.base[1],prior.base[2]);
    }
out:
    if(e && m==cam_smmu_namespace && m && m->exported) {
        if(!m->native_retire_error)m->native_retire_error=e;
        cam_smmu_latch(m,e);if(e==ETIMEDOUT)cam_smmu_retire_timeout(m);
    }
    printf("X9 CAMERA NATIVE SMMU RETIRE: epoch=%llu attempt=%u ste_phase=%u "
        "cr0=%x restored=%u errno=%d (SID80+normal priors; all backing retained)\n",
        epoch,m?m->native_retire_attempted:0,m?m->native_ste_phase:0,
        m?m->native_cr0:0,m?m->native_restored:0,e);
    printf("X9 CAMERA NATIVE SMMU RETIRE STEP: last=%s\n",cam_native_match_step);
    cam_native_match_step="-";
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
