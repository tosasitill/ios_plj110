/* ISP8s PHY3.0 OUTMUX0 masked CFG_DONE history, thread context only.
 * Vendor _get_outmux_irq_st uses OUTER +54 STATUSX, not +44 AP status or
 * +24/+28 debug. INT_WCLR_EN is +20 bit0; only CFG_DONE bit0 is ours.
 * camera_control_lock serializes every caller; AP/CCU IRQ_EN stay zero.
 * This is NOT a GIC source, DMA completion owner or firmware/SMMU lease.
 * The writable inner aperture exists upstream (apply_outmux_for_v2), but
 * WCLR's immediate/peer effect has not been measured on this board. Only
 * the policy bit is written there, never route/tag/DL; exact double actual
 * readback is required. No assumed mirror value is published as observed.
 */
enum { CAM_OUT_IRQ_CTRL=7, CAM_OUT_IRQ_DONE=1, CAM_OUT_IRQ_HISTORY=1,
    CAM_OUT_IRQ_COMMAND=2, CAM_OUT_IRQ_NO_BANK=2 };
static struct {
    bool captured, mode_ready, raw_disabled, event_seen, ack_clear_seen;
    unsigned mode_bank, ack_pending;
    uint32_t mode_next[2], mode_owned, mode_seen;
    int poison;
    struct x9_camera_outmux_irq_status status;
} cam_out_irq;
/* Never recycle a command number, including across sensor epochs. */
static uint64_t cam_out_irq_sequence;

static void cam_out_irq_reset(void)
{
    memset(&cam_out_irq,0,sizeof(cam_out_irq));
    cam_out_irq.mode_bank=CAM_OUT_IRQ_NO_BANK;
}
static void cam_out_irq_status(void)
{
    cam_out_irq.status.mode_owned=cam_out_irq.mode_owned;
    cam_out_irq.status.mode_pending=cam_out_irq.mode_bank<2?1u<<cam_out_irq.mode_bank:0;
    cam_out_irq.status.ack_pending=cam_out_irq.ack_pending;
    cam_out_irq.status.event_seen=cam_out_irq.event_seen;
    if(cam_out_irq.captured && cam_out.error && !cam_out_irq.status.last_errno)
        cam_out_irq.status.last_errno=cam_out.error;
    if(cam_out.held && (cam_out_irq.poison || cam_out.error))
        cam_out_irq.status.flags|=X9_CAM_OUT_IRQ_RETAINED;
    else cam_out_irq.status.flags&=~X9_CAM_OUT_IRQ_RETAINED;
    cam_out.status.irq=cam_out_irq.status;
}
static int cam_out_irq_error(int e)
{
    if(e && e!=EAGAIN && !cam_out_irq.status.last_errno)
        cam_out_irq.status.last_errno=e;
    return e;
}
static int cam_out_irq_poison(int e)
{
    if(cam_out.held && !cam_out_irq.poison)cam_out_irq.poison=e;
    return cam_out_irq_error(e);
}
/* Called from cam_out_guard; MUST NOT call that guard or read IRQ history. */
static int cam_out_irq_control_guard(void)
{
    if(cam_out_irq.poison)return cam_out_irq.poison;
    if(!cam_out_irq.captured)return 0;
    if(cam_out_irq.status.sensor_epoch!=cam_out.epoch)return ESTALE;
    return 0;
}
static bool cam_out_irq_control_allowed(unsigned bank,unsigned reg,uint32_t v,bool prior)
{
    if(!cam_out_irq.captured || cam_out_irq.mode_bank>=2 || reg!=CAM_OUT_IRQ_CTRL)return prior;
    if(cam_out_irq.poison)return false;
    if(v==cam_out_irq.mode_next[bank]) {
        if(v!=cam_out.expected[bank][reg])cam_out_irq.mode_seen|=1u<<bank;
        return true;
    }
    /* Once ANY actual target observation exists, even in ordinary guard,
     * a return to prior is a conflict, not a delayed write to wait out. */
    return prior && !(cam_out_irq.mode_seen&(1u<<bank));
}
static bool cam_out_irq_inner_target_required(void)
{
    return cam_out_irq.captured && cam_out_irq.ack_pending==CAM_OUT_IRQ_COMMAND;
}
/* Only the explicitly issued bank MUST reach target. The same policy bit
 * in its peer may remain prior or take target; readback, not projection,
 * records a peer effect. A late change after confirmation is a conflict. */
