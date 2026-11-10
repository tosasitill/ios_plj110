/* MT6993 I2C_EAST/IMPE and I2C_PERI/IMPC cold parent clocks.
 * clk-mt6993.c A0/B0: MUX_GATE_FENC_CLR_SET_UPD_CHK, NOT HWV.
 * clk-mux.c::mtk_clk_mux_fenc_enable_setclr / disable_setclr write only
 * CLK_CFG_16 CLR/SET (+118/+114); UPDATE2 is for REPARENT, not enable.
 * FENC +278 bits29/31 are ON; CKSTA +338 bits27/29 are BUSY.
 * clk-mt6993-peri.c: all physical IMPE child gates mask3f, IMPC mask0d.
 * No reparent, PLL writes, guessed HWV vote, sibling controller reads,
 * bus reset or IRQ clearing. Local journal guards are NOT a hardware lease.
 * camera_control_lock serializes both sensors' probe/standby transactions.
 */
static uint32_t cam_bus_parent_byte(const struct cam_probe_bus *b)
{ return (cam_rr(b->top,0x110)>>b->clock_shift)&0xff; }
static int cam_bus_parent_conflict(struct cam_probe_bus *b,unsigned off,
                                    uint32_t expected,uint32_t observed)
{
    b->parent_conflict=true;
    printf("X9 CAMERA AP CLOCK CONFLICT: shift=%u reg=%x expected=%08x observed=%08x owned=%u (retained, no overwrite)\n",
        b->clock_shift,off,expected,observed,b->parent_owned);
    return EBUSY;
}
static int cam_bus_source_check(struct cam_probe_bus *b,uint32_t cfg)
{
    /* Keep the entry source; an already awake PLL is required, never
     * inferred from I2C FENC. CAMTG holds UNIVPLL until parent cleanup. */
    uint32_t source=cfg&3;
    vm_offset_t pll=source==2?b->mainpll:b->univpll;
    if(source && !(cam_rr(pll,0x34)&(1u<<4)))return ENOTSUP;
    uint32_t con1=source?cam_rr(pll,0x0c):0;
    if(b->clock_saved && (b->clock_source!=source || b->clock_con1!=con1))
        return cam_bus_parent_conflict(b,source?0x0c:0x110,
            source?b->clock_con1:b->clock_source,source?con1:source);
    b->clock_source=source;b->clock_con1=con1;b->clock_saved=true;
    return 0;
}
static int cam_bus_clock(struct cam_probe_bus *b)
{
    if(b->parent_conflict)return EBUSY;
    int e=cam_wait_bits(b->top,0x338,1u<<(b->clock_bit-2),0,1000);
    if(e)return e;
    uint32_t cfg=cam_bus_parent_byte(b);
    if(b->parent_seen && cfg!=b->parent_expected)
        return cam_bus_parent_conflict(b,0x110,b->parent_expected,cfg);
    if(cfg&0x80)return ENOTSUP;
    if(!(cam_rr(b->top,0x278)&(1u<<b->clock_bit)))return ENOTSUP;
    if(b->parent_owned) {
        uint32_t siblings=b->group_gate_mask&~b->gate_bit;
        uint32_t gates=cam_rr(b->gate,0xe00)&siblings;
        if(gates!=siblings)return cam_bus_parent_conflict(b,0xe00,siblings,gates);
    }
    return cam_bus_source_check(b,cfg);
}
static int cam_bus_parent_take(struct cam_probe_bus *b)
{
    if(b->parent_seen || b->parent_owned || b->parent_conflict)return EALREADY;
    int e=cam_wait_bits(b->top,0x338,1u<<(b->clock_bit-2),0,1000);
    if(e)return e;
    uint32_t cfg=cam_bus_parent_byte(b),bit=1u<<b->clock_bit;
    uint32_t fenc=cam_rr(b->top,0x278)&bit;
    uint32_t gates=cam_rr(b->gate,0xe00)&b->group_gate_mask;
    e=cam_bus_source_check(b,cfg);if(e)return e;
    delay(10);
    if(cam_bus_parent_byte(b)!=cfg || (cam_rr(b->top,0x278)&bit)!=fenc ||
       (cam_rr(b->gate,0xe00)&b->group_gate_mask)!=gates ||
       (cam_rr(b->top,0x338)&(1u<<(b->clock_bit-2))))return EBUSY;
    e=cam_bus_source_check(b,cfg);if(e)return e;
    if(fenc) {
        if(cfg&0x80)return EBUSY; /* unknown forced-on source */
    } else {
        /* Do not wake a shared parent with an ungated child of unknown
         * ownership. DISABLED alone is insufficient, including our child. */
        if(!(cfg&0x80) || gates!=b->group_gate_mask)return EBUSY;
    }
    b->parent_entry=b->parent_expected=cfg;b->parent_seen=true;
    if(!fenc) {
        b->parent_owned=true;b->parent_expected=cfg&0x7f; /* BEFORE CLR */
        cam_rw(b->top,0x118,1u<<(b->clock_shift+7));
        e=cam_wait_bits(b->top,0x278,bit,bit,1000);
        if(e)return e; /* restoration owns even an ambiguous CLR */
    }
    e=cam_bus_clock(b);
    printf("X9 CAMERA AP CLOCK: shift=%u entry=%02x expected=%02x gates=%x/%x owned=%u errno=%d (PDN/FENC, no reparent)\n",
        b->clock_shift,b->parent_entry,b->parent_expected,gates,b->group_gate_mask,b->parent_owned,e);
    return e;
}
static int cam_bus_parent_restore(struct cam_probe_bus *b)
{
    if(!b->parent_owned)return 0; /* borrowed clock is never disabled */
    if(b->parent_conflict || b->owned || b->gate_owned || b->wedged ||
       b->conflict || b->fifo_owned || b->request_owned)return EBUSY;
    int e=cam_wait_bits(b->top,0x338,1u<<(b->clock_bit-2),0,1000);
    if(e)return e;
    uint32_t cfg=cam_bus_parent_byte(b);
    /* A failed CLR may leave either the precise entry or expected byte.
     * Any other source/reserved/PDN change must NOT be overwritten. */
    if(cfg!=b->parent_entry && cfg!=b->parent_expected)
        return cam_bus_parent_conflict(b,0x110,b->parent_expected,cfg);
    uint32_t gates=cam_rr(b->gate,0xe00)&b->group_gate_mask;
    if(gates!=b->group_gate_mask)
        return cam_bus_parent_conflict(b,0xe00,b->group_gate_mask,gates);
    e=cam_bus_source_check(b,cfg);if(e)return e;
    /* Recheck after source validation, before the only possible SET. */
    if(cam_bus_parent_byte(b)!=cfg ||
       (cam_rr(b->gate,0xe00)&b->group_gate_mask)!=gates)return EBUSY;
    if(cfg!=b->parent_entry)
        cam_rw(b->top,0x114,1u<<(b->clock_shift+7));
    e=cam_wait_bits(b->top,0x278,1u<<b->clock_bit,0,1000);
    if(!e)e=cam_wait_bits(b->top,0x338,1u<<(b->clock_bit-2),0,1000);
    if(!e && cam_bus_parent_byte(b)!=b->parent_entry)e=EBUSY;
    if(!e && (cam_rr(b->gate,0xe00)&b->group_gate_mask)!=gates)e=EBUSY;
    if(!e)b->parent_owned=false; /* retain on timeout; never SET twice */
    return e;
}
