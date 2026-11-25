/* Native MT6993 normal SMMUv3 storage and serialized command engine.
 * Register/command facts: arm-smmu-v3-common.h and arm-smmu-v3.c
 * build_cmd/make_*_ste/__arm_smmu_cmdq_poll_until_consumed in OPPO 6.12.
 * No controller takeover here: the native boot owner still must supply
 * the actual cold bootstrap. Own-base equality is NOT a firmware grant.
 */
#include "x9_camera_smmu.h"
#include "x9_camera_smmu_policy.h"
#include "x9_mm_power.h"
#include <pexpert/device_tree.h>
LCK_MTX_DECLARE(camera_smmu_lock,&camera_group);
#include "x9_camera_smmu_irq_layout.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
#define CAM_SMMU_MAGIC 0x583943414d4d5531ull
/* live.dts mm-smmu-cam uses iommus=<MM_SMMU 0x80>; generic
 * arm_smmu_of_xlate passes that ID unchanged. CAMSV0's separate
 * mediatek,smmu-dma-axid=<262 260 1c0 3a0> registers fault callbacks
 * (mtk_cam-sv.c:4282+), not STE indices or an AID->SID programming list.
 * smmu-secure.c's aid2sid is CONFIG_MTK_IOMMU_DEBUG-only. Do not turn
 * the CAMSV fault-source IDs into a new secure mapping/SMC sequence. */
enum { CAM_SMMU_SID=0x80, CAM_SMMU_DISPLAY=0x90, CAM_SMMU_ASID=1,
    CAM_SMMU_STES=256, CAM_SMMU_STE_WORDS=8,
    CAM_SMMU_CR1=(3u<<10)|(3u<<4), /* NC (mtk_smmu_device_reset), ISH table/queue as Android */
    CAM_SMMU_CR2=6, /* private TLB maintenance + report invalid SID, !E2H */
    /* arm-smmu-v3-common.h: CR0_CMDQEN is bit3, EVTQEN is bit2.
     * device_reset enables CMDQ alone before its first CFGI_ALL + SYNC. */
    CAM_SMMU_CR0_CMDQEN=1u<<3, CAM_SMMU_CR0_EVTQEN=1u<<2,
    CAM_SMMU_CR0_SMMUEN=1u<<0,
    CAM_SMMU_CR0=CAM_SMMU_CR0_CMDQEN|CAM_SMMU_CR0_EVTQEN|CAM_SMMU_CR0_SMMUEN,
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
    uint32_t evt_owned_cons,evt_last_prod;
    uint64_t evt_events,evt_overflows;
    struct cam_s1_page ste,cmd,evt;
    struct x9_camera_s1 *map;
    struct x9_camera_smmu_export view;
    vm_offset_t mmio;
    uint64_t commands,syncs;
    struct cam_smmu_cmd batch[3];
    uint32_t batch_start,batch_count,close_samples,close_errno;
    bool exported,pending,close_attempted,timedout_retired,shared_attempted;
    /* Cold native host owns all normal controller stores in this TU. */
    bool native, native_bypass, native_started, native_evtq_programmed;
    uint32_t ste_shift, ste_bytes, native_cr0, native_gbpa, native_irq_ctrl;
    uint64_t native_power;
    bool native_retire_attempted,native_restored,native_setup_issued,native_unstarted_retired;
    bool native_epoch_recycled;
    /* diag32: STRTAB/CMDQ/EVTQ pages borrowed from native_previous (which
     * stays exported and is never freed). The controller keeps only the
     * first base written per boot, as Android reuses its probe-time tables. */
    bool native_queues_adopted;
    /* diag34: Android lifecycle. arm_smmu_device_reset runs once per boot;
     * the bank then stays CR0=CMDQEN|EVTQEN|SMMUEN with the same STRTAB/CMDQ/
     * EVTQ bases. A session only switches STE80 (S1 <-> abort) through the
     * live CMDQ. Set on the cold first start and on every live reopen. */
    bool native_live;
    struct x9_camera_smmu *native_previous;
    uint32_t native_ste_phase,native_retire_error,native_prior_word[17];
    uint64_t native_retire_epoch,native_prior_base[3];
    vm_offset_t native_wp;
    uint32_t native_prior_wp[4],native_wp_expected[4],native_wp_issued;
    struct x9_camera_smmu_policy native_policy;
};
static struct x9_camera_smmu *cam_smmu_namespace;
struct cam_native_snapshot;
static int cam_smmu_irq_bind_locked(struct x9_camera_smmu *,struct cam_native_snapshot *);
static int cam_smmu_irq_shutdown(struct x9_camera_smmu *);
static bool cam_smmu_irq_filter(uint32_t);
static void cam_smmu_irq_notify_fault(int);
static int cam_smmu_shared_check_locked(struct x9_camera_smmu *);
static int cam_native_wp_guard(struct x9_camera_smmu *);
static int cam_smmu_policy_read(struct x9_camera_smmu_policy *out)
{
    DTEntry chosen;const void *p=NULL;unsigned n=0;
    _Static_assert(sizeof(*out)==X9_CAM_SMMU_POLICY_BYTES,"camera SMMU policy");
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-camera-smmu-policy",&p,&n)!=kSuccess ||
       !p || n!=sizeof(*out))return CAM_FAIL(ENXIO);
    memcpy(out,p,sizeof(*out));
    return x9_camera_smmu_policy_valid(out)?0:EINVAL;
}
static uint64_t cam_smmu_s1_ste_word1(const struct x9_camera_smmu *m)
{
    /* arm_smmu_make_cdtable_ste + mtk_smmu_setup_features:
     * this fixed non-ATS master has a valid CD pointer, so DT TCU_PF
     * applies to bits57:56. Keep NC/outer-shareable CD fetch and NS-EL1.
     * The inherited-host API retains its original provider contract. */
    return 2ull|(3ull<<6)|((((m->idr0>>24)&3u)==0)?1ull<<27:0)|
        (m->native?(uint64_t)m->native_policy.tcu_prefetch<<56:0);
}

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

