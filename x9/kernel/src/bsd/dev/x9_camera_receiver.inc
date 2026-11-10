/* MT6993/ISP8s powered register lifetime, camera_control_lock held.
 * Captured DT reg-names, not old unit-test aliases:
 * outmux0 outer 3a200000 / inner 3a208000; CAMSV id0 3a600000,
 * DMA +1000 / CQ +2000 / inner +8000. CSI MAC-top is port base+4000.
 * Only documented configuration CSR reads. Do NOT read interrupt/status
 * banks: upstream enables read-clear on several of them. Probe/prepare does
 * not write receiver CSR. The separately requested OUTMUX engine below
 * journals real shadow/commit/restore and inactive CAMSV RAW10 configuration;
 * no IRQ/all-port setup or PHY guesses.
 * DISABLED is not ownership, queue drain or a mapping lease.
 */
_Static_assert(sizeof(struct x9_camera_receiver)==1192,"camera receiver ABI");
static struct x9_camera_receiver cam_rx_last={.version=1};
static bool cam_rx_attempted;
static int cam_out_guard(void);
static int cam_out_close_locked(uint64_t);
static int cam_mac_guard(void);
static int cam_mac_close_locked(uint64_t);
static int cam_phy_guard(void);
static int cam_phy_retirement_guard(uint64_t);
static int cam_phy_close_locked(uint64_t);
static int cam_smi_clock_guard(void);
static int cam_smi_clocks_close_locked(uint64_t);
static int cam_smi_retirement_guard(uint64_t);
static int cam_sv_guard(void);
static uint32_t cam_sv_fbc_busy_exemption(void);
static uint32_t cam_sv_fbc_compare_word(unsigned,uint32_t);
static uint32_t cam_sv_receive_busy_exemption(unsigned);
static uint32_t cam_sv_receive_compare_word(unsigned,uint32_t);
static int cam_sv_receive_sample_word(unsigned,vm_offset_t,uint32_t);
static int cam_sv_open_locked(uint64_t);
static int cam_sv_close_locked(uint64_t);
static void cam_sv_status_copy(uint64_t,struct x9_camera_sv_hw_status *);
static int cam_irq_prepare_locked(uint64_t);
static int cam_irq_guard_locked(uint64_t);
static int cam_irq_close_locked(uint64_t);
static int cam_irq_credit_guard_locked(uint64_t);
static const unsigned cam_rx_out_offsets[X9_CAM_RX_OUT_WORDS]={
    0x000,0x004,0x008,0x00c,0x010,0x014,0x018,0x020,
    0x02c,0x034,0x038,0x100,0x104,0x108
};
static const unsigned cam_rx_sv_offsets[X9_CAM_RX_SV_WORDS]={
    0x040,0x044,0x140,0x148,
    0x540,0x580,0x5c0,0x600,0x640,0x680,0x6c0,0x700,
    0x1200,0x1204,0x1210,0x2100,0x2140
};
struct cam_rx_maps { vm_offset_t mac,async,out[2],sv[2]; };
static struct cam_rx_maps cam_rx_maps[2];
static struct {
    uint64_t epoch, sensor_epoch;
    bool close_attempted;
    int error;
    struct x9_camera_receiver status;
    struct x9_camera_receiver_sample expected;
} cam_rx_live;
static int cam_rx_sample(const struct cam_rx_maps *m,struct x9_camera_receiver_sample *s)
{
    static const unsigned mac_offsets[4]={0,4,0x40,0x70};
    /* Re-prove power/clock effects before EACH sample. No probe on a failed
     * MM take, and no unsafe read after a changed parent or lost PM_ACK. */
    int e=cam_mm_guard(&cam_mm,true);if(e)return e;
    e=cam_smi_clock_guard();if(e)return e;
    for(unsigned i=0;i<4;i++)s->mac[i]=cam_rr(m->mac,mac_offsets[i]);
    s->async_cfg=cam_rr(m->async,0);s->async_split=cam_rr(m->async,0xc);
    s->afifo=cam_rr(cam_mm.cam_cg,0x7dc);
    for(unsigned bank=0;bank<2;bank++) {
        for(unsigned i=0;i<X9_CAM_RX_OUT_WORDS;i++)
            s->out[bank][i]=cam_rr(m->out[bank],cam_rx_out_offsets[i]);
        for(unsigned i=0;i<X9_CAM_RX_SV_WORDS;i++)
            s->sv[bank][i]=cam_rr(m->sv[bank],cam_rx_sv_offsets[i]);
        /* Record actual DMA-enable observations BEFORE the next sample,
         * not while comparing cached/prior evidence. A seen target cannot
         * regress even if another read later returns to the target again. */
        e=cam_sv_receive_sample_word(bank,m->sv[bank],s->sv[bank][0]);if(e)return e;
    }
    return 0;
}
static uint32_t cam_rx_busy(const struct x9_camera_receiver_sample *s,unsigned port,uint32_t fbc0_mask,
    uint32_t dma0_mask)
{
    uint32_t busy=0;
    /* PHY receiver enable or packet-pattern source, not an analog power
     * assertion. Do not attempt to stop/reset an inherited receiver. */
    if((s->mac[2]&0xf003) || (s->mac[3]&5))busy|=1;
    if(s->async_cfg&(1u<<port))busy|=2; /* test-model source */
    if(s->mac[0]&1)busy|=32;
    for(unsigned bank=0;bank<2;bank++) {
        const uint32_t *out=s->out[bank],*sv=s->sv[bank];
        if((out[7]&(1u<<12)) || (out[8]&1) || (out[10]&1))busy|=4;
        if(out[9]&1)busy|=32;
        if((sv[0]&~dma0_mask) || sv[1] || ((sv[2]|sv[3])&1))busy|=8;
        for(unsigned tag=0;tag<8;tag++) {
            uint32_t mask=(1u<<15)|(1u<<14)|(1u<<12)|(1u<<8)|(1u<<7);
            if(!tag)mask&=~fbc0_mask;
            if(sv[4+tag]&mask)busy|=8;
        }
        if((sv[15]&(1u<<16)) || (sv[16]&1))busy|=16;
    }
    return busy;
}
static uint32_t cam_rx_changed(const struct x9_camera_receiver_sample *a,
    const struct x9_camera_receiver_sample *b,unsigned port)
{
    uint32_t changed=0;
    if(memcmp(a->mac,b->mac,sizeof(a->mac)))changed|=1;
    /* Shared async/AFIFO masks: unrelated AOV ports are not our ownership.
     * AFIFO portA and async split fields both use two-bit slots. */
    if(((a->async_cfg^b->async_cfg)&((1u<<port)|(1u<<(24+port)))) ||
       ((a->async_split^b->async_split)&(3u<<(2*port))) ||
       ((a->afifo^b->afifo)&(3u<<(2*port))))changed|=2;
    if(memcmp(a->out,b->out,sizeof(a->out)))changed|=4;
    for(unsigned bank=0;bank<2;bank++)for(unsigned word=0;word<X9_CAM_RX_SV_WORDS;word++) {
        uint32_t av=a->sv[bank][word],bv=b->sv[bank][word];
        /* FBC's command bit has its own exact, one-issued transition. Only
         * compare its canonical control while that transition is pending;
         * original/cached/public samples are never overwritten or rebased. */
        if(word==4) {
            av=cam_sv_fbc_compare_word(bank,av);bv=cam_sv_fbc_compare_word(bank,bv);
        }
        if(!word) {
            av=cam_sv_receive_compare_word(bank,av);bv=cam_sv_receive_compare_word(bank,bv);
        }
        if(av!=bv)changed|=8;
    }
    return changed;
}
static int cam_rx_inspect(void *arg)
{
    struct x9_camera_receiver *r=arg;
    struct cam_rx_maps *m=&cam_rx_maps[r->sensor==X9_CAMERA_BACK?0:1];
    int e;
    unsigned port=r->sensor==X9_CAMERA_BACK?3:0;
    r->stage=X9_CAM_RX_STAGE_MAP;
    r->mac_base=0x13884000ull+port*0x8000ull;
    r->out_base=0x3a200000;r->out_inner_base=0x3a208000;r->sv_base=0x3a600000;
    /* Permanent Device mappings, at most once per boot, just like the MM
     * journal mappings. ml_io_map is NOT a camera DMA allocator. */
    if(!m->mac)m->mac=ml_io_map(r->mac_base,0x1000);
    if(!m->async)m->async=ml_io_map(0x3a401000,0x1000);
    if(!m->out[0])m->out[0]=ml_io_map(r->out_base,0x1000);
    if(!m->out[1])m->out[1]=ml_io_map(r->out_inner_base,0x1000);
    /* Map only three documented banks per view, not the holes in the DT. */
    if(!m->sv[0])m->sv[0]=ml_io_map(r->sv_base,0x3000);
    if(!m->sv[1])m->sv[1]=ml_io_map(r->sv_base+0x8000,0x3000);
    if(!m->mac || !m->async || !m->out[0] || !m->out[1] || !m->sv[0] || !m->sv[1])return ENOMEM;
    r->stage=X9_CAM_RX_STAGE_SAMPLE;
    e=cam_rx_sample(m,&r->before);if(e)return e;
    delay(10);
    e=cam_rx_sample(m,&r->after);if(e)return e;
    r->flags|=X9_CAM_RX_SNAPSHOT;r->blockers&=~X9_CAM_RX_NEED_SNAPSHOT;
    r->stage=X9_CAM_RX_STAGE_CHECK;
    r->busy=cam_rx_busy(&r->before,port,0,0)|cam_rx_busy(&r->after,port,0,0);
    r->changed=cam_rx_changed(&r->before,&r->after,port);
    if(r->changed)return ESTALE;
    r->flags|=X9_CAM_RX_STABLE;
    if(r->busy)return EBUSY;
    r->flags|=X9_CAM_RX_DISABLED;
    return 0;
}
static void cam_rx_status_init(unsigned sensor,struct x9_camera_receiver *r)
{
    memset(r,0,sizeof(*r));r->version=1;r->sensor=sensor;
    r->blockers=X9_CAM_RX_NEED_OWNER|X9_CAM_RX_NEED_RATE|X9_CAM_RX_NEED_PHY|
        X9_CAM_RX_NEED_ROUTE|X9_CAM_RX_NEED_SMI|X9_CAM_RX_NEED_DMA_MAP|
        X9_CAM_RX_NEED_DRAIN|X9_CAM_RX_NEED_SNAPSHOT;
}
static int cam_rx_session_fault(int e)
{
    if(!cam_rx_live.error)cam_rx_live.error=e;
    cam_mm_session_fault(e);
    cam_rx_live.status.mm=cam_mm_live.status;
    cam_rx_live.status.flags|=X9_CAM_RX_LATCHED;
    cam_rx_live.status.last_errno=cam_rx_live.error;
    cam_rx_last=cam_rx_live.status;
    return e;
}
/* Opens the SAME real MM engine as PROBE_MM_CLOCKS and retains it after
 * disabled-state sampling. No receiver CSR is written by this lifetime.
 * sensor_epoch=0 is the existing immediate-close diagnostic path only. */
