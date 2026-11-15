/* Native cold normal-bank host. Register order/CFG/TLB commands follow
 * OPPO arm_smmu_device_reset, but without resetting an inherited controller,
 * touching secure registers, HYP sharing, or changing other-stream policy.
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
    uint64_t base[3];
};
static const unsigned cam_native_base_off[3]={0x80,0x90,0xa0};
static vm_offset_t cam_native_mmio;
static void cam_native_read(struct x9_camera_smmu *m,struct cam_native_snapshot *s)
{
    bzero(s,sizeof(*s));
    for(unsigned i=0;i<CAM_N_WORDS;i++)s->word[i]=cam_smmu_rr(m,cam_native_off[i]);
    for(unsigned i=0;i<3;i++)s->base[i]=cam_smmu_rr64(m,cam_native_base_off[i]);
    __asm__ volatile("dsb sy":::"memory");
}
static int cam_native_match(struct x9_camera_smmu *m,const struct cam_native_snapshot *s)
{
    struct cam_native_snapshot now;
    int e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);
    if(e)return e;
    cam_native_read(m,&now);
    return memcmp(s,&now,sizeof(now))?ESTALE:0;
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
    *(volatile uint64_t *)(m->mmio+cam_native_base_off[field])=value;
    __asm__ volatile("dsb sy":::"memory");s->base[field]=value;
    return cam_native_match(m,s);
}
static int cam_native_enable(struct x9_camera_smmu *m,struct cam_native_snapshot *s,
    uint32_t value)
{
    int e=cam_native_match(m,s);if(e)return e;
    uint32_t before=s->word[CAM_N_ACK];
    m->native_cr0=value;cam_smmu_rw(m,0x20,value);s->word[CAM_N_CR0]=value;
    uint64_t span;nanoseconds_to_absolutetime(10000000ull,&span);
    uint64_t deadline=mach_absolute_time()+span;
    for(unsigned n=0;n<10000;n++) {
        struct cam_native_snapshot now;
        e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);if(e)return e;
        cam_native_read(m,&now);
        uint32_t ack=now.word[CAM_N_ACK];
        if(ack!=before && ack!=value)return ESTALE;
        now.word[CAM_N_ACK]=s->word[CAM_N_ACK];
        if(memcmp(s,&now,sizeof(now)))return ESTALE;
        if(ack==value){s->word[CAM_N_ACK]=value;return 0;}
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        delay(1);
    }
    return ETIMEDOUT;
}
static int cam_native_full_table(struct x9_camera_smmu *m,unsigned shift,bool bypass)
{
    vm_offset_t va=0;uint64_t pa=0;unsigned bytes=64u<<shift;
    if(shift<8 || shift>16 || m->exported)return EINVAL;
    if(bytes!=m->ste_bytes) {
        if(kmem_alloc_contig(kernel_map,&va,bytes,PAGE_MASK,0,0,
            KMA_DATA|KMA_ZERO,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
        int e=cam_s1_phys(va,cam_s1_oas[m->idr5&7u],&pa);
        for(unsigned off=CAM_S1_PAGE;off<bytes && !e;off+=CAM_S1_PAGE) {
            uint64_t next=0;e=cam_s1_phys(va+off,cam_s1_oas[m->idr5&7u],&next);
            if(!e && next!=pa+off)e=EFAULT;
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
    return cam_smmu_storage(m);
}
int x9_camera_native_smmu_start(uint64_t token,uint64_t iova,uint32_t bytes,
    struct x9_camera_smmu **out,struct x9_camera_smmu_export *view)
{
    vm_offset_t mmio;
    struct cam_native_snapshot entry,again;
    struct x9_camera_smmu probe={0},*m=NULL;
    int e;
    if(!out || *out || !view || !token)return EINVAL;
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=4)return ENOTSUP; /* normal EL1 host, not a guessed EL2 handoff */
    e=x9_mm_power_validate(token,X9_MM_POWER_CAMERA);if(e)return e;
    lck_mtx_lock(&camera_smmu_lock);
    if(!cam_native_mmio)cam_native_mmio=ml_io_map(0x30800000,0x20000);
    mmio=cam_native_mmio;
    lck_mtx_unlock(&camera_smmu_lock);
    if(!mmio)return ENOMEM;
    probe.mmio=mmio;probe.native_power=token;
    cam_native_read(&probe,&entry);cam_native_read(&probe,&again);
    if(memcmp(&entry,&again,sizeof(entry)))return EBUSY;
    unsigned sidbits=entry.word[CAM_N_IDR1]&63u;
    if(!cam_smmu_capable(entry.word[CAM_N_IDR0],entry.word[CAM_N_IDR1],
         entry.word[CAM_N_IDR5]) || sidbits<8 || sidbits>16)return ENOTSUP;
    /* No normal bases/config/queue/interface are inherited. Initial GBPA
     * is preserved exactly; abort and bypass both have complete STE policies. */
    if(entry.base[0] || entry.base[1] || entry.base[2] ||
       entry.word[CAM_N_CR0] || entry.word[CAM_N_ACK] || entry.word[CAM_N_CFG] ||
       entry.word[CAM_N_IRQ] || entry.word[CAM_N_IRQACK] ||
       entry.word[CAM_N_CP] || entry.word[CAM_N_CC] ||
       entry.word[CAM_N_EP] || entry.word[CAM_N_EC] ||
       (entry.word[CAM_N_GBPA]&(1u<<31)))return EBUSY;
    if(entry.word[CAM_N_GE]^entry.word[CAM_N_GEN])return EIO;
    e=x9_camera_smmu_prepare(entry.word[CAM_N_IDR0],entry.word[CAM_N_IDR1],
        entry.word[CAM_N_IDR5],iova,bytes,&m);if(e)return e;
    lck_mtx_lock(&camera_smmu_lock);
    m->native_power=token;m->native_gbpa=entry.word[CAM_N_GBPA];
    e=cam_native_full_table(m,sidbits,!(m->native_gbpa&(1u<<20)));
    if(!e)e=cam_native_match(m,&entry);
    lck_mtx_unlock(&camera_smmu_lock);
    if(e){(void)x9_camera_smmu_discard(&m);return e;}
    e=x9_camera_smmu_export(m,view);
    /* Addresses permanently escaped even when export's cache work fails. */
    if(m->exported)*out=m;
    if(e)return e;
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_native_match(m,&entry);
    if(!e)e=cam_native_write32(m,&entry,CAM_N_CR1,m->view.cr1);
    if(!e)e=cam_native_write32(m,&entry,CAM_N_CR2,m->view.cr2);
    if(!e)e=cam_native_write64(m,&entry,0,m->view.strtab_base);
    if(!e)e=cam_native_write32(m,&entry,CAM_N_CFG,m->view.strtab_cfg);
    if(!e)e=cam_native_write64(m,&entry,1,m->view.cmdq_base);
    if(!e)e=cam_native_write64(m,&entry,2,m->view.evtq_base);
    /* Program owned bases before enabling queue fetch. No write to initial
     * zero producer/consumer, IRQ, EVTQ ACK, GERRORN, GBPA or secure bank. */
    if(!e)e=cam_native_enable(m,&entry,4);
    if(!e) {
        const struct cam_smmu_cmd cfg={{4,31}};
        e=cam_smmu_submit(m,&cfg,1);
    }
    if(!e && (m->idr0&(1u<<9))) {
        const struct cam_smmu_cmd hyp={{0x20,0}};
        e=cam_smmu_submit(m,&hyp,1);
    }
    if(!e) {
        const struct cam_smmu_cmd tlbi={{0x30,0}}; /* TLBI_NSNH_ALL */
        e=cam_smmu_submit(m,&tlbi,1);
    }
    /* The owned queue advanced through SYNC; use its actual consumed
     * cursors in subsequent complete register comparisons. */
    if(!e){entry.word[CAM_N_CP]=m->prod;entry.word[CAM_N_CC]=m->cons;}
    if(!e)e=cam_native_enable(m,&entry,12);
    if(!e)e=cam_native_enable(m,&entry,13);
    if(!e)e=cam_smmu_live(m);
    if(!e)m->native_started=true;
    else {cam_smmu_latch(m,e);if(e==ETIMEDOUT)cam_smmu_retire_timeout(m);}
    printf("X9 CAMERA NATIVE SMMU: sidbits=%u bypass=%u cr0=%x started=%u "
        "prod=%x cons=%x errno=%d (own normal storage, retain MM power/backing)\n",
        sidbits,m->native_bypass,m->native_cr0,m->native_started,m->prod,m->cons,e);
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
int x9_camera_native_smmu_check(struct x9_camera_smmu *m)
{
    if(!m || !m->native || !m->native_started)return ENXIO;
    return x9_camera_smmu_check(m);
}
