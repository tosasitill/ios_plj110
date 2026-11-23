/* Shared MT6993 receive power provider. Independent of the sensor journal.
 * Sources: live PLJ110 DT; hwccf_api.c::_v1_mm_hwccf_voter_ctrl,
 * clk-mt6993-pd.c (CSI_BS_RX bank1 bit11, no VMM/MMINFRA flags);
 * mminfra-util.c::mminfra_ctrl, arm-smmu-v3-common.h.
 *
 * No CAM_MAIN/CAM_MRAW gates, VMM IRQ backup votes, SMMU SEC_INIT/reset,
 * wrapper writes, CR0/GBPA changes, inherited table walks or queue commands.
 * MM SMMUv3 is shared with display, and mt6993_data_mm enables HYP/SEC.
 * An all-zero NS configuration does NOT establish exclusive ownership.
 * Register mappings persist, but every successful HWCCF power reference
 * is paired, and only our newly added CSI AP bit may be removed.
 */
#include "x9_mm_power.h"
_Static_assert(sizeof(struct x9_camera_resources)==224,"camera resources ABI");
enum { CAM_RES_CSI_BIT=1u<<11, CAM_RES_CSI_SET=0x2070c,
    CAM_RES_CSI_EN=0x20714, CAM_RES_CSI_GLOBAL=0x11330,
    CAM_RES_CSI_STA=0x11334, CAM_RES_CSI_DONE=0x11344,
    CAM_RES_CSI_ACK=0x12904, CAM_RES_CSI_ALL_STA=0x1370c,
    CAM_RES_FSM=0x10000, CAM_RES_MM_INFRA=(1u<<31)|(1u<<29) };
enum { CAM_MM_RAW_EN_TIMEOUT_US=200, CAM_MM_VOTER_STATE_TIMEOUT_US=10000 };
struct cam_receive_power {
    vm_offset_t hwv, mmup, smmu;
    uint64_t power_token;
    bool csi_owned, csi_borrowed, smmu_owned, active;
};
static struct cam_receive_power cam_receive;
static struct x9_camera_resources cam_resources_last={.version=1};
static bool cam_resources_attempted, cam_resources_latched;

/* First take/last drop share MM_2 through the native reference authority.
 * smc_get/put are legacy zero fields; X9 MM POWER records actual HWCCF.
 * The private smmu_owned flag means a retained MM power token, not a
 * secure SMC/semaphore or SMMU namespace grant. */
static uint64_t cam_res_read64(vm_offset_t b,unsigned off)
{ return *(volatile uint64_t *)(b+off); }
static void cam_csi_snapshot(struct cam_receive_power *p,uint32_t v[6])
{
    const unsigned off[]={CAM_RES_CSI_EN,CAM_RES_CSI_GLOBAL,CAM_RES_CSI_STA,
        CAM_RES_CSI_DONE,CAM_RES_CSI_ACK,CAM_RES_CSI_ALL_STA};
    for(unsigned i=0;i<6;i++)v[i]=cam_rr(p->hwv,off[i]);
}
static int cam_mm_vote_idle(struct cam_receive_power *p)
{
    int power=x9_mm_power_validate(p->power_token,X9_MM_POWER_CAMERA);
    if(power)return power;
    /* MM raw voter waits STA==0, not AP's DONE==bit. Dummy read is required
     * by the MT6993 MMUP bus path; never write its GPR or global FSM state. */
    (void)cam_rr(p->mmup,0x1ec);
    int e=cam_wait_bits(p->hwv,CAM_RES_CSI_STA,CAM_RES_CSI_BIT,0,10000);
    if(e)return e;
    for(unsigned i=0;i<10000;i++) {
        if((cam_rr(p->hwv,CAM_RES_FSM)&0xffff)<=1)return 0;
        delay(1);
    }
    return CAM_FAIL(ETIMEDOUT);
}
static bool cam_mm_fsm_stuck(struct cam_receive_power *p)
{
    /* Upstream's error handler pulses shared CG47 on this condition. We
     * report it instead: resetting/retriggering display's FSM is not ours. */
    return (cam_rr(p->hwv,0x1131c) && !cam_rr(p->hwv,0x13704)) ||
           (cam_rr(p->hwv,0x11334) && !cam_rr(p->hwv,0x1370c));
}
struct cam_poll_timeout {
    unsigned offset;
    uint32_t value;
};

