/* Native continuous one-credit producer. Register facts: ISP8s
 * mtk_cam_sv_dev_pertag_write_rcnt, toggle_db and mtk_irq_camsv_sof:
 * SOF/CHANNEL are consumed by the actual IAR owner, FBC_EMPTY means no
 * available frame credit; DROP counter is separate from DMA completion.
 * No unmap/clock/source stop is required for same-buffer requeue AFTER
 * the real SW_PASS1_DONE/FBC WCNT+1/CNT0 and per-page CPU retirement.
 * This never describes such a requeue as full AXI/SMI/SMMU retirement.
 * Repeat state/counters are IRQ-spin protected. Control+IRQ mutex serializes
 * preparation; callbacks never execute under spin. Original issued-credit
 * and source-on history are retained, not reset to pretend cold admission.
 */
#include "x9_camera_native_repeat.h"
#define CAM_REPEAT_DROP 0xfe000000u
enum { CAM_REPEAT_NONE, CAM_REPEAT_PARKED, CAM_REPEAT_PREPARING,
    CAM_REPEAT_READY, CAM_REPEAT_POSTED, CAM_REPEAT_LIVE, CAM_REPEAT_FAULT };
static struct {
    uint64_t epoch,completed,submitted,drops;
    unsigned phase;
    bool enabled,admission,prior_done,dma_done_seen;
    uint32_t empty[2],after[2],accepted[2],ack,seen,samples;
    int error;
} cam_repeat;
static int cam_repeat_fault_locked(int e)
{
    if(!cam_repeat.error)cam_repeat.error=e;
    cam_repeat.phase=CAM_REPEAT_FAULT;return cam_repeat.error;
}
/* Record only an actually read CHANNEL DMA_DONE, not PASS1_DONE or a frame.
 * Completion/CPU retirement below supplies the separate prior-credit proof.
 * The non-opted-in one-credit filter is unchanged. */
static int cam_repeat_channel_seen_locked(uint32_t channel)
{
    if(!cam_repeat.enabled || !(channel&4u))return 0;
    if(cam_repeat.dma_done_seen)return cam_repeat_fault_locked(EIO);
    cam_repeat.dma_done_seen=true;return 0;
}
static bool cam_repeat_identity(void)
{
    return cam_repeat.enabled && cam_repeat.epoch==cam_irq.epoch &&
        cam_repeat.epoch==cam_sv.epoch && !cam_irq.close_attempted &&
        !cam_sv.close_attempted && !cam_sv.terminal && !cam_sv.error &&
        !cam_irq_fbc.source_error;
}
static bool cam_repeat_stopped_identity(void)
{
    return cam_repeat.enabled && !cam_repeat.admission &&
        cam_repeat.phase==CAM_REPEAT_PARKED && cam_repeat.epoch==cam_irq.epoch &&
        cam_repeat.epoch==cam_sv.epoch && !cam_repeat.error && !cam_sv.error && !cam_sv.terminal &&
        cam_irq_fbc.done_seen && cam_irq_fbc.count_accepted &&
        cam_irq_fbc.source_phase==CAM_STREAM_STOPPED &&
        cam_irq_fbc.source_issued==(CAM_STREAM_ON|CAM_STREAM_OFF) &&
        cam_irq_fbc.source_ack==(CAM_STREAM_ON|CAM_STREAM_OFF) && !cam_irq_fbc.source_error;
}
/* DROP movement is accepted only in a real no-credit interval. RCNT,
 * WCNT, CNT and reserved stay exact. Each actual sample advances a bounded
 * modulo128 DROP record, not a rewind to a newly guessed baseline. */
static int cam_repeat_empty_note(unsigned bank,uint32_t counter)
{
    if((counter^cam_repeat.after[bank])&~CAM_REPEAT_DROP)return ESTALE;
    unsigned delta=((counter>>25)-(cam_repeat.after[bank]>>25))&127u;
    if(delta>=64)return ESTALE;
    cam_repeat.drops+=delta;cam_repeat.after[bank]=counter;
    return 0;
}
static bool cam_repeat_done_target_locked(unsigned bank,uint32_t counter)
{
    if(counter==cam_irq_fbc.target[bank])return true;
    return (cam_repeat_identity() || cam_repeat_stopped_identity()) &&
        !cam_repeat.error && cam_irq_fbc.count_accepted &&
        !((counter^cam_irq_fbc.target[bank])&~CAM_REPEAT_DROP) &&
        counter==cam_repeat.after[bank];
}
/* Invoked from the existing FBC sampler after request/source identity checks.
 * POSTED accepts actual RCNT advancement even if sensor already consumed
 * CNT1 before the thread sees it. SOF/DONE are still required independently.
 */
