/* X9 sensor-only backend. No SENINF/ISP/DMA or x9_camera_platform registration.
 * Board wiring: captured PLJ110 DT, sensor0/1 and wl28681c_hh@35 on i2c12.
 * Register descriptions: OPPO kernel_device_modules-6.12 spmi-mtk-pmif-core.c, i2c-mt65xx.c
 * (mt6991_compat/v2/32-bit, channel offset ZERO in this DT),
 * clk-mt6993{,-peri}.c, pinctrl-mt6993.c, mt6661-regulator.h;
 * OPLUS camera/regulator/wl28681c-regulator.c. No Qualcomm MMIO.
 *
 * CAMTG uses the MT6993 v1 AP HWCCF voter (XPU0 at +0x20000), not direct
 * PDN/PLL writes. Only an unvoted, FENC-off mux may change parent. The common
 * UNIVPLL programming is read-only; hardware autolink owns its wake/sleep.
 * Idle AP I2C may initialize channel0 timing. Disabled, unvoted shared rails
 * may be claimed; live rails are only borrowed at their exact voltage.
 * WL wake touches only GPIO203, never 187/202 or the chip's UVP control.
 * Do NOT route SCP pins, retune PLLs, clear inherited IRQs or reset a bus.
 * Writes are confined to the selected CAMTG/AP vote, per-I2C gate bits,
 * idle AP FIFO transactions, inactive reset/MCLK pins, disabled WL channels
 * and unowned/off shared rails plus the selected VS bit. A journal
 * is retained after failed restoration; never retry in this boot.
 */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);

enum { CAM_I2C_DATA=0x00, CAM_I2C_MASK=0x08, CAM_I2C_STAT=0x0c,
    CAM_I2C_CTRL=0x10, CAM_I2C_LEN=0x14, CAM_I2C_TRANS=0x18,
    CAM_I2C_TIMING=0x20, CAM_I2C_START=0x24, CAM_I2C_EXT=0x28,
    CAM_I2C_LTIMING=0x2c, CAM_I2C_HS=0x30, CAM_I2C_SDA=0x3c,
    CAM_I2C_IO=0x34, CAM_I2C_FIFO_CLR=0x38, CAM_I2C_AUX=0x44,
    CAM_I2C_DIV=0x48, CAM_I2C_MULTI=0x8c, CAM_I2C_MIS=0x90,
    CAM_I2C_ADDR=0x94, CAM_I2C_FIFO=0xf4 };
static const unsigned cam_i2c_saved_regs[] = {
    CAM_I2C_MASK, CAM_I2C_CTRL, CAM_I2C_LEN, CAM_I2C_TRANS,
    CAM_I2C_AUX, CAM_I2C_ADDR, CAM_I2C_TIMING, CAM_I2C_EXT,
    CAM_I2C_LTIMING, CAM_I2C_HS, CAM_I2C_SDA, CAM_I2C_DIV,
    CAM_I2C_IO, CAM_I2C_MIS
};
#define CAM_I2C_SAVED_COUNT (sizeof(cam_i2c_saved_regs)/sizeof(cam_i2c_saved_regs[0]))
struct cam_probe_bus {
    vm_offset_t base, dma, gate, top, univpll, mainpll;
    uint32_t gate_bit, saved[CAM_I2C_SAVED_COUNT], last_status, clock_shift, clock_bit, clock_source;
    uint32_t banks, dma_banks, timing_hz, clock_con1;
    bool owned, gate_owned, clock_saved, wedged, failed_status;
};
struct cam_probe_board {
    uint32_t reset, mclk, scl, sda, wl_mask, wl_count;
    uint32_t wl_channel[3], wl_code[3], wl_delay[3];
    uint32_t clk_off, clk_shift, clk_status_bit, clk_update_bit, clk_group;
    vm_offset_t drive_base;
};
struct cam_probe_pin {
    uint32_t pin, mode, dir, dout;
    bool owned;
};
struct cam_probe_rail {
    unsigned sid, sel_reg, en_reg, da_reg, hw_reg, eint_reg;
    uint8_t target, selector, enable, da;
    bool valid, voltage_owned, enable_owned;
};
struct cam_probe_journal {
    struct cam_probe_bus sensor, ldo;
    struct cam_probe_board board;
    vm_offset_t gpio, vlp, top, hwv, pll, mainpll, pmif_p, pmif_m, pmif_pending;
    uint32_t clk_saved, clk_expected, pll_saved;
    uint32_t drive, mode, dir, dout;
    uint8_t wl_enable, wl_voltage[3];
    uint8_t wl_programmed, wl_enabled;
    struct cam_probe_pin route[4], wake;
    struct cam_probe_rail dovdd, afvdd;
    bool vs_owned, pmif_fault;
    bool pins_owned, wl_owned, mux_owned, vote_owned;
};
static struct cam_probe_journal cam_journal;
static struct x9_camera_probe cam_probe_last = { .version=1 };
static uint32_t cam_probe_done;
static bool cam_probe_latched;