/* Android arm_smmu_rpm_get: MM SMMU registers are touched only while the
 * MM_2 + MM_INFRA AO/0/1/2 power ACKs are live. A lost domain latches and
 * suppresses ALL later SMMU MMIO (reads return ~0, writes are dropped), so
 * callers fail with ESTALE instead of hanging the bus. */
static bool cam_smmu_power_lost;
static uint64_t cam_smmu_power_token;
static bool cam_smmu_mmio_ok(unsigned off)
{
    if(cam_smmu_power_lost)return false;
    if(x9_mm_power_smmu_live())return true;
    cam_smmu_power_lost=true;
    printf("X9 CAMERA SMMU POWER LOST: off=%x; all further MM SMMU MMIO suppressed\n",off);
    x9_mm_power_smmu_dump("camera-lost");
    return false;
}
static int cam_smmu_power_up(uint64_t token)
{
    if(cam_smmu_power_token==token && !cam_smmu_power_lost)return 0;
    if(cam_smmu_power_token)return CAM_FAIL(EBUSY); /* old token still journaled */
    int e=x9_mm_power_smmu_get(token,X9_MM_POWER_CAMERA);
    if(e)return CAM_FAIL(e);
    cam_smmu_power_token=token;cam_smmu_power_lost=false;
    return 0;
}
static void cam_smmu_power_down(uint64_t token)
{
    if(!token || cam_smmu_power_token!=token)return;
    (void)x9_mm_power_smmu_put(token,X9_MM_POWER_CAMERA);
    cam_smmu_power_token=0;
}
/* 384/386/388 hung in back-to-back MM SMMU reads without barriers; 389 passed
 * with a dsb around every read, and mm_read (x9_mm_power) has never hung.
 * Keep at most one MM SMMU/wrapper access outstanding. */
static uint32_t cam_smmu_rd32(vm_offset_t a)
{ uint32_t v;__asm__ volatile("dsb sy":::"memory");v=*(volatile uint32_t *)a;__asm__ volatile("dsb sy":::"memory");return v; }
static uint64_t cam_smmu_rd64(vm_offset_t a)
{ uint64_t v;__asm__ volatile("dsb sy":::"memory");v=*(volatile uint64_t *)a;__asm__ volatile("dsb sy":::"memory");return v; }
static uint32_t cam_smmu_rr(struct x9_camera_smmu *m,unsigned off)
{ return cam_smmu_mmio_ok(off)?cam_smmu_rd32(m->mmio+off):~0u; }
static uint64_t cam_smmu_rr64(struct x9_camera_smmu *m,unsigned off)
{ return cam_smmu_mmio_ok(off)?cam_smmu_rd64(m->mmio+off):~0ull; }
static void cam_smmu_rw(struct x9_camera_smmu *m,unsigned off,uint32_t v)
{
    if(!cam_smmu_mmio_ok(off))return;
    *(volatile uint32_t *)(m->mmio+off)=v;
    __asm__ volatile("dsb sy":::"memory");
}
static void cam_smmu_rw64(struct x9_camera_smmu *m,unsigned off,uint64_t v)
{
    if(!cam_smmu_mmio_ok(off))return;
    *(volatile uint64_t *)(m->mmio+off)=v;
    __asm__ volatile("dsb sy":::"memory");
}
static uint32_t cam_smmu_wp_rr(struct x9_camera_smmu *m,unsigned off)
{ return cam_smmu_mmio_ok(0x1e0000+off)?cam_smmu_rd32(m->native_wp+off):~0u; }
static void cam_smmu_wp_rw(struct x9_camera_smmu *m,unsigned off,uint32_t v)
{
    if(!cam_smmu_mmio_ok(0x1e0000+off))return;
    *(volatile uint32_t *)(m->native_wp+off)=v;
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
    if(!m || m!=cam_smmu_namespace || m->magic!=CAM_SMMU_MAGIC)return CAM_FAIL(EINVAL);
    if(m->last_errno)return (int)m->last_errno;
    return 0;
}

