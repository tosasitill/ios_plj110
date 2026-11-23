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
enum { CAM_OUT_FIXED=16, CAM_OUT_REGS=CAM_OUT_FIXED+8*5, CAM_OUT_WRITES=8 };
static const unsigned cam_out_fixed[CAM_OUT_FIXED]={
    0,4,8,0xc,0x10,0x14,0x18,0x20,0x2c,0x34,0x40,0x48,0xa0,0xa4,0xa8,0x50
};
static const unsigned cam_out_tag_offsets[5]={0,4,8,0xc,0x14};
struct cam_out_field { unsigned reg; uint32_t mask; };
static const struct cam_out_field cam_out_fields[CAM_OUT_WRITES]={
    {2,1}, {3,0x1f1f73}, {6,0x73}, {4,0x1f},
    {CAM_OUT_FIXED+4,0xf}, {CAM_OUT_FIXED+1,UINT32_MAX},
    {CAM_OUT_FIXED,0x3f1f31},
    {8,1} /* CAM_CFG_RDY=0: Android "write rdy_msk sw is 0 as default" */
};
static struct {
    vm_offset_t top, out[2];
    uint64_t epoch;
    bool held, close_attempted, cg_owned, cg_uncertain, consumed, commit_pending;
    bool cg_borrowed, deferred, ready_raised; /* Android-tolerated states */
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
/* Words Android leaves moving: CAM_CFG_RDY after set_engines_mux_ready, and
 * inner words of a cfg_done still waiting for the reference VSYNC. */
static bool cam_out_rx_relaxed(void)
{
    return cam_out.held && (cam_out.deferred || cam_out.ready_raised);
}
static bool cam_out_tolerated(unsigned bank,unsigned n,uint32_t v)
{
    if(n==8 && cam_out.ready_raised && v<=1)return true;
    if(bank && cam_out.deferred &&
       (v==cam_out.expected[0][n] || v==cam_out.expected[1][n]))return true;
    return false;
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
    uint32_t cg,sel;
    int e=cam_mm_guard(&cam_mm,true);if(e)return e;
    if(cam_rr(cam_out.top,4)&1)return CAM_FAIL(EBUSY); /* selected TOP reset only */
    sel=cam_rr(cam_out.top,0xc0);
    if(admission && !(sel&1)) {
        /* Android core_common_reg_setup -> set_irq_grping: DEVICE_IRQ_SEL_0=0x3ff. */
        cam_rw(cam_out.top,0xc0,sel|0x3ffu);
        printf("X9 CAMERA OUTMUX ADOPT: DEVICE_IRQ_SEL_0 %x->%x sel1-3=%x/%x/%x (Android set_irq_grping)\n",
            sel,cam_rr(cam_out.top,0xc0),cam_rr(cam_out.top,0xc4),cam_rr(cam_out.top,0xc8),
            cam_rr(cam_out.top,0xcc));
        sel=cam_rr(cam_out.top,0xc0);
    }
    if(!(sel&1))return CAM_FAIL(EACCES);
    cg=cam_rr(cam_out.top,0xc)&1;
    if(admission)return 0; /* Android enables every OUTMUX CG; an on bit is borrowed */
    if(!cam_out.cg_uncertain && cg!=((cam_out.cg_owned||cam_out.cg_borrowed)?1u:0u))return CAM_FAIL(ESTALE);
    return 0;
}
static int cam_out_environment(void)
{
    struct x9_camera_receiver_sample live,base;
    unsigned port=cam_sensor_port(cam_out.status.sensor);
    const struct cam_rx_maps *m=&cam_rx_maps[(cam_out.status.sensor-1)];
    int e=cam_rx_sample(m,&live);if(e)return e;
    e=cam_mac_guard();if(e)return e;
    e=cam_phy_guard();if(e)return e;
    base=cam_rx_live.expected;
    /* OUTMUX is checked completely below, including all seven unused tags;
     * never suppress MAC/async/AFIFO/CAMSV changes under a route commit. */
    memset(live.out,0,sizeof(live.out));memset(base.out,0,sizeof(base.out));
    if(cam_rx_changed(&base,&live,port))return CAM_FAIL(ESTALE);
    return cam_rx_busy(&live,port,cam_sv_fbc_busy_exemption(),
        cam_sv_receive_busy_exemption(0x040),cam_sv_gate_busy_exemption(0x140),
        cam_sv_gate_busy_exemption(0x148),cam_source_busy_exemption(m->mac,0x40),0)?CAM_FAIL(EBUSY):0;
}
static int cam_out_read_bank(unsigned bank,uint32_t v[CAM_OUT_REGS])
{
    int e=cam_out_top_guard(false);if(e)return e;
    for(unsigned n=0;n<CAM_OUT_REGS;n++) {
        bool allowed=false;
        v[n]=cam_rr(cam_out.out[bank],cam_out_offset(n));
        e=cam_source_observe(cam_out.out[bank],cam_out_offset(n),v[n],&allowed);if(e)return e;
    }
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
    if(cam_out.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return CAM_FAIL(ESTALE);
    e=cam_source_commit_guard();if(e)return e;
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
            if(!allowed && cam_out_tolerated(bank,n,v[n]))allowed=true;
            if(!allowed)return CAM_FAIL(ESTALE);
        }
        /* Command/status are separate from config CSR, never IRQ_STATUS.
         * An unexpected pending command means somebody else changed the bank. */
        if((cam_rr(cam_out.out[bank],0x1c)&~1u) ||
           (cam_rr(cam_out.out[bank],0x38)&~1u))return CAM_FAIL(EIO);
        if(cam_out_irq_inner_target_required() &&
           ((cam_rr(cam_out.out[bank],0x1c)|cam_rr(cam_out.out[bank],0x38))&1))return CAM_FAIL(ESTALE);
        if(!cam_out.commit_pending && !cam_out.deferred &&
           ((cam_rr(cam_out.out[bank],0x1c)|cam_rr(cam_out.out[bank],0x38))&1))return CAM_FAIL(EBUSY);
    }
    return 0;
}
static int cam_out_guard_untimed(void)
{
    int e=cam_out_guard_impl();
    /* Observed control/authority changes are terminal, unlike a consistent
     * posted write or event timeout. They cannot be adopted on sole close. */
    return e?cam_out_irq_poison(e):0;
}
static int cam_out_guard(void)
{
    static unsigned hits;uint64_t t0=mach_absolute_time();
    int e=cam_out_guard_untimed();
    cam_slow_note("cam_out_guard",&hits,t0);
    return e;
}
/* This observation is NOT sufficient alone for command retirement. The
 * STATUSX owner additionally requires the fresh bound CFG_DONE event. */
