/* PLJ110 cold AUDIO/26M register-clock -> disabled OUT4 -> reverse restore.
 * OPPO mt6993-afe-clk.c, clk-mt6993{,-pd,-adsp}.c and captured live DT.
 * Optional preconfigured APLL2/meter cycle; no pad/PA/DMA writes or engine
 * enable, and no fabricated physical backend.
 * Separate board mutex -> short AP resource leaf; no callbacks to IOService.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <mach/vm_types.h>
#include "x9_audio_resources.h"
#include "x9_adsp_mt6993.h"
#include "x9_adsp_transport.h"
#include "x9_speaker_mt6993.h"
#include "x9_speaker_pll.h"
#include "x9_speaker_path.h"
#include "x9_speaker_dl24.h"
#include "x9_speaker_out4.h"
#include "x9_mic_mt6993_regs.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
extern void IOSleep(unsigned int);
/* Log 264: the SoC stopped inside clock acquire. Each step's line is
 * followed by a pause so the USB console drains before the next access. */
#define CLKSTEP(name) do{printf("X9 SPK CLK: %s\n",name);IOSleep(600);}while(0)
LCK_GRP_DECLARE(x9_speaker_clock_group,"x9-speaker-clock");
LCK_MTX_DECLARE(x9_speaker_clock_lock,&x9_speaker_clock_group);
static X9AudioClockStatus s={.version=X9_AUDIO_CLOCK_STATUS_VERSION};
static struct {
    vm_offset_t hwv,vlp,afe,apll;
    uint32_t mux_owned, mux_expected[2];
    bool captured, stuck,combined;
} j;
struct field {unsigned off;uint32_t mask,value,saved,expected;};
/* Gate bank1 bits0/1: HOPPING/F26M. OUT4/other clocks remain untouched. */
static struct field fields[]={
    {0x10,3,0,0,0}, X9_SPEAKER_OUT4_DISABLED_CONFIG
};
#define FIELD_COUNT ((unsigned)(sizeof(fields)/sizeof(fields[0])))
static uint32_t field_prior[FIELD_COUNT];
static unsigned field_count(void){return j.combined?1:FIELD_COUNT;}
static uint32_t native_engen_expected(void);
static int native_engen_idle(void);
/* Log 265: the SoC stopped between "map done, AO snapshot" and "APLL
 * capture". While `trace` is set every access is printed (physical address)
 * BEFORE it is issued, then paced, so the last line names the access. */
static bool trace;
static const char *trace_name(vm_offset_t b,unsigned long *phys);
static void trace_access(const char *op,vm_offset_t b,unsigned r,uint32_t v)
{
    unsigned long phys=0;const char *n=trace_name(b,&phys);
    printf("X9 SPK CLK %s %s+%x (%lx) %x\n",op,n,r,phys+r,v);IOSleep(300);
}
static uint32_t rd(vm_offset_t b,unsigned r)
{
    if(trace)trace_access("rd",b,r,0);
    uint32_t v=*(volatile uint32_t *)(b+r);__asm__ volatile("dmb oshld":::"memory");
    if(trace)printf("X9 SPK CLK    = %x\n",v);
    return v;
}
static void wr(vm_offset_t b,unsigned r,uint32_t v)
{if(trace)trace_access("wr",b,r,v);*(volatile uint32_t *)(b+r)=v;__asm__ volatile("dsb sy":::"memory");}
static int wait_bits(vm_offset_t b,unsigned r,uint32_t mask,uint32_t value,unsigned us)
{
    uint64_t ticks;nanoseconds_to_absolutetime((uint64_t)us*1000,&ticks);
    uint64_t start=mach_absolute_time();
    for(unsigned n=0;n<us;n++) {
        if((rd(b,r)&mask)==value)return 0;
        if(mach_absolute_time()-start>=ticks)break;delay(1);
    }
    s.last_register=r;s.last_value=rd(b,r);return ETIMEDOUT;
}
static int refuse(vm_offset_t b,unsigned r,uint32_t mask,uint32_t wanted)
{
    uint32_t v=rd(b,r);
    if((v&mask)==wanted)return 0;
    s.last_register=r;s.last_value=v;return EBUSY;
}
/* Log 268: reading 0x1c080708 (ap_hwv + 0x40708, the old "XPU2 EN") stopped
 * the SoC. Those per-XPU offsets belong to mm_hwv (0x31c00000, as the camera
 * driver uses them), not ap_hwv. Derive foreign votes from the aggregate:
 * PD_ALL (0x13700, OR of every voter) minus our AP EN (0x20708), both
 * already read safely by the ADSP path. Same AUDIO/TOP/INFRA mask as before. */
