/* Thread-only final cancellation of the exact READY request for which no
 * RCNT was issued. Separate old-DONE and next-cancel identities throughout.
 * Control + IRQ mutex held, NEVER hard IRQ or IRQ spin during cache/locks. */
static int cam_repeat_cancel_bus_guard_locked(uint64_t epoch)
{
    int e=0;
    boolean_t old=cam_irq_spin_take();
    if(!cam_repeat_cancel_admitted_locked() || cam_repeat_cancel.epoch!=epoch ||
       !cam_irq.close_attempted || !cam_irq.prepared || cam_irq.accepting || cam_irq.busy ||
       cam_irq.count || !cam_stream_close.controls || !cam_stream_close_restore_ready() ||
       cam_source.phase!=CAM_SOURCE_STOPPED || cam_source.error || !cam_sv_gate_off_ready() ||
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_QUIESCENT ||
       !cam_dma_stop.issued || cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED || cam_dma_stop.error ||
       cam_dma_stop.epoch!=epoch || cam_dma_stop.samples<2 || cam_dma_stop.port_samples!=2 ||
       cam_dma_stop.restore_samples!=2 || cam_dma_stop.release_seen!=3)e=ESTALE;
    for(unsigned b=0;b<2 && !e;b++) {
        e=cam_irq_power();if(e)break;
        uint32_t ctl=cam_irq_rd(cam_irq.view[b],0x540),cnt=cam_irq_rd(cam_irq.view[b],0x544);
        e=cam_irq_fbc_note_locked(b,ctl,cnt);
        if(!e && ((cnt&CAM_IRQ_FBC_CNT) ||
           ((cnt^cam_repeat_cancel.prior_fbc.after[b])&~CAM_REPEAT_DROP)))e=ESTALE;
        if(!e){cam_repeat_cancel.control[b]=ctl;cam_repeat_cancel.counter[b]=cnt;}
    }
    cam_irq_spin_drop(old);if(e)return e;
    e=cam_irq_mapping();if(!e)e=cam_irq_power();if(!e)e=cam_irq_controller();
    if(!e)e=cam_irq_idle();if(!e)e=cam_sv_guard();if(!e)e=cam_rx_session_guard();
    if(!e)e=cam_mac_guard();if(!e)e=cam_phy_guard();if(!e)e=cam_out_guard();if(e)return e;
    struct cam_sensor_ap ap={&cam_journal.sensor,
        cam_sensor_slave(cam_repeat_cancel.next.frame.sensor)};
    const uint16_t regs[]={0x0100,0x0104,0x0005};uint8_t value;
    for(unsigned i=0;i<3 && !e;i++) {
        e=cam_sensor_ap_read(&ap,regs[i],&value);
        if(!e && value!=(i==2?0xff:0))e=ESTALE;
    }
    if(!e)e=cam_smi_a0_guard();if(!e)e=cam_smi_shared_voter_guard();if(e)return e;
    if(!cam_smi.held || !cam_smi_shared.held || !cam_smi_shared.common ||
       cam_smi.close_attempted || cam_smi_a0.close_attempted)return ESTALE;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_smi_clock_only_guard();if(e)return e;
        if(cam_rr(cam_smi.larb[0],0x280) || cam_rr(cam_smi.larb[0],0x780) ||
           cam_rr(cam_smi.larb[0],0) || cam_rr(cam_smi.larb[0],0x500))return EBUSY;
        for(unsigned p=0;p<8;p++)if(cam_rr(cam_smi_shared.common,0x400+4*p)&0x07ffe000u)return EBUSY;
        if(!(cam_rr(cam_smi_shared.common,0x440)&1u))return EBUSY;
        if(!sample)delay(10);
    }
    return cam_irq_power();
}
static int cam_repeat_cancel_complete_locked(uint64_t epoch)
{
    boolean_t old=cam_irq_spin_take();bool candidate=cam_repeat_cancel_admitted_locked();
    cam_irq_spin_drop(old);if(!candidate)return 0;
    int e=cam_repeat_cancel_bus_guard_locked(epoch);
    if(!e)e=cam_sv_request_authority(); /* live normal authority, before cache */
    lck_mtx_lock(&camera_camsv_lock);
    struct x9_camera_camsv_slot *slot=cam_irq.slot;
    if(!e && (!slot || slot!=cam_sv.request_slot || slot->state!=X9_CAMSV_ARMED ||
       memcmp(&slot->request,&cam_repeat_cancel.next,sizeof(slot->request)) ||
       slot->dma.backing!=cam_repeat_cancel.prior_slot.dma.backing ||
       slot->dma.context!=cam_repeat_cancel.prior_slot.dma.context ||
       slot->dma.lease!=cam_repeat_cancel.prior_slot.dma.lease ||
       slot->dma.check!=cam_repeat_cancel.prior_slot.dma.check))e=ESTALE;
    if(!e)e=cam_s1_dma_cancel_unissued(slot); /* no RAW bytes read */
    if(!e)slot->state=X9_CAMSV_CANCELLED;
    lck_mtx_unlock(&camera_camsv_lock);
    if(!e)e=cam_repeat_cancel_bus_guard_locked(epoch);
    old=cam_irq_spin_take();
    if(!e) {cam_repeat_cancel.cancelled=true;cam_repeat.phase=CAM_REPEAT_CANCELLED;}
    else {cam_repeat_cancel.error=e;cam_repeat_fault_locked(e);}
    cam_irq_spin_drop(old);
    if(e) {
        lck_mtx_lock(&camera_camsv_lock);if(slot)cam_camsv_retain(slot,e);
        lck_mtx_unlock(&camera_camsv_lock);
    }
    printf("X9 CAMERA CANCEL: epoch=%llu old_done=%llu new_unused=%llu submitted=%llu "
        "cancelled=%u fbc=%x/%x errno=%d (actual OFF/no new RCNT/GIC/DMA halt/bus; no fake DONE)\n",
        epoch,cam_repeat_cancel.prior_slot.request.frame.capture_id,
        cam_repeat_cancel.next.frame.capture_id,cam_repeat.submitted,
        cam_repeat_cancel.cancelled,cam_repeat_cancel.counter[0],cam_repeat_cancel.counter[1],e);
    return e;
}
/* Exact cancelled-current-request teardown/history scope, not DMA/arm/cache
 * permission. May be called with CAMSV mutex already held. No CAMSV re-lock,
 * owner callbacks, SMMU recursion, MMIO or old released MM token validation. */
