/* Actual first-request composition, not a provider-registration shortcut.
 * Include after IRQ and the native AE implementation. Control callbacks run
 * under camera_control_lock; the CAMSV worker's lease check does NOT acquire
 * that mutex, camera_irq_lock or camera_camsv_lock. Exported storage stays
 * pinned forever; its real MM token is held until proven retirement, including
 * prepare failures before core capture=true. Source/IRQ stop and translation restore
 * are distinct stages; only the latter admits the old supplier teardown.
 */
#include "x9_camera_native_capture.h"
#include "x9_camera_native_smmu.h"
#include "x9_camera_native_ae.h"
LCK_MTX_DECLARE(camera_native_capture_lock,&camera_group);

/* This is an allocation in our cold SID80/SSID0/ASID1 namespace, NOT CPU PA
 * identity mapping or a reservation in Android's inherited mm_common tree.
 * live.dts mm_smmu_cam is SID80, 16GiB aperture. mm_common excludes the
 * fc910000..ffffefff framebuffer carveout; this private slot is below it.
 * The native host refuses every configured normal bank and preserves the
 * complete prior abort/bypass policy for EVERY unrelated SID. */
#define CAM_NATIVE_IOVA 0x40000000ull
enum cam_native_capture_phase {
    CAM_CAPTURE_EMPTY, CAM_CAPTURE_PREPARING, CAM_CAPTURE_LIVE,
    CAM_CAPTURE_LOCAL_STOPPED, CAM_CAPTURE_TRANSLATION_RETIRED,
    CAM_CAPTURE_UNSTARTED_RETIRED, CAM_CAPTURE_LOCAL_CANCELLED_STOPPED,
    CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED
};
struct cam_native_capture_context {
    uint64_t epoch,power_token,generation,capture_serial,lease;
    uint32_t sensor,inner_serial,reservation_bytes,phase;
    bool prepare_attempted,prepared,reserved,request_held,on_attempted,off_attempted;
    bool controller_started,controller_busy,repeat_admission;
    uint64_t observed_completed;
    int error,off_error;
    struct x9_camera_pipeline plan;
    struct x9_camera_smmu *smmu;
    struct x9_camera_smmu_export view;
    struct x9_camera_camsv_request request;
};
static struct cam_native_capture_context cam_native_capture;
/* Permanent immutable receipts, separate from the next active context.
 * Native namespace admission bounds published epochs; no exported RAW,
 * descriptor, queue or receipt is freed when a sensor is switched. */
struct cam_native_capture_receipt {
    struct cam_native_capture_context owner;
    struct cam_native_capture_receipt *next;
};
static struct cam_native_capture_receipt *cam_native_capture_history;
static int cam_native_capture_next_locked(uint64_t);
static int cam_native_capture_dma_check(void *,uint64_t,const struct x9_camera_camsv_request *);
static void cam_native_capture_controller(void *,wait_result_t);

static int cam_native_capture_fault(struct cam_native_capture_context *c,int e)
{
    lck_mtx_lock(&camera_native_capture_lock);
    if(!c->error)c->error=e;
    e=c->error;
    lck_mtx_unlock(&camera_native_capture_lock);
    return e;
}

static int cam_native_capture_sensor(uint64_t epoch,const struct x9_camera_sensor *s)
{
    int e;
    if(!s || s->sensor<X9_CAMERA_BACK || s->sensor>X9_CAMERA_FRONT ||
       s!=&sensors[s->sensor-1])return EINVAL;
    e=cam_sensor_session_match(epoch);if(e)return e;
    if(cam_sensor_live.probe.sensor!=s->sensor)return ESTALE;
    if(cam_sensor_live.error)return cam_sensor_live.error;
    return cam_sensor_live.phase==CAM_SENSOR_CONFIGURED?0:ESTALE;
}

/* Necessary route observations with NO request-authority recursion. Calling
 * cam_out_guard here would recurse: OUTMUX -> source commit -> SV authority
 * -> this check. Only our exact pending/source transition journal may span
 * previous/target; unrelated controls/unused tags remain strict in both banks.
 * No read-clear status, sensor transfer, RAW read or hardware write here. */
static int cam_native_capture_route_guard(uint64_t epoch)
{
    uint32_t v[CAM_OUT_REGS];
    int e;
    if(!cam_rx_sensor_held_locked(epoch) || cam_rx_live.close_attempted || cam_rx_live.error ||
       !cam_mac.held || cam_mac.epoch!=epoch || cam_mac.close_attempted || cam_mac.error ||
       !cam_phy.held || cam_phy.epoch!=epoch || cam_phy.close_attempted || cam_phy.error ||
       !cam_out.held || cam_out.epoch!=epoch || cam_out.close_attempted || cam_out.error ||
       !cam_out.consumed || cam_out_irq.poison || !cam_out_irq.mode_ready ||
       !cam_smi.held || cam_smi.epoch!=epoch || cam_smi.close_attempted || cam_smi.error ||
       !cam_sv.held || cam_sv.epoch!=epoch || cam_sv.close_attempted || cam_sv.error)return ESTALE;
    e=cam_out_top_guard(false);if(e)return e;
    e=cam_smi_clock_guard();if(e)return e;
    e=cam_mac_guard();if(e)return e;
    e=cam_phy_guard();if(e)return e;
    e=cam_sv_guard();if(e)return e;
    e=cam_out_irq_control_guard();if(e)return e;
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
        uint32_t pending=cam_rr(cam_out.out[bank],0x1c)|cam_rr(cam_out.out[bank],0x38);
        if(pending&~1u)return EIO;
        if((!cam_out.commit_pending || cam_out_irq_inner_target_required()) && (pending&1))return EBUSY;
    }
    return 0;
}

