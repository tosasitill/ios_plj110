/* Actual cold sensor/AP-route/WL-wake pin path. MODE/DIR/DOUT have
 * independent expected/prior/dirty/uncertain state. Borrowed references
 * are checked too, but are never restored or driven as our writes.
 * MODE uses MT6993 MWR; drive/DIR/DOUT use masked SET/CLR aliases.
 * These visible-state guards are NOT a hardware lease or ABA protection.
 */
static uint32_t cam_pin_entry(const struct cam_probe_pin *p,unsigned field)
{ return field==CAM_PIN_MODE?p->mode:field==CAM_PIN_DIR?p->dir:p->dout; }
static uint32_t cam_pin_value(struct cam_probe_journal *j,const struct cam_probe_pin *p,
                               unsigned field)
{
    return field==CAM_PIN_MODE?cam_pin_mode(j->gpio,p->pin):
        cam_pin_get(j->gpio,field==CAM_PIN_DIR?0:0x100,p->pin);
}
static int cam_pin_conflict(struct cam_probe_pin *p,unsigned field,
                             uint32_t expected,uint32_t observed)
{
    p->conflict=true;
    printf("X9 CAMERA PIN CONFLICT: pin=%u field=%u expected=%x observed=%x "
        "dirty=%x uncertain=%x taken=%u (retained, no overwrite)\n",
        p->pin,field,expected,observed,p->dirty,p->uncertain,p->taken);
    return EBUSY;
}
static int cam_pin_check(struct cam_probe_journal *j,struct cam_probe_pin *p,bool cleanup)
{
    if(!p->valid)return EINVAL;
    if(p->conflict)return EBUSY;
    if(!cleanup && p->uncertain)return EIO;
    for(unsigned i=0;i<CAM_PIN_FIELDS;i++) {
        uint32_t v=cam_pin_value(j,p,i);
        if(v!=p->expected[i] && !(cleanup && (p->uncertain&(1u<<i)) && v==p->prior[i]))
            return cam_pin_conflict(p,i,p->expected[i],v);
    }
    return 0;
}
static void cam_pin_snapshot(struct cam_probe_journal *j,struct cam_probe_pin *p,unsigned pin)
{
    p->pin=pin;p->mode=cam_pin_mode(j->gpio,pin);
    p->dir=cam_pin_get(j->gpio,0,pin);p->dout=cam_pin_get(j->gpio,0x100,pin);
    for(unsigned i=0;i<CAM_PIN_FIELDS;i++)p->expected[i]=p->prior[i]=cam_pin_entry(p,i);
    p->valid=true;
}
/* One pin's guard deliberately does not depend on other pins: during
 * isolation, a known MCLK may still be disconnected after reset conflicts,
 * and a known reset may still be asserted after a MCLK conflict. */