static uint32_t cam_rr(vm_offset_t base, unsigned off)
{ return *(volatile uint32_t *)(base+off); }
static void cam_rw(vm_offset_t base, unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(base+off)=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int cam_update(vm_offset_t base, unsigned off, uint32_t mask, uint32_t value)
{
    cam_rw(base,off,(cam_rr(base,off)&~mask)|(value&mask));
    return (cam_rr(base,off)&mask)==(value&mask)?0:EIO;
}
static int cam_wait_bits(vm_offset_t base, unsigned off, uint32_t mask,
                         uint32_t value, unsigned limit)
{
    for(unsigned i=0;i<limit;i++) {
        if((cam_rr(base,off)&mask)==value)return 0;
        delay(1);
    }
    return ETIMEDOUT;
}
static unsigned cam_vote_off(struct cam_probe_journal *j)
{ return 0x20000+j->board.clk_group*12; }
static unsigned cam_done_off(struct cam_probe_journal *j)
{ return 0x12600+j->board.clk_group*4; }
static unsigned cam_all_off(struct cam_probe_journal *j)
{ return 0x12200+j->board.clk_group*4; }
static uint32_t cam_vote_bit(struct cam_probe_journal *j)
{ return 1u<<(j->board.clk_shift+7); }
static uint32_t cam_clk_byte(struct cam_probe_journal *j)
{ return (cam_rr(j->vlp,j->board.clk_off)>>j->board.clk_shift)&0xff; }
/* clk-pll.c: UNIVPLL has 22-bit PCW, 8 integer bits, postdiv at bit24.
 * This is the programmed nominal rate, NOT a frequency-meter observation.
 * Accept 24MHz +/-1kHz after /104; never repair an incompatible common PLL. */
static bool cam_pll_compatible(uint32_t con1)
{
    uint64_t numerator=(uint64_t)(con1&0x3fffff)*26000000u;
    uint64_t divisor=(uint64_t)16384*104*(1u<<((con1>>24)&7));
    return numerator>=23999000u*divisor && numerator<=24001000u*divisor;
}
/* MUX parent CLR/SET -> UPDATE -> CKSTA clears. clk-mux.c explicitly polls
 * CKSTA == 0: a set bit is BUSY, not clock-present. Never write PDN.
 * Parent changes require every XPU to be unvoted and hardware FENC off.
 * PDN in CLK_CFG is not used as HWCCF's authoritative on/off state. */
static int cam_mux_parent(struct cam_probe_journal *j, unsigned parent, bool take)
{
    uint32_t bit=cam_vote_bit(j), mask=15u<<j->board.clk_shift;
    if((cam_rr(j->hwv,cam_all_off(j))&bit) ||
       (cam_rr(j->hwv,cam_vote_off(j)+8)&bit) ||
       (cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit)))return EBUSY;
    int e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    if(!e)e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(e)return e;
    if((cam_rr(j->hwv,cam_all_off(j))&bit) ||
       (cam_rr(j->hwv,cam_vote_off(j)+8)&bit) ||
       (cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit)))return EBUSY;
    if(take)j->mux_owned=true; /* only after preconditions, before first write */
    cam_rw(j->vlp,j->board.clk_off+8,mask);
    cam_rw(j->vlp,j->board.clk_off+4,parent<<j->board.clk_shift);
    cam_rw(j->vlp,4,1u<<j->board.clk_update_bit);
    e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(!e && (cam_clk_byte(j)&0x7f)!=((j->clk_saved&0x70)|parent))e=EIO;
    return e;
}
/* _v1_ap_hwccf_voter_ctrl: DONE -> SET/CLR -> AP EN -> DONE. Each write is
 * one masked bit; other camera clocks and other XPUs are never cleared.
 * Mark ambiguous SETs owned before issuing them so restore cannot skip one. */
