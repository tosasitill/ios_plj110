/* Whole native epoch retirement/reopen. Only the physical sensor/supplier
 * close-success hook may recycle. Archive complete old software journals
 * before rebuilding per-epoch IRQ/source state; all exported storage remains
 * pinned. Fresh MM power and a freshly observed cold normal bank are required
 * before a different backing can replace a retired software slot. No inactive
 * flag, NULL namespace, stale power token or caller-drained boolean grants it.
 * Caller holds camera_control_lock; internal order IRQ -> CAMSV -> S1/SMMU.
 */
enum { CAM_NATIVE_MAX_PINNED_EPOCHS=8 };
struct cam_native_epoch_receipt {
    struct cam_native_epoch_receipt *previous;
    struct x9_camera_smmu *smmu;
    uint64_t epoch;
    unsigned slot_rebound;
    __typeof__(cam_irq) irq;
    __typeof__(cam_irq_fbc) fbc;
    __typeof__(cam_repeat) repeat;
    __typeof__(cam_repeat_cancel) cancel;
    __typeof__(cam_source) source;
    __typeof__(cam_stream_close) stream_close;
    __typeof__(cam_dma_stop) dma_stop;
    __typeof__(cam_sv) sv;
    __typeof__(cam_sv_gate) gate;
    __typeof__(cam_sensor_live) sensor;
    __typeof__(cam_rx_live) rx;
    __typeof__(cam_mm_live) mm_live;
    __typeof__(cam_mm) mm;
    __typeof__(cam_smi) smi;
    __typeof__(cam_smi_a0) a0;
    __typeof__(cam_smi_shared) shared;
    __typeof__(cam_phy) phy;
    __typeof__(cam_mac) mac;
    __typeof__(cam_out) out;
};
static struct cam_native_epoch_receipt *cam_native_epochs;
static unsigned cam_native_epoch_count;
static struct cam_native_epoch_receipt *cam_native_epoch_find(uint64_t epoch,
    struct x9_camera_smmu *m)
{
    for(struct cam_native_epoch_receipt *h=cam_native_epochs;h;h=h->previous)
        if(h->epoch==epoch && h->smmu==m)return h;
    return NULL;
}
/* Historical receipt scope ONLY. In particular, does not validate an old
 * released MM token or grant any DMA/cache/controller access in a new epoch. */
