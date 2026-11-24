/* Native A0 end-to-end retirement, AFTER the real local IRQ stop receipt.
 * Separate source OFF, accepted spent FBC, CPU/worker/GIC, DMA halt ACK,
 * downstream A0 outstanding/bus idle, targeted SMMU and cold-bank restore.
 * A zero status or software stop flag alone cannot enter this operation.
 * No synthetic drained bool, fake DONE/frame, reset, unmap or backing free.
 */
static int cam_native_producer_guard_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    static unsigned failure_logs;
    const char *step="identity";
    uint32_t detail=0;
    int e;
    if(!m || m!=cam_smmu_namespace || m->magic!=CAM_SMMU_MAGIC || !m->native ||
       !epoch || epoch!=cam_irq.epoch || epoch!=cam_sv.epoch || epoch!=cam_sensor_live.epoch) {
        e=CAM_FAIL(ESTALE);goto out;
    }
    step="mm-power-token";
    if(!cam_mm.active || cam_mm.receive.power_token!=m->native_power) {
        e=CAM_FAIL(ESTALE);goto out;
    }
    step="request-credit-map";
    if(cam_sensor_live.close_attempted || cam_rx_live.close_attempted || cam_sv.close_attempted ||
       cam_sv.terminal || cam_sv.error || !cam_sv.request_held || !cam_sv.credit_retired ||
       cam_sv.request_dma.backing!=m->map || cam_sv.request.iova!=m->map->iova) {
        e=CAM_FAIL(ESTALE);goto out;
    }
    step="source-gate-receive";
    if(!cam_source.captured || cam_source.phase!=CAM_SOURCE_STOPPED || cam_source.error ||
       !cam_sv_gate_off_ready() || cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_QUIESCENT ||
       !cam_stream_close_restore_ready()) {
        e=CAM_FAIL(ESTALE);goto out;
    }
    step="dma-stop";
    if(!cam_dma_stop.issued || cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED ||
       cam_dma_stop.epoch!=epoch || cam_dma_stop.error || cam_dma_stop.samples<2 ||
       cam_dma_stop.port_samples!=2 || cam_dma_stop.restore_samples!=2 ||
       cam_dma_stop.release_seen!=3) {
        e=CAM_FAIL(ESTALE);goto out;
    }
    step="smi-lifetimes";
    if(!cam_smi.held || !cam_smi_a0.held || cam_smi.epoch!=epoch || cam_smi_a0.epoch!=epoch ||
       cam_smi.close_attempted || cam_smi_a0.close_attempted || !cam_smi_shared.common ||
       cam_smi_shared.epoch!=epoch) {
        e=CAM_FAIL(ESTALE);goto out;
    }
    step="mm-power-validate";
    e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);if(e)goto out;
    step="irq-mapping";
    e=cam_irq_mapping();if(e)goto out;
    step="irq-power";
    e=cam_irq_power();if(e)goto out;
    step="irq-controller";
    e=cam_irq_controller();if(e)goto out;
    step="irq-idle";
    e=cam_irq_idle();if(e)goto out;
    step="irq-retired-slot";
    e=cam_irq_credit_retired_slot();if(e)goto out;
    step="irq-spin-state";
    boolean_t old=cam_irq_spin_take();
    e=cam_irq.error?cam_irq.error:cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
    if(!e && (cam_irq.prepared || !cam_irq.close_attempted || cam_irq.owns ||
       cam_irq.accepting || cam_irq.busy || cam_irq.count || !cam_irq_fbc.bound ||
       !cam_repeat_retirement_receipt_locked() || cam_irq_fbc.epoch!=epoch ||
       cam_irq_fbc.source_phase!=CAM_STREAM_STOPPED ||
       cam_irq_fbc.source_issued!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_ack!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
       cam_irq_fbc.source_control || cam_irq_fbc.source_frame_count!=0xff))e=CAM_FAIL(ESTALE);
    for(unsigned bank=0;bank<2 && !e;bank++) {
        uint32_t ctl=cam_irq_rd(cam_irq.view[bank],0x540);
        uint32_t count=cam_irq_rd(cam_irq.view[bank],0x544);
        step="irq-fbc-bank";
        e=cam_irq_fbc_note_locked(bank,ctl,count);
        if(!e && ((count&CAM_IRQ_FBC_CNT) || count!=cam_sv.credit_spent_count[bank])) {
            detail=(bank<<24)|(count&0x00ffffffu);e=CAM_FAIL(ESTALE);
        }
    }
    cam_irq_spin_drop(old);if(e)goto out;
    /* No request-authority callback here: BBM/CR0 disable must not recurse
     * into old translated native_started checks. The same actual endpoint
     * journals, pinned map and source/port checks remain independently live. */
    step="sv-guard";
    e=cam_sv_guard();if(e)goto out;
    step="rx-guard";
    e=cam_rx_session_guard();if(e)goto out;
    step="mac-guard";
    e=cam_mac_guard();if(e)goto out;
    step="phy-guard";
    e=cam_phy_guard();if(e)goto out;
    step="outmux-guard";
    e=cam_out_guard();if(e)goto out;
    struct cam_sensor_ap ap={&cam_journal.sensor,cam_sensor_slave(cam_sv.request.frame.sensor)};
    uint8_t byte;
    const uint16_t regs[]={0x0100,0x0104,0x0005};
    for(unsigned i=0;i<3;i++) {
        step="sensor-read";
        detail=regs[i];
        e=cam_sensor_ap_read(&ap,regs[i],&byte);if(e)goto out;
        if(byte!=(i==2?0xff:0)) {
            step="sensor-value";
            detail=(regs[i]<<8)|byte;e=CAM_FAIL(ESTALE);goto out;
        }
    }
    step="smi-a0-guard";
    e=cam_smi_a0_guard();if(e)goto out;
    step="smi-shared-voter";
    e=cam_smi_shared_voter_guard();if(e)goto out;
    for(unsigned sample=0;sample<2;sample++) {
        step="smi-clock-only";
        e=cam_smi_clock_only_guard();if(e)goto out;
        /* OPPO mtk-smi: external/internal L19P0 OSTD_MON, LARB STATs;
         * common DEBUG_S OSTD mask 0x7ffe000 and DEBUG_MISC bit0=idle.
         * No debug-selector, clamp, common0 or read-clear/status write. */
        static const uint16_t larb_off[]={0x280,0x780,0,0x500};
        for(unsigned n=0;n<sizeof(larb_off)/sizeof(larb_off[0]);n++) {
            uint32_t v=cam_rr(cam_smi.larb[0],larb_off[n]);
            if(v) {
                step="smi-larb-busy";detail=(larb_off[n]<<16)|(v&0xffffu);
                e=CAM_FAIL(EBUSY);goto out;
            }
        }
        for(unsigned p=0;p<8;p++) {
            uint32_t v=cam_rr(cam_smi_shared.common,0x400+4*p)&0x07ffe000u;
            if(v) {
                step="smi-common-ostd";detail=((0x400+4*p)<<16)|((v>>13)&0xffffu);
                e=CAM_FAIL(EBUSY);goto out;
            }
        }
        if(!(cam_rr(cam_smi_shared.common,0x440)&1u)) {
            step="smi-common-not-idle";e=CAM_FAIL(EBUSY);goto out;
        }
        if(!sample)delay(10);
    }
    step="irq-power-final";
    e=cam_irq_power();
