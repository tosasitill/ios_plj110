/* Normal streamed-request control stop, NOT whole RAW/AXI/SMMU retirement.
 * Called by the actual close path with control + IRQ mutex held. This is
 * a private receipt, not a caller-supplied done/drained boolean. A complete
 * sensor OFF and owned DONE/CPU/worker/GIC boundary permits ONLY the normal
 * reverse source/TG/receive stores. The original credit, S1 and all parent
 * dependencies remain pinned. Ordinary CAMSV restore additionally requires
 * the later private natural-spent receipt; this control receipt alone cannot
 * release/rearm the issued credit or justify dropping any mapping/supplier.
 */
static struct {
    uint64_t epoch;
    struct x9_camera_camsv_request request;
    bool attempted,checking,admitted,controls;
    uint32_t fbc_control[2],fbc_counter[2];
    uint8_t control,hold,frame_count;
    uint32_t sampled,samples;
    int error;
} cam_stream_close;
/* Read-only under the IRQ spin from live FBC sampling. Only a completed
 * actual control receipt can use the exact GIC restoration journal; this
 * does not authorize namespace teardown or retire the original credit. */
static bool cam_stream_close_restore_ready(void)
{
    return cam_stream_close.admitted && cam_stream_close.controls &&
        !cam_stream_close.checking && !cam_stream_close.error &&
        cam_stream_close.epoch==cam_irq.epoch && cam_stream_close.samples==2 &&
        !memcmp(&cam_stream_close.request,&cam_irq_fbc.request,sizeof(cam_stream_close.request));
}
static int cam_irq_slot_guard_locked(bool streamed_stop)
{
    int e;
    lck_mtx_lock(&camera_camsv_lock);
    e=!cam_irq.slot || cam_irq.slot!=cam_sv.request_slot ||
        cam_irq.slot->state!=(streamed_stop?X9_CAMSV_RETIRED:X9_CAMSV_ARMED)?ESTALE:0;
    lck_mtx_unlock(&camera_camsv_lock);return e;
}
/* No SV/OUTMUX recursion: the full caller guard independently samples its
 * configuration. Native check is outside spin; counters are read+recorded
 * under the same spin as the IAR, before any other counter sample. */
