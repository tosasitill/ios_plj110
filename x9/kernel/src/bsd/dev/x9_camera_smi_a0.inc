/* CAMSV TOP A0 is L19/P0 (MTK_SMMU_PORT_ID(19,0)=0x260), not
 * L19/P2=0x262 (CQI_A), L14/P0 (A1) or L29/P0 (A2). This fixed single
 * image path does NOT acquire those ports merely by holding their clocks.
 * Upstream mtk-smi.c's is-default-ostdl selects 1. DT real-time type0 is
 * ALWAYS_HRT, but probe initializes that permission as SRT until an actual
 * bandwidth request changes it: DISABLE_ULTRA bit0 and NONSEC bit3 are 1.
 * Do not turn both off by mistaking the enum for the initial permission.
 * No padded misc entry at offset0, no MMU_EN/BANK_SEL/PATH_SEL change,
 * no whole DISABLE_ULTRA/NONSEC bank write, no common0/1 access.
 */
enum { CAM_SMI_A0_CMD=0, CAM_SMI_A0_FLAG=1, CAM_SMI_A0_ULTRA=2,
    CAM_SMI_A0_FORCE=3, CAM_SMI_A0_PRE=4, CAM_SMI_A0_OSTD=5,
    CAM_SMI_A0_NONSEC=10, CAM_SMI_A0_WORDS=15,
    CAM_SMI_A0_COMMON_WORDS=12 };
_Static_assert(CAM_SMI_A0_WORDS==X9_CAM_SMI_A0_LARB_WORDS,"SMI A0 larb snapshot");
_Static_assert(CAM_SMI_A0_COMMON_WORDS==X9_CAM_SMI_A0_COMMON_WORDS,"SMI A0 common snapshot");
static const unsigned cam_smi_a0_larb_offsets[CAM_SMI_A0_WORDS]={
    0x24,0x40,0x70,0x78,0x7c,
    0x200,0x204,0x208,0x20c,0x210,
    0x380,0x384,0x388,0x38c,0x390
};
/* Preserve ALL eight shared L1ARB controls and inherited bus routing.
 * Upstream's mutable BW table is not authority to replace them with zero. */
static const unsigned cam_smi_a0_common_offsets[CAM_SMI_A0_COMMON_WORDS]={
    0x100,0x104,0x108,0x10c,0x110,0x114,0x118,0x11c,0x120,
    0x220,0x244,0x444
};
static struct {
    uint64_t epoch;
    vm_offset_t common;
    uint32_t expected[CAM_SMI_A0_WORDS], common_expected[CAM_SMI_A0_COMMON_WORDS];
    uint32_t owned_mask[CAM_SMI_A0_WORDS], owned, pending_word;
    bool held, pending, restoring, close_attempted;
} cam_smi_a0;
static void cam_smi_a0_status(void)
{
    struct x9_camera_smi_a0_status *s=&cam_smi.status.a0;
    s->owned=cam_smi_a0.owned;s->pending=cam_smi_a0.pending;
    s->pending_word=cam_smi_a0.pending_word;
}
static int cam_smi_a0_fault(int e)
{
    struct x9_camera_smi_a0_status *s=&cam_smi.status.a0;
    if(!s->last_errno)s->last_errno=e;
    if(cam_smi_a0.held)s->flags|=X9_CAM_SMI_A0_RETAINED;
    cam_smi_a0_status();return e;
}
static void cam_smi_a0_sample(uint32_t larb[CAM_SMI_A0_WORDS],
    uint32_t common[CAM_SMI_A0_COMMON_WORDS])
{
    for(unsigned n=0;n<CAM_SMI_A0_WORDS;n++)
        larb[n]=cam_rr(cam_smi.larb[0],cam_smi_a0_larb_offsets[n]);
    for(unsigned n=0;n<CAM_SMI_A0_COMMON_WORDS;n++)
        common[n]=cam_rr(cam_smi_a0.common,cam_smi_a0_common_offsets[n]);
    memcpy(cam_smi.status.a0.larb_after,larb,sizeof(cam_smi_a0.expected));
    memcpy(cam_smi.status.a0.common_after,common,sizeof(cam_smi_a0.common_expected));
}
/* Nonrecursive: called by receiver/AP/PHY's clock dependency guard.
 * This checks controls, not STAT/IRQ history or DMA/worker retirement. */
