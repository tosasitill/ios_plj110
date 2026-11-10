/* MT6993 MM resource lifecycle, under camera_control_lock in thread context.
 * Live DT: mminfra voter 55/BACKUP_GRP0, AO bit0 / infra0 bit1;
 * clk-mt6993-pd: VCORE16 -> MAIN15 -> MRAW8, all bank0;
 * clk-mt6993: CAM is AL (no AP enable), CAMTM mux3 / SENINF0 mux17,
 * SENINF3 mux20. clk-mt6993-cam: CAM CG22, MRAW/CAMSV CG34.
 * hwccf_api.c v1 MM raw/IRQ voters: STA==0, not DONE==1. VMM's last
 * backup10..13 unvote must wait ISP/CAM/VDE PM_ACK off first.
 * clk-gate.c: raw vote, FSM check, physical CG_CLR, actual CG readback.
 *
 * This is a persistent register-access clock lifetime, not capture initialization.
 * No VMM CVFS/AVS/eFuse rewrite, rate/PLL/AL-map change, CG47 pulse, SMI
 * larb enable, PHY/route/DMA write or NS SMMU ownership claim. The secure
 * receive reference is kept through every dummy read and dependent unvote.
 */
_Static_assert(sizeof(struct x9_camera_mm_status)==576,"camera MM ABI");
enum cam_mm_kind { CAM_MM_BACKUP, CAM_MM_DOMAIN, CAM_MM_AL, CAM_MM_MUX, CAM_MM_GATE };
struct cam_mm_desc {
    enum cam_mm_kind kind;
    unsigned set, global, sta, effect, mask, effect_mask;
    unsigned cfg, shift, width, parents;
};
static const struct cam_mm_desc cam_mm_descs[X9_CAM_MM_SLOTS]={
    {CAM_MM_BACKUP,0x20724,0x11358,0x1135c,0x12900,1,1u<<31,0,0,0,0},
    {CAM_MM_BACKUP,0x20724,0x11358,0x1135c,0x12900,2,1u<<29,0,0,0,0},
    {CAM_MM_BACKUP,0x20724,0x11358,0x1135c,0x1135c,0xc00,0xc00,0,0,0,0},
    {CAM_MM_DOMAIN,0x20700,0x11318,0x1131c,0x12900,1u<<16,1u<<16,0,0,0,0},
    {CAM_MM_DOMAIN,0x20700,0x11318,0x1131c,0x12900,1u<<15,1u<<15,0,0,0,0},
    {CAM_MM_DOMAIN,0x20700,0x11318,0x1131c,0x12900,1u<<8,1u<<8,0,0,0,0},
    {CAM_MM_AL,0x20600,0x12330,0x12030,0x270,1u<<2,1u<<18,0x40,8,4,16},
    {CAM_MM_MUX,0x20600,0x12330,0x12030,0x270,1u<<3,1u<<17,0x40,16,3,5},
    {CAM_MM_MUX,0x20600,0x12330,0x12030,0x270,1u<<17,1u<<14,0x50,8,4,16},
    {CAM_MM_GATE,0x20108,0x12258,0x11858,0,(1u<<1)|(1u<<6)|(1u<<7),
        (1u<<1)|(1u<<6)|(1u<<7),0,0,0,0},
    /* Only register-access CAMSV clocks. DMA/SMI larbs remain gated/unowned. */
    {CAM_MM_GATE,0x20198,0x12288,0x11888,0,7u<<12,7u<<12,0,0,0,0}
};
struct cam_mm_journal {
    struct cam_receive_power receive;
    vm_offset_t top, cam_cg, sv_cg;
    struct cam_mm_desc desc[X9_CAM_MM_SLOTS];
    uint32_t owned[X9_CAM_MM_SLOTS], borrowed[X9_CAM_MM_SLOTS];
    uint32_t gate_changed[X9_CAM_MM_SLOTS], gate_before[X9_CAM_MM_SLOTS];
    uint32_t seen, completed, cfg[3];
    bool active;
};
static struct cam_mm_journal cam_mm;
static bool cam_smi_clocks_held(void);
static struct x9_camera_mm_status cam_mm_last={.version=1};
static bool cam_mm_attempted;
static struct {
    uint64_t serial, epoch;
    bool close_attempted;
    int error, close_error;
    struct x9_camera_mm_status status;
    struct x9_camera_resources receive;
} cam_mm_live;