static int cam_rx_session_open(unsigned sensor,uint64_t sensor_epoch,struct x9_camera_receiver *r)
{
    uint64_t epoch=0;
    int e;
    cam_rx_status_init(sensor,r);
    if(cam_rx_live.epoch){r->last_errno=EBUSY;return EBUSY;}
    e=cam_mm_session_open(sensor,&epoch,&r->mm);
    if(!epoch){r->last_errno=e;return e;}
    cam_rx_live.epoch=epoch;cam_rx_live.sensor_epoch=sensor_epoch;
    cam_rx_live.close_attempted=false;cam_rx_live.error=0;
    r->flags=X9_CAM_RX_ATTEMPTED;r->stage=X9_CAM_RX_STAGE_MM;
    if(!e)e=cam_rx_inspect(r);
    cam_rx_live.status=*r;
    cam_rx_live.expected=r->before;
    if(e)cam_rx_session_fault(e);
    else cam_rx_last=*r;
    *r=cam_rx_live.status;
    return e;
}
/* Check against consumed controls plus ONLY our confirmed journal writes,
 * not just two equally redirected current samples. Initial status.before
 * remains evidence; expected is updated only by actual journaled OUTMUX,
 * MAC and CAMSV effects.
 * Never touch read-clear DONE/IRQ/fault banks.
 * Disabled/stable controls are only prerequisites for dropping this
 * unprogrammed lifetime; they do not prove bus/DMA/worker drain. */
