/* Native MT6993 normal SMMUv3 storage and serialized command engine.
 * Register/command facts: arm-smmu-v3-common.h and arm-smmu-v3.c
 * build_cmd/make_*_ste/__arm_smmu_cmdq_poll_until_consumed in OPPO 6.12.
 * No controller takeover here: the native boot owner still must supply
 * the actual cold bootstrap. Own-base equality is NOT a firmware grant.
 */
#include "x9_camera_smmu.h"
LCK_MTX_DECLARE(camera_smmu_lock,&camera_group);
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
#define CAM_SMMU_MAGIC 0x583943414d4d5531ull
enum { CAM_SMMU_SID=0x80, CAM_SMMU_DISPLAY=0x90, CAM_SMMU_ASID=1,
    CAM_SMMU_STES=256, CAM_SMMU_STE_WORDS=8,
    CAM_SMMU_CR1=(2u<<10)|(2u<<4), /* NC, outer-shareable table/queue */
    CAM_SMMU_CR2=6, /* private TLB maintenance + report invalid SID, !E2H */
    CAM_SMMU_CR0=13, /* SMMUEN|CMDQEN|EVTQEN, !PRI/ATS */
    CAM_SMMU_CMD_SYNC=0x46, CAM_SMMU_CFGI_STE=3, CAM_SMMU_CFGI_CD=5,
    CAM_SMMU_TLBI_NH_ASID=0x11 };
struct cam_smmu_cmd { uint64_t word[2]; };
_Static_assert(sizeof(struct cam_smmu_cmd)==16,"SMMUv3 command size");
_Static_assert(CAM_SMMU_STES*CAM_SMMU_STE_WORDS*sizeof(uint64_t)==CAM_S1_PAGE,
    "one wired stream-table page");

struct x9_camera_smmu {
    uint64_t magic;
    uint32_t idr0,idr1,idr5,cmd_shift,evt_shift,prod,cons,wait_cons;
    uint32_t last_errno,last_prod,last_cons,gerror,gerrorn,evt_prod,evt_cons;
    struct cam_s1_page ste,cmd,evt;
    struct x9_camera_s1 *map;
    struct x9_camera_smmu_export view;
    vm_offset_t mmio;
    uint64_t commands,syncs;
    bool exported,pending;
};
static struct x9_camera_smmu *cam_smmu_namespace;

static bool cam_smmu_capable(uint32_t idr0,uint32_t idr1,uint32_t idr5)
{
    unsigned cmd=(idr1>>21)&31u,evt=(idr1>>16)&31u,ttf=(idr0>>2)&3u;
    unsigned endian=(idr0>>21)&3u,stall=(idr0>>24)&3u;
    /* Preset queues/tables and forced stall need different ownership and
     * termination paths. SSID0 does not require nonzero IDR1.SSIDSIZE.
     */
    return PAGE_SHIFT==CAM_S1_SHIFT && (idr0&2u) && (ttf==2 || ttf==3) &&
        (endian==0 || endian==2) && stall<2 &&
        !(idr1&((1u<<29)|(1u<<30))) && (idr1&63u)>=8 && (idr1&63u)<=32 &&
        cmd>=2 && evt>=2 && (idr5&(1u<<5)) && (idr5&7u)<6;
}

static uint32_t cam_smmu_rr(struct x9_camera_smmu *m,unsigned off)
{ return *(volatile uint32_t *)(m->mmio+off); }
static uint64_t cam_smmu_rr64(struct x9_camera_smmu *m,unsigned off)
{ return *(volatile uint64_t *)(m->mmio+off); }
static void cam_smmu_rw(struct x9_camera_smmu *m,unsigned off,uint32_t v)
{
    *(volatile uint32_t *)(m->mmio+off)=v;
    __asm__ volatile("dsb sy":::"memory");
}