static int cam_out_irq_mode_observe(void)
{
    uint32_t first[2],seen[2];
    unsigned pending=cam_out_irq.mode_bank;
    int e;
    if(pending>=2)return 0;
    e=cam_out_guard();if(e)return e;
    for(unsigned b=0;b<2;b++) {
        first[b]=cam_rr(cam_out.out[b],0x20);
        if(!cam_out_irq_control_allowed(b,CAM_OUT_IRQ_CTRL,first[b],
           first[b]==cam_out.expected[b][CAM_OUT_IRQ_CTRL]))return cam_out_irq_poison(ESTALE);
    }
    delay(1);
    e=cam_out_guard();if(e)return e;
    for(unsigned b=0;b<2;b++) {
        seen[b]=cam_rr(cam_out.out[b],0x20);
        if(!cam_out_irq_control_allowed(b,CAM_OUT_IRQ_CTRL,seen[b],
           seen[b]==cam_out.expected[b][CAM_OUT_IRQ_CTRL]))return cam_out_irq_poison(ESTALE);
        if(seen[b]!=first[b])return EAGAIN;
    }
    /* Prior does not retire a possibly posted policy write. */
    if(seen[pending]!=cam_out_irq.mode_next[pending])return EAGAIN;
    for(unsigned b=0;b<2;b++) {
        if(seen[b]!=cam_out.before[b][CAM_OUT_IRQ_CTRL])cam_out_irq.mode_owned|=1u<<b;
        cam_out.expected[b][CAM_OUT_IRQ_CTRL]=seen[b];
    }
    cam_out_irq.mode_bank=CAM_OUT_IRQ_NO_BANK;
    cam_out_publish_expected();
    return cam_out_guard();
}
static int cam_out_irq_mode_write(unsigned bank,uint32_t bit)
{
    uint32_t next;
    int e=cam_out_guard();if(e)return e;
    if(cam_out_irq.mode_bank<2 || cam_out_irq.ack_pending || cam_out.commit_pending)return EBUSY;
    next=(cam_out.expected[bank][CAM_OUT_IRQ_CTRL]&~1u)|(bit&1);
    if(next==cam_out.expected[bank][CAM_OUT_IRQ_CTRL])return 0;
    /* Journal both possible policy effects BEFORE the ordinary CSR write.
     * No route/tag/command is copied into inner to manufacture a target. */
    for(unsigned b=0;b<2;b++)
        cam_out_irq.mode_next[b]=(cam_out.expected[b][CAM_OUT_IRQ_CTRL]&~1u)|(bit&1);
    cam_out_irq.mode_bank=bank;cam_out_irq.mode_seen=0;cam_out_irq.mode_owned|=1u<<bank;
    cam_rw(cam_out.out[bank],0x20,next);
    return cam_out_irq_mode_observe();
}
static int cam_out_irq_require_wclr(void)
{
    int e=cam_out_guard();if(e)return e;
    if(!cam_out_irq.captured || !cam_out_irq.mode_ready || cam_out_irq.raw_disabled ||
       cam_out_irq.mode_bank<2)return EBUSY;
    for(unsigned b=0;b<2;b++)
        if(!(cam_rr(cam_out.out[b],0x20)&1))return cam_out_irq_poison(ESTALE);
    return 0;
}
static int cam_out_irq_sample(uint32_t *raw)
{
    int e=cam_out_irq_require_wclr();if(e)return e;
    *raw=cam_rr(cam_out.out[0],0x54); /* sole raw sampler, masked WCLR only */
    if(!cam_out_irq.status.sampled)cam_out_irq.status.raw_before=*raw;
    if(cam_out_irq.status.sampled!=UINT32_MAX)cam_out_irq.status.sampled++;
    cam_out_irq.status.raw_after=*raw;
    /* Preserve every non-CFG_DONE bit, including known frame/error events.
     * They have no owned producer here. Never acknowledge a mixed word. */
    if(*raw&~CAM_OUT_IRQ_DONE)return cam_out_irq_poison(EIO);
    return cam_out_irq_require_wclr();
}
static int cam_out_irq_clear_baseline(void)
{
    uint32_t raw;
    int e;
    for(unsigned s=0;s<2;s++) {
        e=cam_out_irq_sample(&raw);if(e)return e;
        if(raw)return cam_out_irq_poison(ESTALE); /* no outstanding command */
        if(!s)delay(1);
    }
    cam_out_irq.status.flags|=X9_CAM_OUT_IRQ_BASELINE_CLEAR|X9_CAM_OUT_IRQ_HISTORY_RETIRED;
    return 0;
}
static int cam_out_irq_ack_observe(void)
{
    uint32_t raw;
    int e;
    if(!cam_out_irq.ack_pending)return EINVAL;
    for(unsigned s=0;s<2;s++) {
        e=cam_out_irq_sample(&raw);if(e)return e;
        if(raw) {
            if(cam_out_irq.ack_clear_seen)return cam_out_irq_poison(ESTALE);
            return EAGAIN; /* still posted; NEVER write W1C again */
        }
        cam_out_irq.ack_clear_seen=true;
        if(!s)delay(1);
    }
    return 0;
}
static int cam_out_irq_ack_begin(unsigned kind)
{
    uint32_t raw;
    int e;
    if(cam_out_irq.ack_pending)return EBUSY;
    e=cam_out_irq_sample(&raw);if(e)return e;
    if(raw!=CAM_OUT_IRQ_DONE)return cam_out_irq_poison(ESTALE);
    cam_out_irq.ack_pending=kind;cam_out_irq.ack_clear_seen=false; /* BEFORE W1C */
    cam_out_irq.status.flags&=~X9_CAM_OUT_IRQ_HISTORY_RETIRED;
    cam_rw(cam_out.out[0],0x54,CAM_OUT_IRQ_DONE); /* never echo raw/mixed status */
    return cam_out_irq_ack_observe();
}
static int cam_out_irq_wait(int (*step)(void))
{
    uint64_t span,deadline;
    int e;
    nanoseconds_to_absolutetime(10000000,&span);deadline=mach_absolute_time()+span;
    for(unsigned n=0;n<10000;n++) {
        if(mach_absolute_time()>=deadline)return cam_out_irq_error(ETIMEDOUT);
        e=step();
        if(e!=EAGAIN)return cam_out_irq_error(e);
        delay(1);
    }
    return cam_out_irq_error(ETIMEDOUT);
}
static int cam_out_irq_history_finish(void)
{
    int e;
    if(cam_out_irq.ack_pending!=CAM_OUT_IRQ_HISTORY)return EINVAL;
    e=cam_out_irq_ack_observe();if(e)return e;
    cam_out_irq.ack_pending=0;cam_out_irq.ack_clear_seen=false;
    return cam_out_irq_clear_baseline();
}
static int cam_out_irq_mode_apply(unsigned bank,uint32_t bit)
{
    int e=cam_out_irq_mode_write(bank,bit);
    return e==EAGAIN?cam_out_irq_wait(cam_out_irq_mode_observe):cam_out_irq_error(e);
}
static int cam_out_irq_open_locked(void)
{
    uint32_t raw,again;
    int e;
    cam_out_irq.captured=true;cam_out_irq.status.sensor_epoch=cam_out.epoch;
    cam_out_irq.status.flags=X9_CAM_OUT_IRQ_ATTEMPTED;
    /* Policy-only inner first, outer last. Both actual bits must be WCLR
     * before the first history read. Already-WCLR is borrowed, not owned. */
    e=cam_out_irq_mode_apply(1,1);
    if(!e)e=cam_out_irq_mode_apply(0,1);
    if(e)return e;
    cam_out_irq.mode_ready=true;cam_out_irq.status.flags|=X9_CAM_OUT_IRQ_MODE_READY;
    e=cam_out_irq_sample(&raw);if(e)return cam_out_irq_error(e);
    delay(1);e=cam_out_irq_sample(&again);if(e)return cam_out_irq_error(e);
    if(raw!=again)return cam_out_irq_poison(ESTALE);
    if(!raw)return cam_out_irq_clear_baseline();
    /* Stable, quiet CFG_DONE-only is stale history, NOT our command. The
     * full matched banks/idle controls have already been checked by guard. */
    e=cam_out_irq_ack_begin(CAM_OUT_IRQ_HISTORY);
    if(e && e!=EAGAIN)return cam_out_irq_error(e);
    return cam_out_irq_wait(cam_out_irq_history_finish);
}
static int cam_out_irq_precommit(void)
{
    int e;
    if(cam_out_irq.ack_pending || cam_out_irq.mode_bank<2 || cam_out.commit_pending)return EBUSY;
    e=cam_out_irq_clear_baseline();if(e)return cam_out_irq_error(e);
    if(cam_out_irq_sequence==UINT64_MAX)return cam_out_irq_poison(EOVERFLOW);
    cam_out_irq.status.command=++cam_out_irq_sequence;
    cam_out_irq.event_seen=false;cam_out_irq.ack_clear_seen=false;
    cam_out_irq.status.flags&=~(X9_CAM_OUT_IRQ_BASELINE_CLEAR|X9_CAM_OUT_IRQ_HISTORY_RETIRED);
    return 0; /* ledger bound to sensor epoch BEFORE caller posts +1c */
}
static int cam_out_irq_finish_commit(void)
{
    uint32_t raw,inner[CAM_OUT_REGS];
    int e;
    if(!cam_out.commit_pending || !cam_out_irq.status.command ||
       cam_out_irq.status.command==cam_out_irq.status.completed)return EINVAL;
    if(cam_out_irq.ack_pending && cam_out_irq.ack_pending!=CAM_OUT_IRQ_COMMAND)
        return cam_out_irq_poison(ESTALE);
    if(!cam_out_irq.ack_pending) {
        e=cam_out_irq_sample(&raw);if(e)return cam_out_irq_error(e);
        if(!raw)return cam_out_irq.event_seen?cam_out_irq_poison(ESTALE):EAGAIN;
        cam_out_irq.event_seen=true;
        e=cam_out_target_observe(inner);if(e)return cam_out_irq_error(e);
        e=cam_out_irq_ack_begin(CAM_OUT_IRQ_COMMAND);
        if(e)return cam_out_irq_error(e);
    } else {
        e=cam_out_irq_ack_observe();if(e)return cam_out_irq_error(e);
    }
    /* Target was exact BEFORE W1C; a regression afterwards is not another
     * innocent transfer-in-progress. Keep the pending ack and dependencies. */
    e=cam_out_target_observe(inner);if(e)return cam_out_irq_poison(e==EAGAIN?ESTALE:e);
    e=cam_out_irq_clear_baseline();if(e)return cam_out_irq_error(e);
    memcpy(cam_out.expected[1],inner,sizeof(inner));
    cam_out_irq.status.completed=cam_out_irq.status.command;
    cam_out_irq.ack_pending=0;cam_out_irq.ack_clear_seen=false;
    cam_out.commit_pending=false;cam_out.consumed=true;
    cam_out.status.flags|=X9_CAM_OUT_CONSUMED;
    cam_out_publish_expected();
    return cam_out_guard();
}
static int cam_out_irq_retirement_guard(void)
{
    int e;
    if(cam_out_irq.poison)return cam_out_irq.poison;
    if(!cam_out_irq.captured)return 0;
    if(cam_out_irq.mode_bank<2) {
        /* Only actual late TARGET can retire the issued policy write. No
         * second SET, replay, prior fallback or completing unissued setup. */
        e=cam_out_irq_wait(cam_out_irq_mode_observe);if(e)return e;
    }
    if(!cam_out_irq.mode_ready) {
        if(cam_out_irq.status.sampled || cam_out_irq.ack_pending || cam_out.commit_pending)
            return cam_out_irq_poison(ESTALE);
        return cam_out_guard(); /* partial setup, no IRQ read/trigger issued */
    }
    if(cam_out_irq.ack_pending==CAM_OUT_IRQ_HISTORY) {
        e=cam_out_irq_wait(cam_out_irq_history_finish);if(e)return e;
    }
    if(cam_out.commit_pending) {
        e=cam_out_irq_wait(cam_out_irq_finish_commit);if(e)return e;
    }
    if(cam_out_irq.ack_pending)return cam_out_irq_poison(ESTALE);
    return cam_out_irq_clear_baseline();
}
static int cam_out_irq_close_locked(void)
{
    int e;
    if(!cam_out_irq.captured)return 0;
    e=cam_out_irq_retirement_guard();if(e)return e;
    /* No history reads after a restore may have switched either aperture
     * back to read-clear. Last proven clear precedes the first mode write;
     * no further CFG_DONE is posted, and this is not a frame/GIC drain. */
    cam_out_irq.raw_disabled=true;
    for(unsigned b=0;b<2;b++)if(cam_out_irq.mode_owned&(1u<<b)) {
        e=cam_out_irq_mode_apply(b,cam_out.before[b][CAM_OUT_IRQ_CTRL]&1);
        if(e)return e;
    }
    e=cam_out_guard();if(e)return e;
    for(unsigned b=0;b<2;b++)
        if(cam_rr(cam_out.out[b],0x20)!=cam_out.before[b][CAM_OUT_IRQ_CTRL])
            return cam_out_irq_poison(ESTALE);
    cam_out_irq.mode_ready=false;
    cam_out_irq.status.flags|=X9_CAM_OUT_IRQ_MODE_RESTORED;
    return 0;
}
static void cam_out_irq_release(void)
{
    /* Called ONLY after OUTMUX route/history/policy and physical CG are
     * actually retired. Preserve cached evidence; never read powered-off IO. */
    cam_out_irq.captured=false;
}
