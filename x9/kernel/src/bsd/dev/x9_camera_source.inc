/* ISP8s PHY3.0 normal receive path, NOT sensor stream publication.
 * csirx_mac_top_setting -> mac_csi_setting -> dphy/cphy_setting;
 * route::outmux_switch_config writes DL before trigger_cfg_done.
 * Sole control + IRQ mutex owner, input still inactive throughout. This
 * ledger is separate from cold MAC/PHY writers and never grants native
 * SID80/rate authority, frame completion or RAW/AXI/map retirement.
 * No inner route writes, FLUSH_FLAG_CLEAR, reset or command replay.
 */
enum { CAM_SOURCE_MAC, CAM_SOURCE_PHY, CAM_SOURCE_OUT };
enum { CAM_SOURCE_FIELDS=6, CAM_SOURCE_STEPS=2*CAM_SOURCE_FIELDS };
enum { CAM_SOURCE_IDLE, CAM_SOURCE_STARTING, CAM_SOURCE_ARMED,
    CAM_SOURCE_STOPPING, CAM_SOURCE_STOPPED };
struct cam_source_field {
    unsigned kind,reg,banks,offset;
    vm_offset_t view[2];
    uint32_t mask,full,before[2],on[2];
};
struct cam_source_step {
    unsigned field;
    uint32_t prior[2],target[2],seen;
};
static struct {
    uint64_t epoch;
    struct cam_source_field field[CAM_SOURCE_FIELDS];
    struct cam_source_step step[CAM_SOURCE_STEPS];
    unsigned count,phase,pending,samples;
    uint32_t issued;
    bool captured;
    int error;
} cam_source={.phase=CAM_SOURCE_IDLE,.pending=CAM_SOURCE_STEPS};
static int cam_source_fault(int e)
{
    if(!cam_source.error)cam_source.error=e;
    return cam_source.error;
}
static uint32_t cam_source_expected(const struct cam_source_field *f,unsigned bank)
{
    if(f->kind==CAM_SOURCE_MAC)return cam_mac.reg[f->reg].expected;
    if(f->kind==CAM_SOURCE_PHY)return cam_phy.reg[f->reg].expected;
    return cam_out.expected[bank][f->reg];
}
static void cam_source_accept(const struct cam_source_field *f,unsigned bank,uint32_t v)
{
    /* Publish only the value actually read. Initial snapshots and the
     * immutable per-command priors/targets remain untouched. */
    if(f->kind==CAM_SOURCE_MAC) {
        cam_mac.reg[f->reg].expected=v;cam_mac_publish();
    } else if(f->kind==CAM_SOURCE_PHY)cam_phy.reg[f->reg].expected=v;
    else {cam_out.expected[bank][f->reg]=v;cam_out_publish_expected();}
}
static int cam_source_observe(vm_offset_t view,unsigned offset,uint32_t v,bool *allowed)
{
    if(!cam_source.captured)return 0;
    for(unsigned i=0;i<cam_source.count;i++) {
        const struct cam_source_field *f=&cam_source.field[i];
        if(f->offset!=offset)continue;
        for(unsigned bank=0;bank<f->banks;bank++)if(view==f->view[bank]) {
            unsigned p=cam_source.pending;
            v&=f->full;
            if(cam_source.error)return cam_source.error;
            if(cam_source.epoch!=cam_rx_live.sensor_epoch)return cam_source_fault(ESTALE);
            if(p<2*cam_source.count && (cam_source.issued&(1u<<p)) &&
               cam_source.step[p].field==i) {
                struct cam_source_step *s=&cam_source.step[p];
                if(cam_source.phase!=CAM_SOURCE_STARTING && cam_source.phase!=CAM_SOURCE_STOPPING)
                    return cam_source_fault(ESTALE);
                if(v!=s->target[bank] && ((s->seen&(1u<<bank)) || v!=s->prior[bank]))
                    return cam_source_fault(ESTALE);
                if(v==s->target[bank]) {
                    s->seen|=1u<<bank;cam_source_accept(f,bank,v);
                }
                *allowed=true;
            }
            /* All other values remain subject to the caller's original
             * full guard, including its own distinct cold restore journal.
             * Do not block a legal non-source masked restore after STOPPED,
             * or replace the caller's unchanged-field evidence with ours. */
            return 0;
        }
    }
    return 0;
}
/* Historical comparison does NOT advance the observation ledger. Only
 * this currently issued exact field may span prior/target; every actual
 * read above records target before any later read can conceal regression. */
