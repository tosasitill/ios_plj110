/* MT6993/ISP8s single-tag owned-request -> RAW10 completion adapter.
 * Register semantics: current OPPO mtk_cam-sv.c::mtk_irq_camsv_hybrid and
 * mtk_cam-sv-regs.h. Status values are captured ONCE by the sole IRQ owner:
 * DONE outer +0348, ERR inner +0350, CQ +001c; FIRST_TAG inner +01cc,
 * GROUP_TAG0 +01b8/+4, FH_SPARE_TAG1 +057c/+40. Never re-read a read-clear
 * status bank in this worker. These constants are facts, not a route lease.
 * No MMIO, DMA programming, map allocation, IRQ registration or dummy backend.
 */
LCK_MTX_DECLARE(camera_camsv_lock, &camera_group);
_Static_assert(sizeof(struct x9_camera_camsv_irq)==80,"CAMSV snapshot layout");

static int cam_camsv_retain(struct x9_camera_camsv_slot *slot,int e)
{
    slot->state=X9_CAMSV_RETAINED;slot->last_errno=e;
    return e;
}

int x9_camera_camsv_arm(struct x9_camera_camsv_slot *slot,
    const struct x9_camera_camsv_request *request,const struct x9_camera_camsv_dma *dma)
{
    if(!slot || !request || !dma)return EINVAL;
    const struct x9_camera_camsv_request r=*request;
    const struct x9_camera_camsv_dma d=*dma;
    if(r.frame.sensor!=X9_CAMERA_BACK && r.frame.sensor!=X9_CAMERA_FRONT)return EINVAL;
    const struct x9_camera_sensor *s=&sensors[r.frame.sensor-1];
    int e=cam_completed_layout(s,&r.frame);
    if(!e)e=cam_completed_controls(s,&r.frame);
    /* 16KiB mapping granule and captured CAM dma-range, not just 35-bit DMA
     * register width. A legal owner must still prove EVERY mapped byte. */
    if(!e && (r.reserved || r.tag>=8 || r.group>=4 || r.frame.time ||
        !r.frame.capture_id || !r.iova || (r.iova&0x3fffull) ||
        r.iova>=0x400000000ull || r.frame.bytes>0x400000000ull-r.iova ||
        d.version!=X9_CAMERA_CAMSV_VERSION || d.reserved || !d.context ||
        !d.lease || !d.check || !d.sync_cpu))e=EINVAL;
    if(e)return e; /* no callback/hardware/raw access on malformed inputs */
    lck_mtx_lock(&camera_camsv_lock);
    if(slot->state==X9_CAMSV_RETAINED)e=EIO;
    else if(slot->state!=X9_CAMSV_EMPTY && slot->state!=X9_CAMSV_RETIRED)e=EBUSY;
    else if(slot->state==X9_CAMSV_RETIRED) {
        const struct x9_camera_camsv_request *old=&slot->request;
        if(r.frame.generation<old->frame.generation)e=ESTALE;
        else if(r.frame.generation==old->frame.generation) {
            /* Hardware sequence can wrap through zero. Require an unambiguous
             * forward delta; never widen it to the 64-bit source capture_id. */
            uint32_t delta=r.inner_sequence-old->inner_sequence;
            if(r.frame.capture_id<=old->frame.capture_id || !delta || delta>=0x80000000u)e=ESTALE;
        }
    }
    if(!e) {
        slot->request=r;slot->dma=d;
        /* An uncertain lease check is terminal. It cannot be converted into
         * a free/reusable slot merely because no RAW copy was attempted. */
        e=d.check(d.context,d.lease,&slot->request);
        if(e)cam_camsv_retain(slot,e);
        else {slot->state=X9_CAMSV_ARMED;slot->last_errno=0;}
    }
    lck_mtx_unlock(&camera_camsv_lock);return e;
}

