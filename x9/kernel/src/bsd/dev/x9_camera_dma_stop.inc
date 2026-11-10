/* Normal ISP8s CAMSV0 DMA-top stop request, after real streamed controls.
 * sv_reset writes base_dma+20=1 and polls bit1 (100ms); its subsequent
 * STG/CQ/central resets, inner stores/debug sweep/void-success are NOT used.
 * Restore only this stop command and our DCM inhibit after the real owned
 * OFF/DONE/CPU/worker/GIC boundary and observed ACK/port retirement. No
 * STG/CQ/central reset or claim of whole AXI/SMMU retirement. Native request
 * check independently grants this DMA-top scope; backing stays pinned.
 * Control + IRQ mutex held. All observations precede cached comparison.
 */
enum { CAM_DMA_STOP_IDLE, CAM_DMA_STOP_CLOCK, CAM_DMA_STOP_PENDING, CAM_DMA_STOP_ACKED,
    CAM_DMA_STOP_RESTORING, CAM_DMA_STOP_RESTORED };
static struct {
    uint64_t epoch;
    struct x9_camera_camsv_request request;
    unsigned phase,samples,port_samples;
    bool captured,issued,dcm_issued;
    bool restore_attempted,dcm_restore_issued,release_issued;
    uint32_t dcm_restore_seen,release_seen,restore_samples;
    uint32_t prior[2],actual[2],seen[2],sampled,ostd[2],ostd_sampled;
    uint32_t dcm_prior[2],dcm_actual[2],dcm_seen,dcm_sampled,dcm_samples;
    int error;
} cam_dma_stop;
static int cam_sv_dma_stop_fault(int e)
{
    if(!cam_dma_stop.error)cam_dma_stop.error=e;
    return cam_dma_stop.error;
}
static int cam_sv_dma_stop_observe(unsigned bank,vm_offset_t view,unsigned offset,
    uint32_t value,bool *allowed)
{
    if((offset!=0x1020 && offset!=0x7c) || !cam_dma_stop.captured)return 0;
    if(bank>=2 || view!=cam_sv.view[bank])return cam_sv_dma_stop_fault(ESTALE);
    if(offset==0x7c) {
        cam_dma_stop.dcm_actual[bank]=value;cam_dma_stop.dcm_sampled|=1u<<bank;
    } else {cam_dma_stop.actual[bank]=value;cam_dma_stop.sampled|=1u<<bank;}
    if(cam_dma_stop.error)return cam_dma_stop.error;
    if(cam_dma_stop.epoch!=cam_sv.epoch || !cam_sv.request_held || cam_sv.terminal ||
       memcmp(&cam_dma_stop.request,&cam_sv.request,sizeof(cam_sv.request)))
        return cam_sv_dma_stop_fault(ESTALE);
    if(offset==0x7c) {
        if(!cam_dma_stop.dcm_issued)
            return value==cam_dma_stop.dcm_prior[bank]?0:cam_sv_dma_stop_fault(ESTALE);
        if(cam_dma_stop.dcm_restore_issued) {
            if((cam_dma_stop.phase!=CAM_DMA_STOP_RESTORING &&
                cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED) ||
               (value!=cam_dma_stop.dcm_prior[bank] &&
                ((cam_dma_stop.dcm_restore_seen&(1u<<bank)) || value!=1)))
                return cam_sv_dma_stop_fault(ESTALE);
            if(value==cam_dma_stop.dcm_prior[bank])cam_dma_stop.dcm_restore_seen|=1u<<bank;
            *allowed=true;return 0;
        }
        if(value!=1 && ((cam_dma_stop.dcm_seen&(1u<<bank)) ||
           value!=cam_dma_stop.dcm_prior[bank]))return cam_sv_dma_stop_fault(ESTALE);
        if(value==1)cam_dma_stop.dcm_seen|=1u<<bank;
        *allowed=true;return 0;
    }
    if(!cam_dma_stop.issued) {
        return value==cam_dma_stop.prior[bank]?0:cam_sv_dma_stop_fault(ESTALE);
    }
    /* The normal reader requires ACK bit1, NOT sticky request bit0. Accept
     * full status2 or3 only after OUR issued command; do not infer that
     * bit0 cannot self-clear. ACK cannot regress, and a cleared command
     * at ACK cannot reappear. Pre-ACK prior reads grant nothing and never
     * authorize cancellation/retry. No reserved/reset bits are allowed.
     * Inner may remain prior: no guessed inner ACK requirement or write. */
    if(cam_dma_stop.release_issued && value==cam_dma_stop.prior[bank]) {
        if(cam_dma_stop.phase!=CAM_DMA_STOP_RESTORING &&
           cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED)return cam_sv_dma_stop_fault(ESTALE);
        cam_dma_stop.release_seen|=1u<<bank;*allowed=true;return 0;
    }
    if((cam_dma_stop.release_seen&(1u<<bank)) ||
       (cam_dma_stop.phase!=CAM_DMA_STOP_PENDING && cam_dma_stop.phase!=CAM_DMA_STOP_ACKED &&
        cam_dma_stop.phase!=CAM_DMA_STOP_RESTORING) ||
       (value&~3u) || ((cam_dma_stop.seen[bank]&2u) && !(value&2u)) ||
       ((cam_dma_stop.seen[bank]&4u) && (value&1u)))return cam_sv_dma_stop_fault(ESTALE);
    if(value&1u)cam_dma_stop.seen[bank]|=1u;
    if(value&2u)cam_dma_stop.seen[bank]|=2u;
    if(value==2)cam_dma_stop.seen[bank]|=4u;
    *allowed=true;return 0;
}
/* Read-only busy/comparison exceptions. They grant neither authority nor
 * observation, only this exact issued command's ordinary request/status.
 * All real samples above have already been recorded; cold has no exception. */
