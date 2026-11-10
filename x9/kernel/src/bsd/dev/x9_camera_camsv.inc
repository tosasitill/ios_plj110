/* MT6993/ISP8s single-tag owned-request -> RAW10 completion adapter.
 * Register semantics: current OPPO mtk_cam-sv.c::mtk_irq_camsv_hybrid and
 * mtk_cam-sv-regs.h. Status values are captured ONCE by the sole IRQ owner:
 * DONE outer +0348, ERR inner +0350, CQ +001c; FIRST_TAG inner +01cc,
 * GROUP_TAG0 +01b8/+4, FH_SPARE_TAG1 +057c/+40. Never re-read a read-clear
 * status bank in this worker. These constants are facts, not a route lease.
 * Concrete S1 cache ownership below; no MMIO, DMA programming, map allocation,
 * IRQ registration or dummy backend. The native controller lease is mandatory.
 */
#include "x9_camera_s1_dma.inc"
_Static_assert(sizeof(struct x9_camera_camsv_irq)==80,"CAMSV snapshot layout");

static int cam_camsv_retain(struct x9_camera_camsv_slot *slot,int e)
{
    if(slot->state==X9_CAMSV_RETAINED && slot->last_errno)e=slot->last_errno;
    cam_s1_dma_retain(slot,e);
    slot->state=X9_CAMSV_RETAINED;slot->last_errno=e;
    return e;
}
static int cam_irq_retiring_frame_guard(const struct x9_camera_camsv_slot *,
    const struct x9_camera_camsv_irq *);

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
        !r.frame.generation || !r.frame.capture_id || !r.iova || (r.iova&0x3fffull) ||
        r.iova>=0x400000000ull || r.frame.bytes>0x400000000ull-r.iova ||
        d.version!=X9_CAMERA_CAMSV_VERSION || d.reserved || !d.context ||
        !d.lease || !d.check || !d.backing))e=EINVAL;
    if(e)return e; /* no callback/hardware/raw access on malformed inputs */
    lck_mtx_lock(&camera_camsv_lock);
    if(slot->state==X9_CAMSV_RETAINED)e=slot->last_errno?slot->last_errno:EIO;
    else if(slot->state!=X9_CAMSV_EMPTY && slot->state!=X9_CAMSV_RETIRED)e=EBUSY;
    else if(slot->state==X9_CAMSV_RETIRED) {
        const struct x9_camera_camsv_request *old=&slot->request;
        if(d.backing!=slot->dma.backing || d.context!=slot->dma.context ||
            d.check!=slot->dma.check)e=EINVAL;
        else if(r.frame.generation<old->frame.generation)e=ESTALE;
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
        if(!e)e=cam_s1_dma_device(slot);
        if(!e)e=d.check(d.context,d.lease,&slot->request);
        if(e)cam_camsv_retain(slot,e);
        else {slot->state=X9_CAMSV_ARMED;slot->last_errno=0;}
    }
    lck_mtx_unlock(&camera_camsv_lock);return e;
}

static int cam_camsv_finish(struct x9_camera_camsv_slot *slot,
    const struct x9_camera_camsv_irq *snapshot,bool native_worker)
{
    if(!slot || !snapshot)return EINVAL;
    const struct x9_camera_camsv_irq irq=*snapshot;
    bool drop=false;
    lck_mtx_lock(&camera_camsv_lock);int e=0;
    if(slot->state==X9_CAMSV_RETAINED)e=slot->last_errno?slot->last_errno:EIO;
    else if(slot->state!=X9_CAMSV_ARMED)e=ESTALE;
    else if(irq.version!=X9_CAMERA_CAMSV_VERSION || irq.reserved || irq.reserved1)e=EINVAL;
    else if(irq.generation!=slot->request.frame.generation ||
        irq.capture_id!=slot->request.frame.capture_id)e=ESTALE;
    if(e)goto out;

    /* Standalone finish rejects invalidated sessions before any lease/cache
     * callback. The native worker alone may qualify the actual core STOP's
     * old owned DONE below, with no delivery. Generation is not a DMA grant;
     * the real provider must still pin this worker and its map across stop. */
    lck_mtx_lock(&camera_lock);
    bool current=(st.flags&X9_CAMERA_STREAMING) && !(st.flags&X9_CAMERA_FAULT) &&
        st.generation==irq.generation && st.sensor==slot->request.frame.sensor;
    drop=native_worker && cam_core_retiring_locked(irq.generation,slot->request.frame.sensor);
    lck_mtx_unlock(&camera_lock);
    if(!current && !drop){e=ESTALE;goto out;}

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
    /* A generation mismatch alone never grants cache/DMA retirement. Only
     * the real native worker's original SOF/DONE/FBC receipt can use this
     * private canceled-frame route. Standalone finish stays strictly current. */
    if(drop) {
        e=cam_irq_retiring_frame_guard(slot,&irq);
        if(e){e=cam_camsv_retain(slot,e);goto out;}
    }
    /* Retain by default before external callbacks: no rearm or replay can
     * cross a partial/uncertain cache transition. Never hold camera_lock
     * while invoking owner callbacks, allowing capture(off) to drain. */
    slot->state=X9_CAMSV_RETAINED;
    e=slot->dma.check(slot->dma.context,slot->dma.lease,&slot->request);
    if(!e)e=cam_s1_dma_cpu(slot);
    if(!e)e=slot->dma.check(slot->dma.context,slot->dma.lease,&slot->request);
    if(!e) {
        struct x9_camera_completed_frame f=slot->request.frame;f.time=irq.time;
        e=drop?ESTALE:x9_camera_deliver(&f); /* STOP never publishes canceled pixels */
        /* Successful completion, CPU backpressure or concurrent stop all
         * retire this source. Never replay or hold the hardware slot for VM. */
        if(!e || e==EBUSY || e==ESTALE) {
            int retire=slot->dma.check(slot->dma.context,slot->dma.lease,&slot->request);
            if(!retire)retire=cam_s1_dma_retire(slot);
            if(retire)e=cam_camsv_retain(slot,retire);
            else slot->state=X9_CAMSV_RETIRED;
        } else cam_camsv_retain(slot,e);
    }
    if(e && slot->state==X9_CAMSV_RETAINED)cam_camsv_retain(slot,e);
    slot->last_errno=e;
out:
    if(drop)printf("X9 CAMERA STOP DONE: generation=%llu capture_id=%llu seq=%x "
        "core_cancel=%u state=%u errno=%d (no delivery; S1/lease retained, NOT AXI/map drain)\n",
        irq.generation,irq.capture_id,irq.inner_sequence,drop,slot->state,e);
    if(slot->state!=X9_CAMSV_RETAINED)slot->last_errno=e;
    lck_mtx_unlock(&camera_camsv_lock);return e;
}
int x9_camera_camsv_finish(struct x9_camera_camsv_slot *slot,
    const struct x9_camera_camsv_irq *snapshot)
{
    return cam_camsv_finish(slot,snapshot,false);
}
