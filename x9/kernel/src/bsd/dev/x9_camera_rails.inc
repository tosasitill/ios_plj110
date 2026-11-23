/* MT6661 DOVDD/AFVDD cold shared-rail journal. All reads/writes below hold
 * X9PMIFAcquire, including read -> admission -> selector/SET/CLR -> readback.
 * Entry values are NOT a blanket cleanup exception after a confirmed write.
 * Only an unresolved write's precise prior value may also be accepted, and
 * a live load always requires the TARGET selector. No HW/EINT writes.
 * This detects visible conflicts; it is NOT an AP/SCP hardware lease or
 * protection against a same-value ABA / a change after the last read.
 */
static int cam_rail_state(struct cam_probe_journal *j, struct cam_probe_rail *p,
                          uint8_t *sel, uint8_t *en, uint8_t *da)
{
    int e=cam_pmic_r(j,p->sid,p->sel_reg,sel);
    if(!e)e=cam_pmic_r(j,p->sid,p->en_reg,en);
    if(!e)e=cam_pmic_r(j,p->sid,p->da_reg,da);
    return e;
}
static int cam_rail_hw(struct cam_probe_journal *j, struct cam_probe_rail *p,
                       uint8_t *hw, uint8_t *eint)
{
    *eint=0;
    int e=cam_pmic_r(j,p->sid,p->hw_reg,hw);
    if(!e && p->eint_reg)e=cam_pmic_r(j,p->sid,p->eint_reg,eint);
    return e;
}
static int cam_rail_conflict(struct cam_probe_rail *p,unsigned reg,
                            uint8_t expected,uint8_t observed)
{
    p->conflict=true;
    printf("X9 CAMERA RAIL CONFLICT: sid=%u reg=%04x expected=%02x observed=%02x "
        "taken=%u owned=%u/%u uncertain=%u/%u (retained, no overwrite)\n",
        p->sid,reg,expected,observed,p->taken,p->voltage_owned,p->enable_owned,
        p->selector_uncertain,p->enable_uncertain);
    return CAM_FAIL(EBUSY);
}
static int cam_rail_check_locked(struct cam_probe_journal *j, struct cam_probe_rail *p,
                                 bool cleanup,uint8_t *sel,uint8_t *en,uint8_t *da)
{
    if(!p->valid)return CAM_FAIL(EINVAL);
    if(p->conflict)return CAM_FAIL(EBUSY);
    if(!cleanup && (p->selector_uncertain || p->enable_uncertain))return CAM_FAIL(EIO);
    int e=cam_rail_state(j,p,sel,en,da);if(e)return e;
    uint8_t hw=0,eint=0;
    e=cam_rail_hw(j,p,&hw,&eint);if(e)return e;
    if(hw!=p->hw)return cam_rail_conflict(p,p->hw_reg,p->hw,hw);
    if(eint!=p->eint)return cam_rail_conflict(p,p->eint_reg,p->eint,eint);
    if(*sel!=p->selector_expected &&
       !(cleanup && p->selector_uncertain && *sel==p->selector_prior))
        return cam_rail_conflict(p,p->sel_reg,p->selector_expected,*sel);
    if(*en!=p->enable_expected &&
       !(cleanup && p->enable_uncertain && *en==p->enable_prior))
        return cam_rail_conflict(p,p->en_reg,p->enable_expected,*en);
    /* Uncertain SET/CLR can still be settling. Never admit LP, a prior
     * selector under a live load, or DA-on while EN is already off. */
    if((*en&1) && *sel!=p->target)
        return cam_rail_conflict(p,p->sel_reg,p->target,*sel);
    uint8_t want=(*en&1)?1:0;
    if((*da&5)!=want && !(cleanup && p->enable_uncertain && (*en&1) && !(*da&5)))
        return cam_rail_conflict(p,p->da_reg,want,*da&5);
    return 0;
}
static int cam_rail_snapshot(struct cam_probe_journal *j, struct cam_probe_rail *p,
                             uint32_t *evidence)
{
    int e=cam_rail_state(j,p,&p->selector,&p->enable,&p->da);
    *evidence=p->selector|((uint32_t)p->enable<<8)|((uint32_t)p->da<<16);
    if(!e)e=cam_rail_hw(j,p,&p->hw,&p->eint);
    if(e)return e;
    printf("X9 CAMERA RAIL ENTRY: sid=%u sel=%02x en=%02x da=%02x hw_op=%02x eint=%02x\n",
        p->sid,p->selector,p->enable,p->da,p->hw,p->eint);
    if((p->enable&2) || (p->da&4))return CAM_FAIL(ENOTSUP);
    if(p->enable&1) {
        if(p->selector!=p->target || !(p->da&1))return CAM_FAIL(ENOTSUP);
    } else if(p->da&1)return CAM_FAIL(EBUSY);
    /* Log 332: DOVDD LN60_7 was off (EN0/DA0) with HW_OP_EN0 nonzero.
     * mt6661-regulator.c only uses HW_OP_EN0 bit1 as the HW1 normal-mode
     * select (set_load), and the imgsensor power path enables the rail by
     * RG EN alone over the inherited HW_OP/EINT configuration. Borrow that
     * configuration: never write it, and cam_rail_check_locked still
     * requires it unchanged for the whole session. */
    p->selector_expected=p->selector_prior=p->selector;
    p->enable_expected=p->enable_prior=p->enable;
    p->valid=true;
    uint8_t sel=0,en=0,da=0;
    return cam_rail_check_locked(j,p,false,&sel,&en,&da);
}
static int cam_shared_rails(struct cam_probe_journal *j, struct x9_camera_probe *r)
{
    /* mt6661_volt_range_ops: LN60_7 1.8V coarse HIGH/fine LOW = d0;
     * LDC0_3 table selector10 = 3.0V. Never write HW_OP_EN/EINT controls. */
    j->dovdd=(struct cam_probe_rail){.sid=3,.sel_reg=0x24a,.en_reg=0x2047,
        .da_reg=0x2062,.hw_reg=0x206f,.target=0xd0};
    j->afvdd=(struct cam_probe_rail){.sid=6,.sel_reg=0x1e61,.en_reg=0x1f07,
        .da_reg=0x1f14,.hw_reg=0x1f20,.eint_reg=0x1f2f,.target=0x0a};
    if(r->sensor==X9_CAMERA_TELEPHOTO)
        j->afvdd=(struct cam_probe_rail){.sid=3,.sel_reg=0x1e62,.en_reg=0x1f47,
            .da_reg=0x1f54,.hw_reg=0x1f60,.eint_reg=0x1f6f,.target=0x0b};
    if(r->sensor==X9_CAMERA_ULTRAWIDE) {
        j->afvdd=(struct cam_probe_rail){.sid=5,.sel_reg=0x1e62,.en_reg=0x1f47,
            .da_reg=0x1f54,.hw_reg=0x1f60,.eint_reg=0x1f6f,.target=8};
        /* mt6661 SN-LDO rounds the 1.008V request UP to1.010V (51),
         * then the aliased DVDD1 request asks for1.000V (50). */
        j->dvdd=(struct cam_probe_rail){.sid=5,.sel_reg=0x24d,.en_reg=0x2007,
            .da_reg=0x2022,.hw_reg=0x202f,.target=0x51};
    }
    if(X9PMIFAcquire())return CAM_FAIL(EBUSY);
    int e=cam_rail_snapshot(j,&j->dovdd,&r->dovdd);
    if(!e && r->sensor!=X9_CAMERA_FRONT)e=cam_rail_snapshot(j,&j->afvdd,&r->afvdd);
    if(!e && r->sensor==X9_CAMERA_ULTRAWIDE) {
        uint32_t evidence=0;e=cam_rail_snapshot(j,&j->dvdd,&evidence);
        if(!e && (j->dvdd.enable&1))e=CAM_FAIL(EBUSY); /* live alias cannot be borrowed/retuned */
    }
    X9PMIFRelease();if(e)r->blockers|=X9_CAM_BLOCK_RAIL;return e;
}
static int cam_rails_guard_locked(struct cam_probe_journal *j,bool cleanup)
{
    int guard=cam_pins_guard(j,cleanup);if(guard)return guard;
    struct cam_probe_rail *rails[]={&j->dovdd,&j->afvdd,&j->dvdd};
    for(unsigned i=0;i<3;i++) {
        struct cam_probe_rail *p=rails[i];
        if(!p->taken && !p->voltage_owned && !p->enable_owned && !p->conflict)continue;
        uint8_t sel=0,en=0,da=0;
        int e=cam_rail_check_locked(j,p,cleanup,&sel,&en,&da);if(e)return e;
    }
    return 0;
}
static int cam_rails_guard(struct cam_probe_journal *j,bool cleanup)
{
    if(!j->dovdd.taken && !j->dovdd.voltage_owned && !j->dovdd.enable_owned && !j->dovdd.conflict &&
       !j->afvdd.taken && !j->afvdd.voltage_owned && !j->afvdd.enable_owned && !j->afvdd.conflict &&
       !j->dvdd.taken && !j->dvdd.voltage_owned && !j->dvdd.enable_owned && !j->dvdd.conflict)return 0;
    if(X9PMIFAcquire())return CAM_FAIL(EBUSY);
    int e=cam_rails_guard_locked(j,cleanup);X9PMIFRelease();return e;
}
static int cam_rail_wait_da(struct cam_probe_journal *j, struct cam_probe_rail *p, bool on)
{
    for(unsigned i=0;i<20;i++) {
        uint8_t da=0;int e=cam_pmic_r(j,p->sid,p->da_reg,&da);if(e)return e;
        if((da&5)==(on?1:0))return 0;
        delay(100);
    }
    return CAM_FAIL(ETIMEDOUT);
}
static int cam_rail_selector_write(struct cam_probe_journal *j,struct cam_probe_rail *p,
                                   uint8_t value,bool cleanup)
{
    uint8_t sel=0,en=0,da=0;
    int e=cam_rail_check_locked(j,p,cleanup,&sel,&en,&da);if(e)return e;
    if((en&1) || (da&1))return CAM_FAIL(EBUSY); /* never retune an enabled supply */
    e=cam_pins_guard(j,cleanup);if(e)return e;
    if(sel!=value) {
        p->voltage_owned=true;p->selector_prior=sel;p->selector_expected=value;
        p->selector_uncertain=true; /* BEFORE an ambiguous write */
        e=cam_pmic_w(j,p->sid,p->sel_reg,value);
        if(!e)e=cam_pmic_r(j,p->sid,p->sel_reg,&sel);
        if(e)return e;
        if(sel!=value)return CAM_FAIL(EIO);
    }
    p->selector_expected=value;p->selector_uncertain=false;
    return cam_rail_check_locked(j,p,cleanup,&sel,&en,&da);
}
static int cam_rail_take_locked(struct cam_probe_journal *j,struct cam_probe_rail *p)
{
    uint8_t sel=0,en=0,da=0;
    int e=cam_rail_check_locked(j,p,false,&sel,&en,&da);if(e)return e;
    if(en&1){p->taken=true;return 0;} /* precise live rail is borrowed, not owned */
    if(sel!=p->target)e=cam_rail_selector_write(j,p,p->target,false);
    if(!e)e=cam_rail_check_locked(j,p,false,&sel,&en,&da);
    if(!e)e=cam_pins_guard(j,false);
    if(e)return e;
    p->enable_owned=true;p->enable_prior=en;p->enable_expected=en|1;
    p->enable_uncertain=true; /* BEFORE possibly ambiguous SET */
    e=cam_pmic_w(j,p->sid,p->en_reg+1,1);
    if(!e)e=cam_pmic_r(j,p->sid,p->en_reg,&en);
    if(!e && en!=p->enable_expected)e=CAM_FAIL(EIO);
    if(!e)e=cam_rail_wait_da(j,p,true);
    if(e)return e;
    p->enable_uncertain=false;
    e=cam_rail_check_locked(j,p,false,&sel,&en,&da);
    if(!e)p->taken=true;
    return e;
}
static int cam_rail_take(struct cam_probe_journal *j,struct cam_probe_rail *p)
{
    if(X9PMIFAcquire())return CAM_FAIL(EBUSY);
    int e=cam_rails_guard_locked(j,false);
    if(!e)e=cam_rail_take_locked(j,p);
    if(!e)e=cam_rails_guard_locked(j,false);
    X9PMIFRelease();return e;
}
/* The only allowed live voltage transition: this boot's cold-owned UW
 * DVDD/DVDD1 alias. No general live retune, borrowed load or second enable.
 * An ambiguous selector transfer retains the rail and prerequisites. */