static void cam_smmu_unpublished_free(struct x9_camera_smmu *m)
{
    if(m->exported)panic("X9 CAMERA: free of exported SMMU backing");
    if(m->map && x9_camera_s1_discard(&m->map))
        panic("X9 CAMERA: unpublished SMMU has published Stage-1");
    if(!m->native_queues_adopted) {
        cam_s1_free_page(&m->evt);cam_s1_free_page(&m->cmd);
        if(m->ste.va)kmem_free(kernel_map,m->ste.va,m->ste_bytes);
    }
    if(cam_smmu_namespace==m)cam_smmu_namespace=m->native_previous;
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
        return CAM_FAIL(EFAULT);
    if(m->native && !x9_camera_smmu_policy_valid(&m->native_policy))return CAM_FAIL(ESTALE);
    oas=cam_s1_oas[m->idr5&7u];
    if(m->native)cam_progress("st:s1-validate (camera_s1_lock)");
    e=x9_camera_s1_validate(m->map);
    if(e)return e;
    if(m->native)cam_progress("st:pages");
    for(unsigned i=0;i<3;i++) {
        uint64_t pa=0;
        e=cam_s1_phys(pages[i]->va,oas,&pa);
        if(e)return e;
        if(pa!=pages[i]->pa)return CAM_FAIL(ESTALE);
        for(unsigned j=0;j<i;j++)if(pa==pages[j]->pa)return CAM_FAIL(EFAULT);
        /* RAW must never be able to DMA over its translation or command
         * storage, even if a bad physical alias escaped the VM allocator. */
        if(pa==m->map->root.pa || pa==m->map->cd.pa)return CAM_FAIL(EFAULT);
        for(unsigned j=0;j<m->map->leaves;j++)
            if(pa==m->map->leaf[j].pa)return CAM_FAIL(EFAULT);
        for(unsigned j=0;j<m->map->pages;j++)
            if(pa==m->map->raw_pa[j])return CAM_FAIL(EFAULT);
    }
    if(m->ste_shift<8 || m->ste_shift>16 ||
       m->ste_bytes!=(64u<<m->ste_shift) ||
       (!m->native && m->ste_shift!=8))return CAM_FAIL(EFAULT);
    for(unsigned off=0;off<m->ste_bytes;off+=CAM_S1_PAGE) {
        uint64_t pa=0;e=cam_s1_phys(m->ste.va+off,oas,&pa);
        if(e)return e;
        if(pa!=m->ste.pa+off)return CAM_FAIL(ESTALE);
        for(unsigned j=1;j<3;j++)if(pa==pages[j]->pa)return CAM_FAIL(EFAULT);
        if(pa==m->map->root.pa || pa==m->map->cd.pa)return CAM_FAIL(EFAULT);
        for(unsigned j=0;j<m->map->leaves;j++)
            if(pa==m->map->leaf[j].pa)return CAM_FAIL(EFAULT);
        for(unsigned j=0;j<m->map->pages;j++)
            if(pa==m->map->raw_pa[j])return CAM_FAIL(EFAULT);
    }
    if(m->native)cam_progress("st:ste-compare");
    ste=(const volatile uint64_t *)m->ste.va;
    for(unsigned sid=0;sid<(1u<<m->ste_shift);sid++) {
        for(unsigned w=0;w<CAM_SMMU_STE_WORDS;w++) {
            uint64_t expected=w==0?1ull:0;
            if((m->native && m->native_bypass && sid!=CAM_SMMU_SID) ||
               (!m->native && sid==CAM_SMMU_DISPLAY)) {
                if(w==0)expected=1ull|(4ull<<1);
                if(w==1 && (m->idr1&(1u<<27)))expected=1ull<<44;
            } else if(sid==CAM_SMMU_SID && m->native && m->native_ste_phase && m->native_live) {
                /* Live bank: phase1 V=0 (word1 still S1), phase2 words1..7=0,
                 * phase3 V=1 Config=abort. Never bypass a translated master
                 * while SMMUEN stays on. */
                if(w==0)expected=m->native_ste_phase==3?1ull:0;
                if(w==1 && m->native_ste_phase==1)expected=cam_smmu_s1_ste_word1(m);
            } else if(sid==CAM_SMMU_SID && m->native && m->native_ste_phase) {
                if(w==0)expected=m->native_ste_phase==3?
                    (m->native_bypass?1ull|(4ull<<1):1ull):0;
                if(w==1) {
                    if(m->native_ste_phase==1) {
                        expected=cam_smmu_s1_ste_word1(m);
                    } else if(m->native_bypass && (m->idr1&(1u<<27)))expected=1ull<<44;
                }
            } else if(sid==CAM_SMMU_SID && m->exported) {
                if(w==0)expected=1ull|(5ull<<1)|m->view.map.cd_pa;
                if(w==1) {
                    /* SSID0, NC CD fetch, outer shareable, NS-EL1, !ATS.
                     * S1STALLD only exists on a stall-capable SMMU.
                     */
                    expected=cam_smmu_s1_ste_word1(m);
                }
            }
            if(ste[sid*CAM_SMMU_STE_WORDS+w]!=expected)return CAM_FAIL(ESTALE);
        }
    }
    __asm__ volatile("dsb sy":::"memory");return 0;
}

