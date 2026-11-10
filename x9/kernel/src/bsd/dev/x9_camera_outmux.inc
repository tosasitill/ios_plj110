/* MT6993 ISP8s PHY3.0 OUTMUX0. mtk_cam-seninf-hw_phy_3_0::SET_TAG,
 * config_outmux, init_port; mtk_cam-seninf-route::get_vc_info_by_scenario.
 * Physical TOP 3a400000, OUTMUX outer/inner 3a200000/3a208000.
 * Only normal config, unsplit sensor0 of port0/3, one RAW10 image tag.
 * Tag blocks are 0x1c bytes, NOT CAMSV's 0x40 tag stride.
 * No shared resets, all-port CG/IRQ writes, DL enable, read-clear IRQ reads,
 * incoming-frame fabrication, stream-on, DMA programming or IOVA grant.
 * Masked CFG_DONE history is handled only by the WCLR-qualified STATUSX
 * thread owner below; no AP/CCU IRQ enable or GIC capture claim.
 * camera_control_lock holds the journal for every entry/cleanup/poll.
 */
#include "x9_camera_outmux.h"
enum { CAM_OUT_FIXED=16, CAM_OUT_REGS=CAM_OUT_FIXED+8*5, CAM_OUT_WRITES=7 };
static const unsigned cam_out_fixed[CAM_OUT_FIXED]={
    0,4,8,0xc,0x10,0x14,0x18,0x20,0x2c,0x34,0x40,0x48,0xa0,0xa4,0xa8,0x50
};
static const unsigned cam_out_tag_offsets[5]={0,4,8,0xc,0x14};
struct cam_out_field { unsigned reg; uint32_t mask; };
static const struct cam_out_field cam_out_fields[CAM_OUT_WRITES]={
    {2,1}, {3,0x1f1f73}, {6,0x73}, {4,0x1f},
    {CAM_OUT_FIXED+4,0xf}, {CAM_OUT_FIXED+1,UINT32_MAX},
    {CAM_OUT_FIXED,0x3f1f31}
};
static struct {
    vm_offset_t top, out[2];
    uint64_t epoch;
    bool held, close_attempted, cg_owned, cg_uncertain, consumed, commit_pending;
    unsigned pending_slot;
    uint32_t pending_next, dirty, target[CAM_OUT_WRITES];
    uint32_t before[2][CAM_OUT_REGS], expected[2][CAM_OUT_REGS];
    int error;
    struct x9_camera_outmux_status status;
} cam_out;
static bool cam_out_irq_control_allowed(unsigned,unsigned,uint32_t,bool);
static bool cam_out_irq_inner_target_required(void);
static int cam_out_irq_poison(int);
static void cam_out_irq_reset(void);
static int cam_out_irq_control_guard(void);
static int cam_out_irq_open_locked(void);
static int cam_out_irq_precommit(void);
static int cam_out_irq_finish_commit(void);
static int cam_out_irq_retirement_guard(void);
static int cam_out_irq_close_locked(void);
static void cam_out_irq_release(void);
static void cam_out_irq_status(void);
static unsigned cam_out_offset(unsigned n)
{
    if(n<CAM_OUT_FIXED)return cam_out_fixed[n];
    n-=CAM_OUT_FIXED;
    return 0x100+(n/5)*0x1c+cam_out_tag_offsets[n%5];
}
static void cam_out_status(void)
{
    cam_out_irq_status();
    cam_sv_status_copy(cam_out.status.sensor_epoch,&cam_out.status.sv);
    cam_out.status.last_errno=cam_out.error;cam_out.status.dirty=cam_out.dirty;
    cam_out.status.uncertain=cam_out.pending_slot<CAM_OUT_WRITES?1u<<cam_out.pending_slot:0;
    cam_out.status.commit_pending=cam_out.commit_pending;
    cam_out.status.cg_owned=cam_out.cg_owned;
    cam_out.status.cg_uncertain=cam_out.cg_uncertain;
    if(cam_out.error && cam_out.held)cam_out.status.flags|=X9_CAM_OUT_RETAINED;
    else cam_out.status.flags&=~X9_CAM_OUT_RETAINED;
}
static int cam_out_fault(int e)
{
    if(!cam_out.error)cam_out.error=e;
    if(e==ESTALE || e==EACCES || e==EBUSY || e==EOVERFLOW)cam_out_irq_poison(e);
    cam_out_status();
    return e;
}
/* The receiver keeps its initial snapshot as evidence. Expected samples
 * change ONLY for a confirmed write/inner consumption in this journal. */
