/* Actual first-request composition, not a provider-registration shortcut.
 * Include after IRQ and the native AE implementation. Control callbacks run
 * under camera_control_lock; the CAMSV worker's lease check does NOT acquire
 * that mutex, camera_irq_lock or camera_camsv_lock. All exported storage and
 * its real MM token stay pinned, including prepare failures before the core
 * has recorded capture=true. No local IRQ stop is treated as full retirement.
 */
#include "x9_camera_native_capture.h"
#include "x9_camera_native_smmu.h"
LCK_MTX_DECLARE(camera_native_capture_lock,&camera_group);

/* This is an allocation in our cold SID80/SSID0/ASID1 namespace, NOT CPU PA
 * identity mapping or a reservation in Android's inherited mm_common tree.
 * live.dts mm_smmu_cam is SID80, 16GiB aperture. mm_common excludes the
 * fc910000..ffffefff framebuffer carveout; this private slot is below it.
 * The native host refuses every configured normal bank and preserves the
 * complete prior abort/bypass policy for EVERY unrelated SID. */
#define CAM_NATIVE_IOVA 0x40000000ull
struct cam_native_capture_context {
    uint64_t epoch,power_token,generation,capture_serial,lease;
    uint32_t sensor,inner_serial,reservation_bytes;
    bool prepare_attempted,prepared,reserved,request_held,on_attempted,off_attempted;
    int error,off_error;
    struct x9_camera_pipeline plan;
    struct x9_camera_smmu *smmu;
    struct x9_camera_smmu_export view;
    struct x9_camera_camsv_request request;
};
static struct cam_native_capture_context cam_native_capture;

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

static int cam_native_capture_dma_check(void *context,uint64_t lease,
    const struct x9_camera_camsv_request *r)
{
    struct cam_native_capture_context *c=context;
    struct x9_camera_smmu *smmu;
    struct x9_camera_smmu_export view;
    uint64_t epoch,token;
    int e;
    if(c!=&cam_native_capture || !r || !lease)return EINVAL;
    /* Never keep this mutex while invoking another component. A completion
     * pins the immutable request/namespace; STOP cannot free or rebind them. */
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->error;
    if(!e && (!c->prepared || !c->reserved || !c->request_held ||
       !c->smmu || c->lease!=lease || memcmp(r,&c->request,sizeof(*r))))e=ESTALE;
    epoch=c->epoch;token=c->power_token;smmu=c->smmu;view=c->view;
    if(!e && (!epoch || !token || r->frame.sensor!=c->sensor ||
       r->frame.generation!=c->generation || r->iova!=CAM_NATIVE_IOVA ||
       r->iova!=view.map.iova || r->frame.raw!=view.map.raw ||
       r->frame.bytes!=view.map.bytes || view.map.mapped_bytes!=c->reservation_bytes ||
       view.asid!=CAM_SMMU_ASID || !view.map.backing))e=ESTALE;
    lck_mtx_unlock(&camera_native_capture_lock);
    if(e)return e;
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

/* Closed honestly: first-arm code is not a continuous preview provider.
 * Full retirement must cover source/credit/worker/AXI/SMI/SMMU and supplier
 * dependencies, and continuous rearm must own each AE-bound next request.
 * No registration caller may turn callback presence into READY. */
static int cam_native_capture_admit(void *context)
{
    uint64_t el;
    if(context!=&cam_native_capture)return EINVAL;
    if(!sweet_early_is_x9() || st.mode!=2)return EPERM;
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=4 || PAGE_SIZE!=0x4000)return ENOTSUP;
    return ENOTSUP; /* full producer/translation retirement + next-credit composition missing */
}

