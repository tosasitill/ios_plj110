/* Included ONLY by x9_tfa_plj110.c, sharing i2c6_lock but not probe state.
 * OPPO i2c-mt65xx.c: FIFO v2 byte DATA, WRRD 1/2/2, WR 3/0/1,
 * CLK_EXT/ACKERR, auto_restart START bit14, no remaining message bit15.
 * No Linux timeout reset/retry/arbitration recovery is transplanted here. */
#include "x9_i2c6_timing.h"
enum { NNone,NPrepareIssued,NFilling,NStarted,NCompleted,NAckIssued,
       NAcked,NFifoIssued,NFifoEmpty,NStartClearIssued };
struct native_field {uint32_t saved,expected,prior;};
static struct {
    struct x9_i2c6_policy policy;
    vm_offset_t gpio,gate,perao,bus,dma,top,mainpll,univpll;
    struct native_field field[sizeof(regs)/sizeof(regs[0])];
    uint32_t shadow[sizeof(regs)/sizeof(regs[0])];
    uint32_t pin[2],parent,dma_parent,pcw,dma_pcw,hz,timing_hz;
    uint32_t error,dirty,uncertain,pending,terminal,tx_bytes;
    uint32_t slave,reg,reading,bank_start[2],bank_status[2],bank_fifo[2];
    uint64_t requests,completed,write_attempts;
    uint64_t child;
    bool clock_saved,captured,configured,teardown,restored;
} ni;
static int n_fault(int e)
{
    if(e==EAGAIN)e=EIO;if(!ni.error)ni.error=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,ni.policy.ticket,e);return e;
}
static int n_match(uint64_t ticket,uint64_t lease)
{return !native_bus_held||!ticket||ticket!=ni.policy.ticket||!lease||lease!=ni.child?EPERM:0;}
static int n_online(void)
{
    X9AudioDSPStatus ds={0};int e=x9_adsp_mt6993_speaker_validate(ni.policy.ticket);
    if(!e)e=x9_adsp_status(0,&ds);
    return e?e:ds.state!=X9DSPOnline||ds.error||ds.generation!=ni.policy.generation?ESTALE:0;
}
static int n_authority(void)
{
    int e=x9_pa_reset_bus_validate(ni.policy.ticket,ni.policy.reset_epoch,ni.child);
    if(!e)e=ni.policy.validate_controller(ni.policy.context,ni.policy.ticket,ni.policy.generation);
    if(!e)e=x9_pa_reset_bus_validate(ni.policy.ticket,ni.policy.reset_epoch,ni.child);return e;
}
static int n_mappings(void)
{
    const vm_offset_t va[]={ni.gpio,ni.gate,ni.perao,ni.bus,ni.dma,ni.top,ni.mainpll,ni.univpll};
    const uint64_t pa[]={0x1002d000,0x129d0000,0x16640000,0x12990000,
        0x16410000,0x1c210000,0x1c213000,0x1c213100};
    const unsigned bytes[]={0x1000,0x1000,0x1000,0x20000,0x10000,0x1000,0x100,0x100};
    if(PAGE_SIZE!=0x4000)return ENXIO;
    for(unsigned i=0;i<8;i++) {
        if(!va[i])return ENXIO;
        for(unsigned n=0;n<bytes[i];n+=0x1000)for(unsigned end=0;end<2;end++) {
            unsigned off=n+(end?(bytes[i]-n<0x1000?bytes[i]-n:0x1000)-1:0);
            addr64_t a=(addr64_t)va[i]+off;ppnum_t p=pmap_find_phys(kernel_pmap,a);
            if(!p||ptoa_64(p)+(a&PAGE_MASK)!=pa[i]+off)return EFAULT;
        }
    }
    return 0;
}
static uint32_t n_pin(unsigned pin)
{
    return ((rr(ni.gpio,0x300+(pin/4)*16)>>((pin%4)*8))&31)|
        (((rr(ni.gpio,(pin/32)*16)>>(pin%32))&1)<<8)|
        (((rr(ni.gpio,0x100+(pin/32)*16)>>(pin%32))&1)<<9);
}
static int n_clocks(void)
{
    /* These readbacks supplement, NEVER replace, the mandatory pinned
     * controller supplier. Do not access clock-off CSR/DMA banks. */
    if((rr(ni.gate,0xe00)&4)||(rr(ni.perao,0x14)&(1u<<26)))return ENXIO;
    /* OPPO TOP_MUX_I2C_WEST_SHIFT5 / PERI_AXI_SHIFT1. */
    if((rr(ni.top,0xc)&(1u<<5))||(rr(ni.top,4)&(1u<<1)))return EBUSY;
    if((rr(ni.top,0x338)&(1u<<26))||!(rr(ni.top,0x278)&(1u<<28)))return ENXIO;
    uint32_t cfg=rr(ni.top,0x110),parent=(cfg>>24)&3;
    uint32_t acfg=rr(ni.top,0x10),ap=(acfg>>8)&3;
    if((cfg&(1u<<31))||(acfg&(1u<<15))||ap==1||
       (rr(ni.top,0x330)&(1u<<30))||!(rr(ni.top,0x270)&(1u<<30)))return ENXIO;
    vm_offset_t pll=parent==2?ni.mainpll:ni.univpll;
    if((parent&&!(rr(pll,0x34)&(1u<<4)))||(ap&&!(rr(ni.mainpll,0x34)&(1u<<4))))return ENXIO;
    uint32_t pcw=parent?rr(pll,0xc):0,apcw=ap?rr(ni.mainpll,0xc):0;
    if(ni.clock_saved&&(parent!=ni.parent||ap!=ni.dma_parent||pcw!=ni.pcw||apcw!=ni.dma_pcw))return EBUSY;
    uint64_t hz=x9_i2c6_source_hz(parent,pcw);
    if(!hz)return ENOTSUP;
    for(unsigned i=0;i<2;i++) {
        uint32_t v=n_pin(114+i);
        if((v&31)!=1||(ni.clock_saved&&v!=ni.pin[i]))return EBUSY;
        ni.pin[i]=v;
    }
    ni.parent=parent;ni.dma_parent=ap;ni.pcw=pcw;ni.dma_pcw=apcw;
    ni.hz=(uint32_t)hz;ni.clock_saved=true;return 0;
}
static int n_access(void)
{
    int e=n_authority();if(!e)e=n_mappings();if(!e)e=n_clocks();return e;
}
/* Call only AFTER fresh n_access and complete expected[] CSR validation.
 * No sampling after clock withdrawal, no second independent journal. */