static int cam_smmu_latch(struct x9_camera_smmu *m,int e)
{
    if(!m->last_errno) {
        m->last_errno=(uint32_t)e;
        printf("X9 CAMERA SMMU: errno=%d exported=%u pending=%u prod=%x cons=%x "
            "observed=%x/%x GERROR=%x/%x EVTQ=%x/%x (retain, no retry)\n",
            e,m->exported,m->pending,m->prod,m->cons,m->last_prod,m->last_cons,
            m->gerror,m->gerrorn,m->evt_prod,m->evt_cons);
    }
    return (int)m->last_errno;
}

static int cam_smmu_identity(struct x9_camera_smmu *m)
{
    if(!m || m!=cam_smmu_namespace || m->magic!=CAM_SMMU_MAGIC)return EINVAL;
    if(m->last_errno)return (int)m->last_errno;
    return 0;
}

static void cam_smmu_unpublished_free(struct x9_camera_smmu *m)
{
    if(m->exported)panic("X9 CAMERA: free of exported SMMU backing");
    if(m->map && x9_camera_s1_discard(&m->map))
        panic("X9 CAMERA: unpublished SMMU has published Stage-1");
    cam_s1_free_page(&m->evt);cam_s1_free_page(&m->cmd);cam_s1_free_page(&m->ste);
    if(cam_smmu_namespace==m)cam_smmu_namespace=NULL;
    m->magic=0;kfree_type(struct x9_camera_smmu,m);
}

/* Validate all self-allocated backing before any device access. This reads
 * descriptors, never RAW pixels. Addresses cannot name an inherited table.
 */
static int cam_smmu_storage(struct x9_camera_smmu *m)
{
    const struct cam_s1_page *pages[]={&m->ste,&m->cmd,&m->evt};
    const volatile uint64_t *ste;
    unsigned oas;
    int e;
    if(!cam_smmu_capable(m->idr0,m->idr1,m->idr5) || !m->map ||
        m->cmd_shift<2 || m->cmd_shift>8 || m->evt_shift<2 || m->evt_shift>7)
        return EFAULT;
    oas=cam_s1_oas[m->idr5&7u];
    e=x9_camera_s1_validate(m->map);
    if(e)return e;
    for(unsigned i=0;i<3;i++) {
        uint64_t pa=0;
        e=cam_s1_phys(pages[i]->va,oas,&pa);
        if(e)return e;
        if(pa!=pages[i]->pa)return ESTALE;
        for(unsigned j=0;j<i;j++)if(pa==pages[j]->pa)return EFAULT;
        /* RAW must never be able to DMA over its translation or command
         * storage, even if a bad physical alias escaped the VM allocator. */
        if(pa==m->map->root.pa || pa==m->map->cd.pa)return EFAULT;
        for(unsigned j=0;j<m->map->leaves;j++)
            if(pa==m->map->leaf[j].pa)return EFAULT;
        for(unsigned j=0;j<m->map->pages;j++)
            if(pa==m->map->raw_pa[j])return EFAULT;
    }
    ste=(const volatile uint64_t *)m->ste.va;
    for(unsigned sid=0;sid<CAM_SMMU_STES;sid++) {
        for(unsigned w=0;w<CAM_SMMU_STE_WORDS;w++) {
            uint64_t expected=w==0?1ull:0;
            if(sid==CAM_SMMU_DISPLAY) {
                if(w==0)expected=1ull|(4ull<<1);
                if(w==1 && (m->idr1&(1u<<27)))expected=1ull<<44;
            } else if(sid==CAM_SMMU_SID && m->exported) {
                if(w==0)expected=1ull|(5ull<<1)|m->view.map.cd_pa;
                if(w==1) {
                    /* SSID0, NC CD fetch, outer shareable, NS-EL1, !ATS.
                     * S1STALLD only exists on a stall-capable SMMU.
                     */
                    expected=2ull|(2ull<<6);
                    if(((m->idr0>>24)&3u)==0)expected|=1ull<<27;
                }
            }
            if(ste[sid*CAM_SMMU_STE_WORDS+w]!=expected)return ESTALE;
        }
    }
    __asm__ volatile("dsb sy":::"memory");return 0;
}