static int cam_smmu_prepare_replacement(uint32_t idr0,uint32_t idr1,uint32_t idr5,
    uint64_t iova,uint32_t bytes,struct x9_camera_smmu *previous,bool live,
    struct x9_camera_smmu **out)
{
    struct x9_camera_smmu *m;
    unsigned cmd=(idr1>>21)&31u,evt=(idr1>>16)&31u;
    int e;
    if(!out || *out)return CAM_FAIL(EINVAL);
    if(!cam_smmu_capable(idr0,idr1,idr5))return CAM_FAIL(ENOTSUP);
    lck_mtx_lock(&camera_smmu_lock);
    if(cam_smmu_namespace!=previous || (live && (!previous || !previous->native_live)) || (previous &&
       (!previous->native || !previous->native_restored || !previous->native_epoch_recycled ||
        previous->native_started || previous->pending || previous->native_retire_error))) {
        lck_mtx_unlock(&camera_smmu_lock);return CAM_FAIL(EBUSY);
    }
    m=kalloc_type(struct x9_camera_smmu,Z_WAITOK|Z_ZERO);
    if(!m){lck_mtx_unlock(&camera_smmu_lock);return CAM_FAIL(ENOMEM);}
    m->magic=CAM_SMMU_MAGIC;m->idr0=idr0;m->idr1=idr1;m->idr5=idr5;
    m->native_previous=previous;
    m->ste_shift=8;m->ste_bytes=CAM_S1_PAGE;
    m->cmd_shift=cmd>8?8:cmd;m->evt_shift=evt>7?7:evt;
    cam_smmu_namespace=m;
    e=x9_camera_s1_create(idr5,iova,bytes,&m->map);
    if(!e && previous && previous->exported && previous->ste.va && previous->cmd.va && previous->evt.va &&
       previous->ste_bytes==CAM_S1_PAGE && previous->cmd_shift==m->cmd_shift &&
       previous->evt_shift==m->evt_shift) {
        m->ste=previous->ste;m->cmd=previous->cmd;m->evt=previous->evt;
        m->native_queues_adopted=true;
        if(live) {
            /* The enabled bank is fetching this exact table and these queues.
             * Keep every STE (bypass for others, STE80 abort) and both empty
             * rings untouched; export only rewrites STE80, then CFGI. */
            m->native=true;m->native_bypass=previous->native_bypass;m->native_live=true;
            m->native_ste_phase=3;m->native_policy=previous->native_policy; /* storage() needs it */
        } else {
            /* Controller is CR0=ACK=0 (retired); nothing fetches these pages. */
            bzero((void *)m->ste.va,CAM_S1_PAGE);bzero((void *)m->cmd.va,CAM_S1_PAGE);
            bzero((void *)m->evt.va,CAM_S1_PAGE);
        }
        printf("X9 CAMERA SMMU TABLES REUSED: strtab=%llx cmdq=%llx evtq=%llx live=%u (Android probe-time tables)\n",
            m->ste.pa,m->cmd.pa,m->evt.pa,live);
    } else if(live) {
        printf("X9 CAMERA SMMU TABLES NOT REUSED (live bank needs them): exported=%u ste_bytes=%u shifts=%u/%u\n",
            previous->exported,previous->ste_bytes,previous->cmd_shift,previous->evt_shift);
        if(!e)e=CAM_FAIL(EBUSY);
    } else {
        if(previous)printf("X9 CAMERA SMMU TABLES NOT REUSED: exported=%u ste_bytes=%u shifts=%u/%u\n",
            previous->exported,previous->ste_bytes,previous->cmd_shift,previous->evt_shift);
        if(!e)e=cam_s1_alloc_page(&m->ste,cam_s1_oas[idr5&7u]);
        if(!e)e=cam_s1_alloc_page(&m->cmd,cam_s1_oas[idr5&7u]);
        if(!e)e=cam_s1_alloc_page(&m->evt,cam_s1_oas[idr5&7u]);
    }
    if(!e && !live) {
        uint64_t *ste=(uint64_t *)m->ste.va;
        for(unsigned sid=0;sid<CAM_SMMU_STES;sid++)ste[sid*CAM_SMMU_STE_WORDS]=1;
        ste[CAM_SMMU_DISPLAY*CAM_SMMU_STE_WORDS]=1ull|(4ull<<1);
        if(idr1&(1u<<27))ste[CAM_SMMU_DISPLAY*CAM_SMMU_STE_WORDS+1]=1ull<<44;
    }
    if(!e) {
        m->mmio=ml_io_map(0x30800000,0x20000); /* mapping, not an MMIO read */
        if(!m->mmio)e=CAM_FAIL(ENOMEM);
    }
    if(!e)e=cam_smmu_storage(m);
    if(e)cam_smmu_unpublished_free(m);
    else *out=m;
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

/* Ordinary allocation still cannot replace any namespace. Only the native
 * fresh-MM/cold-bank reopen path supplies an independently recycled prior. */
int x9_camera_smmu_prepare(uint32_t idr0,uint32_t idr1,uint32_t idr5,
    uint64_t iova,uint32_t bytes,struct x9_camera_smmu **out)
{ return cam_smmu_prepare_replacement(idr0,idr1,idr5,iova,bytes,NULL,false,out); }

int x9_camera_smmu_export(struct x9_camera_smmu *m,struct x9_camera_smmu_export *out)
{
    int e;
    if(!out)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e && m->exported)e=CAM_FAIL(EBUSY);
    if(!e)e=cam_smmu_storage(m);
    if(!e) {
        uint64_t *ste=(uint64_t *)m->ste.va;
        /* Pin queues/STE before Stage-1 export, including its failure. */
        m->exported=true;
        /* Live bank: STE80 was proven abort (phase3) above. Word1 is written
         * while Config=abort ignores it, then word0 switches to S1; the
         * caller issues CFGI_STE/CD + TLBI + SYNC (arm_smmu_write_ste order). */
        if(m->native_live)m->native_ste_phase=0;
        e=x9_camera_s1_export(m->map,CAM_SMMU_ASID,&m->view.map);
        if(!e) {
            ste[CAM_SMMU_SID*CAM_SMMU_STE_WORDS+1]=cam_smmu_s1_ste_word1(m);
            __asm__ volatile("dsb sy":::"memory");
            ste[CAM_SMMU_SID*CAM_SMMU_STE_WORDS]=1ull|(5ull<<1)|m->view.map.cd_pa;
            m->view.strtab_base=m->ste.pa;m->view.strtab_cfg=m->ste_shift;
            m->view.cmdq_base=m->cmd.pa|m->cmd_shift;
            m->view.evtq_base=m->evt.pa|m->evt_shift;
            m->view.cr1=CAM_SMMU_CR1;m->view.cr2=CAM_SMMU_CR2;
            m->view.cmdq_shift=m->cmd_shift;m->view.evtq_shift=m->evt_shift;
            m->view.asid=CAM_SMMU_ASID;
            e=cam_smmu_storage(m);
        }
        if(!e) {
            e=cam_cache_range(m->ste.va,m->ste_bytes,CAM_CACHE_CLEAN);
            if(!e)e=cam_cache_range(m->cmd.va,CAM_S1_PAGE,CAM_CACHE_DEVICE);
            if(!e)e=cam_cache_range(m->evt.va,CAM_S1_PAGE,CAM_CACHE_DEVICE);
            if(!e)e=cam_smmu_storage(m);
        }
        if(!e)*out=m->view;
        else cam_smmu_latch(m,e);
    }
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

int x9_camera_smmu_discard(struct x9_camera_smmu **map)
{
    int e;
    if(!map)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_smmu_lock);
    e=0;
    if(!*map || *map!=cam_smmu_namespace || (*map)->magic!=CAM_SMMU_MAGIC)e=CAM_FAIL(EINVAL);
    else if((*map)->exported)e=CAM_FAIL(EBUSY);
    if(!e){cam_smmu_unpublished_free(*map);*map=NULL;}
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

/* Consume only this native host's exported EVTQ while camera_smmu_lock is
 * held. The producer publishes the 32-byte entries before advancing PROD;
 * the allocation is device-owned, so invalidate before reading, never clean
 * stale CPU lines over it. Android queue_remove_raw advances CONS only after
 * copying the four words. This synchronous path is also usable by the future
 * IRQ thread. Fault reporting is not yet the Android device-fault callback:
 * return EIO after reporting so the active caller cannot claim success. */
static int cam_smmu_evtq_service_locked(struct x9_camera_smmu *m)
{
    const uint32_t ring=1u<<m->evt_shift, mask=2u*ring-1u, ovf=1u<<31;
    uint32_t prod,cons=cam_smmu_rr(m,0x100ac);
    unsigned count=0,rounds=0;int e;
    if((cons&~(mask|ovf)) || cons!=m->evt_owned_cons)return CAM_FAIL(ESTALE);
    for(;;) {
        uint32_t next;
        prod=cam_smmu_rr(m,0x100a8);
        if((prod&~(mask|ovf)) || (((prod&mask)-(cons&mask))&mask)>ring)return CAM_FAIL(ESTALE);
        if((prod^m->evt_last_prod)&ovf) {
            m->evt_overflows++;
            printf("X9 CAMERA SMMU EVTQ: overflow prod=%x previous=%x cons=%x lost=unknown\n",
                prod,m->evt_last_prod,cons);
        }
        m->evt_last_prod=prod;
        while((cons&mask)!=(prod&mask)) {
            const volatile uint64_t *entry;
            uint64_t w0,w1,w2,w3;
            /* A permanent fault storm must not hold the global SMMU mutex
             * forever; any remaining entries stay pending, not fabricated. */
            if(count++>=4u*ring) {
                m->evt_prod=prod;m->evt_cons=cons;
                printf("X9 CAMERA SMMU EVTQ: drain bound prod=%x cons=%x count=%u\n",prod,cons,count-1);
                return CAM_FAIL(EOVERFLOW);
            }
            e=cam_cache_range(m->evt.va,CAM_S1_PAGE,CAM_CACHE_CPU);if(e)return e;
            entry=(const volatile uint64_t *)m->evt.va+(cons&(ring-1u))*4u;
            w0=entry[0];w1=entry[1];w2=entry[2];w3=entry[3];
            __asm__ volatile("dsb sy":::"memory");
            m->evt_events++;
            if(count<=16)
                printf("X9 CAMERA SMMU EVTQ: event=%llu id=%x sid=%x ssid_valid=%u "
                    "ssid=%x stall=%u read=%u words=%llx/%llx/%llx/%llx\n",
                    m->evt_events,(uint32_t)(w0&0xffu),(uint32_t)(w0>>32),
                    (unsigned)((w0>>11)&1u),(uint32_t)((w0>>12)&0xfffffu),
                    (unsigned)((w1>>31)&1u),(unsigned)((w1>>35)&1u),w0,w1,w2,w3);
            /* queue_sync_cons_out: CPU reads complete before the CONS store. */
            cons=(cons&ovf)|(((cons&mask)+1u)&mask);
            __asm__ volatile("dsb sy":::"memory");
            cam_smmu_rw(m,0x100ac,cons);m->evt_owned_cons=cons;
        }
        /* Android re-reads PROD after draining, including the overflow bit.
         * Do not overlook arrivals while the first snapshot was processed. */
        next=cam_smmu_rr(m,0x100a8);
        if(next==prod)break;
        if(++rounds>=4u*ring) {
            m->evt_prod=prod;m->evt_cons=cons;
            printf("X9 CAMERA SMMU EVTQ: producer changes exceed bound prod=%x cons=%x rounds=%u\n",prod,cons,rounds);
            return CAM_FAIL(EOVERFLOW);
        }
    }
    if((prod^cons)&ovf) {
        cons=(cons&mask)|(prod&ovf);
        cam_smmu_rw(m,0x100ac,cons);m->evt_owned_cons=cons;
    }
    if(count>16)printf("X9 CAMERA SMMU EVTQ: consumed=%u additional_logs_suppressed=%u\n",count,count-16);
    m->evt_prod=prod;m->evt_cons=cons;
    /* Android evtq thread reports and continues; faults never stop the SMMU. */
    return 0;
}

/* Android arm_smmu_gerror_handler's normal-bank handling, serialized with
 * the sole CMDQ producer. An error is always returned to the original caller;
 * skipping an illegal command must not turn its failed invalidation into a
 * successful SYNC receipt. The retained backing remains terminal on error. */
static int cam_smmu_gerror_service_locked(struct x9_camera_smmu *m)
{
    uint32_t g=cam_smmu_rr(m,0x60),gn=cam_smmu_rr(m,0x64),active=g^gn;
    const uint32_t known=0x1fdu;
    if(!active)return 0;
    printf("X9 CAMERA SMMU GERROR: raw=%x ack=%x active=%x cmd_cons=%x pending=%u\n",
        g,gn,active,cam_smmu_rr(m,0x9c),m->pending);
    if(active&~known)printf("X9 CAMERA SMMU GERROR ADOPT: unknown=%x ignored (Android masks GERROR_ERR_MASK)\n",
        active&~known);
    active&=known;
    if(!active)return 0;
    if(active&(1u<<8)) {
        /* Android disables the device on Service Failure Mode before ACK. */
        uint64_t span,deadline;
        m->native_cr0=0;cam_smmu_rw(m,0x20,0);
        nanoseconds_to_absolutetime(1000000000ull,&span);
        deadline=mach_absolute_time()+span;
        for(unsigned n=0;n<1000000;n++) {
            int e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);
            if(e)return e;
            if(!cam_smmu_rr(m,0x24))break;
            if(mach_absolute_time()>=deadline || n==999999)return CAM_FAIL(ETIMEDOUT);
            delay(1);
        }
    }
    if(active&1u) {
        uint32_t cons=cam_smmu_rr(m,0x9c),code=(cons>>24)&0x7fu;
        uint32_t ring=1u<<m->cmd_shift,mask=2u*ring-1u,position=cons&mask;
        printf("X9 CAMERA SMMU CMDQ ERROR: cons=%x code=%u batch=%x/%u prod=%x\n",
            cons,code,m->batch_start,m->batch_count,m->prod);
        /* Android retries fetch abort and ATC timeout without replacement;
         * illegal/unknown commands are changed to CMD_SYNC in place. */
        if(code && code!=2 && code!=3) {
            unsigned i;struct cam_smmu_cmd *slot;int e;
            if(!m->pending || !m->batch_count)return CAM_FAIL(ESTALE);
            for(i=0;i<m->batch_count;i++)
                if(((m->batch_start+i)&mask)==position)break;
            if(i==m->batch_count)return CAM_FAIL(ESTALE);
            slot=(struct cam_smmu_cmd *)m->cmd.va+(position&(ring-1u));
            if(slot->word[0]!=m->batch[i].word[0] || slot->word[1]!=m->batch[i].word[1])return CAM_FAIL(ESTALE);
            printf("X9 CAMERA SMMU CMDQ ERROR: replacing command=%llx/%llx with SYNC\n",
                slot->word[0],slot->word[1]);
            *slot=(struct cam_smmu_cmd){{CAM_SMMU_CMD_SYNC,0}};
            m->batch[i]=*slot;
            e=cam_cache_range((vm_offset_t)slot,sizeof(*slot),CAM_CACHE_CLEAN);
            if(e)return e;
        }
    }
    /* Snapshot write is the architected toggle ACK. A newer GERROR value is
     * not acknowledged by this older sample. */
    cam_smmu_rw(m,0x64,g);m->gerror=g;m->gerrorn=g;
    /* arm_smmu_gerror_handler acks and continues; only SFM is fatal. */
    return (active&(1u<<8))?CAM_FAIL(EIO):0;
}

