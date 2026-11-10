/* Normal ISP8s mtk_cam_sv_central_common_enable/disable and toggle_tg_db.
 * Sole tag0/group0 AP request, camera_control_lock + camera_irq_lock held.
 * CMOS/VF arm is AFTER real S1/cache/address/FBC/GIC/receive-arm acceptance;
 * upstream PHY/MAC/OUTMUX DL and sensor source are still off. No frame,
 * one-shot write guarantee, RAW/AXI/map drain or ownership is inferred here.
 * Do not port normal disable's global DMA reset or all-tag clearing loop.
 */
enum { CAM_SV_GATE_SEN, CAM_SV_GATE_VF, CAM_SV_GATE_PATH,
    CAM_SV_GATE_ON_STEPS=3, CAM_SV_GATE_STOP_FIRST=3 };
#define CAM_SV_GATE_CMOS 1u
#define CAM_SV_GATE_FLUSH (1u<<13)
#define CAM_SV_GATE_TGOFF (1u<<11)
#define CAM_SV_GATE_TGDB (1u<<8)
static const unsigned cam_sv_gate_offsets[X9_CAM_SV_GATE_FIELDS]={0x140,0x148,0x14c};
static const unsigned cam_sv_gate_step_fields[X9_CAM_SV_GATE_STEPS]={
    CAM_SV_GATE_SEN,CAM_SV_GATE_SEN,CAM_SV_GATE_VF,
    CAM_SV_GATE_SEN,CAM_SV_GATE_VF,CAM_SV_GATE_PATH,CAM_SV_GATE_PATH,CAM_SV_GATE_SEN
};
static struct {
    uint64_t epoch;
    uint32_t phase,pending,issued,seen[2],samples;
    uint32_t before[X9_CAM_SV_GATE_FIELDS];
    uint32_t prior[X9_CAM_SV_GATE_STEPS],target[X9_CAM_SV_GATE_STEPS];
    bool captured;
    bool restore_attempted,restore_issued;
    uint32_t restore_prior,restore_seen,restore_samples;
    int error;
} cam_sv_gate;
static void cam_sv_gate_init(void)
{
    memset(&cam_sv_gate,0,sizeof(cam_sv_gate));
    cam_sv_gate.pending=X9_CAM_SV_GATE_STEPS;
}
static void cam_sv_gate_capture(void)
{
    cam_sv_gate.epoch=cam_sv.epoch;
    for(unsigned i=0;i<X9_CAM_SV_GATE_FIELDS;i++)
        cam_sv_gate.before[i]=cam_sv.before[0][cam_sv_find(cam_sv_gate_offsets[i])];
    cam_sv_gate.captured=true;
}
static void cam_sv_gate_status(void)
{
    struct x9_camera_sv_hw_status *s=&cam_sv.status;
    s->gate_phase=cam_sv_gate.phase;s->gate_pending=cam_sv_gate.pending;
    s->gate_issued=cam_sv_gate.issued;s->gate_samples=cam_sv_gate.samples;
    s->gate_errno=cam_sv_gate.error;
    s->gate_restore_attempted=cam_sv_gate.restore_attempted;s->gate_restore_issued=cam_sv_gate.restore_issued;
    s->gate_restore_seen=cam_sv_gate.restore_seen;s->gate_restore_samples=cam_sv_gate.restore_samples;
    memcpy(s->gate_seen,cam_sv_gate.seen,sizeof(s->gate_seen));
    memcpy(s->gate_before,cam_sv_gate.before,sizeof(s->gate_before));
    memcpy(s->gate_prior,cam_sv_gate.prior,sizeof(s->gate_prior));
    memcpy(s->gate_target,cam_sv_gate.target,sizeof(s->gate_target));
    if(cam_sv_gate.phase==X9_CAM_SV_GATE_ARMED && !cam_sv.error)s->flags|=X9_CAM_SV_HW_GATE_ARMED;
    else s->flags&=~X9_CAM_SV_HW_GATE_ARMED;
    if(cam_sv_gate.phase==X9_CAM_SV_GATE_STOPPED && !cam_sv.error)s->flags|=X9_CAM_SV_HW_GATE_STOPPED;
    else s->flags&=~X9_CAM_SV_HW_GATE_STOPPED;
    if(cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORED && !cam_sv.error)s->flags|=X9_CAM_SV_HW_GATE_RESTORED;
    else s->flags&=~X9_CAM_SV_HW_GATE_RESTORED;
}
static void cam_sv_gate_fault(int e)
{
    if(cam_sv_gate.phase!=X9_CAM_SV_GATE_IDLE && !cam_sv_gate.error)cam_sv_gate.error=e;
}
static void cam_sv_gate_observed(unsigned bank,const uint32_t *v)
{
    for(unsigned i=0;i<X9_CAM_SV_GATE_FIELDS;i++)
        cam_sv.status.gate_after[bank][i]=v[cam_sv_find(cam_sv_gate_offsets[i])];
}
/* These features are NOT our direct, continuous RAW10 TG profile. In
 * particular SINGLE_MODE is not a one-frame write/retirement guarantee.
 * Preserve clock/pixel/timing polarity and all reserved bits; never repair
 * an incompatible inherited profile or use it as sole native authority. */
