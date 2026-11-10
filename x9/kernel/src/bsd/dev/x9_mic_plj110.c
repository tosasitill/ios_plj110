/* PLJ110 cold codec power/calibration/capture pipeline; DMA is separate.
 * Wiring: captured DT scp_i2c1, mt6681-pmic@6b, codec-en GPIO9.
 * Protocol/gates: OPPO i2c-mt65xx.c (mt6991_compat), clk-mt6993-peri.c,
 * pinctrl-mtk-mt6993.h, spmi-mtk-pmif-core.c, mt6685-audclk.c.
 * Dedicated SCP bank + wake handshake; no AP-channel shadow writes, DMA,
 * global reset, foreign IRQ clearing, PLL retuning or off-domain AFE reads.
 * Explicit calibrate additionally acquires the AFE journal below; power-only
 * never touches AFE. Diagnostics restore immediately; a verified DMA
 * provider may instead hold this same pipeline until its STOP/drain barrier.
 * Source clock nominal 130MHz is the DT clk-src-in-hz, not a measured rate.
 * GPIO9 must already be physically low. An inherited live codec/XO/session
 * is refused, not reset. A failed restore retains the journal for this boot.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <mach/vm_types.h>
#include <kern/clock.h>
#include "x9_mic_plj110.h"
#include "x9_mic_mt6993_regs.h"
#include "x9_audio_resources.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);

enum { DATA=0, MASK=8, STAT=0xc, CTRL=0x10, LEN=0x14, TRANS=0x18,
    DELAY=0x1c, TIMING=0x20, START=0x24, EXT=0x28, LTIMING=0x2c,
    HS=0x30, IO=0x34, FIFO_CLR=0x38, SDA=0x3c, AUX=0x44, DIV=0x48,
    MULTI=0x8c, MIS=0x90, ADDR=0x94, FIFO_STAT=0xf4 };
static const unsigned saved_regs[]={MASK,CTRL,LEN,TRANS,DELAY,AUX,ADDR,
    TIMING,EXT,LTIMING,HS,SDA,DIV,IO,MIS};
struct pin {unsigned n,mode,dir,dout;bool owned;};
struct afe_journal;
struct power_journal {
    vm_offset_t gpio,vlp,gipc,gate,bus,pmif;
    struct pin enable,route[2];
    uint32_t saved[sizeof(saved_regs)/sizeof(saved_regs[0])];
    uint8_t xo;
    bool wake_owned,awake,gate_owned,bus_owned,xo_owned;
    bool bus_stuck,pmif_stuck,enabled;
    struct x9_mic_mt6681 codec;
    struct x9_mic_board_status *status;
    struct afe_journal *cal;
    struct x9_mic_hardware_status *hardware;
    struct x9_mic_analog_status *analog;
    struct x9_mic_digital_status *digital;
    struct x9_mic_dma_status *dma;
    uint64_t started, resource_ticket;
    bool session, capture_held, finished;
    int finish_errno;
};
static struct power_journal board;
static uint32_t rr(vm_offset_t b,unsigned r)
{
    uint32_t v=*(volatile uint32_t *)(b+r);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void wr(vm_offset_t b,unsigned r,uint32_t v)
{
    *(volatile uint32_t *)(b+r)=v;__asm__ volatile("dsb sy":::"memory");
}
static int update(vm_offset_t b,unsigned r,uint32_t mask,uint32_t v)
{
    wr(b,r,(rr(b,r)&~mask)|(v&mask));return (rr(b,r)&mask)==(v&mask)?0:EIO;
}
static int wait_bits(vm_offset_t b,unsigned r,uint32_t mask,uint32_t v,unsigned us)
{
    for(unsigned i=0;i<us;i++){if((rr(b,r)&mask)==v)return 0;delay(1);}
    return ETIMEDOUT;
}
#include "x9_mic_plj110_afe.inc"
#include "x9_mic_plj110_digital.inc"
static unsigned pin_bit(struct power_journal *j,unsigned bank,unsigned n)
{return (rr(j->gpio,bank+(n/32)*16)>>(n%32))&1;}
static unsigned pin_mode(struct power_journal *j,unsigned n)
{return (rr(j->gpio,0x300+(n/4)*16)>>((n%4)*8))&31;}
static int set_bit(struct power_journal *j,unsigned bank,unsigned n,unsigned value)
{
    unsigned r=bank+(n/32)*16;uint32_t bit=1u<<(n%32);
    wr(j->gpio,r+(value?4:8),bit);return pin_bit(j,bank,n)==value?0:EIO;
}
static int set_mode(struct power_journal *j,unsigned n,unsigned mode)
{return update(j->gpio,0x300+(n/4)*16,31u<<((n%4)*8),mode<<((n%4)*8));}
static uint32_t pin_state(struct power_journal *j,unsigned n)
{return pin_mode(j,n)|(pin_bit(j,0,n)<<8)|(pin_bit(j,0x100,n)<<9)|(pin_bit(j,0x200,n)<<10);}
static void save_pin(struct power_journal *j,struct pin *p,unsigned n)
{p->n=n;p->mode=pin_mode(j,n);p->dir=pin_bit(j,0,n);p->dout=pin_bit(j,0x100,n);}
static int wake_take(struct power_journal *j)
{
    j->status->sleep_before=rr(j->vlp,0x92c);
    if(j->status->sleep_before&5)return EBUSY; /* no borrowed wake votes */
    j->wake_owned=true;
    int e=update(j->vlp,0x92c,1,1);if(e)return e;
    /* Exact use_slp_stat1_stat2 branch, not the old +0x200 SCP branch. */
    bool ready=false;
    for(unsigned i=0;i<40;i++) {
        if(!(rr(j->vlp,0x934)&0xf0000) && !(rr(j->vlp,0x934)&0xf0000)) {
            ready=true;break;
        }
        delay(20);
    }
    e=update(j->vlp,0x92c,5,5);if(e)return e;
    if(!ready)for(unsigned i=0;i<40;i++) {
        if(!(rr(j->vlp,0x938)&0x3e) && !(rr(j->vlp,0x938)&0x3e)) {
            ready=true;break;
        }
        delay(20);
    }
    j->status->sleep_stat=rr(j->vlp,0x934);j->status->sleep_stat2=rr(j->vlp,0x938);
    if(!ready)return ETIMEDOUT;
    e=update(j->vlp,0x92c,1,0);if(!e)j->awake=true;return e;
}
static int bank_idle(struct power_journal *j)
{
    if(rr(j->bus,START) || rr(j->bus,MASK) || rr(j->bus,STAT) || (rr(j->bus,FIFO_STAT)&31))return EBUSY;
    return 0;
}
static bool factor(unsigned n,unsigned *sample,unsigned *step)
{
    for(unsigned s=1;s<=8;s++)if(n%s==0 && n/s && n/s<=64) {
        *sample=s;*step=n/s;return true;
    }
    return false;
}
static int bus_timing(struct power_journal *j)
{
    /* speed_ver=1 uses the vendor set_speed_v2 layout. Use a conservative
     * <=98kHz power-probe rate rather than trusting an inherited 1MHz setup.
     * 45% high duty, 5us START/STOP. Solve the bounded sample/step products. */
    unsigned div=0,hs=0,ht=0,ls=0,lt=0,ext=0,total=0;bool found=false;
    for(div=1;div<=32 && !found;div++) {
        ext=(130000000u/div)*5/1000000;if(ext>255)continue;
        total=(130000000u+div*98000-1)/(div*98000)+1;
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
    const unsigned regs[]={TIMING,EXT,LTIMING,HS,SDA,DIV,IO,MIS,DELAY};
    const uint32_t values[]={((hs-1)<<8)|(ht-1),(ext<<8)|1,
        ((ls-1)<<6)|(lt-1),0,0,((div-1)<<8)|(div-1),3,0,2};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        wr(j->bus,regs[i],values[i]);if(rr(j->bus,regs[i])!=values[i])return EIO;
    }
    j->status->timing_hz=130000000u/(div*total);return 0;
}
static int bus_take(struct power_journal *j)
{
    /* main clock is scp_i3c_i2c1, bit1 at 1da00e10; SET e18 / CLR e14.
     * i2c-clock-info handling in i2c-mt65xx is FPGA-only, not the runtime
     * provider. No DMA clock needed for 2/3-byte FIFO transfers. Do not read
     * the APDMA CSR with its clock off; the dedicated bank's CTRL DMA bit
     * and hardware-trigger mode are required to be disabled instead. */
    j->status->gate_before=rr(j->gate,0xe10);
    if(j->status->gate_before&2) {
        j->gate_owned=true;wr(j->gate,0xe14,2);
        int e=wait_bits(j->gate,0xe10,2,0,1000);
        if(e){j->bus_stuck=true;return e;}
    }
    int e=bank_idle(j);if(e){j->bus_stuck=true;return e;}
    /* Refuse DMA/IRQ selection, wrapper, shadow/hardware-trigger modes.
     * No assumption that idle alone arbitrates with a live SCP codec owner:
     * the separate GPIO9/XO cold-baseline check is mandatory. */
    if(rr(j->bus,MULTI) || (rr(j->bus,CTRL)&(1u|(1u<<2)|(1u<<7)|(1u<<8)|(1u<<9)))) {
        j->bus_stuck=true;return ENOTSUP;
    }
    for(unsigned i=0;i<sizeof(saved_regs)/sizeof(saved_regs[0]);i++)j->saved[i]=rr(j->bus,saved_regs[i]);
    j->bus_owned=true;
    for(unsigned i=0;i<2;i++) {
        struct pin *p=&j->route[i];
        if(pin_mode(j,p->n)!=p->mode || pin_bit(j,0,p->n)!=p->dir)return EBUSY;
        if(p->mode==1)continue; /* dedicated SCP route, not TDM/AP mux */
        p->owned=true;e=set_mode(j,p->n,1);if(e)return e;
    }
    return bus_timing(j);
}
static int bus_xfer(struct power_journal *j,uint16_t reg,uint8_t *value,bool read)
{
    if(!j->bus_owned || !j->enabled || j->bus_stuck)return ENXIO;
    j->status->last_register=reg;
    if(!j->awake || (rr(j->vlp,0x92c)&5)!=4 || (rr(j->gate,0xe10)&2) ||
       pin_mode(j,9) || !pin_bit(j,0,9) || !pin_bit(j,0x100,9) || !pin_bit(j,0x200,9) ||
       pin_mode(j,96)!=1 || pin_mode(j,97)!=1)return EBUSY;
    int e=bank_idle(j);if(e)return e;
    if(!pin_bit(j,0x200,96) || !pin_bit(j,0x200,97))return EBUSY;
    uint32_t ctrl=(j->saved[1]&~((1u<<1)|(1u<<2)|(1u<<4)|(1u<<8)|(1u<<9)))|(1u<<3)|(1u<<5);
    if(read)ctrl|=(1u<<1)|(1u<<4);
    wr(j->bus,CTRL,ctrl);wr(j->bus,MASK,0);wr(j->bus,ADDR,0x6bu<<1);
    wr(j->bus,FIFO_CLR,4);wr(j->bus,FIFO_CLR,1); /* MCH then normal FIFO */
    wr(j->bus,LEN,read?2:3);wr(j->bus,AUX,read?1:0);wr(j->bus,TRANS,read?2:1);
    uint8_t tx[3]={(uint8_t)(reg>>8),(uint8_t)reg,*value};
    for(unsigned i=0;i<(read?2u:3u);i++)*(volatile uint8_t *)(j->bus+DATA)=tx[i];
    __asm__ volatile("dsb sy":::"memory");
    bool filled=false;
    for(unsigned i=0;i<10;i++)if((rr(j->bus,FIFO_STAT)&31)==(read?2u:3u)){filled=true;break;}
    if(!filled) {
        wr(j->bus,FIFO_CLR,4);wr(j->bus,FIFO_CLR,1);
        if(bank_idle(j))j->bus_stuck=true;
        return EIO;
    }
    wr(j->bus,START,(1u<<14)|1); /* mt6991 auto_restart multi trigger */
    uint32_t status=0;uint64_t ticks;nanoseconds_to_absolutetime(5000000,&ticks);
    uint64_t deadline=mach_absolute_time()+ticks;
    for(unsigned i=0;i<1000;i++) {
        status=rr(j->bus,STAT);if(status)break;
        if(mach_absolute_time()>=deadline)break;delay(5);
    }
    j->status->bus_status=status;j->status->bus_start=rr(j->bus,START);
    j->status->bus_fifo=rr(j->bus,FIFO_STAT);
    if(!status || (j->status->bus_start&1)) {
        j->bus_stuck=true;return ETIMEDOUT; /* no cancel/reset of live request */
    }
    if(status!=1)e=EIO; /* NACK, arbitration, config and unknown are not ACK */
    if(!e && read) {
        if((j->status->bus_fifo&31)!=1)e=EIO;
        else *value=*(volatile uint8_t *)(j->bus+DATA);
    }
    wr(j->bus,STAT,status&0x21f); /* only known flags produced by our request */
    wr(j->bus,FIFO_CLR,4);wr(j->bus,FIFO_CLR,1);wr(j->bus,START,0);
    if(bank_idle(j)){j->bus_stuck=true;if(!e)e=EIO;}
    return e;
}
static int codec_read(void *cookie,uint16_t reg,uint8_t *v)
{if(!v)return EINVAL;*v=0;return bus_xfer(cookie,reg,v,true);}
static int codec_write(void *cookie,uint16_t reg,uint8_t v)
{return bus_xfer(cookie,reg,&v,false);}
static int pmif_wait(struct power_journal *j,unsigned state)
{
    for(unsigned i=0;i<1000;i++) {
        uint32_t s=rr(j->pmif,0x8a8);if(s&(1u<<18))return EIO;
        if(((s>>1)&7)==state)return 0;delay(1);
    }
    return ETIMEDOUT;
}
static int xo_xfer_locked(struct power_journal *j,uint8_t *v,bool write)
{
    /* DT spmi-dev-mask=0x55a8 has bit9 CLEAR: MT6687 SID9 is PMIF-M.
     * SWINF2 is shared with RTC/keys/modem/camera; hold X9PMIF throughout
     * snapshot/update/readback. Only BBCK5 (0x7d6) may be written here. */
    if(j->pmif_stuck)return EIO;
    if(!(rr(j->pmif,0)&1))return ENXIO;
    int e=pmif_wait(j,0);if(e)return e;
    if(write)wr(j->pmif,0x884,*v);
    wr(j->pmif,0x880,(3u<<30)|(write?1u<<29:0)|(9u<<24)|0x7d6);
    e=pmif_wait(j,write?0:6);
    if(!e && !write) {
        *v=(uint8_t)rr(j->pmif,0x894);wr(j->pmif,0x8a4,1);e=pmif_wait(j,0);
    }
    if(e) {
        /* Drain only our own late read while still holding the PMIF lock. */
        uint32_t s=rr(j->pmif,0x8a8);
        if(!write && !(s&(1u<<18)) && ((s>>1)&7)==6) {
            (void)rr(j->pmif,0x894);wr(j->pmif,0x8a4,1);
            if(pmif_wait(j,0))j->pmif_stuck=true;
        } else j->pmif_stuck=true;
    }
    return e;
}
static int xo_take(struct power_journal *j)
{
    if(X9PMIFAcquire())return EBUSY;
    uint8_t v=0;int e=xo_xfer_locked(j,&v,false);
    if(!e) {
        j->xo=v;j->status->xo_before=v;
        if(v&7)e=EBUSY; /* only off, software-controlled BBCK5 */
    }
    if(!e) {
        j->xo_owned=true;v|=4;e=xo_xfer_locked(j,&v,true);
        if(!e)e=xo_xfer_locked(j,&v,false);
        j->status->xo_after=v;
        if(!e && (v&7)!=4)e=EIO;
    }
    X9PMIFRelease();if(!e)delay(420);return e;
}
static int codec_enable(struct power_journal *j)
{
    struct pin *p=&j->enable;
    if(pin_mode(j,9)!=p->mode || pin_bit(j,0,9)!=p->dir ||
       pin_bit(j,0x100,9)!=p->dout || pin_bit(j,0x200,9))return EBUSY;
    p->owned=true;int e=set_bit(j,0x100,9,0);
    if(!e)e=set_bit(j,0,9,1);
    if(!e)delay(1000); /* minimum cold-low dwell, not a chip-ready assertion */
    if(!e){j->enabled=true;e=set_bit(j,0x100,9,1);}
    if(!e)e=wait_bits(j->gpio,0x200,1u<<9,1u<<9,1000);
    return e;
}
static int restore(struct power_journal *j)
{
    /* Never release clocks/wake with an in-flight FIFO or uncertain PMIF.
     * An unsuccessful codec/AFE unwind is latched, not reported restored. */
    if(j->bus_stuck || j->pmif_stuck || j->codec.poisoned)return EIO;
    if(j->cal && j->cal->stuck)return EIO;
    /* Codec cleanup may access AFE PAD/TEST: first prove clocks are held. */
    if(j->codec.afe_snapshot && (!j->cal || afe_access(j->cal)))return EBUSY;
    if(j->bus_owned && ((rr(j->gate,0xe10)&2) || !j->awake ||
       (rr(j->vlp,0x92c)&5)!=4))return EBUSY;
    /* RX/SRC off, then codec TX/SRC off while PAD_CLK is still held. Only
     * after both ends are quiescent restore digital clocks, then analog. */
    int e=j->cal?digital_afe_quiesce(j->cal):0;if(e)return e;
    e=x9_mic_mt6681_digital_stop(&j->codec);if(e)return e;
    if(j->cal){e=digital_afe_restore(j->cal);if(e)return e;}
    e=x9_mic_mt6681_unprepare(&j->codec);if(e)return e;
    if(j->enable.owned) {
        if(pin_mode(j,9) || (pin_bit(j,0,9)!=1 && pin_bit(j,0,9)!=j->enable.dir))return EBUSY;
        e=set_bit(j,0x100,9,0);if(e)return e;
        e=wait_bits(j->gpio,0x200,1u<<9,0,1000);if(e)return e;
        j->enabled=false;delay(1000);
        e=set_bit(j,0,9,j->enable.dir);if(e)return e;
        e=set_bit(j,0x100,9,j->enable.dout);if(e)return e;
        j->enable.owned=false;
    }
    if(j->xo_owned) {
        if(X9PMIFAcquire())return EBUSY;
        uint8_t v=0;e=xo_xfer_locked(j,&v,false);
        if(!e && (v&7)!=4)e=EBUSY;
        if(!e){v=(v&~7u)|(j->xo&7);e=xo_xfer_locked(j,&v,true);}
        if(!e)e=xo_xfer_locked(j,&v,false);
        j->status->xo_after=v;
        if(!e && (v&7)!=(j->xo&7))e=EIO;
        X9PMIFRelease();if(e)return e;j->xo_owned=false;
    }
    /* Codec TX/loopback stopped, GPIO9 low and XO off before pad/domain
     * unwind. Do not release transport/wake if the AFE unwind is uncertain. */
    if(j->cal) {
        e=afe_restore(j->cal);
        j->cal->s->restore_errno=e;
        if(e){j->cal->s->flags|=X9_MIC_AFE_RETAINED;return e;}
        j->cal->s->flags|=X9_MIC_AFE_RESTORED;
        if(!j->cal->s->last_errno)j->cal->s->stage=X9_MIC_AFE_OFF;
    }
    if(j->bus_owned) {
        e=bank_idle(j);if(e)return e;
        for(unsigned i=0;i<sizeof(saved_regs)/sizeof(saved_regs[0]);i++) {
            wr(j->bus,saved_regs[i],j->saved[i]);
            if(rr(j->bus,saved_regs[i])!=j->saved[i])return EIO;
        }
        j->bus_owned=false;
    }
    for(unsigned i=2;i>0;i--) {
        struct pin *p=&j->route[i-1];if(!p->owned)continue;
        unsigned mode=pin_mode(j,p->n);
        if(mode!=1 && mode!=p->mode)return EBUSY;
        e=set_mode(j,p->n,p->mode);if(e)return e;p->owned=false;
    }
    if(j->gate_owned) {
        wr(j->gate,0xe18,2);e=wait_bits(j->gate,0xe10,2,2,1000);
        if(e)return e;j->gate_owned=false;
    }
    if(j->wake_owned) {
        if(j->awake)wr(j->gipc,0,0x10); /* GIPC SET, never RMW other events */
        e=update(j->vlp,0x92c,5,j->status->sleep_before&5);if(e)return e;
        j->awake=j->wake_owned=false;
    }
    return 0;
}
static int board_finish(struct power_journal *j,int e)
{
    /* Persistent pointers/journals are retained on uncertainty. Never read
     * powered-off AFE or replay a failed restore on a second StopIO. */
    if(j->finished)return j->finish_errno;
    j->finished=true;
    struct x9_mic_hardware_status *h=j->hardware;
    struct x9_mic_board_status *s=j->status;
    struct x9_mic_afe_status *a=j->cal?j->cal->s:NULL;
    struct x9_mic_analog_status *analog=j->analog;
    struct x9_mic_digital_status *digital=j->digital;
    struct x9_mic_dma_status *dma=j->dma;
    uint64_t start=j->started;
    s->last_errno=e;if(e && !h->last_errno)h->last_errno=e;
    uint32_t last_reg=s->last_register,codec_reg=h->last_register;
    uint32_t digital_reg=digital?digital->last_register:0;
    if(dma && (dma->flags&X9_MIC_DMA_REGISTERS_VALID) && j->cal && !j->cal->stuck) {
        uint32_t reg=dma->last_register;
        int snapshot=digital_dma_snapshot(j->cal,dma,true);
        if(snapshot && !e){e=snapshot;s->last_errno=e;h->last_errno=e;
            digital_reg=digital->last_register=0x80000000u|dma->last_register;digital->last_errno=e;}
        else dma->last_register=reg;
    }
    if(!e)s->stage=X9_MIC_BOARD_RESTORE;
    if(digital && !e)digital->stage=X9_MIC_DIGITAL_RESTORE;
    int off=restore(j);
    /* All physical snapshots stay inside the single AP transition lease. */
    if(j->cal)afe_ao_snapshot(j->cal);
    if(j->gpio)s->gpio9_after=pin_state(j,9);
    if(j->vlp)s->sleep_after=rr(j->vlp,0x92c);
    if(j->gate)s->gate_after=rr(j->gate,0xe10);
    if(!off) {
        off=x9_audio_resources_release(X9AudioResourceMic,j->resource_ticket);
        if(!off)j->resource_ticket=0;
    }
    if(off)x9_audio_resources_fault(X9AudioResourceMic,j->resource_ticket,off);
    if(digital && (signal_diagnostic.flags&X9_MIC_SIGNAL_ATTEMPTED)) {
        if(off){signal_diagnostic.flags|=X9_MIC_SIGNAL_RETAINED;
            if(!signal_diagnostic.restore_errno)signal_diagnostic.restore_errno=off;}
        else signal_diagnostic.flags|=X9_MIC_SIGNAL_RESTORED;
    }
    if(dma) {
        if(!dma->last_errno)dma->last_errno=e;
        absolutetime_to_nanoseconds(mach_absolute_time()-start,&dma->elapsed_ns);
        /* Never read VUL1 after restore releases the AFE mapping's clocks. */
        if(off){dma->restore_errno=off;dma->flags|=X9_MIC_DMA_RETAINED;}
        else if(dma->flags&X9_MIC_DMA_ATTEMPTED)dma->flags|=X9_MIC_DMA_RESTORED;
    }
    if(digital) {
        if(off){digital->flags|=X9_MIC_DIGITAL_RETAINED;
            if(!digital->restore_errno)digital->restore_errno=off;}
        else {digital->flags|=X9_MIC_DIGITAL_RESTORED;
            if(!e)digital->stage=X9_MIC_DIGITAL_OFF;}
        /* Preserve first failed input stage/register; on unwind-only errors
         * the cleanup helper's register remains the decisive one. */
        if(e || !off)digital->last_register=digital_reg;
        absolutetime_to_nanoseconds(mach_absolute_time()-start,&digital->elapsed_ns);
    }
    if(analog) {
        *analog=j->codec.analog;
        if(!analog->version)analog->version=1;
        if(off && j->codec.analog_owned) {
            analog->flags|=X9_MIC_ANALOG_RETAINED;
            if(!analog->restore_errno)analog->restore_errno=off;
        }
    }
    if(j->cal) {
        if(off && !(a->flags&X9_MIC_AFE_RESTORED)) {
            a->flags|=X9_MIC_AFE_RETAINED;
            if(!a->restore_errno)a->restore_errno=off;
        }
    }
    s->last_register=last_reg;h->last_register=codec_reg;
    s->restore_errno=off;
    if(off) {
        s->flags|=X9_MIC_BOARD_RETAINED;h->flags|=X9_MIC_HW_RESTORE_FAILED;
        if(!h->restore_errno)h->restore_errno=off;
    } else {
        s->flags|=X9_MIC_BOARD_RESTORED;h->flags&=~X9_MIC_HW_POWERED;
        if(!e){s->stage=X9_MIC_BOARD_OFF;h->stage=X9_MIC_HW_STOPPED;}
    }
    printf("X9 MIC %s: stage=%u flags=%x errno=%u restore=%u id=%x ready=%x vaud18=%x gpio9=%x/%x bus=%x (board pipeline only)\n",
        j->session?"CAPTURE-STOP":digital?"DIGITAL-L":analog?"ANALOG-L":a?"CALIBRATE":"POWER",s->stage,s->flags,s->last_errno,s->restore_errno,h->swcid,h->internal_ready,
        h->vaud18_monitor,s->gpio9_before,s->gpio9_after,s->bus_status);
    if(j->cal)printf("X9 MIC AFE: stage=%u flags=%x errno=%u restore=%u AP=%x/%x ACK=%x/%x VLP=%x/%x phases=%d/%d cycles=%d/%d scanned=%u\n",
        a->stage,a->flags,a->last_errno,a->restore_errno,a->ap_vote_before,a->ap_vote_after,
        a->ack_before,a->ack_after,a->vlp_before,a->vlp_after,h->chosen_phase[0],h->chosen_phase[1],
        h->cycle[0],h->cycle[1],h->scanned_phases);
    if(analog)printf("X9 MIC ANALOG: stage=%u flags=%x errno=%u restore=%u HWCID=%x/%x AIN0/ADC_L DCC 2700mV 0dB RC=%x journal=%u/%u (not physical mic/PCM proof)\n",
        analog->stage,analog->flags,analog->last_errno,analog->restore_errno,
        analog->hwcid_h,analog->hwcid_l,analog->rc_monitor,
        analog->journal_restored,analog->journal_count);
    if(digital)printf("X9 MIC DIGITAL: stage=%u flags=%x errno=%u restore=%u delay=%u/%u codecSRC=%x TX=%x/%x RX=%x/%x UL=%x MON=%x/%x journal=%u/%u,%u/%u (configuration readback, not acoustic proof)\n",
        digital->stage,digital->flags,digital->last_errno,digital->restore_errno,
        digital->delay_data,digital->delay_cycle,digital->codec_src,digital->codec_tx[0],digital->codec_tx[1],
        digital->afe_rx[0],digital->afe_rx[1],digital->afe_ul,digital->afe_monitor[0],digital->afe_monitor[1],
        digital->codec_journal_restored,digital->codec_journal_count,
        digital->afe_journal_restored,digital->afe_journal_count);
    if(dma)printf("X9 MIC VUL1: stage=%u flags=%x errno=%u restore=%u eligible=%u blockers=%x memifs=%x CPU=%x CON=%x/%x MON=%x/%x CUR=%llx/%llx (DMA flags are authoritative for start/drain)\n",
        dma->stage,dma->flags,dma->last_errno,dma->restore_errno,dma->eligible,dma->blockers,
        dma->active_memifs,dma->memif_con0,dma->con0_before,dma->con0_after,
        dma->monitor_before,dma->monitor_after,dma->cur_before,dma->cur_after);
    if(digital && (signal_diagnostic.flags&X9_MIC_SIGNAL_ATTEMPTED)) {
        const struct x9_mic_signal_status *v=&signal_diagnostic;
        printf("X9 MIC SHADOW: flags=%x errno=%u restore=%u reads=%u/%u changes=%u/%u zeros=%u/%u signed24=%d..%d,%d..%d elapsed_ns=%llu reg=%x (MMIO shadow reads, not frames/PCM/acoustic proof)\n",
            v->flags,v->last_errno,v->restore_errno,v->reads,v->target_reads,
            v->channel[0].changes,v->channel[1].changes,v->channel[0].zero_reads,v->channel[1].zero_reads,
            v->channel[0].minimum,v->channel[0].maximum,v->channel[1].minimum,v->channel[1].maximum,
            v->elapsed_ns,v->last_register);
    }
    /* Auto-unwind during failed prepare returns its original input error to
     * START, but subsequent unprepare reports only cleanup failure. A held
     * capture's stop failure must remain sticky across repeated StopIO. */
    j->finish_errno=j->capture_held?(off?off:e):off;
    return off?off:e;
}
static int board_probe(struct x9_mic_hardware_status *h,struct x9_mic_board_status *s,
    struct x9_mic_afe_status *a,struct x9_mic_analog_status *analog,
    struct x9_mic_digital_status *digital,struct x9_mic_dma_status *dma,
    volatile uint32_t *mapped_afe,bool hold)
{
    if(s->flags&X9_MIC_BOARD_ATTEMPTED)return EALREADY;
    uint64_t ticket=0;
    int admission=x9_audio_resources_claim(X9AudioResourceMic,&ticket);
    if(admission)return admission; /* no MMIO/reset/attempt consumed */
    uint64_t start=mach_absolute_time();
    /* Static journals may never outlive their persistent status surfaces. */
    bzero(&digital_afe,sizeof(digital_afe));
    if(digital)signal_diagnostic=(struct x9_mic_signal_status){.version=1};
    if(digital)*digital=(struct x9_mic_digital_status){.version=1,
        .stage=X9_MIC_DIGITAL_PREREQUISITES,.flags=X9_MIC_DIGITAL_ATTEMPTED,
        .nominal_rate=X9_MIC_RATE};
    bzero(s,sizeof(*s));s->version=1;s->flags=X9_MIC_BOARD_ATTEMPTED;s->stage=X9_MIC_BOARD_BASELINE;
    bzero(h,sizeof(*h));h->version=1;h->stage=X9_MIC_HW_RESOURCES;
    h->chosen_phase[0]=h->chosen_phase[1]=h->cycle[0]=h->cycle[1]=-1;
    struct power_journal *j=&board;bzero(j,sizeof(*j));j->status=s;
    j->hardware=h;j->analog=analog;j->digital=digital;j->dma=dma;
    j->started=start;j->session=hold;j->resource_ticket=ticket;
    j->gpio=ml_io_map(0x1002d000,0x1000);j->vlp=ml_io_map(0x1c001000,0x1000);
    j->gipc=ml_io_map(0x1df50000,0x1000);if(j->gipc)j->gipc+=0x40;
    j->gate=ml_io_map(0x1da00000,0x1000);j->bus=ml_io_map(0x1d860000,0x1000);
    j->pmif=ml_io_map(0x1c01a000,0x2000);
    int e=0;
    if(!j->gpio || !j->vlp || !j->gipc || !j->gate || !j->bus || !j->pmif)e=ENOMEM;
    if(!e) {
        save_pin(j,&j->enable,9);save_pin(j,&j->route[0],96);save_pin(j,&j->route[1],97);
        s->gpio9_before=pin_state(j,9);
        if(j->enable.mode || j->enable.dout || pin_bit(j,0x200,9))e=EBUSY;
        for(unsigned i=0;i<2 && !e;i++) {
            struct pin *p=&j->route[i];if(p->mode!=1 && (p->mode || p->dir))e=EBUSY;
        }
    }
    if(!e){s->stage=X9_MIC_BOARD_WAKE;e=wake_take(j);}
    if(!e){s->stage=X9_MIC_BOARD_BUS;e=bus_take(j);}
    if(!e){s->stage=X9_MIC_BOARD_XO;e=xo_take(j);}
    if(!e){s->stage=X9_MIC_BOARD_CODEC;e=codec_enable(j);}
    if(!e) {
        const struct x9_mic_mt6681_io io={.version=1,.flags=X9_MT6681_COLD_OWNED,
            .cookie=j,.read8=codec_read,.write8=codec_write};
        e=x9_mic_mt6681_power_on(&j->codec,&io);
        *h=j->codec.status;
        if(!e)s->flags|=X9_MIC_BOARD_POWER_OK;
    }
    if(!e && a) {
        j->cal=&calibration;e=afe_take(j->cal,j->gpio,a,mapped_afe);
        if(!e) {
            j->codec.io.flags=X9_MT6681_REQUIRED;
            a->stage=X9_MIC_AFE_SCAN;
            e=x9_mic_mt6681_calibrate_powered(&j->codec,(volatile uint32_t *)j->cal->afe);
            if(!e)a->flags|=X9_MIC_AFE_CALIBRATED;
        }
        a->last_errno=e;*h=j->codec.status;
        afe_ao_snapshot(j->cal);
        if(!j->cal->stuck)afe_reg_snapshot(j->cal);
    }
    if(!e && analog) {
        e=afe_access(j->cal);
        if(!e)e=x9_mic_mt6681_analog_l(&j->codec);
        *analog=j->codec.analog;*h=j->codec.status;
    }
    if(!e && digital) {
        e=digital_afe_preflight(j->cal,digital,h);
        if(!e && dma){e=digital_dma_snapshot(j->cal,dma,false);
            if(e)digital->last_register=0x80000000u|dma->last_register;}
        if(!e){e=x9_mic_mt6681_digital_l(&j->codec,digital);
            if(e)digital->last_register=j->codec.status.last_register;}
        if(!e)e=digital_afe_enable(j->cal);
        if(!e){e=x9_mic_mt6681_verify_capture(&j->codec);
            if(e)digital->last_register=j->codec.status.last_register;}
        if(!e)e=digital_signal_observe(j->cal);
        *h=j->codec.status;
    }
    if(digital)digital->last_errno=e;
    s->last_errno=e;if(e && !h->last_errno)h->last_errno=e;
    if(hold && !e){j->capture_held=true;return 0;} /* caller owns real RX/TX until DMA stop/drain */
    return board_finish(j,e);
}