static int cam_clock_vote(struct cam_probe_journal *j, bool on)
{
    uint32_t bit=cam_vote_bit(j);unsigned off=cam_vote_off(j);
    int e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    if(e)return e;
    if(on && (cam_rr(j->hwv,off+8)&bit))return EBUSY; /* not our inherited vote */
    if(on)j->vote_owned=true;
    cam_rw(j->hwv,off+(on?0:4),bit);
    e=cam_wait_bits(j->hwv,off+8,bit,on?bit:0,200);
    if(!e)e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    if(!e && !on)j->vote_owned=false;
    return e;
}
static int cam_clock_take(struct cam_probe_journal *j, struct x9_camera_probe *r)
{
    uint32_t bit=cam_vote_bit(j), ap,all;int e=0;
    r->mclk_cfg=cam_rr(j->vlp,j->board.clk_off);
    r->mclk_status=cam_rr(j->vlp,0x330);
    r->hwv_vote=ap=cam_rr(j->hwv,cam_vote_off(j)+8);
    r->hwv_done=cam_rr(j->hwv,cam_done_off(j));
    r->pll_con1=j->pll_saved=cam_rr(j->pll,0x0c);
    r->pll_fence=cam_rr(j->pll,0x34);
    j->clk_expected=j->clk_saved=cam_clk_byte(j);
    if(!cam_pll_compatible(j->pll_saved))return ENOTSUP;
    e=cam_wait_bits(j->hwv,cam_done_off(j),bit,bit,10000);
    if(!e)e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(e)return e;
    /* Refuse a changing baseline rather than applying a stale snapshot. */
    if(cam_clk_byte(j)!=j->clk_saved || cam_rr(j->hwv,cam_vote_off(j)+8)!=ap)return EBUSY;
    all=cam_rr(j->hwv,cam_all_off(j));
    /* Inherited votes must already select the correct, FENC-on source. */
    if((ap|all)&bit) {
        if((j->clk_saved&15)!=7 ||
           !(cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit)))return EBUSY;
    } else {
        if(cam_rr(j->vlp,0x270)&(1u<<j->board.clk_status_bit))return EBUSY;
        if((j->clk_saved&15)!=7) {
            j->clk_expected=(j->clk_saved&0xf0)|7;
            e=cam_mux_parent(j,7,true);if(e)return e;
        }
    }
    if(!(ap&bit)){e=cam_clock_vote(j,true);if(e)return e;}
    e=cam_wait_bits(j->vlp,0x330,1u<<j->board.clk_status_bit,0,1000);
    if(!e)e=cam_wait_bits(j->vlp,j->board.clk_off,15u<<j->board.clk_shift,
                           7u<<j->board.clk_shift,1000);
    if(!e)e=cam_wait_bits(j->vlp,0x270,1u<<j->board.clk_status_bit,
                           1u<<j->board.clk_status_bit,1000);
    /* PLL_FENC is the hardware-on acknowledgement; prepare() in Linux is
     * software-only. Autolink must produce it; no guessed PLL enable write. */
    if(!e)e=cam_wait_bits(j->pll,0x34,1u<<4,1u<<4,1000);
    r->hwv_done=cam_rr(j->hwv,cam_done_off(j));
    r->pll_fence=cam_rr(j->pll,0x34);
    if(!e && cam_rr(j->pll,0x0c)!=j->pll_saved)e=EBUSY;
    if(!e)r->flags|=X9_CAM_PROBE_CLOCK;
    return e;
}
static int cam_clock_restore(struct cam_probe_journal *j)
{
    int e=0;
    if(j->vote_owned){e=cam_clock_vote(j,false);if(e)return e;}
    if(j->mux_owned) {
        /* A different owner may have voted while we were probing. Do not
         * restore a mux under that owner, even after our vote was removed. */
        e=cam_wait_bits(j->vlp,0x270,1u<<j->board.clk_status_bit,0,1000);
        if(e)return e;
        uint32_t current=cam_clk_byte(j)&0x7f;
        /* CLR succeeded but SET/UPDATE may have failed. Permit our two
         * intermediate values (parent0/7) or the unchanged entry byte,
         * never silently restore an unrelated parent/reserved-bit change. */
        if(current!=(j->clk_saved&0x7f) && current!=(j->clk_expected&0x7f) &&
           current!=(j->clk_saved&0x70))return EBUSY;
        e=cam_mux_parent(j,j->clk_saved&15,false);
        if(!e)j->mux_owned=false;
    }
    return e;
}
static uint32_t cam_pin_mode(vm_offset_t gpio, unsigned pin)
{ return (cam_rr(gpio,0x300+(pin/4)*16)>>((pin%4)*8))&31; }
static int cam_pin_mux(vm_offset_t gpio, unsigned pin, unsigned mode)
{ return cam_update(gpio,0x300+(pin/4)*16,31u<<((pin%4)*8),mode<<((pin%4)*8)); }
static int cam_pin_bit(vm_offset_t gpio, unsigned bank, unsigned pin, unsigned value)
{
    unsigned off=bank+(pin/32)*16; uint32_t bit=1u<<(pin%32);
    cam_rw(gpio,off+(value?4:8),bit);
    return !!(cam_rr(gpio,off)&bit)==!!value?0:EIO;
}
static unsigned cam_pin_get(vm_offset_t gpio, unsigned bank, unsigned pin)
{ return (cam_rr(gpio,bank+(pin/32)*16)>>(pin%32))&1; }
static void cam_pin_snapshot(struct cam_probe_journal *j, struct cam_probe_pin *p, unsigned pin)
{
    p->pin=pin;p->mode=cam_pin_mode(j->gpio,pin);
    p->dir=cam_pin_get(j->gpio,0,pin);p->dout=cam_pin_get(j->gpio,0x100,pin);
}
static int cam_routes_take(struct cam_probe_journal *j)
{
    for(unsigned i=0;i<4;i++) {
        struct cam_probe_pin *p=&j->route[i];
        if(cam_pin_mode(j->gpio,p->pin)!=p->mode || cam_pin_get(j->gpio,0,p->pin)!=p->dir)return EBUSY;
        if(p->mode==1)continue;
        if(p->mode || p->dir)return EBUSY; /* never take mode2/SCP or GPIO output */
        p->owned=true;int e=cam_pin_mux(j->gpio,p->pin,1);if(e)return e;
    }
    return 0;
}
static int cam_routes_restore(struct cam_probe_journal *j)
{
    int first=0;
    for(unsigned i=4;i>0;i--) {
        struct cam_probe_pin *p=&j->route[i-1];if(!p->owned)continue;
        unsigned mode=cam_pin_mode(j->gpio,p->pin);
        int e=(mode!=1 && mode!=p->mode)?EBUSY:cam_pin_mux(j->gpio,p->pin,p->mode);
        if(e && !first)first=e;if(!e)p->owned=false;
    }
    return first;
}
static int cam_wake_take(struct cam_probe_journal *j)
{
    struct cam_probe_pin *p=&j->wake;
    if(cam_pin_mode(j->gpio,p->pin)!=p->mode || cam_pin_get(j->gpio,0,p->pin)!=p->dir ||
       cam_pin_get(j->gpio,0x100,p->pin)!=p->dout)return EBUSY;
    if(p->mode)return EBUSY;
    if(p->dir && p->dout)return 0; /* inherited high: borrow, never reset */
    if(!p->dir && cam_pin_get(j->gpio,0x200,p->pin))return EBUSY;
    p->owned=true;
    int e=cam_pin_bit(j->gpio,0x100,p->pin,0);
    if(!e)e=cam_pin_bit(j->gpio,0,p->pin,1);
    if(!e)e=cam_pin_bit(j->gpio,0x100,p->pin,1);
    if(!e)delay(1000); /* wl28681c probe reset_high -> 1ms -> ID */
    return e;
}
static int cam_bus_idle(struct cam_probe_bus *b)
{
    /* This DT has no ch-offset-i2c: use channel0, not guessed AP+0x10000.
     * Refuse inherited requests on the other exposed channel banks too. */
    for(unsigned channel=0;channel<b->banks;channel++) {
        vm_offset_t c=b->base+channel*0x10000;
        if(cam_rr(c,CAM_I2C_START) || cam_rr(c,CAM_I2C_MASK) ||
           cam_rr(c,CAM_I2C_STAT) || (cam_rr(c,CAM_I2C_FIFO)&31))return EBUSY;
    }
    for(unsigned channel=0;channel<b->dma_banks;channel++)
        if(cam_rr(b->dma,channel*0x10000+0x08)&1)return EBUSY;
    return 0;
}
static int cam_bus_clock(struct cam_probe_bus *b)
{
    /* IMPE uses I2C_EAST, IMPC uses I2C_PERI. Common source/PDN are read-only;
     * clk-mt6993 gate_fenc ops use FENC as the hardware-on indication. */
    int e=cam_wait_bits(b->top,0x338,1u<<(b->clock_bit-2),0,1000);
    if(e)return e;
    if(!(cam_rr(b->top,0x278)&(1u<<b->clock_bit)))return ENOTSUP;
    uint32_t source=(cam_rr(b->top,0x110)>>b->clock_shift)&3;
    if(b->clock_saved && b->clock_source!=source)return EBUSY;
    /* parent0=TCK26M, 1=UNIVPLL/20, 2=MAINPLL/16, 3=UNIVPLL/10.
     * A gate FENC alone does not prove the selected PLL is awake. */
    if(source && !(cam_rr(source==2?b->mainpll:b->univpll,0x34)&(1u<<4)))return ENOTSUP;
    uint32_t con1=source?cam_rr(source==2?b->mainpll:b->univpll,0x0c):0;
    if(b->clock_saved && b->clock_con1!=con1)return EBUSY;
    b->clock_con1=con1;b->clock_source=source;b->clock_saved=true;return 0;
}
/* MT6991-compatible speed_ver=1: high/low products are sample(1..8) *
 * step(1..64). Solve close to 45% duty, [41%,49%), at <=98kHz. Bounded
 * arithmetic search, not a full sample/step Cartesian product. */
