/* Normal ISP8s FBC1: RCNT[7:0], WCNT[15:8], CNT[19:16], reserved[24:20],
 * DROP[31:25]. One actually accepted credit, one immutable tag0/group0 RAW.
 * Normal SOF reads FBC_CNT (mtk_cam_sv_is_zero_fbc_cnt); CNT may already
 * reach zero at SOF, not necessarily at PASS1_DONE. WCNT and CNT therefore
 * have separate monotonic observation bits; never invent their update order.
 * This is a required count check BEFORE the actual IRQ worker's CPU access,
 * not source enable, RAW/AXI/map drain, or permission to revoke/reuse credit.
 * Every field below is protected by camera_irq_spin. Bind additionally holds
 * control + IRQ mutex; the worker holds only IRQ mutex and never mutates SV.
 */
#define CAM_IRQ_FBC_WCNT 0x0000ff00u
#define CAM_IRQ_FBC_CNT  0x000f0000u
enum { CAM_STREAM_IDLE, CAM_STREAM_ON_PENDING, CAM_STREAM_RUNNING,
    CAM_STREAM_OFF_PENDING, CAM_STREAM_STOPPED };
#define CAM_STREAM_ON 1u
#define CAM_STREAM_OFF 2u
static struct {
    uint64_t epoch;
    struct x9_camera_camsv_request request;
    struct x9_camera_camsv_irq done;
    uint32_t control[2],before[2],target[2],after_control[2],after[2],seen[2],samples;
    bool bound,done_seen,attempted,count_accepted;
    /* Immutable FBC/request binding above also owns these actual AP stream
     * commands. All fields protected by IRQ spin; never caller consumed. */
    uint32_t source_phase,source_issued,source_ack,source_seen,source_samples,source_sampled;
    uint8_t source_control,source_frame_count,source_prior_control;
    uint64_t source_on_time,source_off_time;
    int source_error;
    int error;
} cam_irq_fbc;
static int cam_irq_fbc_fault_locked(int e)
{
    if(!cam_irq_fbc.error)cam_irq_fbc.error=e;
    return cam_irq_fbc.error;
}
/* AFTER all consumer controls are actually accepted, while input remains
 * inactive. A real producer must preserve this boundary before source-on.
 * Capture actual control/counter values twice, not just the software target.
 * No new owner/grant or mutation. The request's native check remains required.
 */
static int cam_irq_fbc_bind_locked(uint64_t epoch)
{
    uint32_t control[2]={0},counter[2]={0};
    boolean_t old;
    int e;
    if(cam_sv_gate.phase!=X9_CAM_SV_GATE_ARMED || cam_sv_gate.error ||
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_ARMED)return ESTALE;
    old=cam_irq_spin_take();e=cam_irq_fbc.bound?EBUSY:0;cam_irq_spin_drop(old);
    if(e)return e; /* no rebasing/rearm before real whole-producer retirement */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_request_guard_locked(epoch,cam_irq.slot);if(e)return e;
        e=cam_irq_credit_guard_locked(epoch);if(e)return e;
        for(unsigned bank=0;bank<2;bank++) {
            e=cam_irq_power();if(e)return e;
            control[bank]=cam_irq_rd(cam_irq.view[bank],0x540);
            counter[bank]=cam_irq_rd(cam_irq.view[bank],0x544);
            if(control[bank]!=cam_sv.expected[bank][cam_sv.field[CAM_SV_FBC]] ||
               counter[bank]!=cam_sv.credit_target[bank] ||
               (counter[bank]&CAM_IRQ_FBC_CNT)!=0x10000u)return ESTALE;
        }
        if(!sample)delay(1);
    }
    e=cam_sv_request_guard_locked(epoch,cam_irq.slot);if(e)return e;
    e=cam_irq_credit_guard_locked(epoch);if(e)return e;
    old=cam_irq_spin_take();
    if(cam_irq_fbc.bound || cam_irq.epoch!=epoch || cam_irq.error || !cam_irq.accepting ||
       cam_irq.sof_seen || cam_irq.busy || cam_irq.count)e=ESTALE;
    else {
        cam_irq_fbc.epoch=epoch;cam_irq_fbc.request=cam_irq.request;
        for(unsigned bank=0;bank<2;bank++) {
            uint32_t wcnt=((counter[bank]>>8)+1u)&0xffu;
            cam_irq_fbc.control[bank]=cam_irq_fbc.after_control[bank]=control[bank];
            cam_irq_fbc.before[bank]=cam_irq_fbc.after[bank]=counter[bank];
            cam_irq_fbc.target[bank]=(counter[bank]&~(CAM_IRQ_FBC_WCNT|CAM_IRQ_FBC_CNT))|(wcnt<<8);
        }
        cam_irq_fbc.bound=true;
    }
    cam_irq_spin_drop(old);
    if(!e)printf("X9 CAMERA FBC BIND: epoch=%llu control=%x/%x accepted=%x/%x "
        "(input inactive; one credit, NOT producer/RAW/AXI/map drain)\n",
        epoch,control[0],control[1],counter[0],counter[1]);
    return e;
}
/* Called ONLY on actual sampled values, under spin, before another sample.
 * Preserve RCNT/DROP/reserved exactly. The two allowed transitions are each
 * one-way; neither a later return to target nor stable unknown values repair
 * a regression. Without an actually issued bound sensor source command all
 * counts stay prior. After issue CNT/WCNT can change BEFORE SOF service;
 * this count evidence does not manufacture SOF or permit CPU delivery.
 */