/* This is authority for the exact completed OLD request during supplier
 * restore, not permission for DEVICE/cache/credit operations. Callers can
 * already own the CAMSV mutex, so do not acquire it here. Control owns the
 * teardown and the real filter/worker receipts make the immutable slot stable.
 * Require RAW_IDLE and the exact old identity: arm changes slot->request
 * before invoking its callback, and even a replay of that identity is rejected
 * by CAMSV's monotonic generation/capture/cookie rules before this callback.
 * No RAW read, MMIO mutation, controller reset or software drained assertion.
 */
static int cam_native_capture_old_teardown(uint64_t epoch,uint64_t token,
    struct x9_camera_smmu *m,const struct x9_camera_camsv_request *r,uint64_t lease)
{
    struct x9_camera_camsv_slot *slot;
    int e;
    boolean_t old=cam_irq_spin_take();
    slot=cam_irq.slot;
    e=cam_irq.error?cam_irq.error:cam_irq.epoch!=epoch || cam_irq.prepared ||
        !cam_irq.close_attempted || cam_irq.owns || cam_irq.accepting ||
        cam_irq.busy || cam_irq.count || !slot || slot!=cam_sv.request_slot ||
        slot->state!=X9_CAMSV_RETIRED || memcmp(&slot->request,r,sizeof(*r)) ||
        memcmp(&cam_sv.request,r,sizeof(*r)) || !cam_sv.credit_retired ||
        !cam_sv.request_held || cam_sv.epoch!=epoch || cam_sv.terminal || cam_sv.error ||
        slot->dma.backing!=m->map || slot->dma.context!=&cam_native_capture ||
        slot->dma.lease!=lease || slot->dma.check!=cam_native_capture_dma_check?ESTALE:0;
    cam_irq_spin_drop(old);if(e)return e;
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_check_private(m->map);
    if(!e && (m->map->raw_errno || !m->map->published ||
        m->map->raw_state!=CAM_RAW_IDLE || !cam_s1_dma_match(m->map,slot)))
        e=m->map->raw_errno?m->map->raw_errno:ESTALE;
    lck_mtx_unlock(&camera_s1_lock);if(e)return e;
    if(!cam_mm.active || cam_mm.receive.power_token!=token ||
       m->native_power!=token || cam_native_capture.controller_busy ||
       cam_native_capture.repeat_admission)return ESTALE;
    return x9_camera_native_smmu_retirement_check(epoch,m);
}

/* A prepared NEXT request which never received RCNT is not a DONE frame.
 * This separate scope requires the native independent old actual completion
 * plus exact NEW never-issued cancel journal, CANCELLED slot/RAW_CANCELLED
 * and retired filter/worker; it grants no CPU cache access, rearm or delivery.
 * The original new immutable metadata stays time/sof_time=0. */
static int cam_native_capture_cancel_teardown(uint64_t epoch,uint64_t token,
    struct x9_camera_smmu *m,const struct x9_camera_camsv_request *r,uint32_t phase)
{
    if(phase!=CAM_CAPTURE_LOCAL_CANCELLED_STOPPED &&
       phase!=CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED)return ESTALE;
    if(!cam_mm.active || cam_mm.receive.power_token!=token ||
       m->native_power!=token || cam_native_capture.controller_busy ||
       cam_native_capture.repeat_admission)return ESTALE;
    int e=cam_repeat_cancelled_check_locked(epoch,r,m->map);
    if(e)return e;
    /* Local cancel has retired the real producer/worker/unused request,
     * but BBM has not started: validate actual LIVE translation, not a
     * future restored bank. Only the fully restored phase changes scope. */
    return phase==CAM_CAPTURE_LOCAL_CANCELLED_STOPPED?x9_camera_native_smmu_check(m):
        x9_camera_native_smmu_retirement_check(epoch,m);
}