static int cam_out_target_observe(uint32_t inner[CAM_OUT_REGS])
{
    uint32_t v[CAM_OUT_REGS],command,status,diff=0,first=CAM_OUT_REGS;
    int e;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_out_guard();if(e)return e;
        for(unsigned bank=0;bank<2;bank++) {
            command=cam_rr(cam_out.out[bank],0x1c);status=cam_rr(cam_out.out[bank],0x38);
            if((command|status)&~1u)return cam_out_irq_poison(EIO);
            if((command|status)&1)return EAGAIN; /* Android: SW_CFG_DONE_STATUS still 1 */
        }
        e=cam_out_read_bank(1,v);if(e)return e;
        if(sample && memcmp(inner,v,sizeof(v)))return EAGAIN; /* inner still moving */
        memcpy(inner,v,sizeof(v));
        if(!sample)delay(1);
    }
    /* Android never compares inner with outer; adopt and report. */
    for(unsigned n=0;n<CAM_OUT_REGS;n++)if(inner[n]!=cam_out.expected[0][n]) {
        if(first==CAM_OUT_REGS)first=n;
        diff++;
    }
    if(diff)printf("X9 CAMERA OUTMUX ADOPT: cfg_done retired, %u inner word(s) != outer, first off=%x outer=%x inner=%x\n",
        diff,cam_out_offset(first),cam_out.expected[0][first],inner[first]);
    return 0;
}
#include "x9_camera_outmux_irq.inc"
static int cam_out_write(unsigned slot,uint32_t value)
{
    const struct cam_out_field *f=&cam_out_fields[slot];
    uint32_t next,seen;
    int e=cam_out_guard();if(e)return e;
    if(cam_out.pending_slot<CAM_OUT_WRITES || cam_out.commit_pending)return CAM_FAIL(EBUSY);
    next=(cam_out.expected[0][f->reg]&~f->mask)|(value&f->mask);
    if(next==cam_out.expected[0][f->reg])return 0;
    cam_out.pending_slot=slot;cam_out.pending_next=next;cam_out.dirty|=1u<<slot;
    cam_rw(cam_out.out[0],cam_out_offset(f->reg),next);
    seen=cam_rr(cam_out.out[0],cam_out_offset(f->reg));
    if(seen!=next)return CAM_FAIL(EIO); /* journal already includes ambiguous MMIO */
    cam_out.expected[0][f->reg]=next;cam_out.pending_slot=CAM_OUT_WRITES;
    cam_out_publish_expected();
    return cam_out_guard();
}
/* Android outmux_config_all raises SW_CFG_DONE and never waits for it; only
 * the next config waits max(1s,1.1 frame) (_wait_outmux_cfg_done). The latch
 * needs the reference VSYNC, so with the sensor still off the posted command
 * is kept and settled after stream-on (cam_out_settle_locked). */