static uint32_t cam_sv_dma_stop_busy_exemption(unsigned offset)
{
    return offset==0x1020 && cam_dma_stop.captured && cam_dma_stop.issued &&
        !cam_dma_stop.error && cam_dma_stop.epoch==cam_sv.epoch?3u:0;
}
static bool cam_sv_dma_stop_compare_allowed(unsigned bank,unsigned offset,uint32_t value)
{
    if(bank<2 && offset==0x7c && cam_dma_stop.captured && cam_dma_stop.dcm_issued &&
       !cam_dma_stop.error && cam_dma_stop.epoch==cam_sv.epoch)
        return (cam_dma_stop.dcm_sampled&(1u<<bank)) && value==cam_dma_stop.dcm_actual[bank];
    return bank<2 && cam_sv_dma_stop_busy_exemption(offset) &&
        (cam_dma_stop.sampled&(1u<<bank)) && value==cam_dma_stop.actual[bank];
}
static int cam_sv_dma_stop_guard_locked(uint64_t epoch)
{
    int e;
    if(cam_dma_stop.error)return cam_dma_stop.error;
    if(!cam_dma_stop.captured || epoch!=cam_dma_stop.epoch || !cam_stream_close.controls ||
       !cam_stream_close.admitted || cam_stream_close.checking || cam_stream_close.error ||
       cam_stream_close.epoch!=epoch || cam_source.phase!=CAM_SOURCE_STOPPED ||
       cam_source.error || !cam_sv_gate_off_ready() ||
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_QUIESCENT ||
       !cam_smi.held || !cam_smi_a0.held || !cam_smi.larb[0] ||
       cam_smi.epoch!=epoch || cam_smi_a0.epoch!=epoch || cam_smi.close_attempted ||
       cam_smi_a0.close_attempted || cam_smi.error || cam_smi.status.a0.last_errno ||
       memcmp(&cam_dma_stop.request,&cam_stream_close.request,sizeof(cam_dma_stop.request)))return ESTALE;
    e=cam_stream_close_guard_locked(epoch);if(e)return e;
    /* Full actual CAMSV/TG/SMI/receiver/PHY/MAC/OUTMUX plus original S1
     * range/owner, not an idle register or software receipt used as a grant. */
    e=cam_sv_request_guard_locked(epoch,cam_irq.slot);if(e)return e;
    return cam_smi_a0_guard();
}
static int cam_sv_dma_stop_sample_locked(void)
{
    bool allowed=false;
    int e=cam_sv_power_guard();if(e)return e;
    return cam_sv_dma_stop_observe(0,cam_sv.view[0],0x1020,
        cam_rr(cam_sv.view[0],0x1020),&allowed);
}
static int cam_sv_dma_stop_port_sample_locked(void)
{
    int e=cam_smi_a0_guard();if(e)return e;
    /* mtk-smi.c's real L19/P0 monitor pair: external+280 and internal+780.
     * Preserve raw values. Zero is necessary downstream evidence, not a
     * controller/route grant or proof of SMMU/EMI/worker retirement. */
    cam_dma_stop.ostd[0]=cam_rr(cam_smi.larb[0],0x280);cam_dma_stop.ostd_sampled|=1;
    cam_dma_stop.ostd[1]=cam_rr(cam_smi.larb[0],0x780);cam_dma_stop.ostd_sampled|=2;
    return 0;
}
static int cam_sv_dma_stop_locked(uint64_t epoch)
{
    uint64_t span,end;
    unsigned dcm_reg=cam_sv_find(0x7c),stop_reg=cam_sv_find(0x1020);
    int e;
    if(cam_dma_stop.captured)return cam_dma_stop.error?cam_dma_stop.error:ESTALE;
    if(dcm_reg>=cam_sv.count || stop_reg>=cam_sv.count)return ESTALE;
    for(unsigned bank=0;bank<2;bank++) {
        if(cam_sv.expected[bank][dcm_reg]>1)return EBUSY;
        if(cam_sv.expected[bank][stop_reg])return EBUSY;
        cam_dma_stop.dcm_prior[bank]=cam_sv.expected[bank][dcm_reg];
        cam_dma_stop.prior[bank]=cam_sv.expected[bank][stop_reg];
    }
    cam_dma_stop.captured=true;cam_dma_stop.epoch=epoch;cam_dma_stop.request=cam_sv.request;
    /* Strict normal admission: no pre-existing halt/ACK/reset inherited or
     * cleared. This phase does not acquire whole-larb or CQ reset authority. */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
        for(unsigned bank=0;bank<2;bank++) {
            bool allowed=false;
            e=cam_sv_power_guard();if(e)goto out;
            e=cam_sv_dma_stop_observe(bank,cam_sv.view[bank],0x1020,
                cam_rr(cam_sv.view[bank],0x1020),&allowed);if(e)goto out;
        }
    }
    nanoseconds_to_absolutetime(100000000,&span);end=mach_absolute_time()+span;
    e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
    if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
    /* Normal common_disable first inhibits DMA DCM (+7c=1). Do not issue
     * a stop while its internal clock can be gated. This ordinary control
     * readback is NOT a DMA ACK. Borrowed1 is never rewritten/released;
     * inner may remain prior, and no DB/inner/reset store is synthesized. */
    cam_dma_stop.phase=CAM_DMA_STOP_CLOCK;
    if(!cam_dma_stop.dcm_prior[0]) {
        cam_dma_stop.dcm_issued=true;
        cam_rw(cam_sv.view[0],0x7c,1);
    }
    for(unsigned poll=0;poll<100000;poll++) {
        bool allowed=false;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_sv_dma_stop_guard_locked(epoch);if(e)break;
        e=cam_sv_power_guard();if(e)break;
        e=cam_sv_dma_stop_observe(0,cam_sv.view[0],0x7c,
            cam_rr(cam_sv.view[0],0x7c),&allowed);if(e)break;
        cam_dma_stop.dcm_samples=cam_dma_stop.dcm_actual[0]==1?cam_dma_stop.dcm_samples+1:0;
        if(cam_dma_stop.dcm_samples==2)break;
        delay(1);
    }
    if(!e && cam_dma_stop.dcm_samples!=2)e=ETIMEDOUT;
    if(!e)e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
    if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
    cam_dma_stop.phase=CAM_DMA_STOP_PENDING;cam_dma_stop.issued=true;
    cam_rw(cam_sv.view[0],0x1020,1); /* BEFORE possibly posted store: issue above */
    for(unsigned poll=0;poll<100000;poll++) {
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_sv_dma_stop_guard_locked(epoch);if(e)break;
        e=cam_sv_dma_stop_sample_locked();if(e)break;
        cam_dma_stop.samples=(cam_dma_stop.actual[0]&2u)?cam_dma_stop.samples+1:0;
        if(cam_dma_stop.samples>=2) {
            e=cam_sv_dma_stop_port_sample_locked();if(e)break;
            cam_dma_stop.port_samples=(!cam_dma_stop.ostd[0] && !cam_dma_stop.ostd[1])?
                cam_dma_stop.port_samples+1:0;
            if(cam_dma_stop.port_samples==2)break;
        }
        delay(1);
    }
    if(!e && (cam_dma_stop.samples<2 || cam_dma_stop.port_samples!=2))e=ETIMEDOUT;
    if(!e)e=cam_sv_dma_stop_guard_locked(epoch);
    if(!e)e=cam_sv_dma_stop_sample_locked();
    if(!e)e=cam_sv_dma_stop_port_sample_locked();
    if(!e && (!(cam_dma_stop.actual[0]&2u) || cam_dma_stop.ostd[0] || cam_dma_stop.ostd[1]))e=ESTALE;
    if(!e)e=cam_sv_dma_stop_guard_locked(epoch);
    if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    if(!e)cam_dma_stop.phase=CAM_DMA_STOP_ACKED;