int x9_mic_plj110_power_probe(struct x9_mic_hardware_status *h,struct x9_mic_board_status *s)
{return board_probe(h,s,NULL,NULL,NULL,NULL,NULL,false);}
int x9_mic_plj110_calibrate(struct x9_mic_hardware_status *h,struct x9_mic_board_status *s,
    struct x9_mic_afe_status *a)
{return board_probe(h,s,a,NULL,NULL,NULL,NULL,false);}
int x9_mic_plj110_analog_l(struct x9_mic_hardware_status *h,struct x9_mic_board_status *s,
    struct x9_mic_afe_status *a,struct x9_mic_analog_status *analog)
{return board_probe(h,s,a,analog,NULL,NULL,NULL,false);}
int x9_mic_plj110_digital_l(struct x9_mic_hardware_status *h,struct x9_mic_board_status *s,
    struct x9_mic_afe_status *a,struct x9_mic_analog_status *analog,struct x9_mic_digital_status *digital,
    struct x9_mic_dma_status *dma)
{return board_probe(h,s,a,analog,digital,dma,NULL,false);}

int x9_mic_plj110_capture_prepare(volatile uint32_t *afe,
    struct x9_mic_hardware_status *h,struct x9_mic_board_status *s,
    struct x9_mic_afe_status *a,struct x9_mic_analog_status *analog,
    struct x9_mic_digital_status *digital,struct x9_mic_dma_status *dma)
{
    if(!afe || !h || !s || !a || !analog || !digital || !dma)return EINVAL;
    /* Do not adopt/close a prior diagnostic's transaction on rejection. */
    if(s->flags&X9_MIC_BOARD_ATTEMPTED)return EALREADY;
    return board_probe(h,s,a,analog,digital,dma,afe,true);
}
int x9_mic_plj110_capture_unprepare(void)
{
    if(!board.session)return 0;
    return board_finish(&board,board.status->last_errno);
}

void x9_mic_plj110_capture_retain(int error)
{
    /* DMA/route drain failed before board_finish: fault the same held lease
     * without MMIO or teardown, so ADSP/speaker cannot dismantle parents. */
    if(board.session && board.resource_ticket) {
        x9_audio_resources_fault(X9AudioResourceMic,board.resource_ticket,error);
        if(signal_diagnostic.flags&X9_MIC_SIGNAL_ATTEMPTED) {
            signal_diagnostic.flags|=X9_MIC_SIGNAL_RETAINED;
            if(!signal_diagnostic.restore_errno)signal_diagnostic.restore_errno=error?error:EIO;
        }
    }
}
void x9_mic_plj110_get_signal(struct x9_mic_signal_status *s)
{*s=signal_diagnostic;}