static int cam_uw_dvdd_alias(struct cam_probe_journal *j)
{
    struct cam_probe_rail *p=&j->dvdd;uint8_t sel=0,en=0,da=0;
    if(j->board.sensor!=X9_CAMERA_ULTRAWIDE || !p->taken || !p->enable_owned ||
       (p->enable&1) || p->target!=0x51 || p->sid!=5)return CAM_FAIL(EPERM);
    if(X9PMIFAcquire())return CAM_FAIL(EBUSY);
    int e=cam_rails_guard_locked(j,false);
    if(!e)e=cam_rail_check_locked(j,p,false,&sel,&en,&da);
    if(!e && (sel!=0x51 || (en&3)!=1 || (da&5)!=1 || p->hw || p->eint))e=CAM_FAIL(EBUSY);
    if(!e) {
        p->voltage_owned=true;p->selector_prior=sel;p->selector_expected=0x50;
        p->selector_uncertain=true;p->target=0x50;
        e=cam_pmic_w(j,p->sid,p->sel_reg,0x50);
        if(!e)e=cam_pmic_r(j,p->sid,p->sel_reg,&sel);
        if(!e && sel!=0x50)e=CAM_FAIL(EIO);
        if(!e)p->selector_uncertain=false;
        if(!e)e=cam_rails_guard_locked(j,false);
    }
    X9PMIFRelease();return e;
}
static int cam_rail_restore_locked(struct cam_probe_journal *j,struct cam_probe_rail *p)
{
    uint8_t sel=0,en=0,da=0;
    int e=cam_rail_check_locked(j,p,true,&sel,&en,&da);if(e)return e;
    e=cam_pins_guard(j,true);if(e)return e;
    if(p->enable_owned) {
        if(en&1) {
            p->enable_prior=en;p->enable_expected=p->enable;
            p->enable_uncertain=true; /* BEFORE possibly ambiguous CLR */
            e=cam_pmic_w(j,p->sid,p->en_reg+2,1);
            if(!e)e=cam_pmic_r(j,p->sid,p->en_reg,&en);
            if(!e && en!=p->enable_expected)e=CAM_FAIL(EIO);
        }
        /* Exact prior-off after an unresolved SET needs no redundant CLR. */
        if(!e)e=cam_rail_wait_da(j,p,false);
        if(e)return e;
        p->enable_expected=p->enable;p->enable_uncertain=false;
        e=cam_rail_check_locked(j,p,true,&sel,&en,&da);if(e)return e;
        p->enable_owned=false;
    }
    if(p->voltage_owned) {
        e=cam_rail_selector_write(j,p,p->selector,true);if(e)return e;
        p->voltage_owned=false;
    }
    p->taken=false; /* also retires a checked borrowed reference; no CLR */
    return 0;
}
static int cam_rail_restore(struct cam_probe_journal *j,struct cam_probe_rail *p)
{
    if(!p->taken && !p->enable_owned && !p->voltage_owned && !p->conflict)return 0;
    if(X9PMIFAcquire())return CAM_FAIL(EBUSY);
    int e=cam_rails_guard_locked(j,true);
    if(!e)e=cam_rail_restore_locked(j,p);
    if(!e)e=cam_pins_guard(j,true);
    X9PMIFRelease();return e;
}