static void cam_out_publish_expected(void)
{
    for(unsigned bank=0;bank<2;bank++)for(unsigned k=0;k<X9_CAM_RX_OUT_WORDS;k++)
        for(unsigned n=0;n<CAM_OUT_REGS;n++)
            if(cam_rx_out_offsets[k]==cam_out_offset(n))
                cam_rx_live.expected.out[bank][k]=cam_out.expected[bank][n];
}
static int cam_out_top_guard(bool admission)
{
    uint32_t cg;
    int e=cam_mm_guard(&cam_mm,true);if(e)return e;
    if(cam_rr(cam_out.top,4)&1)return EBUSY; /* selected TOP reset only */
    /* Preserve VM routing; never move a bank away from another VM. This
     * observed AP route is NOT a normal-memory/SMMU authorization grant. */
    if(!(cam_rr(cam_out.top,0xc0)&1) || (cam_rr(cam_out.top,0xc4)&1) ||
       (cam_rr(cam_out.top,0xc8)&1) || (cam_rr(cam_out.top,0xcc)&1))return EACCES;
    cg=cam_rr(cam_out.top,0xc)&1;
    if(admission)return cg?EBUSY:0; /* no borrowed/direct-owner CG repair */
    if(!cam_out.cg_uncertain && cg!=(cam_out.cg_owned?1u:0u))return ESTALE;
    return 0;
}
static int cam_out_environment(void)
{
    struct x9_camera_receiver_sample live,base;
    unsigned port=cam_out.status.sensor==X9_CAMERA_BACK?3:0;
    const struct cam_rx_maps *m=&cam_rx_maps[cam_out.status.sensor==X9_CAMERA_BACK?0:1];
    int e=cam_rx_sample(m,&live);if(e)return e;
    e=cam_mac_guard();if(e)return e;
    e=cam_phy_guard();if(e)return e;
    base=cam_rx_live.expected;
    /* OUTMUX is checked completely below, including all seven unused tags;
     * never suppress MAC/async/AFIFO/CAMSV changes under a route commit. */
    memset(live.out,0,sizeof(live.out));memset(base.out,0,sizeof(base.out));
    if(cam_rx_changed(&base,&live,port))return ESTALE;
    return cam_rx_busy(&live,port,cam_sv_fbc_busy_exemption())?EBUSY:0;
}
static int cam_out_read_bank(unsigned bank,uint32_t v[CAM_OUT_REGS])
{
    int e=cam_out_top_guard(false);if(e)return e;
    for(unsigned n=0;n<CAM_OUT_REGS;n++)v[n]=cam_rr(cam_out.out[bank],cam_out_offset(n));
    return 0;
}
/* During an ambiguous single shadow write only exact previous/next values
 * are allowed. During a posted commit inner may contain previous OR target
 * words, but it is never considered consumed until ALL words match. */