out:
    if(e)e=cam_sv_dma_stop_fault(e);
    printf("X9 CAMERA DMA STOP: epoch=%llu capture_id=%llu phase=%u issued=%u "
        "dcm_issued=%u dcm=%x/%x dcm_sampled=%x dcm_seen=%x dcm_samples=%u "
        "prior=%x/%x actual=%x/%x sampled=%x seen=%x/%x samples=%u "
        "ostd=%x/%x ostd_sampled=%x port_samples=%u errno=%d "
        "(request held; ACK+port observations, NOT full AXI/SMMU/map drain)\n",
        epoch,cam_dma_stop.request.frame.capture_id,cam_dma_stop.phase,cam_dma_stop.issued,
        cam_dma_stop.dcm_issued,cam_dma_stop.dcm_actual[0],cam_dma_stop.dcm_actual[1],
        cam_dma_stop.dcm_sampled,cam_dma_stop.dcm_seen,cam_dma_stop.dcm_samples,
        cam_dma_stop.prior[0],cam_dma_stop.prior[1],cam_dma_stop.actual[0],cam_dma_stop.actual[1],
        cam_dma_stop.sampled,cam_dma_stop.seen[0],cam_dma_stop.seen[1],cam_dma_stop.samples,
        cam_dma_stop.ostd[0],cam_dma_stop.ostd[1],cam_dma_stop.ostd_sampled,cam_dma_stop.port_samples,e);
    return e;
}
/* Restore a complete, actually consumed DMA stop, never a partial request.
 * Upstream sv_reset's final normal order is DCM inhibit0 then DMA SW_RST0;
 * only our changed DCM field is returned. No inherited1 is released.
 * This is NOT credit cancellation, full bus/map retirement or RAW rearm.
 * Called while the original control/IRQ locks and native lease still hold,
 * BEFORE IRQ close starts priority/route unwind, so its original qualified
 * worker/GIC/FBC receipt can be revalidated before every posted store.
 */
