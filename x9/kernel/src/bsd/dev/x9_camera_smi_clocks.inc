/* MT6993 CAMSV id0's real supplier/clock graph. Live DT larbs19/14/29:
 * 19 -> cam subcomm3 -> disp common0 (skip-rpm-cb, no clocks);
 * 14 -> cam subcomm1 -> subcomm1_1 -> disp common0;
 * 29 -> cam subcomm2 -> mdp common1 (skip-rpm-cb, no clocks).
 * subcomm1 and 1_1 share the SAME physical R1A bit13; take it only once.
 * Upstream mtk_smi_bus_enable recursively enables suppliers first. Its
 * config_port/common QoS/routing writes are NOT implied by these clocks.
 * Never write VCP/display skip-rpm suppliers, protected/MMU/path fields,
 * aggregate votes, physical CG_SET, AL maps, shared resets or IRQ history.
 */
#include "x9_camera_smi_clocks.h"
enum { CAM_SMI_SLOTS=2, CAM_SMI_NONE=0, CAM_SMI_SET=1,
    CAM_SMI_CG_CLR=2, CAM_SMI_CLR=3 };
struct cam_smi_gate {
    unsigned set, global, sta;
    uint32_t mask, ap_before, ap_expected, cg_before, cg_expected;
    uint32_t owned, borrowed;
    vm_offset_t cg;
};
static struct {
    uint64_t epoch;
    struct cam_smi_gate gate[CAM_SMI_SLOTS];
    vm_offset_t larb[3];
    unsigned seen, completed, pending, pending_slot;
    uint32_t parent_cfg;
    bool held, close_attempted;
    int error;
    struct x9_camera_smi_clock_status status;
} cam_smi;
static int cam_smi_a0_guard(void);
static int cam_smi_a0_open_locked(void);
static int cam_smi_a0_close_locked(void);
static int cam_smi_a0_pending_retire(void);
static int cam_native_bw_guard(void);
static int cam_native_bw_close_locked(void);
static int cam_smi_shared_capture_locked(void);
static int cam_smi_shared_voter_guard(void);
static int cam_smi_shared_control_guard(bool);
static int cam_smi_shared_open_locked(void);
static int cam_smi_shared_close_locked(void);
static int cam_smi_shared_retirement_guard(uint64_t);
static bool cam_smi_clocks_held(void) { return cam_smi.held; }
static void cam_smi_status(void)
{
    struct x9_camera_smi_clock_status *s=&cam_smi.status;
    s->seen=cam_smi.seen;s->completed=cam_smi.completed;s->pending=cam_smi.pending;
    s->parent_cfg=cam_smi.parent_cfg;s->last_errno=cam_smi.error;
    for(unsigned n=0;n<CAM_SMI_SLOTS;n++) {
        const struct cam_smi_gate *g=&cam_smi.gate[n];
        s->mask[n]=g->mask;s->owned[n]=g->owned;s->borrowed[n]=g->borrowed;
        s->gate_before[n]=g->cg_before;
    }
    if(cam_smi.error&&cam_smi.held)s->flags|=X9_CAM_SMI_CLOCKS_RETAINED;
    else s->flags&=~X9_CAM_SMI_CLOCKS_RETAINED;
}
static int cam_smi_fault(int e)
{if(!cam_smi.error)cam_smi.error=e;cam_smi_status();return e;}
/* Nonrecursive, shared by receiver/PHY/AP guards. No unsafe supplier or
 * larb reads until the MM power authority and actual parent are proven. */