static int cam_out_defer(void)
{
    printf("X9 CAMERA OUTMUX ADOPT: cfg_done pending after 1s cmd=%x/%x done_status=%x/%x statusx=%x; "
        "settle after stream-on (Android never waits)\n",
        cam_rr(cam_out.out[0],0x1c),cam_rr(cam_out.out[1],0x1c),
        cam_rr(cam_out.out[0],0x38),cam_rr(cam_out.out[1],0x38),cam_rr(cam_out.out[0],0x54));
    cam_out.deferred=true;cam_out.commit_pending=false;cam_out.consumed=true;
    cam_out.status.flags|=X9_CAM_OUT_CONSUMED;
    cam_out_irq.status.completed=cam_out_irq.status.command;
    return cam_out_guard();
}
static int cam_out_commit(void)
{
    uint64_t span,deadline;
    int e=cam_out_guard();if(e)return e;
    if(cam_out.pending_slot<CAM_OUT_WRITES || cam_out.commit_pending)return CAM_FAIL(EBUSY);
    e=cam_out_irq_precommit();if(e)return e;
    nanoseconds_to_absolutetime(1000000000ull,&span);deadline=mach_absolute_time()+span;
    cam_out.commit_pending=true; /* BEFORE the possibly posted trigger */
    cam_rw(cam_out.out[0],0x1c,1);
    for(;;) {
        e=cam_out_guard();if(e)return e;
        e=cam_out_irq_finish_commit();
        if(!e){cam_out.deferred=false;return cam_out_guard();}
        if(e!=EAGAIN)return e;
        if(mach_absolute_time()>=deadline)break;
        delay(1000);
    }
    return cam_out_defer();
}
/* After the sensor stream-on ACK the reference VSYNC exists. Raw reads only
 * (camera_irq_lock context): adopt the latched inner bank or keep tolerating. */