static int cam_native_capture_dma_check(void *context,uint64_t lease,
    const struct x9_camera_camsv_request *r)
{
    struct cam_native_capture_context *c=context;
    struct x9_camera_smmu *smmu;
    struct x9_camera_smmu_export view;
    uint64_t epoch,token;
    uint32_t phase;
    int e;
    if(c!=&cam_native_capture || !r || !lease)return EINVAL;
    /* Never keep this mutex while invoking another component. A completion
     * pins the immutable request/namespace; STOP cannot free or rebind them. */
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->error;
    if(!e && (!c->prepared || !c->reserved || !c->request_held ||
       !c->smmu || c->lease!=lease || memcmp(r,&c->request,sizeof(*r))))e=ESTALE;
    epoch=c->epoch;token=c->power_token;smmu=c->smmu;view=c->view;phase=c->phase;
    if(!e && (!epoch || !token || r->frame.sensor!=c->sensor ||
       r->frame.generation!=c->generation || r->iova!=CAM_NATIVE_IOVA ||
       r->iova!=view.map.iova || r->frame.raw!=view.map.raw ||
       r->frame.bytes!=view.map.bytes || view.map.mapped_bytes!=c->reservation_bytes ||
       view.asid!=CAM_SMMU_ASID || !view.map.backing))e=ESTALE;
    lck_mtx_unlock(&camera_native_capture_lock);
    if(e)return e;
    if(phase==CAM_CAPTURE_TRANSLATION_RETIRED)
        return cam_native_capture_old_teardown(epoch,token,smmu,r,lease);
    if(phase==CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED ||
       phase==CAM_CAPTURE_LOCAL_CANCELLED_STOPPED)
        return cam_native_capture_cancel_teardown(epoch,token,smmu,r,phase);
    if(phase!=CAM_CAPTURE_LIVE && phase!=CAM_CAPTURE_LOCAL_STOPPED)return ESTALE;
    /* These are live consumed dependencies, not a cached MM_GET/idle grant.
     * The normal host re-walks the whole owned S1/CD/STE/queue storage and
     * verifies exact own controller configuration/producers and real power. */
    if(cam_sensor_live.epoch!=epoch || cam_sensor_live.close_attempted ||
       cam_mm.receive.power_token!=token || !cam_mm.active)return ESTALE;
    e=x9_mm_power_validate(token,X9_MM_POWER_CAMERA);
    if(!e)e=cam_mm_session_check(cam_rx_live.epoch);
    if(!e)e=cam_native_source_rate_guard_locked(epoch);
    if(!e)e=cam_native_capture_route_guard(epoch);
    if(!e)e=x9_camera_native_smmu_check(smmu);
    if(e)return cam_native_capture_fault(c,e);
    /* Reject a changed controller journal even if all necessary hardware
     * observations individually succeeded. Exported maps cannot be released. */
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->error?c->error:c->epoch!=epoch || c->power_token!=token ||
        c->smmu!=smmu || c->lease!=lease || !c->request_held ||
        memcmp(r,&c->request,sizeof(*r))?ESTALE:0;
    lck_mtx_unlock(&camera_native_capture_lock);
    return e;
}

/* Core STOP calls this after invalidating STREAMING, before source-off.
 * Only the short controller mutex: no control/IRQ reentry, spin or MMIO.
 * The stream owner's existing stop closes the hard-IRQ RCNT admission too.
 * A queued controller may still acquire control later, but must observe
 * admission=false before any AP transfer or next request publication. */
static void cam_native_capture_stop_locked(uint64_t epoch)
{
    lck_mtx_lock(&camera_native_capture_lock);
    if(epoch && cam_native_capture.epoch==epoch)cam_native_capture.repeat_admission=false;
    lck_mtx_unlock(&camera_native_capture_lock);
    thread_wakeup((event_t)&cam_repeat.completed);
}

static int cam_native_capture_thread_start_locked(void)
{
    thread_t thread;
    lck_mtx_lock(&camera_native_capture_lock);
    bool started=cam_native_capture.controller_started;
    lck_mtx_unlock(&camera_native_capture_lock);
    if(started)return 0;
    if(kernel_thread_start(cam_native_capture_controller,NULL,&thread)!=KERN_SUCCESS)return ENOMEM;
    thread_deallocate(thread);
    lck_mtx_lock(&camera_native_capture_lock);
    cam_native_capture.controller_started=true;
    lck_mtx_unlock(&camera_native_capture_lock);
    return 0; /* dormant thread only; no source, credit or READY publication */
}

/* Permanent controller thread, never the RAW completion worker. Register
 * the wait BEFORE sampling the predicate: completion may wake without the
 * capture mutex, while STOP uses that mutex. Either a later wake satisfies
 * the registered wait, or a ready predicate cancels it. No lost wake between
 * worker busy=false and assert_wait, no millisecond polling loop. */
static void __attribute__((noreturn)) cam_native_capture_controller(void *arg,wait_result_t result)
{
    (void)arg;(void)result;
    for(;;) {
        uint64_t epoch=0,completed=0;
        bool ready=false;
        lck_mtx_lock(&camera_native_capture_lock);
        assert_wait((event_t)&cam_repeat.completed,THREAD_UNINT);
        if(cam_native_capture.repeat_admission) {
            boolean_t old=cam_irq_spin_take();
            completed=cam_repeat.completed;
            ready=cam_irq.error || cam_repeat.error ||
                (cam_repeat.enabled && cam_repeat.admission &&
                 cam_repeat.epoch==cam_native_capture.epoch &&
                 cam_repeat.phase==CAM_REPEAT_PARKED && !cam_irq.busy && !cam_irq.count &&
                 completed>cam_native_capture.observed_completed);
            cam_irq_spin_drop(old);
            if(ready) {
                epoch=cam_native_capture.epoch;
                cam_native_capture.observed_completed=completed;
            }
        }
        if(ready)clear_wait(current_thread(),THREAD_AWAKENED);
        lck_mtx_unlock(&camera_native_capture_lock);
        if(!ready){thread_block(THREAD_CONTINUE_NULL);continue;}

        lck_mtx_lock(&camera_control_lock);
        lck_mtx_lock(&camera_native_capture_lock);
        bool admitted=cam_native_capture.repeat_admission && cam_native_capture.epoch==epoch;
        if(admitted)cam_native_capture.controller_busy=true;
        lck_mtx_unlock(&camera_native_capture_lock);
        int e=ECANCELED;
        if(admitted) {
            lck_mtx_lock(&camera_irq_lock);
            e=cam_native_capture_next_locked(epoch);
            lck_mtx_unlock(&camera_irq_lock);
        }
        /* next work has really returned and its IRQ mutex is released.
         * Mark only that thread work complete BEFORE invoking STOP; otherwise
         * capture(off) would wait on the controller which is calling it.
         * This is not a worker/GIC/RAW/AXI retirement receipt. */
        lck_mtx_lock(&camera_native_capture_lock);
        cam_native_capture.controller_busy=false;
        if(e)cam_native_capture.repeat_admission=false;
        lck_mtx_unlock(&camera_native_capture_lock);
        thread_wakeup((event_t)&cam_native_capture.controller_busy);
        if(e && e!=ECANCELED) {
            e=cam_native_capture_fault(&cam_native_capture,e);
            cam_native_capture_stop_locked(epoch);
            /* Use the real core STOP with control + frame mutex, WITHOUT
             * IRQ mutex. It invalidates delivery, attempts actual source-off
             * and calls capture(off); uncertain drain keeps every dependency.
             * Never pretend failure removed source, credit, map or power. */
            lck_mtx_lock(&camera_lock);
            if(cam_sensor_live.epoch==epoch && session.power) {
                st.flags|=X9_CAMERA_FAULT;st.last_errno=e;
                int cleanup=stop();st.last_errno=e;
                printf("X9 CAMERA NATIVE CONTROLLER: epoch=%llu errno=%d stop=%d retained=%u\n",
                    epoch,e,cleanup,session.latched);
            }
            lck_mtx_unlock(&camera_lock);
        }
        lck_mtx_unlock(&camera_control_lock);
    }
}