static int cam_smi_parent_guard(void)
{
    uint32_t cfg;
    int e=cam_mm_guard(&cam_mm,true);if(e)return e;
    /* Android clk-mux.c waits for CKSTA busy, never refuses. mm_mminfra_sel
     * (CFG_0[19:16]) is the RC SMI mux DVFS'd by MMPC XPU12 / VCP XPU3
     * (mmdvfs-mt6993.h); Android never pins its parent. Adopt and log. */
    e=cam_wait_bits(cam_mm.top,0x330,1u<<29,0,10000);if(e)return e;
    cfg=(cam_rr(cam_mm.top,0x10)>>16)&0xff;
    if(cfg!=cam_smi.parent_cfg) {
        if(cam_smi.held)
            printf("X9 CAMERA SMI MMINFRA ADOPT: parent cfg %x -> %x (MMPC/VCP DVFS, not refused)\n",
                cam_smi.parent_cfg,cfg);
        cam_smi.parent_cfg=cfg;
    }
    if(!(cam_rr(cam_mm.top,0x270)&(1u<<29)))return CAM_FAIL(ENXIO);
    return 0;
}
/* Android hwccf_api.c _v1_mm_hwccf_voter_ctrl: before a SET, poll
 * CCF_STATUS[15:0]<=1 (0 = an IRQ voter, e.g. VCP/MMUP, in flight),
 * MTK_WAIT_GHWV_DONE_CNT=10000 x 1us. */
static int cam_smi_ccf_idle(void)
{
    uint32_t v=0;unsigned i;
    for(i=0;i<10000;i++) {
        v=cam_rr(cam_mm.receive.hwv,0x10000);
        if((v&0xffff)<=1)return 0;
        delay(1);
    }
    printf("X9 CAMERA SMI CCF_STATUS: vote_idle timeout status=%x\n",v);
    return CAM_FAIL(ETIMEDOUT);
}
static int cam_smi_voter_poll(unsigned slot,const char *step,unsigned offset,
    uint32_t target,unsigned timeout_us)
{
    const struct cam_smi_gate *gate=&cam_smi.gate[slot];
    struct cam_poll_timeout timeout={0};
    int e=cam_poll_deadline(cam_mm.receive.hwv,offset,gate->mask,target,1,timeout_us,&timeout);

    if(e)printf("X9 CAMERA SMI VOTER: slot=%u step=%s errno=%d "
        "timeout_off=%x timeout_raw=%x mask=%x target=%x\n",
        slot,step,e,timeout.offset,timeout.value,gate->mask,target);
    return e;
}
static int cam_smi_slot_idle(unsigned n,const char *step)
{
    const struct cam_smi_gate *g=&cam_smi.gate[n];
    int e=cam_smi_parent_guard();if(e)return e;
    e=cam_smi_voter_poll(n,step,g->sta,0,CAM_MM_VOTER_STATE_TIMEOUT_US);
    return e?e:cam_smi_parent_guard();
}
static void cam_smi_sample(unsigned n,uint32_t v[3])
{
    const struct cam_smi_gate *g=&cam_smi.gate[n];
    v[0]=cam_rr(cam_mm.receive.hwv,g->set+8)&g->mask;
    v[1]=cam_rr(cam_mm.receive.hwv,g->global)&g->mask;
    v[2]=cam_rr(g->cg,0)&g->mask;
    cam_smi.status.ap[n]=v[0];cam_smi.status.global[n]=v[1];cam_smi.status.gate[n]=v[2];
}
static int cam_smi_clock_only_guard(void)
{
    int e;
    if(!cam_smi.held)return 0;
    if(cam_smi.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)
        return CAM_FAIL(ESTALE);
    e=cam_smi_parent_guard();if(e)return e;
    e=cam_smi_shared_voter_guard();if(e)return e;
    for(unsigned n=0;n<CAM_SMI_SLOTS;n++)if(cam_smi.seen&(1u<<n)) {
        const struct cam_smi_gate *g=&cam_smi.gate[n];uint32_t v[3];
        e=cam_smi_slot_idle(n,"guard-sta");if(e)return e;
        cam_smi_sample(n,v);
        if(v[0]!=g->ap_expected && !(cam_smi.pending==CAM_SMI_SET&&cam_smi.pending_slot==n&&
            v[0]==g->ap_before))return CAM_FAIL(ESTALE);
        if((v[1]&g->borrowed)!=g->borrowed || (v[2]&g->borrowed))return CAM_FAIL(ESTALE);
        if(cam_smi.completed&(1u<<n)) {
            if(v[1]!=g->mask || v[2])return CAM_FAIL(ESTALE);
        } else {
            /* A SET may automatically ungate before normal CG_CLR. No
             * newly gated bit is permitted, even on a partial admission. */
            if(v[2]&~g->cg_before)return CAM_FAIL(ESTALE);
            if(!cam_smi.pending && v[2]!=g->cg_expected)return CAM_FAIL(ESTALE);
        }
    }
    return 0;
}
static int cam_smi_clock_guard(void)
{
    int e=cam_smi_clock_only_guard();
    if(!e)e=cam_smi_shared_control_guard(false);
    if(!e)e=cam_smi_a0_guard();
    return e?e:cam_native_bw_guard();
}
/* Android hwccf_api.c _v1_mm_hwccf_mtcmos_fsm_err_handle(), called by
 * clk-gate.c mtk_cg_enable_generic_hwv() after the raw vote of every MM
 * generic HWV gate: if the MTCMOS mapped to this CG group (INIT_CCF_CG_MAP_
 * MTCMOS 0x15a0) has PM_ACK clear, pulse XPU0's CG47 vote to retrigger the
 * FSM and wait for PM_ACK. Return 0x100a = bypass (like Android). */
