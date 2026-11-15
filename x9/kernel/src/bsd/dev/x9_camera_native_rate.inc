/* MT6993 MMDVFS v5 real CAM processing supplier, control mutex/thread.
 * Vendor user-cam is VMMRC XPU0 / mux position0, NOT a VCP message and
 * NOT the generic HWCCF mux AP enable. The RC changes its voltage/AL mux
 * from the single AP level vote. Current live DT level2=416MHz; both
 * preview profiles fit this rate. No RC init/AVS/PMIC/AL-table rewrite.
 * Require the cold all-zero CAM vote baseline; preserve every other
 * mux/XPU field. An outstanding UPDATE is never reissued. Reverse close
 * restores exactly this receipt before domains drop, or retains it.
 * Controller level and FENC are not a PMIC voltage/frequency measurement;
 * native source additionally takes two fresh CAM CKGEN_CK2 ID14 counts.
 */
static struct {
    vm_offset_t rc,pll;
    uint64_t epoch;
    uint32_t entry_cfg,expected_cfg,pcw,fenc,level,prior,pre;
    uint32_t vote,target,current,rc_level,vcore[3];
    bool seen,owned,uncertain,update_pending,applied,conflict;
} cam_native_rate;

static int cam_native_rate_source_guard(struct cam_mm_journal *j)
{
    int e;
    if(!j->receive.smmu_owned || !cam_native_rate.pll)return ENXIO;
    e=x9_mm_power_validate(j->receive.power_token,X9_MM_POWER_CAMERA);if(e)return e;
    if(!sweet_x9_vcore_ufs_check(cam_native_rate.vcore))return ENXIO;
    if((cam_rr(cam_native_rate.pll,0xc)&0x073fffff)!=cam_native_rate.pcw ||
       (cam_rr(cam_native_rate.pll,0x34)&0x10)!=cam_native_rate.fenc)return ESTALE;
    return 0;
}
static int cam_native_rate_foreign_guard(void)
{
    for(unsigned xpu=1;xpu<13;xpu++)
        if(cam_rr(cam_native_rate.rc,0x10000+xpu*0x10)&15)return EBUSY;
    return 0;
}
static uint32_t cam_native_rate_cfg(struct cam_mm_journal *j)
{
    const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_CAM_MUX];
    return (cam_rr(j->top,d->cfg)>>d->shift)&0x7f;
}
static int cam_native_rate_guard(struct cam_mm_journal *j,bool cleanup)
{
    uint32_t pre,cfg;
    int e;
    if(!cam_native_rate.seen)return 0;
    if(cam_native_rate.conflict || cam_native_rate.epoch!=cam_mm_live.epoch)return ESTALE;
    e=cam_native_rate_source_guard(j);if(e)return e;
    e=cam_native_rate_foreign_guard();if(e){cam_native_rate.conflict=true;return e;}
    pre=cam_rr(cam_native_rate.rc,0x100d0)&15;
    if(pre!=cam_native_rate.pre && !(cleanup && cam_native_rate.uncertain &&
       pre==cam_native_rate.prior))goto conflict;
    cfg=cam_native_rate_cfg(j);
    if(cam_native_rate.applied && cfg!=cam_native_rate.expected_cfg)goto conflict;
    if(!cam_native_rate.applied && !cam_native_rate.update_pending &&
       (cfg!=cam_native_rate.entry_cfg || (cam_rr(cam_native_rate.rc,0x10000)&15) ||
        (cam_rr(cam_native_rate.rc,0x10234)&15) ||
        (cam_rr(cam_native_rate.rc,0x10264)&15)))goto conflict;
    if(!cleanup && (cam_native_rate.uncertain || cam_native_rate.update_pending))return EIO;
    if(cam_native_rate.applied) {
        cam_native_rate.vote=cam_rr(cam_native_rate.rc,0x10000)&15;
        cam_native_rate.target=cam_rr(cam_native_rate.rc,0x10234)&15;
        cam_native_rate.current=cam_rr(cam_native_rate.rc,0x10264)&15;
        cam_native_rate.rc_level=cam_rr(cam_native_rate.rc,0x5c4);
        if(cam_native_rate.vote!=cam_native_rate.level ||
           cam_native_rate.target!=cam_native_rate.level ||
           cam_native_rate.current!=cam_native_rate.level ||
           (cam_native_rate.rc_level&0xff)>5 ||
           (cam_native_rate.rc_level&0xff)<cam_native_rate.level)goto conflict;
    }
    return 0;
conflict:
    cam_native_rate.conflict=true;
    printf("X9 CAMERA CAM RATE CONFLICT: epoch=%llu pre=%x expected=%x "
        "vote/target/current=%x/%x/%x cfg=%x expected_cfg=%x (retain RC/MM)\n",
        cam_native_rate.epoch,pre,cam_native_rate.pre,cam_native_rate.vote,
        cam_native_rate.target,cam_native_rate.current,cam_native_rate_cfg(j),
        cam_native_rate.expected_cfg);
    return ESTALE;
}
/* Resolve only the ORIGINAL posted UPDATE by actual RO vote/target/current
 * and actual AL parent. Never clear/retrigger the RC or its interrupt. */