static bool cam_timing_factor(unsigned n, unsigned *sample, unsigned *step)
{
    for(unsigned s=1;s<=8;s++)if(n%s==0 && n/s>=1 && n/s<=64) {
        *sample=s;*step=n/s;return true;
    }
    return false;
}
static int cam_bus_init_timing(struct cam_probe_bus *b)
{
    uint64_t hz=26000000;
    if(b->clock_source) {
        uint64_t divisor=(uint64_t)16384*(1u<<((b->clock_con1>>24)&7));
        hz=((uint64_t)(b->clock_con1&0x3fffff)*26000000+divisor-1)/divisor;
        hz/=b->clock_source==1?20:b->clock_source==2?16:10;
    }
    if(hz<1000000 || hz>500000000)return ENOTSUP;
    unsigned div,hs=0,ht=0,ls=0,lt=0,ext=0,total=0;
    bool found=false;
    for(div=1;div<=32 && !found;div++) {
        ext=(unsigned)((hz/div)*5/1000000);
        if(ext>255)continue;
        total=(unsigned)((hz+(uint64_t)div*98000-1)/((uint64_t)div*98000))+1;
        if(total<4 || total>1024)continue;
        unsigned ideal=total*45/100;
        for(unsigned delta=0;delta<=total/2 && !found;delta++) {
            for(unsigned side=0;side<2 && !found;side++) {
                if(side && delta>ideal)continue;
                unsigned high=side?ideal-delta:ideal+delta;
                if(high<2 || high>=total-1 || high*1000<total*410 || high*1000>=total*490)continue;
                found=cam_timing_factor(high,&hs,&ht) && cam_timing_factor(total-high,&ls,&lt);
            }
        }
    }
    if(!found)return ENOTSUP;
    div--; /* loop increments once after the successful divider */
    int e=cam_bus_clock(b);if(!e)e=cam_bus_idle(b);if(e)return e;
    if(cam_rr(b->base,CAM_I2C_MULTI) || cam_rr(b->base,CAM_I2C_TIMING))return EBUSY;
    /* No softreset, MCU interrupt routing, APDMA reset or shared shadow mode.
     * Journal was captured and marked owned before any timing write. */
    const unsigned regs[]={CAM_I2C_TIMING,CAM_I2C_EXT,CAM_I2C_LTIMING,
        CAM_I2C_HS,CAM_I2C_SDA,CAM_I2C_DIV,CAM_I2C_IO,CAM_I2C_MIS};
    const uint32_t values[]={((hs-1)<<8)|(ht-1),(ext<<8)|1,
        ((ls-1)<<6)|(lt-1),0,0,((div-1)<<8)|(div-1),3,0};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        cam_rw(b->base,regs[i],values[i]);
        if(cam_rr(b->base,regs[i])!=values[i])return EIO;
    }
    b->timing_hz=(uint32_t)(hz/(div*total));
    return 0;
}
static int cam_bus_take(struct cam_probe_bus *b)
{
    int e=cam_bus_clock(b);if(e)return e;
    /* Only the per-controller IMPC/IMPE bit; the common I2C parent is not
     * reparented. Journal BEFORE CLR, including an unconfirmed gate write. */
    if(cam_rr(b->gate,0xe00)&b->gate_bit) {
        for(unsigned channel=0;channel<b->dma_banks;channel++)
            if(cam_rr(b->dma,channel*0x10000+0x08)&1)return EBUSY;
        b->gate_owned=true;
        cam_rw(b->gate,0xe04,b->gate_bit);
        e=cam_wait_bits(b->gate,0xe00,b->gate_bit,0,1000);if(e)return e;
    }
    e=cam_bus_idle(b);if(e)return e;
    for(unsigned i=0;i<CAM_I2C_SAVED_COUNT;i++)b->saved[i]=cam_rr(b->base,cam_i2c_saved_regs[i]);
    /* DMA, IRQ routing and wrapper state are not inherited silently. */
    if(b->saved[1]&((1u<<2)|(1u<<7)|(1u<<8)|(1u<<9)|1u))return ENOTSUP;
    if(cam_rr(b->base,CAM_I2C_MULTI))return ENOTSUP; /* no shared shadow/DMA mode */
    b->owned=true;
    if(!b->saved[6])return cam_bus_init_timing(b);
    if((cam_rr(b->base,CAM_I2C_HS)&0x8001) || cam_rr(b->base,CAM_I2C_IO)!=3)return ENOTSUP;
    return 0;
}
static int cam_bus_restore(struct cam_probe_bus *b)
{
    if(!b->owned && !b->gate_owned)return 0;
    int e=cam_bus_clock(b);if(e)return e;
    if(b->wedged || cam_bus_idle(b))return EBUSY;
    if(b->owned) {
        for(unsigned i=0;i<CAM_I2C_SAVED_COUNT;i++) {
            cam_rw(b->base,cam_i2c_saved_regs[i],b->saved[i]);
            if(cam_rr(b->base,cam_i2c_saved_regs[i])!=b->saved[i])return EIO;
        }
        b->owned=false;
    }
    if(b->gate_owned) {
        cam_rw(b->gate,0xe08,b->gate_bit);
        e=cam_wait_bits(b->gate,0xe00,b->gate_bit,b->gate_bit,1000);if(e)return e;
        b->gate_owned=false;
    }
    return 0;
}
/* WRRD: one/two register-address bytes followed by ONE data byte.
 * FIFO_DATA is byte-accessed in the upstream driver even with 32-bit CSR.
 * No DMA, retries, broad slave scan, shared reset or inherited W1C. */
static int cam_bus_transfer(struct cam_probe_bus *b, uint8_t slave,
                            const uint8_t *tx, unsigned count, uint8_t *rx)
{
    if(!b->owned || b->wedged || !count || count>3)return EINVAL;
    int e=cam_bus_clock(b);if(!e)e=cam_bus_idle(b);if(e)return e;
    uint32_t control=(b->saved[1]&~((1u<<1)|(1u<<2)|(1u<<4)|(1u<<8)|(1u<<9)))|
        (1u<<3)|(1u<<5);
    if(rx)control|=(1u<<1)|(1u<<4);
    cam_rw(b->base,CAM_I2C_CTRL,control);
    cam_rw(b->base,CAM_I2C_MASK,0);
    cam_rw(b->base,CAM_I2C_ADDR,(uint32_t)slave<<1);
    cam_rw(b->base,CAM_I2C_FIFO_CLR,1);
    cam_rw(b->base,CAM_I2C_LEN,count);
    cam_rw(b->base,CAM_I2C_AUX,rx?1:0);
    cam_rw(b->base,CAM_I2C_TRANS,rx?2:1);
    for(unsigned i=0;i<count;i++)*(volatile uint8_t *)(b->base+CAM_I2C_DATA)=tx[i];
    __asm__ volatile("dsb sy" ::: "memory");
    cam_rw(b->base,CAM_I2C_START,(1u<<14)|1u);
    uint32_t status=0;
    for(unsigned i=0;i<1000;i++) {
        status=cam_rr(b->base,CAM_I2C_STAT);
        if(status&(0x200|0xf))break;
        delay(5);
    }
    if(!b->failed_status)b->last_status=status;
    if(!status || (cam_rr(b->base,CAM_I2C_START)&1)) {
        b->wedged=b->failed_status=true;return ETIMEDOUT; /* retain journal, no global reset */
    }
    if(status&~0x207u)e=EIO; /* arbitration/conferr/unknown are not ACKs */
    else if(status&0x206)e=EIO;
    else if(!(status&1))e=EIO;
    if(!e && rx) {
        if((cam_rr(b->base,CAM_I2C_FIFO)&31)!=1)e=EIO;
        else *rx=*(volatile uint8_t *)(b->base+CAM_I2C_DATA);
    }
    /* Clear only status produced by THIS request. Not an inherited IRQ. */
    cam_rw(b->base,CAM_I2C_STAT,status&0x20f);
    cam_rw(b->base,CAM_I2C_FIFO_CLR,1);
    cam_rw(b->base,CAM_I2C_START,0);
    if(cam_bus_idle(b)){b->wedged=true;if(!e)e=EIO;}
    if(e)b->failed_status=true;
    return e;
}
static int cam_wl_read(struct cam_probe_journal *j, uint8_t reg, uint8_t *v)
{ return cam_bus_transfer(&j->ldo,0x35,&reg,1,v); }
static int cam_wl_write(struct cam_probe_journal *j, uint8_t reg, uint8_t value)
{
    bool voltage=false;
    for(unsigned i=0;i<j->board.wl_count;i++)
        if(reg==3+j->board.wl_channel[i])voltage=true;
    if(reg!=3 && !voltage)return EPERM;
    uint8_t enable=0;int e=cam_wl_read(j,3,&enable);if(e)return e;
    if(reg==3) {
        if((enable&~j->board.wl_mask)!=(value&~j->board.wl_mask))return EBUSY;
    } else if(enable&(1u<<(reg-4)))return EBUSY; /* no live voltage change */
    uint8_t tx[2]={reg,value},back=0;
    e=cam_bus_transfer(&j->ldo,0x35,tx,2,NULL);
    if(!e)e=cam_wl_read(j,reg,&back);
    return e?e:back==value?0:EIO;
}
static int cam_pmif_wait(vm_offset_t bus, unsigned expected)
{
    for(unsigned i=0;i<1000;i++) {
        uint32_t s=cam_rr(bus,0x8a8);
        if(s&(1u<<18))return EIO;
        if(((s>>1)&7)==expected)return 0;
        delay(1);
    }
    return ETIMEDOUT;
}
/* PMIF SWINF2 is serialized with every other X9 PMIC consumer. Rail
 * snapshot -> ownership checks -> voltage/SET/CLR -> readback hold ONE lock.
 * A late read may be drained only before releasing that lock. */