static void cam_out_settle_locked(void)
{
    uint64_t span,deadline;
    uint32_t inner[CAM_OUT_REGS],again[CAM_OUT_REGS],pending=1;
    if(!cam_out.held || !cam_out.deferred)return;
    nanoseconds_to_absolutetime(1000000000ull,&span);deadline=mach_absolute_time()+span;
    while(mach_absolute_time()<deadline) {
        pending=(cam_rr(cam_out.out[0],0x1c)|cam_rr(cam_out.out[0],0x38)|
            cam_rr(cam_out.out[1],0x1c)|cam_rr(cam_out.out[1],0x38))&1;
        if(!pending) {
            for(unsigned n=0;n<CAM_OUT_REGS;n++)inner[n]=cam_rr(cam_out.out[1],cam_out_offset(n));
            delay(1);
            for(unsigned n=0;n<CAM_OUT_REGS;n++)again[n]=cam_rr(cam_out.out[1],cam_out_offset(n));
            if(!memcmp(inner,again,sizeof(inner)))break;
            pending=1;
        }
        delay(1000);
    }
    if(!pending) {
        memcpy(cam_out.expected[1],inner,sizeof(inner));
        cam_out.deferred=false;
        cam_out_publish_expected();
    }
    printf("X9 CAMERA OUTMUX SETTLE: deferred cfg_done %s cmd=%x done_status=%x statusx=%x src=%x/%x\n",
        pending?"STILL PENDING (tolerated)":"latched",cam_rr(cam_out.out[0],0x1c),
        cam_rr(cam_out.out[0],0x38),cam_rr(cam_out.out[0],0x54),
        cam_rr(cam_out.out[0],0),cam_rr(cam_out.out[1],0));
}
static int cam_out_cg_retirement_guard(void)
{
    uint64_t span,deadline;
    int e;
    if(!cam_out.cg_uncertain)return 0;
    if(!cam_out.cg_owned)return cam_out_irq_poison(ESTALE);
    /* The only uncertainty before sole close is our initial CG SET. Its
     * prior/off readback is NOT a cancellation or retirement proof. */
    nanoseconds_to_absolutetime(1000000000ull /* log 376: guards cost 3-90 ms per poll */,&span);deadline=mach_absolute_time()+span;
    for(unsigned poll=0;poll<10000;poll++) {
        if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
        e=cam_out_guard();if(e)return e;
        if(cam_rr(cam_out.top,0xc)&1) {
            delay(1);e=cam_out_guard();if(e)return e;
            if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
            if(cam_rr(cam_out.top,0xc)&1) {
                cam_out.cg_uncertain=false;
                return cam_out_guard();
            }
            return cam_out_irq_poison(ESTALE); /* observed target cannot regress */
        }
        delay(1);
    }
    return CAM_FAIL(ETIMEDOUT); /* observe only, never replay SET or remove its parent */
}
/* Android outmux_reset_all -> _disable_outmux(immed=true) before config:
 * W1C STATUSX, then LOCAL_SW_RST 1, udelay(1), 0 while the CG is on. The
 * reset result becomes the journal baseline; before[] stays the restore set. */
