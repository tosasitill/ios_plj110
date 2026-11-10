/* Natural one-credit consumption, NOT whole DMA/SMI/SMMU/EMI retirement.
 * No RCNT/reset/counter write, rebasing, lease drop or backing release.
 * Control + IRQ mutex held for initial recording; subsequent SV restore
 * holds control. Worker/filter have actually retired before this ledger.
 */
static int cam_irq_credit_retired_slot(void)
{
    struct x9_camera_camsv_slot *slot=cam_irq.slot;
    struct x9_camera_s1 *m;
    int e;
    lck_mtx_lock(&camera_camsv_lock);
    e=!slot || slot!=cam_sv.request_slot || slot->state!=X9_CAMSV_RETIRED ||
        memcmp(&slot->request,&cam_sv.request,sizeof(slot->request)) ||
        slot->dma.version!=cam_sv.request_dma.version || slot->dma.reserved ||
        slot->dma.context!=cam_sv.request_dma.context || slot->dma.lease!=cam_sv.request_dma.lease ||
        slot->dma.backing!=cam_sv.request_dma.backing || slot->dma.check!=cam_sv.request_dma.check?ESTALE:0;
    if(!e) {
        m=slot->dma.backing;
        lck_mtx_lock(&camera_s1_lock);
        e=cam_s1_check_private(m);
        if(!e && m->raw_errno)e=m->raw_errno;
        if(!e && (!m->published || m->raw_state!=CAM_RAW_IDLE || !cam_s1_dma_match(m,slot)))e=ESTALE;
        lck_mtx_unlock(&camera_s1_lock);
    }
    lck_mtx_unlock(&camera_camsv_lock);return e;
}
/* Never call SV environment here: SV post/restore invokes this guard.
 * Native owner callbacks stay outside spin and the slot/S1 locks. Changing
 * ordinary addresses/FBC EN during our restore must not invalidate the
 * immutable request's independently retained controller/map authority.
 */
