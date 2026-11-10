/* MT6993 CAMSV id0's real supplier/clock graph. Live DT larbs19/14/29:
 * 19 -> cam subcomm3 -> disp common0 (skip-rpm-cb, no clocks);
 * 14 -> cam subcomm1 -> subcomm1_1 -> disp common0;
 * 29 -> cam subcomm2 -> mdp common1 (skip-rpm-cb, no clocks).
 * subcomm1 and 1_1 share the SAME physical R1A bit13; take it only once.
 * Upstream mtk_smi_bus_enable recursively enables suppliers first. Its
 * config_port/common QoS/routing writes are NOT implied by these clocks.
 * Never write VCP/display skip-rpm suppliers, protected/nonsec ports,
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
    cfg=(cam_rr(cam_mm.top,0x10)>>16)&0xff;
    if(cfg!=cam_smi.parent_cfg)return ESTALE;
    if(cam_rr(cam_mm.top,0x330)&(1u<<29))return EBUSY;
    if(!(cam_rr(cam_mm.top,0x270)&(1u<<29)))return ENXIO;
    /* MMINFRA is an AL mux; no AP SET/parent/PDN/PLL manipulation. The
     * MM/secure reference holds its power, FENC is still an observation,
     * not a throughput or continuous rate lease. */
    return 0;
}
static int cam_smi_slot_idle(unsigned n)
{
    const struct cam_smi_gate *g=&cam_smi.gate[n];
    int e=cam_smi_parent_guard();if(e)return e;
    e=cam_wait_bits(cam_mm.receive.hwv,g->sta,g->mask,0,10000);
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
static int cam_smi_clock_guard(void)
{
    int e;
    if(!cam_smi.held)return 0;
    if(cam_smi.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)
        return ESTALE;
    e=cam_smi_parent_guard();if(e)return e;
    for(unsigned n=0;n<CAM_SMI_SLOTS;n++)if(cam_smi.seen&(1u<<n)) {
        const struct cam_smi_gate *g=&cam_smi.gate[n];uint32_t v[3];
        e=cam_smi_slot_idle(n);if(e)return e;
        cam_smi_sample(n,v);
        if(v[0]!=g->ap_expected && !(cam_smi.pending==CAM_SMI_SET&&cam_smi.pending_slot==n&&
            v[0]==g->ap_before))return ESTALE;
        if((v[1]&g->borrowed)!=g->borrowed || (v[2]&g->borrowed))return ESTALE;
        if(cam_smi.completed&(1u<<n)) {
            if(v[1]!=g->mask || v[2])return ESTALE;
        } else {
            /* A SET may automatically ungate before normal CG_CLR. No
             * newly gated bit is permitted, even on a partial admission. */
            if(v[2]&~g->cg_before)return ESTALE;
            if(!cam_smi.pending && v[2]!=g->cg_expected)return ESTALE;
        }
    }
    return 0;
}
static int cam_smi_slot_take(unsigned n)
{
    struct cam_smi_gate *g=&cam_smi.gate[n];uint32_t a[3],b[3];
    int e=cam_smi_clock_guard();if(e)return e;
    e=cam_smi_slot_idle(n);if(e)return e;
    cam_smi_sample(n,a);delay(10);e=cam_smi_slot_idle(n);if(e)return e;
    cam_smi_sample(n,b);if(memcmp(a,b,sizeof(a)))return ESTALE;
    /* No voted-but-gated repair, or unvoted direct-clock owner takeover. */
    if((a[0]&~a[1]) || (a[2]&a[1]) || ((~a[2]&~a[1])&g->mask))return EBUSY;
    g->ap_before=g->ap_expected=a[0];g->borrowed=a[0];
    g->cg_before=g->cg_expected=a[2];cam_smi.seen|=1u<<n;
    g->owned=g->mask&~a[0];
    if(g->owned) {
        e=cam_smi_clock_guard();if(e)return e;
        cam_smi.pending_slot=n;cam_smi.pending=CAM_SMI_SET;
        g->ap_expected=g->mask; /* mark BEFORE the posted SET */
        cam_rw(cam_mm.receive.hwv,g->set,g->owned);
        e=cam_wait_bits(cam_mm.receive.hwv,g->set+8,g->mask,g->mask,200);
        if(!e)e=cam_smi_slot_idle(n);
        if(!e)e=cam_wait_bits(cam_mm.receive.hwv,g->global,g->mask,g->mask,10000);
        if(e)return e;
        cam_smi.pending=CAM_SMI_NONE;
        cam_smi_sample(n,b);if(b[2]&~g->cg_before)return ESTALE;
        g->cg_expected=b[2];
        if(b[2]) {
            e=cam_smi_clock_guard();if(e)return e;
            cam_smi.pending=CAM_SMI_CG_CLR;g->cg_expected=0;
            cam_rw(g->cg,8,b[2]);
            e=cam_wait_bits(g->cg,0,g->mask,0,10000);if(e)return e;
            cam_smi.pending=CAM_SMI_NONE;
        }
    }
    cam_smi_sample(n,b);
    if(b[0]!=g->mask || b[1]!=g->mask || b[2])return ESTALE;
    cam_smi.completed|=1u<<n;return cam_smi_clock_guard();
}
/* Pending control retirement requires observed TARGET and voter idle.
 * Prior AP/CG levels alone cannot prove an issued SET/CLR was consumed.
 * Never replay the failed request, reset or cancel it at close. */
static int cam_smi_pending_retire(void)
{
    struct cam_smi_gate *g;uint32_t v[3];int e;
    if(!cam_smi.pending)return 0;
    if(cam_smi.pending==CAM_SMI_CLR)return EBUSY; /* sole close already failed */
    g=&cam_smi.gate[cam_smi.pending_slot];
    e=cam_smi_slot_idle(cam_smi.pending_slot);if(e)return e;
    cam_smi_sample(cam_smi.pending_slot,v);
    if(v[0]!=g->ap_expected || v[1]!=g->mask)return EBUSY;
    if(cam_smi.pending==CAM_SMI_SET) {
        if(v[1]!=g->mask || (v[2]&~g->cg_before))return EBUSY;
        g->cg_expected=v[2];
    } else if(v[2]!=g->cg_expected)return EBUSY;
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
            if(cam_smi.status.larb_busy[n][0] || cam_smi.status.larb_busy[n][1])return EBUSY;
        }
        delay(10);
    }
    return 0;
}
static int cam_smi_clocks_close_locked(uint64_t epoch)
{
    int e;
    if(!cam_smi.held)return 0;
    if(epoch!=cam_smi.epoch || cam_smi.close_attempted)return ESTALE;
    cam_smi.close_attempted=true;
    e=cam_smi_pending_retire();if(!e)e=cam_smi_clock_guard();
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
            e=cam_wait_bits(cam_mm.receive.hwv,g->set+8,g->mask,g->ap_before,200);
            if(!e)e=cam_smi_slot_idle(n);
            if(e)break;
            cam_smi_sample(n,v);
            if(g->cg_before&v[1]) {e=ESTALE;break;} /* a new owner keeps dependencies */
            e=cam_wait_bits(g->cg,0,g->cg_before,g->cg_before,10000);if(e)break;
            cam_smi_sample(n,v);
            if((v[1]&g->borrowed)!=g->borrowed || (v[2]&g->borrowed)) {e=ESTALE;break;}
            cam_smi.pending=CAM_SMI_NONE;g->owned=0;
        }
        cam_smi.completed&=~(1u<<n);cam_smi.seen&=~(1u<<n);g->borrowed=0;
    }
    if(!e) {cam_smi.held=false;cam_smi.epoch=0;cam_smi.status.flags|=X9_CAM_SMI_CLOCKS_RESTORED;}
    else cam_smi_fault(e);
    cam_smi.status.restore_errno=e;cam_smi_status();
    printf("X9 CAMERA SMI CLOCK CLOSE: sensor=%u flags=%x errno=%u restore=%d pending=%u seen=%x "
        "(clock restoration only; NOT SMI/AXI/worker drain)\n",cam_smi.status.sensor,
        cam_smi.status.flags,cam_smi.status.last_errno,e,cam_smi.pending,cam_smi.seen);
    return e;
}
static int cam_smi_clocks_open_locked(uint64_t epoch,unsigned sensor)
{
    static vm_offset_t cg[2],larb[3];
    static const uint64_t larb_pa[3]={0x3a01b000,0x3a01a000,0x3a01e000};
    int e=cam_rx_session_guard();if(e)return e;
    if(cam_smi.held)return EALREADY;
    if(!cg[0])cg[0]=ml_io_map(0x3c80d000,0x1000);
    if(!cg[1])cg[1]=ml_io_map(0x3a680000,0x1000);
    for(unsigned n=0;n<3;n++)if(!larb[n])larb[n]=ml_io_map(larb_pa[n],0x1000);
    if(!cg[0]||!cg[1]||!larb[0]||!larb[1]||!larb[2])return ENOMEM;
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
    for(unsigned n=0;n<CAM_SMI_SLOTS&&!e;n++) {
        cam_smi.status.stage=2+n;e=cam_smi_slot_take(n);
    }
    if(!e)e=cam_smi_larb_idle();
    if(!e) {cam_smi.status.stage=4;cam_smi.status.flags|=X9_CAM_SMI_CLOCKS_CONFIGURED;}
    else cam_smi_fault(e);
    cam_smi_status();return e;
}
int x9_camera_sensor_session_prepare_smi_clocks(uint64_t epoch,struct x9_camera_smi_clock_status *s)
{
    struct cam_sensor_ap ap;uint8_t byte=0;int e;
    if(!s)return EINVAL;
    lck_mtx_lock(&camera_control_lock);memset(s,0,sizeof(*s));s->sensor_epoch=epoch;
    e=cam_sensor_session_match(epoch);
    if(!e&&cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e&&(cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED||session.capture||session.stream))e=EBUSY;
    if(!e&&!cam_rx_sensor_held_locked(epoch))e=ENXIO;
    if(!e&&cam_smi.held)e=EALREADY;
    if(!e&&(cam_mac.held||cam_phy.held||cam_out.held))e=EBUSY;
    if(!e) {
        s->sensor=cam_sensor_live.probe.sensor;
        ap=(struct cam_sensor_ap){&cam_journal.sensor,s->sensor==X9_CAMERA_BACK?0x1a:0x10};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e&&byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e&&byte)e=EBUSY;
        if(!e)e=cam_smi_clocks_open_locked(epoch,s->sensor);
        if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e&&byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e&&byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e) {
            if(cam_smi.held) {cam_smi_fault(e);cam_rx_session_fault(e);}
            cam_sensor_session_fault(e);
        }
    }
    if(cam_smi.held&&cam_smi.epoch==epoch)*s=cam_smi.status;
    s->last_errno=e;lck_mtx_unlock(&camera_control_lock);return e;
}