/* Runtime host/boot-topology admission for the native lifecycle.
 * Registration still needs both actual chip ID / physical close probes;
 * prepare separately proves powered CSI/CAM/BW/SMMU capabilities. Neither
 * callback presence nor this return alone publishes physical RAW/READY. */
static int cam_native_capture_admit(void *context)
{
    DTEntry chosen,gic;
    const void *p;
    unsigned n;
    struct x9_camera_irq_layout q;
    uint64_t reg[4];
    uint64_t el;
    int e;
    if(context!=&cam_native_capture)return EINVAL;
    if(!sweet_early_is_x9() || st.mode!=2)return EPERM;
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=4 || PAGE_SIZE!=0x4000)return ENOTSUP;
    /* Runtime boot topology, not generated constants alone. Registration
     * performs only actual cold sensor ID probes. CSI/CAM/SMMU reads and
     * rate/floor/BW checks must wait for prepare's real MM power token.
     * In particular, don't make the cold ID registration depend on UFS's
     * voltage floor becoming ready, and don't sample unpowered CAMSV here. */
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-camera-irq-layout",&p,&n)!=kSuccess ||
       !p || n!=sizeof(q))return ENXIO;
    memcpy(&q,p,sizeof(q));if(!x9_camera_irq_layout_valid(&q))return ENXIO;
    if(SecureDTLookupEntry(NULL,"/arm-io/gic",&gic)!=kSuccess ||
       SecureDTGetProperty(gic,"reg",&p,&n)!=kSuccess || !p || n!=sizeof(reg))return ENXIO;
    memcpy(reg,p,sizeof(reg));
    if(reg[0]!=q.gicd_base-0x400000ull || reg[1]!=q.gicd_bytes ||
       reg[2]!=0x06880000-0x400000ull || reg[3]!=0x200000)return ENXIO;
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    e=st.mode!=2?EPERM:hw.version || opened || cam_sensor_session_active ||
        cam_stop_owned() || session.latched || frame_readers ||
        (st.flags&(X9_CAMERA_FAULT|X9_CAMERA_RETAINED|X9_CAMERA_STREAMING))?EBUSY:0;
    lck_mtx_unlock(&camera_lock);
    lck_mtx_lock(&camera_native_capture_lock);
    if(!e && (cam_native_capture.error || cam_native_capture.prepare_attempted ||
       cam_native_capture.smmu || cam_native_capture.phase!=CAM_CAPTURE_EMPTY ||
       cam_native_capture.controller_busy || cam_native_capture.repeat_admission))e=EBUSY;
    lck_mtx_unlock(&camera_native_capture_lock);
    if(!e && (cam_mm.active || cam_rx_live.epoch || cam_sv.held || cam_smi.held ||
       cam_mac.held || cam_phy.held || cam_out.held))e=EBUSY;
    boolean_t old=cam_irq_spin_take();
    if(!e && (cam_irq.prepared || cam_irq.owns || cam_irq.accepting ||
       cam_irq.busy || cam_irq.count || cam_irq.error))e=EBUSY;
    cam_irq_spin_drop(old);
    lck_mtx_lock(&camera_smmu_lock);
    if(!e && cam_smmu_namespace)e=EBUSY;
    lck_mtx_unlock(&camera_smmu_lock);
    lck_mtx_unlock(&camera_control_lock);
    /* The native lifecycle includes actual streamed/unstarted retirement,
     * READY never-issued cancellation and whole-physical-close/fresh-MM
     * reopen. Admission grants only the cold ID/close registration attempt;
     * prepare and physical SOF/DONE retain their separate live proofs. */
    return e;
}