static int cam_sv_gate_admission_locked(uint64_t epoch)
{
    uint32_t sen=cam_sv_gate.before[CAM_SV_GATE_SEN],vf=cam_sv_gate.before[CAM_SV_GATE_VF];
    uint32_t path=cam_sv_gate.before[CAM_SV_GATE_PATH];
    if(!cam_sv.held || cam_sv.epoch!=epoch || !cam_sv_gate.captured || cam_sv_gate.epoch!=epoch ||
       cam_sv_gate.phase!=X9_CAM_SV_GATE_IDLE || cam_sv_gate.issued)return ESTALE;
    if(sen&((1u<<0)|(1u<<1)|(1u<<2)|(1u<<8)|(1u<<11)|(15u<<16)|(1u<<26)))return ENOTSUP;
    if(vf&0x7773u)return ENOTSUP; /* EN, SINGLE, FR_CON, delay/mux controls */
    if(path&0x80190u)return ENOTSUP; /* JPG, JPG_LINEND, TG DB disable, EXP_ESC */
    for(unsigned i=0;i<X9_CAM_SV_GATE_FIELDS;i++) {
        unsigned n=cam_sv_find(cam_sv_gate_offsets[i]);
        if(n>=cam_sv.count || cam_sv.expected[0][n]!=cam_sv_gate.before[i] ||
           cam_sv.expected[1][n]!=cam_sv_gate.before[i])return ESTALE;
    }
    return 0;
}
static bool cam_sv_gate_off_ready(void)
{
    return !cam_sv_gate.error && !cam_sv.error && !cam_sv.terminal &&
        (cam_sv_gate.phase==X9_CAM_SV_GATE_IDLE || cam_sv_gate.phase==X9_CAM_SV_GATE_STOPPED ||
         cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORING || cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORED);
}
/* Only classification, never a grant. Exact issued current controls and
 * every unchanged word still pass the full snapshot guard. Cold is strict. */
static uint32_t cam_sv_gate_busy_exemption(unsigned offset)
{
    if(!cam_sv.held || !cam_sv.request_held || cam_sv.terminal || cam_sv_gate.error ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED ||
       cam_sv_gate.phase<X9_CAM_SV_GATE_PENDING || cam_sv_gate.phase>X9_CAM_SV_GATE_STOPPING)return 0;
    for(unsigned step=0;step<X9_CAM_SV_GATE_STEPS;step++)
        if((cam_sv_gate.issued&(1u<<step)) &&
           cam_sv_gate_offsets[cam_sv_gate_step_fields[step]]==offset)return 1;
    return 0;
}
/* Comparison may span the ONE current posted control transition. Real
 * samples below and the full guard record targets before subsequent reads,
 * so historical normalization cannot erase a real target->prior regression. */