static int cam_rx_session_guard(void)
{
    struct x9_camera_receiver_sample a,b;
    struct x9_camera_receiver *r=&cam_rx_live.status;
    const struct cam_rx_maps *m=&cam_rx_maps[r->sensor==X9_CAMERA_BACK?0:1];
    unsigned port=r->sensor==X9_CAMERA_BACK?3:0;
    uint32_t busy,changed,fbc0_mask,dma0_mask;
    int e;
    if(!cam_rx_live.epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return ESTALE;
    if(!(r->flags&X9_CAM_RX_SNAPSHOT))return cam_rx_live.error?cam_rx_live.error:ENXIO;
    /* CAMSV may have confirmed a live-through inner DB effect. Publish
     * only its actual accepted values before the receiver comparison. */
    e=cam_sv_guard();if(e)return e;
    e=cam_rx_sample(m,&a);if(e)return e;
    delay(10);
    e=cam_rx_sample(m,&b);if(e)return e;
    fbc0_mask=cam_sv_fbc_busy_exemption();
    dma0_mask=cam_sv_receive_busy_exemption(0x040);
    busy=cam_rx_busy(&a,port,fbc0_mask,dma0_mask)|cam_rx_busy(&b,port,fbc0_mask,dma0_mask);
    changed=cam_rx_changed(&a,&b,port)|cam_rx_changed(&cam_rx_live.expected,&a,port)|
        cam_rx_changed(&cam_rx_live.expected,&b,port);
    r->after=b;r->busy|=busy;r->changed|=changed;
    if(changed)return ESTALE;
    if(busy)return EBUSY;
    /* FBC1 is not part of the public receiver snapshot. Do not return from
     * this live comparison without rechecking the private counter evidence. */
    return cam_sv_guard();
}
static int cam_rx_session_close(struct x9_camera_receiver *r)
{
    int off,prior=cam_rx_live.error;
    if(!cam_rx_live.epoch || cam_rx_live.close_attempted)return ESTALE;
    if(session.capture)return EBUSY;
    cam_rx_live.close_attempted=true;
    /* An incomplete MM take cannot safely sample receiver registers. It
     * has not issued any receiver writes; unwind only its consumed slots.
     * A complete take first refuses unconsumed analog requests, restores
     * inactive CAMSV geometry/DB policy, OUTMUX with MAC/async clocks held,
     * restores/stops analog, then
     * restores MAC/async, then optional SMI clocks. The receiver must still
     * match the initially captured disabled controls. Unconsumed SMI clock
     * requests or new owners retain the MM and sensor dependencies too. */
    off=cam_irq_close_locked(cam_rx_live.sensor_epoch);
    if(!off)off=cam_smi_retirement_guard(cam_rx_live.sensor_epoch);
    if(!off)off=cam_phy_retirement_guard(cam_rx_live.sensor_epoch);
    if(!off)off=cam_sv_close_locked(cam_rx_live.sensor_epoch);
    if(!off)off=cam_out_close_locked(cam_rx_live.sensor_epoch);
    if(!off)off=cam_phy_close_locked(cam_rx_live.sensor_epoch);
    if(!off)off=cam_mac_close_locked(cam_rx_live.sensor_epoch);
    if(!off)off=cam_smi_clocks_close_locked(cam_rx_live.sensor_epoch);
    if(!off)off=cam_mm.active?cam_rx_session_guard():0;
    if(!off)off=cam_mm_session_close(cam_rx_live.epoch,&cam_rx_live.status.mm);
    else cam_mm_session_retain(cam_rx_live.epoch,off);
    if(off)cam_rx_session_fault(off);
    cam_rx_live.status.last_errno=cam_rx_live.error;
    if(cam_rx_live.status.mm.flags&X9_CAM_MM_RESTORED)cam_rx_live.status.flags|=X9_CAM_RX_RESTORED;
    if(!prior)cam_rx_live.status.stage=off?X9_CAM_RX_STAGE_RESTORE:X9_CAM_RX_STAGE_DONE;
    *r=cam_rx_live.status;cam_rx_last=*r;
    if(!off){cam_rx_live.epoch=0;cam_rx_live.sensor_epoch=0;}
    return off;
}
static bool cam_rx_sensor_held_locked(uint64_t epoch)
{ return cam_rx_live.epoch && cam_rx_live.sensor_epoch==epoch; }
static int cam_rx_sensor_prepare_locked(uint64_t epoch,struct x9_camera_receiver *r)
{
    int e;
    if(!epoch || epoch!=cam_sensor_live.epoch)return ESTALE;
    e=cam_rx_session_open(cam_sensor_live.probe.sensor,epoch,r);
    if(e && cam_rx_sensor_held_locked(epoch))cam_sensor_session_fault(e);
    return e;
}
static int cam_rx_sensor_guard_locked(uint64_t epoch)
{
    int e;
    if(!cam_rx_live.epoch)return 0;
    if(!cam_rx_sensor_held_locked(epoch) || cam_rx_live.close_attempted)return ESTALE;
    e=cam_mm_session_check(cam_rx_live.epoch);
    if(!e)e=cam_irq_guard_locked(epoch);
    if(!e)e=cam_rx_session_guard();
    if(!e)e=cam_mac_guard();
    if(!e)e=cam_phy_guard();
    if(!e)e=cam_out_guard();
    return e?cam_rx_session_fault(e):0;
}
static int cam_rx_sensor_close_locked(uint64_t epoch)
{
    struct x9_camera_receiver r;
    int off;
    if(!cam_rx_sensor_held_locked(epoch))return ESTALE;
    off=cam_rx_session_close(&r);
    cam_mm_log(r.sensor,&r.mm);
    printf("X9 CAMERA RX LIFETIME: sensor=%u sensor_epoch=%llu flags=%x errno=%u restore=%u "
        "retained=%x busy=%x changed=%x (CSI/DL/DMA never enabled, NOT DMA drain)\n",
        r.sensor,epoch,r.flags,r.last_errno,r.mm.restore_errno,r.mm.retained,r.busy,r.changed);
    return off;
}
#include "x9_camera_outmux.inc"
#include "x9_camera_mac.inc"
#include "x9_camera_phy.inc"
#include "x9_camera_smi_clocks.inc"
#include "x9_camera_sv_hw.inc"
static int cam_rx_probe(unsigned sensor,struct x9_camera_receiver *r)
{
    int e,off=0;
    if(cam_sensor_session_active || cam_mm_live.epoch || cam_rx_live.epoch) {
        memset(r,0,sizeof(*r));r->version=1;r->sensor=sensor;r->last_errno=EBUSY;
        return EBUSY;
    }
    cam_rx_status_init(sensor,r);
    if(cam_rx_attempted || cam_mm_attempted || cam_resources_latched || cam_probe_latched) {
        if(cam_rx_attempted)*r=cam_rx_last;
        r->last_errno=cam_resources_latched||cam_probe_latched?EIO:EALREADY;
        if(cam_resources_latched || cam_probe_latched)r->flags|=X9_CAM_RX_LATCHED;
        return r->last_errno;
    }
    e=cam_rx_session_open(sensor,0,r);
    if(cam_rx_live.epoch) {
        cam_rx_attempted=cam_mm_attempted=true;
        off=cam_rx_session_close(r);
        cam_mm_log(sensor,&r->mm);
    }
    if(!e)e=off;
    r->last_errno=e;
    if(r->mm.flags&X9_CAM_MM_RESTORED)r->flags|=X9_CAM_RX_RESTORED;
    if(r->mm.flags&X9_CAM_MM_LATCHED)r->flags|=X9_CAM_RX_LATCHED;
    if(r->mm.restore_errno && (r->flags&X9_CAM_RX_DISABLED))r->stage=X9_CAM_RX_STAGE_RESTORE;
    if(!e)r->stage=X9_CAM_RX_STAGE_DONE;
    cam_rx_last=*r;
    printf("X9 CAMERA RX: sensor=%u flags=%x stage=%u errno=%u restore=%u retained=%x "
        "busy=%x changed=%x blockers=%x MAC=%llx out=%llx/%llx SV=%llx "
        "(read-only controls, NOT ownership or DMA drain)\n",sensor,r->flags,r->stage,
        r->last_errno,r->mm.restore_errno,r->mm.retained,r->busy,r->changed,r->blockers,
        r->mac_base,r->out_base,r->out_inner_base,r->sv_base);
    return e;
}
