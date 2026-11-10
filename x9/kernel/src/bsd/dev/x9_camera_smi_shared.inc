/* Scoped cold COMM33/L19 misc configuration, camera_control_lock held.
 * Captured COMM33 also supplies LARB27 (CAM_MAIN) and LARB44 (RAW_C),
 * not just L19. A newly acquired APB clock alone never qualifies it.
 * hwccf.h v1 exposes ten XPU EN banks: XPU0/AP at 20000, XPU1..4 at
 * 30000..60000, XPU5..9 at 70000..74000. The captured MM_HWV resource
 * ends at 74900; map only the additional RO voter window, no history.
 * Never rewrite a foreign vote, shared reset, common0/1, SID or routing.
 */
enum { CAM_SMI_SHARED_GROUPS=4, CAM_SMI_SHARED_XPUS=10,
    CAM_SMI_SHARED_WORDS=5, CAM_SMI_SHARED_COMMON_FIRST=0,
    CAM_SMI_SHARED_LARB_FIRST=3 };
static const unsigned cam_smi_shared_cg[CAM_SMI_SHARED_GROUPS]={35,34,30,22};
/* L44's two DT SMI clocks are RAW_C larbx bit0 AND cam bit1. */
static const uint32_t cam_smi_shared_mask[CAM_SMI_SHARED_GROUPS]={1u<<15,4,3,1};
static const unsigned cam_smi_shared_offsets[CAM_SMI_SHARED_WORDS]={0x100,0x244,0x444,0x24,0x40};
static const uint32_t cam_smi_shared_targets[CAM_SMI_SHARED_WORDS]={0xa,0x4146,1,0x370456,1};
static struct {
    uint64_t epoch;
    vm_offset_t xpu,common;
    uint32_t votes[CAM_SMI_SHARED_GROUPS][CAM_SMI_SHARED_XPUS];
    uint32_t global[CAM_SMI_SHARED_GROUPS],gate[3],rawc_ap,rawc_global,rawc_ack;
    uint32_t before[CAM_SMI_SHARED_WORDS],expected[CAM_SMI_SHARED_WORDS];
    uint32_t sampled,owned,pending_word;
    bool captured,held,common_cold,larb_cold,pending,restoring,close_attempted;
} cam_smi_shared;
static unsigned cam_smi_shared_xpu_base(unsigned n)
{ return n<5?0x20000+n*0x10000:0x70000+(n-5)*0x1000; }
static uint32_t cam_smi_shared_vote(unsigned n,unsigned offset)
{
    if(!n)return cam_rr(cam_mm.receive.hwv,0x20000+offset);
    return cam_rr(cam_smi_shared.xpu,cam_smi_shared_xpu_base(n)-0x30000+offset);
}
static void cam_smi_shared_status(void)
{
    struct x9_camera_smi_shared_status *s=&cam_smi.status.shared;
    s->owned=cam_smi_shared.owned;s->pending=cam_smi_shared.pending;
    s->pending_word=cam_smi_shared.pending_word;s->sampled=cam_smi_shared.sampled;
    memcpy(s->before,cam_smi_shared.before,sizeof(s->before));
    memcpy(s->target,cam_smi_shared.expected,sizeof(s->target));
}
static int cam_smi_shared_fault(int e)
{
    struct x9_camera_smi_shared_status *s=&cam_smi.status.shared;
    if(!s->last_errno)s->last_errno=e;
    if(cam_smi_shared.captured)s->flags|=X9_CAM_SMI_SHARED_RETAINED;
    cam_smi_shared_status();return e;
}
/* Nonrecursive and safe before either consumer clock is on. Exact foreign
 * EN snapshots prevent a hidden second owner masked by our aggregate vote.
 * The two AP bits acquired by SMI are compared to that clock journal's
 * expected targets; no adoption of a late foreign vote. */