static vm_offset_t cam_mm_effect_base(struct cam_mm_journal *j,unsigned n)
{
    if(j->desc[n].kind==CAM_MM_MUX || j->desc[n].kind==CAM_MM_AL)return j->top;
    if(n==X9_CAM_MM_CAM_GATE)return j->cam_cg;
    if(n==X9_CAM_MM_SV_GATE)return j->sv_cg;
    return j->receive.hwv;
}
static void cam_mm_snapshot(struct cam_mm_journal *j,unsigned n,uint32_t v[3])
{
    const struct cam_mm_desc *d=&j->desc[n];vm_offset_t b=j->receive.hwv;
    v[0]=cam_rr(b,d->set+8);v[1]=cam_rr(b,d->global);
    v[2]=cam_rr(cam_mm_effect_base(j,n),d->effect);
}
static int cam_mm_idle(struct cam_mm_journal *j,unsigned n,bool on)
{
    const struct cam_mm_desc *d=&j->desc[n];
    int e;
    /* receive_take proved AO/infra0; retain its secure reference until done. */
    if(!j->receive.smmu_owned)return ENXIO;
    /* Check the parent BEFORE the MMUP dummy read. This also reaches the
     * shared power authority's validate through cam_mm_vote_idle. A cached
     * successful GET never permits an MMUP read below a lost infra ACK. */
    if((cam_rr(j->receive.hwv,0x12900)&CAM_RES_MM_INFRA)!=CAM_RES_MM_INFRA ||
       (cam_rr(j->receive.hwv,0x1131c)&CAM_RES_MM_INFRA))return ENXIO;
    e=cam_mm_vote_idle(&j->receive);if(e)return e;
    e=cam_wait_bits(j->receive.hwv,d->sta,d->mask,0,10000);if(e)return e;
    if(d->kind!=CAM_MM_BACKUP && (on || d->kind==CAM_MM_DOMAIN)) {
        for(unsigned i=0;i<10000;i++) {
            if((cam_rr(j->receive.hwv,CAM_RES_FSM)&0xffff)<=1)goto ready;
            delay(1);
        }
        return ETIMEDOUT;
    }
ready:
    return cam_mm_fsm_stuck(&j->receive)?EBUSY:0;
}
static int cam_mm_cfg_check(struct cam_mm_journal *j,unsigned n)
{
    const struct cam_mm_desc *d=&j->desc[n];
    if(d->kind!=CAM_MM_MUX && d->kind!=CAM_MM_AL)return 0;
    unsigned k=n-X9_CAM_MM_CAM_MUX;
    uint32_t cfg=(cam_rr(j->top,d->cfg)>>d->shift)&0x7f;
    /* The provider never changes a parent, PDN, PLL or shared AL table.
     * Configuration must remain stable, valid, with CKSTA idle. FENC is
     * checked after vote (and CAM only after its parent-domain AL vote). */
    if(cfg!=j->cfg[k] || (cfg&((1u<<d->width)-1))>=d->parents)return ESTALE;
    return cam_wait_bits(j->top,0x330,d->effect_mask,0,10000);
}
static int cam_mm_effect_on(struct cam_mm_journal *j,unsigned n)
{
    const struct cam_mm_desc *d=&j->desc[n];
    uint32_t want=(d->kind==CAM_MM_GATE || n==X9_CAM_MM_VMM)?0:d->effect_mask;
    return cam_wait_bits(cam_mm_effect_base(j,n),d->effect,d->effect_mask,want,10000);
}
/* Whole consumed dependency check, before receiver MMIO AND before the
 * first unvote. Checks are not an atomic lease/ABA detector. An incomplete
 * take may have an ambiguous final SET; only completed slots require the
 * consumed effect, but inherited references must still be present. */