static int cam_native_capture_prepare(void *context,uint64_t epoch,
    const struct x9_camera_sensor *s)
{
    struct cam_native_capture_context *c=context;
    struct x9_camera_pipeline plan;
    struct x9_camera_smmu *smmu=NULL;
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
    if(!e) {
        c->prepare_attempted=true;c->epoch=epoch;c->sensor=s->sensor;
        c->power_token=token;c->plan=plan;c->lease=epoch;
        c->reservation_bytes=(plan.bytes+CAM_S1_PAGE-1)&~(CAM_S1_PAGE-1);
        c->reserved=true; /* once-only private namespace allocation before attach */
    }
    lck_mtx_unlock(&camera_native_capture_lock);
    if(e)return e;
    e=x9_camera_native_smmu_start(token,CAM_NATIVE_IOVA,plan.bytes,&smmu,&view);
    /* Store even the failed exported host. retirement_guard must retain
     * its actual MM dependency before any core configure-error unwind. */
    lck_mtx_lock(&camera_native_capture_lock);
    c->smmu=smmu;c->view=view;
    if(!e && (!smmu || !view.map.backing || !view.map.raw ||
       view.map.iova!=CAM_NATIVE_IOVA || view.map.bytes!=plan.bytes ||
       view.map.mapped_bytes!=c->reservation_bytes || view.asid!=CAM_SMMU_ASID))e=EFAULT;
    if(e && !c->error)c->error=e;
    if(!e)c->prepared=true;
    lck_mtx_unlock(&camera_native_capture_lock);
    printf("X9 CAMERA NATIVE PREPARE: sensor=%u epoch=%llu MM=%llu iova=%llx bytes=%u "
        "exported=%u errno=%d (retain namespace; not READY)\n",s->sensor,epoch,token,
        CAM_NATIVE_IOVA,plan.bytes,smmu!=NULL,e);
    return e;
}

static int cam_native_capture_power_guard_locked(uint64_t epoch)
{
    int e;
    lck_mtx_lock(&camera_native_capture_lock);
    /* No exported host means we installed no translation/fetch dependency.
     * A bare reservation never prevents ordinary cold sensor unwind. */
    e=!cam_native_capture.smmu?0:cam_native_capture.epoch!=epoch?ESTALE:
        cam_native_capture.error?cam_native_capture.error:EBUSY;
    lck_mtx_unlock(&camera_native_capture_lock);
    return e;
}
static int cam_native_capture_retirement_guard(void *context,uint64_t epoch,
    const struct x9_camera_sensor *s)
{
    if(context!=&cam_native_capture || !s || s->sensor<X9_CAMERA_BACK ||
       s->sensor>X9_CAMERA_FRONT || s!=&sensors[s->sensor-1])return EINVAL;
    return cam_native_capture_power_guard_locked(epoch);
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
    lck_mtx_lock(&camera_native_capture_lock);
    e=c->epoch!=epoch || c->sensor!=s->sensor?ESTALE:0;
    if(!e && !on) {
        if(c->off_attempted)e=c->off_error?c->off_error:ESTALE;
        else {c->off_attempted=true;c->off_error=c->error?c->error:ENOTSUP;e=c->off_error;}
    }
    if(!e && (c->error || !c->prepared || c->on_attempted || c->request_held))
        e=c->error?c->error:EBUSY;
    if(!e) {c->on_attempted=true;c->generation=generation;}
    lck_mtx_unlock(&camera_native_capture_lock);
    if(e)return e;
    /* OFF intentionally never reports local consumer/GIC stop as complete
     * AXI/translation retirement, and never retries a partial startup. */
    e=cam_native_capture_sensor(epoch,s);
    if(!e && (!session.capture || session.stream || session.generation!=generation))e=ESTALE;
    if(!e)e=cam_native_ae_seed_locked(epoch,config);
    if(!e)e=cam_native_ae_snapshot_locked(epoch,&r.frame);
    if(e)return cam_native_capture_fault(c,e);
    lck_mtx_lock(&camera_native_capture_lock);
    if(c->capture_serial==UINT64_MAX || c->inner_serial==UINT32_MAX)e=EOVERFLOW;
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
    if(e)return cam_native_capture_fault(c,e);
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

const struct x9_camera_native_engine *x9_camera_native_capture_engine(void)
{
    static const struct x9_camera_native_engine engine={
        .version=X9_CAMERA_NATIVE_ENGINE_VERSION,.context=&cam_native_capture,
        .admit=cam_native_capture_admit,.prepare=cam_native_capture_prepare,
        .capture=cam_native_capture_capture,.exposure=cam_native_capture_exposure,
        .retirement_guard=cam_native_capture_retirement_guard};
    return &engine;
}
