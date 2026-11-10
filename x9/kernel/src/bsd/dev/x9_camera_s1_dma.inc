/* Concrete per-frame cache ownership, called ONLY by CAMSV arm/finish.
 * No callable 'done/drained' boolean, CPU-PA IOVA, external cache callback
 * or mapping retirement. The actual route/controller lease remains required.
 * Lock order: camera_camsv_lock -> camera_s1_lock. Never call an owner or
 * x9_camera_deliver while holding camera_s1_lock; CPU state pins the backing.
 */
_Static_assert(sizeof(struct x9_camera_camsv_request)==96,"CAMSV request layout");

static int cam_s1_dma_latch(struct x9_camera_s1 *m,int e)
{
    /* Keep RAW ownership faults separate from descriptor/storage faults.
     * The native owner must still be able to validate translation backing
     * for a future authorized abort/TLBI/drain; this grants no such operation. */
    if(!m->raw_errno) {
        m->raw_errno=e;
        printf("X9 CAMERA RAW: errno=%d state=%u generation=%llu capture_id=%llu "
            "inner=%x (retain, no rearm)\n",e,m->raw_state,
            m->raw_request.frame.generation,m->raw_request.frame.capture_id,
            m->raw_request.inner_sequence);
    }
    m->raw_state=CAM_RAW_RETAINED;
    return m->raw_errno;
}

static bool cam_s1_dma_match(const struct x9_camera_s1 *m,
    const struct x9_camera_camsv_slot *slot)
{
    return m->raw_slot==slot && m->raw_context==slot->dma.context &&
        m->raw_check==slot->dma.check &&
        m->raw_lease==slot->dma.lease &&
        !memcmp(&m->raw_request,&slot->request,sizeof(m->raw_request));
}

static void cam_s1_dma_retain(struct x9_camera_camsv_slot *slot,int e)
{
    struct x9_camera_s1 *m=slot->dma.backing;
    lck_mtx_lock(&camera_s1_lock);
    /* A failed attempt to bind some OTHER slot must not invalidate the
     * legitimate worker's still-device-owned backing. Retain that attempt,
     * but never claim ownership or perform cache operations on its behalf. */
    if(m && m->magic==CAM_S1_MAGIC && m->published && m->raw_slot==slot)
        cam_s1_dma_latch(m,e);
    lck_mtx_unlock(&camera_s1_lock);
}

static int cam_s1_dma_device(struct x9_camera_camsv_slot *slot)
{
    struct x9_camera_s1 *m=slot->dma.backing;
    const struct x9_camera_camsv_request *r=&slot->request;
    uint32_t delta;
    int e;
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_check_private(m);
    if(!e && m->raw_errno)e=m->raw_errno;
    if(!e && !m->published)e=ENXIO;
    if(!e && ((vm_offset_t)r->frame.raw!=m->raw || r->iova!=m->iova ||
        r->frame.bytes!=m->bytes || !r->frame.generation ||
        !r->frame.capture_id || r->frame.time || r->frame.sof_time))e=EINVAL;
    if(!e && m->raw_slot && (m->raw_slot!=slot ||
        m->raw_context!=slot->dma.context || m->raw_check!=slot->dma.check))e=EBUSY;
    if(!e && m->raw_state!=CAM_RAW_PRIVATE && m->raw_state!=CAM_RAW_IDLE)e=EBUSY;
    if(!e && m->raw_state==CAM_RAW_IDLE) {
        const struct x9_camera_camsv_request *old=&m->raw_request;
        delta=r->inner_sequence-old->inner_sequence;
        if(r->frame.generation<old->frame.generation ||
            r->frame.sensor!=old->frame.sensor || r->tag!=old->tag ||
            r->group!=old->group)e=ESTALE;
        else if(r->frame.generation==old->frame.generation &&
            (r->frame.capture_id<=old->frame.capture_id || !delta ||
             delta>=0x80000000u || slot->dma.lease!=m->raw_lease))e=ESTALE;
    }
    if(!e) {
        /* Bind before any cache op: partial publication cannot be replayed
         * or rebound. Export's one-time flush is NOT a per-frame rearm. */
        m->raw_slot=slot;m->raw_context=slot->dma.context;m->raw_check=slot->dma.check;
        m->raw_lease=slot->dma.lease;m->raw_request=*r;
        m->raw_state=CAM_RAW_RETAINED;
        for(unsigned i=0;i<m->pages && !e;i++)
            e=cam_cache_range(m->raw+(vm_offset_t)i*CAM_S1_PAGE,
                CAM_S1_PAGE,CAM_CACHE_DEVICE);
        if(!e)e=cam_s1_check_private(m);
        if(e)cam_s1_dma_latch(m,e);
        else m->raw_state=CAM_RAW_DEVICE;
    }
    lck_mtx_unlock(&camera_s1_lock);return e;
}

static int cam_s1_dma_cpu(struct x9_camera_camsv_slot *slot)
{
    struct x9_camera_s1 *m=slot->dma.backing;
    int e;
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_check_private(m);
    if(!e && m->raw_errno)e=m->raw_errno;
    if(!e && (!m->published || !cam_s1_dma_match(m,slot)))e=ESTALE;
    if(!e && m->raw_state!=CAM_RAW_DEVICE)e=EBUSY;
    if(!e) {
        /* Called after the sole PASS1_DONE/tag/group/sequence/error branch.
         * NC device output may coexist with speculatively filled WB lines.
         * IVAC drops those lines WITHOUT writing them over the new pixels. */
        m->raw_state=CAM_RAW_CPU;
        for(unsigned i=0;i<m->pages && !e;i++)
            e=cam_cache_range(m->raw+(vm_offset_t)i*CAM_S1_PAGE,
                CAM_S1_PAGE,CAM_CACHE_CPU);
        if(!e)e=cam_s1_check_private(m);
    }
    if(e && m && m->magic==CAM_S1_MAGIC && m->raw_slot==slot)
        cam_s1_dma_latch(m,e);
    lck_mtx_unlock(&camera_s1_lock);return e;
}

static int cam_s1_dma_retire(struct x9_camera_camsv_slot *slot)
{
    struct x9_camera_s1 *m=slot->dma.backing;
    int e;
    lck_mtx_lock(&camera_s1_lock);
    e=cam_s1_check_private(m);
    if(!e && m->raw_errno)e=m->raw_errno;
    if(!e && (!m->published || !cam_s1_dma_match(m,slot)))e=ESTALE;
    if(!e && m->raw_state!=CAM_RAW_CPU)e=EBUSY;
    if(e && m && m->magic==CAM_S1_MAGIC && m->raw_slot==slot)
        cam_s1_dma_latch(m,e);
    else if(!e) {
        __asm__ volatile("dsb sy":::"memory");
        m->raw_state=CAM_RAW_IDLE; /* no further RAW read in this worker */
    }
    lck_mtx_unlock(&camera_s1_lock);return e;
}