static int cam_native_epoch_recycled_check_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    struct cam_native_epoch_receipt *h=cam_native_epoch_find(epoch,m);
    return !h || !m || m->magic!=CAM_SMMU_MAGIC || !m->native ||
        !m->native_epoch_recycled || !m->native_restored || m->native_started ||
        !m->native_retire_attempted || m->native_retire_epoch!=epoch ||
        m->native_retire_error || m->pending?ESTALE:0;
}
static int cam_native_epoch_recycle_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    int e=0;
    if(!m || m!=cam_smmu_namespace || m->magic!=CAM_SMMU_MAGIC || !m->native ||
       !m->exported || !m->native_restored || !m->native_retire_attempted ||
       m->native_retire_epoch!=epoch || m->native_retire_error || m->native_started ||
       m->pending || m->native_epoch_recycled || !m->map)return ESTALE;
    /* Sensor/session close recorded its ACTUAL reverse journal success.
     * MM is already off: DO NOT touch sensor/receiver/SMMU registers or try
     * to validate the now-released old MM token here. Their actual restore
     * observations remain in the immutable archived close journals. */
    if(cam_sensor_live.phase!=CAM_SENSOR_EMPTY || cam_sensor_live.epoch ||
       !cam_sensor_live.close_attempted || cam_sensor_live.close_error ||
       cam_sensor_session_active || cam_rx_live.epoch || cam_rx_live.sensor_epoch ||
       !cam_rx_live.close_attempted || cam_rx_live.status.mm.restore_errno ||
       cam_mm.active || cam_mm.receive.active || cam_mm_live.epoch ||
       !cam_mm_live.close_attempted || cam_mm_live.close_error || cam_sv.held ||
       !cam_sv.close_attempted || cam_sv.status.restore_errno || cam_sv.error ||
       cam_sv.terminal || cam_sv.request_held || cam_sv.request_slot ||
       cam_phy.held || cam_mac.held || cam_out.held || cam_smi.held || cam_smi_a0.held ||
       cam_smi_shared.held || cam_smi_shared.captured)return ESTALE;
    vm_offset_t receipt_va=0;
    if(kmem_alloc(kernel_map,&receipt_va,round_page(sizeof(struct cam_native_epoch_receipt)),
       KMA_DATA|KMA_ZERO,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
    struct cam_native_epoch_receipt *h=(struct cam_native_epoch_receipt *)receipt_va;
    lck_mtx_lock(&camera_irq_lock);
    boolean_t old=cam_irq_spin_take();
    if(cam_irq.epoch!=epoch || cam_irq.prepared || !cam_irq.close_attempted || cam_irq.owns ||
       cam_irq.accepting || cam_irq.busy || cam_irq.count || cam_irq.error ||
       cam_irq.dirty_route || cam_irq.dirty_priority || cam_irq.uncertain_route ||
       cam_irq.uncertain_priority || cam_irq_fbc.error || cam_irq_fbc.source_error ||
       cam_repeat.error || cam_source.error || cam_stream_close.error || cam_dma_stop.error)e=ESTALE;
    if(!e && !m->native_unstarted_retired &&
       (!cam_irq_fbc.bound || !cam_repeat_retirement_receipt_locked() ||
        cam_irq_fbc.epoch!=epoch || cam_irq_fbc.source_phase!=CAM_STREAM_STOPPED ||
        cam_irq_fbc.source_issued!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
        cam_irq_fbc.source_ack!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
        !cam_sv.credit_retired || !cam_source.captured || cam_source.phase!=CAM_SOURCE_STOPPED ||
        cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED || !cam_stream_close.controls))e=ESTALE;
    if(!e && m->native_unstarted_retired &&
       (cam_irq_fbc.bound || cam_irq_fbc.source_issued || cam_sv.credit_issued ||
        cam_source.captured || m->native_setup_issued))e=ESTALE;
    if(!e) {h->irq=cam_irq;h->fbc=cam_irq_fbc;h->repeat=cam_repeat;h->cancel=cam_repeat_cancel;}
    cam_irq_spin_drop(old);
    /* The always-powered GIC is independently live; never sample the gated
     * camera CSR to assert idle after power removal. Prior config restoration
     * was consumed before MM-off; actual enable/pending/active must stay empty. */
    if(!e)e=cam_irq_mapping();if(!e)e=cam_irq_controller();if(!e)e=cam_irq_idle();
    if(!e){delay(1);e=cam_irq_idle();}
    lck_mtx_lock(&camera_camsv_lock);lck_mtx_lock(&camera_s1_lock);
    if(!e && (!cam_irq.slot || cam_irq.slot->last_errno || m->map->raw_errno ||
       cam_irq.slot->state!=(m->native_unstarted_retired?X9_CAMSV_EMPTY:
           cam_repeat_cancel.cancelled?X9_CAMSV_CANCELLED:X9_CAMSV_RETIRED) ||
       m->map->raw_state!=(m->native_unstarted_retired?CAM_RAW_PRIVATE:
           cam_repeat_cancel.cancelled?CAM_RAW_CANCELLED:CAM_RAW_IDLE)))e=ESTALE;
    if(!e && !m->native_unstarted_retired &&
       (cam_irq.slot->dma.backing!=m->map || !cam_s1_dma_match(m->map,cam_irq.slot)))e=ESTALE;
    lck_mtx_unlock(&camera_s1_lock);lck_mtx_unlock(&camera_camsv_lock);
    if(!e) {
        h->epoch=epoch;h->smmu=m;h->source=cam_source;h->stream_close=cam_stream_close;
        h->dma_stop=cam_dma_stop;h->sv=cam_sv;h->gate=cam_sv_gate;
        h->sensor=cam_sensor_live;h->rx=cam_rx_live;h->mm_live=cam_mm_live;h->mm=cam_mm;
        h->smi=cam_smi;h->a0=cam_smi_a0;h->shared=cam_smi_shared;
        h->phy=cam_phy;h->mac=cam_mac;h->out=cam_out;
        h->previous=cam_native_epochs;cam_native_epochs=h;cam_native_epoch_count++;
        /* The permanent thread/maps, monotonic last_epoch, and all sensor slots
         * stay intact. Selected RETIRED slot is NOT made EMPTY here. */
        old=cam_irq_spin_take();
        bzero(&cam_irq_fbc,sizeof(cam_irq_fbc));bzero(&cam_repeat,sizeof(cam_repeat));
        bzero(&cam_repeat_cancel,sizeof(cam_repeat_cancel));
        bzero(&cam_irq.request,sizeof(cam_irq.request));bzero(&cam_irq.sof_frame,sizeof(cam_irq.sof_frame));
        bzero(cam_irq.queue,sizeof(cam_irq.queue));bzero(cam_irq.deliveries,sizeof(cam_irq.deliveries));
        cam_irq.head=0;cam_irq.sof_seen=false;cam_irq.reported=0;
        cam_irq.epoch=0;cam_irq.slot=NULL;
        cam_irq_spin_drop(old);
        bzero(&cam_source,sizeof(cam_source));cam_source.pending=CAM_SOURCE_STEPS;
        bzero(&cam_stream_close,sizeof(cam_stream_close));bzero(&cam_dma_stop,sizeof(cam_dma_stop));
        lck_mtx_lock(&camera_smmu_lock);m->native_epoch_recycled=true;lck_mtx_unlock(&camera_smmu_lock);
    }
    lck_mtx_unlock(&camera_irq_lock);
    if(e)kmem_free(kernel_map,receipt_va,round_page(sizeof(*h)));
    printf("X9 CAMERA NATIVE EPOCH: old=%llu recycled=%u retained_epochs=%u errno=%d "
        "(physical suppliers closed; complete history/backing pinned, slot still retired)\n",
        epoch,m->native_epoch_recycled,cam_native_epoch_count,e);
    return e;
}
/* Exact new dormant epoch. Native prepare already holds fresh suppliers;
 * software thread/map existence is not a live producer or DMA grant. */
static int cam_native_reopen_dormant_locked(uint64_t epoch,uint64_t token)
{
    int e=0;
    if(!epoch || epoch!=cam_sensor_live.epoch || epoch!=cam_irq.epoch ||
       cam_sensor_live.close_attempted || cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED ||
       !cam_mm.active || cam_mm.receive.power_token!=token || !cam_sv.held ||
       cam_sv.epoch!=epoch || cam_sv.close_attempted || cam_sv.request_held ||
       cam_sv.credit_issued || cam_sv.receive_issued || cam_source.captured ||
       cam_sv_gate.issued || session.capture || session.stream)return ESTALE;
    boolean_t old=cam_irq_spin_take();
    if(!cam_irq.prepared || cam_irq.close_attempted || cam_irq.owns || cam_irq.accepting ||
       cam_irq.busy || cam_irq.count || cam_irq.error || cam_irq_fbc.bound ||
       cam_irq_fbc.source_issued || cam_repeat.enabled)e=ESTALE;
    cam_irq_spin_drop(old);
    if(!e)e=x9_mm_power_validate(token,X9_MM_POWER_CAMERA);
    if(!e)e=cam_sv_environment();if(!e)e=cam_irq_mapping();if(!e)e=cam_irq_power();
    if(!e)e=cam_irq_controller();if(!e)e=cam_irq_idle();
    struct cam_sensor_ap ap={&cam_journal.sensor,
        cam_sensor_slave(cam_sensor_live.probe.sensor)};
    const uint16_t regs[]={0x0100,0x0104,0x0005};
    for(unsigned sample=0;sample<2 && !e;sample++) {
        uint8_t value;
        for(unsigned i=0;i<3 && !e;i++) {
            e=cam_sensor_ap_read(&ap,regs[i],&value);
            if(!e && value!=(i==2?0xff:0))e=ESTALE;
        }
        if(!sample && !e)delay(1);
    }
    return e;
}
static int cam_native_reopen_locked(uint64_t epoch,struct x9_camera_smmu *previous,
    uint64_t token,uint64_t iova,uint32_t bytes,struct x9_camera_smmu **out,
    struct x9_camera_smmu_export *view)
{
    if(!previous || previous!=cam_smmu_namespace || !out || *out || !view)return EINVAL;
    int e=cam_native_epoch_recycled_check_locked(previous->native_retire_epoch,previous);
    /* Bound accumulation: up to eight exported lifetimes remain pinned.
     * Refuse BEFORE namespace/slot mutation or new hardware exposure. */
    if(!e && cam_native_epoch_count>=CAM_NATIVE_MAX_PINNED_EPOCHS)e=ENOSPC;
    if(e)return e;
    lck_mtx_lock(&camera_irq_lock);
    e=cam_native_reopen_dormant_locked(epoch,token);
    struct cam_native_snapshot prior;
    cam_native_prior(previous,&prior);
    struct x9_camera_smmu probe={0};probe.mmio=previous->mmio;
    probe.native_wp=previous->native_wp;probe.native_power=token;
    /* Fresh token, never the released historical token. Entire normal bank
     * must still be the actual consumed prior, not merely own-base equality. */
    if(!e)e=cam_native_match(&probe,&prior);
    if(!e){delay(1);e=cam_native_match(&probe,&prior);}
    lck_mtx_lock(&camera_camsv_lock);lck_mtx_lock(&camera_s1_lock);
    struct cam_native_epoch_receipt *slot_history=NULL;
    unsigned which=cam_sv.status.sensor-1;
    if(!e && (which>=X9_CAMERA_SENSOR_COUNT ||
       cam_irq.slot!=&cam_irq.slots[which] || cam_irq.slot->last_errno))e=ESTALE;
    if(!e && (cam_irq.slot->state==X9_CAMSV_RETIRED || cam_irq.slot->state==X9_CAMSV_CANCELLED)) {
        for(struct cam_native_epoch_receipt *h=cam_native_epochs;h;h=h->previous) {
            if(h->smmu->map!=cam_irq.slot->dma.backing)continue;
            if(h->smmu->native_unstarted_retired || (h->slot_rebound&(1u<<which)) ||
               cam_native_epoch_recycled_check_locked(h->epoch,h->smmu) ||
               h->smmu->map->raw_state!=(h->cancel.cancelled?CAM_RAW_CANCELLED:CAM_RAW_IDLE) ||
               (h->cancel.cancelled && (cam_irq.slot->state!=X9_CAMSV_CANCELLED ||
                memcmp(&cam_irq.slot->request,&h->cancel.next,sizeof(h->cancel.next)))) ||
               h->smmu->map->raw_errno ||
               !cam_s1_dma_match(h->smmu->map,cam_irq.slot) ||
               memcmp(cam_irq.slot,&h->irq.slots[which],sizeof(*cam_irq.slot)))e=ESTALE;
            else slot_history=h;
            break;
        }
        if(!slot_history && !e)e=ESTALE;
    } else if(!e && cam_irq.slot->state!=X9_CAMSV_EMPTY)e=EBUSY;
    if(!e && slot_history) {
        /* Archive above already contains every old slot/request/callback.
         * No old map is freed, moved or made DEVICE. This rebuild follows
         * actual whole-epoch retirement plus fresh cold-bank proof only. */
        slot_history->slot_rebound|=1u<<which;bzero(cam_irq.slot,sizeof(*cam_irq.slot));
    }
    lck_mtx_unlock(&camera_s1_lock);lck_mtx_unlock(&camera_camsv_lock);
    if(!e)e=cam_native_smmu_start_impl(token,iova,bytes,out,view,previous);
    printf("X9 CAMERA NATIVE REOPEN: old=%llu new=%llu sensor=%u tombstone=%u exported=%u errno=%d "
        "(fresh MM/cold bank; old backing/history retained, bounded lifetimes)\n",
        previous->native_retire_epoch,epoch,cam_sv.status.sensor,slot_history!=NULL,
        *out!=NULL,e);
    lck_mtx_unlock(&camera_irq_lock);return e;
}