static int cam_out_guard_impl(void)
{
    uint32_t v[CAM_OUT_REGS];
    int e;
    if(!cam_out.held)return 0;
    if(cam_out.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return ESTALE;
    e=cam_sv_guard();if(e)return e;
    e=cam_out_irq_control_guard();if(e)return e;
    e=cam_out_top_guard(false);if(e)return e;
    e=cam_out_environment();if(e)return e;
    for(unsigned bank=0;bank<2;bank++) {
        e=cam_out_read_bank(bank,v);if(e)return e;
        for(unsigned n=0;n<CAM_OUT_REGS;n++) {
            bool allowed=v[n]==cam_out.expected[bank][n];
            if(!bank && cam_out.pending_slot<CAM_OUT_WRITES &&
               n==cam_out_fields[cam_out.pending_slot].reg && v[n]==cam_out.pending_next)allowed=true;
            if(bank && cam_out.commit_pending) {
                if(cam_out_irq_inner_target_required())allowed=v[n]==cam_out.expected[0][n];
                else if(v[n]==cam_out.expected[0][n])allowed=true;
            }
            allowed=cam_out_irq_control_allowed(bank,n,v[n],allowed);
            if(!allowed)return ESTALE;
        }
        /* Command/status are separate from config CSR, never IRQ_STATUS.
         * An unexpected pending command means somebody else changed the bank. */
        if((cam_rr(cam_out.out[bank],0x1c)&~1u) ||
           (cam_rr(cam_out.out[bank],0x38)&~1u))return EIO;
        if(cam_out_irq_inner_target_required() &&
           ((cam_rr(cam_out.out[bank],0x1c)|cam_rr(cam_out.out[bank],0x38))&1))return ESTALE;
        if(!cam_out.commit_pending &&
           ((cam_rr(cam_out.out[bank],0x1c)|cam_rr(cam_out.out[bank],0x38))&1))return EBUSY;
    }
    return 0;
}
static int cam_out_guard(void)
{
    int e=cam_out_guard_impl();
    /* Observed control/authority changes are terminal, unlike a consistent
     * posted write or event timeout. They cannot be adopted on sole close. */
    return e?cam_out_irq_poison(e):0;
}
/* This observation is NOT sufficient alone for command retirement. The
 * STATUSX owner additionally requires the fresh bound CFG_DONE event. */
static int cam_out_target_observe(uint32_t inner[CAM_OUT_REGS])
{
    uint32_t v[CAM_OUT_REGS],command,status;
    int e;
    for(unsigned sample=0;sample<2;sample++) {
        bool match=true;
        e=cam_out_guard();if(e)return e;
        for(unsigned bank=0;bank<2;bank++) {
            e=cam_out_read_bank(bank,v);if(e)return e;
            for(unsigned n=0;n<CAM_OUT_REGS;n++)if(v[n]!=cam_out.expected[0][n]) {
                if(!bank || cam_out_irq_inner_target_required() ||
                   v[n]!=cam_out.expected[1][n])return cam_out_irq_poison(ESTALE);
                match=false;
            }
            command=cam_rr(cam_out.out[bank],0x1c);status=cam_rr(cam_out.out[bank],0x38);
            if((command|status)&~1u)return cam_out_irq_poison(EIO);
            if((command|status)&1) {
                if(cam_out_irq_inner_target_required())return cam_out_irq_poison(ESTALE);
                match=false;
            }
            if(bank)memcpy(inner,v,sizeof(v));
        }
        if(!match)return EAGAIN;
        if(!sample)delay(1);
    }
    return 0;
}
#include "x9_camera_outmux_irq.inc"
static int cam_out_write(unsigned slot,uint32_t value)
{
    const struct cam_out_field *f=&cam_out_fields[slot];
    uint32_t next,seen;
    int e=cam_out_guard();if(e)return e;
    if(cam_out.pending_slot<CAM_OUT_WRITES || cam_out.commit_pending)return EBUSY;
    next=(cam_out.expected[0][f->reg]&~f->mask)|(value&f->mask);
    if(next==cam_out.expected[0][f->reg])return 0;
    cam_out.pending_slot=slot;cam_out.pending_next=next;cam_out.dirty|=1u<<slot;
    cam_rw(cam_out.out[0],cam_out_offset(f->reg),next);
    seen=cam_rr(cam_out.out[0],cam_out_offset(f->reg));
    if(seen!=next)return EIO; /* journal already includes ambiguous MMIO */
    cam_out.expected[0][f->reg]=next;cam_out.pending_slot=CAM_OUT_WRITES;
    cam_out_publish_expected();
    return cam_out_guard();
}
static int cam_out_commit(void)
{
    uint64_t span,deadline;
    int e=cam_out_guard();if(e)return e;
    if(cam_out.pending_slot<CAM_OUT_WRITES || cam_out.commit_pending)return EBUSY;
    e=cam_out_irq_precommit();if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);deadline=mach_absolute_time()+span;
    cam_out.commit_pending=true; /* BEFORE the possibly posted trigger */
    cam_rw(cam_out.out[0],0x1c,1);
    for(unsigned poll=0;poll<10000;poll++) {
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        e=cam_out_guard();if(e)return e;
        e=cam_out_irq_finish_commit();
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        if(!e)return cam_out_guard();
        if(e!=EAGAIN)return e;
        delay(1);
    }
    return ETIMEDOUT; /* never reset/cancel/retrigger/force inner CSR */
}
static int cam_out_cg_retirement_guard(void)
{
    uint64_t span,deadline;
    int e;
    if(!cam_out.cg_uncertain)return 0;
    if(!cam_out.cg_owned)return cam_out_irq_poison(ESTALE);
    /* The only uncertainty before sole close is our initial CG SET. Its
     * prior/off readback is NOT a cancellation or retirement proof. */
    nanoseconds_to_absolutetime(10000000,&span);deadline=mach_absolute_time()+span;
    for(unsigned poll=0;poll<10000;poll++) {
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        e=cam_out_guard();if(e)return e;
        if(cam_rr(cam_out.top,0xc)&1) {
            delay(1);e=cam_out_guard();if(e)return e;
            if(mach_absolute_time()>=deadline)return ETIMEDOUT;
            if(cam_rr(cam_out.top,0xc)&1) {
                cam_out.cg_uncertain=false;
                return cam_out_guard();
            }
            return cam_out_irq_poison(ESTALE); /* observed target cannot regress */
        }
        delay(1);
    }
    return ETIMEDOUT; /* observe only, never replay SET or remove its parent */
}
static int cam_out_open_locked(uint64_t epoch,struct x9_camera_outmux_status *r)
{
    struct x9_camera_pipeline plan;
    uint32_t again[CAM_OUT_REGS],cg;
    const struct cam_rx_maps *m;
    int e;
    memset(r,0,sizeof(*r));r->sensor_epoch=epoch;r->sensor=cam_sensor_live.probe.sensor;
    if(cam_out.held)return EALREADY;
    if(!cam_rx_sensor_held_locked(epoch) || cam_rx_live.close_attempted || cam_rx_live.error)return ESTALE;
    e=cam_pipeline_plan(r->sensor,&plan);if(e)return e;
    e=cam_rx_session_guard();if(e)return cam_rx_session_fault(e);
    e=cam_mac_guard();if(e)return cam_rx_session_fault(e);
    if(!cam_out.top)cam_out.top=ml_io_map(0x3a400000,0x1000);
    if(!cam_out.top)return ENOMEM;
    m=&cam_rx_maps[r->sensor==X9_CAMERA_BACK?0:1];
    cam_out.out[0]=m->out[0];cam_out.out[1]=m->out[1];
    e=cam_out_top_guard(true);if(e)return e;
    for(unsigned bank=0;bank<2;bank++) {
        /* Admission reads while the complete register-clock lifetime is
         * held. TOP data-path CG remains off, as in powered preflight. */
        for(unsigned n=0;n<CAM_OUT_REGS;n++)cam_out.before[bank][n]=cam_rr(cam_out.out[bank],cam_out_offset(n));
        delay(10);
        e=cam_out_top_guard(true);if(e)return e;
        for(unsigned n=0;n<CAM_OUT_REGS;n++)again[n]=cam_rr(cam_out.out[bank],cam_out_offset(n));
        if(memcmp(again,cam_out.before[bank],sizeof(again)))return ESTALE;
        if(cam_rr(cam_out.out[bank],0x1c) || cam_rr(cam_out.out[bank],0x38))return EBUSY;
    }
    /* A commit transfers more than tag0. Reject any inherited staged
     * difference rather than accidentally publishing another owner's tags. */
    if(memcmp(cam_out.before[0],cam_out.before[1],sizeof(cam_out.before[0])))return EBUSY;
    if((cam_out.before[0][0]&0x3f0103) || (cam_out.before[0][1]&3) ||
       (cam_out.before[0][7]&((1u<<12)|(1u<<28)|(1u<<29))) ||
       cam_out.before[0][8] || cam_out.before[0][9] || cam_out.before[0][10] ||
       cam_out.before[0][11] || (cam_out.before[0][12]&0x80) ||
       (cam_out.before[0][13]&0x71311) || cam_out.before[0][15])return EBUSY;
    for(unsigned tag=0;tag<8;tag++) {
        unsigned n=CAM_OUT_FIXED+tag*5;
        if((cam_out.before[0][n]&0x31) || cam_out.before[0][n+2] || cam_out.before[0][n+3])return EBUSY;
    }
    memcpy(cam_out.expected,cam_out.before,sizeof(cam_out.expected));
    cam_out.status=*r;cam_out.status.image_vc=plan.vc;
    cam_out.status.first_vc=plan.vc;cam_out.status.last_vc=r->sensor==X9_CAMERA_BACK?3:0;
    cam_out.status.vcdt=plan.vcdt;cam_out.status.expected_size=plan.expected_size;
    cam_out.target[0]=0; /* not 16-pixel mode; CAMSV RAW10 ordinary path */
    cam_out.target[1]=(plan.csi_port<<4)|(plan.vc<<8)|(cam_out.status.last_vc<<16);
    cam_out.target[2]=plan.csi_port<<4; /* unsplit init_port => src_sen=0 */
    cam_out.target[3]=plan.vc;cam_out.target[4]=1;
    cam_out.target[5]=plan.expected_size;cam_out.target[6]=plan.vcdt;
    cam_out.epoch=epoch;cam_out.held=true;cam_out.close_attempted=false;
    cam_out.cg_owned=cam_out.cg_uncertain=false;cam_out.consumed=cam_out.commit_pending=false;
    cam_out.pending_slot=CAM_OUT_WRITES;cam_out.dirty=0;cam_out.error=0;
    cam_out.status.flags=X9_CAM_OUT_ATTEMPTED;
    cam_out_irq_reset();
    /* Only newly added TOP outmux0 CG bit, preserve every unrelated port. */
    e=cam_rx_session_guard();
    if(!e)e=cam_out_top_guard(true);
    if(!e) {
        cg=cam_rr(cam_out.top,0xc);
        if(cg&1)e=EBUSY;
        else {
            cam_out.cg_owned=cam_out.cg_uncertain=true; /* BEFORE SET */
            cam_rw(cam_out.top,0xc,cg|1);
            if(!(cam_rr(cam_out.top,0xc)&1))e=EIO;
            else if((cam_rr(cam_out.top,0xc)^cg)&~1u)e=ESTALE;
            else {cam_out.cg_uncertain=false;e=cam_out_guard();}
        }
    }
    if(!e)e=cam_out_irq_open_locked();
    for(unsigned slot=0;slot<CAM_OUT_WRITES && !e;slot++)e=cam_out_write(slot,cam_out.target[slot]);
    if(!e){cam_out.status.flags|=X9_CAM_OUT_SHADOW;e=cam_out_commit();}
    if(!e)e=cam_sv_open_locked(epoch);
    if(e)cam_out_fault(e);
    cam_out_status();*r=cam_out.status;
    return e;
}
static int cam_out_close_locked(uint64_t epoch)
{
    uint32_t cg;
    int e;
    if(!cam_out.held)return 0;
    if(cam_out.epoch!=epoch || cam_out.close_attempted)return ESTALE;
    cam_out.close_attempted=true;
    /* A late target AND the bound fresh event may qualify the sole close.
     * Never cancel/replay a trigger, accept prior as retirement, or retry a
     * failed W1C. Unknown events/controls retain all remaining dependencies. */
    e=cam_out_cg_retirement_guard();
    if(!e)e=cam_out_irq_retirement_guard();
    if(!e)e=cam_out_guard();
    if(!e && cam_out.pending_slot<CAM_OUT_WRITES) {
        unsigned reg=cam_out_fields[cam_out.pending_slot].reg;
        /* guard accepted only exact previous/next; retire uncertainty by
         * observation, never by repeating the original failed write. */
        cam_out.expected[0][reg]=cam_rr(cam_out.out[0],cam_out_offset(reg));
        if(cam_out.expected[0][reg]!=cam_out.pending_next &&
           cam_out.expected[0][reg]!=cam_out.before[0][reg])e=ESTALE;
        else {cam_out.pending_slot=CAM_OUT_WRITES;cam_out_publish_expected();}
    }
    for(unsigned i=CAM_OUT_WRITES;i && !e;i--) {
        unsigned slot=i-1;
        if(cam_out.dirty&(1u<<slot))e=cam_out_write(slot,cam_out.before[0][cam_out_fields[slot].reg]);
    }
    if(!e && cam_out.consumed)e=cam_out_commit();
    if(!e)e=cam_out_guard();
    if(!e)e=cam_out_irq_close_locked();
    if(!e)e=cam_out_guard();
    if(!e) {
        cg=cam_rr(cam_out.top,0xc);
        if(!cam_out.cg_owned && (cg&1))e=ESTALE;
        else {
            cam_out.cg_uncertain=true; /* ambiguous CLR retains the CG journal */
            if(cam_out.cg_owned && (cg&1))cam_rw(cam_out.top,0xc,cg&~1u);
            if(cam_rr(cam_out.top,0xc)&1)e=EIO;
            else if((cam_rr(cam_out.top,0xc)^cg)&~1u)e=ESTALE;
        }
        if(!e) {
            cam_out_irq_release();
            cam_out.cg_owned=cam_out.cg_uncertain=false;cam_out.held=false;cam_out.epoch=0;
            cam_out.dirty=0;cam_out.consumed=false;cam_out.status.flags|=X9_CAM_OUT_RESTORED;
        }
    }
    if(e)cam_out_fault(e);
    cam_out.status.restore_errno=e;cam_out_status();
    printf("X9 CAMERA OUTMUX CLOSE: sensor=%u epoch=%llu flags=%x errno=%u restore=%d dirty=%x "
        "uncertain=%x pending=%u CG=%u/%u irq=%x/%u/%u/%llu raw=%x/%x samples=%u event=%u "
        "(DL/IRQ/DMA never enabled)\n",cam_out.status.sensor,
        epoch,cam_out.status.flags,cam_out.status.last_errno,e,cam_out.status.dirty,
        cam_out.status.uncertain,cam_out.status.commit_pending,cam_out.status.cg_owned,
        cam_out.status.cg_uncertain,cam_out.status.irq.flags,cam_out.status.irq.mode_pending,
        cam_out.status.irq.ack_pending,cam_out.status.irq.completed,cam_out.status.irq.raw_before,
        cam_out.status.irq.raw_after,cam_out.status.irq.sampled,cam_out.status.irq.event_seen);
    return e;
}
int x9_camera_sensor_session_configure_outmux(uint64_t epoch,struct x9_camera_outmux_status *r)
{
    struct cam_sensor_ap ap;
    uint8_t byte=0;
    int e;
    if(!r)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    memset(r,0,sizeof(*r));r->sensor_epoch=epoch;
    e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED || session.capture || session.stream))e=EBUSY;
    if(!e && cam_out.held)e=EALREADY;
    if(!e && !cam_rx_sensor_held_locked(epoch))e=ENXIO;
    if(!e) {
        ap=(struct cam_sensor_ap){&cam_journal.sensor,cam_sensor_live.probe.sensor==X9_CAMERA_BACK?0x1a:0x10};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
        if(e)cam_sensor_session_fault(e);
        if(!e)e=cam_out_open_locked(epoch,r);
        if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e && cam_out.held){cam_out_fault(e);cam_rx_session_fault(e);cam_sensor_session_fault(e);}
        else if(e && cam_rx_live.error)cam_sensor_session_fault(e);
        if(cam_out.held)*r=cam_out.status;
    }
    r->last_errno=e;
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