static int cam_smi_fsm_retrigger(unsigned rid)
{
    vm_offset_t b=cam_mm.receive.hwv;
    uint32_t map=cam_rr(b,0x15a0+(rid/4)*4),id=(map>>(7*(rid%4)))&0x7f,ack_ofs,m;
    int e;
    if(id>=64)return 0x100a;
    ack_ofs=id<32?0x12900:0x12904;m=id%32;
    if(cam_rr(b,ack_ofs)&(1u<<m))return 0x100a;
    printf("X9 CAMERA SMI FSM RETRIGGER: rid=%u mtcmos=%u ack_ofs=%x ack=%x (CG47 pulse as Android)\n",
        rid,id,ack_ofs,cam_rr(b,ack_ofs));
    cam_rw(b,0x20000+47*0xc,1);
    e=cam_wait_bits(b,0x12200+47*4,1,1,200);if(e)return e;
    e=cam_wait_bits(b,0x11800+47*4,1,0,10000);if(e)return e;
    cam_rw(b,0x20004+47*0xc,1);
    e=cam_wait_bits(b,0x12200+47*4,1,0,200);if(e)return e;
    e=cam_wait_bits(b,0x11800+47*4,1,0,10000);if(e)return e;
    return cam_wait_bits(b,ack_ofs,1u<<m,1u<<m,200);
}
static int cam_smi_slot_take(unsigned n)
{
    struct cam_smi_gate *g=&cam_smi.gate[n];uint32_t a[3],b[3];
    int e=cam_smi_clock_guard();if(e)return e;
    e=cam_smi_slot_idle(n,"pre-sta");if(e)return e;
    cam_smi_sample(n,a);delay(10);e=cam_smi_slot_idle(n,"pre-sta");if(e)return e;
    cam_smi_sample(n,b);
    if(memcmp(a,b,sizeof(a))) {
        printf("X9 CAMERA SMI SLOT %u ADOPT: foreign voter moved ap=%x/%x global=%x/%x cg=%x/%x (latest used)\n",
            n,a[0],b[0],a[1],b[1],a[2],b[2]);
        memcpy(a,b,sizeof(a));
    }
    /* Android generic HWV gate never refuses voted-but-gated: it votes,
     * retriggers the MTCMOS FSM and waits the gate (clk-gate.c). */
    if(a[2]&a[1]) {
        int r;
        printf("X9 CAMERA SMI SLOT %u INHERITED: voted-but-gated ap=%x global=%x cg=%x (FSM retrigger+wait as Android)\n",
            n,a[0],a[1],a[2]);
        r=cam_smi_fsm_retrigger(n==0?35u:34u);
        if(r && r!=0x100a)return r;
        e=cam_wait_bits(g->cg,0,a[1]&g->mask,0,100000);if(e)return e;
        e=cam_smi_slot_idle(n,"inherit-sta");if(e)return e;
        cam_smi_sample(n,a);
    }
    /* Voted-but-gated is repaired above. An unvoted-but-ungated clock (direct
     * owner, e.g. the now-running VCP/MMUP mmqos) is NOT refused: Android's
     * clk/HWV path just adds its own vote; our later CLR leaves the gate to
     * the HWV aggregate. Logged so a shared clock is visible. */
    if((a[0]&~a[1]) || (a[2]&a[1]))return CAM_FAIL(EBUSY);
    if((~a[2]&~a[1])&g->mask)
        printf("X9 CAMERA SMI SLOT %u: clock ungated without vote ap=%x global=%x cg=%x (shared, not refused)\n",
            n,a[0],a[1],a[2]);
    g->ap_before=g->ap_expected=a[0];g->borrowed=a[0];
    g->cg_before=g->cg_expected=a[2];cam_smi.seen|=1u<<n;
    g->owned=g->mask&~a[0];
    if(g->owned) {
        e=cam_smi_clock_guard();if(e)return e;
        e=cam_smi_ccf_idle();if(e)return e;
        cam_smi.pending_slot=n;cam_smi.pending=CAM_SMI_SET;
        g->ap_expected=g->mask; /* mark BEFORE the posted SET */
        cam_rw(cam_mm.receive.hwv,g->set,g->owned);
        /* CG34/35 use the v1 raw MM voter: AP EN, global EN, then STA. */
        e=cam_smi_voter_poll(n,"set-ap-en",g->set+8,g->mask,CAM_MM_RAW_EN_TIMEOUT_US);
        if(!e)e=cam_smi_voter_poll(n,"global-en",g->global,g->mask,CAM_MM_VOTER_STATE_TIMEOUT_US);
        if(!e)e=cam_smi_slot_idle(n,"set-final-sta");
        if(e)return e;
        cam_smi.pending=CAM_SMI_NONE;
        {   /* Android generic MM HWV gate: FSM retrigger, then CG enable. */
            int r=cam_smi_fsm_retrigger(n==0?35u:34u);
            if(r && r!=0x100a)return r;
        }
        cam_smi_sample(n,b);if(b[2]&~g->cg_before)return CAM_FAIL(ESTALE);
        g->cg_expected=b[2];
        if(b[2]) {
            e=cam_smi_clock_guard();if(e)return e;
            cam_smi.pending=CAM_SMI_CG_CLR;g->cg_expected=0;
            cam_rw(g->cg,8,b[2]);
            /* Android __hwv_cg_dma_back: MTK_WAIT_HWV_DONE_CNT=5000000 x 1us. */
            e=cam_wait_bits(g->cg,0,g->mask,0,100000);if(e)return e;
            cam_smi.pending=CAM_SMI_NONE;
        }
    }
    cam_smi_sample(n,b);
    if(b[0]!=g->mask || b[1]!=g->mask || b[2])return CAM_FAIL(ESTALE);
    cam_smi.completed|=1u<<n;return cam_smi_clock_guard();
}
/* Pending control retirement requires observed TARGET and voter idle.
 * Prior AP/CG levels alone cannot prove an issued SET/CLR was consumed.
 * Never replay the failed request, reset or cancel it at close. */
