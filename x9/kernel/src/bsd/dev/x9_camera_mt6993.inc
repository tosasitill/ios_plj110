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
 * Writes are confined to the selected CAMTG/AP vote, guarded I2C parent PDN
 * and per-I2C gate bits,
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
    uint32_t banks, dma_banks, timing_hz, clock_con1, max_hz;
    bool vendor_reset; /* run mtk_i2c_init_hw reset at take (AOV-shared sensor bus) */
    uint32_t expected[CAM_I2C_SAVED_COUNT], prior[CAM_I2C_SAVED_COUNT];
    uint32_t dirty, uncertain;
    uint32_t group_gate_mask, parent_entry, parent_expected;
    bool parent_seen, parent_owned, parent_conflict;
    bool owned, gate_owned, clock_saved, wedged, failed_status, conflict;
    bool fifo_owned, request_owned; /* attempted operations, NOT hardware leases */
    uint32_t multi_entry; /* vendor init_hw SHADOW_REG_MODE etc.: borrowed, unchanged */
};
struct cam_probe_board {
    uint32_t reset, mclk, scl, sda, wl_mask, wl_count, sensor;
    uint32_t wl_channel[3], wl_code[3], wl_delay[3];
    uint32_t clk_off, clk_shift, clk_status_bit, clk_update_bit, clk_group;
    vm_offset_t drive_base;
    uint32_t drive_shift;
};
enum { CAM_PIN_MODE, CAM_PIN_DIR, CAM_PIN_DOUT, CAM_PIN_FIELDS };
struct cam_probe_pin {
    uint32_t pin, mode, dir, dout;
    uint32_t ap_mode; /* route pins only: MT6993 AP I2C function, per pin */
    uint32_t expected[CAM_PIN_FIELDS], prior[CAM_PIN_FIELDS], dirty, uncertain;
    bool valid, taken, owned, conflict;
};
struct cam_probe_drive {
    uint32_t saved, expected, prior, dirty;
    bool valid, taken, uncertain, conflict;
};
struct cam_probe_rail {
    unsigned sid, sel_reg, en_reg, da_reg, hw_reg, eint_reg;
    uint8_t target, selector, enable, da;
    uint8_t selector_expected, selector_prior, enable_expected, enable_prior;
    uint8_t hw, eint;
    bool valid, taken, voltage_owned, enable_owned;
    bool selector_uncertain, enable_uncertain, conflict;
};
struct cam_probe_journal {
    struct cam_probe_bus sensor, ldo;
    struct cam_probe_board board;
    vm_offset_t gpio, vlp, top, hwv, pll, mainpll, pmif_p, pmif_m, pmif_pending;
    uint32_t clk_saved, clk_expected, clk_prior, pll_saved;
    bool clock_seen, clock_active, clock_conflict, mux_uncertain, mux_update_pending;
    bool vote_expected, vote_prior, vote_uncertain;
    struct cam_probe_drive drive;
    uint8_t wl_enable, wl_voltage[3];
    uint8_t wl_programmed, wl_enabled;
    uint8_t wl_enable_expected, wl_enable_prior, wl_enable_uncertain;
    bool wl_tracking, wl_conflict;
    struct cam_probe_pin route[4], wake, reset, mclk;
    struct cam_probe_rail dovdd, afvdd;
    struct cam_probe_rail dvdd; /* UW DVDD/DVDD1 are ONE SID5 provider. */
    uint16_t sensor_page;
    bool page_known, page_pending;
    bool vs_owned, pmif_fault;
    bool pins_owned, wl_owned, mux_owned, vote_owned;
    uint32_t restore_step, restore_channel;
};
static struct cam_probe_journal cam_journal;
static struct x9_camera_probe cam_probe_last = { .version=1 };
static uint32_t cam_probe_done;
static bool cam_probe_latched;
/* camera_control_lock protects this and the shared cold journal. */
static bool cam_sensor_session_active;
static int cam_rails_guard(struct cam_probe_journal *, bool);

