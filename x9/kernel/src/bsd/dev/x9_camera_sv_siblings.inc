/* CAMSV A register-access admission, camera_control_lock held.
 * Captured DT: A shares L19 with B, L14 with C, L29 with D/E/F.
 * clk-mt6993-cam.c: CG34 bit12=CAMTG, bit13=TOP, bit14=A,
 * bit15=B, bit16=C, bit17=D, bit18=E, bit19=F. TOP/CAMTG or a
 * LARB ungate must not revive a sibling with inherited producer controls.
 * Freeze B..F's ten XPU EN banks before ANY MM vote. Only after the
 * journal has consumed MRAW domain8 may its physical CG be inspected.
 * No sibling register access, enable, reset, IRQ read/ack or vote write.
 * Gated/unvoted is admission for this inactive lifetime, NOT DMA drain,
 * a firmware lease, SID authority, or protection against same-value ABA.
 */
enum { CAM_SV_SIBLING_MASK=0x1fu<<15, CAM_SV_SIBLING_XPUS=10 };
static struct {
    vm_offset_t xpu;
    uint64_t epoch;
    uint32_t before[CAM_SV_SIBLING_XPUS],after[CAM_SV_SIBLING_XPUS];
    uint32_t global_before,global_after,gate,sta,checks,reads;
    bool attempted,captured,gate_sampled;
    int error;
} cam_sv_siblings;
static unsigned cam_sv_sibling_xpu_base(unsigned n)
{ return n<5?0x20000+n*0x10000:0x70000+(n-5)*0x1000; }
static uint32_t cam_sv_sibling_vote(struct cam_mm_journal *j,unsigned n)
{
    unsigned offset=34*12+8;
    if(!n)return cam_rr(j->receive.hwv,0x20000+offset)&CAM_SV_SIBLING_MASK;
    return cam_rr(cam_sv_siblings.xpu,cam_sv_sibling_xpu_base(n)-0x30000+offset)&CAM_SV_SIBLING_MASK;
}
static int cam_sv_sibling_fault(int e)
{
    if(!cam_sv_siblings.error) {
        cam_sv_siblings.error=e;
        printf("X9 CAMERA SV SIBLINGS: errno=%d epoch=%llu captured=%u votes_sampled=%u gate_sampled=%u "
            "gate=%x global=%x sta=%x (no sibling write; retain admitted dependencies)\n",
            e,cam_sv_siblings.epoch,cam_sv_siblings.captured,cam_sv_siblings.reads!=0,
            cam_sv_siblings.gate_sampled,
            cam_sv_siblings.gate,cam_sv_siblings.global_after,cam_sv_siblings.sta);
    }
    return e;
}
static int cam_sv_sibling_sample(struct cam_mm_journal *j,bool physical)
{
    int e;
    /* No cam_mm_guard recursion. Prove the actual shared power/infra path
     * before the RO foreign EN window and, separately, before MRAW CG. */
    if(!j->receive.smmu_owned)return ENXIO;
    e=x9_mm_power_validate(j->receive.power_token,X9_MM_POWER_CAMERA);if(e)return e;
    if((cam_rr(j->receive.hwv,0x12900)&CAM_RES_MM_INFRA)!=CAM_RES_MM_INFRA ||
        (cam_rr(j->receive.hwv,0x1131c)&CAM_RES_MM_INFRA))return ENXIO;
    e=cam_mm_vote_idle(&j->receive);if(e)return e;
    for(unsigned sample=0;sample<2;sample++) {
        cam_sv_siblings.sta=cam_rr(j->receive.hwv,0x11888)&CAM_SV_SIBLING_MASK;
        if(cam_sv_siblings.sta)return EBUSY;
        cam_sv_siblings.global_after=cam_rr(j->receive.hwv,0x12288)&CAM_SV_SIBLING_MASK;
        for(unsigned n=0;n<CAM_SV_SIBLING_XPUS;n++)
            cam_sv_siblings.after[n]=cam_sv_sibling_vote(j,n);
        if(!cam_sv_siblings.reads) {
            memcpy(cam_sv_siblings.before,cam_sv_siblings.after,sizeof(cam_sv_siblings.before));
            cam_sv_siblings.global_before=cam_sv_siblings.global_after;
        }
        if(cam_sv_siblings.reads!=UINT32_MAX)cam_sv_siblings.reads++;
        if(cam_sv_siblings.global_after)return EBUSY;
        for(unsigned n=0;n<CAM_SV_SIBLING_XPUS;n++)
            if(cam_sv_siblings.after[n])return EBUSY;
        if(physical) {
            /* An ambiguous MRAW SET may have exposed CG even without a
             * completed slot. Require its actual AP/global/ACK target;
             * prior/off alone cannot retire that request at close. */
            uint32_t bit=1u<<8;
            if((cam_rr(j->receive.hwv,0x1131c)&bit) ||
                !(cam_rr(j->receive.hwv,0x20708)&bit) ||
                !(cam_rr(j->receive.hwv,0x11318)&bit) ||
                !(cam_rr(j->receive.hwv,0x12900)&bit))return ENXIO;
            cam_sv_siblings.gate=cam_rr(j->sv_cg,0)&CAM_SV_SIBLING_MASK;
            cam_sv_siblings.gate_sampled=true;
            if(cam_sv_siblings.gate!=CAM_SV_SIBLING_MASK)return EBUSY;
        }
        __asm__ volatile("dsb sy":::"memory");
        /* Bracket the hardware reads by the real token/infra authority.
         * No late adoption or extra waiting for a foreign voter to go away. */
        e=x9_mm_power_validate(j->receive.power_token,X9_MM_POWER_CAMERA);if(e)return e;
        if((cam_rr(j->receive.hwv,0x12900)&CAM_RES_MM_INFRA)!=CAM_RES_MM_INFRA ||
            (cam_rr(j->receive.hwv,0x1131c)&CAM_RES_MM_INFRA))return ENXIO;
        if(!sample)delay(1);
    }
    if(cam_sv_siblings.checks!=UINT32_MAX)cam_sv_siblings.checks++;
    return 0;
}
static int cam_sv_sibling_begin(struct cam_mm_journal *j)
{
    static vm_offset_t xpu;
    int e;
    if(cam_sv_siblings.captured)return EBUSY;
    memset(&cam_sv_siblings,0,sizeof(cam_sv_siblings));
    cam_sv_siblings.epoch=cam_mm_live.epoch;cam_sv_siblings.attempted=true;
    /* DT MM_HWV size74900. Existing power map ends22000, insufficient
     * for XPU1..9; this separate Device window reads only their EN words. */
    if(!xpu)xpu=ml_io_map(0x31c30000,0x45000);
    if(!xpu)return cam_sv_sibling_fault(ENOMEM);
    cam_sv_siblings.xpu=xpu;
    e=cam_sv_sibling_sample(j,false);if(e)return cam_sv_sibling_fault(e);
    cam_sv_siblings.captured=true;return 0;
}
static int cam_sv_sibling_guard(struct cam_mm_journal *j)
{
    bool physical;
    int e;
    /* A rejected begin issued no MM vote: its power-only receive reference
     * can unwind. After admission, ANY uncertain change is terminal even
     * if a later sample goes back to the all-zero voter baseline. */
    if(!cam_sv_siblings.captured)return 0;
    if(cam_sv_siblings.error)return cam_sv_siblings.error;
    if(j!=&cam_mm || !cam_mm_live.epoch || cam_sv_siblings.epoch!=cam_mm_live.epoch)
        return cam_sv_sibling_fault(ESTALE);
    /* completed persists in reverse close; still-SEEN is essential. An
     * issued/borrowed MRAW reference also needs target qualification on
     * partial take, not a blind unwind just because completed is clear. */
    physical=(j->seen&(1u<<X9_CAM_MM_MRAW)) &&
        ((j->completed&(1u<<X9_CAM_MM_MRAW)) || j->owned[X9_CAM_MM_MRAW] ||
         j->borrowed[X9_CAM_MM_MRAW]);
    e=cam_sv_sibling_sample(j,physical);
    return e?cam_sv_sibling_fault(e):0;
}
static void cam_sv_sibling_retire(void)
{
    /* Called only after the real MM/receive restore succeeds. Cached log
     * fields remain evidence; no reads of an already unpowered domain. */
    cam_sv_siblings.captured=false;
}
static void cam_sv_sibling_log(void)
{
    if(!cam_sv_siblings.attempted || cam_sv_siblings.epoch!=cam_mm_live.serial)return;
    printf("X9 CAMERA SV SIBLING JOURNAL: epoch=%llu captured=%u checks=%u errno=%d "
        "mask=%x votes_sampled=%u gate_sampled=%u gate=%x global=%x/%x (cached controls, NOT DMA drain)\n",
        cam_sv_siblings.epoch,cam_sv_siblings.captured,cam_sv_siblings.checks,
        cam_sv_siblings.error,CAM_SV_SIBLING_MASK,cam_sv_siblings.reads!=0,cam_sv_siblings.gate_sampled,
        cam_sv_siblings.gate,cam_sv_siblings.global_before,cam_sv_siblings.global_after);
    for(unsigned n=0;n<CAM_SV_SIBLING_XPUS && cam_sv_siblings.reads;n++)
        printf("X9 CAMERA SV SIBLING XPU: n=%u EN=%x/%x\n",n,
            cam_sv_siblings.before[n],cam_sv_siblings.after[n]);
}