static int cam_irq_fbc_note_locked(unsigned bank,uint32_t control,uint32_t counter)
{
    uint32_t prior,want,wcnt,cnt;
    cam_irq_fbc.after_control[bank]=control;cam_irq_fbc.after[bank]=counter;
    if(!cam_irq_fbc.bound || cam_irq_fbc.epoch!=cam_irq.epoch || cam_irq_fbc.error ||
       memcmp(&cam_irq_fbc.request,&cam_irq.request,sizeof(cam_irq.request)))
        return cam_irq_fbc_fault_locked(ESTALE);
    if(cam_irq_fbc.source_error)return cam_irq_fbc.source_error;
    prior=cam_irq_fbc.before[bank];want=cam_irq_fbc.target[bank];
    wcnt=counter&CAM_IRQ_FBC_WCNT;cnt=counter&CAM_IRQ_FBC_CNT;
    if(control!=cam_irq_fbc.control[bank] ||
       ((counter^prior)&~(CAM_IRQ_FBC_WCNT|CAM_IRQ_FBC_CNT)) ||
       (wcnt!=(prior&CAM_IRQ_FBC_WCNT) && wcnt!=(want&CAM_IRQ_FBC_WCNT)) ||
       (cnt && cnt!=0x10000u) || (!(cam_irq_fbc.source_issued&CAM_STREAM_ON) && counter!=prior) ||
       ((cam_irq_fbc.seen[bank]&1u) && wcnt!=(want&CAM_IRQ_FBC_WCNT)) ||
       ((cam_irq_fbc.seen[bank]&2u) && cnt))return cam_irq_fbc_fault_locked(ESTALE);
    if(cam_irq_fbc.source_issued&CAM_STREAM_ON) {
        if(wcnt==(want&CAM_IRQ_FBC_WCNT))cam_irq_fbc.seen[bank]|=1u;
        if(!cnt)cam_irq_fbc.seen[bank]|=2u;
    }
    return 0;
}
/* Thread/control guard, read and record under the SAME spin as actual IAR
 * samples. It never updates immutable accepted credit or any SV expected
 * control, never calls a native owner/cache/PTE API under spin, and does
 * not require IRQ accepting after CPU retirement. Whole bus/map retention
 * still holds. Cold/no-source samples retain the original strict guard. */