static int cam_out_local_reset(void)
{
    uint32_t v,statusx,diff=0,first=0,was=0,now=0;
    statusx=cam_rr(cam_out.out[0],0x54);
    if(statusx)cam_rw(cam_out.out[0],0x54,statusx);
    cam_rw(cam_out.out[0],0x34,1);delay(1);cam_rw(cam_out.out[0],0x34,0);
    for(unsigned bank=0;bank<2;bank++)for(unsigned n=0;n<CAM_OUT_REGS;n++) {
        v=cam_rr(cam_out.out[bank],cam_out_offset(n));
        if(v!=cam_out.expected[bank][n]) {
            if(!diff){first=bank*0x10000u+cam_out_offset(n);was=cam_out.expected[bank][n];now=v;}
            diff++;
        }
        cam_out.expected[bank][n]=v;
    }
    cam_out_publish_expected();
    printf("X9 CAMERA OUTMUX LOCAL RESET: statusx=%x changed=%u first=%x %x->%x cmd=%x/%x done=%x/%x "
        "(Android disable_outmux immed)\n",statusx,diff,first,was,now,
        cam_rr(cam_out.out[0],0x1c),cam_rr(cam_out.out[1],0x1c),
        cam_rr(cam_out.out[0],0x38),cam_rr(cam_out.out[1],0x38));
    return (cam_rr(cam_out.out[0],0x34)&1)?CAM_FAIL(EIO):0;
}
static int cam_out_open_locked(uint64_t epoch,struct x9_camera_outmux_status *r)
{
    struct x9_camera_pipeline plan;
    uint32_t again[CAM_OUT_REGS],cg;
    const struct cam_rx_maps *m;
    int e;
    memset(r,0,sizeof(*r));r->sensor_epoch=epoch;r->sensor=cam_sensor_live.probe.sensor;
    if(cam_out.held)return CAM_FAIL(EALREADY);
    if(!cam_rx_sensor_held_locked(epoch) || cam_rx_live.close_attempted || cam_rx_live.error)return CAM_FAIL(ESTALE);
    e=cam_pipeline_plan(r->sensor,&plan);if(e)return e;
    e=cam_rx_session_guard();if(e)return cam_rx_session_fault(e);
    e=cam_mac_guard();if(e)return cam_rx_session_fault(e);
    if(!cam_out.top)cam_out.top=ml_io_map(0x3a400000,0x1000);
    if(!cam_out.top)return CAM_FAIL(ENOMEM);
    m=&cam_rx_maps[(r->sensor-1)];
    cam_out.out[0]=m->out[0];cam_out.out[1]=m->out[1];
    e=cam_out_top_guard(true);if(e)return e;
    for(unsigned bank=0;bank<2;bank++) {
        /* Admission reads while the complete register-clock lifetime is
         * held. TOP data-path CG remains off, as in powered preflight. */
        for(unsigned n=0;n<CAM_OUT_REGS;n++)cam_out.before[bank][n]=cam_rr(cam_out.out[bank],cam_out_offset(n));
        delay(10);
        e=cam_out_top_guard(true);if(e)return e;
        for(unsigned n=0;n<CAM_OUT_REGS;n++)again[n]=cam_rr(cam_out.out[bank],cam_out_offset(n));
        if(memcmp(again,cam_out.before[bank],sizeof(again)))return CAM_FAIL(ESTALE);
        /* Android _wait_outmux_cfg_done: wait for SW_CFG_DONE_STATUS to clear. */
        /* Android cfg_done_max_delay >= 1s; on timeout it only asserts. */
        for(unsigned w=0;w<10000 && (cam_rr(cam_out.out[bank],0x38)&1);w++)delay(100);
        if(cam_rr(cam_out.out[bank],0x1c) || cam_rr(cam_out.out[bank],0x38))
            printf("X9 CAMERA OUTMUX INHERITED: bank=%u cmd=%x done_status=%x pending after 1s; "
                "LOCAL_SW_RST follows (Android only asserts)\n",
                bank,cam_rr(cam_out.out[bank],0x1c),cam_rr(cam_out.out[bank],0x38));
    }
    printf("X9 CAMERA OUTMUX ADMIT: outer ctrl/rdy=%x/%x inner=%x/%x src=%x/%x\n",
        cam_out.before[0][7],cam_out.before[0][8],cam_out.before[1][7],cam_out.before[1][8],
        cam_out.before[0][0],cam_out.before[1][0]);
    {
        uint32_t odd=0,tags=0;
        /* Android _disable_outmux + config_outmux overwrite all of these. */
        if(memcmp(cam_out.before[0],cam_out.before[1],sizeof(cam_out.before[0])))odd|=1;
        if((cam_out.before[0][0]&0x3f0103) || (cam_out.before[0][1]&3) ||
           cam_out.before[0][9] || cam_out.before[0][10] ||
           cam_out.before[0][11] || (cam_out.before[0][12]&0x80) ||
           (cam_out.before[0][13]&0x71311) || cam_out.before[0][15])odd|=2;
        for(unsigned tag=0;tag<8;tag++) {
            unsigned n=CAM_OUT_FIXED+tag*5;
            if((cam_out.before[0][n]&0x31) || cam_out.before[0][n+2] || cam_out.before[0][n+3])tags|=1u<<tag;
        }
        if(odd || tags)printf("X9 CAMERA OUTMUX INHERITED: banks_differ=%u nonidle=%u tags=%x mode=%x filt=%x "
            "rst=%x irq_en=%x/%x ctrl2=%x ctrl3=%x trig=%x top_sel=%x/%x/%x/%x cg=%x (Android overwrites)\n",
            odd&1,(odd>>1)&1,tags,cam_out.before[0][0],cam_out.before[0][1],cam_out.before[0][9],
            cam_out.before[0][10],cam_out.before[0][11],cam_out.before[0][12],cam_out.before[0][13],
            cam_out.before[0][15],cam_rr(cam_out.top,0xc0),cam_rr(cam_out.top,0xc4),
            cam_rr(cam_out.top,0xc8),cam_rr(cam_out.top,0xcc),cam_rr(cam_out.top,0xc));
    }
    memcpy(cam_out.expected,cam_out.before,sizeof(cam_out.expected));
    cam_out.status=*r;cam_out.status.image_vc=plan.vc;
    cam_out.status.first_vc=plan.vc;cam_out.status.last_vc=sensors[r->sensor-1].last_vc;
    cam_out.status.vcdt=plan.vcdt;cam_out.status.expected_size=plan.expected_size;
    cam_out.target[0]=0; /* not 16-pixel mode; CAMSV RAW10 ordinary path */
    cam_out.target[1]=(plan.csi_port<<4)|(plan.vc<<8)|(cam_out.status.last_vc<<16);
    cam_out.target[2]=plan.csi_port<<4; /* unsplit init_port => src_sen=0 */
    cam_out.target[3]=plan.vc;cam_out.target[4]=1;
    cam_out.target[5]=plan.expected_size;cam_out.target[6]=plan.vcdt;cam_out.target[7]=0;
    cam_out.epoch=epoch;cam_out.held=true;cam_out.close_attempted=false;
    cam_out.cg_owned=cam_out.cg_uncertain=false;cam_out.consumed=cam_out.commit_pending=false;
    cam_out.cg_borrowed=cam_out.deferred=cam_out.ready_raised=false;
    cam_out.pending_slot=CAM_OUT_WRITES;cam_out.dirty=0;cam_out.error=0;
    cam_out.status.flags=X9_CAM_OUT_ATTEMPTED;
    cam_out_irq_reset();
    /* Only newly added TOP outmux0 CG bit, preserve every unrelated port. */
    e=cam_rx_session_guard();
    if(!e)e=cam_out_top_guard(true);
    if(!e) {
        cg=cam_rr(cam_out.top,0xc);
        if(cg&1) {
            cam_out.cg_borrowed=true;
            printf("X9 CAMERA OUTMUX INHERITED: TOP OUTMUX_CG_EN=%x already on; borrowed, left on at close "
                "(Android enables all)\n",cg);
            e=cam_out_guard();
        } else {
            cam_out.cg_owned=cam_out.cg_uncertain=true; /* BEFORE SET */
            cam_rw(cam_out.top,0xc,cg|1);
            if(!(cam_rr(cam_out.top,0xc)&1))e=CAM_FAIL(EIO);
            else if((cam_rr(cam_out.top,0xc)^cg)&~1u)e=CAM_FAIL(ESTALE);
            else {cam_out.cg_uncertain=false;e=cam_out_guard();}
        }
    }
    if(!e)e=cam_out_local_reset();
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
    if(cam_out.epoch!=epoch)return CAM_FAIL(ESTALE);
    if(cam_out.close_attempted)
        printf("X9 CAMERA OUTMUX CLOSE RETRY: epoch=%llu prior errno=%d (retry allowed)\n",epoch,cam_out.error);
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
           cam_out.expected[0][reg]!=cam_out.before[0][reg])e=CAM_FAIL(ESTALE);
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
        if(cam_out.cg_owned) {
            cam_out.cg_uncertain=true; /* ambiguous CLR retains the CG journal */
            if(cg&1)cam_rw(cam_out.top,0xc,cg&~1u);
            if(cam_rr(cam_out.top,0xc)&1)e=CAM_FAIL(EIO);
            else if((cam_rr(cam_out.top,0xc)^cg)&~1u)e=CAM_FAIL(ESTALE);
        } /* a borrowed (or never set) CG is left as found */
        if(!e) {
            cam_out_irq_release();
            cam_out.cg_owned=cam_out.cg_uncertain=cam_out.cg_borrowed=false;cam_out.held=false;cam_out.epoch=0;
            cam_out.deferred=cam_out.ready_raised=false;
            cam_out.dirty=0;cam_out.consumed=false;cam_out.status.flags|=X9_CAM_OUT_RESTORED;
        }
    }
    if(e)cam_out_fault(e);
    cam_out.status.restore_errno=e;cam_out_status();
    if(e) {
        /* Non-wedging: the next open re-runs Android's reset (LOCAL_SW_RST)
         * and the full config, so the software journal is released here. */
        printf("X9 CAMERA OUTMUX CLOSE FORCED: errno=%d dirty=%x pending=%u deferred=%u cg=%u/%u; journal released\n",
            e,cam_out.dirty,cam_out.commit_pending,cam_out.deferred,cam_out.cg_owned,cam_out.cg_borrowed);
        cam_out_irq_release();
        cam_out.held=false;cam_out.epoch=0;cam_out.cg_owned=cam_out.cg_uncertain=cam_out.cg_borrowed=false;
        cam_out.deferred=cam_out.ready_raised=cam_out.commit_pending=cam_out.consumed=false;
        cam_out.pending_slot=CAM_OUT_WRITES;cam_out.dirty=0;
    }
    printf("X9 CAMERA OUTMUX CLOSE: sensor=%u epoch=%llu flags=%x errno=%u restore=%d dirty=%x "
        "uncertain=%x pending=%u CG=%u/%u irq=%x/%u/%u/%llu raw=%x/%x samples=%u event=%u "
        "(DL/IRQ/DMA never enabled)\n",cam_out.status.sensor,
        epoch,cam_out.status.flags,cam_out.status.last_errno,e,cam_out.status.dirty,
        cam_out.status.uncertain,cam_out.status.commit_pending,cam_out.status.cg_owned,
        cam_out.status.cg_uncertain,cam_out.status.irq.flags,cam_out.status.irq.mode_pending,
        cam_out.status.irq.ack_pending,cam_out.status.irq.completed,cam_out.status.irq.raw_before,
        cam_out.status.irq.raw_after,cam_out.status.irq.sampled,cam_out.status.irq.event_seen);
    return 0; /* any error is logged above and the journal released (non-wedging) */
}
/* Android set_engines_mux_ready(true) right before the sensor START:
 * _set_outmux_rdy_msk_sw_rdy_status(1) writes the outer CAM_CFG_RDY only. */