static uint32_t cam_sv_gate_compare_word(unsigned bank,unsigned offset,uint32_t value)
{
    unsigned step=cam_sv_gate.pending;
    if(bank<2 && cam_sv_gate.restore_issued && !cam_sv_gate.error &&
       offset==cam_sv_gate_offsets[CAM_SV_GATE_SEN] &&
       (cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORING || cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORED) &&
       (value==cam_sv_gate.restore_prior || value==cam_sv_gate.before[CAM_SV_GATE_SEN]))return 0;
    if(bank<2 && step<X9_CAM_SV_GATE_STEPS && !cam_sv.terminal &&
       (cam_sv_gate.issued&(1u<<step)) &&
       cam_sv_gate_offsets[cam_sv_gate_step_fields[step]]==offset &&
       (value==cam_sv_gate.prior[step] || value==cam_sv_gate.target[step]))return 0;
    return value;
}
static int cam_sv_gate_note(unsigned bank,unsigned offset,uint32_t value,bool *allowed)
{
    unsigned step=cam_sv_gate.pending;
    uint32_t bit;
    if(cam_sv_gate.restore_issued && offset==cam_sv_gate_offsets[CAM_SV_GATE_SEN] &&
       (cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORING || cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORED)) {
        if(bank>=2 || cam_sv_gate.epoch!=cam_sv.epoch || !cam_sv.credit_retired ||
           !cam_sv.close_attempted || (value!=cam_sv_gate.before[CAM_SV_GATE_SEN] &&
            ((cam_sv_gate.restore_seen&(1u<<bank)) || value!=cam_sv_gate.restore_prior)))return ESTALE;
        if(value==cam_sv_gate.before[CAM_SV_GATE_SEN])cam_sv_gate.restore_seen|=1u<<bank;
        *allowed=true;return 0;
    }
    if(step>=X9_CAM_SV_GATE_STEPS || !(cam_sv_gate.issued&(1u<<step)) ||
       cam_sv_gate_offsets[cam_sv_gate_step_fields[step]]!=offset)return 0;
    if(cam_sv_gate.epoch!=cam_sv.epoch || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED ||
       (cam_sv_gate.phase!=X9_CAM_SV_GATE_PENDING && cam_sv_gate.phase!=X9_CAM_SV_GATE_STOPPING))return ESTALE;
    bit=1u<<step;
    if(value!=cam_sv_gate.target[step] && ((cam_sv_gate.seen[bank]&bit) ||
       value!=cam_sv_gate.prior[step]))return ESTALE;
    if(value==cam_sv_gate.target[step])cam_sv_gate.seen[bank]|=bit;
    *allowed=true;return 0;
}
static int cam_sv_gate_observe_word(unsigned bank,unsigned reg,uint32_t value,bool *allowed)
{
    unsigned step=cam_sv_gate.pending;
    bool owned=false;
    int e=cam_sv_gate_note(bank,cam_sv.offset[reg],value,&owned);
    if(e)return e;
    if(owned) {
        *allowed=true;
        if(cam_sv_gate.restore_issued && cam_sv.offset[reg]==cam_sv_gate_offsets[CAM_SV_GATE_SEN] &&
           (cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORING || cam_sv_gate.phase==X9_CAM_SV_GATE_RESTORED)) {
            if(value==cam_sv_gate.before[CAM_SV_GATE_SEN])cam_sv.expected[bank][reg]=value;
        } else if(value==cam_sv_gate.target[step])cam_sv.expected[bank][reg]=value;
    }
    return 0;
}
static int cam_sv_gate_sample_word(unsigned bank,vm_offset_t view,unsigned offset,uint32_t value)
{
    unsigned field=0,reg;
    bool allowed=false;
    int e;
    if(cam_sv_gate.phase==X9_CAM_SV_GATE_IDLE)return 0;
    if(bank>=2 || view!=cam_sv.view[bank] || !cam_sv.held || !cam_sv.request_held ||
       cam_sv.terminal || cam_sv_gate.epoch!=cam_sv.epoch)return cam_sv_fault(ESTALE);
    while(field<X9_CAM_SV_GATE_FIELDS && cam_sv_gate_offsets[field]!=offset)field++;
    if(field==X9_CAM_SV_GATE_FIELDS)return cam_sv_fault(EINVAL);
    reg=cam_sv_find(offset);if(reg>=cam_sv.count)return cam_sv_fault(ESTALE);
    cam_sv.status.gate_after[bank][field]=value;
    e=cam_sv_gate_note(bank,offset,value,&allowed);if(e)return cam_sv_fault(e);
    if(!allowed && value!=cam_sv.expected[bank][reg])return cam_sv_fault(ESTALE);
    return 0;
}
static int cam_sv_gate_transition_locked(uint64_t epoch,bool off)
{
    uint64_t span,end;
    unsigned first=off?CAM_SV_GATE_STOP_FIRST:0,last=off?X9_CAM_SV_GATE_STEPS:CAM_SV_GATE_ON_STEPS;
    int e;
    if(!cam_sv.held || cam_sv.epoch!=epoch || !cam_sv.request_ready || !cam_sv.fbc_ready ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED ||
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_ARMED || cam_sv_gate.pending<X9_CAM_SV_GATE_STEPS)return ESTALE;
    if(off) {
        if(cam_sv_gate.phase!=X9_CAM_SV_GATE_ARMED)return ESTALE;
    } else {
        uint32_t sen,vf,path;
        e=cam_sv_gate_admission_locked(epoch);if(e)return e;
        sen=cam_sv_gate.before[CAM_SV_GATE_SEN];vf=cam_sv_gate.before[CAM_SV_GATE_VF];
        path=cam_sv_gate.before[CAM_SV_GATE_PATH];
        cam_sv_gate.prior[0]=sen;cam_sv_gate.target[0]=sen|CAM_SV_GATE_CMOS;
        cam_sv_gate.prior[1]=cam_sv_gate.target[0];cam_sv_gate.target[1]=cam_sv_gate.target[0]|CAM_SV_GATE_FLUSH;
        cam_sv_gate.prior[2]=vf;cam_sv_gate.target[2]=vf|1;
        cam_sv_gate.prior[3]=cam_sv_gate.target[1];cam_sv_gate.target[3]=cam_sv_gate.target[1]|CAM_SV_GATE_TGOFF;
        cam_sv_gate.prior[4]=cam_sv_gate.target[2];cam_sv_gate.target[4]=vf;
        cam_sv_gate.prior[5]=path;cam_sv_gate.target[5]=path|CAM_SV_GATE_TGDB;
        cam_sv_gate.prior[6]=cam_sv_gate.target[5];cam_sv_gate.target[6]=path;
        cam_sv_gate.prior[7]=cam_sv_gate.target[3];cam_sv_gate.target[7]=cam_sv_gate.target[3]&~CAM_SV_GATE_CMOS;
    }
    e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)return e;
    e=cam_irq_control_guard_locked(epoch,off && session.stream);if(e)return e;
    e=cam_irq_slot_guard_locked(off && session.stream);if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    cam_sv_gate.phase=off?X9_CAM_SV_GATE_STOPPING:X9_CAM_SV_GATE_PENDING;
    for(unsigned step=first;step<last;step++) {
        unsigned field=cam_sv_gate_step_fields[step],offset=cam_sv_gate_offsets[field];
        unsigned reg=cam_sv_find(offset);
        uint32_t bit=1u<<step,next=cam_sv_gate.target[step];
        bool complete=false;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)break;
        e=cam_irq_control_guard_locked(epoch,off && session.stream);if(e)break;
        if(cam_sv_gate.issued&bit || cam_sv.expected[0][reg]!=cam_sv_gate.prior[step] ||
           cam_sv.expected[1][reg]!=cam_sv_gate.prior[step]){e=ESTALE;break;}
        cam_sv_gate.pending=step;cam_sv_gate.samples=0;
        if(next!=cam_sv_gate.prior[step]) {
            cam_sv_gate.issued|=bit;cam_sv_status();
            cam_rw(cam_sv.view[0],offset,next); /* one store, NEVER retry */
        }
        for(unsigned poll=0;poll<10000;poll++) {
            bool match=true;
            if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
            e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)break;
            e=cam_irq_control_guard_locked(epoch,off && session.stream);if(e)break;
            for(unsigned bank=0;bank<2;bank++) {
                if(cam_sv.status.gate_after[bank][field]!=next)match=false;
                else cam_sv_gate.seen[bank]|=bit; /* also records actual no-op observation */
            }
            if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
            cam_sv_gate.samples=match?cam_sv_gate.samples+1:0;
            if(cam_sv_gate.samples==2){complete=true;break;}
            delay(1);
        }
        if(e)break;
        if(!complete){e=ETIMEDOUT;break;}
        cam_sv_gate.pending=X9_CAM_SV_GATE_STEPS;
    }
    if(!e) {
        cam_sv_gate.phase=off?X9_CAM_SV_GATE_STOPPED:X9_CAM_SV_GATE_ARMED;
        e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);
        if(!e)e=cam_irq_control_guard_locked(epoch,off && session.stream);
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    }
    if(e){cam_sv.terminal=true;cam_sv_fault(e);}
    cam_sv_status();
    printf("X9 CAMERA TG GATE: epoch=%llu capture_id=%llu off=%u phase=%u pending=%u "
        "issued=%x seen=%x/%x samples=%u errno=%d SEN=%x/%x VF=%x/%x PATH=%x/%x "
        "(upstream PHY/DL/sensor off; NOT frame/RAW/AXI/map drain)\n",
        epoch,cam_sv.request.frame.capture_id,off,cam_sv_gate.phase,cam_sv_gate.pending,
        cam_sv_gate.issued,cam_sv_gate.seen[0],cam_sv_gate.seen[1],cam_sv_gate.samples,e,
        cam_sv.status.gate_after[0][0],cam_sv.status.gate_after[1][0],
        cam_sv.status.gate_after[0][1],cam_sv.status.gate_after[1][1],
        cam_sv.status.gate_after[0][2],cam_sv.status.gate_after[1][2]);
    return e;
}
static int cam_sv_gate_quiesce_locked(uint64_t epoch)
{
    if(cam_sv_gate.phase==X9_CAM_SV_GATE_IDLE)return 0;
    /* An uncertain TG/DB phase cannot be canceled by a blind VF/CMOS off,
     * a reset or replay. A streamed gate also requires the private actual
     * OFF/DONE/worker/GIC receipt at every transition guard. */
    if(cam_sv_gate.phase!=X9_CAM_SV_GATE_ARMED)return cam_sv.error?cam_sv.error:ESTALE;
    return cam_sv_gate_transition_locked(epoch,true);
}
/* Return only OUR stopped OFF/FLUSH profile. This is ordinary SEN_MODE,
 * not a TG DB load, credit return, central reset or permission to rearm.
 * Control lock held; original native lease and stopped request stay pinned.
 */