static int cam_repeat_note_locked(unsigned bank,uint32_t control,uint32_t counter,bool *handled)
{
    *handled=false;
    if(!cam_repeat.enabled)return 0;
    if(bank>=2)return EINVAL;
    if(cam_sv.credit_retired || (!cam_repeat_identity() && !cam_repeat_stopped_identity()))return 0;
    if(cam_repeat.error){*handled=true;return cam_repeat.error;}
    uint32_t expected=cam_irq_fbc.control[bank];
    bool completed=cam_irq_fbc.count_accepted && cam_irq_fbc.done_seen;
    if(cam_repeat.phase==CAM_REPEAT_NONE && !completed)return 0;
    if(cam_repeat.phase==CAM_REPEAT_LIVE && !completed)return 0;
    *handled=true;
    if(cam_repeat.phase==CAM_REPEAT_NONE || (cam_repeat.phase==CAM_REPEAT_LIVE && completed)) {
        if(control!=expected || ((counter^cam_irq_fbc.target[bank])&~CAM_REPEAT_DROP))return ESTALE;
        /* First completion's DROP may start before the CPU copy ends. Only
         * this actual accepted no-credit target permits that movement. */
        if(!cam_repeat.after[bank])cam_repeat.after[bank]=cam_irq_fbc.target[bank];
        return cam_repeat_empty_note(bank,counter);
    }
    if(cam_repeat.phase==CAM_REPEAT_PARKED || cam_repeat.phase==CAM_REPEAT_PREPARING ||
       cam_repeat.phase==CAM_REPEAT_READY) {
        if(control!=expected)return ESTALE;
        return cam_repeat_empty_note(bank,counter);
    }
    if(cam_repeat.phase!=CAM_REPEAT_POSTED)return ESTALE;
    uint32_t bit=1u<<bank;
    if(control==expected)cam_repeat.ack|=bit;
    else if((cam_repeat.ack&bit) || control!=(expected|CAM_SV_FBC_RCNT))return ESTALE;
    uint32_t old=cam_repeat.empty[bank],rcnt=(old+1)&255u;
    if((counter&255u)==(old&255u)) {
        if(cam_repeat.seen&bit)return ESTALE;
        return cam_repeat_empty_note(bank,counter);
    }
    if((counter&255u)!=rcnt || ((counter^old)&0x01f00000u))return ESTALE;
    unsigned wc=(counter>>8)&255u,old_wc=(old>>8)&255u,cnt=(counter>>16)&15u;
    if((wc!=old_wc && wc!=((old_wc+1)&255u)) || cnt>1)return ESTALE;
    if(!(cam_repeat.seen&bit)) {
        unsigned delta=((counter>>25)-(cam_repeat.after[bank]>>25))&127u;
        if(delta>=64)return ESTALE;
        cam_repeat.drops+=delta;
        cam_repeat.accepted[bank]=(counter&~(0x000fffffu))|rcnt|(old_wc<<8)|0x10000u;
        /* Expected credit target is a computed criterion, not a fabricated
         * observed CNT1: actual RCNT advancement is recorded in seen. */
        cam_irq_fbc.before[bank]=cam_repeat.accepted[bank];
        cam_irq_fbc.target[bank]=(cam_repeat.accepted[bank]&~(CAM_IRQ_FBC_CNT|CAM_IRQ_FBC_WCNT))|
            (((old_wc+1)&255u)<<8);
        cam_sv.credit_target[bank]=cam_repeat.accepted[bank];
        cam_repeat.seen|=bit;
    }
    if((counter^cam_repeat.accepted[bank])&~(CAM_IRQ_FBC_WCNT|CAM_IRQ_FBC_CNT))return ESTALE;
    if((cam_irq_fbc.seen[bank]&1u) && wc!=((old_wc+1)&255u))return ESTALE;
    if((cam_irq_fbc.seen[bank]&2u) && cnt)return ESTALE;
    if(wc==((old_wc+1)&255u))cam_irq_fbc.seen[bank]|=1u;
    if(!cnt)cam_irq_fbc.seen[bank]|=2u;
    cam_repeat.after[bank]=counter;
    if(cam_repeat.seen==3 && cam_repeat.ack==3)cam_repeat.phase=CAM_REPEAT_LIVE;
    return 0;
}
static int cam_repeat_fbc_live(unsigned bank,uint32_t *value,bool *handled)
{
    boolean_t old=cam_irq_spin_take();
    int e=0;*handled=false;
    if((cam_repeat_identity() || cam_repeat_stopped_identity()) &&
       cam_repeat.phase>=CAM_REPEAT_PARKED && !cam_sv.credit_retired) {
        *handled=true;e=cam_irq_power();
        if(!e) {
            uint32_t control=cam_irq_rd(cam_irq.view[bank],0x540);
            *value=cam_irq_rd(cam_irq.view[bank],0x544);
            e=cam_irq_fbc_note_locked(bank,control,*value);
        }
        if(e)e=cam_repeat_fault_locked(e);
    }
    cam_irq_spin_drop(old);return e;
}
static uint32_t cam_repeat_fbc_busy(void)
{
    boolean_t old=cam_irq_spin_take();
    uint32_t mask=cam_repeat_identity() && cam_repeat.phase==CAM_REPEAT_POSTED &&
        !cam_repeat.error?CAM_SV_FBC_RCNT:0;
    cam_irq_spin_drop(old);return mask;
}
static uint32_t cam_repeat_fbc_compare(unsigned bank,uint32_t value)
{
    boolean_t old=cam_irq_spin_take();
    if(bank<2 && cam_repeat_identity() && cam_repeat.phase==CAM_REPEAT_POSTED &&
       !cam_repeat.error && !(cam_repeat.ack&(1u<<bank)) &&
       value==(cam_sv.target[CAM_SV_FBC]|CAM_SV_FBC_RCNT))value&=~CAM_SV_FBC_RCNT;
    cam_irq_spin_drop(old);return value;
}
/* The existing worker calls this only for a actually RETIRED slot after
 * RAW CPU→IDLE. Busy remains set until its real unlock/wakeup afterwards. */