static int cam_pin_write(struct cam_probe_journal *j,struct cam_probe_pin *p,
                          unsigned field,uint32_t value,bool cleanup)
{
    if(field>=CAM_PIN_FIELDS || value>(field==CAM_PIN_MODE?15u:1u) || !p->taken)return EINVAL;
    int e=cam_pin_check(j,p,cleanup);if(e)return e;
    uint32_t bit=1u<<field,before=cam_pin_value(j,p,field);
    if(before!=p->expected[field] && !(cleanup && (p->uncertain&bit) && before==p->prior[field]))
        return cam_pin_conflict(p,field,p->expected[field],before);
    if(before==value) {
        p->expected[field]=value;p->uncertain&=~bit;return 0;
    }
    p->prior[field]=before;p->expected[field]=value;p->dirty|=bit;p->uncertain|=bit;
    p->owned=true; /* BEFORE the possibly ambiguous write */
    if(field==CAM_PIN_MODE)e=cam_pin_mux(j->gpio,p->pin,value);
    else e=cam_pin_bit(j->gpio,field==CAM_PIN_DIR?0:0x100,p->pin,value);
    uint32_t observed=cam_pin_value(j,p,field);
    if(e || observed!=value) {
        if(observed!=before && observed!=value)return cam_pin_conflict(p,field,value,observed);
        return EIO;
    }
    p->uncertain&=~bit; /* confirmed values have no prior fallback */
    return cam_pin_check(j,p,cleanup);
}
static int cam_drive_conflict(struct cam_probe_journal *j,uint32_t expected,uint32_t observed)
{
    j->drive.conflict=true;
    printf("X9 CAMERA DRIVE CONFLICT: pin=%u expected=%x observed=%x dirty=%x "
        "uncertain=%u (retained, no overwrite)\n",j->board.mclk,expected,observed,
        j->drive.dirty,j->drive.uncertain);
    return EBUSY;
}
static int cam_drive_check(struct cam_probe_journal *j,bool cleanup)
{
    struct cam_probe_drive *p=&j->drive;
    if(!p->valid)return EINVAL;
    if(p->conflict)return EBUSY;
    if(!cleanup && p->uncertain)return EIO;
    uint32_t v=(cam_rr(j->board.drive_base,0)>>j->board.drive_shift)&7;
    if(v!=p->expected && !(cleanup && p->uncertain && v==p->prior))
        return cam_drive_conflict(j,p->expected,v);
    return 0;
}
static int cam_pins_guard(struct cam_probe_journal *j,bool cleanup)
{
    struct cam_probe_pin *pins[]={&j->reset,&j->mclk,&j->wake,
        &j->route[0],&j->route[1],&j->route[2],&j->route[3]};
    for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++) {
        struct cam_probe_pin *p=pins[i];
        if(!p->taken && !p->owned && !p->conflict)continue;
        int e=cam_pin_check(j,p,cleanup);if(e)return e;
    }
    if(j->drive.taken || j->drive.dirty || j->drive.conflict)return cam_drive_check(j,cleanup);
    return 0;
}
static int cam_drive_alias(struct cam_probe_journal *j,uint32_t bits,bool set,bool cleanup)
{
    if(!bits)return 0;
    struct cam_probe_drive *p=&j->drive;
    int e=cam_pins_guard(j,cleanup);if(e)return e;
    if(cam_pin_mode(j->gpio,j->board.mclk))return EBUSY; /* never retune live MCLK */
    uint32_t before=(cam_rr(j->board.drive_base,0)>>j->board.drive_shift)&7;
    if(before!=p->expected && !(cleanup && p->uncertain && before==p->prior))
        return cam_drive_conflict(j,p->expected,before);
    p->prior=before;p->expected=set?(before|bits):(before&~bits);
    p->dirty|=bits;p->uncertain=true; /* BEFORE SET/CLR; never whole-bank RMW */
    cam_rw(j->board.drive_base,set?4:8,bits<<j->board.drive_shift);
    uint32_t observed=(cam_rr(j->board.drive_base,0)>>j->board.drive_shift)&7;
    if(observed!=p->expected) {
        if(observed!=before)return cam_drive_conflict(j,p->expected,observed);
        return EIO;
    }
    p->uncertain=false;
    return cam_pins_guard(j,cleanup);
}
static int cam_drive_write(struct cam_probe_journal *j,uint32_t value,bool cleanup)
{
    if(value>7)return EINVAL;
    int e=cam_pins_guard(j,cleanup);if(e)return e;
    e=cam_drive_check(j,cleanup);if(e)return e;
    j->drive.taken=true;
    uint32_t before=(cam_rr(j->board.drive_base,0)>>j->board.drive_shift)&7;
    if(before!=j->drive.expected && !(cleanup && j->drive.uncertain && before==j->drive.prior))
        return cam_drive_conflict(j,j->drive.expected,before);
    if(cam_pin_mode(j->gpio,j->board.mclk))return EBUSY;
    e=cam_drive_alias(j,value&~before,true,cleanup);if(e)return e;
    /* SET is confirmed before computing the CLR delta; an ambiguous SET
     * stops here, never silently proceeds to the second mutation. */
    e=cam_drive_check(j,cleanup);if(e)return e;
    before=(cam_rr(j->board.drive_base,0)>>j->board.drive_shift)&7;
    if(before!=j->drive.expected && !(cleanup && j->drive.uncertain && before==j->drive.prior))
        return cam_drive_conflict(j,j->drive.expected,before);
    e=cam_drive_alias(j,before&~value,false,cleanup);if(e)return e;
    j->drive.expected=value;j->drive.uncertain=false;
    return cam_drive_check(j,cleanup);
}
static int cam_pin_restore(struct cam_probe_journal *j,struct cam_probe_pin *p)
{
    if(!p->taken && !p->owned && !p->conflict)return 0;
    int e=cam_pin_check(j,p,true);if(e)return e;
    /* An input's inherited high DOUT is restored ONLY after DIR=input.
     * A borrowed pin with no dirty fields receives no write at all. */
    if(p->dirty&(1u<<CAM_PIN_DOUT)) {
        e=cam_pin_write(j,p,CAM_PIN_DOUT,0,true);if(e)return e;
    }
    const unsigned order[]={CAM_PIN_DIR,CAM_PIN_DOUT,CAM_PIN_MODE};
    for(unsigned n=0;n<sizeof(order)/sizeof(order[0]);n++) {
        unsigned i=order[n];uint32_t bit=1u<<i;if(!(p->dirty&bit))continue;
        e=cam_pin_write(j,p,i,cam_pin_entry(p,i),true);if(e)return e;
        p->dirty&=~bit;p->owned=!!p->dirty;
    }
    e=cam_pin_check(j,p,true);if(e)return e;
    p->taken=p->owned=false;return 0;
}
/* Sensor I2C pad bias. Android live state (adb debugfs pinctrl, camera
 * idle and streaming alike) for SCL4/SDA4 174/175 and SCL12/SDA12 162/163:
 * "1001001111 (rsel = 7)" = IES1 SMT1 PU1 PD0 RSEL7. The DT has no pinctrl
 * for these pads; LK only uses i2c12. Logs 335-337: everything else matches
 * Android but every sensor ID read NACKs. pinctrl-mt6993.c fields:
 * bank5 0x120e0000: IES 0x50, PD 0x80, PU 0x90, RSEL 0xb0, SMT 0xc0;
 * bank4 0x12060000 (162/163): IES 0x70, PD 0xa0, PU 0xb0, RSEL 0xd0, SMT 0xe0.
 * SET/CLR aliases at +4/+8, like the drive field. */