static int cam_native_rate_wait(struct cam_mm_journal *j)
{
    uint32_t cfg=(cam_native_rate.entry_cfg&0x70)|(cam_native_rate.level?10u:0u);
    int e;
    for(unsigned n=0;n<10000;n++) {
        e=cam_native_rate_source_guard(j);if(e)return e;
        e=cam_native_rate_foreign_guard();if(e){cam_native_rate.conflict=true;return e;}
        if((cam_rr(cam_native_rate.rc,0x100d0)&15)!=cam_native_rate.pre)return ESTALE;
        cam_native_rate.vote=cam_rr(cam_native_rate.rc,0x10000)&15;
        cam_native_rate.target=cam_rr(cam_native_rate.rc,0x10234)&15;
        cam_native_rate.current=cam_rr(cam_native_rate.rc,0x10264)&15;
        cam_native_rate.rc_level=cam_rr(cam_native_rate.rc,0x5c4);
        if(cam_native_rate.rc_level==0xffffffffu)return ENXIO;
        if(cam_native_rate.vote==cam_native_rate.level &&
           cam_native_rate.target==cam_native_rate.level &&
           cam_native_rate.current==cam_native_rate.level && cam_native_rate_cfg(j)==cfg &&
           (cam_native_rate.rc_level&0xff)<=5 &&
           (cam_native_rate.rc_level&0xff)>=cam_native_rate.level &&
           !(cam_rr(j->top,0x330)&(1u<<18))) {
            cam_native_rate.expected_cfg=cfg;cam_native_rate.update_pending=false;
            cam_native_rate.applied=true;j->cfg[0]=cfg;
            return cam_native_rate_guard(j,true);
        }
        delay(10);
    }
    return ETIMEDOUT;
}
static int cam_native_rate_field(struct cam_mm_journal *j,unsigned value,bool clear)
{
    uint32_t before,after;
    int e=cam_native_rate_guard(j,true);if(e)return e;
    if(cam_native_rate.update_pending)return EBUSY;
    before=cam_rr(cam_native_rate.rc,0x100d0);
    cam_native_rate.prior=before&15;cam_native_rate.pre=value;cam_native_rate.uncertain=true;
    cam_rw(cam_native_rate.rc,clear?0x110c:0x1110,clear?15:value);
    after=cam_rr(cam_native_rate.rc,0x100d0);
    if((after&~15u)!=(before&~15u)) {
        cam_native_rate.conflict=true;return ESTALE;
    }
    if((after&15)!=value) {
        if((after&15)!=cam_native_rate.prior)cam_native_rate.conflict=true;
        return EIO;
    }
    cam_native_rate.uncertain=false;return 0;
}
static int cam_native_rate_apply(struct cam_mm_journal *j,unsigned level)
{
    int e=cam_native_rate_guard(j,true);if(e)return e;
    if(cam_native_rate.update_pending)return EBUSY;
    cam_native_rate.owned=true;
    e=cam_native_rate_field(j,0,true);
    if(!e)e=cam_native_rate_field(j,level,false);
    if(!e)e=cam_native_rate_guard(j,true);
    if(e)return e;
    cam_native_rate.level=level;cam_native_rate.applied=false;
    cam_native_rate.update_pending=true;
    cam_rw(cam_native_rate.rc,0x1114,15);
    return cam_native_rate_wait(j);
}
static int cam_native_rate_take(struct cam_mm_journal *j)
{
    static vm_offset_t rc,pll;
    uint64_t vco,hz;
    int e;
    if(!cam_native_clock_request.epoch)return 0;
    if(cam_native_rate.seen)return EBUSY;
    if(!rc)rc=ml_io_map(0x31d00000,0x11000);
    if(!pll)pll=ml_io_map(0x10043100,0x100);
    if(!rc || !pll)return ENOMEM;
    memset(&cam_native_rate,0,sizeof(cam_native_rate));
    cam_native_rate.rc=rc;cam_native_rate.pll=pll;cam_native_rate.epoch=cam_mm_live.epoch;
    cam_native_rate.entry_cfg=cam_native_rate.expected_cfg=cam_native_rate_cfg(j);
    cam_native_rate.pcw=cam_rr(pll,0xc)&0x073fffff;cam_native_rate.fenc=cam_rr(pll,0x34)&0x10;
    if(cam_native_rate.fenc!=0x10)return ENXIO;
    vco=(26000000ull*(cam_native_rate.pcw&0x3fffffu)+0x3fffu)>>14;
    hz=((vco+(1u<<((cam_native_rate.pcw>>24)&7))-1)/
        (1u<<((cam_native_rate.pcw>>24)&7)))/6;
    if(hz<415584000ull || hz>416416000ull)return ERANGE;
    e=cam_native_rate_source_guard(j);if(e)return e;
    e=cam_native_rate_foreign_guard();if(e)return e;
    /* Only the currently idle camera baseline is ours to raise/restore.
     * No adoption of an Android/vcp/cpc/debug user, even a matching rate. */
    if((cam_native_rate.entry_cfg&15) || (cam_rr(rc,0x100d0)&15) ||
       (cam_rr(rc,0x10000)&15) || (cam_rr(rc,0x10234)&15) ||
       (cam_rr(rc,0x10264)&15))return EBUSY;
    delay(10);e=cam_native_rate_source_guard(j);if(e)return e;
    cam_native_rate.seen=true;
    e=cam_native_rate_guard(j,true);
    if(!e)e=cam_native_rate_apply(j,2);
    printf("X9 CAMERA CAM RATE TAKE: epoch=%llu cfg=%x->%x pre=%x "
        "vote/target/current=%x/%x/%x rc_level=%x source=%llu errno=%d "
        "(real VMMRC user-cam vote, voltage not PMIC measurement)\n",
        cam_native_rate.epoch,cam_native_rate.entry_cfg,cam_native_rate.expected_cfg,
        cam_native_rate.pre,cam_native_rate.vote,cam_native_rate.target,
        cam_native_rate.current,cam_native_rate.rc_level,hz,e);
    return e;
}
static int cam_native_rate_restore(struct cam_mm_journal *j)
{
    int e;
    if(!cam_native_rate.seen)return 0;
    if(j->seen&(1u<<X9_CAM_MM_CAM_MUX))return EBUSY;
    e=cam_native_rate_guard(j,true);if(e)return e;
    if(cam_native_rate.update_pending) {
        e=cam_native_rate_wait(j);if(e)return e;
    }
    if(cam_native_rate.owned) {
        if(cam_native_rate.applied || cam_native_rate.pre)
            e=cam_native_rate_apply(j,0);
        /* No UPDATE was posted: precise baseline still needs actual RO
         * confirmation, never a blind release based on PRE alone. */
        else if((cam_rr(cam_native_rate.rc,0x10000)&15) ||
           (cam_rr(cam_native_rate.rc,0x10234)&15) ||
           (cam_rr(cam_native_rate.rc,0x10264)&15))e=EBUSY;
    }
    if(!e && cam_native_rate_cfg(j)!=cam_native_rate.entry_cfg)e=ESTALE;
    if(e)return e;
    j->cfg[0]=cam_native_rate.entry_cfg;cam_native_rate.seen=cam_native_rate.owned=false;
    printf("X9 CAMERA CAM RATE RESTORE: epoch=%llu original_cfg=%x "
        "(RC vote retired before CAM domains)\n",cam_native_rate.epoch,cam_native_rate.entry_cfg);
    return 0;
}