static void cam_repeat_completed_locked(void)
{
    if(!cam_repeat_identity() || cam_repeat.error || !cam_irq_fbc.done_seen ||
       !cam_irq_fbc.count_accepted || cam_irq.slot->state!=X9_CAMSV_RETIRED)return;
    if(cam_repeat.completed==UINT64_MAX){cam_repeat_fault_locked(EOVERFLOW);return;}
    for(unsigned b=0;b<2;b++) {
        cam_repeat.empty[b]=cam_repeat.after[b]=cam_irq_fbc.after[b];
        if(!cam_repeat_done_target_locked(b,cam_repeat.after[b])) {
            cam_repeat_fault_locked(ESTALE);return;
        }
    }
    cam_repeat.completed++;cam_repeat.prior_done=true;cam_repeat.phase=CAM_REPEAT_PARKED;
}
/* Only the actual IAR-owned parked source. No arbitrary R-C sweep or pending
 * clear is performed. Consume genuine no-credit SOF/CHANNEL, as the vendor
 * fbc_empty/drop path does, never publish it as a RAW frame. Hybrid errors
 * PASS1_DONE/errors are terminal. One actual late CHANNEL DMA_DONE may
 * belong to the independently completed prior credit; never a new frame.
 * Repeated/unknown DMA_DONE and debug remain terminal. */
