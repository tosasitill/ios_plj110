/* WL28681C ENABLE is one shared byte. Vendor regulator_enable_regmap uses
 * per-channel enable_mask BIT(channel-1); a whole camera-mask exemption is
 * NOT equivalent. Keep unrelated channels from the LAST read, change only
 * one bit, and refuse observed changes to our controlled channel group.
 * No new ioctl or hardware lease: AP FIFO/clock guards still apply, and
 * reads cannot eliminate the read/write race or detect an identical ABA.
 */
static int cam_wl_conflict(struct cam_probe_journal *j,uint8_t reg,
                            uint8_t expected,uint8_t observed)
{
    j->wl_conflict=true;
    printf("X9 CAMERA WL CONFLICT: reg=%02x expected=%02x observed=%02x enabled=%02x uncertain=%02x (retained, no overwrite)\n",
        reg,expected,observed,j->wl_enabled,j->wl_enable_uncertain);
    return EBUSY;
}
static int cam_wl_enable_check(struct cam_probe_journal *j,uint8_t enable,bool cleanup)
{
    if(!j->wl_tracking)return ENXIO;
    if(j->wl_conflict)return EBUSY;
    uint8_t mask=j->board.wl_mask, uncertain=j->wl_enable_uncertain;
    if(uncertain && !cleanup)return EIO;
    if(((enable^j->wl_enable_expected)&mask)==0)return 0;
    /* There is at most ONE attempted enable write. Only that bit's exact
     * prior value is admissible to cleanup; no other channel may change. */
    if(cleanup && uncertain && !(uncertain&(uncertain-1)) &&
       !((enable^j->wl_enable_expected)&mask&~uncertain) &&
       !((enable^j->wl_enable_prior)&uncertain))return 0;
    return cam_wl_conflict(j,3,j->wl_enable_expected&mask,enable&mask);
}
static int cam_wl_guard(struct cam_probe_journal *j,bool cleanup,uint8_t *enable)
{
    if(!j->wl_tracking)return ENXIO;
    if(j->wl_conflict)return EBUSY;
    int e=cam_pins_guard(j,cleanup);if(e)return e;
    e=cam_wl_read(j,3,enable);if(e)return e;
    e=cam_wl_enable_check(j,*enable,cleanup);if(e)return e;
    /* Never turn off an enabled channel after its selector was changed.
     * Check all our potentially enabled channels before dropping any
     * interleaved supply; an inactive attempted SET need not have a load. */
    for(unsigned i=0;i<j->board.wl_count;i++) {
        uint8_t bit=(uint8_t)(1u<<(j->board.wl_channel[i]-1));
        if(!(*enable&j->wl_enabled&bit))continue;
        uint8_t voltage=0,reg=(uint8_t)(3+j->board.wl_channel[i]);
        e=cam_wl_read(j,reg,&voltage);if(e)return e;
        if(voltage!=j->board.wl_code[i])
            return cam_wl_conflict(j,reg,(uint8_t)j->board.wl_code[i],voltage);
    }
    /* PMIC rails may be interleaved with these WL channels. Catch their
     * changes BEFORE a WL disable or MCLK/reset release, not only when a
     * later reverse-order PMIC restore happens to reach that rail. */
    e=cam_rails_guard(j,cleanup);if(e)return e;
    /* Selector/PMIC reads take real transactions. Re-read ENABLE after them,
     * preserving non-camera bits from this fresh byte, not the entry value. */
    e=cam_wl_read(j,3,enable);if(e)return e;
    e=cam_wl_enable_check(j,*enable,cleanup);if(e)return e;
    return cam_pins_guard(j,cleanup);
}
static int cam_wl_enable_change(struct cam_probe_journal *j,unsigned i,bool on)
{
    if(i>=j->board.wl_count)return EINVAL;
    uint8_t bit=(uint8_t)(1u<<(j->board.wl_channel[i]-1)),enable=0;
    if(j->wl_enable_uncertain && j->wl_enable_uncertain!=bit)return EBUSY;
    if(on && (j->wl_enable_uncertain || (j->wl_enabled&bit)))return EIO;
    if(!on && !(j->wl_enabled&bit))return EINVAL; /* cannot clear a borrowed bit */
    int e=cam_wl_guard(j,!on,&enable);if(e)return e;
    if(on) {
        if(enable&bit)return cam_wl_conflict(j,3,0,bit);
        uint8_t voltage=0,reg=(uint8_t)(3+j->board.wl_channel[i]);
        e=cam_wl_read(j,reg,&voltage);if(e)return e;
        if(voltage!=j->board.wl_code[i])
            return cam_wl_conflict(j,reg,(uint8_t)j->board.wl_code[i],voltage);
        /* Enabling must not use the older byte from before selector read. */
        e=cam_wl_read(j,3,&enable);if(e)return e;
        e=cam_wl_enable_check(j,enable,false);if(e)return e;
    }
    uint8_t value=on?(uint8_t)(enable|bit):(uint8_t)(enable&~bit);
    if(value!=enable) {
        j->wl_enable_prior=enable;j->wl_enable_expected=value;
        j->wl_enable_uncertain=bit; /* BEFORE an ambiguous write */
        if(on){j->wl_enabled|=bit;j->wl_owned=true;}
        const uint8_t tx[2]={3,value};uint8_t back=0;
        e=cam_bus_transfer(&j->ldo,0x35,tx,2,NULL);
        if(!e)e=cam_wl_read(j,3,&back);
        if(e)return e; /* only cleanup may resolve this attempted write */
        if(back!=value)return cam_wl_conflict(j,3,value,back);
    }
    /* A failed SET may have left the exact prior (off) bit. No second CLR
     * is needed in that case; confirmation still retires the journal. */
    j->wl_enable_expected=value;j->wl_enable_uncertain=0;
    if(!on)j->wl_enabled&=~bit;
    return 0;
}