/* Android mtk_smmu_irq_handler: IRQ_STA covers the shared MM SMMU,
 * not just camera SIDs. ACK each observed source with its sampled count;
 * never clear unrecognized bits or translation-fault monitor evidence. */
static int cam_smmu_wp_irq_service_locked(struct x9_camera_smmu *m)
{
    volatile uint32_t *wp=(volatile uint32_t *)m->native_wp;
    uint32_t sta=wp[0x80/4], unknown=sta&~0xfffffu;
    static const uint8_t order[]={0,1,2,3,4,5,6,7,8,12,16,9,13,17,10,14,18,11,15,19};
    for(unsigned i=0;i<sizeof(order);i++) {
        unsigned bit=order[i];uint32_t count,ack;
        if(!(sta&(1u<<bit)))continue;
        count=wp[(0x100+4*bit)/4];ack=wp[0x88/4];
        wp[0x88/4]=(ack&~0xffu)|(count&0xffu);
        __asm__ volatile("dsb sy":::"memory");
        wp[0x84/4]=1u<<bit;
        __asm__ volatile("dsb sy":::"memory");
        printf("X9 CAMERA SMMU WRAPPER IRQ: sta=%x bit=%u count=%u\n",sta,bit,count);
    }
    if(unknown)printf("X9 CAMERA SMMU WRAPPER IRQ: unknown=%x retained sta=%x\n",unknown,sta);
    return 0; /* mtk_smmu_irq_handler logs, never stops the SMMU */
}