static bool cam_repeat_idle_irq_locked(unsigned role,int *error)
{
    *error=0;
    if(!cam_repeat_identity() || cam_repeat.phase<CAM_REPEAT_PARKED ||
       cam_repeat.phase>CAM_REPEAT_POSTED)return false;
    int e=cam_irq_power();if(!e)e=cam_irq_config(true);
    if(!e && cam_repeat.phase==CAM_REPEAT_POSTED) {
        if(role!=X9_CAM_IRQ_SOF)return false;
        bool precredit=false;
        for(unsigned bank=0;bank<2;bank++) {
            uint32_t ctl=cam_irq_rd(cam_irq.view[bank],0x540);
            uint32_t count=cam_irq_rd(cam_irq.view[bank],0x544);
            e=cam_irq_fbc_note_locked(bank,ctl,count);
            if(e)break;
            if((count&255u)==(cam_repeat.empty[bank]&255u))precredit=true;
        }
        /* A genuine SOF while hardware still has no posted RCNT is a drop,
         * not the next RAW SOF. Leave its read-clear history for the normal
         * path only when both banks have actually advanced our RCNT. */
        if(!e && !precredit)return false;
    }
    bool actual_sof=false;
    if(!e && role==X9_CAM_IRQ_SOF) {
        uint32_t sof=cam_irq_rd(cam_irq.view[0],0x360);
        uint32_t channel=cam_irq_rd(cam_irq.view[0],0x370);
        /* tag0 SOF bit2, SW_ENQ_ERR bit0. CHANNEL-only IAR is a genuine
         * vendor event but cannot publish a new RCNT. No status is replayed.
         * DMA_DONE is NOT a drop: only one receipt for our prior completed
         * credit while no next credit has been issued may be consumed. */
        if((sof && sof!=4) || (channel&~5u) || (!sof&&!channel))e=EIO;
        if(!e && (channel&4u)) {
            if(!cam_repeat.prior_done || !cam_repeat.completed ||
               cam_repeat.phase==CAM_REPEAT_POSTED)e=EIO;
            else e=cam_repeat_channel_seen_locked(channel);
        }
        actual_sof=sof==4;
    } else if(!e && role==X9_CAM_IRQ_HYBRID) {
        uint32_t err=cam_irq_rd(cam_irq.view[1],0x350);
        uint32_t done=cam_irq_rd(cam_irq.view[0],0x348);
        uint32_t cq=cam_irq_rd(cam_irq.view[0],0x201c);
        if(err || done || (cq&~1u))e=EIO;
    } else if(!e)e=EIO;
    for(unsigned bank=0;bank<2 && !e;bank++) {
        uint32_t ctl=cam_irq_rd(cam_irq.view[bank],0x540);
        uint32_t count=cam_irq_rd(cam_irq.view[bank],0x544);
        bool handled=false;e=cam_repeat_note_locked(bank,ctl,count,&handled);
        if(!e && !handled)e=ESTALE;
    }
    if(!e && actual_sof && cam_repeat.phase==CAM_REPEAT_READY && cam_repeat.admission) {
        /* A genuine empty SOF history was just consumed. Publish exactly
         * one credit here, not from a thread racing a stale pending SOF.
         * Source ON history is unchanged; only the next request changes. */
        cam_repeat.empty[0]=cam_repeat.after[0];cam_repeat.empty[1]=cam_repeat.after[1];
        cam_repeat.phase=CAM_REPEAT_POSTED;cam_repeat.ack=cam_repeat.seen=0;
        cam_repeat.prior_done=false;cam_repeat.dma_done_seen=false;
        cam_irq.accepting=true;cam_irq.sof_seen=false;
        bzero(&cam_irq.sof_frame,sizeof(cam_irq.sof_frame));
        cam_rw(cam_sv.view[0],0x540,cam_sv.target[CAM_SV_FBC]|CAM_SV_FBC_RCNT);
        cam_repeat.submitted++;
    }
    if(e){*error=cam_repeat_fault_locked(e);return true;}
    /* Re-enable only the consumed line. Its current IAR is still ACTIVE,
     * sleh performs the unchanged single EOI; next actual SOF may retrigger. */
    cam_irq_wr(cam_irq_bank(role,0x100),cam_irq_bit(role));
    return true;
}
static void cam_repeat_stop_locked(uint64_t epoch)
{
    boolean_t old=cam_irq_spin_take();
    if(cam_repeat.enabled && cam_repeat.epoch==epoch)cam_repeat.admission=false;
    cam_irq_spin_drop(old);
}
static int cam_repeat_enable_locked(uint64_t epoch)
{
    int e;
    lck_mtx_lock(&camera_irq_lock);
    boolean_t old=cam_irq_spin_take();
    e=cam_repeat.enabled || epoch!=cam_irq.epoch || !cam_irq.prepared ||
        !cam_irq.owns || !cam_irq.accepting || cam_irq.close_attempted ||
        !cam_irq_fbc.bound || cam_irq_fbc.source_issued || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED?
        ESTALE:cam_irq.error;
    if(!e){cam_repeat.enabled=cam_repeat.admission=true;cam_repeat.epoch=epoch;}
    cam_irq_spin_drop(old);
    lck_mtx_unlock(&camera_irq_lock);return e;
}
int x9_camera_native_repeat_enable(uint64_t epoch)
{
    lck_mtx_lock(&camera_control_lock);
    int e=cam_repeat_enable_locked(epoch);
    lck_mtx_unlock(&camera_control_lock);return e;
}
/* Caller holds camera_control_lock + camera_irq_lock, NOT IRQ spin.
 * A real completed/retired CPU boundary for AP group-held exposure updates
 * or the next arm. PARKED forbids a READY/preparing/posted credit; actual
 * worker retirement is checked separately from the slot's RAW_IDLE state.
 * The source and namespace intentionally remain live. This does NOT prove
 * whole AXI/SMI/SMMU drain, permit unmapping, or cancel any hardware credit.
 * Mutex/callback/cache/PTE work stays outside the two short spin sections.
 */