static int cam_pmic_xfer_locked(struct cam_probe_journal *j, unsigned sid,
                                unsigned reg, uint8_t *v, bool write)
{
    if((sid!=3 && sid!=6) || reg>0xffff)return EPERM;
    if(write && !((sid==3 && (reg==0x24a ||
        ((reg==0x2048 || reg==0x2049) && *v==1) ||
        ((reg==0x178b || reg==0x178c) && *v==4))) ||
        (sid==6 && (reg==0x1e61 || ((reg==0x1f08 || reg==0x1f09) && *v==1)))))return EPERM;
    if(j->pmif_fault)return EIO;
    vm_offset_t bus=sid==3?j->pmif_p:j->pmif_m;
    if(!bus || !(cam_rr(bus,0)&1))return ENXIO;
    int e=cam_pmif_wait(bus,0);if(e)return e;
    j->pmif_pending=bus;
    if(write)cam_rw(bus,0x884,*v);
    cam_rw(bus,0x880,(3u<<30)|(write?1u<<29:0)|(sid<<24)|reg);
    e=cam_pmif_wait(bus,write?0:6);
    if(!e && !write) {
        *v=(uint8_t)cam_rr(bus,0x894);cam_rw(bus,0x8a4,1);
        e=cam_pmif_wait(bus,0);
    }
    if(e && !write) {
        uint32_t state=cam_rr(bus,0x8a8);
        if(!(state&(1u<<18)) && ((state>>1)&7)==6) {
            (void)cam_rr(bus,0x894);cam_rw(bus,0x8a4,1);
            if(!cam_pmif_wait(bus,0))j->pmif_pending=0;
        }
    }
    if(!e)j->pmif_pending=0;
    if(j->pmif_pending)j->pmif_fault=true;
    return e;
}
static int cam_pmic_r(struct cam_probe_journal *j, unsigned sid, unsigned reg, uint8_t *v)
{ return cam_pmic_xfer_locked(j,sid,reg,v,false); }
static int cam_pmic_w(struct cam_probe_journal *j, unsigned sid, unsigned reg, uint8_t v)
{ return cam_pmic_xfer_locked(j,sid,reg,&v,true); }
static int cam_rail_state(struct cam_probe_journal *j, struct cam_probe_rail *p,
                          uint8_t *sel, uint8_t *en, uint8_t *da)
{
    int e=cam_pmic_r(j,p->sid,p->sel_reg,sel);
    if(!e)e=cam_pmic_r(j,p->sid,p->en_reg,en);
    if(!e)e=cam_pmic_r(j,p->sid,p->da_reg,da);
    return e;
}
static int cam_rail_no_hw(struct cam_probe_journal *j, struct cam_probe_rail *p)
{
    uint8_t hw=0,eint=0;int e=cam_pmic_r(j,p->sid,p->hw_reg,&hw);
    if(!e && p->eint_reg)e=cam_pmic_r(j,p->sid,p->eint_reg,&eint);
    return e?e:(hw || eint)?EBUSY:0;
}
static int cam_rail_snapshot(struct cam_probe_journal *j, struct cam_probe_rail *p,
                             uint32_t *evidence)
{
    int e=cam_rail_state(j,p,&p->selector,&p->enable,&p->da);
    *evidence=p->selector|((uint32_t)p->enable<<8)|((uint32_t)p->da<<16);
    if(e)return e;
    if((p->enable&2) || (p->da&4))return ENOTSUP;
    if(p->enable&1) {
        if(p->selector!=p->target || !(p->da&1))return ENOTSUP;
    } else {
        if(p->da&1)return EBUSY;
        e=cam_rail_no_hw(j,p);if(e)return e;
    }
    p->valid=true;return 0;
}
static int cam_shared_rails(struct cam_probe_journal *j, struct x9_camera_probe *r)
{
    /* mt6661_volt_range_ops encodes coarse in HIGH nibble and fine in LOW:
     * LN60_7 1.8V is 0xd0, not 0x0d. LDC0_3 table selector10 is 3.0V. */
    j->dovdd=(struct cam_probe_rail){.sid=3,.sel_reg=0x24a,.en_reg=0x2047,
        .da_reg=0x2062,.hw_reg=0x206f,.target=0xd0};
    j->afvdd=(struct cam_probe_rail){.sid=6,.sel_reg=0x1e61,.en_reg=0x1f07,
        .da_reg=0x1f14,.hw_reg=0x1f20,.eint_reg=0x1f2f,.target=0x0a};
    int e=X9PMIFAcquire()?EBUSY:0;if(e)return e;
    e=cam_rail_snapshot(j,&j->dovdd,&r->dovdd);
    if(!e && r->sensor==X9_CAMERA_BACK)e=cam_rail_snapshot(j,&j->afvdd,&r->afvdd);
    X9PMIFRelease();if(e)r->blockers|=X9_CAM_BLOCK_RAIL;return e;
}
static int cam_rail_wait_da(struct cam_probe_journal *j, struct cam_probe_rail *p, bool on)
{
    for(unsigned i=0;i<20;i++) {
        uint8_t da=0;int e=cam_pmic_r(j,p->sid,p->da_reg,&da);if(e)return e;
        if((da&5)==(on?1:0))return 0;
        delay(100);
    }
    return ETIMEDOUT;
}
static int cam_rail_take_locked(struct cam_probe_journal *j, struct cam_probe_rail *p)
{
    if(!p->valid)return EINVAL;
    uint8_t sel=0,en=0,da=0;int e=cam_rail_state(j,p,&sel,&en,&da);if(e)return e;
    if(sel!=p->selector || en!=p->enable || da!=p->da)return EBUSY;
    if(en&1)return 0; /* borrowed: never change voltage/mode/enable */
    e=cam_rail_no_hw(j,p);if(e)return e;
    if(sel!=p->target) {
        p->voltage_owned=true;
        e=cam_pmic_w(j,p->sid,p->sel_reg,p->target);
        if(!e)e=cam_pmic_r(j,p->sid,p->sel_reg,&sel);
        if(!e && sel!=p->target)e=EIO;
    }
    if(!e) {
        p->enable_owned=true; /* before possibly ambiguous SET */
        e=cam_pmic_w(j,p->sid,p->en_reg+1,1);
        if(!e)e=cam_pmic_r(j,p->sid,p->en_reg,&en);
        if(!e && en!=(p->enable|1))e=EIO;
        if(!e)e=cam_rail_wait_da(j,p,true);
    }
    return e;
}
static int cam_rail_take(struct cam_probe_journal *j, struct cam_probe_rail *p)
{
    if(X9PMIFAcquire())return EBUSY;
    int e=cam_rail_take_locked(j,p);X9PMIFRelease();return e;
}
static int cam_rail_restore_locked(struct cam_probe_journal *j, struct cam_probe_rail *p)
{
    if(!p->enable_owned && !p->voltage_owned)return 0;
    uint8_t sel=0,en=0,da=0;int e=cam_rail_state(j,p,&sel,&en,&da);
    if(!e)e=cam_rail_no_hw(j,p);
    if(e)return e;
    if((en&~1)!=p->enable || (da&4) || (sel!=p->target && sel!=p->selector))return EBUSY;
    if(p->enable_owned) {
        e=cam_pmic_w(j,p->sid,p->en_reg+2,1);
        if(!e)e=cam_pmic_r(j,p->sid,p->en_reg,&en);
        if(!e && en!=p->enable)e=EIO;
        if(!e)e=cam_rail_wait_da(j,p,false);
        if(e)return e;
        p->enable_owned=false;
    } else if((en&1) || (da&1))return EBUSY;
    if(p->voltage_owned) {
        e=cam_pmic_w(j,p->sid,p->sel_reg,p->selector);
        if(!e)e=cam_pmic_r(j,p->sid,p->sel_reg,&sel);
        if(!e && sel!=p->selector)e=EIO;
        if(!e)p->voltage_owned=false;
    }
    return e;
}
static int cam_rail_restore(struct cam_probe_journal *j, struct cam_probe_rail *p)
{
    if(!p->enable_owned && !p->voltage_owned)return 0;
    if(X9PMIFAcquire())return EBUSY;
    int e=cam_rail_restore_locked(j,p);X9PMIFRelease();return e;
}
static int cam_vs_vote(struct cam_probe_journal *j, bool on)
{
    if(!on && !j->vs_owned)return 0;
    if(X9PMIFAcquire())return EBUSY;
    uint8_t en=0;int e=cam_pmic_r(j,3,0x178a,&en);
    if(!e && (on?!(en&4):true)) {
        if(on)j->vs_owned=true;
        e=cam_pmic_w(j,3,on?0x178b:0x178c,4);
        if(!e)e=cam_pmic_r(j,3,0x178a,&en);
        if(!e && !!(en&4)!=on)e=EIO;
        if(!e && !on)j->vs_owned=false;
    }
    X9PMIFRelease();return e;
}
static int cam_wl_restore_channel(struct cam_probe_journal *j, unsigned i)
{
    uint8_t bit=(uint8_t)(1u<<(j->board.wl_channel[i]-1)),current=0;
    int e=0;
    if(j->wl_enabled&bit) {
        e=cam_wl_read(j,3,&current);
        if(!e)e=cam_wl_write(j,3,(uint8_t)(current&~bit));
        if(e)return e;
        j->wl_enabled&=~bit;delay(150);
    }
    if(j->wl_programmed&bit) {
        uint8_t voltage=0;
        e=cam_wl_read(j,(uint8_t)(3+j->board.wl_channel[i]),&voltage);
        if(e)return e;
        if(voltage!=j->wl_voltage[i] && voltage!=j->board.wl_code[i])return EBUSY;
        e=cam_wl_write(j,(uint8_t)(3+j->board.wl_channel[i]),j->wl_voltage[i]);
        if(!e)j->wl_programmed&=~bit;
    }
    j->wl_owned=!!(j->wl_enabled|j->wl_programmed);return e;
}
static int cam_wake_restore(struct cam_probe_journal *j)
{
    struct cam_probe_pin *p=&j->wake;if(!p->owned)return 0;
    uint8_t enable=0;int e=cam_wl_read(j,3,&enable);if(e)return e;
    if(enable || j->wl_owned)return EBUSY; /* ALL channels, not only our mask */
    if(cam_pin_mode(j->gpio,p->pin)!=0)return EBUSY;
    e=cam_pin_bit(j->gpio,0x100,p->pin,0);
    if(!e)e=cam_pin_bit(j->gpio,0,p->pin,p->dir);
    if(!e)e=cam_pin_bit(j->gpio,0x100,p->pin,p->dout);
    if(!e)p->owned=false;
    return e;
}
static int cam_restore(struct cam_probe_journal *j)
{
    int first=j->pmif_fault?EIO:0,e,pin_error=0;
    /* After releasing PMIF lock never clear a later response as ours. */
    if(j->pmif_pending)first=EIO;
    if(j->pins_owned) {
        e=cam_pin_bit(j->gpio,0x100,j->board.reset,0);if(e)pin_error=e;
        e=cam_pin_mux(j->gpio,j->board.mclk,0);if(e && !pin_error)pin_error=e;
        delay(1000);
    }
    /* Reverse actual board power order, including the interleaved shared
     * supplies. A failed rail/channel keeps its journal and all prerequisites. */
    if(j->board.wl_count==3) {
        e=cam_rail_restore(j,&j->dovdd);if(e && !first)first=e;
    }
    for(unsigned i=j->board.wl_count;i>0;i--) {
        e=cam_wl_restore_channel(j,i-1);if(e && !first)first=e;
        if(j->board.wl_count==3 && i==3) {
            e=cam_rail_restore(j,&j->afvdd);if(e && !first)first=e;
        }
        if(j->board.wl_count==2 && i==2) {
            e=cam_rail_restore(j,&j->dovdd);if(e && !first)first=e;
        }
    }
    e=cam_wake_restore(j);if(e && !first)first=e;
    if(j->pins_owned) {
        e=cam_update(j->board.drive_base,0,7,j->drive);if(e && !pin_error)pin_error=e;
        /* Restore input direction BEFORE a possibly high inherited DOUT
         * latch. Otherwise cleanup would pulse reset high on an input pin. */
        e=cam_pin_bit(j->gpio,0,j->board.reset,j->dir);if(e && !pin_error)pin_error=e;
        e=cam_pin_bit(j->gpio,0x100,j->board.reset,j->dout);if(e && !pin_error)pin_error=e;
        e=cam_pin_mux(j->gpio,j->board.reset,j->mode);if(e && !pin_error)pin_error=e;
        if(!pin_error)j->pins_owned=false;
    }
    if(pin_error && !first)first=pin_error;
    e=cam_bus_restore(&j->sensor);if(e && !first)first=e;
    /* A failed WL shutdown retains its bus gate/timing/pins for diagnostics;
     * do not gate a still-enabled supply controller or undo its wake. */
    if(!j->wl_owned && !j->wake.owned) {
        e=cam_bus_restore(&j->ldo);if(e && !first)first=e;
    }
    if(!j->sensor.owned && !j->ldo.owned) {
        e=cam_routes_restore(j);if(e && !first)first=e;
    }
    if(!j->pins_owned) {
        e=cam_clock_restore(j);if(e && !first)first=e;
    }
    if(!first && !j->wl_owned && !j->wake.owned &&
       !j->dovdd.enable_owned && !j->dovdd.voltage_owned &&
       !j->afvdd.enable_owned && !j->afvdd.voltage_owned) {
        e=cam_vs_vote(j,false);if(e)first=e;
    }
    return first;
}
static int cam_board_prepare(struct cam_probe_journal *j, uint32_t which,
                              struct x9_camera_probe *r)
{
    memset(j,0,sizeof(*j));
    bool back=which==X9_CAMERA_BACK;
    j->board=(struct cam_probe_board){
        .reset=back?171:170, .mclk=back?208:72,
        .scl=back?174:172, .sda=back?175:173,
        .wl_mask=back?0x4a:0x05, .wl_count=back?3:2,
        .wl_channel={back?4u:3u,back?7u:1u,2},
        /* WL PLDO=1.372V+code*8mV; NLDO=0.496V+(code-0x3d)*8mV. */
        .wl_code={back?179u:191u,back?54u:137u,144},
        .wl_delay={back?3000u:1000u,back?3000u:1000u,4000},
        .clk_off=back?0x70:0x60, .clk_shift=back?24:8,
        .clk_status_bit=back?4:10, .clk_update_bit=back?27:21,
        .clk_group=back?6:5
    };
    j->gpio=ml_io_map(0x1002d000,0x1000);
    j->vlp=ml_io_map(0x1c200000,0x1000);
    j->top=ml_io_map(0x1c210000,0x1000);
    j->hwv=ml_io_map(0x1c040000,0x75000);
    j->pll=ml_io_map(0x1c213100,0x100);
    j->mainpll=ml_io_map(0x1c213000,0x100);
    j->pmif_p=ml_io_map(0x1c018000,0x2000);
    j->pmif_m=back?ml_io_map(0x1c01a000,0x2000):0;
    j->board.drive_base=ml_io_map(back?0x13800000:0x13040000,0x1000);
    j->sensor=(struct cam_probe_bus){
        .base=ml_io_map(back?0x12450000:0x123d0000,0x80000),
        .dma=ml_io_map(back?0x163d0000:0x16390000,0x30000),.banks=8,.dma_banks=3,
        .gate=ml_io_map(0x12650000,0x1000),.gate_bit=back?4:2,
        .top=j->top,.univpll=j->pll,.mainpll=j->mainpll,.clock_shift=16,.clock_bit=29};
    j->ldo=(struct cam_probe_bus){.base=ml_io_map(0x16200000,0x40000),
        .dma=ml_io_map(0x164f0000,0x10000),.banks=4,.dma_banks=1,
        .gate=ml_io_map(0x16300000,0x1000),.gate_bit=1,
        .top=j->top,.univpll=j->pll,.mainpll=j->mainpll,.clock_shift=0,.clock_bit=31};
    if(!j->gpio || !j->vlp || !j->top || !j->hwv || !j->pll || !j->mainpll || !j->pmif_p || (back && !j->pmif_m) ||
       !j->board.drive_base || !j->sensor.base || !j->sensor.dma || !j->sensor.gate ||
       !j->ldo.base || !j->ldo.dma || !j->ldo.gate)return ENOMEM;
    r->mclk_cfg=cam_rr(j->vlp,j->board.clk_off);
    r->mclk_status=cam_rr(j->vlp,0x330);
    const unsigned route[]={j->board.scl,j->board.sda,162,163};
    for(unsigned i=0;i<4;i++) {
        cam_pin_snapshot(j,&j->route[i],route[i]);
        if(j->route[i].mode!=1 && (j->route[i].mode || j->route[i].dir))r->blockers|=X9_CAM_BLOCK_ROUTE;
    }
    j->mode=cam_pin_mode(j->gpio,j->board.reset);
    j->dir=(cam_rr(j->gpio,(j->board.reset/32)*16)>>(j->board.reset%32))&1;
    j->dout=(cam_rr(j->gpio,0x100+(j->board.reset/32)*16)>>(j->board.reset%32))&1;
    j->drive=cam_rr(j->board.drive_base,0)&7;
    cam_pin_snapshot(j,&j->wake,203);
    if(j->mode || (j->dir?j->dout:cam_pin_get(j->gpio,0x200,j->board.reset)) ||
       cam_pin_mode(j->gpio,j->board.mclk) || j->wake.mode ||
       (!j->wake.dir && cam_pin_get(j->gpio,0x200,203)))r->blockers|=X9_CAM_BLOCK_INHERITED;
    if(r->blockers)return ENOTSUP;
    r->stage=X9_CAM_STAGE_PMIC;
    int e=cam_shared_rails(j,r);if(e)return e;
    r->stage=X9_CAM_STAGE_CLOCK;
    e=cam_clock_take(j,r);
    if(e){r->blockers|=X9_CAM_BLOCK_CLOCK;return e;}
    r->stage=X9_CAM_STAGE_BUS;
    e=cam_routes_take(j);if(e){r->blockers|=X9_CAM_BLOCK_ROUTE;return e;}
    e=cam_bus_take(&j->sensor);if(!e)e=cam_bus_take(&j->ldo);
    if(e)r->blockers|=X9_CAM_BLOCK_I2C;
    return e;
}
/* Optional configuration runs only after matched ID, before the SAME cold
 * journal is restored. Probe wrapper below never writes sensor registers. */