static int cam_mm_guard(struct cam_mm_journal *j,bool complete)
{
    int e;
    if(complete && (!j->active || j->completed!=((1u<<X9_CAM_MM_SLOTS)-1)))return ENXIO;
    if(!j->receive.smmu_owned)return j->seen?ENXIO:0;
    if((cam_rr(j->receive.hwv,0x12900)&CAM_RES_MM_INFRA)!=CAM_RES_MM_INFRA ||
       (cam_rr(j->receive.hwv,0x1131c)&CAM_RES_MM_INFRA))return ENXIO;
    e=cam_mm_vote_idle(&j->receive);if(e)return e;
    if(j->receive.active || j->receive.csi_borrowed) {
        if(!(cam_rr(j->receive.hwv,CAM_RES_CSI_EN)&CAM_RES_CSI_BIT) ||
           !(cam_rr(j->receive.hwv,CAM_RES_CSI_GLOBAL)&CAM_RES_CSI_BIT) ||
           !(cam_rr(j->receive.hwv,CAM_RES_CSI_ACK)&CAM_RES_CSI_BIT))return ESTALE;
    }
    for(unsigned n=0;n<X9_CAM_MM_SLOTS;n++)if(j->seen&(1u<<n)) {
        const struct cam_mm_desc *d=&j->desc[n];
        uint32_t live[3];
        e=cam_mm_idle(j,n,true);if(e)return e;
        e=cam_mm_cfg_check(j,n);if(e)return e;
        cam_mm_snapshot(j,n,live);
        if((live[0]&j->borrowed[n])!=j->borrowed[n] ||
           (live[1]&j->borrowed[n])!=j->borrowed[n])return ESTALE;
        if(j->completed&(1u<<n)) {
            if(d->kind!=CAM_MM_AL &&
               ((live[0]&d->mask)!=d->mask || (live[1]&d->mask)!=d->mask))return ESTALE;
            e=cam_mm_effect_on(j,n);if(e)return e;
        }
    }
    return cam_mm_fsm_stuck(&j->receive)?EBUSY:0;
}
static int cam_mm_slot_take(struct cam_mm_journal *j,unsigned n,struct x9_camera_mm_status *r)
{
    const struct cam_mm_desc *d=&j->desc[n];struct x9_camera_mm_sample *s=&r->sample[n];
    vm_offset_t b=j->receive.hwv;uint32_t before[3];
    s->mask=d->mask;s->flags=X9_CAM_MM_SLOT_SEEN;j->seen|=1u<<n;
    cam_mm_snapshot(j,n,s->before);
    int e=cam_mm_idle(j,n,true);if(e)return e;
    cam_mm_snapshot(j,n,before);
    if(((before[0]^s->before[0])&d->mask) || ((before[1]^s->before[1])&d->mask))return EBUSY;
    e=cam_mm_cfg_check(j,n);if(e)return e;
    if(d->kind==CAM_MM_AL) {
        /* clk-mux.c mtk_mux_generic_hwv_al_ops has NO enable/disable.
         * CAM FENC must be supplied by domain/CG autolink, not forged with
         * a generic MUX_PWR AP SET. Our domain votes keep its parent alive. */
        e=cam_mm_effect_on(j,n);if(e)return e;
        s->flags|=X9_CAM_MM_SLOT_BORROWED;
    } else {
        uint32_t ap=before[0]&d->mask;
        if((ap&~before[1])!=0)return EBUSY;
        /* Preserve arbitrary partial inherited AP masks (e.g. VMM10).
         * A borrowed vote without its real effect is NOT ready. */
        j->borrowed[n]=ap;
        if(ap)s->flags|=X9_CAM_MM_SLOT_BORROWED;
        if(d->kind==CAM_MM_GATE) {
            j->gate_before[n]=before[2]&d->mask;
            /* Never repair an existing owner's gated-but-voted clock. */
            if((before[2]&before[1]&d->mask)!=0)return EBUSY;
            /* Unvoted-but-ungated is an unknown direct owner. A later
             * last HWV CLR would gate it and destroy that owner's state. */
            if((~before[2]&~before[1]&d->mask)!=0)return EBUSY;
        } else if(d->kind==CAM_MM_DOMAIN) {
            if(!!(before[1]&d->mask)!=!!(before[2]&d->effect_mask))return EBUSY;
        } else if(n<X9_CAM_MM_VMM) {
            /* Secure PM_GET also holds AO/infra0 independently of BACKUP.
             * BACKUP=0/PM_ACK=1 is expected here, not evidence of exclusivity. */
            if(!(before[2]&d->effect_mask))return ENXIO;
        }
        uint32_t add=d->mask&~ap;
        if(add) {
            j->owned[n]=add;s->flags|=X9_CAM_MM_SLOT_OWNED;
            cam_rw(b,d->set,add); /* ambiguous SET is already journaled */
            e=cam_wait_bits(b,d->set+8,d->mask,d->mask,200);
            if(!e)e=cam_wait_bits(b,d->global,d->mask,d->mask,10000);
            if(!e)e=cam_wait_bits(b,d->sta,d->mask,0,10000);
            if(e)return e;
        }
        if(cam_mm_fsm_stuck(&j->receive))return EBUSY;
        if(d->kind==CAM_MM_GATE) {
            vm_offset_t cg=cam_mm_effect_base(j,n);
            uint32_t gated=cam_rr(cg,0)&d->mask;
            if(gated&~j->gate_before[n])return ESTALE;
            if(gated) {
                j->gate_changed[n]=gated;
                /* Same single-bit CG_CLR as clk-gate.c's normal path,
                 * never a whole-bank RMW, never a reset/retrigger. */
                cam_rw(cg,8,gated);
            }
        }
        e=cam_mm_effect_on(j,n);if(e)return e;
    }
    e=cam_mm_cfg_check(j,n);if(e)return e;
    cam_mm_snapshot(j,n,s->active);s->sta=cam_rr(b,d->sta);
    j->completed|=1u<<n;s->flags|=X9_CAM_MM_SLOT_ACTIVE;return 0;
}
static int cam_mm_vmm_can_drop(struct cam_mm_journal *j,uint32_t drop)
{
    /* Exactly the upstream last-VMM dependency rule; do not lower VMM
     * beneath inherited or newly appearing ISP/CAM/VDE power domains. */
    for(unsigned i=0;i<10000;i++) {
        uint32_t others=cam_rr(j->receive.hwv,0x11358)&0x3c00&~drop;
        if(others)return 0;
        if(!(cam_rr(j->receive.hwv,0x12900)&0x10004) &&
           !(cam_rr(j->receive.hwv,0x12904)&2))return 0;
        delay(1);
    }
    return ETIMEDOUT;
}
static int cam_mm_slot_restore(struct cam_mm_journal *j,unsigned n,struct x9_camera_mm_status *r)
{
    if(!(j->seen&(1u<<n)))return 0;
    const struct cam_mm_desc *d=&j->desc[n];vm_offset_t b=j->receive.hwv;
    struct x9_camera_mm_sample *s=&r->sample[n];
    int e=cam_mm_idle(j,n,false);if(e)return e;
    e=cam_mm_cfg_check(j,n);if(e)return e;
    if(d->kind==CAM_MM_AL) {
        /* Not our AP vote. Its effect may go away only with our domains. */
        if((j->completed&(1u<<n)) && (e=cam_mm_effect_on(j,n)))return e;
    } else {
        if((cam_rr(b,d->set+8)&j->borrowed[n])!=j->borrowed[n])return ESTALE;
        uint32_t drop=j->owned[n];
        if(drop) {
            if(n==X9_CAM_MM_VMM && (e=cam_mm_vmm_can_drop(j,drop)))return e;
            cam_rw(b,d->set+4,drop);
            e=cam_wait_bits(b,d->set+8,drop,0,200);
            if(d->kind==CAM_MM_BACKUP || d->kind==CAM_MM_DOMAIN)delay(100);
            if(!e)e=cam_wait_bits(b,d->sta,d->mask,0,10000);
            if(e)return e;
            /* Preserve journal until actual effect restoration is known.
             * No physical CG_SET: HWV must gate on the last aggregate vote.
             * A new owner prevents restoration -> retain our dependencies,
             * never force its gate off or reset shared hardware. */
            uint32_t global=cam_rr(b,d->global)&d->mask;
            if(d->kind==CAM_MM_GATE) {
                uint32_t off=j->gate_before[n]&~global;
                e=cam_wait_bits(cam_mm_effect_base(j,n),0,off,off,10000);
                /* HWV may already have ungated on SET, with no manual
                 * CG_CLR recorded. Original gated bits still require a
                 * last-owner check in that case: do not drop dependencies
                 * under a new aggregate owner just because gate_changed=0. */
                if(!e && (j->gate_before[n]&global))e=ESTALE;
            } else if(d->kind==CAM_MM_DOMAIN && !global)
                e=cam_wait_bits(b,d->effect,d->effect_mask,0,10000);
            if(e)return e;
            j->owned[n]=0;j->gate_changed[n]=0;
        }
        if(j->borrowed[n] && (j->completed&(1u<<n)) &&
           (e=cam_mm_effect_on(j,n)))return e;
        j->borrowed[n]=0;
    }
    cam_mm_snapshot(j,n,s->after);s->sta=cam_rr(b,d->sta);
    s->flags|=X9_CAM_MM_SLOT_RESTORED;j->seen&=~(1u<<n);return 0;
}
static int cam_mm_restore(struct cam_mm_journal *j,struct x9_camera_mm_status *r,
                           struct x9_camera_resources *receive)
{
    int guard=cam_mm_guard(j,false);
    if(guard) {
        r->retained=j->seen;
        for(unsigned n=0;n<X9_CAM_MM_SLOTS;n++)if(j->seen&(1u<<n))
            r->sample[n].flags|=X9_CAM_MM_SLOT_RETAINED;
        printf("X9 CAMERA MM RESTORE: dependency guard errno=%d pending=%x; no unvote\n",
            guard,j->seen);
        return guard;
    }
    for(unsigned i=X9_CAM_MM_SLOTS;i;i--) {
        unsigned n=i-1;int e=cam_mm_slot_restore(j,n,r);
        if(e) {
            cam_mm_snapshot(j,n,r->sample[n].after);
            r->sample[n].sta=cam_rr(j->receive.hwv,j->desc[n].sta);
            printf("X9 CAMERA MM RESTORE: failed slot=%u errno=%d pending=%x; retaining dependencies\n",
                n,e,j->seen);
            r->retained=j->seen;r->slot=n;
            for(unsigned k=0;k<X9_CAM_MM_SLOTS;k++)if(j->seen&(1u<<k))
                r->sample[k].flags|=X9_CAM_MM_SLOT_RETAINED;
            return e; /* lower domains/VMM/infra/secure power must stay */
        }
    }
    int e=cam_receive_power_restore(&j->receive,receive);
    if(!e){j->active=false;j->completed=0;}return e;
}
static int cam_mm_take(struct cam_mm_journal *j,unsigned sensor,struct x9_camera_mm_status *r,
                        struct x9_camera_resources *receive)
{
    if(j->active || j->seen || j->receive.active || j->receive.smmu_owned ||
       j->receive.csi_owned || j->receive.csi_borrowed)return EBUSY;
    j->completed=0;
    r->stage=X9_CAM_MM_STAGE_RECEIVE;
    int e=cam_receive_power_take(&j->receive,receive);if(e)return e;
    r->blockers&=~X9_CAM_MM_NEED_RECEIVE;r->stage=X9_CAM_MM_STAGE_MAP;
    if(!j->top)j->top=ml_io_map(0x10040000,0x1000);
    if(!j->cam_cg)j->cam_cg=ml_io_map(0x3a010000,0x1000);
    if(!j->sv_cg)j->sv_cg=ml_io_map(0x3a680000,0x1000);
    if(!j->top || !j->cam_cg || !j->sv_cg)return ENOMEM;
    memcpy(j->desc,cam_mm_descs,sizeof(j->desc));
    if(sensor==X9_CAMERA_BACK) {
        struct cam_mm_desc *d=&j->desc[X9_CAM_MM_SENINF_MUX];
        d->mask=1u<<20;d->effect_mask=1u<<11;d->cfg=0x60;d->shift=0;
    }
    for(unsigned k=0;k<3;k++) {
        const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_CAM_MUX+k];
        j->cfg[k]=r->cfg[k]=(cam_rr(j->top,d->cfg)>>d->shift)&0x7f;
        if((j->cfg[k]&((1u<<d->width)-1))>=d->parents)return ENOTSUP;
    }
    r->stage=X9_CAM_MM_STAGE_TAKE;
    for(unsigned n=0;n<X9_CAM_MM_SLOTS;n++) {
        r->slot=n;e=cam_mm_slot_take(j,n,r);
        if(e) {
            cam_mm_snapshot(j,n,r->sample[n].active);
            r->sample[n].sta=cam_rr(j->receive.hwv,j->desc[n].sta);
            return e;
        }
        if(n==X9_CAM_MM_MRAW) {
            r->flags|=X9_CAM_MM_POWER;r->blockers&=~X9_CAM_MM_NEED_POWER;
        }
    }
    /* Recheck the whole dependency chain while it is still held. A valid
     * early PM_ACK is not sufficient if a parent changed later in take. */
    for(unsigned n=0;n<X9_CAM_MM_SLOTS;n++) {
        const struct cam_mm_desc *d=&j->desc[n];r->slot=n;
        e=cam_mm_cfg_check(j,n);if(e)return e;
        e=cam_mm_effect_on(j,n);if(e)return e;
        cam_mm_snapshot(j,n,r->sample[n].active);
        if(d->kind!=CAM_MM_AL &&
           ((r->sample[n].active[0]&d->mask)!=d->mask ||
            (r->sample[n].active[1]&d->mask)!=d->mask))return ESTALE;
    }
    r->flags|=X9_CAM_MM_CLOCKS;r->blockers&=~X9_CAM_MM_NEED_CLOCKS;
    j->active=true;return 0;
}
static void cam_mm_status_init(unsigned sensor,struct x9_camera_mm_status *r)
{
    memset(r,0,sizeof(*r));r->version=1;r->sensor=sensor;
    r->blockers=X9_CAM_MM_NEED_RECEIVE|X9_CAM_MM_NEED_POWER|X9_CAM_MM_NEED_CLOCKS|
        X9_CAM_MM_NEED_RATE|X9_CAM_MM_NEED_SMI|X9_CAM_MM_NEED_DMA_MAP|
        X9_CAM_MM_NEED_PHY|X9_CAM_MM_NEED_ROUTE|X9_CAM_MM_NEED_DRAIN;
}
static void cam_mm_status_retained(struct x9_camera_mm_status *r)
{
    r->retained=cam_mm.seen;
    if(cam_mm.receive.csi_owned || cam_mm.receive.csi_borrowed)r->retained|=X9_CAM_MM_RETAIN_CSI;
    if(cam_mm.receive.smmu_owned)r->retained|=X9_CAM_MM_RETAIN_SECURE;
}
static int cam_mm_session_fault(int e)
{
    if(!cam_mm_live.error)cam_mm_live.error=e;
    cam_resources_latched=true;
    cam_mm_live.status.flags|=X9_CAM_MM_LATCHED;
    cam_mm_live.status.last_errno=cam_mm_live.error;
    cam_mm_status_retained(&cam_mm_live.status);
    cam_mm_last=cam_mm_live.status;
    return e;
}
/* Nonzero epoch, including on failure, owns ONE close attempt. Only these
 * helpers mutate the single journal; successful lifetime retirement permits
 * another sensor, independent of the once-only diagnostic success flag. */