static int cam_smi_a0_guard(void)
{
    uint32_t larb[CAM_SMI_A0_WORDS],common[CAM_SMI_A0_COMMON_WORDS];int e;
    if(!cam_smi_a0.held)return 0;
    if(!cam_smi.held || cam_smi_a0.epoch!=cam_smi.epoch || cam_smi.completed!=3)
        return ESTALE;
    e=cam_smi_clock_only_guard();if(e)return e;
    cam_smi_a0_sample(larb,common);
    if(memcmp(larb,cam_smi_a0.expected,sizeof(larb)) ||
        memcmp(common,cam_smi_a0.common_expected,sizeof(common)))return ESTALE;
    return cam_smi_a0.pending?EBUSY:0;
}
static int cam_smi_a0_idle(void)
{
    int e=cam_smi_larb_idle();if(e)return e;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_smi_clock_only_guard();if(e)return e;
        /* SLP_CON protection EN/RDY, plus ordinary per-port outstanding
         * monitors. No protection handshake, reset, IRQ/R-C or clear. */
        if(cam_rr(cam_smi.larb[0],0xc)&0x10001)return EBUSY;
        cam_smi.status.a0.outstanding[0]=cam_rr(cam_smi.larb[0],0x280);
        cam_smi.status.a0.outstanding[1]=cam_rr(cam_smi.larb[0],0x780);
        if(cam_smi.status.a0.outstanding[0] || cam_smi.status.a0.outstanding[1])return EBUSY;
        delay(10);
    }
    /* Necessary inactive-port conditions only. No producer was enabled by
     * this lifetime. These zeros must NEVER be advertised as full drain. */
    return 0;
}
static int cam_smi_a0_write(unsigned word,uint32_t mask,uint32_t value)
{
    uint32_t old,next;int e;
    if(!cam_smi_a0.held || cam_smi_a0.pending)return EBUSY;
    /* Fixed allowlist. Even a future internal caller cannot smuggle a
     * legacy MMU/bank/path, another port or a shared misc write through. */
    if(!((word==CAM_SMI_A0_ULTRA && mask==1 && value==1) ||
        (word==CAM_SMI_A0_NONSEC && mask==8 && value==8) ||
        (word==CAM_SMI_A0_OSTD && mask==UINT32_MAX && value==1)))return EINVAL;
    e=cam_rx_session_guard();if(e)return e;
    e=cam_smi_a0_idle();if(e)return e;
    e=cam_smi_a0_guard();if(e)return e;
    old=cam_smi_a0.expected[word];next=(old&~mask)|(value&mask);
    if(old==next)return 0; /* borrowed target: no write, no restoration */
    cam_smi_a0.owned_mask[word]=mask;
    cam_smi_a0.owned|=1u<<word;cam_smi_a0.expected[word]=next;
    cam_smi_a0.pending_word=word;cam_smi_a0.pending=true;
    cam_smi_a0_status(); /* before posting the one write */
    cam_rw(cam_smi.larb[0],cam_smi_a0_larb_offsets[word],next);
    e=cam_wait_bits(cam_smi.larb[0],cam_smi_a0_larb_offsets[word],UINT32_MAX,next,200);
    if(e)return e;
    cam_smi_a0.pending=false;cam_smi_a0_status();
    return cam_rx_session_guard();
}
static int cam_smi_a0_pending_retire(void)
{
    uint32_t larb[CAM_SMI_A0_WORDS],common[CAM_SMI_A0_COMMON_WORDS];int e;
    if(!cam_smi_a0.pending)return 0;
    if(cam_smi_a0.restoring)return EBUSY; /* failed sole close: never replay */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_smi_clock_only_guard();if(e)return e;
        cam_smi_a0_sample(larb,common);
        /* Require the posted TARGET, preserving every other consumed
         * control. A prior value alone is NOT request retirement. */
        if(memcmp(larb,cam_smi_a0.expected,sizeof(larb)) ||
            memcmp(common,cam_smi_a0.common_expected,sizeof(common)))return EBUSY;
        delay(10);
    }
    cam_smi_a0.pending=false;cam_smi_a0_status();return cam_smi_a0_guard();
}
static int cam_smi_a0_close_locked(void)
{
    static const unsigned order[3]={CAM_SMI_A0_NONSEC,CAM_SMI_A0_ULTRA,CAM_SMI_A0_OSTD};
    struct x9_camera_smi_a0_status *s=&cam_smi.status.a0;int e;
    if(!cam_smi_a0.held)return 0;
    if(cam_smi_a0.epoch!=cam_smi.epoch || cam_smi_a0.close_attempted)return ESTALE;
    cam_smi_a0.close_attempted=true;
    e=cam_smi_a0_pending_retire();if(!e)e=cam_rx_session_guard();
    if(!e)e=cam_smi_a0_idle();
    /* Open order OSTD -> DISABLE_ULTRA -> NONSEC; reverse here. Only the
     * three allowlisted P0 fields can have an owned restoration mask. */
    for(unsigned n=0;n<3&&!e;n++) {
        unsigned word=order[n];uint32_t mask=cam_smi_a0.owned_mask[word],next;
        if(!(cam_smi_a0.owned&(1u<<word)))continue;
        e=cam_rx_session_guard();if(!e)e=cam_smi_a0_idle();
        if(!e)e=cam_smi_a0_guard();if(e)break;
        next=(cam_smi_a0.expected[word]&~mask)|(s->larb_before[word]&mask);
        cam_smi_a0.expected[word]=next;cam_smi_a0.pending_word=word;
        cam_smi_a0.pending=true;cam_smi_a0.restoring=true;cam_smi_a0_status();
        cam_rw(cam_smi.larb[0],cam_smi_a0_larb_offsets[word],next);
        e=cam_wait_bits(cam_smi.larb[0],cam_smi_a0_larb_offsets[word],UINT32_MAX,next,200);
        if(e)break;
        cam_smi_a0.pending=false;cam_smi_a0.restoring=false;
        cam_smi_a0.owned&=~(1u<<word);cam_smi_a0.owned_mask[word]=0;
        cam_smi_a0_status();e=cam_rx_session_guard();
    }
    if(!e)e=cam_smi_a0_guard();
    if(!e) {
        cam_smi_a0.held=false;cam_smi_a0.epoch=0;
        s->flags=(s->flags&~X9_CAM_SMI_A0_RETAINED)|X9_CAM_SMI_A0_RESTORED;
    } else cam_smi_a0_fault(e);
    s->restore_errno=e;cam_smi_a0_status();return e;
}
static int cam_smi_a0_open_locked(void)
{
    static vm_offset_t common;
    uint32_t a[CAM_SMI_A0_WORDS],b[CAM_SMI_A0_WORDS];
    uint32_t c[CAM_SMI_A0_COMMON_WORDS],d[CAM_SMI_A0_COMMON_WORDS];int e;
    if(cam_smi_a0.held)return EALREADY;
    if(!cam_smi.held || cam_smi.completed!=3 || cam_smi.pending)return ENXIO;
    e=cam_rx_session_guard();if(e)return e;
    if(!common)common=ml_io_map(0x3c808000,0x1000); /* COMM33, A0's supplier */
    if(!common)return ENOMEM;
    memset(&cam_smi_a0,0,sizeof(cam_smi_a0));cam_smi_a0.common=common;
    cam_smi.status.a0.flags=X9_CAM_SMI_A0_ATTEMPTED;cam_smi.status.a0.stage=1;
    e=cam_smi_a0_idle();if(e)return cam_smi_a0_fault(e);
    e=cam_smi_clock_only_guard();if(e)return cam_smi_a0_fault(e);
    cam_smi_a0_sample(a,c);delay(10);
    e=cam_rx_session_guard();if(e)return cam_smi_a0_fault(e);
    e=cam_smi_a0_idle();if(e)return cam_smi_a0_fault(e);
    cam_smi_a0_sample(b,d);
    if(memcmp(a,b,sizeof(a)) || memcmp(c,d,sizeof(c)))return cam_smi_a0_fault(ESTALE);
    memcpy(cam_smi_a0.expected,a,sizeof(a));memcpy(cam_smi_a0.common_expected,c,sizeof(c));
    memcpy(cam_smi.status.a0.larb_before,a,sizeof(a));
    memcpy(cam_smi.status.a0.common_before,c,sizeof(c));
    cam_smi_a0.held=true;cam_smi_a0.epoch=cam_smi.epoch;
    cam_smi.status.a0.flags|=X9_CAM_SMI_A0_SNAPSHOT;cam_smi.status.a0.stage=2;
    /* These controls are shared with B1/CQI/STG or other CAM masters.
     * Clock ownership is not a whole-common/misc lease: borrow a matching
     * profile or fail without altering it. Missing cold common authority
     * remains explicit; no reset-value adoption or bulk vendor restore. */
    if(a[CAM_SMI_A0_CMD]!=0x370456 || a[CAM_SMI_A0_FLAG]!=1 ||
        c[0]!=0xa || c[10]!=0x4146 || c[11]!=1)return cam_smi_a0_fault(EOPNOTSUPP);
    if((a[CAM_SMI_A0_FORCE]|a[CAM_SMI_A0_PRE])&1)return cam_smi_a0_fault(EBUSY);
    cam_smi.status.a0.flags|=X9_CAM_SMI_A0_SHARED_PROFILE_MATCHED;
    cam_smi.status.a0.stage=3;e=cam_smi_a0_write(CAM_SMI_A0_OSTD,UINT32_MAX,1);
    if(!e) {cam_smi.status.a0.stage=4;e=cam_smi_a0_write(CAM_SMI_A0_ULTRA,1,1);}
    if(!e) {cam_smi.status.a0.stage=5;e=cam_smi_a0_write(CAM_SMI_A0_NONSEC,8,8);}
    if(!e)e=cam_smi_a0_idle();
    if(!e)e=cam_rx_session_guard();
    if(e)return cam_smi_a0_fault(e);
    cam_smi.status.a0.stage=6;cam_smi.status.a0.flags|=X9_CAM_SMI_A0_CONFIGURED;
    cam_smi_a0_status();return 0;
}