static int cam_native_capture_prepare(void *context,uint64_t epoch,
    const struct x9_camera_sensor *s)
{
    struct cam_native_capture_context *c=context;
    struct x9_camera_pipeline plan;
    struct x9_camera_smmu *smmu=NULL;
    struct x9_camera_smmu *old_smmu;
    struct x9_camera_smmu_export view={0};
    uint64_t token;
    int e;
    if(c!=&cam_native_capture)return EINVAL;
    e=cam_native_capture_sensor(epoch,s);if(e)return e;
    if(session.capture || session.stream)return EBUSY;
    e=cam_native_source_standby_locked(epoch);if(e)return e;
    e=cam_pipeline_plan(s->sensor,&plan);if(e)return e;
    token=cam_mm.receive.power_token;
    if(!token || !cam_mm.active || !cam_irq.prepared || cam_irq.epoch!=epoch ||
       cam_irq.close_attempted || !cam_native_source.rate_valid ||
       cam_native_source.status.sensor_epoch!=epoch ||
       cam_native_source.status.stage!=X9_CAM_NATIVE_SOURCE_CONFIGURED)return ENXIO;
    e=cam_native_source_rate_guard_locked(epoch);if(e)return e;
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->error?c->error:c->prepare_attempted?EALREADY:0;
    old_smmu=c->smmu; /* pinned retired namespace, never freed/reset as live */
    if(!e) {
        c->prepare_attempted=true;c->epoch=epoch;c->sensor=s->sensor;
        c->phase=CAM_CAPTURE_PREPARING;
        c->power_token=token;c->plan=plan;c->lease=epoch;
        c->reservation_bytes=(plan.bytes+CAM_S1_PAGE-1)&~(CAM_S1_PAGE-1);
        c->reserved=true; /* per-epoch private allocation; old namespace stays pinned */
    }
    lck_mtx_unlock(&camera_native_capture_lock);
    if(e)return e;
    e=cam_native_capture_thread_start_locked();
    if(e)return cam_native_capture_fault(c,e);
    e=old_smmu?cam_native_reopen_locked(epoch,old_smmu,token,CAM_NATIVE_IOVA,
        plan.bytes,&smmu,&view):x9_camera_native_smmu_start(token,CAM_NATIVE_IOVA,
        plan.bytes,&smmu,&view);
    /* Store even the failed exported host. retirement_guard must retain
     * its actual MM dependency before any core configure-error unwind. */
    lck_mtx_lock(&camera_native_capture_lock);
    c->smmu=smmu;c->view=view;
    if(!e && (!smmu || !view.map.backing || !view.map.raw ||
       view.map.iova!=CAM_NATIVE_IOVA || view.map.bytes!=plan.bytes ||
       view.map.mapped_bytes!=c->reservation_bytes || view.asid!=CAM_SMMU_ASID))e=EFAULT;
    if(e && !c->error)c->error=e;
    if(!e){c->prepared=true;c->phase=CAM_CAPTURE_LIVE;}
    lck_mtx_unlock(&camera_native_capture_lock);
    printf("X9 CAMERA NATIVE PREPARE: sensor=%u epoch=%llu MM=%llu iova=%llx bytes=%u "
        "exported=%u errno=%d (retain namespace; not READY)\n",s->sensor,epoch,token,
        CAM_NATIVE_IOVA,plan.bytes,smmu!=NULL,e);
    return e;
}

static int cam_native_capture_power_guard_locked(uint64_t epoch)
{
    struct x9_camera_smmu *m;
    struct x9_camera_camsv_request request;
    uint64_t token;
    uint32_t phase;
    int e;
    lck_mtx_lock(&camera_native_capture_lock);
    /* No exported host means we installed no translation/fetch dependency.
     * A bare reservation never prevents ordinary cold sensor unwind. */
    m=cam_native_capture.smmu;token=cam_native_capture.power_token;request=cam_native_capture.request;
    phase=cam_native_capture.phase;
    bool recycled_idle=m && phase==CAM_CAPTURE_EMPTY && !cam_native_capture.epoch &&
        !cam_native_capture.prepare_attempted && !cam_native_capture.error &&
        !cam_native_capture.controller_busy && !cam_native_capture.repeat_admission &&
        m->native_epoch_recycled && cam_native_capture_history &&
        cam_native_capture_history->owner.smmu==m &&
        (cam_native_capture_history->owner.phase==CAM_CAPTURE_TRANSLATION_RETIRED ||
         cam_native_capture_history->owner.phase==CAM_CAPTURE_UNSTARTED_RETIRED ||
         cam_native_capture_history->owner.phase==CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED) &&
        cam_native_capture_history->owner.epoch==m->native_retire_epoch;
    e=!m || recycled_idle?0:cam_native_capture.epoch!=epoch?ESTALE:
        cam_native_capture.controller_busy || cam_native_capture.repeat_admission?EBUSY:
        phase!=CAM_CAPTURE_TRANSLATION_RETIRED && phase!=CAM_CAPTURE_UNSTARTED_RETIRED &&
        phase!=CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED?EBUSY:0;
    bool never_candidate=m && cam_native_capture.epoch==epoch &&
        phase==CAM_CAPTURE_PREPARING && !cam_native_capture.prepared &&
        !cam_native_capture.on_attempted && !cam_native_capture.request_held &&
        !cam_native_capture.controller_busy && !cam_native_capture.repeat_admission;
    lck_mtx_unlock(&camera_native_capture_lock);
    /* A genuinely full-closed old namespace is not a dependency of this
     * NEW cold sensor before capture prepare/export. Its original MM ticket
     * is already released and must not be resurrected as a live grant.
     * The native recycle receipt + permanent exact history authorizes only
     * this no-new-export unwind. Fresh mapping/reopen separately re-proves
     * the entire cold bank under the new actual MM token. */
    if(recycled_idle)return cam_native_epoch_recycled_check_locked(m->native_retire_epoch,m);
    if(never_candidate) {
        /* A configure/export error can reach power(false) before capture
         * was journaled by the core. Only the independent native operation
         * may close the actually dormant IRQ and prove never-issued setup,
         * RAW_PRIVATE, no request/credit/source and exact fresh cold priors.
         * The original prepare/cache error remains terminal and pinned. */
        e=cam_native_unstarted_retire_locked(epoch,m);
        if(!e)e=x9_camera_native_smmu_retirement_check(epoch,m);
        if(!e) {
            lck_mtx_lock(&camera_native_capture_lock);
            cam_native_capture.phase=CAM_CAPTURE_UNSTARTED_RETIRED;
            lck_mtx_unlock(&camera_native_capture_lock);
        }
    }
    if(e || !m)return e;
    if(phase==CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED)
        return cam_native_capture_cancel_teardown(epoch,token,m,&request,phase);
    if(!cam_mm.active || cam_mm.receive.power_token!=token || m->native_power!=token)return ESTALE;
    /* Keep the handle/history forever; the real restored register/storage
     * check, not c.smmu=NULL or error clearing, grants supplier teardown. */
    return x9_camera_native_smmu_retirement_check(epoch,m);
}
static int cam_native_capture_retirement_guard(void *context,uint64_t epoch,
    const struct x9_camera_sensor *s)
{
    if(context!=&cam_native_capture || !s || s->sensor<X9_CAMERA_BACK ||
       s->sensor>X9_CAMERA_FRONT || s!=&sensors[s->sensor-1])return EINVAL;
    return cam_native_capture_power_guard_locked(epoch);
}