static int cam_irq_fbc_live_sample(uint64_t epoch,unsigned bank,vm_offset_t view,
    const struct x9_camera_camsv_request *r,uint32_t accepted,uint32_t *value,bool *live)
{
    boolean_t old;
    int e=0;
    *live=false;
    old=cam_irq_spin_take();
    if(cam_irq_fbc.source_issued&CAM_STREAM_ON) {
        uint32_t control;
        *live=true;
        if(bank>=2 || !cam_irq_fbc.bound || epoch!=cam_irq_fbc.epoch || epoch!=cam_irq.epoch ||
           view!=cam_irq.view[bank] || accepted!=cam_irq_fbc.before[bank] ||
           memcmp(r,&cam_irq_fbc.request,sizeof(*r)))e=ESTALE;
        if(!e)e=cam_irq_power();
        if(!e && cam_irq.close_attempted) {
            /* The qualified control stop precedes route/priority unwind.
             * Compare the exact original dirty/uncertain journal here, not
             * the all-published target after OUR first prior restoration.
             * No generic close flag or masked line grants this exception. */
            if(!cam_stream_close_restore_ready())e=ESTALE;
            if(!e)e=cam_irq_controller_impl(true);
            if(!e)e=cam_irq_partial_config();
        } else if(!e)e=cam_irq_config(true);
        if(!e) {
            control=cam_irq_rd(view,0x540);*value=cam_irq_rd(view,0x544);
            e=cam_irq_fbc_note_locked(bank,control,*value);
        }
        if(e && !cam_irq_fbc.error)e=cam_irq_fbc_fault_locked(e);
    }
    cam_irq_spin_drop(old);return e;
}
/* Actual IAR path, spin held. No callback, PTE/cache/RAW access, mutex,
 * status reread/ack, or counter write. The queue retains the raw evidence.
 * A DONE binds the already captured SOF, never a later group/sequence read.
 */
static void cam_irq_fbc_event_locked(struct cam_irq_event *ev)
{
    const struct x9_camera_camsv_irq *f=&ev->frame;
    bool done=ev->role==X9_CAM_IRQ_HYBRID && (f->done_status&0x000f0000u);
    if(!cam_irq_fbc.bound || !(cam_irq_fbc.source_issued&CAM_STREAM_ON) ||
       cam_irq_fbc.epoch!=cam_irq.epoch ||
       f->generation!=cam_irq_fbc.request.frame.generation ||
       f->capture_id!=cam_irq_fbc.request.frame.capture_id || f->first_tag!=1 ||
       f->inner_sequence!=cam_irq_fbc.request.inner_sequence ||
       f->outer_sequence!=cam_irq_fbc.request.inner_sequence) {
        ev->error=cam_irq_fbc_fault_locked(ESTALE);return;
    }
    if(done && (!cam_irq.sof_seen || cam_irq_fbc.done_seen || f->last_tag!=1 ||
       (f->done_status&0x000f0000u)!=0x10000u || f->err_status || (f->cq_status&~1u) ||
       f->time<cam_irq.sof_frame.time || f->group_tags[0]!=1 ||
       f->group_tags[1] || f->group_tags[2] || f->group_tags[3])) {
        ev->error=cam_irq_fbc_fault_locked(EIO);return;
    }
    for(unsigned bank=0;bank<2;bank++) {
        int e=cam_irq_power();
        if(e){ev->error=cam_irq_fbc_fault_locked(e);return;}
        ev->fbc_control[bank]=cam_irq_rd(cam_irq.view[bank],0x540);
        ev->fbc_counter[bank]=cam_irq_rd(cam_irq.view[bank],0x544);
        ev->fbc_sampled|=1u<<bank;
        e=cam_irq_fbc_note_locked(bank,ev->fbc_control[bank],ev->fbc_counter[bank]);
        if(e){ev->error=e;return;}
    }
    if(done){cam_irq_fbc.done=*f;cam_irq_fbc.done_seen=true;}
}
/* The real worker, IRQ mutex held, BEFORE x9_camera_camsv_finish performs
 * CIVAC/CPU copy. No control mutex (stop may drain this worker there), and no
 * SV journal mutation. Counter acceptance remains in the IRQ-private ledger;
 * control guards use its bound live sample only after actual source issue.
 * Sole 10ms read-only observation of the original DONE/credit; no retry/reset,
 * credit return, source disable, CQ/DB write or inference of AXI retirement.
 */