static uint32_t cam_source_compare_word(vm_offset_t view,unsigned offset,uint32_t v)
{
    unsigned p=cam_source.pending;
    const struct cam_source_field *f;
    if(!cam_source.captured || cam_source.error || p>=2*cam_source.count ||
       !(cam_source.issued&(1u<<p)))return v;
    f=&cam_source.field[cam_source.step[p].field];
    if(offset!=f->offset)return v;
    for(unsigned bank=0;bank<f->banks;bank++)if(view==f->view[bank] &&
       (v==cam_source.step[p].prior[bank] || v==cam_source.step[p].target[bank]))return 0;
    return v;
}
static uint32_t cam_source_busy_exemption(vm_offset_t view,unsigned offset)
{
    if(!cam_source.captured || cam_source.error || cam_source.epoch!=cam_sv.epoch ||
       !cam_sv.request_held || cam_sv.terminal || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED)
        return 0;
    for(unsigned i=0;i<cam_source.count;i++) {
        const struct cam_source_field *f=&cam_source.field[i];
        if(f->view[0]==view && f->offset==offset && (cam_source.issued&(1u<<i)))return f->mask;
    }
    return 0; /* classification only; cold inspect always passes zero masks */
}
/* OUTMUX's existing engine can poll/ack between source-loop callbacks.
 * Fence its every full guard (including before CFG_DONE and W1C) with the
 * same native request and actual input-inactive GIC evidence. Deliberately
 * no SV/OUTMUX recursion and no callback under spin. Cold commits unchanged. */
static int cam_source_commit_guard(void)
{
    unsigned p=cam_source.pending;
    int e;
    if(!cam_source.captured || p>=2*cam_source.count ||
       cam_source.field[cam_source.step[p].field].kind!=CAM_SOURCE_OUT)return 0;
    if(cam_source.error)return cam_source.error;
    if(session.stream || cam_source.epoch!=cam_irq.epoch)return cam_source_fault(ESTALE);
    e=cam_sv_request_authority();
    if(!e)e=cam_irq_credit_guard_locked(cam_source.epoch);
    return e?cam_source_fault(e):0;
}
static int cam_source_add(unsigned kind,unsigned bank,unsigned offset,uint32_t mask,uint32_t value)
{
    struct cam_source_field f={.kind=kind,.offset=offset,.mask=mask,.banks=1};
    if(cam_source.count>=CAM_SOURCE_FIELDS || !value || (value&~mask))return EINVAL;
    if(kind==CAM_SOURCE_MAC) {
        for(f.reg=0;f.reg<cam_mac.count;f.reg++)
            if(cam_mac.reg[f.reg].bank==bank && cam_mac.reg[f.reg].offset==offset)break;
        if(f.reg==cam_mac.count)return EINVAL;
        f.view[0]=cam_mac.bank[bank];f.full=cam_mac.reg[f.reg].mask;
    } else if(kind==CAM_SOURCE_PHY) {
        f.reg=cam_phy_find(bank,offset);if(f.reg==CAM_PHY_NO_REG)return EINVAL;
        f.view[0]=cam_phy.bank[bank];f.full=cam_phy.reg[f.reg].mask;
    } else {
        if(offset!=0x20 || mask!=(1u<<12))return EINVAL;
        f.reg=7;f.banks=2;f.full=UINT32_MAX;
        f.view[0]=cam_out.out[0];f.view[1]=cam_out.out[1];
    }
    if(mask&~f.full)return EINVAL;
    for(unsigned b=0;b<f.banks;b++) {
        f.before[b]=cam_source_expected(&f,b);
        if(f.before[b]&mask)return EBUSY; /* no inherited enable repair */
        f.on[b]=f.before[b]|value;
    }
    if(f.banks==2 && f.before[0]!=f.before[1])return ESTALE;
    cam_source.field[cam_source.count++]=f;return 0;
}
/* Admission precedes S1 arm. Flags are prerequisites, never grants. The
 * native lease .check still proves normal domain/route/rate/PHY/IRQ and
 * whole retained backing independently at every mutation boundary. */