static uint32_t cam_rr(vm_offset_t base, unsigned off)
{ return *(volatile uint32_t *)(base+off); }
static void cam_rw(vm_offset_t base, unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(base+off)=value;
    __asm__ volatile("dsb sy" ::: "memory");
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
#include "x9_camera_mclk.inc"
static uint32_t cam_pin_mode(vm_offset_t gpio, unsigned pin)
{ return (cam_rr(gpio,0x300+(pin/4)*16)>>((pin%4)*8))&31; }
static int cam_pin_mux(vm_offset_t gpio, unsigned pin, unsigned mode)
{
    if(mode>15)return EINVAL;
    /* MT6993 FLAG_RACE_FREE_ACCESS: MODE MWR, bit4 is modify-enable.
     * No full-word RMW or transient CLR of another pin's mode. */
    cam_rw(gpio,0x300+(pin/4)*16+12,(16u|mode)<<((pin%4)*8));
    return cam_pin_mode(gpio,pin)==mode?0:EIO;
}
static int cam_pin_bit(vm_offset_t gpio, unsigned bank, unsigned pin, unsigned value)
{
    unsigned off=bank+(pin/32)*16; uint32_t bit=1u<<(pin%32);
    cam_rw(gpio,off+(value?4:8),bit);
    return !!(cam_rr(gpio,off)&bit)==!!value?0:EIO;
}
static unsigned cam_pin_get(vm_offset_t gpio, unsigned bank, unsigned pin)
{ return (cam_rr(gpio,bank+(pin/32)*16)>>(pin%32))&1; }
#include "x9_camera_pins.inc"
static int cam_bus_idle(struct cam_probe_bus *b)
{
    /* This DT has no ch-offset-i2c: use channel0, not guessed AP+0x10000.
     * Refuse inherited requests on the other exposed channel banks too. */
    for(unsigned channel=0;channel<b->banks;channel++) {
        vm_offset_t c=b->base+channel*0x10000;
        /* Vendor i2c-mt65xx: INTR_MASK is per-transfer configuration and
         * stale STAT/FIFO are cleared by init_hw/each transfer; only a
         * running START (bit0) or an enabled APDMA is real activity. */
        if(cam_rr(c,CAM_I2C_START)&1)return EBUSY;
    }
    for(unsigned channel=0;channel<b->dma_banks;channel++)
        if(cam_rr(b->dma,channel*0x10000+0x08)&1)return EBUSY;
    return 0;
}
#include "x9_camera_i2c_clock.inc"
/* Idle/FENC is not ownership of TIMING or transfer controls. Track every
 * writable CSR separately; never overwrite a changed borrowed configuration.
 * The selected AP mutex serializes our callers, not SCP/other hardware users.
 * Only an ambiguous write's exact pre-write value is also admissible during
 * restore. No third value, IRQ clearing, reset or retry can establish a lease. */
static int cam_bus_conflict(struct cam_probe_bus *b, unsigned reg, uint32_t expected,
                             uint32_t observed)
{
    b->conflict=true;
    printf("X9 CAMERA AP CONFLICT: gate=%x reg=%x expected=%08x observed=%08x dirty=%x uncertain=%x (retained, no overwrite)\n",
        b->gate_bit,reg,expected,observed,b->dirty,b->uncertain);
    return EBUSY;
}
static int cam_bus_controls(struct cam_probe_bus *b, bool restoring)
{
    if(b->conflict)return EBUSY;
    if(!restoring && b->uncertain)return EIO;
    int e=cam_pins_guard(&cam_journal,restoring);if(e)return e;
    e=cam_clock_check(&cam_journal,restoring,cam_journal.clock_active);if(e)return e;
    e=cam_bus_clock(b);if(e)return e;
    if(cam_rr(b->gate,0xe00)&b->gate_bit)return EBUSY;
    uint32_t multi=cam_rr(b->base,CAM_I2C_MULTI);
    if(multi!=b->multi_entry)return cam_bus_conflict(b,CAM_I2C_MULTI,b->multi_entry,multi);
    for(unsigned i=0;i<CAM_I2C_SAVED_COUNT;i++) {
        uint32_t v=cam_rr(b->base,cam_i2c_saved_regs[i]);
        if(v==b->expected[i] || (restoring && (b->uncertain&(1u<<i)) && v==b->prior[i]))continue;
        return cam_bus_conflict(b,cam_i2c_saved_regs[i],b->expected[i],v);
    }
    return 0;
}
static int cam_bus_control_write(struct cam_probe_bus *b, unsigned reg, uint32_t value,
                                  bool restoring)
{
    unsigned i=0;while(i<CAM_I2C_SAVED_COUNT && cam_i2c_saved_regs[i]!=reg)i++;
    if(i==CAM_I2C_SAVED_COUNT || !b->owned)return EINVAL;
    int e=cam_bus_controls(b,restoring);if(e)return e;
    uint32_t before=cam_rr(b->base,reg),bit=1u<<i;
    if(before!=b->expected[i] && !(restoring && (b->uncertain&bit) && before==b->prior[i]))
        return cam_bus_conflict(b,reg,b->expected[i],before);
    if(before==value) {
        b->expected[i]=value;b->uncertain&=~bit;
        if(restoring)b->dirty&=~bit;
        return 0;
    }
    b->prior[i]=before;b->expected[i]=value;
    b->dirty|=bit;b->uncertain|=bit; /* BEFORE even an ambiguous write */
    cam_rw(b->base,reg,value);
    uint32_t got=cam_rr(b->base,reg);
    if(got!=value) {
        printf("X9 CAMERA AP WRITE: gate=%x reg=%x before=%x want=%x got=%x\n",
            b->gate_bit,reg,before,value,got);
        return EIO;
    }
    b->uncertain&=~bit;if(restoring)b->dirty&=~bit;
    return 0;
}
#include "x9_camera_i2c_timing.inc"
static int cam_bus_take(struct cam_probe_bus *b)
{
    int e=cam_bus_parent_take(b);if(e)return e;
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
    if(b->vendor_reset) {
        /* Vendor mtk_i2c_init_hw (mt6991: apdma_sync, dma_ver 2) runs on
         * every runtime resume, i.e. right before the first Android sensor
         * ID read, and after each ACK error. Log 335/336: powered, clocked
         * and muxed exactly like the live Android state, yet a clean NACK
         * on the AOV-shared i2c4 (MULTI=4380). Reset APDMA ch0+controller. */
        uint32_t m0=cam_rr(b->base,CAM_I2C_MULTI),t0=cam_rr(b->base,CAM_I2C_TIMING);
        cam_rw(b->base,CAM_I2C_START,0);
        uint32_t st=cam_rr(b->base,CAM_I2C_STAT);if(st)cam_rw(b->base,CAM_I2C_STAT,st);
        cam_rw(b->dma,0x0c,1);delay(2);           /* I2C_DMA_WARM_RST */
        cam_rw(b->dma,0x0c,0);delay(2);           /* I2C_DMA_CLR_FLAG */
        cam_rw(b->dma,0x0c,4|2);                  /* HANDSHAKE_RST|HARD_RST */
        cam_rw(b->base,0x50,0x20|1);delay(2);     /* I2C_HANDSHAKE_RST|I2C_SOFT_RST */
        cam_rw(b->dma,0x0c,0);
        cam_rw(b->base,0x50,0);
        __asm__ volatile("dsb sy" ::: "memory");
        cam_rw(b->dma,0x1c,0x1000);               /* I2C_DMA_TX_MEM_ADDR_INIT_VALUE */
        cam_rw(b->base,0x1c,2);                   /* I2C_DELAY_LEN */
        printf("X9 CAMERA AP RESET: gate=%x multi=%x->%x timing=%x->%x stat=%x start=%x delay=%x (vendor init_hw)\n",
            b->gate_bit,m0,cam_rr(b->base,CAM_I2C_MULTI),t0,cam_rr(b->base,CAM_I2C_TIMING),
            cam_rr(b->base,CAM_I2C_STAT),cam_rr(b->base,CAM_I2C_START),cam_rr(b->base,0x1c));
        e=cam_bus_idle(b);if(e)return e;
    }
    /* Like vendor mtk_i2c_init_hw: W1C whatever INTR_STAT a previous
     * owner (LK RT5133/WL28681C) left, and clear the FIFO pointers. */
    uint32_t stale=cam_rr(b->base,CAM_I2C_STAT);
    if(stale)cam_rw(b->base,CAM_I2C_STAT,stale);
    cam_rw(b->base,CAM_I2C_FIFO_CLR,1);
    if(cam_rr(b->base,CAM_I2C_STAT) || (cam_rr(b->base,CAM_I2C_FIFO)&31))return EBUSY;
    b->multi_entry=cam_rr(b->base,CAM_I2C_MULTI);
    for(unsigned i=0;i<CAM_I2C_SAVED_COUNT;i++)b->saved[i]=cam_rr(b->base,cam_i2c_saved_regs[i]);
    memcpy(b->expected,b->saved,sizeof(b->saved));
    printf("X9 CAMERA AP ENTRY: gate=%x stale_stat=%x multi=%x ctrl=%x mask=%x timing=%x ext=%x ltiming=%x hs=%x sda=%x div=%x io=%x mis=%x\n",
        b->gate_bit,stale,b->multi_entry,b->saved[1],b->saved[0],b->saved[6],b->saved[7],
        b->saved[8],b->saved[9],b->saved[10],b->saved[11],b->saved[12],b->saved[13]);
    e=cam_bus_controls(b,false);if(e)return e; /* reject an incoherent snapshot */
    /* CONTROL is rewritten per transfer exactly like vendor (keep bits
     * 0/6/7, clear RS/DMA/DIR/DMAACK/ASYNC); MULTI shadow mode is borrowed. */
    b->owned=true;
    if(!b->saved[6])return cam_bus_init_timing(b);
    if(!cam_bus_timing_check(b,false))return 0;
    /* i2c12 (WL28681C/RT5133): Android runs it at DT 400 kHz with the
     * timing LK/vendor programmed; borrow that instead of retiming. */
    if(b->max_hz>100000 && !cam_bus_timing_borrow(b))return 0;
    /* Vendor init_hw always reprograms the whole timing set (TIMING/EXT/
     * LTIMING/HS/SDA/DIV/IO/MIS) on an idle bus; do the same with our
     * standard profile. The journal restores the inherited values. */
    return cam_bus_init_timing(b);
}
static bool cam_bus_unretired(const struct cam_probe_bus *b)
{
    return b->fifo_owned || b->request_owned || b->wedged ||
        b->conflict || b->parent_conflict;
}
/* Called after local reset/MCLK isolation, BEFORE any supply teardown.
 * An idle snapshot is not a lease, but a hot journal may never be bypassed
 * by the owned/gate flags or by a subsequently cleared START bit. */
static int cam_bus_supply_guard(struct cam_probe_bus *b)
{
    if(cam_bus_unretired(b))return EBUSY;
    if(!b->owned)return 0; /* no complete control snapshot to validate */
    int e=cam_bus_controls(b,true);
    if(!e)e=cam_bus_idle(b);
    if(e)b->wedged=true; /* preserve an unknown active/configuration state */
    return e;
}
static int cam_bus_restore(struct cam_probe_bus *b)
{
    if(cam_bus_unretired(b))return EBUSY;
    if(!b->owned && !b->gate_owned)return 0;
    int e=cam_bus_clock(b);if(e)return e;
    if(cam_bus_idle(b))return EBUSY;
    if(b->owned) {
        e=cam_bus_controls(b,true);if(e)return e;
        for(unsigned n=CAM_I2C_SAVED_COUNT;n>0;n--) {
            unsigned i=n-1;if(!(b->dirty&(1u<<i)))continue;
            e=cam_bus_control_write(b,cam_i2c_saved_regs[i],b->saved[i],true);
            if(e)return e;
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
#include "x9_camera_i2c_xfer.inc"
static int cam_wl_read(struct cam_probe_journal *j, uint8_t reg, uint8_t *v)
{ return cam_bus_transfer(&j->ldo,0x35,&reg,1,v); }
#include "x9_camera_wl.inc"
static int cam_wl_write(struct cam_probe_journal *j, uint8_t reg, uint8_t value)
{
    bool voltage=false;
    for(unsigned i=0;i<j->board.wl_count;i++)
        if(reg==3+j->board.wl_channel[i])voltage=true;
    /* ENABLE is an explicit single-channel operation, never a caller's
     * stale whole-byte value. This helper only programs disabled selectors. */
    if(!voltage)return EPERM;
    uint8_t enable=0;int e=cam_wl_guard(j,false,&enable);if(e)return e;
    if(enable&(1u<<(reg-4)))return EBUSY; /* no live voltage change */
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
    if((sid!=3 && sid!=6 && !(sid==5 && j->board.sensor==X9_CAMERA_ULTRAWIDE)) || reg>0xffff)return EPERM;
    bool uw_rail=j->board.sensor==X9_CAMERA_ULTRAWIDE && sid==5 &&
        (reg==j->dvdd.sel_reg || reg==j->afvdd.sel_reg ||
         ((reg==j->dvdd.en_reg+1 || reg==j->dvdd.en_reg+2 ||
           reg==j->afvdd.en_reg+1 || reg==j->afvdd.en_reg+2) && *v==1));
    if(write && !(uw_rail || (j->board.sensor==X9_CAMERA_TELEPHOTO && sid==j->afvdd.sid &&
        (reg==j->afvdd.sel_reg || ((reg==j->afvdd.en_reg+1 || reg==j->afvdd.en_reg+2) && *v==1))) || (sid==3 && (reg==0x24a ||
        ((reg==0x2048 || reg==0x2049) && *v==1) ||
        ((reg==0x178b || reg==0x178c) && *v==4) || reg==0x244 || reg==0x1687)) ||
        (sid==6 && (reg==0x1e61 || ((reg==0x1f08 || reg==0x1f09) && *v==1)))))return EPERM;
    if(j->pmif_fault)return EIO;
    /* Captured spmi-dev-mask55a8: SID3/5 on P, SID6 on M. */
    vm_offset_t bus=(sid==3 || sid==5)?j->pmif_p:j->pmif_m;
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
#include "x9_camera_rails.inc"
static int cam_vs_vote(struct cam_probe_journal *j, bool on)
{
    if(!on && !j->vs_owned)return 0;
    int e=cam_pins_guard(j,!on);if(e)return e;
    if(X9PMIFAcquire())return EBUSY;
    uint8_t en=0;e=cam_pmic_r(j,3,0x178a,&en);
    if(!e && (on?!(en&4):true)) {
        if(on)j->vs_owned=true;
        e=cam_pmic_w(j,3,on?0x178b:0x178c,4);
        if(!e)e=cam_pmic_r(j,3,0x178a,&en);
        if(!e && !!(en&4)!=on)e=EIO;
        if(!e && !on)j->vs_owned=false;
    }
    if(!e && on) {
        /* DT mt6661_3_buck5 is regulator-always-on: Linux enables it at boot
         * and never disables it. Android live (adb regmap 0-03), camera off
         * AND on: EN 0x1687=01, DA 0x16c5=03, VOSEL 0x244=be. sensor0's
         * vs-voter (178a bit2) only stacks on that SW enable. BUCK5 feeds the
         * WL28681C NLDO (DVDD) input. Logs 335-341: EEPROM/AF (DOVDD/AFVDD)
         * ACK, only the sensor is silent, MCLK measured 24 MHz, WL regs = Android.
         * Like Android, enable it and never turn it off. */
        uint8_t ben=0,bda=0,bvs=0;
        e=cam_pmic_r(j,3,0x1687,&ben);
        if(!e)e=cam_pmic_r(j,3,0x16c5,&bda);
        if(!e)e=cam_pmic_r(j,3,0x244,&bvs);
        uint8_t en0=ben,da0=bda,vs0=bvs;
        if(!e && !(ben&1)) {
            if(bvs!=0xbe){e=cam_pmic_w(j,3,0x244,0xbe);if(!e)e=cam_pmic_r(j,3,0x244,&bvs);}
            if(!e)e=cam_pmic_w(j,3,0x1687,(uint8_t)(ben|1));
            for(unsigned n=0;!e && n<20;n++) {
                delay(100);
                e=cam_pmic_r(j,3,0x16c5,&bda);
                if(!e && (bda&1))break;
            }
            if(!e)e=cam_pmic_r(j,3,0x1687,&ben);
            if(!e && (!(ben&1) || !(bda&1)))e=EIO;
            if(!e)delay(200); /* regulator-enable-ramp-delay 180 us */
        }
        printf("X9 CAMERA BUCK5: en=%02x->%02x da=%02x->%02x vosel=%02x->%02x errno=%d (Android always-on: 01/03/be)\n",
            en0,ben,da0,bda,vs0,bvs,e);
    }
    X9PMIFRelease();return e;
}
static int cam_wl_restore_channel(struct cam_probe_journal *j, unsigned i)
{
    uint8_t bit=(uint8_t)(1u<<(j->board.wl_channel[i]-1));
    int e=0;
    if(j->wl_enabled&bit) {
        e=cam_wl_enable_change(j,i,false);
        if(e)return e;
        delay(150);
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
    struct cam_probe_pin *p=&j->wake;
    if(!p->taken && !p->owned && !p->conflict)return 0;
    int e=cam_pins_guard(j,true);if(e)return e;
    /* Borrowed output-high never receives a write and does not require
     * unrelated WL channels to be off just to retire a reference. */
    if(p->owned || p->dirty) {
        uint8_t enable=0;e=cam_wl_read(j,3,&enable);if(e)return e;
        if((enable&j->board.wl_mask) || j->wl_owned)return EBUSY;
        /* Log 335: ENABLE keeps non-LDO bit7 (entry c8 = ours|0x80), so an
         * all-zero requirement always failed. Vendor wl28681c probe selects
         * reset_high once and never drives it low (GPIO203 is also RT5133
         * enable). Match Android: leave it output-high, retire the journal. */
        printf("X9 CAMERA WAKE: pin=%u kept high like Android (enable=%02x)\n",p->pin,enable);
        p->taken=p->owned=false;p->dirty=0;p->uncertain=0;
        return 0;
    }
    return cam_pin_restore(j,p);
}
static bool cam_pin_retained(const struct cam_probe_pin *p)
{ return p->taken || p->owned || p->dirty || p->uncertain || p->conflict; }
static uint32_t cam_cold_retained(const struct cam_probe_journal *j)
{
    uint32_t mask=0;
    if(j->pins_owned || cam_pin_retained(&j->reset) || cam_pin_retained(&j->mclk) ||
       j->drive.taken || j->drive.dirty || j->drive.uncertain || j->drive.conflict)mask|=X9_CAM_COLD_PINS;
    if(j->mux_owned || j->vote_owned || j->clock_conflict || j->mux_uncertain ||
       j->vote_uncertain || j->mux_update_pending)mask|=X9_CAM_COLD_MCLK;
    if(j->sensor.owned || j->sensor.gate_owned || cam_bus_unretired(&j->sensor))mask|=X9_CAM_COLD_SENSOR_BUS;
    if(j->ldo.owned || j->ldo.gate_owned || cam_bus_unretired(&j->ldo))mask|=X9_CAM_COLD_WL_BUS;
    if(j->wl_owned || j->wl_enabled || j->wl_programmed)mask|=X9_CAM_COLD_WL_RAILS;
    if(j->dovdd.taken || j->dovdd.enable_owned || j->dovdd.voltage_owned)mask|=X9_CAM_COLD_DOVDD;
    if(j->afvdd.taken || j->afvdd.enable_owned || j->afvdd.voltage_owned)mask|=X9_CAM_COLD_AFVDD;
    if(j->dvdd.taken || j->dvdd.enable_owned || j->dvdd.voltage_owned ||
       j->dvdd.conflict || j->dvdd.selector_uncertain || j->dvdd.enable_uncertain)mask|=X9_CAM_COLD_DVDD;
    if(j->vs_owned)mask|=X9_CAM_COLD_VS;
    if(cam_pin_retained(&j->wake))mask|=X9_CAM_COLD_WAKE;
    for(unsigned i=0;i<4;i++)if(cam_pin_retained(&j->route[i]))mask|=X9_CAM_COLD_ROUTES;
    if(j->pmif_fault || j->pmif_pending)mask|=X9_CAM_COLD_PMIF;
    if(j->sensor.parent_owned)mask|=X9_CAM_COLD_SENSOR_PARENT;
    if(j->ldo.parent_owned)mask|=X9_CAM_COLD_WL_PARENT;
    return mask;
}
static int cam_restore_failed(struct cam_probe_journal *j, int e)
{
    printf("X9 CAMERA COLD RESTORE: step=%u channel=%u errno=%d retained=%x (journal, no retry)\n",
        j->restore_step,j->restore_channel,e,cam_cold_retained(j));
    return e;
}
/* Isolate and qualify every consumed sensor dependency WITHOUT lowering a
 * rail, AP parent or MM power reference. Paired receiver close uses this
 * exact prefix before releasing its own clock lifetime. Repeating the
 * verified isolation in cam_restore is idempotent, not a fault retry. */
static int cam_cold_isolate(struct cam_probe_journal *j)
{
    int e;j->restore_channel=0;j->restore_step=X9_CAM_COLD_QUIESCE;
    e=cam_pins_isolate(j);if(e)return cam_restore_failed(j,e);
    /* Includes borrowed routes/wake and the selected drive field. A low
     * DOUT latch alone never proves an output reset after DIR changed. */
    j->restore_step=X9_CAM_COLD_RESTORE_PINS;
    e=cam_pins_guard(j,true);if(e)return cam_restore_failed(j,e);
    /* After releasing PMIF lock never clear a later response as ours. */
    if(j->pmif_pending || j->pmif_fault) {
        j->restore_step=X9_CAM_COLD_RESTORE_PMIF;return cam_restore_failed(j,EIO);
    }
    j->restore_step=X9_CAM_COLD_RESTORE_CLOCK;
    e=cam_clock_check(j,true,j->clock_active);if(e)return cam_restore_failed(j,e);
    /* AP requests must be retired before lowering DOVDD/AFVDD/WL, not
     * first checked in cam_bus_restore after the supplies have gone away.
     * No W1C, FIFO clear, START stop, reset or polling to retire a fault. */
    j->restore_step=X9_CAM_COLD_RESTORE_SENSOR_BUS;
    e=cam_bus_supply_guard(&j->sensor);if(e)return cam_restore_failed(j,e);
    j->restore_step=X9_CAM_COLD_RESTORE_WL_BUS;
    e=cam_bus_supply_guard(&j->ldo);if(e)return cam_restore_failed(j,e);
    /* Validate ALL consumed PMIC rails before lowering even the first WL
     * channel. Front DOVDD and main AFVDD occur later in reverse order. */
    j->restore_step=X9_CAM_COLD_RESTORE_DOVDD;
    e=cam_rails_guard(j,true);
    if(e) {
        if(j->afvdd.conflict && !j->dovdd.conflict)j->restore_step=X9_CAM_COLD_RESTORE_AFVDD;
        return cam_restore_failed(j,e);
    }
    /* A changed live WL channel must be caught BEFORE interleaved DOVDD/
     * AFVDD are lowered, not only later when undoing its selector. */
    if(j->wl_conflict || j->wl_enabled || j->wl_programmed) {
        uint8_t enable=0;j->restore_step=X9_CAM_COLD_RESTORE_WL;
        e=cam_wl_guard(j,true,&enable);if(e)return cam_restore_failed(j,e);
    }
    return 0;
}
static int cam_restore(struct cam_probe_journal *j)
{
    int e=cam_cold_isolate(j);if(e)return e;
    if(j->board.sensor==X9_CAMERA_ULTRAWIDE) {
        j->restore_step=X9_CAM_COLD_RESTORE_AFVDD;
        e=cam_rail_restore(j,&j->afvdd);if(e)return cam_restore_failed(j,e);
        j->restore_step=X9_CAM_COLD_RESTORE_DVDD;
        e=cam_rail_restore(j,&j->dvdd);if(e)return cam_restore_failed(j,e);
    }
    /* Reverse actual board power order, including the interleaved shared
     * supplies. A failed rail/channel keeps its journal and all prerequisites. */
    if(j->board.sensor==X9_CAMERA_TELEPHOTO) {
        j->restore_step=X9_CAM_COLD_RESTORE_AFVDD;
        e=cam_rail_restore(j,&j->afvdd);if(e)return cam_restore_failed(j,e);
        j->restore_step=X9_CAM_COLD_RESTORE_DOVDD;
        e=cam_rail_restore(j,&j->dovdd);if(e)return cam_restore_failed(j,e);
    }
    if(j->board.wl_count==3) {
        j->restore_step=X9_CAM_COLD_RESTORE_DOVDD;
        e=cam_rail_restore(j,&j->dovdd);if(e)return cam_restore_failed(j,e);
    }
    for(unsigned i=j->board.wl_count;i>0;i--) {
        j->restore_step=X9_CAM_COLD_RESTORE_WL;j->restore_channel=j->board.wl_channel[i-1];
        e=cam_wl_restore_channel(j,i-1);if(e)return cam_restore_failed(j,e);
        j->restore_channel=0;
        if(j->board.wl_count==3 && i==3) {
            j->restore_step=X9_CAM_COLD_RESTORE_AFVDD;
            e=cam_rail_restore(j,&j->afvdd);if(e)return cam_restore_failed(j,e);
        }
        if(j->board.sensor==X9_CAMERA_FRONT && i==2) {
            j->restore_step=X9_CAM_COLD_RESTORE_DOVDD;
            e=cam_rail_restore(j,&j->dovdd);if(e)return cam_restore_failed(j,e);
        }
    }
    if(j->board.sensor==X9_CAMERA_ULTRAWIDE) {
        j->restore_step=X9_CAM_COLD_RESTORE_DOVDD;
        e=cam_rail_restore(j,&j->dovdd);if(e)return cam_restore_failed(j,e);
    }
    j->restore_step=X9_CAM_COLD_RESTORE_WAKE;
    e=cam_wake_restore(j);if(e)return cam_restore_failed(j,e);
    j->restore_step=X9_CAM_COLD_RESTORE_PINS;
    e=cam_pins_restore(j);if(e)return cam_restore_failed(j,e);
    j->restore_step=X9_CAM_COLD_RESTORE_SENSOR_BUS;
    e=cam_bus_restore(&j->sensor);if(e)return cam_restore_failed(j,e);
    /* A failed WL shutdown retains its bus gate/timing/pins for diagnostics;
     * do not gate a still-enabled supply controller or undo its wake. */
    if(!j->wl_owned && !j->wake.owned) {
        j->restore_step=X9_CAM_COLD_RESTORE_WL_BUS;
        e=cam_bus_restore(&j->ldo);if(e)return cam_restore_failed(j,e);
    }
    j->restore_step=X9_CAM_COLD_RESTORE_ROUTES;
    e=cam_routes_restore(j);if(e)return cam_restore_failed(j,e);
    /* Restore both child buses and AP pins BEFORE removing either common
     * parent. A failed child/parent restore retains CAMTG/PLL and VS. */
    j->restore_step=X9_CAM_COLD_RESTORE_WL_PARENT;
    e=cam_bus_parent_restore(&j->ldo);if(e)return cam_restore_failed(j,e);
    j->restore_step=X9_CAM_COLD_RESTORE_SENSOR_PARENT;
    e=cam_bus_parent_restore(&j->sensor);if(e)return cam_restore_failed(j,e);
    j->restore_step=X9_CAM_COLD_RESTORE_CLOCK;
    e=cam_clock_restore(j);if(e)return cam_restore_failed(j,e);
    j->restore_step=X9_CAM_COLD_RESTORE_VS;
    e=cam_vs_vote(j,false);if(e)return cam_restore_failed(j,e);
    if(cam_cold_retained(j))return cam_restore_failed(j,EIO);
    j->restore_step=X9_CAM_COLD_RESTORE_DONE;return 0;
}
/* IO mappings have boot lifetime; resetting a cold transaction journal does
 * not release ml_io_map allocations. Reuse the exact board apertures across
 * native probe/open/front-back transitions, including a partially failed map
 * admission. All callers already hold camera_control_lock. Never cache rail,
 * pin, FIFO or ownership observations with these virtual addresses. */
static struct {
    vm_offset_t pa, va;
    vm_size_t size;
} cam_board_maps[32];
static vm_offset_t cam_board_map(vm_offset_t pa, vm_size_t size)
{
    unsigned empty=32;
    for(unsigned i=0;i<32;i++) {
        if(cam_board_maps[i].pa==pa && cam_board_maps[i].size==size)
            return cam_board_maps[i].va;
        if(!cam_board_maps[i].va && empty==32)empty=i;
    }
    if(empty==32)return 0;
    vm_offset_t va=ml_io_map(pa,size);
    if(va) {
        cam_board_maps[empty].pa=pa;cam_board_maps[empty].size=size;
        cam_board_maps[empty].va=va;
    }
    return va;
}
static int cam_board_prepare(struct cam_probe_journal *j, uint32_t which,
                              struct x9_camera_probe *r)
{
    memset(j,0,sizeof(*j));
    if(!x9_camera_sensor_valid(which))return EINVAL;
    bool back=which==X9_CAMERA_BACK,tele=which==X9_CAMERA_TELEPHOTO,uw=which==X9_CAMERA_ULTRAWIDE;
    j->board=(struct cam_probe_board){
        .sensor=which, .reset=uw?205:tele?189:back?171:170, .mclk=uw?192:tele?188:back?208:72,
        .scl=uw?200:tele?190:back?174:172, .sda=uw?201:tele?191:back?175:173,
        .wl_mask=uw?0x20:tele?0x11:back?0x4a:0x05, .wl_count=uw?1:back?3:2,
        .wl_channel={uw?6u:tele?5u:back?4u:3u,back?7u:1u,2},
        /* WL PLDO=1.372V+code*8mV; NLDO=0.496V+(code-0x3d)*8mV. */
        .wl_code={uw?104u:tele?179u:back?179u:191u,tele?137u:back?54u:137u,144},
        .wl_delay={uw?1000u:tele?3000u:back?3000u:1000u,tele?4000u:back?3000u:1000u,4000},
        /* Captured mclk ID23/pin208 is CAMTG4, not CAMTG7. */
        /* Live DT mclk: back 23=CAMTG4, tele 21=CAMTG2, UW 22=CAMTG3,
         * front 19=CAMTG0 (GPIO72 CMMCLK0), not CAMTG1. clk-mt6993.c:
         * CAMTG0 lsb0/pdn7, cksta/fenc 11, update shift 20. */
        .clk_off=back?0x70:0x60, .clk_shift=uw?24:tele?16:back?0:0,
        .clk_status_bit=uw?8:tele?9:back?7:11, .clk_update_bit=uw?23:tele?22:back?24:20,
        .clk_group=back?6:5,.drive_shift=uw?3:0
    };
    j->gpio=cam_board_map(0x1002d000,0x1000);
    j->vlp=cam_board_map(0x1c200000,0x1000);
    j->top=cam_board_map(0x1c210000,0x1000);
    j->hwv=cam_board_map(0x1c040000,0x75000);
    j->pll=cam_board_map(0x1c213100,0x100);
    j->mainpll=cam_board_map(0x1c213000,0x100);
    j->pmif_p=cam_board_map(0x1c018000,0x2000);
    j->pmif_m=(back||tele)?cam_board_map(0x1c01a000,0x2000):0;
    j->board.drive_base=cam_board_map((tele||uw)?0x12060000:back?0x13800000:0x13040000,0x1000);
    j->sensor=(struct cam_probe_bus){
        .base=cam_board_map(uw?0x139d0000:tele?0x124d0000:back?0x12450000:0x123d0000,0x80000),
        .dma=cam_board_map(uw?0x16480000:tele?0x16420000:back?0x163d0000:0x16390000,0x30000),.banks=8,.dma_banks=3,
        .gate=cam_board_map(uw?0x13a50000:0x12650000,0x1000),.gate_bit=tele?8:back?4:2,
        .top=j->top,.univpll=j->pll,.mainpll=j->mainpll,.clock_shift=uw?8:16,.clock_bit=uw?30:29,
        .group_gate_mask=uw?0x03:0x3f,.vendor_reset=true};
    j->ldo=(struct cam_probe_bus){.max_hz=400000, /* live DT i2c12 clock-frequency */
        .base=cam_board_map(0x16200000,0x40000),
        .dma=cam_board_map(0x164f0000,0x10000),.banks=4,.dma_banks=1,
        .gate=cam_board_map(0x16300000,0x1000),.gate_bit=1,
        .top=j->top,.univpll=j->pll,.mainpll=j->mainpll,.clock_shift=0,.clock_bit=31,
        .group_gate_mask=0x0d};
    if(!j->gpio || !j->vlp || !j->top || !j->hwv || !j->pll || !j->mainpll || !j->pmif_p || ((back||tele) && !j->pmif_m) ||
       !j->board.drive_base || !j->sensor.base || !j->sensor.dma || !j->sensor.gate ||
       !j->ldo.base || !j->ldo.dma || !j->ldo.gate)return ENOMEM;
    r->mclk_cfg=cam_rr(j->vlp,j->board.clk_off);
    r->mclk_status=cam_rr(j->vlp,0x330);
    /* mt6993-pinfunc.h: sensor SCL/SDA (172-175,190/191,200/201) are AP
     * SCLn/SDAn in mode1, but GPIO162/163 mode1 is SCP_SCL2/SDA2 and AP
     * SCL12/SDA12 (WL28681C + RT5133 bus i2c@16200000) is mode2. Live DT
     * drives the only alternative SDA12 pin, GPIO187, as a GPIO in the
     * WL28681C reset group, so I2C12 must be 162/163 mode2. */
    const unsigned route[]={j->board.scl,j->board.sda,162,163};
    const unsigned route_mode[]={1,1,2,2};
    for(unsigned i=0;i<4;i++) {
        cam_pin_snapshot(j,&j->route[i],route[i]);
        j->route[i].ap_mode=route_mode[i];
        if(j->route[i].mode!=route_mode[i] && (j->route[i].mode || j->route[i].dir))
            r->blockers|=X9_CAM_BLOCK_ROUTE;
    }
    cam_pin_snapshot(j,&j->reset,j->board.reset);
    cam_pin_snapshot(j,&j->mclk,j->board.mclk);
    j->drive.saved=j->drive.expected=j->drive.prior=(cam_rr(j->board.drive_base,0)>>j->board.drive_shift)&7;
    j->drive.valid=true;
    cam_pin_snapshot(j,&j->wake,203);
    if(j->reset.mode || (j->reset.dir?j->reset.dout:cam_pin_get(j->gpio,0x200,j->board.reset)) ||
       j->mclk.mode || j->wake.mode ||
       (!j->wake.dir && cam_pin_get(j->gpio,0x200,203)))r->blockers|=X9_CAM_BLOCK_INHERITED;
    struct cam_probe_pin *pins[]={&j->reset,&j->mclk,&j->wake,
        &j->route[0],&j->route[1],&j->route[2],&j->route[3]};
    for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++) {
        if(cam_pin_check(j,pins[i],false))r->blockers|=i<3?X9_CAM_BLOCK_INHERITED:X9_CAM_BLOCK_ROUTE;
    }
    if(cam_drive_check(j,false))r->blockers|=X9_CAM_BLOCK_INHERITED;
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
/* Shared cold power engine: leaves a matched sensor and its exact journal
 * powered on. The session layer owns admission, lifetime and ONE unwind.
 * No CSI/SMMU/DMA dependency is established here. */
/* Log 340 diagnostic (user-approved): vendor clk-fmeter-mt6993.c
 * cksys_vlp_fqmtr()+fqmtr_cal(CKMTR_VLP, cali 0, div 1, load 0x3ff).
 * VLP 0x1c200000: FENC mon 0x270, FQMTR DBGMUX sel 0x344[22:16];
 * CKMTR 0x1c201000 CON0..CON6 = +0x00..+0x18. Only debug meter registers
 * are written and all are restored. Returns kHz, 0 on timeout,
 * 0xffffffff if the clock's FENC is off. */
static uint32_t cam_fmeter_vlp(unsigned id, unsigned fenc_bit)
{
    static vm_offset_t vlp,mtr;
    if(!vlp)vlp=ml_io_map(0x1c200000,0x1000);
    if(!mtr)mtr=ml_io_map(0x1c201000,0x1000);
    if(!vlp || !mtr)return 0;
    unsigned n=0;
    while(!(cam_rr(vlp,0x270)&(1u<<fenc_bit))){if(++n>1000)return 0xffffffffu;}
    uint32_t dbg=cam_rr(vlp,0x344);
    cam_rw(vlp,0x344,dbg&~(0x7fu<<16));
    cam_rw(vlp,0x344,(dbg&~(0x7fu<<16))|(id<<16));
    uint32_t c0=cam_rr(mtr,0),c1=cam_rr(mtr,4),c2=cam_rr(mtr,8),c3=cam_rr(mtr,0xc);
    uint32_t freq=0;
    cam_rw(mtr,0xc,c3&~1u);
    for(n=0;(cam_rr(mtr,0x18)&1) && n<2000;n++)delay(1);
    if(!(cam_rr(mtr,0x18)&1)) {
        cam_rw(mtr,0,cam_rr(mtr,0)&~((1u<<15)|(1u<<8)|(1u<<4)));
        cam_rw(mtr,0,cam_rr(mtr,0)|(1u<<15)|(1u<<12));
        cam_rw(mtr,0,(cam_rr(mtr,0)&0xffffu)|(0x410u<<16));
        cam_rw(mtr,4,cam_rr(mtr,4)&~(0x3ffu<<16));
        cam_rw(mtr,4,cam_rr(mtr,4)|(0x3ffu<<16));
        cam_rw(mtr,8,cam_rr(mtr,8)&~((0xffu<<24)|(1u<<4)|(1u<<3)|(1u<<2)|3u));
        cam_rw(mtr,8,cam_rr(mtr,8)|(1u<<24));
        cam_rw(mtr,0,cam_rr(mtr,0)|(1u<<4));
        for(n=0;(cam_rr(mtr,0x18)&1) && n<2000;n++)delay(1);
        cam_rw(mtr,0,cam_rr(mtr,0)&~(1u<<4));
        uint32_t cal=cam_rr(mtr,4)&0xffffu,load=(cam_rr(mtr,4)>>16)&0x3ffu,div=(cam_rr(mtr,8)>>24)&0xffu;
        freq=(uint32_t)(26000ull*cal*(div+1)/(load+1));
    }
    cam_rw(mtr,0,c0);cam_rw(mtr,4,c1);cam_rw(mtr,8,c2);cam_rw(mtr,0xc,c3);
    cam_rw(vlp,0x344,dbg);
    return freq;
}
static int cam_sensor_cold_open(uint32_t which, struct x9_camera_probe *r)
{
    struct cam_probe_journal *j=&cam_journal;
    int e=cam_board_prepare(j,which,r);
    uint8_t v=0;
    if(!e) {
        r->stage=X9_CAM_STAGE_LDO;
        e=cam_wake_take(j);
        if(!e)e=cam_wl_read(j,0,&v);r->wl_id=v;
        if(!e && v!=0x0d)e=ENODEV;
        if(!e) {
            /* Oplus wl28681c-regulator.c probe, after reset_high and
             * regulator registration: regmap_write(0x11, 0x07) "disable UVP".
             * Android does it once per boot before any camera rail is used
             * and never restores it. Logs 335-339: bus proven good (EEPROM
             * 0x50/AF 0x0c ACK), every rail/pin/clock register matches the
             * live Android state, yet the sensor (only device on WL
             * AVDD/AVDD1/DVDD) never answers: an undervoltage trip on a WL
             * output under sensor inrush would do exactly that. */
            uint8_t uvp=0;e=cam_wl_read(j,0x11,&uvp);
            if(!e && uvp!=0x07) {
                const uint8_t tx[2]={0x11,0x07};uint8_t back=0;
                e=cam_bus_transfer(&j->ldo,0x35,tx,2,NULL);
                if(!e)e=cam_wl_read(j,0x11,&back);
                if(!e && back!=0x07)e=EIO;
                printf("X9 CAMERA WL UVP: reg11=%02x->%02x errno=%d (Android probe: disable UVP)\n",uvp,back,e);
            } else if(!e) printf("X9 CAMERA WL UVP: reg11=07 already (Android probe value)\n");
        }
        if(!e)e=cam_wl_read(j,3,&j->wl_enable);
        r->wl_enable=j->wl_enable;
        if(!e && (j->wl_enable&j->board.wl_mask)) {
            /* Android regulator_late_cleanup turns off unclaimed LDOs long
             * before camera open, so imgsensor power-on always starts from
             * our channels off. Clear only our bits (vendor enable_mask is
             * per channel), confirm, and let the vendor off settle. */
            const uint8_t tx[2]={3,(uint8_t)(j->wl_enable&~j->board.wl_mask)};
            printf("X9 CAMERA WL ENTRY: enable=%02x mask=%02x (inherited on, cleared like Android)\n",
                j->wl_enable,j->board.wl_mask);
            e=cam_bus_transfer(&j->ldo,0x35,tx,2,NULL);
            if(!e)e=cam_wl_read(j,3,&j->wl_enable);
            if(!e && (j->wl_enable&j->board.wl_mask))e=EIO;
            if(!e)delay(150000);
            r->wl_enable=j->wl_enable;
        }
        for(unsigned i=0;i<j->board.wl_count && !e;i++)
            e=cam_wl_read(j,(uint8_t)(3+j->board.wl_channel[i]),&j->wl_voltage[i]);
        if(!e) {
            j->wl_enable_expected=j->wl_enable;j->wl_tracking=true;
            e=cam_wl_guard(j,false,&v); /* reject an incoherent entry snapshot */
        }
    }
    if(!e) {
        r->stage=X9_CAM_STAGE_POWER;
        e=cam_pins_guard(j,false);
        if(!e)e=cam_clock_check(j,false,true);
        if(!e && which==X9_CAMERA_BACK)e=cam_vs_vote(j,true);
        if(!e)e=cam_pins_take(j);
        if(!e && which==X9_CAMERA_ULTRAWIDE) {
            /* Select drive while MCLK is disconnected, not a live-bank RMW.
             * Keep4mA throughout; vendor's final drive step is a10ms settle. */
            e=cam_drive_write(j,1,false);
            if(!e)e=cam_pin_write(j,&j->mclk,CAM_PIN_MODE,1,false);
            if(!e)delay(2000);
        }
        delay(1000);
        if(!e && which==X9_CAMERA_ULTRAWIDE) {
            e=cam_rail_take(j,&j->dovdd);if(!e)delay(1000);
        }
        /* Journal before writes: an ambiguous write must still be unwound. */
        for(unsigned i=0;i<j->board.wl_count && !e;i++) {
            uint8_t bit=(uint8_t)(1u<<(j->board.wl_channel[i]-1));
            j->wl_owned=true;j->wl_programmed|=bit;
            e=cam_wl_write(j,(uint8_t)(3+j->board.wl_channel[i]),(uint8_t)j->board.wl_code[i]);
            if(!e)e=cam_wl_enable_change(j,i,true);
            if(!e)delay(j->board.wl_delay[i]);
            if(!e && which==X9_CAMERA_BACK && i==1) {
                e=cam_wl_guard(j,false,&v);
                if(!e)e=cam_rail_take(j,&j->afvdd);if(!e)delay(3000);
            }
            if(!e && which==X9_CAMERA_FRONT && i==0) {
                e=cam_wl_guard(j,false,&v);
                if(!e)e=cam_rail_take(j,&j->dovdd);if(!e)delay(1000);
            }
        }
        if(!e && which==X9_CAMERA_BACK) {
            e=cam_wl_guard(j,false,&v);
            if(!e)e=cam_rail_take(j,&j->dovdd);if(!e)delay(3000);
        }
        if(!e && which==X9_CAMERA_TELEPHOTO) {
            e=cam_wl_guard(j,false,&v);
            if(!e)e=cam_rail_take(j,&j->dovdd);if(!e)delay(3000);
            if(!e)e=cam_rail_take(j,&j->afvdd);if(!e)delay(1000);
        }
        if(!e && which==X9_CAMERA_ULTRAWIDE) {
            e=cam_wl_guard(j,false,&v);
            if(!e)e=cam_rail_take(j,&j->dvdd);if(!e)delay(1000);
            if(!e)e=cam_uw_dvdd_alias(j);
            if(!e)e=cam_rail_take(j,&j->afvdd);
        }
        if(!e)e=cam_wl_guard(j,false,&v);
        if(!e)e=cam_clock_check(j,false,true);
        if(!e && which!=X9_CAMERA_ULTRAWIDE)e=cam_drive_write(j,1,false); /* DT encoded 4mA */
        if(!e)e=cam_clock_check(j,false,true);
        if(!e)e=cam_pins_guard(j,false);
        if(!e && which!=X9_CAMERA_ULTRAWIDE)e=cam_pin_write(j,&j->mclk,CAM_PIN_MODE,1,false);
        if(!e && which!=X9_CAMERA_ULTRAWIDE)delay(which==X9_CAMERA_FRONT?1000:6000);
        if(!e)e=cam_wl_guard(j,false,&v);
        if(!e)e=cam_clock_check(j,false,true);
        if(!e)e=cam_pins_guard(j,false);
        if(!e)e=cam_pin_write(j,&j->reset,CAM_PIN_DOUT,1,false);
        if(!e)delay((which==X9_CAMERA_TELEPHOTO || which==X9_CAMERA_ULTRAWIDE)?2000:4000);
        if(!e && which==X9_CAMERA_ULTRAWIDE) {e=cam_drive_check(j,false);if(!e)delay(10000);}
        if(!e)e=cam_wl_guard(j,false,&v); /* after reset settle, before the first sensor request */
        if(!e) {
            uint8_t st[12]={0};static const uint8_t regs[12]={3,0x0b,0x0c,0x0d,0x0e,0x0f,0x11,0x12,1,2,0x10,0x13};
            for(unsigned i=0;i<12;i++)(void)cam_wl_read(j,regs[i],&st[i]);
            printf("X9 CAMERA WL STATUS: en=%02x 0b=%02x 0c=%02x 0d=%02x 0e=%02x 0f=%02x 11=%02x 12=%02x 01=%02x 02=%02x 10=%02x 13=%02x (Android streaming: ee 00 00 00 00 00 07 01 03 7f 7f 05)\n",
                st[0],st[1],st[2],st[3],st[4],st[5],st[6],st[7],st[8],st[9],st[10],st[11]);
            if(which==X9_CAMERA_BACK) {
                uint32_t ref=cam_fmeter_vlp(1,31),tg4=cam_fmeter_vlp(25,7),tg3=cam_fmeter_vlp(24,8);
                printf("X9 CAMERA FMETER: ref26m=%u kHz camtg4=%u kHz camtg3=%u kHz (expect 26000/24000; ffffffff=FENC off, 0=timeout)\n",ref,tg4,tg3);
            }
        }
    }
    if(!e) {
        r->stage=X9_CAM_STAGE_ID;uint8_t hi=0,lo=0;
        const uint16_t id_reg=sensors[which-1].id_reg;
        const uint8_t high[2]={(uint8_t)(id_reg>>8),(uint8_t)id_reg},
            low[2]={(uint8_t)((id_reg+1)>>8),(uint8_t)(id_reg+1)};
        uint8_t slave=cam_sensor_slave(which);
        /* Vendor get_imgsensor_id: i2c_addr_table {0x34,0x35} (both 7-bit
         * 0x1a) x GET_SENSOR_ID_RETRY_CNT 5 = up to 10 reads; a NACK is
         * retried, not fatal. Only a clean NACK (ENXIO, bus not wedged)
         * is retried; any other error stops immediately. */
        unsigned attempt=0;
        for(attempt=1;attempt<=10;attempt++) {
            e=0;
            if(which==X9_CAMERA_ULTRAWIDE && !j->page_known) {
                const uint8_t page[]={0xfc,0xfc,0x40,0};
                j->page_pending=true;
                e=cam_bus_transfer_n(&j->sensor,slave,page,4,NULL,0);
                if(!e) {j->page_pending=false;j->page_known=true;j->sensor_page=0x4000;}
            }
            if(!e)e=cam_bus_transfer(&j->sensor,slave,high,2,&hi);
            if(!e)e=cam_bus_transfer(&j->sensor,slave,low,2,&lo);
            if(!e && (((uint32_t)hi<<8)|lo)==sensors[which-1].chip_id)break;
            if(e && (e!=ENXIO || cam_bus_unretired(&j->sensor)))break;
            if(attempt<10)delay(1000);
        }
        printf("X9 CAMERA ID: sensor=%u slave=%02x attempts=%u id=%02x%02x errno=%d status=%08x\n",
            which,slave,attempt>10?10:attempt,hi,lo,e,j->sensor.last_status);
        if(!e)r->chip_id=((uint32_t)hi<<8)|lo;
        if(!e && r->chip_id!=sensors[which-1].chip_id)e=ENODEV;
        if(!e)r->flags|=X9_CAM_PROBE_MATCHED;
    }
    r->i2c_status[0]=j->sensor.last_status;
    r->i2c_status[1]=j->ldo.last_status;
    return e;
}
static void cam_sensor_cold_log(const struct x9_camera_probe *r)
{
    const struct cam_probe_journal *j=&cam_journal;
    uint32_t which=r->sensor;
    printf("X9 CAMERA PROBE: sensor=%u stage=%u blockers=%x chip=%04x flags=%x errno=%u restore=%u (no capture)\n",
        which,r->stage,r->blockers,r->chip_id,r->flags,r->last_errno,r->restore_errno);
    printf("X9 CAMERA CLOCK: entry cfg=%08x busy=%08x ap=%08x done=%08x pll=%08x fence=%08x (configured nominal, no frequency measurement)\n",
        r->mclk_cfg,r->mclk_status,r->hwv_vote,r->hwv_done,r->pll_con1,r->pll_fence);
    printf("X9 CAMERA COLD: timing_hz=%u/%u entry dovdd=%06x afvdd=%06x retained wl=%x/%x wake=%u rails=%u/%u vs=%u pmif=%u (no CSI/DMA)\n",
        j->sensor.timing_hz,j->ldo.timing_hz,r->dovdd,r->afvdd,j->wl_enabled,j->wl_programmed,
        j->wake.owned,j->dovdd.enable_owned||j->dovdd.voltage_owned,
        j->afvdd.enable_owned||j->afvdd.voltage_owned,j->vs_owned,j->pmif_fault);
    printf("X9 CAMERA COLD RAIL: taken=%u/%u selector_expected=%02x/%02x "
        "enable_expected=%02x/%02x uncertain_sel=%u/%u uncertain_en=%u/%u conflict=%u/%u "
        "(journal, NOT live readback or lease)\n",j->dovdd.taken,j->afvdd.taken,
        j->dovdd.selector_expected,j->afvdd.selector_expected,
        j->dovdd.enable_expected,j->afvdd.enable_expected,
        j->dovdd.selector_uncertain,j->afvdd.selector_uncertain,
        j->dovdd.enable_uncertain,j->afvdd.enable_uncertain,j->dovdd.conflict,j->afvdd.conflict);
    printf("X9 CAMERA COLD CLOCK: parent seen=%u/%u entry=%02x/%02x expected=%02x/%02x retained=%u/%u conflict=%u/%u (journal, no CSI/DMA)\n",
        j->sensor.parent_seen,j->ldo.parent_seen,j->sensor.parent_entry,j->ldo.parent_entry,
        j->sensor.parent_expected,j->ldo.parent_expected,j->sensor.parent_owned,j->ldo.parent_owned,
        j->sensor.parent_conflict,j->ldo.parent_conflict);
    printf("X9 CAMERA COLD WL: expected=%02x prior=%02x uncertain=%02x conflict=%u retained=%02x/%02x (journal, single-channel enable)\n",
        j->wl_enable_expected,j->wl_enable_prior,j->wl_enable_uncertain,j->wl_conflict,
        j->wl_enabled,j->wl_programmed);
    printf("X9 CAMERA COLD AP: fifo=%u/%u request=%u/%u wedged=%u/%u conflict=%u/%u "
        "status=%08x/%08x (journal, NOT live readback or lease)\n",
        j->sensor.fifo_owned,j->ldo.fifo_owned,j->sensor.request_owned,j->ldo.request_owned,
        j->sensor.wedged,j->ldo.wedged,j->sensor.conflict,j->ldo.conflict,
        j->sensor.last_status,j->ldo.last_status);
    printf("X9 CAMERA COLD MCLK: seen=%u active=%u cfg=%02x expected=%02x prior=%02x "
        "vote=%u/%u uncertain=%u/%u update=%u conflict=%u owned=%u/%u "
        "(journal, NOT live readback or lease)\n",
        j->clock_seen,j->clock_active,j->clk_saved,j->clk_expected,j->clk_prior,
        j->vote_expected,j->vote_prior,j->mux_uncertain,j->vote_uncertain,
        j->mux_update_pending,j->clock_conflict,j->mux_owned,j->vote_owned);
    const struct cam_probe_pin *pins[]={&j->reset,&j->mclk,&j->wake,
        &j->route[0],&j->route[1],&j->route[2],&j->route[3]};
    for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++) {
        const struct cam_probe_pin *p=pins[i];
        printf("X9 CAMERA COLD PIN: pin=%u taken=%u owned=%u expected=%x/%x/%x "
            "prior=%x/%x/%x dirty=%x uncertain=%x conflict=%u "
            "(journal, NOT live readback or lease)\n",p->pin,p->taken,p->owned,
            p->expected[CAM_PIN_MODE],p->expected[CAM_PIN_DIR],p->expected[CAM_PIN_DOUT],
            p->prior[CAM_PIN_MODE],p->prior[CAM_PIN_DIR],p->prior[CAM_PIN_DOUT],
            p->dirty,p->uncertain,p->conflict);
    }
    printf("X9 CAMERA COLD DRIVE: taken=%u entry=%x expected=%x prior=%x dirty=%x "
        "uncertain=%u conflict=%u (journal, NOT live readback or lease)\n",
        j->drive.taken,j->drive.saved,j->drive.expected,j->drive.prior,j->drive.dirty,
        j->drive.uncertain,j->drive.conflict);
}