static int cam_repeat_cpu_boundary_locked(uint64_t epoch)
{
    int e;
    boolean_t old=cam_irq_spin_take();
    e=cam_repeat.error?cam_repeat.error:cam_irq.error?cam_irq.error:
        cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
    if(!e && (!cam_repeat_identity() || !cam_repeat.admission || cam_repeat.epoch!=epoch ||
       cam_repeat.phase!=CAM_REPEAT_PARKED || !cam_repeat.completed ||
       !cam_irq.prepared || !cam_irq.owns || cam_irq.accepting || cam_irq.busy || cam_irq.count ||
       !cam_irq_fbc.bound || !cam_irq_fbc.done_seen || !cam_irq_fbc.count_accepted ||
       cam_irq_fbc.epoch!=epoch || cam_irq_fbc.source_phase!=CAM_STREAM_RUNNING ||
       cam_irq_fbc.source_issued!=CAM_STREAM_ON || cam_irq_fbc.source_ack!=CAM_STREAM_ON ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED || cam_sv.credit_retired))e=ESTALE;
    cam_irq_spin_drop(old);if(e)return e;
    e=cam_irq_credit_retired_slot(); /* real RETIRED + S1 RAW_IDLE, not bus drain */
    if(!e)e=cam_sv_request_guard_locked(epoch,cam_irq.slot);
    if(!e)e=cam_irq_mapping();if(!e)e=cam_irq_power();if(!e)e=cam_irq_config(false);
    if(e)return e;
    old=cam_irq_spin_take();
    e=cam_repeat.error?cam_repeat.error:cam_irq.error?cam_irq.error:
        cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
    if(!e && (!cam_repeat_identity() || !cam_repeat.admission || cam_repeat.epoch!=epoch ||
       cam_repeat.phase!=CAM_REPEAT_PARKED || cam_irq.accepting || cam_irq.busy || cam_irq.count ||
       !cam_irq_fbc.done_seen || !cam_irq_fbc.count_accepted ||
       cam_irq_fbc.source_phase!=CAM_STREAM_RUNNING || cam_irq_fbc.source_issued!=CAM_STREAM_ON ||
       cam_irq_fbc.source_ack!=CAM_STREAM_ON))e=ESTALE;
    for(unsigned bank=0;bank<2 && !e;bank++) {
        e=cam_irq_power();if(e)break;
        uint32_t control=cam_irq_rd(cam_irq.view[bank],0x540);
        uint32_t counter=cam_irq_rd(cam_irq.view[bank],0x544);
        e=cam_irq_fbc_note_locked(bank,control,counter);
        if(!e && !cam_repeat_done_target_locked(bank,counter))e=ESTALE;
    }
    cam_irq_spin_drop(old);return e;
}
/* Source OFF has really ACKed. Drain no-credit SOF/CHANNEL accumulated
 * while the completed CPU slot was parked/masked, through actual IAR/EOI,
 * never ICPENDR/ICACTIVER or a blind R-C sweep. Control + IRQ mutex held. */
