/* MT6993 native CSI step0: parent9=UNIVPLL2/8=312MHz, floor575mV.
 * Only a prevalidated native cold sensor requests this path. Diagnostic
 * MM opens retain their original parent. Use the already locked MM PLL
 * and the real boot-long UFS >=650mV floor, never reprogram either.
 * CLR/SET change just the selected low nibble; HWV controls PDN bit7.
 * Before each mutation all TEN XPU mux votes and aggregate must be off,
 * STA/CKSTA idle and the port FENC off. Restore after our SENINF unvote,
 * before CAMTM/domains drop; partial take uses the same original journal.
 * These checks expose conflicts, not a firmware lease or ABA protection.
 */
#include "x9_ufs/ufs_platform.h"
static struct { uint64_t epoch; unsigned sensor; } cam_native_clock_request;

static int cam_native_clock_request_locked(uint64_t epoch,unsigned sensor)
{
    if(!epoch || (!x9_camera_sensor_valid(sensor)))return EINVAL;
    if(cam_native_clock_request.epoch || cam_mm_live.epoch || cam_mm.native_clock.seen)
        return EBUSY;
    cam_native_clock_request.epoch=epoch;cam_native_clock_request.sensor=sensor;
    return 0;
}
static void cam_native_clock_cancel_unused_locked(void)
{
    /* Only when MM open failed BEFORE allocating its lifetime. */
    if(!cam_mm_live.epoch && !cam_mm.native_clock.seen)
        memset(&cam_native_clock_request,0,sizeof(cam_native_clock_request));
}
static uint32_t cam_native_clock_byte(struct cam_mm_journal *j)
{
    const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_SENINF_MUX];
    return (cam_rr(j->top,d->cfg)>>d->shift)&0x7f;
}
static int cam_native_clock_conflict(struct cam_mm_journal *j,unsigned off,
    uint32_t expected,uint32_t observed)
{
    struct cam_native_clock_receipt *c=&j->native_clock;
    c->conflict=true;
    printf("X9 CAMERA NATIVE CLOCK CONFLICT: sensor_epoch=%llu mm_epoch=%llu reg=%x "
        "expected=%x observed=%x owned/uncertain/update=%u/%u/%u (retain, no overwrite)\n",
        c->sensor_epoch,c->mm_epoch,off,expected,observed,c->owned,c->uncertain,c->update_pending);
    return ESTALE;
}
static int cam_native_clock_source_guard(struct cam_mm_journal *j)
{
    struct cam_native_clock_receipt *c=&j->native_clock;
    if(!c->pll || !sweet_x9_vcore_ufs_check(c->vcore))return ENXIO;
    if((cam_rr(c->pll,0xc)&0x073fffff)!=c->pcw ||
       (cam_rr(c->pll,0x34)&0x10)!=c->fenc)return ESTALE;
    return 0;
}
static int cam_native_clock_guard(struct cam_mm_journal *j,bool cleanup)
{
    struct cam_native_clock_receipt *c=&j->native_clock;
    const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_SENINF_MUX];
    uint32_t cfg;
    int e;
    if(!c->seen)return 0;
    if(c->conflict)return ESTALE;
    if(c->mm_epoch!=cam_mm_live.epoch || !c->sensor_epoch ||
       cam_native_clock_request.epoch!=c->sensor_epoch)return ESTALE;
    if(!j->receive.smmu_owned)return ENXIO;
    e=x9_mm_power_validate(j->receive.power_token,X9_MM_POWER_CAMERA);if(e)return e;
    e=cam_native_clock_source_guard(j);if(e)return e;
    if(!cleanup && (c->uncertain || c->update_pending))return EIO;
    e=cam_wait_bits(j->top,0x330,d->effect_mask,0,10000);if(e)return e;
    cfg=cam_native_clock_byte(j);
    if(cfg!=c->expected && !(cleanup && c->uncertain && cfg==c->prior))
        return cam_native_clock_conflict(j,d->cfg,c->expected,cfg);
    /* An issued UPDATE is never replayed. Its original wait may only be
     * retired by the real idle check and exact target readback. */
    if(c->update_pending) {
        if(cfg!=c->expected || c->uncertain)return EIO;
        c->update_pending=false;
    }
    return 0;
}
static int cam_native_clock_off_guard(struct cam_mm_journal *j,bool cleanup)
{
    const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_SENINF_MUX];
    int e=cam_native_clock_guard(j,cleanup);if(e)return e;
    e=cam_sv_sibling_guard(j);if(e)return e;
    e=cam_mm_vote_idle(&j->receive);if(e)return e;
    for(unsigned pass=0;pass<2;pass++) {
        if((cam_rr(j->receive.hwv,d->global)&d->mask) ||
           (cam_rr(j->receive.hwv,d->sta)&d->mask) ||
           (cam_rr(j->top,0x270)&d->effect_mask) ||
           (cam_rr(j->top,0x330)&d->effect_mask))return EBUSY;
        for(unsigned n=0;n<CAM_SV_SIBLING_XPUS;n++) {
            uint32_t vote=n?cam_rr(cam_sv_siblings.xpu,
                cam_sv_sibling_xpu_base(n)-0x30000+0x608):
                cam_rr(j->receive.hwv,0x20608);
            if(vote&d->mask)return EBUSY;
        }
        e=cam_native_clock_guard(j,cleanup);if(e)return e;
        __asm__ volatile("dsb sy":::"memory");
        if(!pass)delay(1);
    }
    return 0;
}
static int cam_native_clock_field_write(struct cam_mm_journal *j,unsigned parent,
    bool clear,bool cleanup)
{
    struct cam_native_clock_receipt *c=&j->native_clock;
    const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_SENINF_MUX];
    uint32_t word,other,observed,mask=15u<<d->shift;
    int e=cam_native_clock_off_guard(j,cleanup);if(e)return e;
    word=cam_rr(j->top,d->cfg);other=word&~mask;
    c->prior=(word>>d->shift)&0x7f;
    c->expected=(c->prior&0x70)|parent;c->uncertain=true;
    cam_rw(j->top,d->cfg+(clear?8:4),(clear?15u:parent)<<d->shift);
    observed=cam_rr(j->top,d->cfg);
    if((observed&~mask)!=other)
        return cam_native_clock_conflict(j,d->cfg,other,observed&~mask);
    observed=(observed>>d->shift)&0x7f;
    if(observed!=c->expected) {
        if(observed!=c->prior)return cam_native_clock_conflict(j,d->cfg,c->expected,observed);
        return EIO;
    }
    c->uncertain=false;return 0;
}
static int cam_native_clock_parent(struct cam_mm_journal *j,unsigned parent,bool cleanup)
{
    struct cam_native_clock_receipt *c=&j->native_clock;
    const struct cam_mm_desc *d=&j->desc[X9_CAM_MM_SENINF_MUX];
    int e=cam_native_clock_off_guard(j,cleanup);if(e)return e;
    if(cam_native_clock_byte(j)==((c->entry&0x70)|parent)) {
        c->expected=(c->entry&0x70)|parent;c->uncertain=false;return 0;
    }
    c->owned=true; /* BEFORE the first posted CLR */
    e=cam_native_clock_field_write(j,0,true,cleanup);
    if(!e)e=cam_native_clock_field_write(j,parent,false,cleanup);
    if(!e)e=cam_native_clock_off_guard(j,cleanup);
    if(e)return e;
    c->update_pending=true;
    cam_rw(j->top,4,d->mask); /* SENINF0 update17 / SENINF3 update20 */
    return cam_native_clock_guard(j,true);
}
static int cam_native_clock_take(struct cam_mm_journal *j,unsigned sensor)
{
    static vm_offset_t pll;
    struct cam_native_clock_receipt *c=&j->native_clock;
    uint64_t hz,vco;
    int e;
    if(!cam_native_clock_request.epoch)return 0;
    if(cam_native_clock_request.sensor!=sensor || c->seen)return ESTALE;
    memset(c,0,sizeof(*c));
    if(!pll)pll=ml_io_map(0x10043100,0x100);
    if(!pll)return ENOMEM;
    c->pll=pll;c->pcw=cam_rr(pll,0xc)&0x073fffff;c->fenc=cam_rr(pll,0x34)&0x10;
    if(c->fenc!=0x10 || !sweet_x9_vcore_ufs_check(c->vcore))return ENXIO;
    vco=(26000000ull*(c->pcw&0x3fffffu)+0x3fffu)>>14;
    hz=((vco+(1u<<((c->pcw>>24)&7))-1)/(1u<<((c->pcw>>24)&7)))/8;
    if(hz<311688000ull || hz>312312000ull)return ERANGE;
    c->sensor_epoch=cam_native_clock_request.epoch;c->mm_epoch=cam_mm_live.epoch;
    c->sensor=sensor;c->entry=c->expected=cam_native_clock_byte(j);c->seen=true;
    e=cam_native_clock_parent(j,9,false);
    if(!e)j->cfg[2]=c->expected;
    printf("X9 CAMERA NATIVE CLOCK TAKE: sensor=%u epoch=%llu/%llu cfg=%x->%x "
        "PLL=%x/%x source=%llu floor=%x/%x/%x errno=%d (CSI step0, not CAM processing)\n",
        sensor,c->sensor_epoch,c->mm_epoch,c->entry,c->expected,c->pcw,c->fenc,hz,
        c->vcore[0],c->vcore[1],c->vcore[2],e);
    return e;
}
static int cam_native_clock_restore(struct cam_mm_journal *j)
{
    struct cam_native_clock_receipt *c=&j->native_clock;
    int e;
    if(!c->seen)return 0;
    if(j->seen&(1u<<X9_CAM_MM_SENINF_MUX))return EBUSY;
    e=cam_native_clock_off_guard(j,true);if(e)return e;
    if(c->owned)e=cam_native_clock_parent(j,c->entry&15,true);
    if(!e && cam_native_clock_byte(j)!=c->entry)e=ESTALE;
    if(e)return e;
    j->cfg[2]=c->entry;c->owned=false;c->seen=false;c->restored=true;
    printf("X9 CAMERA NATIVE CLOCK RESTORE: sensor_epoch=%llu mm_epoch=%llu cfg=%x "
        "(all-XPU off, original parent restored)\n",c->sensor_epoch,c->mm_epoch,c->entry);
    return 0;
}