static int cam_irq_fbc_done_locked(const struct cam_irq_event *ev)
{
    const struct x9_camera_camsv_dma *d=&cam_irq.slot->dma;
    uint64_t span,end;
    uint32_t actual[2],seen[2],samples;
    boolean_t old;
    bool accepted=false;
    int e;
    old=cam_irq_spin_take();
    e=cam_irq_fbc.error;
    if(!e)e=cam_irq_fbc.source_error;
    if(!e && (!cam_irq_fbc.bound || !(cam_irq_fbc.source_ack&CAM_STREAM_ON) ||
       !cam_irq_fbc.done_seen || cam_irq_fbc.attempted ||
       cam_irq_fbc.epoch!=cam_irq.epoch || ev->role!=X9_CAM_IRQ_HYBRID || ev->error || ev->fbc_sampled!=3u ||
       memcmp(&cam_irq_fbc.done,&ev->frame,sizeof(ev->frame))))e=ESTALE;
    if(!e)cam_irq_fbc.attempted=true;
    cam_irq_spin_drop(old);if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    for(unsigned poll=0;poll<10000;poll++) {
        bool match=true;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=d->check(d->context,d->lease,&cam_irq.slot->request);if(e)break; /* never under spin */
        if(!e)e=cam_irq_mapping();if(!e)e=cam_irq_power();
        if(!e)e=cam_irq_config(false);if(e)break;
        old=cam_irq_spin_take();
        if(cam_irq.error || !cam_irq.accepting || !cam_irq.sof_seen ||
           !cam_irq.busy || cam_irq.close_attempted)e=cam_irq.error?cam_irq.error:ESTALE;
        for(unsigned bank=0;bank<2 && !e;bank++) {
            uint32_t control,counter;
            e=cam_irq_power();if(e)break;
            control=cam_irq_rd(cam_irq.view[bank],0x540);
            counter=cam_irq_rd(cam_irq.view[bank],0x544);
            e=cam_irq_fbc_note_locked(bank,control,counter);
            if(counter!=cam_irq_fbc.target[bank])match=false;
        }
        if(!e)cam_irq_fbc.samples=match?cam_irq_fbc.samples+1:0;
        samples=cam_irq_fbc.samples;
        cam_irq_spin_drop(old);if(e)break;
        e=d->check(d->context,d->lease,&cam_irq.slot->request);if(e)break;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        if(samples==2){accepted=true;break;}
        delay(1);
    }
    if(!e && !accepted)e=ETIMEDOUT;
    old=cam_irq_spin_take();
    if(!e && cam_irq.error)e=cam_irq.error;
    /* The last owner check may have taken time. Re-read the exact final
     * controls/counters before accepting, without replaying any command.
     * This is still count evidence, not an atomic exclusion of HW writes. */
    if(!e)e=cam_irq_power();
    if(!e)e=cam_irq_config(false);
    for(unsigned bank=0;bank<2 && !e;bank++) {
        uint32_t control=cam_irq_rd(cam_irq.view[bank],0x540);
        uint32_t counter=cam_irq_rd(cam_irq.view[bank],0x544);
        e=cam_irq_fbc_note_locked(bank,control,counter);
        if(!e && counter!=cam_irq_fbc.target[bank])e=ESTALE;
    }
    if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    if(e)e=cam_irq_fbc_fault_locked(e);
    else cam_irq_fbc.count_accepted=true;
    memcpy(actual,cam_irq_fbc.after,sizeof(actual));memcpy(seen,cam_irq_fbc.seen,sizeof(seen));
    samples=cam_irq_fbc.samples;
    cam_irq_spin_drop(old);
    printf("X9 CAMERA DONE FBC: epoch=%llu capture_id=%llu seq=%x count_accepted=%u "
        "actual=%x/%x seen=%x/%x samples=%u errno=%d (NOT RAW/AXI/map drain or credit revoke)\n",
        cam_irq.epoch,ev->frame.capture_id,ev->frame.inner_sequence,!e,actual[0],actual[1],
        seen[0],seen[1],samples,e);
    return e;
}
