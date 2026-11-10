/* MT6993 clk-fmeter CKGEN_CK2, not the VLP audio meter or a PLL test route.
 * DT: CKSYS_MM10040000, CKMTR_MM10041000/0x20. SENINF0/3 IDs18/21, FENC14/11.
 * Software mode0, divider1 (/2), load1023, source1/reference0 (26MHz).
 * No HW-mode takeover, timeout result, reset, cancel or unbounded retry.
 * All writes use their own immediate old/new journal, no RO count replay.
 * Included inside the inactive PHY lifetime under camera_control_lock. */
enum { CAM_CLOCK_FIELDS=4, CAM_CLOCK_NO_FIELD=4 };
struct cam_phy_meter_field { vm_offset_t base; unsigned offset; uint32_t mask,before,expected,owned; };
static struct {
    vm_offset_t meter;
    struct cam_phy_meter_field field[CAM_CLOCK_FIELDS];
    unsigned pending;
    uint32_t next,fenc,source_cfg;
    bool active,triggered,done;
} cam_phy_meter;
static void cam_phy_meter_status(void)
{
    cam_phy.status.clock_dirty_mask=0;
    for(unsigned n=0;n<CAM_CLOCK_FIELDS;n++)if(cam_phy_meter.field[n].owned)
        cam_phy.status.clock_dirty_mask|=1u<<n;
    cam_phy.status.clock_pending_field=cam_phy_meter.active?cam_phy_meter.pending:CAM_CLOCK_NO_FIELD;
}
static int cam_phy_clock_guard(void)
{
    const struct cam_mm_desc *d=&cam_mm.desc[X9_CAM_MM_SENINF_MUX];
    int e;
    if(!cam_phy_meter.active)return 0;
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    if(!(cam_rr(cam_mm.top,0x270)&cam_phy_meter.fenc))return ESTALE;
    if(((cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xff)!=cam_phy_meter.source_cfg)return ESTALE;
    if(cam_rr(cam_phy_meter.meter,0xc)&1)return EBUSY;
    for(unsigned n=0;n<CAM_CLOCK_FIELDS;n++) {
        const struct cam_phy_meter_field *r=&cam_phy_meter.field[n];
        uint32_t v=cam_rr(r->base,r->offset)&r->mask;
        if(v!=r->expected && !(n==cam_phy_meter.pending && v==cam_phy_meter.next))return ESTALE;
    }
    return 0;
}
/* The short window must not include full MM/HWCCF/secure guards after the
 * trigger and before observing BUSY. This local poll guard checks the
 * meter inputs, current SENINF parent/PDN, CKSTA and FENC every iteration;
 * the full dependency guard runs immediately before issue and after done. */
static int cam_phy_meter_poll_guard(void)
{
    unsigned slot=X9_CAM_MM_SENINF_MUX;
    const struct cam_mm_desc *d=&cam_mm.desc[slot];
    if(((cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xff)!=cam_phy_meter.source_cfg ||
       (cam_rr(cam_mm.top,0x330)&d->effect_mask) ||
       !(cam_rr(cam_mm.top,0x270)&cam_phy_meter.fenc) ||
       (cam_rr(cam_phy_meter.meter,0xc)&1))return ESTALE;
    for(unsigned n=0;n<CAM_CLOCK_FIELDS;n++) {
        const struct cam_phy_meter_field *r=&cam_phy_meter.field[n];
        uint32_t v=cam_rr(r->base,r->offset)&r->mask;
        if(v!=r->expected && !(n==cam_phy_meter.pending && v==cam_phy_meter.next))return ESTALE;
    }
    return 0;
}
static int cam_phy_meter_change(unsigned n,uint32_t mask,uint32_t value)
{
    struct cam_phy_meter_field *r;
    uint32_t raw,next,v;
    bool trigger=n==1 && (mask&16) && (value&16);
    int e;
    if(n>=CAM_CLOCK_FIELDS || cam_phy_meter.pending!=CAM_CLOCK_NO_FIELD)return EBUSY;
    r=&cam_phy_meter.field[n];if(mask&~r->mask)return EINVAL;
    if(trigger && cam_phy_meter.triggered)return EALREADY;
    e=cam_phy_dependencies();if(e)return e;
    raw=cam_rr(r->base,r->offset);if((raw&r->mask)!=r->expected)return ESTALE;
    next=(raw&~mask)|(value&mask);
    if(n==2)next&=~0xffffu; /* CON1[15:0] is RO count, NOT an RW saved field */
    if((next&r->mask)==r->expected)return trigger?EBUSY:0;
    r->owned|=mask;cam_phy_meter.pending=n;cam_phy_meter.next=next&r->mask;
    if(trigger) {
        cam_phy_meter.triggered=true;cam_phy_meter.done=false;
        cam_phy.status.clock_pending=1;cam_phy.status.clock_busy_seen=0;
    }
    cam_rw(r->base,r->offset,next);v=cam_rr(r->base,r->offset)&r->mask;
    if(v!=cam_phy_meter.next)return EIO;
    r->expected=v;cam_phy_meter.pending=CAM_CLOCK_NO_FIELD;
    /* No slow SMC/HWV path between trigger and the fresh BUSY observation. */
    return trigger?0:cam_phy_dependencies();
}
static int cam_phy_meter_restore(void)
{
    int e;
    if(!cam_phy_meter.active)return 0;
    if(cam_phy_meter.triggered && !cam_phy_meter.done)return EBUSY;
    e=cam_phy_clock_guard();if(e)return e;
    if(cam_rr(cam_phy_meter.meter,0x18)&1)return EBUSY;
    if(cam_phy_meter.pending!=CAM_CLOCK_NO_FIELD) {
        struct cam_phy_meter_field *r=&cam_phy_meter.field[cam_phy_meter.pending];
        uint32_t v=cam_rr(r->base,r->offset)&r->mask;
        if(v!=r->expected && v!=cam_phy_meter.next)return ESTALE;
        r->expected=v;cam_phy_meter.pending=CAM_CLOCK_NO_FIELD;
    }
    if(cam_phy_meter.triggered) {
        e=cam_phy_meter_change(1,16,0);if(e)return e;
        cam_phy_meter.triggered=false;
    }
    for(unsigned i=CAM_CLOCK_FIELDS;i;i--) {
        struct cam_phy_meter_field *r=&cam_phy_meter.field[i-1];
        if(r->owned) {
            e=cam_phy_meter_change(i-1,r->owned,r->before);if(e)return e;
            r->owned=0;
        }
    }
    e=cam_phy_clock_guard();if(e)return e;
    if((cam_rr(cam_phy_meter.meter,0x18)&1) || (cam_rr(cam_phy_meter.meter,0)&16))return EBUSY;
    cam_phy_meter.active=false;return 0;
}
static int cam_phy_meter_capture(void)
{
    static const unsigned offsets[]={0x340,0,4,8};
    static const uint32_t masks[]={0x7f00,0xffff9110,0x03ff0000,0xff00001f};
    uint32_t again[CAM_CLOCK_FIELDS];
    const struct cam_mm_desc *d=&cam_mm.desc[X9_CAM_MM_SENINF_MUX];
    int e;
    if(cam_phy_meter.active)return EBUSY;
    e=cam_phy_dependencies();if(e)return e;
    if(!cam_phy_meter.meter)cam_phy_meter.meter=ml_io_map(0x10041000,0x20);
    if(!cam_phy_meter.meter)return ENOMEM;
    cam_phy_meter.fenc=1u<<(14-cam_phy.status.csi_port);
    cam_phy_meter.source_cfg=(cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xff;
    for(unsigned n=0;n<CAM_CLOCK_FIELDS;n++) {
        struct cam_phy_meter_field *r=&cam_phy_meter.field[n];
        r->base=n?cam_phy_meter.meter:cam_mm.top;r->offset=offsets[n];r->mask=masks[n];
        r->before=r->expected=cam_rr(r->base,r->offset)&r->mask;r->owned=0;
    }
    /* Do not clear an inherited software request or autonomous HW mode. */
    if((cam_rr(cam_phy_meter.meter,0xc)&1) || (cam_rr(cam_phy_meter.meter,0x18)&1) ||
       (cam_phy_meter.field[1].before&0x9110) || cam_phy_meter.field[0].before)return EBUSY;
    delay(10);e=cam_phy_dependencies();if(e)return e;
    for(unsigned n=0;n<CAM_CLOCK_FIELDS;n++) {
        const struct cam_phy_meter_field *r=&cam_phy_meter.field[n];
        again[n]=cam_rr(r->base,r->offset)&r->mask;
        if(again[n]!=r->before)return ESTALE;
    }
    if((cam_rr(cam_phy_meter.meter,0xc)&1) || (cam_rr(cam_phy_meter.meter,0x18)&1) ||
       !(cam_rr(cam_mm.top,0x270)&cam_phy_meter.fenc))return EBUSY;
    cam_phy_meter.active=true;cam_phy_meter.triggered=cam_phy_meter.done=false;
    cam_phy_meter.pending=CAM_CLOCK_NO_FIELD;return cam_phy_clock_guard();
}
static int cam_phy_meter_sample(unsigned sample)
{
    uint64_t span,deadline;
    uint32_t count;
    bool busy_seen=false;
    int e;
    e=cam_phy_meter_capture();if(e)return e;
    e=cam_phy_meter_change(0,0x7f00,(18+cam_phy.status.csi_port)<<8);
    if(!e)e=cam_phy_meter_change(1,0xffff9110,0x04109000);
    if(!e)e=cam_phy_meter_change(2,0x03ff0000,0x03ff0000);
    if(!e)e=cam_phy_meter_change(3,0xff00001f,0x01000001); /* /2, CKGEN_CK2, ref0 */
    if(e)return e;
    nanoseconds_to_absolutetime(500000,&span);deadline=mach_absolute_time()+span;
    e=cam_phy_meter_change(1,16,16);if(e)return e;
    for(unsigned i=0;i<400;i++) {
        bool busy=(cam_rr(cam_phy_meter.meter,0x18)&1)!=0;
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        e=cam_phy_meter_poll_guard();if(e)return e;
        if(busy) {busy_seen=true;cam_phy.status.clock_busy_seen=1;}
        else if(busy_seen) {
            cam_phy_meter.done=true;cam_phy.status.clock_pending=0;
            e=cam_phy_dependencies();if(e)return e;
            count=cam_rr(cam_phy_meter.meter,4)&0xffff;
            if(!count || count==0xffff || (cam_rr(cam_phy_meter.meter,4)&0xffff)!=count)return EIO;
            if(cam_rr(cam_phy_meter.meter,0x18)&1)return EBUSY;
            cam_phy.status.clock_count[sample]=count;
            return cam_phy_meter_restore();
        } else if(i>=20)return ETIMEDOUT;
        delay(1);
    }
    return ETIMEDOUT;
}
static int cam_phy_meter_retirement_guard(void)
{
    if(cam_phy_meter.active && cam_phy_meter.triggered && !cam_phy_meter.done)return EBUSY;
    return cam_phy_clock_guard();
}