/* Called only AFTER successful actual sensor/receiver/supplier/MM close,
 * under control mutex, while the sensor adapter still knows the old epoch.
 * ID-only registration has no active capture and is a strict no-op. The
 * native operation proves full physical close and archives its real endpoint
 * ledgers; software epoch mismatch or a caller boolean is never that proof.
 */
static int cam_native_capture_recycle_locked(uint64_t epoch)
{
    struct cam_native_capture_context *c=&cam_native_capture;
    struct cam_native_capture_receipt *receipt;
    struct x9_camera_smmu *m;
    int e;
    if(!epoch)return EINVAL;
    lck_mtx_lock(&camera_native_capture_lock);
    if(!c->epoch || c->epoch!=epoch || !c->prepare_attempted) {
        lck_mtx_unlock(&camera_native_capture_lock);return 0;
    }
    m=c->smmu;
    /* An unsuccessful attempt with no new export has no namespace to
     * recycle; retain its original terminal error, not fictitious resources.
     * The core may still finish genuine no-export supplier cleanup. */
    if(!m){lck_mtx_unlock(&camera_native_capture_lock);return 0;}
    e=c->error?c->error:c->off_error?c->off_error:
        c->controller_busy || c->repeat_admission ||
        (c->phase!=CAM_CAPTURE_TRANSLATION_RETIRED &&
         c->phase!=CAM_CAPTURE_UNSTARTED_RETIRED &&
         c->phase!=CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED)?EBUSY:0;
    lck_mtx_unlock(&camera_native_capture_lock);if(e)return e;
    receipt=kalloc_type(struct cam_native_capture_receipt,Z_WAITOK|Z_ZERO);
    if(!receipt)return ENOMEM;
    lck_mtx_lock(&camera_native_capture_lock);
    receipt->owner=*c;
    lck_mtx_unlock(&camera_native_capture_lock);
    e=cam_native_epoch_recycle_locked(epoch,m);
    if(e){kfree_type(struct cam_native_capture_receipt,receipt);return e;}
    lck_mtx_lock(&camera_native_capture_lock);
    receipt->next=cam_native_capture_history;cam_native_capture_history=receipt;
    /* Preserve the old smmu/view/request also in c until fresh reopen. Keep
     * global capture_id/cookie monotonic across Stop/Start and front-switch:
     * the core's last_capture_id is not reset when a sensor closes. Only the
     * active epoch admission changes, after the independent physical proof.
     * Original error fields were required zero and are never cleared here. */
    c->phase=CAM_CAPTURE_EMPTY;c->epoch=c->power_token=c->generation=c->lease=0;
    c->sensor=c->reservation_bytes=0;c->prepare_attempted=c->prepared=c->reserved=false;
    c->request_held=c->on_attempted=c->off_attempted=false;c->observed_completed=0;
    lck_mtx_unlock(&camera_native_capture_lock);
    printf("X9 CAMERA NATIVE RECYCLE: epoch=%llu capture_id=%llu (full-close receipts/maps pinned)\n",
        epoch,receipt->owner.capture_serial);
    return 0;
}