static int cam_sv_gate_restore_locked(uint64_t epoch)
{
    uint64_t span,end;
    uint32_t target=cam_sv_gate.before[CAM_SV_GATE_SEN];
    unsigned sen=cam_sv_find(0x140),vf=cam_sv_find(0x148),path=cam_sv_find(0x14c);
    int e=0;
    if(cam_sv_gate.restore_attempted)return cam_sv_gate.error?cam_sv_gate.error:ESTALE;
    cam_sv_gate.restore_attempted=true;
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    if(!cam_sv.credit_retired || !cam_sv.close_attempted || cam_sv.epoch!=epoch ||
       !cam_sv_gate.captured || cam_sv_gate.epoch!=epoch || cam_sv_gate.error ||
       cam_sv_gate.phase!=X9_CAM_SV_GATE_STOPPED || cam_sv_gate.pending!=X9_CAM_SV_GATE_STEPS ||
       cam_sv.pending_slot<=CAM_SV_FIELDS || cam_sv.commit_pending ||
       !cam_sv.credit_restore_issued || cam_sv.credit_restore_seen!=3 ||
       sen>=cam_sv.count || vf>=cam_sv.count || path>=cam_sv.count || (target&CAM_SV_GATE_CMOS)) {
        e=ESTALE;goto out;
    }
    e=cam_sv_environment();if(!e)e=cam_irq_credit_retired_guard(epoch);if(e)goto out;
    cam_sv_gate.restore_prior=cam_sv.expected[0][sen];
    for(unsigned bank=0;bank<2;bank++) {
        if(cam_sv.expected[bank][sen]!=cam_sv_gate.restore_prior ||
           ((cam_sv_gate.restore_prior^target)&~(CAM_SV_GATE_TGOFF|CAM_SV_GATE_FLUSH)) ||
           (cam_sv.expected[bank][sen]&CAM_SV_GATE_CMOS) || (cam_sv.expected[bank][vf]&1) ||
           cam_sv.expected[bank][vf]!=cam_sv_gate.before[CAM_SV_GATE_VF] ||
           cam_sv.expected[bank][path]!=cam_sv_gate.before[CAM_SV_GATE_PATH] ||
           cam_sv.expected[bank][cam_sv.field[CAM_SV_FBC]]!=
                cam_sv.before[bank][cam_sv.field[CAM_SV_FBC]]) {e=ESTALE;goto out;}
    }
    e=cam_sv_environment();if(!e)e=cam_irq_credit_retired_guard(epoch);if(e)goto out;
    if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
    cam_sv_gate.phase=X9_CAM_SV_GATE_RESTORING;
    if(cam_sv_gate.restore_prior!=target) {
        cam_sv_gate.restore_issued=true;cam_sv_status(); /* BEFORE sole store */
        cam_rw(cam_sv.view[0],0x140,target);
    }
    for(unsigned poll=0;poll<10000;poll++) {
        bool match=true;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
        e=cam_sv_environment();if(!e)e=cam_irq_credit_retired_guard(epoch);if(e)goto out;
        for(unsigned bank=0;bank<2;bank++) {
            if(cam_sv.status.gate_after[bank][CAM_SV_GATE_SEN]!=target)match=false;
            else cam_sv_gate.restore_seen|=1u<<bank; /* actual no-op also recorded */
        }
        if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
        cam_sv_gate.restore_samples=match?cam_sv_gate.restore_samples+1:0;
        if(cam_sv_gate.restore_samples==2)break;
        delay(1);
    }
    if(cam_sv_gate.restore_samples!=2 || cam_sv_gate.restore_seen!=3){e=ETIMEDOUT;goto out;}
    e=cam_sv_environment();if(!e)e=cam_irq_credit_retired_guard(epoch);
    if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    if(!e)cam_sv_gate.phase=X9_CAM_SV_GATE_RESTORED;
out:
    if(e){cam_sv.terminal=true;cam_sv_fault(e);}
    cam_sv_status();
    printf("X9 CAMERA TG RESTORE: epoch=%llu capture_id=%llu phase=%u issued=%u "
        "seen=%x samples=%u SEN=%x/%x errno=%d (own OFF/FLUSH only; credit history/map retained)\n",
        epoch,cam_sv.request.frame.capture_id,cam_sv_gate.phase,cam_sv_gate.restore_issued,
        cam_sv_gate.restore_seen,cam_sv_gate.restore_samples,
        cam_sv.status.gate_after[0][0],cam_sv.status.gate_after[1][0],e);
    return e;
}