static int cam_sensor_transaction(uint32_t which, struct x9_camera_probe *r,
    int (*configure)(struct cam_probe_journal *, void *), void *context)
{
    memset(r,0,sizeof(*r));r->version=1;r->sensor=which;
    if(cam_probe_latched || (cam_probe_done&(1u<<which))) {
        r->last_errno=cam_probe_latched?EIO:EALREADY;
        if(cam_probe_latched)r->flags=X9_CAM_PROBE_LATCHED;
        return r->last_errno;
    }
    r->flags=X9_CAM_PROBE_ATTEMPTED;r->stage=X9_CAM_STAGE_PREFLIGHT;
    struct cam_probe_journal *j=&cam_journal;
    int e=cam_board_prepare(j,which,r);
    uint8_t v=0;
    if(!e) {
        r->stage=X9_CAM_STAGE_LDO;
        e=cam_wake_take(j);
        if(!e)e=cam_wl_read(j,0,&v);r->wl_id=v;
        if(!e && v!=0x0d)e=ENODEV;
        if(!e)e=cam_wl_read(j,3,&j->wl_enable);
        r->wl_enable=j->wl_enable;
        if(!e && (j->wl_enable&j->board.wl_mask)) {
            r->blockers|=X9_CAM_BLOCK_INHERITED;e=EBUSY;
        }
        for(unsigned i=0;i<j->board.wl_count && !e;i++)
            e=cam_wl_read(j,(uint8_t)(3+j->board.wl_channel[i]),&j->wl_voltage[i]);
    }
    if(!e) {
        r->stage=X9_CAM_STAGE_POWER;
        if(which==X9_CAMERA_BACK)e=cam_vs_vote(j,true);
        if(!e)j->pins_owned=true;
        if(!e)e=cam_pin_bit(j->gpio,0x100,j->board.reset,0);
        if(!e)e=cam_pin_mux(j->gpio,j->board.reset,0);
        if(!e)e=cam_pin_bit(j->gpio,0,j->board.reset,1);
        delay(1000);
        /* Journal before writes: an ambiguous write must still be unwound. */
        for(unsigned i=0;i<j->board.wl_count && !e;i++) {
            uint8_t en=0,bit=(uint8_t)(1u<<(j->board.wl_channel[i]-1));
            j->wl_owned=true;j->wl_programmed|=bit;
            e=cam_wl_write(j,(uint8_t)(3+j->board.wl_channel[i]),(uint8_t)j->board.wl_code[i]);
            if(!e)e=cam_wl_read(j,3,&en);
            if(!e && (en&bit))e=EBUSY;
            if(!e){j->wl_enabled|=bit;e=cam_wl_write(j,3,(uint8_t)(en|bit));}
            if(!e)delay(j->board.wl_delay[i]);
            if(!e && which==X9_CAMERA_BACK && i==1) {
                e=cam_rail_take(j,&j->afvdd);if(!e)delay(3000);
            }
            if(!e && which==X9_CAMERA_FRONT && i==0) {
                e=cam_rail_take(j,&j->dovdd);if(!e)delay(1000);
            }
        }
        if(!e && which==X9_CAMERA_BACK) {
            e=cam_rail_take(j,&j->dovdd);if(!e)delay(3000);
        }
        if(!e)e=cam_update(j->board.drive_base,0,7,1); /* DT encoded 4mA */
        if(!e)e=cam_pin_mux(j->gpio,j->board.mclk,1);
        if(!e)delay(which==X9_CAMERA_BACK?6000:1000);
        if(!e)e=cam_pin_bit(j->gpio,0x100,j->board.reset,1);
        if(!e)delay(4000);
    }
    if(!e) {
        r->stage=X9_CAM_STAGE_ID;uint8_t hi=0,lo=0;
        const uint8_t high[2]={0,0x16},low[2]={0,0x17};
        uint8_t slave=which==X9_CAMERA_BACK?0x1a:0x10;
        e=cam_bus_transfer(&j->sensor,slave,high,2,&hi);
        if(!e)e=cam_bus_transfer(&j->sensor,slave,low,2,&lo);
        if(!e)r->chip_id=((uint32_t)hi<<8)|lo;
        if(!e && r->chip_id!=(which==X9_CAMERA_BACK?0x9186:0x0615))e=ENODEV;
        if(!e)r->flags|=X9_CAM_PROBE_MATCHED;
    }
    if(!e && configure){r->stage=X9_CAM_STAGE_CONFIG;e=configure(j,context);}
    r->i2c_status[0]=j->sensor.last_status;
    r->i2c_status[1]=j->ldo.last_status;
    /* Keep the failed decisive stage. DONE means ID matched AND restored. */
    int off=cam_restore(j);r->restore_errno=off;
    if(!off)r->flags|=X9_CAM_PROBE_RESTORED;
    r->last_errno=e?e:off;
    if(r->last_errno) {
        cam_probe_latched=true;r->flags|=X9_CAM_PROBE_LATCHED;
        if(!e)r->stage=X9_CAM_STAGE_RESTORE;
    } else {cam_probe_done|=1u<<which;r->stage=X9_CAM_STAGE_DONE;}
    cam_probe_last=*r;
    printf("X9 CAMERA PROBE: sensor=%u stage=%u blockers=%x chip=%04x flags=%x errno=%u restore=%u (no capture)\n",
        which,r->stage,r->blockers,r->chip_id,r->flags,r->last_errno,r->restore_errno);
    printf("X9 CAMERA CLOCK: entry cfg=%08x busy=%08x ap=%08x done=%08x pll=%08x fence=%08x (configured nominal, no frequency measurement)\n",
        r->mclk_cfg,r->mclk_status,r->hwv_vote,r->hwv_done,r->pll_con1,r->pll_fence);
    printf("X9 CAMERA COLD: timing_hz=%u/%u entry dovdd=%06x afvdd=%06x retained wl=%x/%x wake=%u rails=%u/%u vs=%u pmif=%u (no CSI/DMA)\n",
        j->sensor.timing_hz,j->ldo.timing_hz,r->dovdd,r->afvdd,j->wl_enabled,j->wl_programmed,
        j->wake.owned,j->dovdd.enable_owned||j->dovdd.voltage_owned,
        j->afvdd.enable_owned||j->afvdd.voltage_owned,j->vs_owned,j->pmif_fault);
    return r->last_errno;
}
static int cam_sensor_probe(uint32_t which, struct x9_camera_probe *r)
{ return cam_sensor_transaction(which,r,NULL,NULL); }