static int cam_irq_credit_retired_guard(uint64_t epoch)
{
    boolean_t old;
    int e;
    if(!cam_sv.credit_retire_attempted || !cam_sv.credit_issued ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED || !cam_sv.request_held ||
       cam_sv.epoch!=epoch || cam_sv.error || cam_sv.terminal ||
       !session.power || !session.stream || session.generation!=cam_sv.request.frame.generation ||
       cam_sensor_live.epoch!=epoch || cam_sensor_live.probe.sensor!=cam_sv.request.frame.sensor ||
       !cam_stream_close_restore_ready() || cam_stream_close.epoch!=epoch ||
       !cam_source.captured || cam_source.phase!=CAM_SOURCE_STOPPED || cam_source.epoch!=epoch || cam_source.error ||
       !cam_sv_gate_off_ready() || cam_sv_gate.phase<X9_CAM_SV_GATE_STOPPED ||
       cam_sv_gate.epoch!=epoch || cam_sv_gate.pending!=X9_CAM_SV_GATE_STEPS ||
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_QUIESCENT ||
       !cam_dma_stop.restore_attempted || !cam_dma_stop.issued || !cam_dma_stop.release_issued ||
       cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED || cam_dma_stop.epoch!=epoch || cam_dma_stop.error ||
       cam_dma_stop.release_seen!=3 || cam_dma_stop.restore_samples!=2 ||
       (cam_dma_stop.dcm_issued && (!cam_dma_stop.dcm_restore_issued || cam_dma_stop.dcm_restore_seen!=3)) ||
       !cam_smi.held || !cam_smi_a0.held || cam_smi.epoch!=epoch || cam_smi_a0.epoch!=epoch ||
       cam_smi.close_attempted || cam_smi_a0.close_attempted || cam_smi.error ||
       cam_smi.status.a0.last_errno ||
       memcmp(&cam_dma_stop.request,&cam_sv.request,sizeof(cam_sv.request)))return ESTALE;
    e=cam_sv_request_authority();if(!e)e=cam_irq_mapping();if(!e)e=cam_irq_power();
    if(!e)e=cam_irq_wait_rwp();if(!e)e=cam_irq_controller();if(!e)e=cam_irq_idle();
    if(!e)e=cam_irq_credit_retired_slot();if(e)return e;
    old=cam_irq_spin_take();
    e=cam_irq.error?cam_irq.error:cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
    if(!e && (epoch!=cam_irq.epoch || !cam_irq.close_attempted || cam_irq.owns ||
       cam_irq.accepting || cam_irq.busy || cam_irq.count || !cam_irq.sof_seen ||
       cam_irq.dirty_route || cam_irq.dirty_priority || cam_irq.uncertain_route || cam_irq.uncertain_priority ||
       !cam_irq_fbc.bound || !cam_irq_fbc.done_seen || !cam_irq_fbc.count_accepted ||
       cam_irq_fbc.epoch!=epoch || cam_irq_fbc.source_phase!=CAM_STREAM_STOPPED ||
       cam_irq_fbc.source_issued!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_ack!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_seen!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_sampled!=7 || cam_irq_fbc.source_control ||
       cam_irq_fbc.source_frame_count!=0xff ||
       memcmp(&cam_irq_fbc.request,&cam_sv.request,sizeof(cam_sv.request))))e=ESTALE;
    if(!e)e=cam_irq_power();if(!e)e=cam_irq_controller();
    if(!e)e=cam_irq_idle();if(!e)e=cam_irq_partial_config();
    for(unsigned bank=0;bank<2 && !e;bank++) {
        uint32_t control,counter;
        e=cam_irq_power();if(e)break;
        control=cam_irq_rd(cam_irq.view[bank],0x540);counter=cam_irq_rd(cam_irq.view[bank],0x544);
        e=cam_irq_fbc_note_locked(bank,control,counter);
        if(!e && (counter!=cam_irq_fbc.target[bank] ||
           (cam_sv.credit_retired && counter!=cam_sv.credit_spent_count[bank])))e=ESTALE;
    }
    cam_irq_spin_drop(old);if(e)return e;
    /* Real restored stop/DCM and outstanding monitors, not cached zeros. */
    for(unsigned bank=0;bank<2;bank++) {
        bool allowed=false;
        e=cam_sv_power_guard();if(e)return e;
        e=cam_sv_dma_stop_observe(bank,cam_sv.view[bank],0x7c,cam_rr(cam_sv.view[bank],0x7c),&allowed);
        if(!e)e=cam_sv_dma_stop_observe(bank,cam_sv.view[bank],0x1020,
            cam_rr(cam_sv.view[bank],0x1020),&allowed);if(e)return e;
        if(cam_dma_stop.dcm_actual[bank]!=cam_dma_stop.dcm_prior[bank] ||
           cam_dma_stop.actual[bank]!=cam_dma_stop.prior[bank])return ESTALE;
    }
    e=cam_smi_clock_guard();if(!e)e=cam_sv_dma_stop_port_sample_locked();if(e)return e;
    if(cam_dma_stop.ostd[0] || cam_dma_stop.ostd[1])return EBUSY;
    return cam_sv_request_authority();
}
static int cam_sv_credit_retire_locked(uint64_t epoch)
{
    struct cam_sensor_ap ap;
    uint64_t span,end;
    boolean_t old;
    uint8_t control=0,hold=0,count=0;
    int e=0;
    if(cam_sv.credit_retire_attempted)return cam_sv.error?cam_sv.error:ESTALE;
    cam_sv.credit_retire_attempted=true; /* no replay even before first sample */
    nanoseconds_to_absolutetime(100000000,&span);end=mach_absolute_time()+span;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,
        cam_sv.request.frame.sensor==X9_CAMERA_BACK?0x1a:0x10};
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_irq_credit_retired_guard(epoch);if(e)goto out;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
        /* Actual fresh sensor OFF/hold0/FF, with existing AP/FIFO guards.
         * A sensor already isolated/closing cannot manufacture this receipt.
         * Native capture-stop must run here BEFORE sensor power teardown. */
        e=cam_sensor_ap_read(&ap,0x0100,&control);
        if(!e && control)e=ESTALE;
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&hold);
        if(!e && hold)e=ESTALE;
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
        if(!e)e=cam_sensor_ap_read(&ap,0x0005,&count);
        if(!e && count!=0xff)e=ESTALE;
        if(!e)e=cam_sv_environment();
        if(!e)e=cam_irq_credit_retired_guard(epoch);
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
        if(e)goto out;
        cam_sv.credit_retire_samples++;
    }
    e=cam_irq_credit_retired_guard(epoch);if(e)goto out;
    old=cam_irq_spin_take();
    /* Last complete real sample; immutable acceptance is never rebased.
     * No writer remains in our filter/worker, nor an external lease holder. */
    e=cam_irq_power();if(!e)e=cam_irq_controller();if(!e)e=cam_irq_idle();
    for(unsigned bank=0;bank<2 && !e;bank++) {
        uint32_t control_word=cam_irq_rd(cam_irq.view[bank],0x540);
        uint32_t counter=cam_irq_rd(cam_irq.view[bank],0x544);
        e=cam_irq_fbc_note_locked(bank,control_word,counter);
        if(!e && counter!=cam_irq_fbc.target[bank])e=ESTALE;
    }
    if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    if(!e) {
        memcpy(cam_sv.credit_spent_count,cam_irq_fbc.after,sizeof(cam_sv.credit_spent_count));
        cam_sv.credit_retired=true; /* keep credit_issued/ACCEPTED history */
    }
    cam_irq_spin_drop(old);
out:
    if(e) {
        cam_sv.terminal=true;cam_sv_fault(e);cam_irq_fault(e);
        lck_mtx_lock(&camera_camsv_lock);
        if(cam_irq.slot && cam_irq.slot==cam_sv.request_slot)cam_camsv_retain(cam_irq.slot,e);
        lck_mtx_unlock(&camera_camsv_lock);
    }
    cam_sv_status();
    printf("X9 CAMERA CREDIT SPENT: epoch=%llu capture_id=%llu spent=%u samples=%u "
        "count=%x/%x errno=%d (natural credit only; original lease/S1/suppliers retained)\n",
        epoch,cam_sv.request.frame.capture_id,cam_sv.credit_retired,cam_sv.credit_retire_samples,
        cam_sv.credit_spent_count[0],cam_sv.credit_spent_count[1],e);
    return e;
}