/* The powered, authoritative native controller must own this namespace.
 * These checks prevent this engine from writing to an inherited CMDQ or
 * accepting base redirection by an active HYP interceptor. They cannot
 * establish firmware authority by themselves, and never set a lease bit.
 */
static int cam_smmu_live_impl(struct x9_camera_smmu *m, bool irq_observed)
{
    uint32_t mask=(1u<<(m->cmd_shift+1))-1u;
    uint32_t evt_mask=(1u<<(m->evt_shift+1))-1u;
    int first_error=0;
    if(!m->exported)return CAM_FAIL(ENXIO);
    if(m->native) {
        int e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);
        if(!e)e=cam_native_wp_guard(m);
        if(e)return e;
    }
    if(cam_smmu_rr(m,0)!=m->idr0 || cam_smmu_rr(m,4)!=m->idr1 ||
        cam_smmu_rr(m,0x14)!=m->idr5 ||
        cam_smmu_rr64(m,0x80)!=m->view.strtab_base || cam_smmu_rr(m,0x88)!=m->view.strtab_cfg ||
        cam_smmu_rr64(m,0x90)!=m->view.cmdq_base ||
        cam_smmu_rr64(m,0xa0)!=(m->native && !m->native_evtq_programmed ?
            m->native_prior_base[2] : m->view.evtq_base) ||
        cam_smmu_rr(m,0x20)!=(m->native?m->native_cr0:CAM_SMMU_CR0) ||
        cam_smmu_rr(m,0x24)!=(m->native?m->native_cr0:CAM_SMMU_CR0) ||
        cam_smmu_rr(m,0x28)!=CAM_SMMU_CR1 || cam_smmu_rr(m,0x2c)!=CAM_SMMU_CR2 ||
        (m->native?cam_smmu_rr(m,0x44)!=m->native_gbpa:
          (cam_smmu_rr(m,0x44)&((1u<<31)|(1u<<20)))!=(1u<<20)) ||
        cam_smmu_rr(m,0x50)!=(m->native?m->native_irq_ctrl:0) ||
        cam_smmu_rr(m,0x54)!=(m->native?m->native_irq_ctrl:0))return CAM_FAIL(ESTALE);
    m->last_prod=cam_smmu_rr(m,0x98);m->last_cons=cam_smmu_rr(m,0x9c);
    m->gerror=cam_smmu_rr(m,0x60);m->gerrorn=cam_smmu_rr(m,0x64);
    /* Android combined handler services GERROR, wrapper IRQ counts, then
     * the EVTQ thread. Only a real IAR authorizes wrapper ACKs here. */
    if(m->gerror^m->gerrorn) {
        int e;
        if(!m->native)return CAM_FAIL(EIO);
        e=cam_smmu_gerror_service_locked(m);
        if(e && (e!=EIO || m->gerror!=m->gerrorn))return e;
        if(e)first_error=e;
    }
    if(irq_observed && m->native) {
        int e=cam_smmu_wp_irq_service_locked(m);
        if(e && !first_error)first_error=e;
    }
    /* EVTQ page1, NOT CMDQ's page0. */
    m->evt_prod=cam_smmu_rr(m,0x100a8);m->evt_cons=cam_smmu_rr(m,0x100ac);
    __asm__ volatile("dsb sy":::"memory");
    if((m->evt_prod&~(evt_mask|(1u<<31))) ||
        (m->evt_cons&~(evt_mask|(1u<<31))))return first_error?first_error:EIO;
    if(m->native && m->native_evtq_programmed) {
        int e=cam_smmu_evtq_service_locked(m);
        if(e && !first_error)first_error=e;
    } else if(m->evt_cons || m->evt_prod) {
        if(!first_error)first_error=EIO;
    }
    if(first_error)return first_error;
    if(m->last_cons&~mask)return CAM_FAIL(EIO);
    if(m->last_prod!=m->prod)return CAM_FAIL(ESTALE);
    if(!m->pending && m->last_cons!=m->cons)return CAM_FAIL(ESTALE);
    if(m->pending) {
        /* Only our serial batch is in flight. Consumer must move forward
         * inside [last_confirmed_cons, submitted_prod], including wrap.
         */
        uint32_t remaining=(m->prod-m->wait_cons)&mask;
        uint32_t advanced=(m->last_cons-m->wait_cons)&mask;
        if(advanced>remaining)return CAM_FAIL(ESTALE);
        m->wait_cons=m->last_cons;
    }
    return 0;
}

