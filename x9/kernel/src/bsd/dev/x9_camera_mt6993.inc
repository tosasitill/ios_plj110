/* X9 sensor-only backend. No SENINF/ISP/DMA or x9_camera_platform registration.
 * Board wiring: captured PLJ110 DT, sensor0/1 and wl28681c_hh@35 on i2c12.
 * Register descriptions: OPPO kernel_device_modules-6.12 spmi-mtk-pmif-core.c, i2c-mt65xx.c
 * (mt6991_compat/v2/32-bit, channel offset ZERO in this DT),
 * clk-mt6993{,-peri}.c, pinctrl-mt6993.c, mt6661-regulator.h;
 * OPLUS camera/regulator/wl28681c-regulator.c. No Qualcomm MMIO.
 *
 * Deliberately conditional bring-up: borrow ONLY already-running CAMTG
 * 24MHz, already-configured/ungated/idle AP I2C and compatible shared rails.
 * Do NOT wake/reset WL (shared GPIO203/187/202), change PLL/voter/PMIC rails,
 * route SCP pins, acknowledge inherited IRQs, or reset a wedged controller.
 * A full clock/shared-power ownership provider remains required for cold LK.
 * All writes below are confined to idle AP FIFO transactions, inactive
 * sensor reset/MCLK pins, and that sensor's disabled WL channels. A journal
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
    vm_offset_t base, dma, gate;
    uint32_t gate_bit, saved[6], last_status;
    bool owned, wedged, failed_status;
};
struct cam_probe_board {
    uint32_t reset, mclk, scl, sda, wl_mask, wl_count;
    uint32_t wl_channel[3], wl_code[3], wl_delay[3];
    uint32_t clk_off, clk_shift, clk_status_bit;
    vm_offset_t drive_base;
};
struct cam_probe_journal {
    struct cam_probe_bus sensor, ldo;
    struct cam_probe_board board;
    vm_offset_t gpio, vlp, pmif_p, pmif_m, pmif_pending;
    uint32_t drive, mode, dir, dout;
    uint8_t wl_enable, wl_voltage[3];
    bool pins_owned, wl_owned;
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
static int cam_bus_take(struct cam_probe_bus *b)
{
    if((cam_rr(b->gate,0xe00)&b->gate_bit) || !cam_rr(b->base,CAM_I2C_TIMING) ||
       (cam_rr(b->base,CAM_I2C_HS)&0x8001) ||
       cam_rr(b->base,CAM_I2C_IO)!=3)return ENOTSUP;
    int e=cam_bus_idle(b);if(e)return e;
    for(unsigned i=0;i<6;i++)b->saved[i]=cam_rr(b->base,cam_i2c_saved_regs[i]);
    /* DMA, IRQ routing and wrapper state are not inherited silently. */
    if(b->saved[1]&((1u<<2)|(1u<<7)|(1u<<8)|(1u<<9)|1u))return ENOTSUP;
    b->owned=true;return 0;
}
static int cam_bus_restore(struct cam_probe_bus *b)
{
    if(!b->owned)return 0;
    if(b->wedged || cam_bus_idle(b))return EBUSY;
    for(unsigned i=0;i<6;i++) {
        cam_rw(b->base,cam_i2c_saved_regs[i],b->saved[i]);
        if(cam_rr(b->base,cam_i2c_saved_regs[i])!=b->saved[i])return EIO;
    }
    b->owned=false;return 0;
}
/* WRRD: one/two register-address bytes followed by ONE data byte.
 * FIFO_DATA is byte-accessed in the upstream driver even with 32-bit CSR.
 * No DMA, retries, broad slave scan, shared reset or inherited W1C. */
static int cam_bus_transfer(struct cam_probe_bus *b, uint8_t slave,
                            const uint8_t *tx, unsigned count, uint8_t *rx)
{
    if(!b->owned || b->wedged || !count || count>3)return EINVAL;
    int e=cam_bus_idle(b);if(e)return e;
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
        .clk_status_bit=back?4:10
    };
    j->gpio=ml_io_map(0x1002d000,0x1000);
    j->vlp=ml_io_map(0x1c200000,0x1000);
    j->pmif_p=ml_io_map(0x1c018000,0x2000);
    j->pmif_m=back?ml_io_map(0x1c01a000,0x2000):0;
    j->board.drive_base=ml_io_map(back?0x13800000:0x13040000,0x1000);
    j->sensor=(struct cam_probe_bus){
        .base=ml_io_map(back?0x12450000:0x123d0000,0x40000),
        .dma=ml_io_map(back?0x163d0000:0x16390000,0x1000),
        .gate=ml_io_map(0x12650000,0x1000),.gate_bit=back?4:2};
    j->ldo=(struct cam_probe_bus){.base=ml_io_map(0x16200000,0x40000),
        .dma=ml_io_map(0x164f0000,0x1000),.gate=ml_io_map(0x16300000,0x1000),.gate_bit=1};
    if(!j->gpio || !j->vlp || !j->pmif_p || (back && !j->pmif_m) ||
       !j->board.drive_base || !j->sensor.base || !j->sensor.dma || !j->sensor.gate ||
       !j->ldo.base || !j->ldo.dma || !j->ldo.gate)return ENOMEM;
    r->mclk_cfg=cam_rr(j->vlp,j->board.clk_off);
    r->mclk_status=cam_rr(j->vlp,0x330);
    /* CAMTG1/7 parent index7=UNIVPLL_192M_D8=24MHz. Do not change mux,
     * AP voter or common PLL. CKSTA is a precondition, not frequency proof. */
    if(((r->mclk_cfg>>j->board.clk_shift)&0x8f)!=7 ||
       !(r->mclk_status&(1u<<j->board.clk_status_bit)))r->blockers|=X9_CAM_BLOCK_CLOCK;
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
    return r->last_errno;
}