static int n_timing_check(void)
{
    const struct x9_i2c6_timing p={ni.field[6].expected,ni.field[8].expected,
        ni.field[7].expected,ni.field[11].expected,ni.field[9].expected,
        ni.field[10].expected,ni.field[12].expected,ni.field[13].expected};
    uint32_t rate=x9_i2c6_timing_rate(ni.hz,&p);
    if(!rate || rate>98000)return ENOTSUP;
    ni.timing_hz=rate;return 0;
}
static int n_guard(bool recovering)
{
    int e=n_access();if(e)return e;
    if(!ni.captured)return ENXIO;
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        struct native_field *f=&ni.field[i];uint32_t v=rr(ni.bus,regs[i]);
        if(v!=f->expected&&!(recovering&&(ni.uncertain&(1u<<i))&&v==f->prior))return EBUSY;
        if(rr(ni.bus+0x10000,regs[i])!=ni.shadow[i])return EBUSY;
    }
    /* No shared/shadow/IRQ/DMA takeover during this owner's FIFO request. */
    for(unsigned i=0;i<2;i++) {
        ni.bank_start[i]=rr(ni.bus+i*0x10000,START);
        ni.bank_status[i]=rr(ni.bus+i*0x10000,STAT);
        ni.bank_fifo[i]=rr(ni.bus+i*0x10000,FIFO_STATUS);
    }
    if(rr(ni.bus,MASK)||rr(ni.bus,MULTI)||ni.bank_start[1]||
       rr(ni.bus+0x10000,MASK)||ni.bank_status[1]||
       (ni.bank_fifo[1]&31)||rr(ni.bus+0x10000,MULTI)||
       (rr(ni.dma,8)&1))return EBUSY;
    return ni.configured?n_timing_check():0;
}
static int n_idle(bool recovering)
{
    int e=ni.captured?n_guard(recovering):n_access();if(e)return e;
    for(unsigned i=0;i<2;i++) {
        vm_offset_t b=ni.bus+i*0x10000;
        ni.bank_start[i]=rr(b,START);ni.bank_status[i]=rr(b,STAT);ni.bank_fifo[i]=rr(b,FIFO_STATUS);
        if(ni.bank_start[i]||rr(b,MASK)||ni.bank_status[i]||(ni.bank_fifo[i]&31)||
           rr(b,MULTI)||(rr(b,CTRL)&(1u|4u|64u|128u|256u|512u)))return EBUSY;
    }
    if(rr(ni.dma,8)&1)return EBUSY;
    for(unsigned pin=114;pin<=115;pin++)if(!(rr(ni.gpio,0x200+(pin/32)*16)&(1u<<(pin%32))))return EBUSY;
    return 0;
}
static int n_put(unsigned reg,uint32_t value,bool recovering)
{
    unsigned i;for(i=0;i<sizeof(regs)/sizeof(regs[0]);i++)if(regs[i]==reg)break;
    if(i==sizeof(regs)/sizeof(regs[0]))return EINVAL;
    int e=n_idle(recovering);if(e)return e;
    struct native_field *f=&ni.field[i];uint32_t bit=1u<<i,old=rr(ni.bus,reg);
    if(old!=f->expected&&!(recovering&&(ni.uncertain&bit)&&old==f->prior))return EBUSY;
    if(old==value){f->expected=old;ni.uncertain&=~bit;return 0;}
    f->prior=old;f->expected=value;ni.dirty|=bit;ni.uncertain|=bit;
    wr(ni.bus,reg,value);e=n_access();if(e)return e;
    if(rr(ni.bus,reg)!=value)return EIO;ni.uncertain&=~bit;return n_guard(false);
}
static int n_timing(void)
{
    struct x9_i2c6_timing p;
    if(!x9_i2c6_timing_make(ni.hz,&p))return ENOTSUP;
    /* Complete rational rate/phase/EXT proof before FIRST write. */
    const unsigned r[]={TIMING,EXT,LTIMING,HS,SDA,DIV,IO,MIS,DELAY};
    const uint32_t v[]={p.timing,p.ext,p.low,p.hs,p.sda,p.div,p.io,p.mis,2};
    for(unsigned i=0;i<sizeof(r)/sizeof(r[0]);i++){int e=n_put(r[i],v[i],false);if(e)return e;}
    int e=n_idle(false);if(!e)e=n_timing_check();return e;
}
/* Resume only acknowledgements of observed effects, not action replay.
 * NStarted + START0/STAT0 is deliberately UNRESOLVED: idle does not prove
 * a possibly late START cannot produce another completion. */