static int cam_smmu_live(struct x9_camera_smmu *m)
{
    return cam_smmu_live_impl(m,false);
}

int x9_camera_smmu_check(struct x9_camera_smmu *m)
{
    int e;
    static uint64_t chk_n,chk_ns,chk_max,chk_t0;
    uint64_t t0=mach_absolute_time(),ns;
    lck_mtx_lock(&camera_smmu_lock);
    e=cam_smmu_identity(m);
    if(!e)e=cam_smmu_storage(m);
    if(!e)e=m->shared_attempted?cam_smmu_shared_check_locked(m):cam_smmu_live(m);
    if(e && m==cam_smmu_namespace && m && m->exported)cam_smmu_latch(m,e);
    absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);
    if(!chk_t0)chk_t0=t0;
    chk_n++;chk_ns+=ns;if(ns>chk_max)chk_max=ns;
    if(!(chk_n&511)) {
        uint64_t span;absolutetime_to_nanoseconds(mach_absolute_time()-chk_t0,&span);
        printf("X9 CAMERA SMMU CHECK STATS: n=512 avg_us=%llu max_us=%llu per_s=%llu busy_permille=%llu\n",
            chk_ns/512/1000,chk_max/1000,span?512ull*1000000000ull/span:0,span?chk_ns*1000/span:0);
        chk_ns=chk_max=0;chk_t0=mach_absolute_time();
    }
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

/* CMDQ is device-read-only. Never accept CONS advancement for a substituted
 * CPU queue record, even if its bases/configuration still match. The actual
 * native owner remains responsible for excluding other firmware producers. */
static int cam_smmu_batch_check(struct x9_camera_smmu *m)
{
    uint32_t ring=1u<<m->cmd_shift,mask=2*ring-1;
    if(!m->batch_count || m->batch_count>3 || m->batch_count>ring ||
        (m->batch_start&~mask))return CAM_FAIL(EINVAL);
    if(m->pending && (m->cons!=m->batch_start ||
        m->prod!=((m->batch_start+m->batch_count)&mask)))return CAM_FAIL(ESTALE);
    if(m->batch[m->batch_count-1].word[0]!=CAM_SMMU_CMD_SYNC ||
        m->batch[m->batch_count-1].word[1])return CAM_FAIL(ESTALE);
    for(unsigned i=0;i<m->batch_count;i++) {
        unsigned slot=(m->batch_start+i)&(ring-1);
        const volatile uint64_t *entry=(const volatile uint64_t *)m->cmd.va+slot*2;
        if(entry[0]!=m->batch[i].word[0] || entry[1]!=m->batch[i].word[1])return CAM_FAIL(ESTALE);
    }
    __asm__ volatile("dsb sy":::"memory");return 0;
}

/* Sole timeout-close observation, inside the same owner-pinned call/lock.
 * A late SYNC may actually consume our submitted batch, but cannot turn the
 * original expired request into success or make this controller reusable.
 * No second PROD store, CONS/GERRORN/EVTQ ack, cancel, reset or new command.
 */