static int cam_mm_session_open(unsigned sensor,uint64_t *epoch,struct x9_camera_mm_status *r)
{
    int e;
    *epoch=0;cam_mm_status_init(sensor,r);
    if(sensor!=X9_CAMERA_BACK && sensor!=X9_CAMERA_FRONT)e=EINVAL;
    else if(st.mode!=2 && st.mode!=3)e=EPERM;
    else if(cam_mm_live.epoch || cam_mm.active || cam_mm.seen || session.capture)e=EBUSY;
    else if(cam_resources_latched || cam_probe_latched)e=EIO;
    else if(cam_mm_live.serial==UINT64_MAX)e=EOVERFLOW;
    else e=0;
    if(e){r->last_errno=e;return e;}
    cam_mm_live.epoch=++cam_mm_live.serial;*epoch=cam_mm_live.epoch;
    cam_mm_live.close_attempted=false;cam_mm_live.error=cam_mm_live.close_error=0;
    memset(&cam_mm_live.receive,0,sizeof(cam_mm_live.receive));cam_mm_live.receive.version=1;
    r->flags=X9_CAM_MM_ATTEMPTED;
    e=cam_mm_take(&cam_mm,sensor,r,&cam_mm_live.receive);
    cam_mm_live.status=*r;
    if(!e)e=cam_mm_guard(&cam_mm,true);
    if(e)cam_mm_session_fault(e);
    else {cam_mm_status_retained(&cam_mm_live.status);cam_mm_last=cam_mm_live.status;}
    *r=cam_mm_live.status;
    return e;
}
static int cam_mm_session_check(uint64_t epoch)
{
    int e;
    if(!epoch || epoch!=cam_mm_live.epoch || cam_mm_live.close_attempted)return ESTALE;
    if(cam_mm_live.error)return cam_mm_live.error;
    e=cam_mm_guard(&cam_mm,true);
    return e?cam_mm_session_fault(e):0;
}
static int cam_mm_session_close(uint64_t epoch,struct x9_camera_mm_status *r)
{
    unsigned stage,slot;
    int off,prior;
    if(!epoch || epoch!=cam_mm_live.epoch || cam_mm_live.close_attempted)return ESTALE;
    if(session.capture)return EBUSY;
    if(cam_smi_clocks_held())return EBUSY;
    cam_mm_live.close_attempted=true;
    prior=cam_mm_live.error;
    stage=cam_mm_live.status.stage;slot=cam_mm_live.status.slot;
    off=cam_mm_restore(&cam_mm,&cam_mm_live.status,&cam_mm_live.receive);
    cam_mm_live.close_error=off;cam_mm_live.status.restore_errno=off;
    if(off)cam_mm_session_fault(off);
    else {
        cam_mm_live.status.flags|=X9_CAM_MM_RESTORED;
        cam_mm_live.epoch=0;
    }
    if(!prior)cam_mm_live.status.stage=off?X9_CAM_MM_STAGE_RESTORE:X9_CAM_MM_STAGE_DONE;
    else {cam_mm_live.status.stage=stage;cam_mm_live.status.slot=slot;}
    cam_mm_live.status.last_errno=cam_mm_live.error;
    cam_mm_status_retained(&cam_mm_live.status);
    *r=cam_mm_live.status;cam_mm_last=*r;
    return off; /* never replace an unwind result with the earlier take error */
}
/* Receiver qualification can fail BEFORE any MM unvote. This is still the
 * lifetime's one failed close attempt: retain both identity and resources,
 * record the actual restore error, and refuse accidental private retries. */
