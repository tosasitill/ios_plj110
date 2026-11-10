/* Actual cold probe/standby CAMTG path. VLP mux parent (low nibble),
 * reserved bits4..6 and our AP vote have independent expected/prior state.
 * Bit7/PDN is hardware-voter controlled: never write or restore it.
 * Only an unresolved write permits its exact prior value during cleanup.
 * This is a visible-conflict journal, NOT an AP/SCP hardware lease or ABA
 * protection. No PLL programming, aggregate vote writes or global reset.
 */
static int cam_clock_conflict(struct cam_probe_journal *j,unsigned reg,
                               uint32_t expected,uint32_t observed)
{
    j->clock_conflict=true;
    printf("X9 CAMERA MCLK CONFLICT: cfg=%x shift=%u reg=%x expected=%08x observed=%08x "
        "owned=%u/%u uncertain=%u/%u (retained, no overwrite)\n",
        j->board.clk_off,j->board.clk_shift,reg,expected,observed,
        j->mux_owned,j->vote_owned,j->mux_uncertain,j->vote_uncertain);
    return EBUSY;
}
/* Inactive cleanup may admit an unresolved write's prior; active use may
 * not. DONE/CKSTA have bounded waits, FENC is checked only for active use.
 * Do not infer a frequency measurement from CON1 or FENC. */
static int cam_clock_check(struct cam_probe_journal *j,bool cleanup,bool active)
{
    if(j->clock_conflict)return EBUSY;
    if(!j->clock_seen)return active?ENOTSUP:0;
    if(!cleanup && (j->mux_uncertain || j->vote_uncertain || j->mux_update_pending))return EIO;
    uint32_t bit=cam_vote_bit(j);
    int e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    if(!e)e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(e)return e;
    uint32_t cfg=cam_clk_byte(j)&0x7f;
    if(cfg!=j->clk_expected && !(cleanup && j->mux_uncertain && cfg==j->clk_prior))
        return cam_clock_conflict(j,j->board.clk_off,j->clk_expected,cfg);
    bool ap=!!(cam_rr(j->hwv,cam_vote_off(j)+8)&bit);
    if(ap!=j->vote_expected && !(cleanup && j->vote_uncertain && ap==j->vote_prior))
        return cam_clock_conflict(j,cam_vote_off(j)+8,j->vote_expected,ap);
    uint32_t con1=cam_rr(j->pll,0x0c);
    if(con1!=j->pll_saved)return cam_clock_conflict(j,0x0c,j->pll_saved,con1);
    if(active) {
        if(j->mux_uncertain || j->vote_uncertain || j->mux_update_pending ||
           (cfg&15)!=7 || !ap)return EBUSY;
        uint32_t fenc=cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit);
        if(!fenc)return cam_clock_conflict(j,0x270,1u<<j->board.clk_status_bit,fenc);
        fenc=cam_rr(j->pll,0x34)&(1u<<4);
        if(!fenc)return cam_clock_conflict(j,0x34,1u<<4,fenc);
    }
    return 0;
}
static int cam_mux_off_guard(struct cam_probe_journal *j,bool cleanup)
{
    int e=cam_clock_check(j,cleanup,false);if(e)return e;
    uint32_t bit=cam_vote_bit(j);
    if((cam_rr(j->hwv,cam_all_off(j))&bit) ||
       (cam_rr(j->hwv,cam_vote_off(j)+8)&bit) ||
       (cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit)))return EBUSY;
    /* The off-owner checks are several reads, not an atomic reservation. */
    uint32_t cfg=cam_clk_byte(j)&0x7f;
    if(cfg!=j->clk_expected && !(cleanup && j->mux_uncertain && cfg==j->clk_prior))
        return cam_clock_conflict(j,j->board.clk_off,j->clk_expected,cfg);
    return 0;
}
static int cam_mux_field_write(struct cam_probe_journal *j,unsigned off,
                                unsigned parent,bool cleanup)
{
    int e=cam_mux_off_guard(j,cleanup);if(e)return e;
    uint32_t before=cam_clk_byte(j)&0x7f,expected=(before&0x70)|parent;
    if(before!=j->clk_expected && !(cleanup && j->mux_uncertain && before==j->clk_prior))
        return cam_clock_conflict(j,j->board.clk_off,j->clk_expected,before);
    if(before==expected) {
        j->clk_expected=expected;j->mux_uncertain=false;return 0;
    }
    j->clk_prior=before;j->clk_expected=expected;j->mux_uncertain=true;
    uint32_t value=(off==j->board.clk_off+8?15u:parent)<<j->board.clk_shift;
    cam_rw(j->vlp,off,value);
    uint32_t observed=cam_clk_byte(j)&0x7f;
    if(observed!=expected) {
        if(observed!=before)return cam_clock_conflict(j,j->board.clk_off,expected,observed);
        return EIO; /* unresolved write: ONLY this exact prior is admissible */
    }
    j->mux_uncertain=false; /* confirmed CLR/SET may not later fall back */
    return 0;
}
/* Parent changes require every XPU unvoted and CAMTG FENC off. */
static int cam_mux_parent(struct cam_probe_journal *j,unsigned parent,bool take)
{
    if(parent>15)return EINVAL;
    int e=cam_mux_off_guard(j,!take);if(e)return e;
    if((cam_clk_byte(j)&0x7f)==((j->clk_saved&0x70)|parent) && !j->mux_update_pending) {
        /* An unresolved CLR/SET still at the entry needs no redundant write. */
        j->clk_expected=(j->clk_saved&0x70)|parent;j->mux_uncertain=false;return 0;
    }
    if(take)j->mux_owned=true;
    e=cam_mux_field_write(j,j->board.clk_off+8,0,!take);
    if(!e)e=cam_mux_field_write(j,j->board.clk_off+4,parent,!take);
    if(!e)e=cam_mux_off_guard(j,!take);
    if(e)return e;
    j->mux_update_pending=true; /* BEFORE UPDATE, not a readable RW journal */
    cam_rw(j->vlp,4,1u<<j->board.clk_update_bit);
    e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(!e)e=cam_clock_check(j,true,false);
    if(!e)j->mux_update_pending=false;
    return e;
}
/* AP vote state is independent from the mux journal. Never CLR a confirmed
 * vote after its observed state/configuration changed. */