static int cam_repeat_stopped_irqs_locked(uint64_t epoch)
{
    uint64_t wait_span;nanoseconds_to_absolutetime(500000000,&wait_span);
    uint64_t wait_end=mach_absolute_time()+wait_span;
    /* A real already-issued request may finish while core STOP has
     * invalidated delivery. Leave its SOF/DONE/worker binding untouched,
     * release IRQ mutex for the worker and wait for actual CPU retirement.
     * Never turn queue-empty or SOURCE_OFF into a fabricated completion. */
    for(;;) {
        boolean_t wait_old=cam_irq_spin_take();
        if(!cam_repeat.enabled){cam_irq_spin_drop(wait_old);return 0;}
        int wait_error=cam_repeat.error?cam_repeat.error:cam_irq.error;
        if(!wait_error && (cam_repeat.epoch!=epoch || cam_repeat.admission ||
           cam_irq.close_attempted || cam_sv.close_attempted ||
           cam_irq_fbc.source_phase!=CAM_STREAM_STOPPED ||
           cam_irq_fbc.source_issued!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
           cam_irq_fbc.source_ack!=(CAM_STREAM_ON|CAM_STREAM_OFF)))wait_error=ESTALE;
        bool ready=cam_repeat.phase==CAM_REPEAT_PARKED && !cam_irq.busy && !cam_irq.count;
        if(!wait_error && (cam_repeat.phase==CAM_REPEAT_READY || cam_repeat.phase==CAM_REPEAT_PREPARING))
            wait_error=ENOTSUP; /* never-issued DEVICE slot has a different cancellation proof */
        if(!wait_error && !ready && mach_absolute_time()>=wait_end)wait_error=ETIMEDOUT;
        if(wait_error || ready){cam_irq_spin_drop(wait_old);if(wait_error)return wait_error;break;}
        assert_wait_deadline((event_t)&cam_repeat.completed,THREAD_UNINT,wait_end);
        cam_irq_spin_drop(wait_old);lck_mtx_unlock(&camera_irq_lock);
        thread_block(THREAD_CONTINUE_NULL);lck_mtx_lock(&camera_irq_lock);
    }
    boolean_t old=cam_irq_spin_take();
    if(!cam_repeat.enabled){cam_irq_spin_drop(old);return 0;}
    int e=cam_repeat.error?cam_repeat.error:cam_irq.error;
    if(!e && (!cam_repeat_stopped_identity() || cam_repeat.epoch!=epoch ||
       cam_irq.accepting || cam_irq.count || cam_irq.busy || !cam_irq.owns))e=ESTALE;
    if(!e)for(unsigned role=0;role<3;role++)cam_irq_wr(cam_irq_bank(role,0x100),cam_irq_bit(role));
    cam_irq_spin_drop(old);if(e)return e;
    uint64_t span;nanoseconds_to_absolutetime(10000000,&span);
    uint64_t end=mach_absolute_time()+span;unsigned samples=0;
    for(unsigned poll=0;poll<5000;poll++) {
        e=cam_irq_power();if(!e)e=cam_irq_config(false);if(e)break;
        old=cam_irq_spin_take();
        e=cam_repeat.error?cam_repeat.error:cam_irq.error;
        bool pending=false;
        for(unsigned role=0;role<3 && !e;role++)
            if((cam_irq_rd(cam_irq.gic,cam_irq_bank(role,0x200))|
                cam_irq_rd(cam_irq.gic,cam_irq_bank(role,0x300)))&cam_irq_bit(role))pending=true;
        samples=!e&&!pending?samples+1:0;
        cam_irq_spin_drop(old);
        if(e || samples==2)break;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        delay(2);
    }
    if(!e && samples!=2)e=ETIMEDOUT;
    old=cam_irq_spin_take();cam_irq_mask_all();
    if(e)cam_repeat_fault_locked(e);cam_irq_spin_drop(old);
    if(!e)e=cam_irq_wait_rwp();if(!e)e=cam_irq_wait_idle();return e;
}
/* Real continuous same-buffer requeue. The native controller creates its
 * next request with actual exposure/gain; sequence/capture_id strictly move
 * forward through x9_camera_camsv_arm and per-page DEVICE synchronization. */
/* Control + IRQ mutex held. The optional native-controller publication
 * callback runs AFTER every old-request boundary check, BEFORE arm first
 * calls the new immutable DMA check. It runs outside IRQ spin and slot/S1
 * locks; it may publish the next request, never grant mapping authority.
 * A failed publication/arm is terminal retained; there is no rollback to
 * pretend the old request remains the only device owner. */