out:
    if(e && failure_logs++<16)
        printf("X9 CAMERA NATIVE PRODUCER GUARD FAIL: epoch=%llu step=%s errno=%d detail=%x\n",
            epoch,step,e,detail);
    return e;
}
/* Caller holds control + IRQ mutex. This old-request authority admission
 * is evaluated BEFORE BBM, never recursively from a phase guard afterwards. */
static int cam_native_retire_irq_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    int e=cam_irq_credit_retired_guard(epoch);
    if(!e)e=cam_native_producer_guard_locked(epoch,m);
    if(!e)e=cam_native_smmu_retire_locked(epoch,m);
    return e;
}
/* Caller holds control mutex only, as the native engine's STOP callback. */
static int cam_native_retire_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    lck_mtx_lock(&camera_irq_lock);
    int e=cam_native_retire_irq_locked(epoch,m);
    lck_mtx_unlock(&camera_irq_lock);return e;
}
int x9_camera_native_smmu_retire(uint64_t epoch,struct x9_camera_smmu *m)
{
    lck_mtx_lock(&camera_control_lock);int e=cam_native_retire_locked(epoch,m);
    lck_mtx_unlock(&camera_control_lock);return e;
}
/* Minimal never-hardware-exposed failure close. This is NOT streamed
 * OFF/DONE/retirement: export may have failed in private cache work, but
 * not a single normal-controller setup store or RAW/credit arm was issued.
 * Keep every allocation and original error; prove the real prior normal
 * bank and sensor OFF, rather than inventing a DMA completion receipt.
 * Partial normal startup, queue timeout, ARMED/RETAINED RAW or any credit
 * can NEVER enter here. Caller holds control only. */