static int cam_sv_dma_restore_sample_locked(bool *match)
{
    int e;
    *match=true;
    for(unsigned bank=0;bank<2;bank++) {
        bool allowed=false;
        e=cam_sv_power_guard();if(e)return e;
        e=cam_sv_dma_stop_observe(bank,cam_sv.view[bank],0x7c,
            cam_rr(cam_sv.view[bank],0x7c),&allowed);if(e)return e;
        e=cam_sv_dma_stop_observe(bank,cam_sv.view[bank],0x1020,
            cam_rr(cam_sv.view[bank],0x1020),&allowed);if(e)return e;
        if(cam_dma_stop.dcm_actual[bank]!=cam_dma_stop.dcm_prior[bank] ||
           (cam_dma_stop.release_issued && cam_dma_stop.actual[bank]!=cam_dma_stop.prior[bank]))
            *match=false;
    }
    e=cam_sv_dma_stop_port_sample_locked();if(e)return e;
    if(cam_dma_stop.ostd[0] || cam_dma_stop.ostd[1])return EBUSY;
    return 0;
}
static int cam_sv_dma_stop_restore_locked(uint64_t epoch)
{
    uint64_t span,end;
    int e;
    if(!cam_dma_stop.captured || cam_dma_stop.epoch!=epoch ||
       cam_dma_stop.restore_attempted || !cam_dma_stop.issued ||
       cam_dma_stop.phase!=CAM_DMA_STOP_ACKED || cam_dma_stop.error ||
       cam_dma_stop.samples<2 || cam_dma_stop.port_samples!=2 ||
       cam_dma_stop.ostd_sampled!=3)return cam_dma_stop.error?cam_dma_stop.error:ESTALE;
    cam_dma_stop.restore_attempted=true; /* no retry even before first store */
    nanoseconds_to_absolutetime(100000000,&span);end=mach_absolute_time()+span;
    /* Fresh ACK+P0-zero, original OFF/DONE/CPU/worker/GIC and current lease,
     * not the cached prior stop samples used as an independent grant. */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
        e=cam_sv_dma_stop_sample_locked();if(e)goto out;
        e=cam_sv_dma_stop_port_sample_locked();if(e)goto out;
        if(!(cam_dma_stop.actual[0]&2u) || cam_dma_stop.ostd[0] || cam_dma_stop.ostd[1]) {
            e=ESTALE;goto out;
        }
        if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
    }
    cam_dma_stop.phase=CAM_DMA_STOP_RESTORING;
    if(cam_dma_stop.dcm_issued) {
        e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
        cam_dma_stop.dcm_restore_issued=true;
        cam_rw(cam_sv.view[0],0x7c,cam_dma_stop.dcm_prior[0]);
    }
    /* Two complete target samples at each stage; DCM must be restored
     * before SW_RST is released. Commands are journaled BEFORE the store. */
    for(unsigned stage=0;stage<2;stage++) {
        bool complete=false;
        if(stage) {
            e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
            if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
            cam_dma_stop.release_issued=true;
            cam_rw(cam_sv.view[0],0x1020,cam_dma_stop.prior[0]);
        }
        cam_dma_stop.restore_samples=0;
        for(unsigned poll=0;poll<100000;poll++) {
            bool match;
            if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
            e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
            e=cam_sv_dma_restore_sample_locked(&match);if(e)goto out;
            if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
            cam_dma_stop.restore_samples=match?cam_dma_stop.restore_samples+1:0;
            if(cam_dma_stop.restore_samples==2){complete=true;break;}
            delay(1);
        }
        if(!complete){e=ETIMEDOUT;goto out;}
    }
    e=cam_sv_dma_stop_guard_locked(epoch);if(e)goto out;
    if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
    cam_dma_stop.phase=CAM_DMA_STOP_RESTORED;
out:
    if(e)e=cam_sv_dma_stop_fault(e);
    printf("X9 CAMERA DMA RESTORE: epoch=%llu capture_id=%llu phase=%u "
        "issued=%u/%u seen=%x/%x samples=%u dcm=%x/%x stop=%x/%x "
        "ostd=%x/%x errno=%d (own stop/DCM only; credit/map/backing retained)\n",
        epoch,cam_dma_stop.request.frame.capture_id,cam_dma_stop.phase,
        cam_dma_stop.dcm_restore_issued,cam_dma_stop.release_issued,
        cam_dma_stop.dcm_restore_seen,cam_dma_stop.release_seen,cam_dma_stop.restore_samples,
        cam_dma_stop.dcm_actual[0],cam_dma_stop.dcm_actual[1],
        cam_dma_stop.actual[0],cam_dma_stop.actual[1],cam_dma_stop.ostd[0],cam_dma_stop.ostd[1],e);
    return e;
}