static int cam_repeat_cancelled_check_locked(uint64_t epoch,
    const struct x9_camera_camsv_request *request,void *backing)
{
    if(!request || !backing)return EINVAL;
    boolean_t old=cam_irq_spin_take();
    int e=!cam_repeat_cancel_admitted_locked() || !cam_repeat_cancel.cancelled ||
        cam_repeat.phase!=CAM_REPEAT_CANCELLED || epoch!=cam_repeat_cancel.epoch ||
        memcmp(request,&cam_repeat_cancel.next,sizeof(*request)) ||
        backing!=cam_repeat_cancel.prior_slot.dma.backing?ESTALE:0;
    struct x9_camera_camsv_slot *slot=cam_irq.slot;
    if(!e && (!slot || slot->state!=X9_CAMSV_CANCELLED || slot->last_errno ||
       slot->dma.backing!=backing || memcmp(&slot->request,request,sizeof(*request))))e=ESTALE;
    cam_irq_spin_drop(old);if(e)return e;
    struct x9_camera_s1 *m=backing;lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_check_private(m);
    if(!e && (m->raw_errno || !m->published || m->raw_state!=CAM_RAW_CANCELLED ||
       !cam_s1_dma_match(m,slot)))e=ESTALE;
    lck_mtx_unlock(&camera_s1_lock);return e;
}