static int cam_clock_vote(struct cam_probe_journal *j,bool on)
{
    if(!on && !j->vote_owned)return 0;
    int e=cam_clock_check(j,!on,on?false:j->clock_active);if(e)return e;
    uint32_t bit=cam_vote_bit(j);unsigned off=cam_vote_off(j);
    bool before=!!(cam_rr(j->hwv,off+8)&bit);
    if(before!=j->vote_expected && !(!on && j->vote_uncertain && before==j->vote_prior))
        return cam_clock_conflict(j,off+8,j->vote_expected,before);
    if(on && before)return EBUSY;
    if(!on && !before) {
        /* Confirmed off after an unresolved SET: no redundant CLR. */
        j->vote_expected=false;j->vote_uncertain=false;j->clock_active=false;
        e=cam_clock_check(j,true,false);if(!e)j->vote_owned=false;
        return e;
    }
    if(on)j->vote_owned=true;
    j->vote_prior=before;j->vote_expected=on;j->vote_uncertain=true;
    cam_rw(j->hwv,off+(on?0:4),bit);
    e=cam_wait_bits(j->hwv,off+8,bit,on?bit:0,200);
    if(!e) {
        j->vote_uncertain=false; /* EN confirmed: prior is no longer allowed */
        if(!on)j->clock_active=false;
        e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    }
    if(!e)e=cam_clock_check(j,true,false);
    if(!e && !on)j->vote_owned=false;
    return e;
}
static int cam_clock_take(struct cam_probe_journal *j,struct x9_camera_probe *r)
{
    uint32_t bit=cam_vote_bit(j),ap,all;int e=0;
    r->mclk_cfg=cam_rr(j->vlp,j->board.clk_off);
    r->mclk_status=cam_rr(j->vlp,0x330);
    r->hwv_vote=ap=cam_rr(j->hwv,cam_vote_off(j)+8);
    r->hwv_done=cam_rr(j->hwv,cam_done_off(j));
    r->pll_con1=j->pll_saved=cam_rr(j->pll,0x0c);
    r->pll_fence=cam_rr(j->pll,0x34);
    j->clk_saved=cam_clk_byte(j);j->clk_expected=j->clk_prior=j->clk_saved&0x7f;
    if(!cam_pll_compatible(j->pll_saved))return ENOTSUP;
    e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    if(!e)e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(e)return e;
    if(cam_clk_byte(j)!=j->clk_saved || cam_rr(j->hwv,cam_vote_off(j)+8)!=ap ||
       cam_rr(j->pll,0x0c)!=j->pll_saved)return EBUSY;
    j->vote_expected=j->vote_prior=!!(ap&bit);j->clock_seen=true;
    all=cam_rr(j->hwv,cam_all_off(j));
    if((ap|all)&bit) {
        if((j->clk_saved&15)!=7 ||
           !(cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit)))return EBUSY;
    } else {
        if(cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit))return EBUSY;
        if((j->clk_saved&15)!=7){e=cam_mux_parent(j,7,true);if(e)return e;}
    }
    if(!(ap&bit)){e=cam_clock_vote(j,true);if(e)return e;}
    e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(!e)e=cam_wait_bits(j->vlp,j->board.clk_off,15u<<j->board.clk_shift,
                           7u<<j->board.clk_shift,1000);
    if(!e)e=cam_wait_bits(j->vlp,0x270,1u<<j->board.clk_status_bit,
                           1u<<j->board.clk_status_bit,1000);
    if(!e)e=cam_wait_bits(j->pll,0x34,1u<<4,1u<<4,1000);
    r->hwv_done=cam_rr(j->hwv,cam_done_off(j));
    r->pll_fence=cam_rr(j->pll,0x34);
    if(!e)e=cam_clock_check(j,false,true);
    if(!e){j->clock_active=true;r->flags|=X9_CAM_PROBE_CLOCK;}
    return e;
}
static int cam_clock_restore(struct cam_probe_journal *j)
{
    if(!j->mux_owned && !j->vote_owned && !j->clock_conflict) {
        j->clock_active=false;return 0; /* retire borrowed use, no hardware CLR */
    }
    int e=cam_clock_check(j,true,j->clock_active);if(e)return e;
    if(j->vote_owned){e=cam_clock_vote(j,false);if(e)return e;}
    if(j->mux_owned) {
        e=cam_wait_bits(j->vlp,0x270,1u<<j->board.clk_status_bit,0,1000);
        if(e)return e;
        e=cam_mux_parent(j,j->clk_saved&15,false);if(e)return e;
        j->mux_owned=false;
    }
    j->clock_active=false;return 0;
}