static int cam_mm_session_retain(uint64_t epoch,int e)
{
    int prior=cam_mm_live.error;
    if(!epoch || epoch!=cam_mm_live.epoch || cam_mm_live.close_attempted)return ESTALE;
    cam_mm_live.close_attempted=true;cam_mm_live.close_error=e;
    cam_mm_live.status.restore_errno=e;
    if(!prior)cam_mm_live.status.stage=X9_CAM_MM_STAGE_RESTORE;
    return cam_mm_session_fault(e);
}
static void cam_mm_log(unsigned sensor,const struct x9_camera_mm_status *r)
{
    const struct x9_camera_resources *receive=&cam_mm_live.receive;
    cam_mm_last=*r;
    printf("X9 CAMERA MM: sensor=%u flags=%x stage/slot=%u/%u errno=%u restore=%u "
        "blockers=%x retained=%x CSI/secure=%u/%u smc=%x/%x cfg=%x/%x/%x "
        "(register clocks only, NOT capture ready)\n",sensor,r->flags,r->stage,r->slot,
        r->last_errno,r->restore_errno,r->blockers,r->retained,cam_mm.receive.csi_owned,
        cam_mm.receive.smmu_owned,receive->smc_get,receive->smc_put,r->cfg[0],r->cfg[1],r->cfg[2]);
    for(unsigned n=0;n<X9_CAM_MM_SLOTS;n++)if(r->sample[n].flags) {
        const struct x9_camera_mm_sample *s=&r->sample[n];
        printf("X9 CAMERA MM SLOT: n=%u mask=%x flags=%x AP=%x/%x/%x global=%x/%x/%x "
            "effect=%x/%x/%x sta=%x\n",n,s->mask,s->flags,s->before[0],s->active[0],s->after[0],
            s->before[1],s->active[1],s->after[1],s->before[2],s->active[2],s->after[2],s->sta);
    }
}
static int cam_mm_probe(unsigned sensor,struct x9_camera_mm_status *r)
{
    uint64_t epoch=0;
    int e,off=0;
    if(cam_sensor_session_active || cam_mm_live.epoch) {
        memset(r,0,sizeof(*r));r->version=1;r->sensor=sensor;r->last_errno=EBUSY;
        return EBUSY;
    }
    cam_mm_status_init(sensor,r);
    if(cam_mm_attempted || cam_resources_latched || cam_probe_latched) {
        if(cam_mm_attempted)*r=cam_mm_last;
        r->last_errno=cam_resources_latched||cam_probe_latched?EIO:EALREADY;
        if(cam_resources_latched || cam_probe_latched)r->flags|=X9_CAM_MM_LATCHED;
        return r->last_errno;
    }
    e=cam_mm_session_open(sensor,&epoch,r);
    if(epoch){cam_mm_attempted=true;off=cam_mm_session_close(epoch,r);}
    r->last_errno=e?e:off;
    if(epoch)cam_mm_log(sensor,r);
    return e?e:off;
}
