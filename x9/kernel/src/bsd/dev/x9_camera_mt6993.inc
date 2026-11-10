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
 * AP I2C may ungate its own bit, but still requires inherited timing.
 * Do NOT wake/reset WL (shared GPIO203/187/202), retune PLL/PMIC rails,
 * route SCP pins, acknowledge inherited IRQs, or reset a wedged controller.
 * Shared-power/AP timing providers remain required for a full cold LK boot.
 * Writes are confined to the selected CAMTG/AP vote, per-I2C gate bits,
 * idle AP FIFO transactions, inactive reset/MCLK pins and disabled WL channels. A journal
 * is retained after failed restoration; never retry in this boot.
 */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);

enum { CAM_I2C_DATA=0x00, CAM_I2C_MASK=0x08, CAM_I2C_STAT=0x0c,
    CAM_I2C_CTRL=0x10, CAM_I2C_LEN=0x14, CAM_I2C_TRANS=0x18,
    CAM_I2C_TIMING=0x20, CAM_I2C_START=0x24, CAM_I2C_HS=0x30,
    CAM_I2C_IO=0x34, CAM_I2C_FIFO_CLR=0x38, CAM_I2C_AUX=0x44,
    CAM_I2C_ADDR=0x94, CAM_I2C_FIFO=0xf4 };