static void cam_smmu_retire_timeout(struct x9_camera_smmu *m)
{
    uint64_t span,deadline;
    int e=CAM_FAIL(ETIMEDOUT);
    if(!m->pending || m->last_errno!=ETIMEDOUT || m->close_attempted)return;
    m->close_attempted=true;
    nanoseconds_to_absolutetime(1000000000ull,&span);
    deadline=mach_absolute_time()+span;
    e=cam_smmu_storage(m);
    for(unsigned poll=0;poll<1000000 && !e;poll++) {
        if(mach_absolute_time()>=deadline){e=CAM_FAIL(ETIMEDOUT);break;}
        e=cam_smmu_batch_check(m);if(!e)e=cam_smmu_live(m);
        if(e)break;
        m->close_samples++;
        if(mach_absolute_time()>=deadline){e=CAM_FAIL(ETIMEDOUT);break;}
        if(m->last_cons!=m->prod){delay(1);continue;}
        /* First full target observation is irreversible: live's forward
         * window now has zero remaining; a regression/ABA is a conflict. */
        delay(1);
        e=cam_smmu_storage(m);
        if(!e)e=cam_smmu_batch_check(m);
        if(!e)e=cam_smmu_live(m);
        if(e)break;
        m->close_samples++;
        if(mach_absolute_time()>=deadline){e=CAM_FAIL(ETIMEDOUT);break;}
        if(m->last_cons!=m->prod){e=CAM_FAIL(ESTALE);break;}
        m->cons=m->prod;m->pending=false;m->timedout_retired=true;
        __asm__ volatile("dsb sy":::"memory");break;
    }
    if(!e && !m->timedout_retired)e=CAM_FAIL(ETIMEDOUT);
    m->close_errno=(uint32_t)e;
    printf("X9 CAMERA SMMU CLOSE: original=%u qualify=%u pending=%u retired=%u "
        "samples=%u batch=%x/%u actual=%x/%x target=%x "
        "(observation only; retain backing/power, no DMA drain)\n",
        m->last_errno,m->close_errno,m->pending,m->timedout_retired,m->close_samples,
        m->batch_start,m->batch_count,m->last_prod,m->last_cons,m->prod);
}

/* One batch + one architected SYNC, no restart or blind retry. The common
 * live check may service native EVTQ/GERROR under this same mutex. CS_NONE
 * needs neither MSI nor SEV: advancing CONS beyond SYNC proves completion.
 */
static int cam_smmu_submit(struct x9_camera_smmu *m,const struct cam_smmu_cmd *cmd,
    unsigned count)
{
    uint32_t ring=(1u<<m->cmd_shift),mask=2*ring-1,old=m->prod;
    uint64_t interval,deadline;
    int e;
    if(m->pending || m->prod!=m->cons)return CAM_FAIL(EBUSY);
    if(count+1>ring || count>2)return CAM_FAIL(EINVAL);
    e=cam_smmu_storage(m);if(!e)e=cam_smmu_live(m);
    /* A matching normal-bank snapshot alone is insufficient during cold
     * startup: CMDQ must be acknowledged enabled before publishing PROD.
     * Otherwise a disabled queue can only time out, leaving backing pinned. */
    if(!e && !(cam_smmu_rr(m,0x24)&CAM_SMMU_CR0_CMDQEN))e=CAM_FAIL(ENXIO);
    if(e)return cam_smmu_latch(m,e);
    m->batch_start=old;m->batch_count=count+1;
    for(unsigned i=0;i<=count;i++) {
        unsigned slot=(old+i)&(ring-1);
        struct cam_smmu_cmd *entry=(struct cam_smmu_cmd *)m->cmd.va+slot;
        m->batch[i]=i<count?cmd[i]:(struct cam_smmu_cmd){{CAM_SMMU_CMD_SYNC,0}};
        *entry=m->batch[i];
        e=cam_cache_range(m->cmd.va+(vm_offset_t)slot*16,16,CAM_CACHE_CLEAN);
        if(e)return cam_smmu_latch(m,e);
    }
    __asm__ volatile("dsb sy":::"memory");
    /* Recheck producer/config after CPU cache work but before the only
     * device write. This is still not atomic exclusion of firmware users.
     */
    e=cam_smmu_batch_check(m);if(!e)e=cam_smmu_live(m);
    if(e)return cam_smmu_latch(m,e);
    m->wait_cons=m->cons;m->prod=(old+count+1)&mask;m->pending=true;
    cam_smmu_rw(m,0x98,m->prod);
    nanoseconds_to_absolutetime(1000000000ull,&interval); /* ARM_SMMU_POLL_TIMEOUT_US */
    deadline=mach_absolute_time()+interval;
    for(unsigned poll=0;poll<1000000;poll++) {
        if(mach_absolute_time()>=deadline)break;
        e=cam_smmu_batch_check(m);if(!e)e=cam_smmu_live(m);
        if(e)return cam_smmu_latch(m,e);
        if(mach_absolute_time()>=deadline)break;
        if(m->last_cons==m->prod) {
            m->cons=m->prod;m->pending=false;m->commands+=count;m->syncs++;
            __asm__ volatile("dsb sy":::"memory");return 0;
        }
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
    if(!e && !m->exported)e=CAM_FAIL(ENXIO);
    /* Shared attachment owns its inherited queue through the retained host,
     * never the private own-base command path or an unleased CFGI/TLBI. */
    if(!e && m->shared_attempted)e=CAM_FAIL(ENOTSUP);
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
        if(e==ETIMEDOUT)cam_smmu_retire_timeout(m);
    }
    lck_mtx_unlock(&camera_smmu_lock);return e;
}
int x9_camera_smmu_cfgi(struct x9_camera_smmu *m){return cam_smmu_commands(m,1);}
int x9_camera_smmu_tlbi(struct x9_camera_smmu *m){return cam_smmu_commands(m,2);}
int x9_camera_smmu_sync(struct x9_camera_smmu *m){return cam_smmu_commands(m,0);}
#include "x9_camera_smmu_attach.inc"

#include "x9_camera_native_smmu.inc"
#include "x9_camera_smmu_irq.inc"