struct cam_pad { unsigned pin, bit, rsel_shift; };
static void cam_pad_log(const char *what,vm_offset_t b,const struct cam_pad *p,
    unsigned ies,unsigned pd,unsigned pu,unsigned rs,unsigned smt)
{
    printf("X9 CAMERA PAD %s: pin=%u ies=%u smt=%u pu=%u pd=%u rsel=%u\n",what,p->pin,
        (cam_rr(b,ies)>>p->bit)&1,(cam_rr(b,smt)>>p->bit)&1,(cam_rr(b,pu)>>p->bit)&1,
        (cam_rr(b,pd)>>p->bit)&1,(cam_rr(b,rs)>>p->rsel_shift)&7);
}
static int cam_sensor_pads(struct cam_probe_journal *j)
{
    static vm_offset_t b4,b5;
    if(!b5)b5=ml_io_map(0x120e0000,0x1000);
    if(!b4)b4=ml_io_map(0x12060000,0x1000);
    if(!b4 || !b5)return ENOMEM;
    const struct cam_pad ref[2]={{162,13,15},{163,15,21}};
    for(unsigned i=0;i<2;i++)cam_pad_log("ref-i2c12",b4,&ref[i],0x70,0xa0,0xb0,0xd0,0xe0);
    struct cam_pad pads[2];
    if(j->board.scl==174){pads[0]=(struct cam_pad){174,3,3};pads[1]=(struct cam_pad){175,5,9};}
    else if(j->board.scl==172){pads[0]=(struct cam_pad){172,2,0};pads[1]=(struct cam_pad){173,4,6};}
    else return 0; /* tele/UW: no Android capture, unchanged */
    for(unsigned i=0;i<2;i++) {
        const struct cam_pad *p=&pads[i];uint32_t m=1u<<p->bit;
        cam_pad_log("entry",b5,p,0x50,0x80,0x90,0xb0,0xc0);
        cam_rw(b5,0x80+8,m);                       /* PD clear */
        cam_rw(b5,0x50+4,m);                       /* IES set */
        cam_rw(b5,0xc0+4,m);                       /* SMT set */
        cam_rw(b5,0xb0+4,7u<<p->rsel_shift);       /* RSEL = 7 */
        cam_rw(b5,0x90+4,m);                       /* PU set */
        __asm__ volatile("dsb sy" ::: "memory");
        cam_pad_log("android",b5,p,0x50,0x80,0x90,0xb0,0xc0);
        if(!((cam_rr(b5,0x50)>>p->bit)&1) || !((cam_rr(b5,0x90)>>p->bit)&1) ||
           ((cam_rr(b5,0x80)>>p->bit)&1) || ((cam_rr(b5,0xb0)>>p->rsel_shift)&7)!=7)return EIO;
    }
    return 0;
}
static int cam_routes_take(struct cam_probe_journal *j)
{
    for(unsigned i=0;i<4;i++) {
        struct cam_probe_pin *p=&j->route[i];
        int e=cam_pins_guard(j,false);if(!e)e=cam_pin_check(j,p,false);if(e)return e;
        if(!p->ap_mode || (p->mode!=p->ap_mode && (p->mode || p->dir)))return EBUSY;
        p->taken=true; /* includes a borrowed AP function: guard, never restore */
        e=cam_pin_write(j,p,CAM_PIN_MODE,p->ap_mode,false);if(e)return e;
    }
    int e=cam_sensor_pads(j);if(e)return e;
    return cam_pins_guard(j,false);
}
static int cam_routes_restore(struct cam_probe_journal *j)
{
    for(unsigned i=4;i>0;i--) {
        int e=cam_pins_guard(j,true);if(!e)e=cam_pin_restore(j,&j->route[i-1]);if(e)return e;
    }
    return 0;
}
/* Android wl28681c probe selects pinctrl "reset_high" = state_reset_high_a2:
 * pinmux <0xcb00 0xbb00 0xca00> = GPIO203, GPIO187, GPIO202, mode0,
 * output-high, and never lowers 187/202 ("reset_low" drives only 203).
 * Both sensors (back i2c4, front i2c2) NACK with MCLK/XCLR proven at the
 * pads (logs 347/349) while 187/202 were never driven. Drive them like
 * Android, once, and keep them high (no restore, as in Android). */