static int cam_native_capture_off_locked(struct cam_native_capture_context *c,uint64_t epoch,
    const struct x9_camera_sensor *s)
{
    struct x9_camera_smmu *m;
    struct x9_camera_camsv_request request;
    bool cancelled=false;
    int e;
    cam_native_capture_stop_locked(epoch);
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->epoch!=epoch || c->sensor!=s->sensor?ESTALE:c->off_attempted?
        c->off_error?c->off_error:ESTALE:c->controller_busy?EBUSY:0;
    if(!e){c->off_attempted=true;m=c->smmu;request=c->request;}
    else m=NULL;
    lck_mtx_unlock(&camera_native_capture_lock);if(e)return e;
    /* Streamed lifecycle: this first operation includes genuine source OFF,
     * spent credit, CPU/worker/GIC and local halt/ACK restoration. It is NOT
     * yet complete DMA/translation retirement. Unstarted startup failures
     * need a distinct never-issued proof; do not route them through streamed
     * receipts or simply return success because prepared/stream is false. */
    e=!m?ESTALE:!session.stream?cam_native_unstarted_retire_locked(epoch,m):cam_irq_stop_locked(epoch);
    if(!e && session.stream) {
        if(cam_irq.slot && cam_irq.slot->state==X9_CAMSV_CANCELLED) {
            e=cam_repeat_cancelled_check_locked(epoch,&request,m->map);
            if(!e)cancelled=true;
        }
    }
    if(!e && session.stream) {
        lck_mtx_lock(&camera_native_capture_lock);
        c->phase=cancelled?CAM_CAPTURE_LOCAL_CANCELLED_STOPPED:CAM_CAPTURE_LOCAL_STOPPED;
        lck_mtx_unlock(&camera_native_capture_lock);
        e=cam_native_retire_locked(epoch,m); /* does NOT repeat IRQ stop */
    }
    if(!e)e=x9_camera_native_smmu_retirement_check(epoch,m);
    lck_mtx_lock(&camera_native_capture_lock);
    c->off_error=e;
    if(!e)c->phase=!session.stream?CAM_CAPTURE_UNSTARTED_RETIRED:
        cancelled?CAM_CAPTURE_CANCELLED_TRANSLATION_RETIRED:CAM_CAPTURE_TRANSLATION_RETIRED;
    else if(!c->error)c->error=e;
    lck_mtx_unlock(&camera_native_capture_lock);
    printf("X9 CAMERA NATIVE OFF: epoch=%llu phase=%u errno=%d (pinned map/history retained)\n",
        epoch,c->phase,e);
    return e;
}

static int cam_native_capture_capture(void *context,uint64_t epoch,
    const struct x9_camera_sensor *s,int on,uint64_t generation,
    const struct x9_camera_config *config)
{
    struct cam_native_capture_context *c=context;
    struct x9_camera_camsv_request r={0};
    struct x9_camera_camsv_dma dma={0};
    int e;
    if(c!=&cam_native_capture || !s || s->sensor<X9_CAMERA_BACK ||
       s->sensor>X9_CAMERA_FRONT || s!=&sensors[s->sensor-1] ||
       (on!=0 && on!=1) || !generation)return EINVAL;
    if(!on)return cam_native_capture_off_locked(c,epoch,s);
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->epoch!=epoch || c->sensor!=s->sensor?ESTALE:0;
    if(!e && (c->error || !c->prepared || c->on_attempted || c->request_held))
        e=c->error?c->error:EBUSY;
    if(!e) {c->on_attempted=true;c->generation=generation;}
    lck_mtx_unlock(&camera_native_capture_lock);
    if(e)return e;
    e=cam_native_capture_sensor(epoch,s);
    if(!e && (!session.capture || session.stream || session.generation!=generation))e=ESTALE;
    if(!e)e=cam_native_ae_seed_locked(epoch,config);
    if(!e)e=cam_native_ae_snapshot_locked(epoch,&r.frame);
    if(e)return cam_native_capture_fault(c,e);
    lck_mtx_lock(&camera_native_capture_lock);
    if(c->capture_serial==UINT64_MAX)e=EOVERFLOW;
    if(!e) {
        r.frame.version=X9_CAMERA_COMPLETED_VERSION;r.frame.sensor=s->sensor;
        r.frame.stride=c->plan.stride;r.frame.bytes=c->plan.bytes;
        r.frame.generation=generation;r.frame.capture_id=++c->capture_serial;
        r.frame.raw=c->view.map.raw;r.iova=c->view.map.iova;
        /* Independent software cookie, never a widened hardware counter. */
        r.inner_sequence=++c->inner_serial;
        e=cam_completed_layout(s,&r.frame);
        if(!e)e=cam_completed_controls(s,&r.frame);
    }
    if(!e) {
        c->request=r;c->request_held=true;
        dma=(struct x9_camera_camsv_dma){.version=X9_CAMERA_CAMSV_VERSION,
            .context=c,.lease=c->lease,.backing=c->view.map.backing,
            .check=cam_native_capture_dma_check};
    }
    lck_mtx_unlock(&camera_native_capture_lock);
    if(!e)e=cam_native_capture_dma_check(c,dma.lease,&r);
    if(!e)e=cam_irq_begin_locked(epoch,&r,&dma);
    if(!e)e=cam_repeat_enable_locked(epoch); /* actual first credit; sensor still OFF */
    if(e)return cam_native_capture_fault(c,e);
    lck_mtx_lock(&camera_native_capture_lock);
    c->repeat_admission=true;
    lck_mtx_unlock(&camera_native_capture_lock);
    thread_wakeup((event_t)&cam_repeat.completed);
    printf("X9 CAMERA NATIVE FIRST ARM: sensor=%u epoch=%llu generation=%llu "
        "capture_id=%llu inner=%x iova=%llx (physical RCNT; sensor stream follows)\n",
        s->sensor,epoch,generation,r.frame.capture_id,r.inner_sequence,r.iova);
    return 0;
}

static int cam_native_capture_exposure(void *context,uint64_t epoch,
    const struct x9_camera_sensor *s,uint32_t lines,uint32_t gain,uint32_t frame_lines)
{
    struct cam_native_capture_context *c=context;
    int e;
    if(c!=&cam_native_capture)return EINVAL;
    e=cam_native_capture_sensor(epoch,s);if(e)return e;
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->error?c->error:c->epoch!=epoch || c->sensor!=s->sensor ||
        !c->prepared || !c->request_held || c->off_attempted?ESTALE:0;
    lck_mtx_unlock(&camera_native_capture_lock);
    if(!e && (!session.capture || !session.stream))e=ENXIO;
    /* Accepts a unit request only. Current immutable frame metadata does
     * not change merely because an AE request was queued successfully. */
    return e?e:cam_native_ae_queue_locked(epoch,lines,gain,frame_lines);
}