static uint32_t foreign(void)
{
    return (rd(j.hwv,0x13700)&~rd(j.hwv,0x20708))&7;
}
static const char *trace_name(vm_offset_t b,unsigned long *phys)
{
    if(b==j.hwv){*phys=0x1c040000;return "hwv";}
    if(b==j.vlp){*phys=0x1c200000;return "vlp";}
    if(b==j.afe){*phys=0x1a110000;return "afe";}
    if(b==j.apll){*phys=0x1c203100;return "apll";}
    *phys=0;return "?";
}
static void ao_snapshot(void)
{
    if(!(s.flags&X9ClockAOCaptured))return;
    s.ap_after=rd(j.hwv,0x20708);s.all_after=rd(j.hwv,0x13700);
    s.ack_after=rd(j.hwv,0x12900);s.done=rd(j.hwv,0x1132c);
    s.foreign_after=foreign();s.vlp_after=rd(j.vlp,0x90);s.fenc_after=rd(j.vlp,0x274);
}
static int access(void)
{
    /* Native acquisition borrows the ACTUAL board owner, not a probe flag.
     * Its AO-safe board qualification precedes any powered-domain read. */
    int e=j.combined?x9_adsp_mt6993_speaker_validate(s.ticket):0;
    if(!e)e=refuse(j.hwv,0x20708,13,13);
    if(!e)e=refuse(j.hwv,0x12900,13,13);
    if(!e)e=refuse(j.hwv,0x13700,13,13);
    if(!e)e=refuse(j.hwv,0x20708,2,j.combined?2:0);
    if(!e)e=refuse(j.hwv,0x13700,2,j.combined?2:0);
    if(!e)e=refuse(j.hwv,0x13704,13,0);
    if(!e)e=refuse(j.hwv,0x1132c,13,13);
    if(!e)e=refuse(j.vlp,0x90,0x8383,j.mux_expected[0]|j.mux_expected[1]);
    if(!e)e=refuse(j.vlp,0x274,0xc0000000,0xc0000000);
    /* Mux SEL/FENC alone cannot qualify a native lease while a foreign
     * UPDATE is in flight. OPPO VLP_CKSTA_REG1: INTBUS30 / AUDIO_H29. */
    if(!e && j.combined)e=refuse(j.vlp,0x334,0x60000000,0);
    if(!e && foreign())e=EBUSY;
    if(e)j.stuck=true;return e;
}
static int idle(void)
{
    int e=access();if(e)return e;
    /* Cold domain was proven before power; reject all 32 memif enables,
     * 7 IN + 7 OUT engines, SRC/test/sine/pad TX and engine monitor activity.
     * Never infer authoritative DMA drain merely from a disabled memif. */
    const unsigned engines[]={0x14,0x18,0x1c};
    for(unsigned n=0;!e && n<3;n++)e=j.combined && !n?native_engen_idle():
        refuse(j.afe,engines[n],15,0);
    /* AUDIO_ENGEN_MON has no documented bit mapping. With our own APLL2
     * generator active it is NOT a zero/DMA-idle certificate. Native clock
     * release separately requires the authoritative drain callback. */
    if(!e && (!j.combined || !native_engen_expected()))e=refuse(j.afe,0x7c,UINT32_MAX,0);
    const unsigned src[]={0xbd0,0xc50,0xcd0,0x98,0x1204};
    for(unsigned n=0;!e && n<5;n++)e=refuse(j.afe,src[n],1,0);
    if(!e)e=refuse(j.afe,0xf14,0xfff,0);
    if(!e)e=refuse(j.afe,0x20,1u<<26,0);
    for(unsigned n=0;!e && n<7;n++) {
        e=refuse(j.afe,0x1300+n*0x30,1,0);
        if(!e)e=refuse(j.afe,0x1480+n*0x40,1,0);
    }
    for(unsigned n=0;!e && n<sizeof(x9_mic_memif_regs)/sizeof(x9_mic_memif_regs[0]);n++)
        e=refuse(j.afe,x9_mic_memif_regs[n],VUL1_ON,0);
    if(e)j.stuck=true;return e;
}
static int smc(unsigned op,unsigned slot)
{
    if(j.combined) {
        int e=x9_adsp_mt6993_speaker_validate(s.ticket);if(e){j.stuck=true;return e;}
    }
    register uint64_t x0 __asm__("x0")=0xc2000517ULL;
    register uint64_t x1 __asm__("x1")=op,x2 __asm__("x2")=0,x3 __asm__("x3")=0;
    register uint64_t x4 __asm__("x4")=0,x5 __asm__("x5")=0,x6 __asm__("x6")=0,x7 __asm__("x7")=0;
    __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::"memory");
    s.smc_result[slot]=x0;
    /* Unknown secure mutation cannot be repaired by dropping its clocks. */
    if(x0){j.stuck=true;return (int64_t)x0==-1?ENOTSUP:EIO;}
    if(j.combined) {
        int e=x9_adsp_mt6993_speaker_validate(s.ticket);if(e){j.stuck=true;return e;}
    }
    return 0;
}
static int vote(unsigned bit,bool on)
{
    int e=wait_bits(j.hwv,0x1132c,bit,bit,10000);if(e)return e;
    if(on)s.owned_vote|=bit; /* BEFORE a possibly late SET */
    wr(j.hwv,on?0x20700:0x20704,bit);
    e=wait_bits(j.hwv,0x20708,bit,on?bit:0,10000);
    if(!e)e=wait_bits(j.hwv,0x1132c,bit,bit,10000);
    if(!e && on)e=wait_bits(j.hwv,0x12900,bit,bit,10000);
    if(e)j.stuck=true;else if(!on)s.owned_vote&=~bit;
    return e;
}
static int mux_parent(unsigned n,uint32_t parent)
{
    uint32_t mask=3u<<(n*8),pdn=1u<<(7+n*8);
    int e=refuse(j.vlp,0x90,pdn,pdn);if(e){j.stuck=true;return e;}
    wr(j.vlp,0x98,mask);wr(j.vlp,0x94,parent&mask);wr(j.vlp,8,1u<<(n+1));
    e=wait_bits(j.vlp,0x334,1u<<(30-n),0,1000);
    if(!e)e=refuse(j.vlp,0x90,mask,parent&mask);
    if(e)j.stuck=true;else j.mux_expected[n]=(parent&mask)|pdn;
    return e;
}
static int mux_take(unsigned n)
{
    uint32_t pdn=1u<<(7+n*8),fenc=1u<<(31-n);
    int e=refuse(j.vlp,0x90,pdn,pdn);
    if(!e)e=refuse(j.vlp,0x274,fenc,0);
    if(e)return e;
    j.mux_owned|=1u<<n;e=mux_parent(n,0);if(e)return e;
    wr(j.vlp,0x98,pdn); /* direct FENC enable has no UPDATE trigger */
    e=wait_bits(j.vlp,0x274,fenc,fenc,1000);
    if(!e)e=refuse(j.vlp,0x90,pdn|(3u<<(n*8)),0);
    if(e)j.stuck=true;else j.mux_expected[n]=0;
    return e;
}
static int change(unsigned n,uint32_t value)
{
    int e=j.combined?access():0;if(e)return e;
    struct field *p=&fields[n];uint32_t old=rd(j.afe,p->off),bit=1u<<n;
    if((old&p->mask)!=(p->expected&p->mask) &&
       !((s.uncertain_mask&bit) && (old&p->mask)==(field_prior[n]&p->mask))) {
        s.last_register=p->off;s.last_value=old;return EBUSY;
    }
    uint32_t next=(old&~p->mask)|(value&p->mask);
    if((next&p->mask)==(old&p->mask)){p->expected=next;s.uncertain_mask&=~bit;return 0;}
    field_prior[n]=old;s.dirty_mask|=bit;s.uncertain_mask|=bit;p->expected=next;
    wr(j.afe,p->off,next);
    if(j.combined){e=access();if(e)return e;}
    uint32_t actual=rd(j.afe,p->off);
    if((actual&p->mask)!=(next&p->mask)){s.last_register=p->off;s.last_value=actual;return EIO;}
    s.uncertain_mask&=~bit;return 0;
}
static int verify_fields(bool saved)
{
    for(unsigned n=0;n<field_count();n++) {
        struct field *p=&fields[n];
        int e=refuse(j.afe,p->off,p->mask,(saved?p->saved:p->expected)&p->mask);
        if(e)return e;
    }
    return 0;
}
static void afe_snapshot(bool configured)
{
    uint32_t top=rd(j.afe,0x10);
    if(configured)s.top_configured=top;else s.top_after=top;
    if(j.combined)return; /* OUT4 belongs to the AFE engine, not this child */
    for(unsigned n=0;n<4;n++) {
        uint32_t v=rd(j.afe,fields[n+1].off);
        if(configured)s.out_configured[n]=v;else s.out_after[n]=v;
    }
}
#include "x9_speaker_mt6993_pll.inc"
#include "x9_speaker_mt6993_path.inc"
#include "x9_speaker_mt6993_native.inc"
static int take(bool with_pll,bool with_path)
{
    s.stage=X9ClockMap;
    if(!j.hwv)j.hwv=ml_io_map(0x1c040000,0x75000);
    if(!j.vlp)j.vlp=ml_io_map(0x1c200000,0x1000);
    if(!j.afe)j.afe=ml_io_map(0x1a110000,0x10000);
    if(!j.apll)j.apll=ml_io_map(0x1c203100,0x100);
    if(!j.hwv || !j.vlp || !j.afe || !j.apll)return ENOMEM;
    if(j.combined){int mapping=native_mapping();if(mapping)return mapping;}
    CLKSTEP("map done, AO snapshot (tracing accesses)");
    trace=true;
    s.flags|=X9ClockAOCaptured;ao_snapshot();
    s.ap_before=s.ap_after;s.all_before=s.all_after;s.ack_before=s.ack_after;
    s.foreign_before=s.foreign_after;s.vlp_before=s.vlp_after;s.fenc_before=s.fenc_after;
    s.stage=X9ClockCold;
    /* AUDIO and ADSP TOP must be globally off. INFRA/AO AP votes may be
     * borrowed under the same AP lease; foreign XPU votes are never reset. */
    /* Log 269: LK leaves the AUDIO domain AP-voted and on (ap=3ce0f, still
     * set after our full teardown, so not ours). In combined mode adopt
     * exactly that: AP EN, PD_ALL and PM ACK bit0 set, not busy, no foreign
     * voter. It is borrowed (never withdrawn); anything partial is refused. */
    bool audio_inherited=j.combined && (s.ap_before&1) && (s.all_before&1) &&
        (s.ack_before&1) && !(rd(j.hwv,0x13704)&1) && !s.foreign_before;
    if(audio_inherited) {
        /* Log 272/273: qualify the SSYS power ACK before AFE access. */
        static vm_offset_t spm_pbus;
        if(!spm_pbus)spm_pbus=ml_io_map(0x1c00d000,0x1000);
        uint32_t pb=spm_pbus?*(volatile uint32_t *)(spm_pbus+0x3c):0;
        trace=false;
        printf("X9 SPK CLK: inherited AUDIO SSYSPM 0x903c=%x\n",pb);
        /* Log 273: bits 2/3 stay set off and on in HWV mode; PWR_ACK only. */
        if(!(pb&0x80000000u)){CLKSTEP("AUDIO inherited but SSYS PWR_ACK clear: refused (EBUSY)");return EBUSY;}
        /* Log 283: record the borrow now, not at the later domain-vote
         * step. A failure before that step (pll cold) otherwise made
         * native_restored_ao demand AUDIO off, so release returned EBUSY,
         * the lease stayed held and the mic got EBUSY. */
        s.borrowed_vote|=1;
        CLKSTEP("AUDIO domain inherited on: borrowed");trace=true;
    }
    else if((s.ap_before|s.all_before|s.ack_before|rd(j.hwv,0x1131c)|rd(j.hwv,0x13704))&(j.combined?1:3) ||
       s.foreign_before){trace=false;CLKSTEP("cold check refused (EBUSY)");return EBUSY;}
    if(j.combined) {
        trace=false;CLKSTEP("adsp speaker_validate");trace=true;
        int lease=x9_adsp_mt6993_speaker_validate(s.ticket);if(lease){trace=false;return lease;}
    }
    /* Log 270: both VLP muxes may be left enabled by LK (PDN clear, FENC
     * set). Adopt each complete enabled mux as-is (expected = observed,
     * never restored); cold ones are taken; anything else is refused. */
    int e=0;uint32_t mux_borrowed=0;
    {
        uint32_t v90=rd(j.vlp,0x90),v274=rd(j.vlp,0x274);
        for(unsigned n=0;n<2 && !e;n++) {
            uint32_t pdn=1u<<(7+n*8),fenc=1u<<(31-n);
            if((v90&pdn) && !(v274&fenc))continue;
            if(j.combined && !(v90&pdn) && (v274&fenc)) {
                mux_borrowed|=1u<<n;j.mux_expected[n]=v90&(3u<<(n*8));
            } else e=EBUSY;
        }
    }
    if(e){trace=false;return e;}
    trace=false;
    CLKSTEP("APLL capture");
    const unsigned apll_regs[]={8,0xc,0x14,0x34,0x38,0x3c,0x40};
    for(unsigned n=0;n<7;n++)s.apll[n]=rd(j.apll,apll_regs[n]);
    s.flags|=X9ClockAPLLCaptured;
    if(with_pll){CLKSTEP("pll cold");e=pll_cold();if(e)return e;}
    CLKSTEP("smc 0 (AFE init)");
    s.stage=X9ClockInit;s.flags|=X9ClockInitIssued;e=smc(0,0);if(e)return e;
    CLKSTEP("domain votes");
    s.stage=X9ClockDomains;
    const unsigned bits[]={8,4,1};
    /* ADSP owner retains TOP/INFRA/AO. Native child MUST NOT vote/recover
     * those dependencies itself; it owns only AUDIO bit0. */
    for(unsigned n=j.combined?2:0;n<3;n++) {
        if(j.combined){e=x9_adsp_mt6993_speaker_validate(s.ticket);if(e){j.stuck=true;return e;}}
        if(bits[n]==1 && audio_inherited) {
            if((rd(j.hwv,0x20708)&rd(j.hwv,0x13700)&rd(j.hwv,0x12900)&1)!=1 ||
               (rd(j.hwv,0x13704)&1) || foreign()){j.stuck=true;return EBUSY;}
            s.borrowed_vote|=1;continue;
        }
        if(bits[n]==1 && ((rd(j.hwv,0x13700)|rd(j.hwv,0x12900)|rd(j.hwv,0x13704))&(j.combined?1:3) || foreign())) {
            j.stuck=true;return EBUSY;
        }
        if(rd(j.hwv,0x20708)&bits[n]) {
            if(bits[n]==1){j.stuck=true;return EBUSY;}
            s.borrowed_vote|=bits[n];e=wait_bits(j.hwv,0x12900,bits[n],bits[n],10000);
            if(e)j.stuck=true;
        } else e=vote(bits[n],true);
        if(e)return e;
    }
    CLKSTEP("mux take");
    s.stage=X9ClockMux;
    for(unsigned n=0;n<2;n++){if(mux_borrowed&(1u<<n))continue;e=mux_take(n);if(e)return e;}
    if(mux_borrowed)printf("X9 SPK CLK: VLP mux borrowed=%x expected=%x/%x\n",mux_borrowed,j.mux_expected[0],j.mux_expected[1]);
    CLKSTEP("first AFE reads (idle)");
    s.stage=X9ClockIdle;e=idle();if(e)return e; /* FIRST AFE reads */
    for(unsigned n=0;n<field_count();n++)field_prior[n]=fields[n].saved=fields[n].expected=rd(j.afe,fields[n].off);
    s.top_before=fields[0].saved;
    if(!j.combined)for(unsigned n=0;n<4;n++)s.out_before[n]=fields[n+1].saved;
    /* AUDIO_TOP_CON4 (OPPO mt6993-reg.h): bit3 CG_APLL2, bit4 CG_H208M,
     * bit12 PDN_APLL_TUNER2. These used to be required gated (1), but LK's
     * state was never logged, and with a borrowed running APLL2 there is
     * nothing to settle. Any inherited value is journaled (saved) and
     * restored by restore(); the tuner take below opens them as OPPO does. */
    if(j.combined)printf("X9 SPK CLK: inherited top4=%x top0=%x engen=%x spm=%x\n",
        fields[0].saved,rd(j.afe,0),rd(j.afe,0x14),rd(j.afe,0x60));
    j.captured=true;s.flags|=X9ClockAFECaptured;
    /* Keep native APLL2/TUNER2 leaf gates off until PLL settling/meter
     * qualification completes; open HOPPING/F26M first for register access. */
    CLKSTEP("AFE gates");
    s.stage=X9ClockGates;e=change(0,j.combined?fields[0].saved&0x1018:0);if(e)return e;
    CLKSTEP("smc 7 (sidebands)");
    s.stage=X9ClockSidebands;s.flags|=X9ClockSidebandsIssued;e=smc(7,1);if(e)return e;
    if(with_pll){CLKSTEP("pll take");e=pll_take();if(e)return e;}
    if(j.combined){CLKSTEP("tuner take");e=native_tuner_take();if(e)return e;}
    CLKSTEP("I2S fields");
    s.stage=X9ClockI2S;
    for(unsigned n=1;!e && n<field_count();n++)e=change(n,fields[n].value);
    if(e)return e;
    if(with_path){e=path_take();if(e)return e;}
    CLKSTEP("recheck");
    s.stage=X9ClockRecheck;e=idle();if(!e)e=verify_fields(false);if(e)return e;
    afe_snapshot(true);s.flags|=X9ClockConfigured;return 0;
}
static int restore(void)
{
    if(j.stuck)return EIO; /* never unvote an uncertain/in-flight request */
    int e=0;
    if(j.captured) {
        e=idle();if(e)return e;
        for(unsigned n=field_count();n>0;n--)if(s.dirty_mask&(1u<<(n-1))) {
            e=change(n-1,fields[n-1].saved);if(e)return e;
            s.dirty_mask&=~(1u<<(n-1));
        }
        /* Also validate fields whose initial value needed no write. A
         * third-party change cannot silently become a restored baseline. */
        e=verify_fields(true);if(e)return e;
        afe_snapshot(false);j.captured=false;
    }
    for(unsigned n=2;n>0;n--) {
        unsigned k=n-1;if(!(j.mux_owned&(1u<<k)))continue;
        uint32_t pdn=1u<<(7+k*8),mask=pdn|(3u<<(k*8)),fenc=1u<<(31-k);
        e=refuse(j.vlp,0x90,mask,j.mux_expected[k]);if(e)return e;
        wr(j.vlp,0x94,pdn);e=wait_bits(j.vlp,0x274,fenc,0,1000);
        if(e){j.stuck=true;return e;}
        e=mux_parent(k,s.vlp_before);if(e)return e;
        e=refuse(j.vlp,0x90,mask,s.vlp_before&mask);if(e)return e;
        j.mux_owned&=~(1u<<k);
    }
    /* AUDIO must finish off at authoritative surfaces before INFRA release.
     * Keep an added AO erratum floor for this boot, matching ADSP stop.
     * Borrowed votes and all unrelated bits are never withdrawn. */
    const unsigned bits[]={1,4};
    for(unsigned n=0;n<2;n++)if(s.owned_vote&bits[n]) {
        if(foreign()){j.stuck=true;return EBUSY;}
        e=vote(bits[n],false);if(e)return e;
        if(bits[n]==1) {
            e=wait_bits(j.hwv,0x12900,1,0,10000);
            if(!e)e=refuse(j.hwv,0x13700,1,0);
            if(!e)e=refuse(j.hwv,0x13704,1,0);
            if(!e)e=refuse(j.hwv,0x1131c,1,0);
            if(e){j.stuck=true;return e;}
        }
    }
    if(s.owned_vote&8){s.parked_vote|=8;s.owned_vote&=~8u;}
    return 0;
}
static int run_probe(unsigned mode)
{
    bool with_pll=mode!=0,with_path=mode==2;
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&x9_speaker_clock_lock);
    if(s.flags&X9ClockAttempted){lck_mtx_unlock(&x9_speaker_clock_lock);return EBUSY;}
    uint64_t ticket=0;int e=x9_audio_resources_claim(X9AudioResourceSpeaker,&ticket);
    if(e){lck_mtx_unlock(&x9_speaker_clock_lock);return e;} /* no attempt consumption */
    s.ticket=ticket;s.flags=X9ClockAttempted|X9ClockHeld;
    if(with_pll){pll_s.flags=X9PLLAttempted|X9PLLHeld;pll_s.ticket=ticket;}
    if(with_path){path_s.flags=X9PathAttempted|X9PathHeld;path_s.ticket=ticket;}
    s.smc_result[0]=s.smc_result[1]=INT64_MIN;
    e=take(with_pll,with_path);s.error=(uint32_t)e;
    unsigned failure_stage=s.stage,failure_register=s.last_register,failure_value=s.last_value;
    unsigned pll_failure_stage=pll_s.stage,pll_failure_register=pll_s.last_register,pll_failure_value=pll_s.last_value;
    unsigned path_failure_stage=path_s.stage,path_failure_register=path_s.last_register,path_failure_value=path_s.last_value;
    s.stage=X9ClockRestore;if(with_path)path_s.stage=X9PathRestore;
    int off=with_path?path_restore():0;
    if(with_pll)pll_s.stage=X9PLLRestore;
    if(!off && with_pll)off=pll_restore();
    if(!off)off=restore();ao_snapshot();
    if(!off)off=x9_audio_resources_release(X9AudioResourceSpeaker,ticket);
    s.restore_error=(uint32_t)off;
    if(off){s.flags|=X9ClockPoisoned;x9_audio_resources_fault(X9AudioResourceSpeaker,ticket,e?e:off);}
    else {s.flags&=~X9ClockHeld;s.ticket=0;s.flags|=X9ClockRestored;}
    if(e){s.stage=failure_stage;s.last_register=failure_register;s.last_value=failure_value;}
    else if(!off)s.stage=X9ClockDone;
    if(with_pll) {
        pll_s.error=(uint32_t)e;pll_s.restore_error=(uint32_t)off;
        if(off)pll_s.flags|=X9PLLPoisoned;
        else {pll_s.flags&=~X9PLLHeld;pll_s.flags|=X9PLLRestored;pll_s.ticket=0;}
        if(e){pll_s.stage=pll_failure_stage;pll_s.last_register=pll_failure_register;pll_s.last_value=pll_failure_value;}
        else pll_s.stage=off?X9PLLRestore:X9PLLDone;
    }
    if(with_path) {
        path_s.error=(uint32_t)e;path_s.restore_error=(uint32_t)off;
        if(off)path_s.flags|=X9PathPoisoned;
        else {path_s.flags&=~X9PathHeld;path_s.flags|=X9PathRestored;path_s.ticket=0;}
        if(e){path_s.stage=path_failure_stage;path_s.last_register=path_failure_register;path_s.last_value=path_failure_value;}
        else path_s.stage=off?X9PathRestore:X9PathDone;
    }
    lck_mtx_unlock(&x9_speaker_clock_lock);return e?e:off;
}
int x9_speaker_mt6993_clock_probe(void){return run_probe(0);}
int x9_speaker_mt6993_pll_probe(void){return run_probe(1);}
int x9_speaker_mt6993_clock_status(X9AudioClockStatus *out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&x9_speaker_clock_lock);*out=s;lck_mtx_unlock(&x9_speaker_clock_lock);return 0;
}
int x9_speaker_mt6993_pll_status(X9AudioPLLStatus *out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&x9_speaker_clock_lock);*out=pll_s;lck_mtx_unlock(&x9_speaker_clock_lock);return 0;
}
int x9_speaker_mt6993_path_probe(void){return run_probe(2);}
int x9_speaker_mt6993_path_status(X9AudioPathStatus *out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&x9_speaker_clock_lock);*out=path_s;lck_mtx_unlock(&x9_speaker_clock_lock);return 0;
}