static int cam_native_unstarted_retire_locked(uint64_t epoch,struct x9_camera_smmu *m)
{
    int e=0;
    /* Native source prepare creates an exact-epoch dormant IRQ worker
     * BEFORE SMMU export. Retire that real no-issued preparation through
     * its original inactive close; never pretend prepared was already false.
     * A queue/RAW/credit/source lifetime cannot take this branch. */
    boolean_t prior_old=cam_irq_spin_take();
    bool dormant=cam_irq.prepared && cam_irq.epoch==epoch && !cam_irq.owns &&
        !cam_irq.accepting && !cam_irq.busy && !cam_irq.count && !cam_irq.close_attempted &&
        !cam_irq_fbc.bound && !cam_irq_fbc.source_issued && !cam_irq.error;
    cam_irq_spin_drop(prior_old);
    if(dormant && !session.capture && !session.stream && !cam_sv.request_held &&
       !cam_sv.credit_issued && m && m==cam_smmu_namespace && !m->native_setup_issued) {
        lck_mtx_lock(&camera_camsv_lock);
        e=!cam_irq.slot || cam_irq.slot->state!=X9_CAMSV_EMPTY?CAM_FAIL(ESTALE):0;
        lck_mtx_unlock(&camera_camsv_lock);
        if(!e)e=cam_irq_close_locked(epoch);
        if(e)return e;
    }
    lck_mtx_lock(&camera_irq_lock);
    if(!m || m!=cam_smmu_namespace || m->magic!=CAM_SMMU_MAGIC || !m->native ||
       !m->exported || m->native_setup_issued || m->native_started || m->pending ||
       m->native_retire_attempted || !epoch || epoch!=cam_sensor_live.epoch ||
       !cam_mm.active || cam_mm.receive.power_token!=m->native_power ||
       cam_sensor_live.close_attempted || cam_rx_live.close_attempted ||
       cam_sv.request_held || cam_sv.credit_issued || cam_sv.receive_issued ||
       cam_source.captured || cam_sv_gate.pending!=X9_CAM_SV_GATE_STEPS ||
       cam_sv_gate.issued || !m->map ||
       m->map->raw_state!=CAM_RAW_PRIVATE)e=CAM_FAIL(ESTALE);
    boolean_t old=cam_irq_spin_take();
    if(!e && (cam_irq.prepared || cam_irq.owns || cam_irq.accepting || cam_irq.busy ||
       cam_irq.count || cam_irq_fbc.bound || cam_irq_fbc.source_issued))e=CAM_FAIL(ESTALE);
    cam_irq_spin_drop(old);
    if(!e)e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);
    if(!e) {
        struct cam_sensor_ap ap={&cam_journal.sensor,
            cam_sensor_slave(cam_sensor_live.probe.sensor)};
        uint8_t value;
        const uint16_t regs[]={0x0100,0x0104,0x0005};
        for(unsigned i=0;i<3 && !e;i++) {
            e=cam_sensor_ap_read(&ap,regs[i],&value);
            if(!e && value!=(i==2?0xff:0))e=CAM_FAIL(ESTALE);
        }
    }
    if(!e) {
        struct cam_native_snapshot prior;
        cam_native_prior(m,&prior);
        lck_mtx_lock(&camera_smmu_lock);
        e=cam_native_match(m,&prior);
        if(!e){delay(1);e=cam_native_match(m,&prior);}
        if(!e) {
            m->native_retire_attempted=true;m->native_retire_epoch=epoch;
            m->native_unstarted_retired=true;m->native_restored=true;
        }
        lck_mtx_unlock(&camera_smmu_lock);
    }
    printf("X9 CAMERA NATIVE UNSTARTED: epoch=%llu no_setup=%u no_arm=%u retired=%u "
        "errno=%d (never hardware-exposed; backing/original error retained, no fake DONE)\n",
        epoch,m?!m->native_setup_issued:0,!cam_sv.request_held,
        m?m->native_unstarted_retired:0,e);
    lck_mtx_unlock(&camera_irq_lock);return e;
}
int x9_camera_native_smmu_unstarted_retire(uint64_t epoch,struct x9_camera_smmu *m)
{
    lck_mtx_lock(&camera_control_lock);int e=cam_native_unstarted_retire_locked(epoch,m);
    lck_mtx_unlock(&camera_control_lock);return e;
}
/* Old-request teardown scope ONLY, not permission for DEVICE/cache/credit
 * arm or a live DMA check. Exported storage remains pinned after restore.
 * Recheck exact prior normal bank while the real MM token is still held;
 * caller independently validates its same immutable old request and source
 * teardown phase. No NULL handle or caller 'drained' can grant this scope. */
int x9_camera_native_smmu_retirement_check(uint64_t epoch,struct x9_camera_smmu *m)
{
    lck_mtx_lock(&camera_smmu_lock);
    /* A private export/cache error may stay terminal even when independent
     * never-exposed cleanup is proved; do not erase it to permit cleanup. */
    int e=!m || m!=cam_smmu_namespace || m->magic!=CAM_SMMU_MAGIC?CAM_FAIL(EINVAL):0;
    if(!e && !m->native_unstarted_retired)e=cam_smmu_identity(m);
    if(!e && (!m->native || !m->native_retire_attempted || !m->native_restored ||
       m->native_started || m->native_retire_epoch!=epoch || m->native_retire_error || m->pending))e=CAM_FAIL(ESTALE);
    if(!e && !m->native_unstarted_retired)e=cam_smmu_storage(m);
    if(!e) {
        struct cam_native_snapshot prior;
        cam_native_prior(m,&prior);
        e=cam_native_match(m,&prior);
    }
    lck_mtx_unlock(&camera_smmu_lock);return e;
}

#include "x9_camera_native_epoch.inc"