/* Match regmap_read_poll_timeout_atomic(): use the final read after the
 * deadline for both status and diagnostics. */
static int cam_poll_deadline(vm_offset_t base,unsigned offset,uint32_t mask,
    uint32_t target,unsigned interval_us,unsigned timeout_us,struct cam_poll_timeout *timeout)
{
    uint64_t deadline;

    clock_interval_to_deadline(timeout_us,NSEC_PER_USEC,&deadline);
    for (;;) {
        if((cam_rr(base,offset)&mask)==target)return 0;
        if(mach_absolute_time()>deadline) {
            uint32_t value=cam_rr(base,offset);
            if((value&mask)==target)return 0;
            *timeout=(struct cam_poll_timeout){.offset=offset,.value=value};
            return CAM_FAIL(ETIMEDOUT);
        }
        delay(interval_us);
    }
}

static int cam_csi_poll(struct cam_receive_power *p,unsigned offset,uint32_t mask,
    uint32_t target,unsigned timeout_us,struct cam_poll_timeout *timeout)
{
    return cam_poll_deadline(p->hwv,offset,mask,target,1,timeout_us,timeout);
}

static int cam_csi_prepare_vote(struct cam_receive_power *p,struct cam_poll_timeout *timeout)
{
    int e=x9_mm_power_validate(p->power_token,X9_MM_POWER_CAMERA);

    if(e)return e;
    (void)cam_rr(p->mmup,0x1ec);
    e=cam_csi_poll(p,CAM_RES_CSI_STA,CAM_RES_CSI_BIT,0,CAM_MM_VOTER_STATE_TIMEOUT_US,timeout);
    if(!e)e=cam_csi_poll(p,CAM_RES_FSM,0xfffe,0,CAM_MM_VOTER_STATE_TIMEOUT_US,timeout);
    return e;
}

static int cam_csi_error(const char *op,const char *step,const struct cam_receive_power *p,
    struct cam_poll_timeout timeout,int error)
{
    printf("X9 CAMERA CSI POWER %s: step=%s owned=%u borrowed=%u "
        "errno=%d timeout_off=%x timeout_raw=%x\n",
        op,step,p->csi_owned,p->csi_borrowed,error,timeout.offset,timeout.value);
    return error;
}