/* Called exclusively by cam_irq_repeat_publish_locked AFTER its last OLD
 * request/RAW_IDLE/worker boundary check, BEFORE new arm's first DMA check.
 * It must never run while old completion cache/copy callbacks still execute.
 * A failed new arm retains this new identity; no rollback or old replay. */
static int cam_native_capture_publish_next(void *context,
    const struct x9_camera_camsv_request *r,const struct x9_camera_camsv_dma *d)
{
    struct cam_native_capture_context *c=context;
    int e;
    if(c!=&cam_native_capture || !r || !d)return EINVAL;
    boolean_t old=cam_irq_spin_take();
    e=cam_repeat.phase!=CAM_REPEAT_PREPARING || !cam_repeat.admission ||
        cam_irq.busy || cam_irq.count || cam_irq.accepting?ESTALE:0;
    cam_irq_spin_drop(old);if(e)return e;
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->error?c->error:!c->repeat_admission || !c->controller_busy || c->off_attempted ||
        !c->request_held || c->capture_serial==UINT64_MAX ||
        r->frame.generation!=c->generation || r->frame.sensor!=c->sensor ||
        r->frame.capture_id!=c->capture_serial+1 || r->inner_sequence!=c->inner_serial+1u ||
        r->iova!=c->view.map.iova || r->frame.raw!=c->view.map.raw ||
        r->frame.bytes!=c->plan.bytes || r->frame.stride!=c->plan.stride ||
        r->frame.time || r->frame.sof_time || r->tag || r->group || r->reserved ||
        d->version!=X9_CAMERA_CAMSV_VERSION || d->reserved || d->context!=c ||
        d->lease!=c->lease || d->backing!=c->view.map.backing ||
        d->check!=cam_native_capture_dma_check?ESTALE:0;
    if(!e)e=cam_completed_layout(&sensors[c->sensor-1],&r->frame);
    if(!e)e=cam_completed_controls(&sensors[c->sensor-1],&r->frame);
    if(!e) {
        c->request=*r;c->capture_serial=r->frame.capture_id;c->inner_serial=r->inner_sequence;
    }
    lck_mtx_unlock(&camera_native_capture_lock);return e;
}

/* Control + IRQ mutex held, outside spin. The previous immutable request
 * stays authoritative throughout genuine no-credit parking and AE settling.
 * AE next performs actual group hold/readback + vendor delayed 0005 samples;
 * this function never marks READY until those controls really apply. */
static int cam_native_capture_next_locked(uint64_t epoch)
{
    struct cam_native_capture_context *c=&cam_native_capture;
    struct x9_camera_camsv_request r;
    struct x9_camera_camsv_dma d;
    int e;
    lck_mtx_lock(&camera_native_capture_lock);
    e=!c->repeat_admission || c->epoch!=epoch?ECANCELED:c->error?c->error:
        !c->prepared || !c->request_held || !c->controller_busy || c->off_attempted?ESTALE:0;
    r=c->request;
    d=(struct x9_camera_camsv_dma){.version=X9_CAMERA_CAMSV_VERSION,.context=c,
        .lease=c->lease,.backing=c->view.map.backing,.check=cam_native_capture_dma_check};
    if(!e && c->capture_serial==UINT64_MAX)e=EOVERFLOW;
    lck_mtx_unlock(&camera_native_capture_lock);if(e)return e;
    lck_mtx_lock(&camera_lock);
    bool current=(st.flags&X9_CAMERA_STREAMING) && !(st.flags&X9_CAMERA_FAULT) &&
        st.sensor==r.frame.sensor && st.generation==r.frame.generation &&
        session.capture && session.stream && !session.stopping && !session.latched;
    lck_mtx_unlock(&camera_lock);if(!current)return ECANCELED;
    e=cam_repeat_cpu_boundary_locked(epoch);if(e)return e;
    e=cam_native_ae_next_locked(epoch,&r.frame);if(e)return e;
    /* next() may spend up to the real settling deadline. Preserve/check
     * old authority again before the wrapper's final old-to-new handoff. */
    e=cam_repeat_cpu_boundary_locked(epoch);if(e)return e;
    lck_mtx_lock(&camera_native_capture_lock);
    e=!c->repeat_admission || c->epoch!=epoch?ECANCELED:c->error;
    if(!e) {
        r.frame.capture_id=c->capture_serial+1;r.inner_sequence=c->inner_serial+1u;
        r.frame.time=r.frame.sof_time=0;
    }
    lck_mtx_unlock(&camera_native_capture_lock);if(e)return e;
    return cam_irq_repeat_publish_locked(epoch,&r,&d,cam_native_capture_publish_next,c);
}

const struct x9_camera_native_engine *x9_camera_native_capture_engine(void)
{
    static const struct x9_camera_native_engine engine={
        .version=X9_CAMERA_NATIVE_ENGINE_VERSION,.context=&cam_native_capture,
        .admit=cam_native_capture_admit,.prepare=cam_native_capture_prepare,
        .capture=cam_native_capture_capture,.exposure=cam_native_capture_exposure,
        .retirement_guard=cam_native_capture_retirement_guard};
    return &engine;
}