static int cam_smi_shared_voter_guard(void)
{
    int e;
    if(!cam_smi_shared.captured)return 0;
    if(!cam_smi.held || cam_smi_shared.epoch!=cam_smi.epoch)return ESTALE;
    e=cam_smi_parent_guard();if(e)return e;
    for(unsigned g=0;g<CAM_SMI_SHARED_GROUPS;g++) {
        unsigned cg=cam_smi_shared_cg[g];uint32_t mask=cam_smi_shared_mask[g];
        if(cam_rr(cam_mm.receive.hwv,0x11800+cg*4)&mask)return EBUSY;
        for(unsigned n=0;n<CAM_SMI_SHARED_XPUS;n++) {
            uint32_t expected=cam_smi_shared.votes[g][n];
            if(!n && g<2 && (cam_smi.seen&(1u<<g)))
                expected=cam_smi.gate[g].ap_expected&mask;
            if((cam_smi_shared_vote(n,cg*12+8)&mask)!=expected)return ESTALE;
        }
        if(g>=2 && (cam_rr(cam_mm.receive.hwv,0x12200+cg*4)&mask)!=cam_smi_shared.global[g])
            return ESTALE;
    }
    /* LARB44's sibling RAW_C domain: no speculative physical-CG access
     * when PM_ACK is off, and no domain enable to inspect it. */
    if((cam_rr(cam_mm.receive.hwv,0x20708)&(1u<<11))!=cam_smi_shared.rawc_ap ||
        (cam_rr(cam_mm.receive.hwv,0x11318)&(1u<<11))!=cam_smi_shared.rawc_global ||
        (cam_rr(cam_mm.receive.hwv,0x12900)&(1u<<11))!=cam_smi_shared.rawc_ack)return ESTALE;
    if(cam_rr(cam_mm.receive.hwv,0x1131c)&(1u<<11))return EBUSY;
    for(unsigned n=1;n<CAM_SMI_SHARED_XPUS;n++)
        if((cam_smi_shared_vote(n,0x708)&(1u<<11))!=cam_smi.status.shared.rawc_votes[n])
            return ESTALE;
    if((cam_rr(cam_mm.cam_cg,0)&1)!=cam_smi_shared.gate[1])return ESTALE;
    return 0;
}
static int cam_smi_shared_capture_locked(void)
{
    static vm_offset_t xpu;
    uint32_t votes[CAM_SMI_SHARED_GROUPS][CAM_SMI_SHARED_XPUS];
    struct x9_camera_smi_shared_status *s=&cam_smi.status.shared;int e;
    if(cam_smi_shared.captured)return EALREADY;
    e=cam_smi_parent_guard();if(e)return e;
    /* Separate from the existing AP-only 22000-byte power mapping. */
    if(!xpu)xpu=ml_io_map(0x31c30000,0x45000);
    if(!xpu)return ENOMEM;
    memset(&cam_smi_shared,0,sizeof(cam_smi_shared));cam_smi_shared.xpu=xpu;
    cam_smi_shared.epoch=cam_smi.epoch;s->flags=X9_CAM_SMI_SHARED_ATTEMPTED;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_smi_parent_guard();if(e)return cam_smi_shared_fault(e);
        for(unsigned g=0;g<CAM_SMI_SHARED_GROUPS;g++) {
            unsigned cg=cam_smi_shared_cg[g];uint32_t mask=cam_smi_shared_mask[g];
            if(cam_rr(cam_mm.receive.hwv,0x11800+cg*4)&mask)return cam_smi_shared_fault(EBUSY);
            for(unsigned n=0;n<CAM_SMI_SHARED_XPUS;n++)
                votes[g][n]=cam_smi_shared_vote(n,cg*12+8)&mask;
        }
        if(!sample) {
            memcpy(cam_smi_shared.votes,votes,sizeof(votes));
            for(unsigned g=0;g<CAM_SMI_SHARED_GROUPS;g++)
                cam_smi_shared.global[g]=cam_rr(cam_mm.receive.hwv,0x12200+cam_smi_shared_cg[g]*4)&cam_smi_shared_mask[g];
            cam_smi_shared.gate[0]=cam_rr(cam_smi.gate[0].cg,0)&(1u<<15);
            cam_smi_shared.gate[1]=cam_rr(cam_mm.cam_cg,0)&1;
            cam_smi_shared.gate[2]=cam_rr(cam_smi.gate[1].cg,0)&4;
            cam_smi_shared.rawc_ap=cam_rr(cam_mm.receive.hwv,0x20708)&(1u<<11);
            cam_smi_shared.rawc_global=cam_rr(cam_mm.receive.hwv,0x11318)&(1u<<11);
            cam_smi_shared.rawc_ack=cam_rr(cam_mm.receive.hwv,0x12900)&(1u<<11);
            for(unsigned n=0;n<CAM_SMI_SHARED_XPUS;n++)
                s->rawc_votes[n]=cam_smi_shared_vote(n,0x708)&(1u<<11);
            cam_smi_shared.captured=true;
        } else if(memcmp(votes,cam_smi_shared.votes,sizeof(votes)))return cam_smi_shared_fault(ESTALE);
        e=cam_smi_shared_voter_guard();if(e)return cam_smi_shared_fault(e);
        for(unsigned g=0;g<2;g++)
            if((cam_rr(cam_mm.receive.hwv,0x12200+cam_smi_shared_cg[g]*4)&cam_smi_shared_mask[g])!=cam_smi_shared.global[g])
                return cam_smi_shared_fault(ESTALE);
        if((cam_rr(cam_smi.gate[0].cg,0)&(1u<<15))!=cam_smi_shared.gate[0] ||
            (cam_rr(cam_smi.gate[1].cg,0)&4)!=cam_smi_shared.gate[2])return cam_smi_shared_fault(ESTALE);
        delay(10);
    }
    memcpy(s->votes,cam_smi_shared.votes,sizeof(s->votes));
    memcpy(s->global,cam_smi_shared.global,sizeof(s->global));
    s->supplier_gate=cam_smi_shared.gate[0];s->larb27_gate=cam_smi_shared.gate[1];
    s->larb19_gate=cam_smi_shared.gate[2];
    cam_smi_shared.larb_cold=!cam_smi_shared.global[1] && cam_smi_shared.gate[2];
    cam_smi_shared.common_cold=!cam_smi_shared.global[0] &&
        !cam_smi_shared.global[2] && !cam_smi_shared.global[3] &&
        cam_smi_shared.gate[0] && cam_smi_shared.gate[1] &&
        !cam_smi_shared.rawc_global && !cam_smi_shared.rawc_ack;
    for(unsigned n=0;n<CAM_SMI_SHARED_XPUS;n++) {
        if(cam_smi_shared.votes[1][n])cam_smi_shared.larb_cold=false;
        for(unsigned g=0;g<CAM_SMI_SHARED_GROUPS;g++)
            if(cam_smi_shared.votes[g][n])cam_smi_shared.common_cold=false;
        if(s->rawc_votes[n])cam_smi_shared.common_cold=false;
    }
    if(!cam_smi_shared.larb_cold)cam_smi_shared.common_cold=false;
    if(cam_smi_shared.larb_cold)s->flags|=X9_CAM_SMI_SHARED_LARB_COLD;
    if(cam_smi_shared.common_cold)s->flags|=X9_CAM_SMI_SHARED_COMMON_COLD;
    s->flags|=X9_CAM_SMI_SHARED_VOTERS_CAPTURED;return 0;
}
static vm_offset_t cam_smi_shared_base(unsigned word)
{ return word<3?cam_smi_shared.common:cam_smi.larb[0]; }
static int cam_smi_shared_control_guard(bool qualify_pending)
{
    int e=cam_smi_shared_voter_guard();if(e)return e;
    if(!cam_smi_shared.held)return 0;
    e=cam_smi_clock_only_guard();if(e)return e;
    for(unsigned word=0;word<CAM_SMI_SHARED_WORDS;word++)if(cam_smi_shared.sampled&(1u<<word)) {
        uint32_t value=cam_rr(cam_smi_shared_base(word),cam_smi_shared_offsets[word]);
        cam_smi.status.shared.after[word]=value;
        if(value!=cam_smi_shared.expected[word])return ESTALE;
    }
    return cam_smi_shared.pending&&!qualify_pending?EBUSY:0;
}
static int cam_smi_shared_write(unsigned word,uint32_t value)
{
    uint32_t old;int e;
    if(word>=CAM_SMI_SHARED_WORDS || value!=cam_smi_shared_targets[word])return EINVAL;
    e=cam_rx_session_guard();if(e)return e;
    e=cam_smi_larb_idle();if(e)return e;
    e=cam_smi_shared_control_guard(false);if(e)return e;
    old=cam_smi_shared.expected[word];if(old==value)return 0;
    /* The pre-vote cold predicates are essential. Existing shared owners
     * may use a matching profile, but never have their misc reconfigured. */
    if(word<3?!cam_smi_shared.common_cold:!cam_smi_shared.larb_cold)return EOPNOTSUPP;
    cam_smi_shared.expected[word]=value;cam_smi_shared.owned|=1u<<word;
    cam_smi_shared.pending_word=word;cam_smi_shared.pending=true;cam_smi_shared_status();
    cam_rw(cam_smi_shared_base(word),cam_smi_shared_offsets[word],value);
    e=cam_wait_bits(cam_smi_shared_base(word),cam_smi_shared_offsets[word],UINT32_MAX,value,200);
    if(e)return e;
    cam_smi_shared.pending=false;cam_smi_shared_status();
    return cam_rx_session_guard();
}
static int cam_smi_shared_open_locked(void)
{
    static vm_offset_t common;
    uint32_t a[CAM_SMI_SHARED_WORDS],b[CAM_SMI_SHARED_WORDS];int e;
    if(!cam_smi_shared.captured || cam_smi_shared.held)return ESTALE;
    e=cam_smi_shared_voter_guard();if(e)return cam_smi_shared_fault(e);
    e=cam_smi_clock_only_guard();if(e)return cam_smi_shared_fault(e);
    if(!common)common=ml_io_map(0x3c808000,0x1000);
    if(!common)return cam_smi_shared_fault(ENOMEM);
    cam_smi_shared.common=common;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_rx_session_guard();if(e)return cam_smi_shared_fault(e);
        e=cam_smi_larb_idle();if(e)return cam_smi_shared_fault(e);
        for(unsigned word=0;word<CAM_SMI_SHARED_WORDS;word++)
            b[word]=cam_rr(cam_smi_shared_base(word),cam_smi_shared_offsets[word]);
        if(!sample)memcpy(a,b,sizeof(a));
        else if(memcmp(a,b,sizeof(a)))return cam_smi_shared_fault(ESTALE);
        delay(10);
    }
    memcpy(cam_smi_shared.before,a,sizeof(a));memcpy(cam_smi_shared.expected,a,sizeof(a));
    memcpy(cam_smi.status.shared.after,a,sizeof(a));
    cam_smi_shared.sampled=31;cam_smi_shared.held=true;
    cam_smi.status.shared.flags|=X9_CAM_SMI_SHARED_SNAPSHOT;cam_smi_shared_status();
    for(unsigned word=0;word<CAM_SMI_SHARED_WORDS;word++) {
        cam_smi.status.shared.stage=word+1;e=cam_smi_shared_write(word,cam_smi_shared_targets[word]);
        if(e)return cam_smi_shared_fault(e);
    }
    cam_smi.status.shared.stage=6;cam_smi.status.shared.flags|=X9_CAM_SMI_SHARED_CONFIGURED;
    cam_smi_shared_status();return 0;
}
/* Separate retirement from restoration: OUTMUX/PHY/MAC also consume the
 * shared profile. A failed pending config must be qualified BEFORE those
 * dependent closes, not only after they have already rejected pending. */