static int cam_stream_close_guard_locked(uint64_t epoch)
{
    boolean_t old;
    int e;
    if(!cam_stream_close.attempted || (!cam_stream_close.checking && !cam_stream_close.admitted) ||
       cam_stream_close.error || epoch!=cam_stream_close.epoch || !session.stream ||
       !session.capture || session.generation!=cam_stream_close.request.frame.generation ||
       !cam_irq.prepared || cam_irq.close_attempted || epoch!=cam_irq.epoch ||
       !cam_sv.request_held || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED ||
       memcmp(&cam_stream_close.request,&cam_sv.request,sizeof(cam_sv.request)))
        return cam_stream_close.error?cam_stream_close.error:ESTALE;
    e=cam_sv_request_authority();if(!e)e=cam_irq_mapping();
    if(!e)e=cam_irq_power();if(!e)e=cam_irq_wait_rwp();
    if(!e)e=cam_irq_config(false);if(!e)e=cam_irq_idle();
    if(!e)e=cam_irq_slot_guard_locked(true);if(e)return e;
    old=cam_irq_spin_take();
    e=cam_irq.error?cam_irq.error:cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
    if(!e && (!cam_irq.owns || cam_irq.accepting || cam_irq.count || cam_irq.busy ||
       !cam_irq.sof_seen || !cam_irq_fbc.bound || !cam_irq_fbc.done_seen ||
       !cam_irq_fbc.count_accepted || cam_irq_fbc.epoch!=epoch ||
       cam_irq_fbc.source_phase!=CAM_STREAM_STOPPED ||
       cam_irq_fbc.source_issued!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_ack!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_seen!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_sampled!=7 || cam_irq_fbc.source_control ||
       cam_irq_fbc.source_frame_count!=0xff ||
       memcmp(&cam_irq_fbc.request,&cam_stream_close.request,sizeof(cam_stream_close.request))))e=ESTALE;
    for(unsigned bank=0;bank<2 && !e;bank++) {
        e=cam_irq_power();if(!e)e=cam_irq_config(true);if(e)break;
        cam_stream_close.fbc_control[bank]=cam_irq_rd(cam_irq.view[bank],0x540);
        cam_stream_close.fbc_counter[bank]=cam_irq_rd(cam_irq.view[bank],0x544);
        e=cam_irq_fbc_note_locked(bank,cam_stream_close.fbc_control[bank],
            cam_stream_close.fbc_counter[bank]);
        if(!e && (cam_stream_close.fbc_control[bank]!=cam_irq_fbc.control[bank] ||
           cam_stream_close.fbc_counter[bank]!=cam_irq_fbc.target[bank]))e=ESTALE;
    }
    cam_irq_spin_drop(old);return e;
}
static int cam_irq_control_guard_locked(uint64_t epoch,bool streamed_stop)
{
    if(!streamed_stop)return cam_irq_credit_guard_locked(epoch);
    if(!cam_stream_close.admitted || cam_stream_close.checking ||
       cam_stream_close.samples!=2)return ESTALE;
    return cam_stream_close_guard_locked(epoch);
}
static int cam_stream_close_begin_locked(uint64_t epoch)
{
    struct cam_sensor_ap ap;
    uint64_t span,end;
    boolean_t old;
    int e;
    if(cam_stream_close.attempted)return cam_stream_close.error?cam_stream_close.error:ESTALE;
    cam_stream_close.attempted=true;cam_stream_close.epoch=epoch;
    cam_stream_close.request=cam_sv.request;
    /* Do not hold the IRQ mutex while waiting for worker pins. Leave the
     * same binding and retained resources intact; do not mask/discard work
     * to manufacture a successful DONE. A canceled/unfinished request will
     * fail the actual receipt below, even if the software queue is empty. */
    nanoseconds_to_absolutetime(500000000,&span);end=mach_absolute_time()+span;
    e=0;
    for(;;) {
        bool pending;
        old=cam_irq_spin_take();pending=cam_irq.busy || cam_irq.count;
        if(!pending){cam_irq_spin_drop(old);break;}
        if(mach_absolute_time()>=end){cam_irq_spin_drop(old);e=ETIMEDOUT;break;}
        assert_wait_deadline((event_t)&cam_irq.busy,THREAD_UNINT,end);
        cam_irq_spin_drop(old);lck_mtx_unlock(&camera_irq_lock);
        thread_block(THREAD_CONTINUE_NULL);lck_mtx_lock(&camera_irq_lock);
    }
    if(e)goto out;
    cam_stream_close.checking=true;
    e=cam_stream_close_guard_locked(epoch);if(e)goto out;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,
        cam_stream_close.request.frame.sensor==X9_CAMERA_BACK?0x1a:0x10};
    /* OFF ACK is necessary, not sufficient even for control stop. Read
     * fresh sensor state again; no command, reset, retry or new credit. */
    for(unsigned sample=0;sample<2 && !e;sample++) {
        e=cam_stream_close_guard_locked(epoch);if(e)break;
        e=cam_sensor_ap_read(&ap,0x0100,&cam_stream_close.control);
        if(!e)cam_stream_close.sampled|=1;
        if(!e && cam_stream_close.control)e=ESTALE;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&cam_stream_close.hold);
        if(!e)cam_stream_close.sampled|=2;
        if(!e && cam_stream_close.hold)e=ESTALE;
        if(!e)e=cam_sensor_ap_read(&ap,0x0005,&cam_stream_close.frame_count);
        if(!e)cam_stream_close.sampled|=4;
        if(!e && cam_stream_close.frame_count!=0xff)e=ESTALE;
        if(!e)e=cam_stream_close_guard_locked(epoch);
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
        if(!e)cam_stream_close.samples++;
    }
out:
    cam_stream_close.checking=false;
    if(!e)cam_stream_close.admitted=true;
    if(e)cam_stream_close.error=e;
    printf("X9 CAMERA STREAM STOP: epoch=%llu capture_id=%llu admitted=%u samples=%u "
        "sensor=%x/%x/%x sampled=%x fbc=%x/%x control=%x/%x errno=%d "
        "(owned OFF/DONE/CPU/worker/GIC; control stop only, NOT AXI/map drain)\n",
        epoch,cam_stream_close.request.frame.capture_id,cam_stream_close.admitted,
        cam_stream_close.samples,cam_stream_close.control,cam_stream_close.hold,
        cam_stream_close.frame_count,cam_stream_close.sampled,
        cam_stream_close.fbc_counter[0],cam_stream_close.fbc_counter[1],
        cam_stream_close.fbc_control[0],cam_stream_close.fbc_control[1],e);
    return e;
}
static int cam_stream_close_controls_locked(uint64_t epoch)
{
    int e=cam_stream_close_guard_locked(epoch);
    if(!e && (cam_stream_close.controls || cam_stream_close.samples!=2 ||
       cam_source.phase!=CAM_SOURCE_STOPPED || cam_source.error ||
       !cam_sv_gate_off_ready() || cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_QUIESCENT))e=ESTALE;
    if(e) {if(!cam_stream_close.error)cam_stream_close.error=e;}
    else cam_stream_close.controls=true;
    printf("X9 CAMERA STREAM CONTROLS: epoch=%llu capture_id=%llu stopped=%u errno=%d "
        "(credit/S1/all suppliers retained; NOT AXI/map drain)\n",
        epoch,cam_stream_close.request.frame.capture_id,cam_stream_close.controls,e);
    return e;
}