static int cam_smi_pending_retire(void)
{
    struct cam_smi_gate *g;uint32_t v[3];int e;
    if(!cam_smi.pending)return 0;
    if(cam_smi.pending==CAM_SMI_CLR) {
        /* A failed close may be retried: an HWV CLR is idempotent. */
        printf("X9 CAMERA SMI CLOSE RETRY: slot=%u pending CLR re-armed\n",cam_smi.pending_slot);
        cam_smi.pending=CAM_SMI_NONE;return 0;
    }
    g=&cam_smi.gate[cam_smi.pending_slot];
    e=cam_smi_slot_idle(cam_smi.pending_slot,"retire-sta");if(e)return e;
    cam_smi_sample(cam_smi.pending_slot,v);
    if(v[0]!=g->ap_expected || v[1]!=g->mask)return CAM_FAIL(EBUSY);
    if(cam_smi.pending==CAM_SMI_SET) {
        if(v[1]!=g->mask || (v[2]&~g->cg_before))return CAM_FAIL(EBUSY);
        g->cg_expected=v[2];
    } else if(v[2]!=g->cg_expected)return CAM_FAIL(EBUSY);
    cam_smi.pending=CAM_SMI_NONE;return cam_smi_clock_guard();
}
static int cam_smi_larb_idle(void)
{
    int e;
    /* Plain RO STAT0/internal STAT500, not IRQ/R-C history. A zero sample
     * is only a necessary clock-unvote check, NOT full AXI/cache/worker
     * drain. This lifetime never enables a producer or posts DMA/CQ. */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_smi_clock_guard();if(e)return e;
        for(unsigned n=0;n<3;n++) {
            unsigned slot=1;uint32_t bit=n==0?4:n==1?2:32;
            const struct cam_smi_gate *g=&cam_smi.gate[slot];
            if(!(cam_smi.seen&(1u<<slot)))continue;
            /* Never read a still gated larb after a failed partial take. */
            if(cam_rr(g->cg,0)&bit)continue;
            cam_smi.status.larb_busy[n][0]=cam_rr(cam_smi.larb[n],0);
            cam_smi.status.larb_busy[n][1]=cam_rr(cam_smi.larb[n],0x500);
            if(cam_smi.status.larb_busy[n][0] || cam_smi.status.larb_busy[n][1])return CAM_FAIL(EBUSY);
        }
        delay(10);
    }
    return 0;
}
static int cam_smi_clocks_close_locked(uint64_t epoch)
{
    int e;
    if(!cam_smi.held)return 0;
    if(epoch!=cam_smi.epoch)return CAM_FAIL(ESTALE);
    if(cam_smi.close_attempted)
        printf("X9 CAMERA SMI CLOSE RETRY: epoch=%llu last_errno=%d (non-wedging)\n",epoch,cam_smi.error);
    cam_smi.close_attempted=true;
    e=cam_sv_qos_release_guard(epoch);
    if(!e)e=cam_smi_pending_retire();if(!e)e=cam_smi_clock_only_guard();
    /* Port controls retire while BOTH supplier and consumer clocks remain
     * held. A pending A0 write is qualified by its own sole close below.
     * Native AP budget withdraw precedes A0 restore. Port idle here is only
     * the SMI-side condition, not a DMA grant/full capture retirement:
     * upstream mandatory engine authority must qualify its actual retire
     * scope before it permits this sole supplier close. */
    if(!e)e=cam_smi_a0_pending_retire();
    if(!e)e=cam_native_bw_close_locked();
    if(!e)e=cam_smi_a0_close_locked();
    if(!e)e=cam_smi_shared_close_locked();
    if(!e)e=cam_smi_clock_guard();
    if(!e)e=cam_rx_session_guard();
    if(!e)e=cam_smi_larb_idle();
    for(unsigned i=CAM_SMI_SLOTS;i&&!e;i--) {
        unsigned n=i-1;struct cam_smi_gate *g=&cam_smi.gate[n];uint32_t v[3];
        if(!(cam_smi.seen&(1u<<n)))continue;
        e=cam_smi_clock_guard();if(e)break;
        if(g->owned) {
            cam_smi.pending_slot=n;cam_smi.pending=CAM_SMI_CLR;
            g->ap_expected=g->ap_before;
            cam_rw(cam_mm.receive.hwv,g->set+4,g->owned);
            e=cam_smi_voter_poll(n,"clr-ap-en",g->set+8,g->ap_before,CAM_MM_RAW_EN_TIMEOUT_US);
            if(!e)e=cam_smi_slot_idle(n,"clr-final-sta");
            if(e)break;
            cam_smi_sample(n,v);
            /* Android unvote only drops our own vote; another XPU (VCP/MMUP)
             * may keep the clock on. Only wait bits nobody votes. */
            if(g->cg_before&v[1])
                printf("X9 CAMERA SMI CLOSE INHERITED: slot=%u foreign vote keeps clock global=%x cg_before=%x\n",
                    n,v[1],g->cg_before);
            e=cam_wait_bits(g->cg,0,g->cg_before&~v[1],g->cg_before&~v[1],10000);if(e)break;
            cam_smi_sample(n,v);
            if((v[1]&g->borrowed)!=g->borrowed || (v[2]&g->borrowed))
                printf("X9 CAMERA SMI CLOSE INHERITED: slot=%u borrowed=%x global=%x cg=%x (foreign owner changed)\n",
                    n,g->borrowed,v[1],v[2]);
            cam_smi.pending=CAM_SMI_NONE;g->owned=0;
        }
        cam_smi.completed&=~(1u<<n);cam_smi.seen&=~(1u<<n);g->borrowed=0;
    }
    if(!e) {cam_smi.held=false;cam_smi.epoch=0;cam_smi.status.flags|=X9_CAM_SMI_CLOCKS_RESTORED;}
    else cam_smi_fault(e);
    cam_smi.status.restore_errno=e;cam_smi_status();
    printf("X9 CAMERA SMI CLOCK CLOSE: sensor=%u flags=%x errno=%u restore=%d pending=%u seen=%x "
        "a0=%x/%x/%u shared=%x/%x/%u (inactive control/clock restoration; NOT SMI/AXI/worker drain)\n",
        cam_smi.status.sensor,cam_smi.status.flags,cam_smi.status.last_errno,e,
        cam_smi.pending,cam_smi.seen,cam_smi.status.a0.flags,cam_smi.status.a0.owned,
        cam_smi.status.a0.pending,cam_smi.status.shared.flags,cam_smi.status.shared.owned,
        cam_smi.status.shared.pending);
    return e;
}
static int cam_smi_clocks_open_locked(uint64_t epoch,unsigned sensor)
{
    static vm_offset_t cg[2],larb[3];
    static const uint64_t larb_pa[3]={0x3a01b000,0x3a01a000,0x3a01e000};
    int e=cam_rx_session_guard();if(e)return e;
    if(cam_smi.held)return CAM_FAIL(EALREADY);
    if(!cg[0])cg[0]=ml_io_map(0x3c80d000,0x1000);
    if(!cg[1])cg[1]=ml_io_map(0x3a680000,0x1000);
    for(unsigned n=0;n<3;n++)if(!larb[n])larb[n]=ml_io_map(larb_pa[n],0x1000);
    if(!cg[0]||!cg[1]||!larb[0]||!larb[1]||!larb[2])return CAM_FAIL(ENOMEM);
    memset(&cam_smi,0,sizeof(cam_smi));cam_smi.epoch=epoch;
    cam_smi.parent_cfg=(cam_rr(cam_mm.top,0x10)>>16)&0xff;
    /* CG35 R1A subcomm1/2/3 bits13/14/15, CG34 CAM_MRAW larb14/19/29
     * bits1/2/5. Real CG35 is 3c80d000, NOT adjacent CCU 3c800000. */
    cam_smi.gate[0]=(struct cam_smi_gate){.set=0x201a4,.global=0x1228c,.sta=0x1188c,
        .mask=7u<<13,.cg=cg[0]};
    cam_smi.gate[1]=(struct cam_smi_gate){.set=0x20198,.global=0x12288,.sta=0x11888,
        .mask=0x26,.cg=cg[1]};
    memcpy(cam_smi.larb,larb,sizeof(larb));
    cam_smi.status.sensor_epoch=epoch;cam_smi.status.sensor=sensor;
    cam_smi.status.flags=X9_CAM_SMI_CLOCKS_ATTEMPTED;
    cam_smi.held=true;cam_smi.status.stage=1;
    e=cam_smi_parent_guard();
    if(!e)e=cam_smi_shared_capture_locked();
    for(unsigned n=0;n<CAM_SMI_SLOTS&&!e;n++) {
        cam_smi.status.stage=2+n;e=cam_smi_slot_take(n);
    }
    if(!e)e=cam_smi_larb_idle();
    if(!e) {cam_smi.status.stage=4;cam_smi.status.flags|=X9_CAM_SMI_CLOCKS_CONFIGURED;}
    if(!e) {cam_smi.status.stage=5;e=cam_smi_shared_open_locked();}
    if(!e) {cam_smi.status.stage=6;e=cam_smi_a0_open_locked();}
    if(!e)cam_smi.status.stage=7;
    else cam_smi_fault(e);
    cam_smi_status();return e;
}
#include "x9_camera_smi_shared.inc"
#include "x9_camera_smi_a0.inc"
#include "x9_camera_native_bw.inc"
static int cam_smi_retirement_guard(uint64_t epoch)
{
    int e;
    if(!cam_smi.held)return 0;
    if(epoch!=cam_smi.epoch)return CAM_FAIL(ESTALE);
    /* First qualify posted controls at their actual TARGET while every
     * dependent component is still powered. This is not DMA drain. */
    e=cam_smi_pending_retire();
    if(!e)e=cam_smi_shared_retirement_guard(epoch);
    if(!e)e=cam_smi_a0_pending_retire();
    if(!e)e=cam_native_bw_pending_retire();
    return e?cam_smi_fault(e):cam_smi_clock_guard();
}
int x9_camera_sensor_session_prepare_smi_clocks(uint64_t epoch,struct x9_camera_smi_clock_status *s)
{
    struct cam_sensor_ap ap;uint8_t byte=0;int e;
    if(!s)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_control_lock);memset(s,0,sizeof(*s));s->sensor_epoch=epoch;
    e=cam_sensor_session_match(epoch);
    if(!e&&cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e&&(cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED||session.capture||session.stream))e=CAM_FAIL(EBUSY);
    if(!e&&!cam_rx_sensor_held_locked(epoch))e=CAM_FAIL(ENXIO);
    if(!e&&cam_smi.held) {
        if(cam_smi.epoch!=epoch && !cam_mac.held && !cam_phy.held && !cam_out.held) {
            printf("X9 CAMERA SMI STALE ADOPT: abandoning epoch=%llu state errno=%d shared=%u a0=%u bw=%u (HW kept; Android resume reconfigures)\n",
                cam_smi.epoch,cam_smi.error,cam_smi_shared.captured,cam_smi_a0.held,cam_native_bw.held);
            memset(&cam_smi,0,sizeof(cam_smi));memset(&cam_smi_shared,0,sizeof(cam_smi_shared));
            memset(&cam_smi_a0,0,sizeof(cam_smi_a0));memset(&cam_native_bw,0,sizeof(cam_native_bw));
        } else e=CAM_FAIL(EALREADY);
    }
    if(!e&&(cam_mac.held||cam_phy.held||cam_out.held))e=CAM_FAIL(EBUSY);
    if(!e) {
        s->sensor=cam_sensor_live.probe.sensor;
        ap=(struct cam_sensor_ap){&cam_journal.sensor,cam_sensor_slave(s->sensor)};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e&&byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e&&byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_smi_clocks_open_locked(epoch,s->sensor);
        if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e&&byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e&&byte)e=CAM_FAIL(EBUSY);
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e) {
            if(cam_smi.held) {cam_smi_fault(e);cam_rx_session_fault(e);}
            cam_sensor_session_fault(e);
        }
    }
    if(cam_smi.held&&cam_smi.epoch==epoch)*s=cam_smi.status;
    s->last_errno=e;lck_mtx_unlock(&camera_control_lock);return e;
}