static void cam_wl_group_high(struct cam_probe_journal *j)
{
    static const unsigned pins[]={187,202};
    for(unsigned i=0;i<2;i++) {
        unsigned n=pins[i];
        unsigned m0=cam_pin_mode(j->gpio,n),d0=cam_pin_get(j->gpio,0,n),
            o0=cam_pin_get(j->gpio,0x100,n),i0=cam_pin_get(j->gpio,0x200,n);
        int e=0;
        if(m0)e=cam_pin_mux(j->gpio,n,0);
        if(!e)e=cam_pin_bit(j->gpio,0x100,n,1);
        if(!e)e=cam_pin_bit(j->gpio,0,n,1);
        printf("X9 CAMERA WL GROUP: pin=%u mode=%u->%u dir=%u->%u dout=%u->%u din=%u->%u errno=%d (Android reset_high a2)\n",
            n,m0,cam_pin_mode(j->gpio,n),d0,cam_pin_get(j->gpio,0,n),o0,cam_pin_get(j->gpio,0x100,n),
            i0,cam_pin_get(j->gpio,0x200,n),e);
    }
}
static int cam_wake_take(struct cam_probe_journal *j)
{
    struct cam_probe_pin *p=&j->wake;
    int e=cam_pins_guard(j,false);if(!e)e=cam_pin_check(j,p,false);if(e)return e;
    cam_wl_group_high(j);
    /* Vendor pinctrl state = GPIO mode0 output-high; an input that
     * reads high via its pull is not a reason to refuse (log 333: GPIO171
     * entry mode0 input). Mode is journaled and restored like DIR/DOUT. */
    p->taken=true;
    if(p->mode){e=cam_pin_write(j,p,CAM_PIN_MODE,0,false);if(e)return e;}
    if(p->dir && p->dout)return 0; /* inherited high is never pulsed */
    e=cam_pin_write(j,p,CAM_PIN_DOUT,0,false);
    if(!e)e=cam_pins_guard(j,false);
    if(!e)e=cam_pin_write(j,p,CAM_PIN_DIR,1,false);
    if(!e)e=cam_pins_guard(j,false);
    if(!e)e=cam_pin_write(j,p,CAM_PIN_DOUT,1,false);
    if(!e)delay(1000);
    if(!e)e=cam_pins_guard(j,false);
    return e;
}
static int cam_pins_take(struct cam_probe_journal *j)
{
    int e=cam_pins_guard(j,false);
    if(!e)e=cam_pin_check(j,&j->reset,false);if(!e)e=cam_pin_check(j,&j->mclk,false);
    if(!e)e=cam_drive_check(j,false);if(e)return e;
    /* Vendor power-on starts from pinctrl rst-low / mclk-off (mode0);
     * it selects those states unconditionally, so do the same. */
    j->reset.taken=j->mclk.taken=true;j->pins_owned=true;
    if(j->mclk.mode)e=cam_pin_write(j,&j->mclk,CAM_PIN_MODE,0,false);
    if(!e && j->reset.mode)e=cam_pin_write(j,&j->reset,CAM_PIN_MODE,0,false);
    if(!e)e=cam_pin_write(j,&j->reset,CAM_PIN_DOUT,0,false);
    if(!e)e=cam_pins_guard(j,false);
    if(!e)e=cam_pin_write(j,&j->reset,CAM_PIN_DIR,1,false);
    if(!e)e=cam_pins_guard(j,false);
    return e;
}
static int cam_pins_isolate(struct cam_probe_journal *j)
{
    int first=0,e;
    if(!j->pins_owned)return 0;
    e=cam_pin_write(j,&j->reset,CAM_PIN_DOUT,0,true);if(e)first=e;
    /* No re-take/retry of an unresolved DIR write to manufacture isolation. */
    if(!e && (cam_pin_mode(j->gpio,j->reset.pin) ||
       !cam_pin_get(j->gpio,0,j->reset.pin) || cam_pin_get(j->gpio,0x100,j->reset.pin)))first=EBUSY;
    e=cam_pin_write(j,&j->mclk,CAM_PIN_MODE,0,true);if(e && !first)first=e;
    delay(1000);
    if(!first)first=cam_pins_guard(j,true);
    return first;
}
static int cam_pins_restore(struct cam_probe_journal *j)
{
    int e=cam_pins_guard(j,true);if(e)return e;
    if(j->drive.taken || j->drive.dirty) {
        if(j->drive.dirty){e=cam_drive_write(j,j->drive.saved,true);if(e)return e;}
        e=cam_drive_check(j,true);if(e)return e;
        j->drive.dirty=0;j->drive.taken=false;
    }
    e=cam_pin_restore(j,&j->reset);if(!e)e=cam_pins_guard(j,true);
    if(!e)e=cam_pin_restore(j,&j->mclk);
    if(!e)j->pins_owned=false;
    return e;
}