static int cam_smi_shared_retirement_guard(uint64_t epoch)
{
    int e;
    if(!cam_smi_shared.captured)return 0;
    if(epoch!=cam_smi_shared.epoch)return ESTALE;
    if(!cam_smi_shared.pending)return cam_smi_shared_control_guard(false);
    if(cam_smi_shared.restoring)return EBUSY;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_smi_shared_control_guard(true);if(e)return e;
        delay(10);
    }
    cam_smi_shared.pending=false;cam_smi_shared_status();
    return cam_smi_shared_control_guard(false);
}
static int cam_smi_shared_close_locked(void)
{
    struct x9_camera_smi_shared_status *s=&cam_smi.status.shared;int e;
    if(!cam_smi_shared.captured)return 0;
    if(cam_smi_shared.close_attempted)return ESTALE;
    cam_smi_shared.close_attempted=true;
    e=cam_smi_shared_retirement_guard(cam_smi.epoch);
    if(!e && cam_smi_shared.held)e=cam_rx_session_guard();
    for(unsigned i=CAM_SMI_SHARED_WORDS;i&&!e;i--) {
        unsigned word=i-1;
        if(!(cam_smi_shared.owned&(1u<<word)))continue;
        e=cam_rx_session_guard();if(!e)e=cam_smi_larb_idle();
        if(!e)e=cam_smi_shared_control_guard(false);if(e)break;
        cam_smi_shared.expected[word]=cam_smi_shared.before[word];
        cam_smi_shared.pending_word=word;cam_smi_shared.pending=true;
        cam_smi_shared.restoring=true;cam_smi_shared_status();
        cam_rw(cam_smi_shared_base(word),cam_smi_shared_offsets[word],cam_smi_shared.expected[word]);
        e=cam_wait_bits(cam_smi_shared_base(word),cam_smi_shared_offsets[word],UINT32_MAX,
            cam_smi_shared.expected[word],200);if(e)break;
        cam_smi_shared.pending=false;cam_smi_shared.restoring=false;
        cam_smi_shared.owned&=~(1u<<word);cam_smi_shared_status();
        e=cam_rx_session_guard();
    }
    if(!e) {
        cam_smi_shared.held=false;cam_smi_shared.captured=false;cam_smi_shared.epoch=0;
        s->flags=(s->flags&~X9_CAM_SMI_SHARED_RETAINED)|X9_CAM_SMI_SHARED_RESTORED;
    } else cam_smi_shared_fault(e);
    s->restore_errno=e;cam_smi_shared_status();return e;
}