int x9_camera_smmu_prepare(uint32_t idr0,uint32_t idr1,uint32_t idr5,
    uint64_t iova,uint32_t bytes,struct x9_camera_smmu **out)
{
    struct x9_camera_smmu *m;
    unsigned cmd=(idr1>>21)&31u,evt=(idr1>>16)&31u;
    int e;
    if(!out || *out)return EINVAL;
    if(!cam_smmu_capable(idr0,idr1,idr5))return ENOTSUP;
    lck_mtx_lock(&camera_smmu_lock);
    if(cam_smmu_namespace){lck_mtx_unlock(&camera_smmu_lock);return EBUSY;}
    m=kalloc_type(struct x9_camera_smmu,Z_WAITOK|Z_ZERO);
    if(!m){lck_mtx_unlock(&camera_smmu_lock);return ENOMEM;}
    m->magic=CAM_SMMU_MAGIC;m->idr0=idr0;m->idr1=idr1;m->idr5=idr5;
    m->cmd_shift=cmd>8?8:cmd;m->evt_shift=evt>7?7:evt;
    cam_smmu_namespace=m;
    e=x9_camera_s1_create(idr5,iova,bytes,&m->map);
    if(!e)e=cam_s1_alloc_page(&m->ste,cam_s1_oas[idr5&7u]);
    if(!e)e=cam_s1_alloc_page(&m->cmd,cam_s1_oas[idr5&7u]);
    if(!e)e=cam_s1_alloc_page(&m->evt,cam_s1_oas[idr5&7u]);
    if(!e) {
        uint64_t *ste=(uint64_t *)m->ste.va;
        for(unsigned sid=0;sid<CAM_SMMU_STES;sid++)ste[sid*CAM_SMMU_STE_WORDS]=1;
        ste[CAM_SMMU_DISPLAY*CAM_SMMU_STE_WORDS]=1ull|(4ull<<1);
        if(idr1&(1u<<27))ste[CAM_SMMU_DISPLAY*CAM_SMMU_STE_WORDS+1]=1ull<<44;
        m->mmio=ml_io_map(0x30800000,0x20000); /* mapping, not an MMIO read */
        if(!m->mmio)e=ENOMEM;
    }
    if(!e)e=cam_smmu_storage(m);
    if(e)cam_smmu_unpublished_free(m);
    else *out=m;
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

int x9_camera_smmu_export(struct x9_camera_smmu *m,struct x9_camera_smmu_export *out)
{
    int e;
    if(!out)return EINVAL;
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e && m->exported)e=EBUSY;
    if(!e)e=cam_smmu_storage(m);
    if(!e) {
        uint64_t *ste=(uint64_t *)m->ste.va;
        /* Pin queues/STE before Stage-1 export, including its failure. */
        m->exported=true;
        e=x9_camera_s1_export(m->map,CAM_SMMU_ASID,&m->view.map);
        if(!e) {
            ste[CAM_SMMU_SID*CAM_SMMU_STE_WORDS+1]=2ull|(2ull<<6)|
                ((((m->idr0>>24)&3u)==0)?1ull<<27:0);
            __asm__ volatile("dsb sy":::"memory");
            ste[CAM_SMMU_SID*CAM_SMMU_STE_WORDS]=1ull|(5ull<<1)|m->view.map.cd_pa;
            m->view.strtab_base=m->ste.pa;m->view.strtab_cfg=8;
            m->view.cmdq_base=m->cmd.pa|m->cmd_shift;
            m->view.evtq_base=m->evt.pa|m->evt_shift;
            m->view.cr1=CAM_SMMU_CR1;m->view.cr2=CAM_SMMU_CR2;
            m->view.cmdq_shift=m->cmd_shift;m->view.evtq_shift=m->evt_shift;
            m->view.asid=CAM_SMMU_ASID;
            e=cam_smmu_storage(m);
        }
        if(!e) {
            clean_dcache64(m->ste.pa,CAM_S1_PAGE,TRUE);
            flush_dcache64(m->cmd.pa,CAM_S1_PAGE,TRUE);
            flush_dcache64(m->evt.pa,CAM_S1_PAGE,TRUE);
            __asm__ volatile("dsb sy":::"memory");
            *out=m->view;
        } else cam_smmu_latch(m,e);
    }
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

int x9_camera_smmu_discard(struct x9_camera_smmu **map)
{
    int e;
    if(!map)return EINVAL;
    lck_mtx_lock(&camera_smmu_lock);
    e=0;
    if(!*map || *map!=cam_smmu_namespace || (*map)->magic!=CAM_SMMU_MAGIC)e=EINVAL;
    else if((*map)->exported)e=EBUSY;
    if(!e){cam_smmu_unpublished_free(*map);*map=NULL;}
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

/* The powered, authoritative native controller must own this namespace.
 * These checks prevent this engine from writing to an inherited CMDQ or
 * accepting base redirection by an active HYP interceptor. They cannot
 * establish firmware authority by themselves, and never set a lease bit.
 */
static int cam_smmu_live(struct x9_camera_smmu *m)
{
    uint32_t mask=(1u<<(m->cmd_shift+1))-1u;
    uint32_t evt_mask=(1u<<(m->evt_shift+1))-1u;
    if(!m->exported)return ENXIO;
    if(cam_smmu_rr(m,0)!=m->idr0 || cam_smmu_rr(m,4)!=m->idr1 ||
        cam_smmu_rr(m,0x14)!=m->idr5 ||
        cam_smmu_rr64(m,0x80)!=m->view.strtab_base || cam_smmu_rr(m,0x88)!=8 ||
        cam_smmu_rr64(m,0x90)!=m->view.cmdq_base ||
        cam_smmu_rr64(m,0xa0)!=m->view.evtq_base ||
        cam_smmu_rr(m,0x20)!=CAM_SMMU_CR0 || cam_smmu_rr(m,0x24)!=CAM_SMMU_CR0 ||
        cam_smmu_rr(m,0x28)!=CAM_SMMU_CR1 || cam_smmu_rr(m,0x2c)!=CAM_SMMU_CR2 ||
        (cam_smmu_rr(m,0x44)&((1u<<31)|(1u<<20)))!=(1u<<20) ||
        cam_smmu_rr(m,0x50) || cam_smmu_rr(m,0x54))return ESTALE;
    m->last_prod=cam_smmu_rr(m,0x98);m->last_cons=cam_smmu_rr(m,0x9c);
    m->gerror=cam_smmu_rr(m,0x60);m->gerrorn=cam_smmu_rr(m,0x64);
    /* EVTQ page1, NOT CMDQ's page0. Leave foreign/unknown events pending;
     * event acknowledgement/IRQ ownership is not implemented here.
     */
    m->evt_prod=cam_smmu_rr(m,0x100a8);m->evt_cons=cam_smmu_rr(m,0x100ac);
    __asm__ volatile("dsb sy":::"memory");
    if(m->gerror^m->gerrorn || (m->last_cons&~mask) ||
        (m->evt_prod&~(evt_mask|(1u<<31))) || m->evt_cons || m->evt_prod)return EIO;
    if(m->last_prod!=m->prod)return ESTALE;
    if(!m->pending && m->last_cons!=m->cons)return ESTALE;
    if(m->pending) {
        /* Only our serial batch is in flight. Consumer must move forward
         * inside [last_confirmed_cons, submitted_prod], including wrap.
         */
        uint32_t remaining=(m->prod-m->wait_cons)&mask;
        uint32_t advanced=(m->last_cons-m->wait_cons)&mask;
        if(advanced>remaining)return ESTALE;
        m->wait_cons=m->last_cons;
    }
    return 0;
}

int x9_camera_smmu_check(struct x9_camera_smmu *m)
{
    int e;
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e)e=cam_smmu_storage(m);
    if(!e)e=cam_smmu_live(m);
    if(e && m==cam_smmu_namespace && m && m->exported)cam_smmu_latch(m,e);
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

/* One batch + one architected SYNC, no restart, blind retry, global TLBI,
 * consumer writes, IRQ enables or GERRORN acknowledgements. CS_NONE needs
 * neither MSI nor SEV: advancing CONS beyond SYNC is the completion proof.
 */
static int cam_smmu_submit(struct x9_camera_smmu *m,const struct cam_smmu_cmd *cmd,
    unsigned count)
{
    uint32_t ring=(1u<<m->cmd_shift),mask=2*ring-1,old=m->prod;
    uint64_t interval,deadline;
    int e;
    if(m->pending || m->prod!=m->cons)return EBUSY;
    if(count+1>ring || count>2)return EINVAL;
    e=cam_smmu_storage(m);if(!e)e=cam_smmu_live(m);
    if(e)return cam_smmu_latch(m,e);
    for(unsigned i=0;i<=count;i++) {
        unsigned slot=(old+i)&(ring-1);
        struct cam_smmu_cmd *entry=(struct cam_smmu_cmd *)m->cmd.va+slot;
        *entry=i<count?cmd[i]:(struct cam_smmu_cmd){{CAM_SMMU_CMD_SYNC,0}};
        clean_dcache64(m->cmd.pa+(uint64_t)slot*16,16,TRUE);
    }
    __asm__ volatile("dsb sy":::"memory");
    /* Recheck producer/config after CPU cache work but before the only
     * device write. This is still not atomic exclusion of firmware users.
     */
    e=cam_smmu_live(m);if(e)return cam_smmu_latch(m,e);
    m->wait_cons=m->cons;m->prod=(old+count+1)&mask;m->pending=true;
    cam_smmu_rw(m,0x98,m->prod);
    nanoseconds_to_absolutetime(10000000ull,&interval);
    deadline=mach_absolute_time()+interval;
    for(unsigned poll=0;poll<10000;poll++) {
        e=cam_smmu_live(m);if(e)return cam_smmu_latch(m,e);
        if(m->last_cons==m->prod) {
            m->cons=m->prod;m->pending=false;m->commands+=count;m->syncs++;
            __asm__ volatile("dsb sy":::"memory");return 0;
        }
        if(mach_absolute_time()>=deadline)break;
        delay(1);
    }
    return cam_smmu_latch(m,ETIMEDOUT);
}

static int cam_smmu_commands(struct x9_camera_smmu *m,unsigned operation)
{
    struct cam_smmu_cmd cmd[2]={0};
    unsigned count=0;
    int e;
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e && !m->exported)e=ENXIO;
    if(!e) {
        if(operation==1) {
            cmd[0]=(struct cam_smmu_cmd){{CAM_SMMU_CFGI_STE|((uint64_t)CAM_SMMU_SID<<32),1}};
            cmd[1]=(struct cam_smmu_cmd){{CAM_SMMU_CFGI_CD|((uint64_t)CAM_SMMU_SID<<32),1}};
            count=2;
        } else if(operation==2) {
            cmd[0]=(struct cam_smmu_cmd){{CAM_SMMU_TLBI_NH_ASID|
                ((uint64_t)CAM_SMMU_ASID<<48),0}};count=1;
        }
        e=cam_smmu_submit(m,cmd,count);
    }
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
int x9_camera_smmu_cfgi(struct x9_camera_smmu *m){return cam_smmu_commands(m,1);}
int x9_camera_smmu_tlbi(struct x9_camera_smmu *m){return cam_smmu_commands(m,2);}
int x9_camera_smmu_sync(struct x9_camera_smmu *m){return cam_smmu_commands(m,0);}