static int cam_out_ready_raise_locked(uint64_t epoch)
{
    uint32_t prior,inner;
    if(!cam_out.held || cam_out.epoch!=epoch || !cam_out.consumed)return CAM_FAIL(ESTALE);
    if(cam_out.ready_raised)return 0;
    prior=cam_rr(cam_out.out[0],0x2c);
    cam_out.ready_raised=true; /* BEFORE the write: guards accept 0/1 */
    cam_rw(cam_out.out[0],0x2c,1);
    cam_out.expected[0][8]=cam_rr(cam_out.out[0],0x2c);
    delay(1);
    inner=cam_rr(cam_out.out[1],0x2c);
    cam_out.expected[1][8]=inner;
    cam_out_publish_expected();
    printf("X9 CAMERA OUTMUX READY: CAM_CFG_RDY outer %x->%x inner=%x ctrl=%x/%x deferred=%u "
        "(Android set_engines_mux_ready)\n",prior,cam_out.expected[0][8],inner,
        cam_rr(cam_out.out[0],0x20),cam_rr(cam_out.out[1],0x20),cam_out.deferred);
    return (cam_out.expected[0][8]&1)?0:CAM_FAIL(EIO);
}
int x9_camera_sensor_session_configure_outmux(uint64_t epoch,struct x9_camera_outmux_status *r)
{
    struct cam_sensor_ap ap;
    uint8_t byte=0;
    int e;
    if(!r)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_control_lock);
    memset(r,0,sizeof(*r));r->sensor_epoch=epoch;
    e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED || session.capture || session.stream))e=CAM_FAIL(EBUSY);
    if(!e && cam_out.held)e=CAM_FAIL(EALREADY);
    if(!e && !cam_rx_sensor_held_locked(epoch))e=CAM_FAIL(ENXIO);
    if(!e) {
        ap=(struct cam_sensor_ap){&cam_journal.sensor,cam_sensor_slave(cam_sensor_live.probe.sensor)};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=CAM_FAIL(EBUSY);
        if(e)cam_sensor_session_fault(e);
        if(!e)e=cam_out_open_locked(epoch,r);
        if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e && cam_out.held){cam_out_fault(e);cam_rx_session_fault(e);cam_sensor_session_fault(e);}
        else if(e && cam_rx_live.error)cam_sensor_session_fault(e);
        if(cam_out.held)*r=cam_out.status;
    }
    r->last_errno=e;
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