static int cam_irq_repeat_publish_locked(uint64_t epoch,const struct x9_camera_camsv_request *r,
    const struct x9_camera_camsv_dma *d,
    int (*publish)(void *,const struct x9_camera_camsv_request *,const struct x9_camera_camsv_dma *),
    void *context)
{
    int e;
    if(!r || !d)return EINVAL;
    boolean_t old;
    e=cam_repeat_cpu_boundary_locked(epoch);
    lck_mtx_lock(&camera_lock);
    if(!e && (!(st.flags&X9_CAMERA_STREAMING) || (st.flags&X9_CAMERA_FAULT) ||
        st.generation!=r->frame.generation || st.sensor!=r->frame.sensor))e=ESTALE;
    lck_mtx_unlock(&camera_lock);
    if(!e && (r->frame.generation!=cam_sv.request.frame.generation || r->iova!=cam_sv.request.iova ||
        r->frame.sensor!=cam_sv.request.frame.sensor || d->lease!=cam_sv.request_dma.lease ||
        d->context!=cam_sv.request_dma.context || d->check!=cam_sv.request_dma.check ||
        d->backing!=cam_sv.request_dma.backing))e=ESTALE;
    if(!e) {
        old=cam_irq_spin_take();cam_repeat.phase=CAM_REPEAT_PREPARING;cam_irq_spin_drop(old);
        if(publish)e=publish(context,r,d);
        if(!e)e=x9_camera_camsv_arm(cam_irq.slot,r,d);
    }
    if(!e) {
        /* Keep original credit_issued/ACCEPTED and all source/receive/GIC
         * history. Bind the new already-device-owned slot before stores. */
        cam_sv.request=*r;cam_sv.request_dma=*d;cam_sv.request_ready=false;
        cam_sv.target[CAM_SV_ADDR_LO]=(uint32_t)r->iova;
        cam_sv.target[CAM_SV_ADDR_HI]=(uint32_t)(r->iova>>32);
        cam_sv.target[CAM_SV_FH]=r->inner_sequence;
        bool changed=false;
        for(unsigned f=CAM_SV_ADDR_LO;f<=CAM_SV_FH && !e;f++) {
            if(cam_sv.expected[0][cam_sv.field[f]]!=cam_sv.target[f])changed=true;
            e=cam_sv_field_write(f,cam_sv.target[f]);
        }
        if(!e)e=changed?cam_sv_commit():cam_sv_target_observe();
        if(!e)e=cam_sv_environment();
        if(!e)cam_sv.request_ready=true;
    }
    if(!e) {
        old=cam_irq_spin_take();
        /* Source control/ACK/issued/time stay authentic and unchanged. */
        cam_irq.request=*r;cam_irq_fbc.request=*r;
        bzero(&cam_irq_fbc.done,sizeof(cam_irq_fbc.done));
        cam_irq_fbc.done_seen=cam_irq_fbc.attempted=cam_irq_fbc.count_accepted=false;
        cam_irq_fbc.samples=0;cam_irq_fbc.seen[0]=cam_irq_fbc.seen[1]=0;
        cam_irq.sof_seen=false;bzero(&cam_irq.sof_frame,sizeof(cam_irq.sof_frame));
        cam_repeat.phase=CAM_REPEAT_READY;
        /* Parked SOF/CHANNEL are consumed by their actual IAR owner. No
         * blind ICPENDR/status read; the next empty SOF issues sole RCNT. */
        for(unsigned role=0;role<3;role++)cam_irq_wr(cam_irq_bank(role,0x100),cam_irq_bit(role));
        cam_irq_spin_drop(old);
        e=cam_irq_wait_rwp();
    }
    if(e) {
        old=cam_irq_spin_take();cam_repeat_fault_locked(e);cam_irq_spin_drop(old);
        e=cam_irq_fault(e);
        lck_mtx_lock(&camera_camsv_lock);cam_camsv_retain(cam_irq.slot,e);lck_mtx_unlock(&camera_camsv_lock);
    }
    cam_sv_status();
    printf("X9 CAMERA REPEAT: epoch=%llu completed=%llu submitted=%llu capture_id=%llu "
        "phase=%u drops=%llu errno=%d (live source/domain; preparation not frame success)\n",
        epoch,cam_repeat.completed,cam_repeat.submitted,r->frame.capture_id,cam_repeat.phase,cam_repeat.drops,e);
    return e;
}
static int cam_irq_repeat_locked(uint64_t epoch,const struct x9_camera_camsv_request *r,
    const struct x9_camera_camsv_dma *d)
{return cam_irq_repeat_publish_locked(epoch,r,d,NULL,NULL);}
int x9_camera_native_repeat(uint64_t epoch,const struct x9_camera_camsv_request *r,
    const struct x9_camera_camsv_dma *d)
{
    if(!r || !d)return EINVAL;
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_irq_lock);
    int e=cam_irq_repeat_locked(epoch,r,d);
    lck_mtx_unlock(&camera_irq_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