int x9_camera_camsv_finish(struct x9_camera_camsv_slot *slot,
    const struct x9_camera_camsv_irq *snapshot)
{
    if(!slot || !snapshot)return EINVAL;
    const struct x9_camera_camsv_irq irq=*snapshot;
    lck_mtx_lock(&camera_camsv_lock);int e=0;
    if(slot->state==X9_CAMSV_RETAINED)e=EIO;
    else if(slot->state!=X9_CAMSV_ARMED)e=ESTALE;
    else if(irq.version!=X9_CAMERA_CAMSV_VERSION || irq.reserved || irq.reserved1)e=EINVAL;
    else if(irq.generation!=slot->request.frame.generation ||
        irq.capture_id!=slot->request.frame.capture_id)e=ESTALE;
    if(e)goto out;

    /* Reject invalidated sessions BEFORE any lease/cache callback. The real
     * provider must pin this worker across stop; checking generation is not
     * itself a hardware drain or protection against unmapping by the owner. */
    lck_mtx_lock(&camera_lock);
    bool current=(st.flags&X9_CAMERA_STREAMING) && !(st.flags&X9_CAMERA_FAULT) &&
        st.generation==irq.generation && st.sensor==slot->request.frame.sensor;
    lck_mtx_unlock(&camera_lock);
    if(!current){e=ESTALE;goto out;}

    /* Treat any central error or any CQ status other than setting-done as
     * uncertain. This includes known code/VB/DMA/OTF errors and unknown bits.
     * Error has precedence even if PASS1_DONE is reported in the same IRQ. */
    if(irq.err_status || (irq.cq_status&~1u)) {
        e=cam_camsv_retain(slot,EIO);goto out;
    }
    uint32_t done=1u<<(16+slot->request.group);
    if(!(irq.done_status&done)) {
        e=(irq.done_status&0x000f0000u)?cam_camsv_retain(slot,EIO):EAGAIN;
        goto out;
    }
    /* Restrict the first implementation to one owned tag/group. A multi-tag
     * or multi-group IRQ needs its own slot binding, never this fallback. */
    uint32_t tag=1u<<slot->request.tag;
    if((irq.done_status&0x000f0000u)!=done || irq.first_tag!=tag || irq.last_tag!=tag) {
        e=cam_camsv_retain(slot,EIO);goto out;
    }
    for(unsigned group=0;group<4;group++) {
        if(irq.group_tags[group]!=(group==slot->request.group?tag:0)) {
            e=cam_camsv_retain(slot,EIO);goto out;
        }
    }
    /* Outer sequence may already describe the next CQ request. It is NOT
     * evidence that this owned RAW slot has finished; match inner exactly. */
    if(irq.inner_sequence!=slot->request.inner_sequence) {
        e=cam_camsv_retain(slot,ESTALE);goto out;
    }
    if(!irq.time || irq.time>mach_absolute_time()) {
        e=cam_camsv_retain(slot,EINVAL);goto out;
    }
    /* Retain by default before external callbacks: no rearm or replay can
     * cross a partial/uncertain cache transition. Never hold camera_lock
     * while invoking owner callbacks, allowing capture(off) to drain. */
    slot->state=X9_CAMSV_RETAINED;
    e=slot->dma.check(slot->dma.context,slot->dma.lease,&slot->request);
    if(!e)e=slot->dma.sync_cpu(slot->dma.context,slot->dma.lease,&slot->request);
    if(!e)e=slot->dma.check(slot->dma.context,slot->dma.lease,&slot->request);
    if(!e) {
        struct x9_camera_completed_frame f=slot->request.frame;f.time=irq.time;
        e=x9_camera_deliver(&f);
        /* Successful completion, CPU backpressure or concurrent stop all
         * retire this source. Never replay or hold the hardware slot for VM. */
        if(!e || e==EBUSY || e==ESTALE)slot->state=X9_CAMSV_RETIRED;
    }
    slot->last_errno=e;
out:
    if(slot->state!=X9_CAMSV_RETAINED)slot->last_errno=e;
    lck_mtx_unlock(&camera_camsv_lock);return e;
}