static int n_finish(void)
{
    int e=n_guard(false);if(e)return e;
    if(ni.pending==NStarted) {
        uint32_t st=rr(ni.bus,STAT),start=rr(ni.bus,START);
        if(!st||(start&1))return EBUSY;
        if((st&~7u)||(start&~(1u<<14)))return EIO;
        ni.terminal=st;ni.pending=NCompleted;
    }
    if(ni.pending==NCompleted) {
        if(rr(ni.bus,STAT)!=ni.terminal||(rr(ni.bus,START)&~(1u<<14)))return EBUSY;
        ni.pending=NAckIssued;wr(ni.bus,STAT,ni.terminal);
    }
    if(ni.pending==NAckIssued) {
        e=n_guard(false);if(e)return e;
        if(rr(ni.bus,STAT))return EBUSY;ni.pending=NAcked;
    }
    if(ni.pending==NAcked) {
        if(rr(ni.bus,STAT)||(rr(ni.bus,START)&~(1u<<14)))return EBUSY;
        ni.pending=NFifoIssued;wr(ni.bus,FIFO_CLR,1);
    }
    if(ni.pending==NFifoIssued) {
        e=n_guard(false);if(e)return e;
        if((rr(ni.bus,FIFO_STATUS)&31)||rr(ni.bus,STAT))return EBUSY;
        ni.pending=NFifoEmpty;
    }
    if(ni.pending==NFifoEmpty) {
        if(rr(ni.bus,STAT)||(rr(ni.bus,START)&~(1u<<14)))return EBUSY;
        ni.pending=NStartClearIssued;wr(ni.bus,START,0);
    }
    if(ni.pending==NStartClearIssued) {
        e=n_guard(false);if(e)return e;
        if(rr(ni.bus,START))return EBUSY;ni.pending=NNone;
    }
    return ni.pending?EBUSY:n_idle(false);
}
static int n_transfer(uint8_t slave,uint8_t reg,uint16_t *value,bool reading)
{
    if(slave!=0x34&&slave!=0x35)return EINVAL;
    if(!ni.configured||ni.teardown)return ENXIO;
    if(ni.pending||ni.uncertain)return EBUSY;
    int e=n_idle(false);if(e)return e;
    if(ni.requests==UINT64_MAX||(!reading&&ni.write_attempts==UINT64_MAX))return EOVERFLOW;
    ni.slave=slave;ni.reg=reg;ni.reading=reading;
    ni.terminal=ni.tx_bytes=0;
    const unsigned r[]={CTRL,ADDR,LEN,AUX,TRANS};
    const uint32_t v[]={ (ni.field[0].saved&~(2u|4u|16u|64u|128u|256u|512u))|
        8u|32u|(reading?2u|16u:0),slave<<1,reading?1u:3u,reading?2u:0u,reading?2u:1u };
    for(unsigned i=0;i<5;i++){e=n_put(r[i],v[i],false);if(e)return e;}
    e=n_idle(false);if(e)return e;
    ni.terminal=ni.tx_bytes=0;ni.pending=NPrepareIssued;wr(ni.bus,FIFO_CLR,1);
    e=n_guard(false);if(e)return e;
    if((rr(ni.bus,FIFO_STATUS)&31)||rr(ni.bus,START)||rr(ni.bus,STAT))return EIO;
    ni.pending=NFilling;
    const uint8_t data[]={reg,(uint8_t)(*value>>8),(uint8_t)*value};
    for(unsigned i=0;i<(reading?1u:3u);i++) {
        e=n_guard(false);if(e)return e;
        if(rr(ni.bus,START)||rr(ni.bus,STAT)||(rr(ni.bus,FIFO_STATUS)&31)!=ni.tx_bytes)return EBUSY;
        ni.tx_bytes++; /* journal BEFORE the possibly landed byte write */
        *(volatile uint8_t *)(ni.bus+DATA)=data[i];__asm__ volatile("dsb sy":::"memory");
        e=n_guard(false);if(e)return e;
        if((rr(ni.bus,FIFO_STATUS)&31)!=ni.tx_bytes)return EIO;
    }
    e=n_guard(false);if(e)return e;
    if(rr(ni.bus,START)||rr(ni.bus,STAT))return EBUSY;
    for(unsigned pin=114;pin<=115;pin++)if(!(rr(ni.gpio,0x200+(pin/32)*16)&(1u<<(pin%32))))return EBUSY;
    ni.pending=NStarted;ni.requests++;if(!reading)ni.write_attempts++;
    wr(ni.bus,START,(1u<<14)|1);
    uint64_t deadline;clock_interval_to_deadline(5,NSEC_PER_MSEC,&deadline);
    for(unsigned i=0;i<1000;i++) {
        e=n_guard(false);if(e)return e;
        uint32_t st=rr(ni.bus,STAT),start=rr(ni.bus,START);
        if((st&~7u)||(start&~((1u<<14)|1u)))return EIO;
        if(st&&!(start&1)){ni.terminal=st;ni.pending=NCompleted;break;}
        if(mach_absolute_time()>=deadline)break;delay(5);
    }
    if(ni.pending!=NCompleted)return ETIMEDOUT;
    int outcome=ni.terminal==1?0:EIO;uint16_t result=0;
    if(!outcome) {
        e=n_guard(false);if(e)return e;
        if(rr(ni.bus,STAT)!=ni.terminal||(rr(ni.bus,START)&~(1u<<14)))return EBUSY;
        if((rr(ni.bus,FIFO_STATUS)&31)!=(reading?2u:0u))return EIO;
        if(reading) {
            result=(uint16_t)((unsigned)*(volatile uint8_t *)(ni.bus+DATA)<<8);
            __asm__ volatile("dmb oshld":::"memory");
            e=n_guard(false);if(e)return e;
            if(rr(ni.bus,STAT)!=ni.terminal||(rr(ni.bus,START)&~(1u<<14)))return EBUSY;
            if((rr(ni.bus,FIFO_STATUS)&31)!=1)return EIO;
            result|=*(volatile uint8_t *)(ni.bus+DATA);__asm__ volatile("dmb oshld":::"memory");
            e=n_guard(false);if(e)return e;
            if(rr(ni.bus,FIFO_STATUS)&31)return EIO;
        }
    }
    e=n_finish();if(e)return e;
    if(!outcome&&ni.completed!=UINT64_MAX)ni.completed++;
    if(!outcome&&reading)*value=result;return outcome;
}
int x9_i2c6_acquire(const struct x9_i2c6_policy *p,uint64_t *lease)
{
    if(!lease)return EINVAL;*lease=0;
    if(!p||p->version!=X9_I2C6_POLICY_VERSION||!p->ticket||!p->generation||
       !p->reset_epoch||!p->validate_controller)return EINVAL;
    if(!sweet_early_is_x9()||sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&i2c6_lock);
    int e=native_bus_held||(s.flags&(X9PAPoisoned|X9PAJournalHeld))?EBUSY:0;
    struct x9_adsp_handoff h={0};if(!e)e=x9_adsp_mt6993_copy_handoff(&h);
    /* This metadata already checks mt6991-i2c and BOTH reg tuples, slaves
     * and actual reset controller. Clock/pad exclusivity is NOT metadata. */
    if(!e&&(!x9_adsp_handoff_valid(&h)||!(h.flags&X9_ADSP_PA_RESET_VERIFIED)))e=ENXIO;
    uint64_t child=0;if(!e)e=x9_pa_reset_bus_hold(p->ticket,p->reset_epoch,&child);
    if(!e) {
        ni.policy=*p;ni.child=child;native_bus_held=true;*lease=child;
        ni.error=ni.dirty=ni.uncertain=ni.pending=ni.terminal=ni.tx_bytes=0;
        ni.slave=ni.reg=ni.reading=0;ni.requests=ni.completed=ni.write_attempts=0;
        memset(ni.bank_start,0,sizeof(ni.bank_start));memset(ni.bank_status,0,sizeof(ni.bank_status));
        memset(ni.bank_fifo,0,sizeof(ni.bank_fifo));
        ni.clock_saved=ni.captured=ni.configured=ni.teardown=ni.restored=false;
        ni.parent=ni.dma_parent=ni.pcw=ni.dma_pcw=ni.hz=ni.timing_hz=0;
        memset(ni.pin,0,sizeof(ni.pin));
        e=n_online();if(!e)e=n_authority();if(!e)e=n_online();
        if(!e) {
            if(!ni.gpio)ni.gpio=ml_io_map(0x1002d000,0x1000);
            if(!ni.gate)ni.gate=ml_io_map(0x129d0000,0x1000);
            if(!ni.perao)ni.perao=ml_io_map(0x16640000,0x1000);
            if(!ni.bus)ni.bus=ml_io_map(0x12990000,0x20000);
            if(!ni.dma)ni.dma=ml_io_map(0x16410000,0x10000);
            if(!ni.top)ni.top=ml_io_map(0x1c210000,0x1000);
            if(!ni.mainpll)ni.mainpll=ml_io_map(0x1c213000,0x100);
            if(!ni.univpll)ni.univpll=ml_io_map(0x1c213100,0x100);
            e=n_idle(false);
        }
        if(!e) {
            for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
                ni.field[i].saved=ni.field[i].prior=ni.field[i].expected=rr(ni.bus,regs[i]);
                ni.shadow[i]=rr(ni.bus+0x10000,regs[i]);
            }
            ni.captured=true;e=n_idle(false);if(!e)e=n_timing();
            if(!e)e=n_online();if(!e)e=n_idle(false);if(!e)ni.configured=true;
        }
        if(e)e=n_fault(e); /* caller explicitly halts and releases retained child */
    }
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_i2c6_validate(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&i2c6_lock);int e=n_match(ticket,lease);
    if(!e)e=!ni.configured||ni.teardown||ni.pending||ni.uncertain?ENXIO:n_idle(false);
    if(e&&n_match(ticket,lease)==0)e=n_fault(e);
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_i2c6_read16(uint64_t ticket,uint64_t lease,uint8_t slave,uint8_t reg,uint16_t *out)
{
    if(!out||(slave!=0x34&&slave!=0x35))return EINVAL;
    lck_mtx_lock(&i2c6_lock);int e=n_match(ticket,lease);uint16_t v=0;
    if(!e)e=n_transfer(slave,reg,&v,true);
    if(e&&n_match(ticket,lease)==0)e=n_fault(e);if(!e)*out=v;
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_i2c6_write16(uint64_t ticket,uint64_t lease,uint8_t slave,uint8_t reg,uint16_t value)
{
    if(slave!=0x34&&slave!=0x35)return EINVAL;
    lck_mtx_lock(&i2c6_lock);int e=n_match(ticket,lease);
    if(!e)e=n_transfer(slave,reg,&value,false);
    if(e&&n_match(ticket,lease)==0)e=n_fault(e);
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_i2c6_release(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&i2c6_lock);int e=n_match(ticket,lease);
    if(!e) {
        ni.teardown=true;ni.configured=false;
        e=x9_pa_reset_bus_quiesced_validate(ticket,ni.policy.reset_epoch,lease);
    }
    if(!e)e=n_authority();
    if(!e&&!ni.restored&&ni.captured) {
        e=n_guard(true);
        if(!e&&ni.pending==NPrepareIssued) {
            if(rr(ni.bus,START)||rr(ni.bus,STAT)||(rr(ni.bus,FIFO_STATUS)&31))e=EBUSY;
            if(!e)ni.pending=NNone; /* observe initial clear; no replay */
        }
        if(!e&&ni.pending==NFilling) {
            /* No START attempted: only our queued bytes can be discarded.
             * No DATA byte is ever reissued, even when count is short. */
            if(rr(ni.bus,START)||rr(ni.bus,STAT)||
               (rr(ni.bus,FIFO_STATUS)&31)>ni.tx_bytes)e=EBUSY;
            if(!e)ni.pending=NAcked;
        }
        if(!e&&ni.pending)e=n_finish();
        if(!e)e=n_idle(true);
        for(unsigned i=sizeof(regs)/sizeof(regs[0]);!e&&i;i--)if(ni.dirty&(1u<<(i-1))) {
            e=n_put(regs[i-1],ni.field[i-1].saved,true);
            if(!e)ni.dirty&=~(1u<<(i-1));
        }
        if(!e)e=n_idle(false);
        if(!e)ni.restored=true;
    }
    /* A failed parent drop does NOT permit a second restore. Re-prove the
     * saved/idle controller under the still-pinned authority before retry. */
    if(!e&&ni.captured&&ni.restored)e=n_idle(false);
    if(!e)e=x9_pa_reset_bus_drop(ticket,ni.policy.reset_epoch,lease);
    if(!e){native_bus_held=false;ni.child=0;}
    else if(n_match(ticket,lease)==0)e=n_fault(e);
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_i2c6_status(X9AudioI2C6Status *out)
{
    if(!out)return EINVAL;lck_mtx_lock(&i2c6_lock);memset(out,0,sizeof(*out));
    out->version=X9_I2C6_STATUS_VERSION;
    out->flags=(native_bus_held?X9I2C6Held:0)|(ni.captured?X9I2C6Captured:0)|
        (ni.configured?X9I2C6Configured:0)|(ni.teardown?X9I2C6Teardown:0)|(ni.restored?X9I2C6Restored:0);
    out->generation=ni.policy.generation;out->error=ni.error;out->dirty=ni.dirty;out->uncertain=ni.uncertain;
    out->pending=ni.pending;out->terminal=ni.terminal;
    out->parent=ni.parent;out->dma_parent=ni.dma_parent;out->nominal_hz=ni.hz;out->timing_hz=ni.timing_hz;
    out->slave=ni.slave;out->reg=ni.reg;out->reading=ni.reading;out->tx_bytes=ni.tx_bytes;
    out->requests=ni.requests;out->completed=ni.completed;out->write_attempts=ni.write_attempts;
    memcpy(out->bank_start,ni.bank_start,sizeof(out->bank_start));
    memcpy(out->bank_status,ni.bank_status,sizeof(out->bank_status));
    memcpy(out->bank_fifo,ni.bank_fifo,sizeof(out->bank_fifo));
    lck_mtx_unlock(&i2c6_lock);return 0;
}