static int cam_csi_power_take(struct cam_receive_power *p,struct x9_camera_resources *r)
{
    struct cam_poll_timeout timeout={0};
    uint32_t before[6];
    uint32_t bit=CAM_RES_CSI_BIT;
    const char *step="pre-vote";
    int e=cam_csi_prepare_vote(p,&timeout);

    if(e)goto fail;
    step="ownership";
    cam_csi_snapshot(p,before);
    for(unsigned i=0;i<6;i++) {
        if(i!=3 && ((before[i]^r->csi_before[i])&bit)) {
            e=CAM_FAIL(EBUSY);
            goto fail;
        }
    }
    if(cam_mm_fsm_stuck(p)) {
        step="fsm-pre-vote";
        e=CAM_FAIL(EBUSY);
        goto fail;
    }
    if(before[0]&bit) {
        if(!(before[1]&bit) || !(before[4]&bit)) {
            e=CAM_FAIL(EBUSY);
            goto fail;
        }
        p->csi_borrowed=true;
    } else {
        /* Unvoted-but-powered is unknown direct/firmware ownership. Other
         * XPUs' voted power may be pinned by our own additional AP vote. */
        if(!!(before[1]&bit)!=!!(before[4]&bit)) {
            e=CAM_FAIL(EBUSY);
            goto fail;
        }
        p->csi_owned=true; /* include an ambiguous SET in the restore journal */
        cam_rw(p->hwv,CAM_RES_CSI_SET,bit);
        step="ap-en";
        e=cam_csi_poll(p,CAM_RES_CSI_EN,bit,bit,CAM_MM_RAW_EN_TIMEOUT_US,&timeout);
        if(e)goto fail;
        step="global-en";
        e=cam_csi_poll(p,CAM_RES_CSI_GLOBAL,bit,bit,CAM_MM_VOTER_STATE_TIMEOUT_US,&timeout);
        if(e)goto fail;
        step="sta-idle";
        e=cam_csi_poll(p,CAM_RES_CSI_STA,bit,0,CAM_MM_VOTER_STATE_TIMEOUT_US,&timeout);
        if(e)goto fail;
        step="supply-ack";
        /* Native supply guard after the raw HWCCF vote has completed. */
        e=cam_wait_bits(p->hwv,CAM_RES_CSI_ACK,bit,bit,10000);
        if(e)goto fail;
    }
    if(cam_mm_fsm_stuck(p)) {
        step="fsm-post-vote";
        e=CAM_FAIL(EBUSY);
        goto fail;
    }
    r->flags|=X9_CAM_RES_CSI_POWER;
    r->blockers&=~X9_CAM_RES_NEED_CSI_POWER;
    return 0;
fail:
    return cam_csi_error("GET",step,p,timeout,e);
}
static void cam_smmu_snapshot(struct cam_receive_power *p,struct x9_camera_resources *r)
{
    vm_offset_t b=p->smmu;
    /* IDR2/4 are reserved in this architecture; do not read them. */
    r->idr[0]=cam_rr(b,0);r->idr[1]=cam_rr(b,4);
    r->idr[3]=cam_rr(b,0xc);r->idr[5]=cam_rr(b,0x14);
    r->cr0=cam_rr(b,0x20);r->cr0ack=cam_rr(b,0x24);
    r->cr1=cam_rr(b,0x28);r->cr2=cam_rr(b,0x2c);r->gbpa=cam_rr(b,0x44);
    r->irq_ctrl=cam_rr(b,0x50);r->irq_ack=cam_rr(b,0x54);
    r->gerror=cam_rr(b,0x60);r->gerrorn=cam_rr(b,0x64);
    r->strtab_base=cam_res_read64(b,0x80);r->strtab_cfg=cam_rr(b,0x88);
    r->cmdq_base=cam_res_read64(b,0x90);
    r->cmdq_prod=cam_rr(b,0x98);r->cmdq_cons=cam_rr(b,0x9c);
    r->evtq_base=cam_res_read64(b,0xa0);
    /* EVTQ prod/cons use architected page1 (+0x10000), unlike CMDQ.
     * MT6993 does not advertise ARM_SMMU_OPT_PAGE0_REGS_ONLY in its DT. */
    r->evtq_prod=cam_rr(b,0x100a8);r->evtq_cons=cam_rr(b,0x100ac);
    __asm__ volatile("dsb sy":::"memory");
}
static bool cam_smmu_config_same(const struct x9_camera_resources *a,
                                  const struct x9_camera_resources *b)
{
    return !memcmp(a->idr,b->idr,sizeof(a->idr)) && a->cr0==b->cr0 &&
        a->cr0ack==b->cr0ack && a->cr1==b->cr1 && a->cr2==b->cr2 &&
        a->gbpa==b->gbpa && a->irq_ctrl==b->irq_ctrl && a->irq_ack==b->irq_ack &&
        a->strtab_base==b->strtab_base && a->strtab_cfg==b->strtab_cfg &&
        a->cmdq_base==b->cmdq_base && a->evtq_base==b->evtq_base;
}
static int cam_smmu_observe(struct cam_receive_power *p,struct x9_camera_resources *r)
{
    struct x9_camera_resources again={0};
    int power=x9_mm_power_validate(p->power_token,X9_MM_POWER_CAMERA);
    if(power)return power;
    cam_smmu_snapshot(p,r);cam_smmu_snapshot(p,&again);
    r->flags|=X9_CAM_RES_SMMU_SNAPSHOT;
    if(!cam_smmu_config_same(r,&again) || r->cr0!=r->cr0ack || r->irq_ctrl!=r->irq_ack) {
        r->flags|=X9_CAM_RES_SMMU_CHANGED;return CAM_FAIL(EBUSY);
    }
    /* Capability prerequisite for a future 16KiB stage1 SID80/SSID domain.
     * No page table is allocated or attached by this power-only provider. */
    /* Use the actual native IAS34/16K/SSID0 engine's prerequisites. A
     * hardware SSIDSIZE of zero still permits the default SSID0 CD. This
     * is capability screening only; it allocates/attaches nothing here. */
    if(!cam_smmu_capable(r->idr[0],r->idr[1],r->idr[5])) {
        r->flags|=X9_CAM_RES_SMMU_UNSUPPORTED;
        r->blockers|=X9_CAM_RES_NEED_SMMU_FEATURE;return CAM_FAIL(ENOTSUP);
    }
    if(!r->cr0 && !r->strtab_base && !r->strtab_cfg && !r->cmdq_base &&
       !r->evtq_base && !r->irq_ctrl && (r->gbpa&(1u<<20)) &&
       !(r->gbpa&(1u<<31)))r->flags|=X9_CAM_RES_SMMU_QUIESCENT;
    else r->flags|=X9_CAM_RES_SMMU_INHERITED;
    /* GERRORN is an acknowledgement, NOT a mask. Reading doesn't clear
     * errors, and a pending global error is not silently repaired here. */
    return (r->gerror^r->gerrorn)?CAM_FAIL(EIO):0;
}
static int cam_receive_power_restore(struct cam_receive_power *p,struct x9_camera_resources *r)
{
    /* Keep the MM_2 power reference through the final MMUP dummy read
     * and CSI unvote. On failure retain dependencies and the journal. */
    if(p->csi_owned || p->csi_borrowed) {
        struct cam_poll_timeout timeout={0};
        uint32_t bit=CAM_RES_CSI_BIT;
        int e=cam_csi_prepare_vote(p,&timeout);

        if(e)return cam_csi_error("PUT","pre-vote",p,timeout,e);
        if(cam_mm_fsm_stuck(p))return cam_csi_error("PUT","fsm-pre-vote",p,timeout,EBUSY);
        if(p->csi_owned) {
            cam_rw(p->hwv,CAM_RES_CSI_SET+4,bit);
            e=cam_csi_poll(p,CAM_RES_CSI_EN,bit,0,CAM_MM_RAW_EN_TIMEOUT_US,&timeout);
            if(e)return cam_csi_error("PUT","ap-en",p,timeout,e);
            delay(100); /* upstream v1 MM MTCMOS delay after EN clears */
            e=cam_csi_poll(p,CAM_RES_CSI_STA,bit,0,CAM_MM_VOTER_STATE_TIMEOUT_US,&timeout);
            if(e)return cam_csi_error("PUT","sta-idle",p,timeout,e);
            if(!(cam_rr(p->hwv,CAM_RES_CSI_GLOBAL)&bit)) {
                e=cam_wait_bits(p->hwv,CAM_RES_CSI_ACK,bit,0,10000);
                if(e)return cam_csi_error("PUT","supply-ack",p,timeout,e);
            }
            p->csi_owned=false;
        } else if(!(cam_rr(p->hwv,CAM_RES_CSI_EN)&bit) || !(cam_rr(p->hwv,CAM_RES_CSI_ACK)&bit))
            return cam_csi_error("PUT","borrowed-vote",p,timeout,ESTALE);
        p->csi_borrowed=false;
    }
    if(p->smmu_owned) {
        int e=x9_mm_power_release(&p->power_token,X9_MM_POWER_CAMERA,&r->smc_put);
        if(e)return e;
        p->smmu_owned=false;
    }
    p->active=false;return 0;
}
static int cam_receive_power_take(struct cam_receive_power *p,struct x9_camera_resources *r)
{
    if(p->active || p->smmu_owned || p->power_token || p->csi_owned || p->csi_borrowed)return CAM_FAIL(EBUSY);
    r->stage=X9_CAM_RES_STAGE_MAP;
    if(!p->hwv)p->hwv=ml_io_map(0x31c00000,0x22000);
    if(!p->mmup)p->mmup=ml_io_map(0x31a04000,0x1000);
    if(!p->smmu)p->smmu=ml_io_map(0x30800000,0x20000);
    if(!p->hwv || !p->mmup || !p->smmu)return CAM_FAIL(ENOMEM);
    r->hwccf_before=cam_rr(p->hwv,CAM_RES_FSM);
    cam_csi_snapshot(p,r->csi_before);
    r->mm_ack_before=cam_rr(p->hwv,0x12900);
    r->mm_ack_required=CAM_RES_MM_INFRA;
    r->stage=X9_CAM_RES_STAGE_SMMU_POWER;
    int power=x9_mm_power_acquire(X9_MM_POWER_CAMERA,&p->power_token,&r->smc_get);
    /* An ambiguous issued SET returns its faulted token on error too. */
    p->smmu_owned=p->power_token!=0;
    if(power)return power;
    r->flags|=X9_CAM_RES_SMMU_POWER;
    r->blockers&=~X9_CAM_RES_NEED_SMMU_POWER;
    r->stage=X9_CAM_RES_STAGE_MM_INFRA;
    r->mm_ack_active=cam_rr(p->hwv,0x12900);
    power=x9_mm_power_validate(p->power_token,X9_MM_POWER_CAMERA);
    if(power)return power;
    /* MM_2 keeps its real AO/infra0 parent chain awake. Do not invent VMM
     * backup/IRQ votes or initialize their firmware handlers here. */
    if((r->mm_ack_active&CAM_RES_MM_INFRA)!=CAM_RES_MM_INFRA ||
       (cam_rr(p->hwv,0x1131c)&CAM_RES_MM_INFRA))return CAM_FAIL(ENXIO);
    r->blockers&=~X9_CAM_RES_NEED_MM_INFRA;
    r->stage=X9_CAM_RES_STAGE_CSI;
    int e=cam_csi_power_take(p,r);if(e)return e;
    r->stage=X9_CAM_RES_STAGE_SMMU_SNAPSHOT;
    e=cam_smmu_observe(p,r);if(e)return e;
    p->active=true;return 0;
}
static int cam_resources_probe(struct x9_camera_resources *r)
{
    if(cam_sensor_session_active) {
        memset(r,0,sizeof(*r));r->version=1;r->last_errno=EBUSY;
        return CAM_FAIL(EBUSY); /* no MMIO/unwind of another session's dependencies */
    }
    memset(r,0,sizeof(*r));r->version=1;
    r->blockers=X9_CAM_RES_NEED_CAM_POWER|X9_CAM_RES_NEED_PHY|X9_CAM_RES_NEED_ROUTE|
        X9_CAM_RES_NEED_DMA_MAP|X9_CAM_RES_NEED_DRAIN|X9_CAM_RES_NEED_SMMU_OWNER|
        X9_CAM_RES_NEED_CSI_POWER|X9_CAM_RES_NEED_SMMU_POWER|X9_CAM_RES_NEED_MM_INFRA;
    if(cam_resources_attempted || cam_resources_latched || cam_probe_latched) {
        if(cam_resources_attempted)*r=cam_resources_last;
        r->last_errno=cam_resources_latched||cam_probe_latched?EIO:EALREADY;
        if(cam_resources_latched || cam_probe_latched)r->flags|=X9_CAM_RES_LATCHED;
        return r->last_errno;
    }
    cam_resources_attempted=true;r->flags=X9_CAM_RES_ATTEMPTED;
    int e=cam_receive_power_take(&cam_receive,r);
    int off=cam_receive_power_restore(&cam_receive,r);
    if(cam_receive.hwv) {
        cam_csi_snapshot(&cam_receive,r->csi_after);
        r->hwccf_after=cam_rr(cam_receive.hwv,CAM_RES_FSM);
        r->mm_ack_after=cam_rr(cam_receive.hwv,0x12900);
    }
    if(!e && off)r->stage=X9_CAM_RES_STAGE_RESTORE;
    r->last_errno=e?e:off;r->restore_errno=off;
    if(!off)r->flags|=X9_CAM_RES_RESTORED;
    if(e || off){cam_resources_latched=true;r->flags|=X9_CAM_RES_LATCHED;}
    else r->stage=X9_CAM_RES_STAGE_DONE;
    cam_resources_last=*r;
    printf("X9 CAMERA RESOURCES: flags=%x stage=%u errno=%d restore=%d blockers=%x "
        "CSI ap=%x/%x ack=%x/%x sta=%x/%x smc=%x/%x SMMU cr0=%x/%x "
        "sidbits=%u ssidbits=%u retained=%u/%u (power only, NOT capture ready)\n",
        r->flags,r->stage,r->last_errno,off,r->blockers,
        r->csi_before[0],r->csi_after[0],r->csi_before[4],r->csi_after[4],
        r->csi_before[2],r->csi_after[2],r->smc_get,r->smc_put,r->cr0,r->cr0ack,
        r->idr[1]&63,(r->idr[1]>>6)&31,cam_receive.csi_owned,cam_receive.smmu_owned);
    return r->last_errno;
}