static const unsigned cam_i2c_saved_regs[] = {
    CAM_I2C_MASK, CAM_I2C_CTRL, CAM_I2C_LEN, CAM_I2C_TRANS,
    CAM_I2C_AUX, CAM_I2C_ADDR
};
struct cam_probe_bus {
    vm_offset_t base, dma, gate, top, univpll, mainpll;
    uint32_t gate_bit, saved[6], last_status, clock_shift, clock_bit, clock_source;
    bool owned, gate_owned, clock_saved, wedged, failed_status;
};
struct cam_probe_board {
    uint32_t reset, mclk, scl, sda, wl_mask, wl_count;
    uint32_t wl_channel[3], wl_code[3], wl_delay[3];
    uint32_t clk_off, clk_shift, clk_status_bit, clk_update_bit, clk_group;
    vm_offset_t drive_base;
};
struct cam_probe_journal {
    struct cam_probe_bus sensor, ldo;
    struct cam_probe_board board;
    vm_offset_t gpio, vlp, top, hwv, pll, mainpll, pmif_p, pmif_m, pmif_pending;
    uint32_t clk_saved, clk_expected, pll_saved;
    uint32_t drive, mode, dir, dout;
    uint8_t wl_enable, wl_voltage[3];
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
static int cam_bus_idle(struct cam_probe_bus *b)
{
    /* This DT has no ch-offset-i2c: use channel0, not guessed AP+0x10000.
     * Refuse inherited requests on the other exposed channel banks too. */
    for(unsigned channel=0;channel<4;channel++) {
        vm_offset_t c=b->base+channel*0x10000;
        if(cam_rr(c,CAM_I2C_START) || cam_rr(c,CAM_I2C_MASK) ||
           cam_rr(c,CAM_I2C_STAT) || (cam_rr(c,CAM_I2C_FIFO)&31))return EBUSY;
    }
    if(cam_rr(b->dma,0x08)&1)return EBUSY;
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
    b->clock_source=source;b->clock_saved=true;return 0;
}
static int cam_bus_take(struct cam_probe_bus *b)
{
    int e=cam_bus_clock(b);if(e)return e;
    /* Only the per-controller IMPC/IMPE bit; the common I2C parent is not
     * reparented. Journal BEFORE CLR, including an unconfirmed gate write. */
    if(cam_rr(b->gate,0xe00)&b->gate_bit) {
        if(cam_rr(b->dma,0x08)&1)return EBUSY;
        b->gate_owned=true;
        cam_rw(b->gate,0xe04,b->gate_bit);
        e=cam_wait_bits(b->gate,0xe00,b->gate_bit,0,1000);if(e)return e;
    }
    if(!cam_rr(b->base,CAM_I2C_TIMING) ||
       (cam_rr(b->base,CAM_I2C_HS)&0x8001) ||
       cam_rr(b->base,CAM_I2C_IO)!=3)return ENOTSUP;
    e=cam_bus_idle(b);if(e)return e;
    for(unsigned i=0;i<6;i++)b->saved[i]=cam_rr(b->base,cam_i2c_saved_regs[i]);
    /* DMA, IRQ routing and wrapper state are not inherited silently. */
    if(b->saved[1]&((1u<<2)|(1u<<7)|(1u<<8)|(1u<<9)|1u))return ENOTSUP;
    b->owned=true;return 0;
}
static int cam_bus_restore(struct cam_probe_bus *b)
{
    if(!b->owned && !b->gate_owned)return 0;
    int e=cam_bus_clock(b);if(e)return e;
    if(b->wedged || cam_bus_idle(b))return EBUSY;
    if(b->owned) {
        for(unsigned i=0;i<6;i++) {
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
/* Read-only shared-rail precondition. DT spmi-dev-mask=0x55a8 routes SID3
 * to PMIF-P, SID6 to PMIF-M. Single X9PMIF lock serializes SWINF2. */
static int cam_pmic_read(struct cam_probe_journal *j, unsigned sid, unsigned reg, uint8_t *v)
{
    if(sid!=3 && sid!=6)return EPERM;
    vm_offset_t bus=sid==3?j->pmif_p:j->pmif_m;
    if(!bus || !(cam_rr(bus,0)&1))return ENXIO;
    int e=X9PMIFAcquire()?EBUSY:0;if(e)return e;
    e=cam_pmif_wait(bus,0);
    if(!e) {
        j->pmif_pending=bus;
        cam_rw(bus,0x880,(3u<<30)|(sid<<24)|reg);
        e=cam_pmif_wait(bus,6);
        if(!e){*v=(uint8_t)cam_rr(bus,0x894);cam_rw(bus,0x8a4,1);e=cam_pmif_wait(bus,0);}
        if(!e)j->pmif_pending=0;
    }
    if(e && j->pmif_pending) {
        uint32_t s=cam_rr(bus,0x8a8);
        if(!(s&(1u<<18)) && ((s>>1)&7)==6) {
            /* Still holding the common lock: this late response is ours. */
            (void)cam_rr(bus,0x894);cam_rw(bus,0x8a4,1);
            if(!cam_pmif_wait(bus,0))j->pmif_pending=0;
        }
    }
    X9PMIFRelease();return e;
}
static int cam_shared_rails(struct cam_probe_journal *j, struct x9_camera_probe *r)
{
    uint8_t sel=0,en=0,da=0;
    int e=cam_pmic_read(j,3,0x24a,&sel);
    if(!e)e=cam_pmic_read(j,3,0x2047,&en);
    if(!e)e=cam_pmic_read(j,3,0x2062,&da);
    r->dovdd=sel|((uint32_t)en<<8)|((uint32_t)da<<16);
    /* LN60_7 0.5V + selector*0.1V, high nibble is fine calibration.
     * Only exact 1.8V / normal mode / DA enabled may be borrowed. */
    if(!e && (sel!=0x0d || !(en&1) || (en&2) || !(da&1) || (da&4)))e=ENOTSUP;
    if(!e && r->sensor==X9_CAMERA_BACK) {
        e=cam_pmic_read(j,6,0x1e61,&sel);
        if(!e)e=cam_pmic_read(j,6,0x1f07,&en);
        if(!e)e=cam_pmic_read(j,6,0x1f14,&da);
        r->afvdd=sel|((uint32_t)en<<8)|((uint32_t)da<<16);
        if(!e && (sel!=0x0a || !(en&1) || (en&2) || !(da&1) || (da&4)))e=ENOTSUP;
    }
    if(e)r->blockers|=X9_CAM_BLOCK_RAIL;return e;
}
static int cam_restore(struct cam_probe_journal *j)
{
    int first=0,e;uint8_t current=0;
    /* After releasing the common PMIF lock we cannot identify a later
     * response as ours. Never clear it or announce restoration by guessing. */
    if(j->pmif_pending)first=EIO;
    /* Reset LOW and detach MCLK even if the LDO bus wedged. Baseline reset
     * was inactive/low, never restore an inherited streaming sensor. */
    if(j->pins_owned) {
        e=cam_pin_bit(j->gpio,0x100,j->board.reset,0);if(e && !first)first=e;
        e=cam_pin_mux(j->gpio,j->board.mclk,0);if(e && !first)first=e;
        delay(1000);
    }
    if(j->wl_owned) {
        e=cam_wl_read(j,3,&current);
        /* Preserve other live channels; only our disabled-at-entry mask. */
        if(!e)e=cam_wl_write(j,3,(uint8_t)(current&~j->board.wl_mask));
        if(e && !first)first=e;
        if(!e) {
            delay(150);
            for(unsigned i=j->board.wl_count;i>0;i--) {
                e=cam_wl_write(j,(uint8_t)(3+j->board.wl_channel[i-1]),j->wl_voltage[i-1]);
                if(e && !first)first=e;
            }
            if(!first)j->wl_owned=false;
        }
    }
    if(j->pins_owned) {
        e=cam_update(j->board.drive_base,0,7,j->drive);if(e && !first)first=e;
        e=cam_pin_bit(j->gpio,0x100,j->board.reset,j->dout);if(e && !first)first=e;
        e=cam_pin_bit(j->gpio,0,j->board.reset,j->dir);if(e && !first)first=e;
        e=cam_pin_mux(j->gpio,j->board.reset,j->mode);if(e && !first)first=e;
        if(!first)j->pins_owned=false;
    }
    e=cam_bus_restore(&j->sensor);if(e && !first)first=e;
    e=cam_bus_restore(&j->ldo);if(e && !first)first=e;
    /* If pin detach failed, keep the clock alive instead of changing the
     * source under an attached sensor. Retain ownership for diagnostics. */
    if(j->pins_owned && first) { if(j->vote_owned || j->mux_owned)return first; }
    e=cam_clock_restore(j);if(e && !first)first=e;
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
        .base=ml_io_map(back?0x12450000:0x123d0000,0x40000),
        .dma=ml_io_map(back?0x163d0000:0x16390000,0x1000),
        .gate=ml_io_map(0x12650000,0x1000),.gate_bit=back?4:2,
        .top=j->top,.univpll=j->pll,.mainpll=j->mainpll,.clock_shift=16,.clock_bit=29};
    j->ldo=(struct cam_probe_bus){.base=ml_io_map(0x16200000,0x40000),
        .dma=ml_io_map(0x164f0000,0x1000),.gate=ml_io_map(0x16300000,0x1000),.gate_bit=1,
        .top=j->top,.univpll=j->pll,.mainpll=j->mainpll,.clock_shift=0,.clock_bit=31};
    if(!j->gpio || !j->vlp || !j->top || !j->hwv || !j->pll || !j->mainpll || !j->pmif_p || (back && !j->pmif_m) ||
       !j->board.drive_base || !j->sensor.base || !j->sensor.dma || !j->sensor.gate ||
       !j->ldo.base || !j->ldo.dma || !j->ldo.gate)return ENOMEM;
    r->mclk_cfg=cam_rr(j->vlp,j->board.clk_off);
    r->mclk_status=cam_rr(j->vlp,0x330);
    if(cam_pin_mode(j->gpio,j->board.scl)!=1 || cam_pin_mode(j->gpio,j->board.sda)!=1 ||
       cam_pin_mode(j->gpio,162)!=1 || cam_pin_mode(j->gpio,163)!=1)r->blockers|=X9_CAM_BLOCK_ROUTE;
    j->mode=cam_pin_mode(j->gpio,j->board.reset);
    j->dir=(cam_rr(j->gpio,(j->board.reset/32)*16)>>(j->board.reset%32))&1;
    j->dout=(cam_rr(j->gpio,0x100+(j->board.reset/32)*16)>>(j->board.reset%32))&1;
    j->drive=cam_rr(j->board.drive_base,0)&7;
    if(j->mode || !j->dir || j->dout || cam_pin_mode(j->gpio,j->board.mclk) ||
       cam_pin_mode(j->gpio,203) || !(cam_rr(j->gpio,0x60)&(1u<<11)) ||
       !(cam_rr(j->gpio,0x160)&(1u<<11)))r->blockers|=X9_CAM_BLOCK_INHERITED;
    if(r->blockers)return ENOTSUP;
    r->stage=X9_CAM_STAGE_PMIC;
    int e=cam_shared_rails(j,r);if(e)return e;
    r->stage=X9_CAM_STAGE_CLOCK;
    e=cam_clock_take(j,r);
    if(e){r->blockers|=X9_CAM_BLOCK_CLOCK;return e;}
    r->stage=X9_CAM_STAGE_BUS;
    e=cam_bus_take(&j->sensor);if(!e)e=cam_bus_take(&j->ldo);
    if(e)r->blockers|=X9_CAM_BLOCK_I2C;
    return e;
}
static int cam_sensor_probe(uint32_t which, struct x9_camera_probe *r)
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
        e=cam_wl_read(j,0,&v);r->wl_id=v;
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
        r->stage=X9_CAM_STAGE_POWER;j->pins_owned=true;
        e=cam_pin_bit(j->gpio,0x100,j->board.reset,0);
        if(!e)e=cam_pin_mux(j->gpio,j->board.reset,0);
        delay(1000);
        /* Journal before writes: an ambiguous write must still be unwound. */
        j->wl_owned=true;
        for(unsigned i=0;i<j->board.wl_count && !e;i++) {
            uint8_t en=0;
            e=cam_wl_write(j,(uint8_t)(3+j->board.wl_channel[i]),(uint8_t)j->board.wl_code[i]);
            if(!e)e=cam_wl_read(j,3,&en);
            if(!e)e=cam_wl_write(j,3,(uint8_t)(en|(1u<<(j->board.wl_channel[i]-1))));
            delay(j->board.wl_delay[i]);
            /* Match the borrowed AFVDD stage between AVDD1 and DVDD. */
            if(which==X9_CAMERA_BACK && i==1)delay(3000);
            if(which==X9_CAMERA_FRONT && i==0)delay(1000); /* borrowed DOVDD before DVDD */
        }
        if(!e && which==X9_CAMERA_BACK)delay(3000); /* borrowed DOVDD */
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
    return r->last_errno;
}
