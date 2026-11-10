/* Explicit I2C6 reg03 reads only. OPPO mt6991-i2c FIFO v2 + captured PLJ110 DT.
 * Borrow only ALREADY ON clocks / ALREADY ROUTED pads; never power/reset a PA,
 * change a shared PLL, take APDMA clock, scan a slave, or expose MMIO to users.
 * I2C6 also has display/charger fixtures: idle/shadow/IRQ/DMA admission is
 * mandatory. XNU currently has no other I2C6 consumer; future users must share
 * a controller arbiter before this diagnostic may coexist with them.
 * A bus read necessarily writes its register-address byte and AP FIFO CSRs;
 * it never writes a PA register value. This is NOT x9_tfa_bus verified=7.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <mach/vm_types.h>
#include <kern/clock.h>
#include "x9_tfa_plj110.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
enum { DATA=0, MASK=8, STAT=0xc, CTRL=0x10, LEN=0x14, TRANS=0x18,
    DELAY=0x1c, TIMING=0x20, START=0x24, EXT=0x28, LTIMING=0x2c,
    HS=0x30, IO=0x34, FIFO_CLR=0x38, SDA=0x3c, AUX=0x44, DIV=0x48,
    MULTI=0x8c, MIS=0x90, ADDR=0x94, FIFO_STATUS=0xf4 };
static const unsigned regs[]={CTRL,LEN,TRANS,DELAY,AUX,ADDR,
    TIMING,EXT,LTIMING,HS,SDA,DIV,IO,MIS};
static const unsigned reset_regs[]={0x8b,0xa3,0xa7,0xb9,0xd5,0xd6};
static const unsigned supply_regs[]={0x24b,0x2087,0x20a2,0x20af};
struct journal {
    vm_offset_t pmif,gpio,gate,perao,bus,dma,top,mainpll,univpll;
    uint32_t saved[sizeof(regs)/sizeof(regs[0])], expected[sizeof(regs)/sizeof(regs[0])];
    bool bus_stuck,pmif_stuck,clock_saved;
};
static struct journal j;
static X9AudioAmpStatus s={.version=X9_TFA_BOARD_STATUS_VERSION};
static uint32_t rr(vm_offset_t b,unsigned r)
{
    uint32_t v=*(volatile uint32_t *)(b+r);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void wr(vm_offset_t b,unsigned r,uint32_t v)
{*(volatile uint32_t *)(b+r)=v;__asm__ volatile("dsb sy":::"memory");}
static int pmif_wait(unsigned expected)
{
    uint64_t ticks;nanoseconds_to_absolutetime(1000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(unsigned i=0;i<1000;i++) {
        uint32_t v=rr(j.pmif,0x8a8);s.pmif_status=v;
        if(v&(1u<<18))return EIO;
        if(((v>>1)&7)==expected)return 0;
        if(mach_absolute_time()>=end)break;delay(1);
    }
    return ETIMEDOUT;
}
static int pmic_read(unsigned sid,unsigned reg,uint32_t *value)
{
    if(j.pmif_stuck)return EIO;
    if(!(rr(j.pmif,0)&1))return ENXIO;
    int e=pmif_wait(0);if(e)return e; /* no request: never clear inherited valid */
    wr(j.pmif,0x880,(3u<<30)|(sid<<24)|reg);
    e=pmif_wait(6);
    if(!e) {
        *value=rr(j.pmif,0x894)&255;wr(j.pmif,0x8a4,1);e=pmif_wait(0);
    }
    if(e) {
        /* Only our late response, while still holding X9PMIF mutex. Timeout
         * remains a failed probe even if draining succeeds; no retry. */
        uint32_t v=rr(j.pmif,0x8a8);s.pmif_status=v;
        if(!(v&(1u<<18)) && ((v>>1)&7)==6) {
            (void)rr(j.pmif,0x894);wr(j.pmif,0x8a4,1);
            if(pmif_wait(0))j.pmif_stuck=true;
        } else j.pmif_stuck=true;
    }
    return e;
}
static int pmic_snapshot(uint32_t *reset,uint32_t *supply)
{
    for(unsigned i=0;i<6;i++) {int e=pmic_read(3,reset_regs[i],&reset[i]);if(e)return e;}
    for(unsigned i=0;i<4;i++) {int e=pmic_read(5,supply_regs[i],&supply[i]);if(e)return e;}
    return 0;
}
static int pmic_admit(void)
{
    /* OPPO tfa98xx_ext_reset: reset-polarity=0 pulses LOW then HIGH.
     * MT6661-3 GPIO10/11 must be digital SW outputs, deasserted HIGH
     * with physical DI HIGH. The captured output-low pinctrl is asserted
     * reset, not PA readiness; this probe never deasserts it itself. */
    if((s.reset[0]&12)!=12 || (s.reset[1]&12)!=12 || (s.reset[2]&12)!=12 ||
       (s.reset[3]&63) || (s.reset[4]&12)!=12 || (s.reset[5]&12)!=12)return EBUSY;
    /* Only the LEFT supply is described by DT. Require stable explicit SW
     * 1.8V/on/non-LP/no-HW votes; keep full DA byte as evidence, not a guessed
     * all-rails-ready predicate. Successful I2C ACK is not right-rail proof. */
    if(s.supply[0]!=0xd0 || (s.supply[1]&3)!=1 || s.supply[3])return ENXIO;
    return 0;
}
static uint32_t pin_state(unsigned n)
{
    return ((rr(j.gpio,0x300+(n/4)*16)>>((n%4)*8))&31) |
        (((rr(j.gpio,(n/32)*16)>>(n%32))&1)<<8) |
        (((rr(j.gpio,0x100+(n/32)*16)>>(n%32))&1)<<9) |
        (((rr(j.gpio,0x200+(n/32)*16)>>(n%32))&1)<<10);
}
static int clock_check(void)
{
    s.gate=rr(j.gate,0xe00);s.dma_gate=rr(j.perao,0x14);
    /* Do not read an off-clock controller OR APDMA bank. PERAO1 AP_DMA
     * physical gate bit26 (I2C is just an alias), DT base 0x16640000. */
    if((s.gate&4) || (s.dma_gate&(1u<<26)))return ENXIO;
    if((rr(j.top,0x338)&(1u<<26)) || !(rr(j.top,0x278)&(1u<<28)))return ENXIO;
    /* Shared DMA parent PERI_AXI: do not trust its leaf gate alone.
     * ULPOSC parent1 is unsupported until its own readiness is established. */
    uint32_t acfg=rr(j.top,0x10),ap=(acfg>>8)&3;
    if((acfg&(1u<<15)) || ap==1 || (rr(j.top,0x330)&(1u<<30)) ||
       !(rr(j.top,0x270)&(1u<<30)))return ENXIO;
    if(ap && !(rr(j.mainpll,0x34)&(1u<<4)))return ENXIO;
    uint32_t acon=ap?rr(j.mainpll,0xc):0;
    if(j.clock_saved && (s.dma_parent!=ap || s.dma_pll_con1!=acon))return EBUSY;
    uint32_t cfg=rr(j.top,0x110),parent=(cfg>>24)&3;
    if(cfg&(1u<<31))return ENXIO;
    vm_offset_t pll=parent==2?j.mainpll:j.univpll;
    if(parent && !(rr(pll,0x34)&(1u<<4)))return ENXIO;
    uint32_t con=parent?rr(pll,0xc):0;
    if(j.clock_saved && (s.parent!=parent || s.pll_con1!=con))return EBUSY;
    uint64_t hz=26000000;
    if(parent) {
        uint64_t d=(uint64_t)16384*(1u<<((con>>24)&7));
        hz=((uint64_t)(con&0x3fffff)*26000000+d-1)/d;
        hz/=parent==1?20:parent==2?16:10;
    }
    if(hz<1000000 || hz>500000000)return ENOTSUP;
    s.dma_parent=ap;s.dma_pll_con1=acon;
    s.parent=parent;s.pll_con1=con;s.nominal_parent_hz=(uint32_t)hz;j.clock_saved=true;
    for(unsigned i=0;i<2;i++) {
        s.pin[i]=pin_state(114+i);
        if((s.pin[i]&31)!=1 || !(s.pin[i]&(1u<<10)))return EBUSY;
    }
    return 0;
}
static int idle(void)
{
    int e=clock_check();if(e)return e;
    for(unsigned i=0;i<2;i++) {
        vm_offset_t b=j.bus+i*0x10000;
        s.bank_start[i]=rr(b,START);s.bank_mask[i]=rr(b,MASK);
        s.bank_status[i]=rr(b,STAT);s.bank_fifo[i]=rr(b,FIFO_STATUS);
        if(s.bank_start[i] || s.bank_mask[i] || s.bank_status[i] ||
           (s.bank_fifo[i]&31) || rr(b,MULTI) ||
           (rr(b,CTRL)&(1u|4u|128u|256u|512u)))return EBUSY;
    }
    s.dma_enable=rr(j.dma,8);return s.dma_enable&1?EBUSY:0;
}
static int put(unsigned reg,uint32_t v)
{
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)if(regs[i]==reg) {
        /* Journal before the write, including a possibly landed failure. */
        s.dirty_mask|=1u<<i;j.expected[i]=v;wr(j.bus,reg,v);return rr(j.bus,reg)==v?0:EIO;
    }
    return EINVAL;
}
static bool factor(unsigned n,unsigned *sample,unsigned *step)
{
    for(unsigned a=1;a<=8;a++)if(n%a==0 && n/a && n/a<=64) {
        *sample=a;*step=n/a;return true;
    }
    return false;
}
static int timing(void)
{
    uint64_t hz=s.nominal_parent_hz;unsigned div=0,hs=0,ht=0,ls=0,lt=0,ext=0,total=0;
    bool found=false;
    for(div=1;div<=32 && !found;div++) {
        ext=(unsigned)((hz/div)*5/1000000);if(ext>255)continue;
        total=(unsigned)((hz+(uint64_t)div*98000-1)/((uint64_t)div*98000))+1;
        if(total<4 || total>1024)continue;
        unsigned ideal=total*45/100;
        for(unsigned delta=0;delta<=total/2 && !found;delta++)for(unsigned side=0;side<2 && !found;side++) {
            if(side && delta>ideal)continue;
            unsigned h=side?ideal-delta:ideal+delta;
            if(h<2 || h>=total-1 || h*1000<total*410 || h*1000>=total*490)continue;
            found=factor(h,&hs,&ht) && factor(total-h,&ls,&lt);
        }
    }
    if(!found)return ENOTSUP;div--;
    int e=idle();if(e)return e;
    const unsigned r[]={TIMING,EXT,LTIMING,HS,SDA,DIV,IO,MIS,DELAY};
    const uint32_t v[]={((hs-1)<<8)|(ht-1),(ext<<8)|1,((ls-1)<<6)|(lt-1),
        0,0,((div-1)<<8)|(div-1),3,0,2};
    for(unsigned i=0;i<sizeof(r)/sizeof(r[0]);i++){e=put(r[i],v[i]);if(e)return e;}
    s.timing_hz=(uint32_t)(hz/(div*total));return 0;
}
static int read_revision(unsigned i)
{
    int e=idle();if(e)return e;
    /* One register byte, two big-endian result bytes. No PA value write. */
    const unsigned r[]={CTRL,ADDR,LEN,AUX,TRANS};
    const uint32_t v[]={ (j.saved[0]&~(2u|4u|16u|256u|512u))|2u|8u|16u|32u,
        (0x34u+i)<<1,1,2,2 };
    for(unsigned k=0;k<sizeof(r)/sizeof(r[0]);k++){e=put(r[k],v[k]);if(e)return e;}
    s.last_slave=0x34+i;wr(j.bus,FIFO_CLR,1);
    *(volatile uint8_t *)(j.bus+DATA)=3;__asm__ volatile("dsb sy":::"memory");
    if((rr(j.bus,FIFO_STATUS)&31)!=1) {
        wr(j.bus,FIFO_CLR,1);if(idle())j.bus_stuck=true;return EIO;
    }
    wr(j.bus,START,(1u<<14)|1);
    uint64_t ticks;nanoseconds_to_absolutetime(5000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;uint32_t status=0;
    for(unsigned n=0;n<1000;n++) {
        status=rr(j.bus,STAT);if(status)break;
        if(mach_absolute_time()>=end)break;delay(5);
    }
    s.request_status=status;s.request_start=rr(j.bus,START);s.request_fifo=rr(j.bus,FIFO_STATUS);
    if(!status || (s.request_start&1)) {j.bus_stuck=true;return ETIMEDOUT;}
    /* Unknown/arbitration/config status is not permission to clear it or
     * cancel the request. Only completed ACK/NACK belongs to this probe. */
    if(status&~7u) {j.bus_stuck=true;return EIO;}
    if(status!=1)e=EIO;
    if(!e) {
        if((s.request_fifo&31)!=2)e=EIO;
        else {
            uint16_t rev=(uint16_t)((unsigned)*(volatile uint8_t *)(j.bus+DATA)<<8);
            rev|=*(volatile uint8_t *)(j.bus+DATA);
            s.revision[i]=rev;s.revision_read|=1u<<i;
            unsigned family=rev&255;
            if(family==0x72 || family==0x73 || family==0x74)s.known_family|=1u<<i;
            else e=ENOTSUP;
        }
    }
    wr(j.bus,STAT,status);wr(j.bus,FIFO_CLR,1);wr(j.bus,START,0);
    if(idle()){j.bus_stuck=true;if(!e)e=EIO;}return e;
}
static int restore(void)
{
    if(!s.dirty_mask)return 0;
    if(j.bus_stuck)return EBUSY;
    int e=idle();if(e)return e;
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)if(s.dirty_mask&(1u<<i)) {
        uint32_t current=rr(j.bus,regs[i]);
        if(current!=j.saved[i]) {
            if(current!=j.expected[i])return EBUSY; /* changed by an unknown owner */
            wr(j.bus,regs[i],j.saved[i]);if(rr(j.bus,regs[i])!=j.saved[i])return EIO;
        }
        s.dirty_mask&=~(1u<<i); /* never replay restored fields */
    }
    s.flags&=~X9PAJournalHeld;return 0;
}
int x9_tfa_plj110_probe(void)
{
    if(s.flags&X9PAProbeAttempted)return EBUSY; /* once per boot, including misses */
    s.flags=X9PAProbeAttempted;s.stage=X9PAStageMap;
    j.pmif=ml_io_map(0x1c018000,0x2000);j.gpio=ml_io_map(0x1002d000,0x1000);
    j.gate=ml_io_map(0x129d0000,0x1000);j.perao=ml_io_map(0x16640000,0x1000);
    j.bus=ml_io_map(0x12990000,0x20000);j.dma=ml_io_map(0x16410000,0x10000);
    j.top=ml_io_map(0x1c210000,0x1000);
    j.mainpll=ml_io_map(0x1c213000,0x100);j.univpll=ml_io_map(0x1c213100,0x100);
    int e=0;
    if(!j.pmif || !j.gpio || !j.gate || !j.perao || !j.bus || !j.dma ||
       !j.top || !j.mainpll || !j.univpll)e=ENOMEM;
    bool locked=false;
    if(!e) {s.stage=X9PAStagePMIC;e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
    if(!e)e=pmic_snapshot(s.reset,s.supply);
    if(!e){s.flags|=X9PAPMICCaptured;e=pmic_admit();}
    /* Hold the shared PMIF mutex from live snapshot through FIFO restore;
     * this prevents XNU rail/reset changes, not unknown remote ownership. */
    if(!e){s.stage=X9PAStageClock;e=clock_check();}
    if(!e){s.stage=X9PAStageIdle;e=idle();}
    if(!e) {
        for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)j.saved[i]=rr(j.bus,regs[i]);
        s.flags|=X9PABusCaptured|X9PAJournalHeld;s.stage=X9PAStageTiming;e=timing();
    }
    if(!e){s.stage=X9PAStageLeft;e=read_revision(0);}
    if(!e){s.stage=X9PAStageRight;e=read_revision(1);}
    if(!e) {
        s.stage=X9PAStageRecheck;e=pmic_snapshot(s.reset_after,s.supply_after);
        if(!e && (memcmp(s.reset,s.reset_after,sizeof(s.reset)) ||
           memcmp(s.supply,s.supply_after,sizeof(s.supply))))e=EBUSY;
        if(!e)s.flags|=X9PAPMICStable;
    }
    /* Primary failure stage/error preserved; restore gets its own errno. */
    s.error=(uint32_t)e;s.restore_error=(uint32_t)restore();
    if(!e && s.restore_error)s.stage=X9PAStageRestore;
    if(!s.dirty_mask)s.flags&=~X9PAJournalHeld;
    if(j.pmif_stuck || j.bus_stuck || s.restore_error)s.flags|=X9PAPoisoned;
    if(locked)X9PMIFRelease();
    if(!e && !s.restore_error){s.flags|=X9PAProbeComplete;s.stage=X9PAStageDone;}
    return e?e:(int)s.restore_error;
}
int x9_tfa_plj110_status(X9AudioAmpStatus *out)
{if(!out)return EINVAL;*out=s;return 0;}