static int cam_source_admission_locked(uint64_t epoch)
{
    const uint32_t phy_flags=X9_CAM_PHY_CALIBRATED|X9_CAM_PHY_TIMING_CONFIGURED|
        X9_CAM_PHY_DIGITAL_CONFIGURED|X9_CAM_PHY_CTLE_CONFIGURED;
    unsigned sensor=cam_sv.status.sensor;
    struct cam_sensor_ap ap={&cam_journal.sensor,sensor==X9_CAMERA_BACK?0x1a:0x10};
    uint8_t byte;
    int e;
    if(cam_sensor_live.epoch!=epoch || cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED ||
       cam_sensor_live.close_attempted || session.stream)return ESTALE;
    if(!cam_mac.held || cam_mac.epoch!=epoch || cam_mac.close_attempted || cam_mac.error ||
       !(cam_mac.status.flags&X9_CAM_MAC_CONFIGURED) || cam_mac.pending_slot<CAM_MAC_WRITES ||
       cam_mac.cg_uncertain || !cam_phy.held || cam_phy.epoch!=epoch || cam_phy.close_attempted ||
       cam_phy.error || (cam_phy.status.flags&phy_flags)!=phy_flags || cam_phy.calibration_pending ||
       cam_phy.pending_reg!=CAM_PHY_NO_REG || !cam_out.held || cam_out.epoch!=epoch ||
       cam_out.close_attempted || cam_out.error || !cam_out.consumed || cam_out.commit_pending ||
       cam_out.pending_slot<CAM_OUT_WRITES || cam_out.cg_uncertain || cam_out_irq.poison ||
       !cam_out_irq.mode_ready || cam_out_irq.raw_disabled || cam_out_irq.mode_bank<2 ||
       cam_out_irq.ack_pending || !cam_smi.held || cam_smi.epoch!=epoch || cam_smi.close_attempted ||
       cam_smi.error || !(cam_smi.status.flags&X9_CAM_SMI_CLOCKS_CONFIGURED) ||
       !(cam_smi.status.a0.flags&X9_CAM_SMI_A0_CONFIGURED) ||
       !(cam_smi.status.shared.flags&X9_CAM_SMI_SHARED_CONFIGURED))return ESTALE;
    if(sensor!=X9_CAMERA_FRONT && sensor!=X9_CAMERA_BACK)return EINVAL;
    e=cam_sv_environment();if(e)return e;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(e)return e;if(byte)return EBUSY;
        e=cam_sensor_ap_read(&ap,0x0104,&byte);if(e)return e;if(byte)return EBUSY;
    }
    e=cam_sensor_ap_guard(&ap);if(e)return e;
    if(cam_source.captured) {
        if(cam_source.epoch!=epoch || cam_source.phase!=CAM_SOURCE_IDLE || cam_source.issued)return EBUSY;
        return cam_source.error; /* no snapshot rebase after first admission */
    }
    cam_source.epoch=epoch;cam_source.count=0;
    e=cam_source_add(CAM_SOURCE_MAC,CAM_MAC_TOP,0x40,3,sensor==X9_CAMERA_FRONT?1:2);
    if(!e)e=cam_source_add(CAM_SOURCE_MAC,CAM_MAC_A,0,0xf,sensor==X9_CAMERA_FRONT?0xf:7);
    if(sensor==X9_CAMERA_FRONT) {
        unsigned lane=cam_phy_find(CAM_PHY_D,4);
        if(lane==CAM_PHY_NO_REG || cam_phy.reg[lane].expected!=0x80403102u)e=ESTALE;
        if(!e)e=cam_source_add(CAM_SOURCE_PHY,CAM_PHY_D,0,0xf03,0xf01);
        if(!e)e=cam_source_add(CAM_SOURCE_PHY,CAM_PHY_A,0x100,0x101,0x101);
        if(!e)e=cam_source_add(CAM_SOURCE_PHY,CAM_PHY_B,0x100,0x101,0x101);
    } else {
        if(!e)e=cam_source_add(CAM_SOURCE_PHY,CAM_PHY_C,0,7,7);
        if(!e)e=cam_source_add(CAM_SOURCE_PHY,CAM_PHY_C,0x168,7,7);
    }
    if(!e)e=cam_source_add(CAM_SOURCE_OUT,0,0x20,1u<<12,1u<<12);
    if(e){cam_source.count=0;return e;}
    for(unsigned i=0;i<cam_source.count;i++) {
        const struct cam_source_field *f=&cam_source.field[i];
        struct cam_source_step *on=&cam_source.step[i],*off=&cam_source.step[2*cam_source.count-1-i];
        on->field=off->field=i;
        memcpy(on->prior,f->before,sizeof(on->prior));memcpy(on->target,f->on,sizeof(on->target));
        memcpy(off->prior,f->on,sizeof(off->prior));memcpy(off->target,f->before,sizeof(off->target));
    }
    cam_source.captured=true;return cam_sv_environment();
}
static int cam_source_guard_locked(uint64_t epoch)
{
    int e;
    if(!cam_source.captured || cam_source.epoch!=epoch || cam_source.error ||
       cam_sensor_live.epoch!=epoch || session.stream ||
       cam_sv_gate.phase!=X9_CAM_SV_GATE_ARMED || cam_sv_gate.error ||
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_ARMED)return cam_source.error?cam_source.error:ESTALE;
    e=cam_sv_request_guard_locked(epoch,cam_irq.slot);if(e)return e;
    e=cam_irq_credit_guard_locked(epoch);if(e)return e;
    lck_mtx_lock(&camera_camsv_lock);
    e=cam_irq.slot->state==X9_CAMSV_ARMED?0:ESTALE;
    lck_mtx_unlock(&camera_camsv_lock);return e;
}
static int cam_source_wait_locked(uint64_t epoch,uint64_t end,bool outer_only)
{
    unsigned p=cam_source.pending;
    const struct cam_source_step *s=&cam_source.step[p];
    const struct cam_source_field *f=&cam_source.field[s->field];
    cam_source.samples=0;
    for(unsigned poll=0;poll<10000;poll++) {
        bool match=true;
        int e;
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        e=cam_source_guard_locked(epoch);if(e)return e;
        for(unsigned b=0;b<(outer_only?1:f->banks);b++)
            if(cam_source_expected(f,b)!=s->target[b] || !(s->seen&(1u<<b)))match=false;
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        cam_source.samples=match?cam_source.samples+1:0;
        if(cam_source.samples==2)return 0;
        delay(1);
    }
    return ETIMEDOUT;
}
static int cam_source_transition_locked(uint64_t epoch,bool off)
{
    uint64_t span,end;
    unsigned first=off?cam_source.count:0,last=off?2*cam_source.count:cam_source.count;
    int e;
    if(!cam_source.captured || cam_source.epoch!=epoch || cam_source.pending<CAM_SOURCE_STEPS ||
       cam_source.phase!=(off?CAM_SOURCE_ARMED:CAM_SOURCE_IDLE))return ESTALE;
    e=cam_source_guard_locked(epoch);if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    cam_source.phase=off?CAM_SOURCE_STOPPING:CAM_SOURCE_STARTING;
    for(unsigned p=first;p<last;p++) {
        struct cam_source_step *s=&cam_source.step[p];
        const struct cam_source_field *f=&cam_source.field[s->field];
        uint32_t raw,next;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_source_guard_locked(epoch);if(e)break;
        if(cam_source.issued&(1u<<p)){e=ESTALE;break;}
        for(unsigned b=0;b<f->banks;b++)if(cam_source_expected(f,b)!=s->prior[b])e=ESTALE;
        if(e)break;
        raw=cam_rr(f->view[0],f->offset);
        if((raw&f->full)!=s->prior[0]){e=ESTALE;break;}
        /* AFIFO CTRL0 has command bits7/15/23, not RO completion flags.
         * Never echo those commands; actual FLUSH_FLAG lives at +104. */
        if(f->kind==CAM_SOURCE_PHY && f->offset==0x100 && (raw&0x00808080u)){e=EBUSY;break;}
        next=(raw&~f->mask)|(s->target[0]&f->mask);
        e=cam_source_guard_locked(epoch);if(e)break;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        cam_source.pending=p;cam_source.issued|=1u<<p; /* BEFORE unique ordinary RW store */
        if(f->kind==CAM_SOURCE_OUT) {
            cam_out.consumed=false;cam_out.status.flags&=~X9_CAM_OUT_CONSUMED;
        }
        cam_rw(f->view[0],f->offset,next);
        if(f->kind==CAM_SOURCE_OUT) {
            e=cam_source_wait_locked(epoch,end,true);if(e)break;
            /* Fresh WCLR-qualified event and actual full inner target;
             * use the existing engine, never force inner DL/tag/route. */
            e=cam_out_commit();if(e)break;
        }
        e=cam_source_wait_locked(epoch,end,false);if(e)break;
        cam_source.pending=CAM_SOURCE_STEPS;
    }
    if(!e) {
        e=cam_source_guard_locked(epoch);
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    }
    if(e)cam_source_fault(e);
    else cam_source.phase=off?CAM_SOURCE_STOPPED:CAM_SOURCE_ARMED;
    printf("X9 CAMERA RX PATH: epoch=%llu capture_id=%llu sensor=%u off=%u phase=%u "
        "fields=%u pending=%u issued=%x samples=%u errno=%d "
        "(sensor input inactive; NOT frame/RAW/AXI/map drain)\n",
        epoch,cam_sv.request.frame.capture_id,cam_sv.status.sensor,off,cam_source.phase,
        cam_source.count,cam_source.pending,cam_source.issued,cam_source.samples,e);
    return e;
}
static int cam_source_quiesce_locked(uint64_t epoch)
{
    if(!cam_source.captured)return 0;
    if(cam_source.epoch!=epoch)return ESTALE;
    if(cam_source.error)return cam_source.error;
    if(session.stream)return EBUSY;
    if(cam_source.phase==CAM_SOURCE_IDLE || cam_source.phase==CAM_SOURCE_STOPPED)return 0;
    /* Core sets session.stream BEFORE issuing 0100=1 and keeps it through
     * stop/drain uncertainty. Refuse even if no SOF has yet been serviced.
     * A partial arm, sampled input, changed counter or unknown commit
     * cannot be canceled by a reverse write. No source-on path uses this. */
    if(cam_source.phase!=CAM_SOURCE_ARMED)return ESTALE;
    return cam_source_transition_locked(epoch,true);
}
