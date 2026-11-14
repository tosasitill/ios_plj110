/* Bounded, preconfigured APLL2/VLP clock cycle; included under board mutex.
 * OPPO clk-fmeter-mt6993.c: VLP mux IDs30/33/34, load1023, divider1,
 * software trigger CON0.bit4, busy CON6.bit0; 26MHz reference. Unlike vendor
 * code require a fresh BUSY rise/fall, never use timeout/stale count as rate.
 * No PCW/update trigger write, PLL test clock or MM/TOP mux modification.
 */
static X9AudioPLLStatus pll_s={.version=X9_AUDIO_PLL_STATUS_VERSION};
static struct {
    vm_offset_t meter,top,mm;
    uint32_t gen_expected,gen_fenc;
    bool captured,stuck,pll_written,trigger_issued,meter_done,borrowed;
} pj;
struct pll_field {vm_offset_t base;unsigned off;uint32_t mask,saved,expected;};
static struct pll_field pf[7];
static uint32_t pll_prior[7];
static const unsigned pll_regs[]={8,0xc,0x14,0x34,0x38,0x3c,0x40};
static int pll_refuse(vm_offset_t b,unsigned off,uint32_t mask,uint32_t wanted)
{
    int e=refuse(b,off,mask,wanted);
    if(e){pll_s.last_register=off;pll_s.last_value=s.last_value;}return e;
}
static int pll_change(unsigned n,uint32_t value)
{
    struct pll_field *p=&pf[n];uint32_t old=rd(p->base,p->off),bit=1u<<n;
    if((old&p->mask)!=(p->expected&p->mask) &&
       !((pll_s.uncertain_mask&bit) && (old&p->mask)==(pll_prior[n]&p->mask))) {
        pll_s.last_register=p->off;pll_s.last_value=old;return EBUSY;
    }
    if(n<3 && (old&~p->mask)!=(p->saved&~p->mask)) {
        pll_s.last_register=p->off;pll_s.last_value=old;return EBUSY;
    }
    uint32_t next=(old&~p->mask)|(value&p->mask);
    if((next&p->mask)==(old&p->mask)){p->expected=next;pll_s.uncertain_mask&=~bit;return 0;}
    /* Only the immediate prewrite image may reconcile an uncertain RW
     * transition, including a failed restore. The cold saved value is not
     * necessarily that image after configure -> restore. Triggered meter
     * operations remain separately fenced by trigger_issued/meter_done. */
    pll_prior[n]=old;pll_s.dirty_mask|=bit;pll_s.uncertain_mask|=bit;p->expected=next;
    if(n<3)pj.pll_written=true;
    if(n==4 && (value&16)){pj.trigger_issued=true;pj.meter_done=false;}
    wr(p->base,p->off,next);uint32_t actual=rd(p->base,p->off);
    if((actual&p->mask)!=(next&p->mask)) {
        pll_s.last_register=p->off;pll_s.last_value=actual;return EIO;
    }
    pll_s.uncertain_mask&=~bit;return 0;
}
static int pll_wait(vm_offset_t b,unsigned off,uint32_t mask,uint32_t wanted,unsigned us)
{
    int e=wait_bits(b,off,mask,wanted,us);
    if(e){pll_s.last_register=off;pll_s.last_value=s.last_value;pj.stuck=true;}return e;
}
static void pll_snapshot(uint32_t out[7])
{for(unsigned n=0;n<7;n++)out[n]=rd(j.apll,pll_regs[n]);}
/* OPPO mt6993-afe-clk.c: apll_init writes PCW_CON0 0x3c=0x78fd5264 and
 * PCW_CON1 0x40=0x78fd5265; apll_enable sets CON0 0x8 bit0, CON1 0xc bit31
 * (PCW toggle), CON3 0x14 bit0 (SDM on), TUNER_CON0 0x38 bit0.
 * clk-mt6993.c PLL_FENC(apll2): running = FENC status 0x34 bit4, postdiv
 * CON1 bits24-26; engen2 = apll2_d4 (49.152M) so apll2 is 196.608M after
 * postdiv. LK need not use the OPPO enable sequence, and the old borrow
 * image also demanded postdiv 0 (unproven; 0x78fd5264 gives 196.608M only
 * with /16), so those bits are logged, not required. The fmeter measures
 * engen2 and audio_h before anything uses them. */
static uint64_t apll2_rate(void)
{
    uint32_t pcw=rd(j.apll,0x3c),pd=(rd(j.apll,0xc)>>24)&7;
    return ((26000000ULL*pcw)>>24)>>pd;
}
static int pll_unowned_consumers(void)
{
    /* CKSYS_MM is PD_NULL in OPPO clkchk. Read its AO clock controller,
     * never a powered-off MM engine. All known non-owned APLL2 consumers
     * must be gated, FENC off and no mux update in flight. */
    int e=pll_refuse(pj.top,0xa0,0x810000,0x800000);
    if(!e)e=pll_refuse(pj.top,0x274,1u<<25,0);
    if(!e)e=pll_refuse(pj.top,0x334,1u<<24,0);
    if(!e)e=pll_refuse(pj.mm,0x40,0x87,0x80);
    if(!e)e=pll_refuse(pj.mm,0x270,1u<<19,0);
    if(!e)e=pll_refuse(pj.mm,0x330,1u<<19,0);
    if(!e)e=pll_refuse(j.vlp,0x80,0x80000000,0x80000000); /* SW_ENGEN2 */
    if(!e)e=pll_refuse(j.vlp,0x270,1,0);
    if(!e)e=pll_refuse(j.vlp,0x334,1u<<31,0);
    /* PCW, postdiv and update latch are immutable throughout this cycle.
     * Never retune an inherited PLL or replay its PCW_UPDATE trigger. */
    if(!e)e=pll_refuse(j.apll,0x3c,UINT32_MAX,0x78fd5264);
    if(!e)e=pll_refuse(j.apll,0x40,UINT32_MAX,0x78fd5265);
    if(!e)e=pll_refuse(j.apll,0xc,0x87000000,0x80000000);
    for(unsigned n=0;!e && n<3;n++)e=pll_refuse(pf[n].base,pf[n].off,~pf[n].mask,pf[n].saved&~pf[n].mask);
    return e;
}
static int pll_idle(void)
{
    int e=idle();if(e){pll_s.last_register=s.last_register;pll_s.last_value=s.last_value;}return e;
}
/* Log 276: LK leaves APLL2 running with exactly the target topology:
 * aud_engen2 enabled on parent 3, audio_h enabled on parent 3 (apll2),
 * vlp 0x80=0x20303. Borrow that complete state as-is: no PLL enable, PCW,
 * mux or gate write, never withdrawn; only the fmeter is used to measure. */
static int pll_borrowed_ok(void)
{
    /* Running = FENC status (mtk_pll_fenc_is_prepared) with OPPO's PCW. */
    int e=pll_refuse(j.apll,0x34,16,16);
    if(!e)e=pll_refuse(j.apll,0x3c,UINT32_MAX,0x78fd5264);
    if(!e)e=pll_refuse(j.vlp,0x80,0x8300,0x300);
    if(!e)e=pll_refuse(j.vlp,0x270,4,4);
    if(!e)e=pll_refuse(j.vlp,0x330,4,0);
    if(!e)e=pll_refuse(j.vlp,0x90,0x8300,0x300);
    if(!e)e=pll_refuse(j.vlp,0x274,1u<<30,1u<<30);
    return e;
}
static int pll_consumers(void)
{
    if(pj.borrowed)return pll_borrowed_ok();
    int e=pll_unowned_consumers();
    if(!e)e=pll_refuse(j.vlp,0x80,0x8300,pj.gen_expected);
    if(!e)e=pll_refuse(j.vlp,0x270,4,pj.gen_fenc);
    if(!e)e=pll_refuse(j.vlp,0x330,4,0);
    for(unsigned n=0;!e && n<3;n++)e=pll_refuse(pf[n].base,pf[n].off,pf[n].mask,pf[n].expected&pf[n].mask);
    return e;
}
static int meter_idle(void)
{
    int e=pll_refuse(pj.meter,0xc,1,0); /* refuse autonomous HW mode */
    if(!e)e=pll_refuse(pj.meter,0x18,1,0);
    if(!e)e=pll_refuse(pj.meter,0,16,0);
    return e;
}
static int meter_restore(void)
{
    if(pj.stuck)return EIO;
    /* A trigger without a fresh complete BUSY cycle is not cancellable.
     * A proven completion may clear only our trigger, even on count error. */
    if(pj.trigger_issued && !pj.meter_done){pj.stuck=true;return EIO;}
    int e=pll_refuse(pj.meter,0xc,1,0);
    if(!e)e=pll_refuse(pj.meter,0x18,1,0);
    if(!e && pj.trigger_issued)e=pll_change(4,pf[4].expected&~16u);
    if(e)return e;
    pj.trigger_issued=pj.meter_done=false;
    e=meter_idle();if(e)return e;
    for(unsigned n=7;n>3;n--)if(pll_s.dirty_mask&(1u<<(n-1))) {
        e=pll_change(n-1,pf[n-1].saved);if(e)return e;
        pll_s.dirty_mask&=~(1u<<(n-1));
    }
    for(unsigned n=3;n<7;n++) {
        e=pll_refuse(pf[n].base,pf[n].off,pf[n].mask,pf[n].saved&pf[n].mask);if(e)return e;
    }
    pll_s.meter_after[0]=rd(j.vlp,0x344);
    for(unsigned n=0;n<3;n++)pll_s.meter_after[n+1]=rd(pj.meter,n*4);
    return 0;
}
static int meter_sample(unsigned id,unsigned slot,uint32_t target)
{
    pll_s.stage=slot<2?X9PLLReferenceStage:X9PLLMeasure;
    int e=pll_idle();if(!e)e=pll_consumers();if(!e)e=meter_idle();if(e)return e;
    for(unsigned n=3;n<7;n++) {
        e=pll_refuse(pf[n].base,pf[n].off,pf[n].mask,pf[n].saved&pf[n].mask);if(e)return e;
    }
    uint32_t fenc=1u<<(id==30?2:id==33?31:30);
    e=pll_refuse(j.vlp,id==30?0x270:0x274,fenc,fenc);if(e)return e;
    e=pll_change(3,id<<16);
    if(!e)e=pll_change(4,0x04109000); /* enable, normal cal mode, trigger=0 */
    if(!e)e=pll_change(5,0x3ff0000);
    if(!e)e=pll_change(6,1u<<24); /* source0/ref0, div1 = divide by2 */
    if(!e)e=pll_change(4,pf[4].expected|16);
    if(e)return e;
    /* The window is ~39us; require a real start before accepting completion.
     * Missing BUSY is ambiguous, so keep the entire clock/power lease. */
    e=pll_wait(pj.meter,0x18,1,1,20);
    if(!e)e=pll_wait(pj.meter,0x18,1,0,320);
    if(e)return e;pj.meter_done=true;
    for(unsigned n=3;n<7;n++) {
        e=pll_refuse(pf[n].base,pf[n].off,pf[n].mask,pf[n].expected&pf[n].mask);if(e)return e;
    }
    uint32_t count=rd(pj.meter,4)&0xffff;
    if(!count || (rd(pj.meter,4)&0xffff)!=count){pll_s.last_register=4;pll_s.last_value=count;return EIO;}
    pll_s.counts[slot]=count;
    pll_s.hz[slot]=(uint32_t)((uint64_t)count*26000000*2/1024);
    e=meter_restore();if(e)return e;
    uint32_t hz=pll_s.hz[slot],diff=hz>target?hz-target:target-hz;
    /* <=1% is a nominal path sanity bound, NOT audio-rate accuracy or lock.
     * Counter granularity and unmeasured reference ppm remain constraints. */
    if(diff>target/100){pll_s.last_register=4;pll_s.last_value=count;return ERANGE;}return 0;
}
static int pll_cold(void)
{
    pll_s.stage=X9PLLCold;
    if(!pj.meter)pj.meter=ml_io_map(0x1c201000,0x1000);
    if(!pj.top)pj.top=ml_io_map(0x1c210000,0x1000);
    if(!pj.mm)pj.mm=ml_io_map(0x10040000,0x1000);
    if(!pj.meter || !pj.top || !pj.mm)return ENOMEM;
    pll_snapshot(pll_s.pll_before);pll_s.flags|=X9PLLCaptured;
    const unsigned offsets[]={8,0x14,0x38,0x344,0,4,8};
    const uint32_t masks[]={1,1,1,0x7f0000,0xffff9110,0x3ff0000,0xff00001f};
    for(unsigned n=0;n<7;n++) {
        pf[n].base=n<3?j.apll:n==3?j.vlp:pj.meter;
        pf[n].off=offsets[n];pf[n].mask=masks[n];
        pll_prior[n]=pf[n].saved=pf[n].expected=rd(pf[n].base,pf[n].off);
    }
    pj.gen_expected=rd(j.vlp,0x80)&0x8300;
    pll_s.consumers[0]=rd(j.vlp,0x80);pll_s.consumers[1]=rd(j.vlp,0x270);
    pll_s.consumers[2]=rd(pj.top,0xa0);pll_s.consumers[3]=rd(pj.top,0x274);
    pll_s.consumers[4]=rd(pj.mm,0x40);pll_s.consumers[5]=rd(pj.mm,0x270);
    uint64_t hz=apll2_rate();
    printf("X9 SPK PLL: apll2 con0=%x con1=%x con3=%x fenc=%x tuner=%x pcw=%x/%x rate=%llu vlp80=%x 270=%x 330=%x\n",
        pll_s.pll_before[0],pll_s.pll_before[1],pll_s.pll_before[2],pll_s.pll_before[3],
        pll_s.pll_before[4],pll_s.pll_before[5],pll_s.pll_before[6],hz,
        rd(j.vlp,0x80),rd(j.vlp,0x270),rd(j.vlp,0x330));
    pj.borrowed=false;
    int e=pll_borrowed_ok();
    if(!e) {
        pj.borrowed=true;pll_s.last_register=0;pll_s.last_value=0;
        printf("X9 SPK PLL: APLL2 inherited running (engen2/audio_h on apll2): borrowed, measure only\n");
    } else {
        printf("X9 SPK PLL: borrow refused reg=%x val=%x\n",pll_s.last_register,pll_s.last_value);
        /* Logs 276/283: with the engen2 mux enabled (PDN clear) the cold
         * path below can never pass; report the borrow refusal itself
         * instead of overwriting it with the cold check's vlp+0x80. */
        if(!(rd(j.vlp,0x80)&0x8000))return e;
        e=pll_refuse(j.vlp,0x80,0x8000,0x8000);
        if(!e)e=pll_consumers();
        if(!e)e=pll_refuse(j.apll,8,1u|(1u<<15)|(1u<<17),0);
        if(!e)e=pll_refuse(j.apll,0x14,1,0);
        if(!e)e=pll_refuse(j.apll,0x38,1,0);
        if(!e)e=pll_refuse(j.apll,0x34,16,0);
        if(e)return e;
    }
    pll_s.stage=X9PLLMeter;e=meter_idle();
    if(!e)e=pll_refuse(pj.meter,0,0x9110,0);
    if(!e)e=pll_refuse(j.vlp,0x344,0x7f0000,0);
    if(e)return e;
    pll_s.meter_before[0]=rd(j.vlp,0x344);
    for(unsigned n=0;n<3;n++)pll_s.meter_before[n+1]=rd(pj.meter,n*4);
    pll_s.meter_hw=rd(pj.meter,0xc);
    pj.captured=true;pll_s.flags|=X9PLLBaseline|X9PLLMeterCaptured;return 0;
}
static int pll_h_parent(uint32_t parent)
{
    int e=pll_refuse(j.vlp,0x90,0x8300,j.mux_expected[1]);if(e)return e;
    /* AUDIO engine stays off; register access continues via 26M intbus. */
    wr(j.vlp,0x94,0x8000);e=pll_wait(j.vlp,0x274,1u<<30,0,1000);if(e)return e;
    e=mux_parent(1,parent<<8);if(e){pll_s.last_register=s.last_register;pll_s.last_value=s.last_value;return e;}
    wr(j.vlp,0x98,0x8000);e=pll_wait(j.vlp,0x274,1u<<30,1u<<30,1000);
    if(!e)e=pll_refuse(j.vlp,0x90,0x8300,parent<<8);
    if(!e)j.mux_expected[1]=parent<<8;return e;
}
static int pll_gen_parent(uint32_t parent)
{
    int e=pll_refuse(j.vlp,0x80,0x8000,0x8000);if(e)return e;
    wr(j.vlp,0x88,0x300);wr(j.vlp,0x84,parent&0x300);
    wr(j.vlp,4,1u<<29);
    e=pll_wait(j.vlp,0x330,4,0,1000);
    if(!e)e=pll_refuse(j.vlp,0x80,0x300,parent&0x300);
    if(!e)pj.gen_expected=0x8000|(parent&0x300);return e;
}
static int pll_take(void)
{
    int e=pll_idle();if(!e)e=pll_consumers();if(e)return e;
    if(pj.borrowed) {
        e=meter_sample(33,0,26000000);if(e)return e;pll_s.flags|=X9PLLReference;
        goto measure;
    }
    e=meter_sample(33,0,26000000);if(!e)e=meter_sample(34,1,26000000);
    if(e)return e;pll_s.flags|=X9PLLReference;
    pll_s.stage=X9PLLPower;
    for(unsigned n=0;!e && n<3;n++)e=pll_change(n,1);
    if(!e)e=pll_wait(j.apll,0x34,16,16,1000);
    if(e)return e;pll_s.flags|=X9PLLPowered;
    pll_s.stage=X9PLLMux;
    /* Own ENGEN2 before reparent/update; a late operation must keep lease. */
    pll_s.mux_owned|=1;e=pll_gen_parent(0x300);if(e)return e;
    wr(j.vlp,0x88,0x8000);e=pll_wait(j.vlp,0x270,4,4,1000);
    if(!e){pj.gen_fenc=4;e=pll_refuse(j.vlp,0x80,0x8300,0x300);}
    if(e)return e;pj.gen_expected=0x300;
    e=pll_consumers();if(e)return e;
    pll_s.mux_owned|=2;e=pll_h_parent(3);if(e)return e;
measure:
    for(unsigned n=0;n<3;n++) {
        e=pll_idle();if(!e)e=pll_consumers();if(e)return e;
        e=meter_sample(30,2+n*2,49152000);
        if(!e)e=meter_sample(34,3+n*2,196608000);
        if(e)return e;
    }
    /* Spread <=2 counts at each output; a stationary stale counter still
     * cannot pass because every sample required its own BUSY rise/fall. */
    for(unsigned k=0;k<2;k++) {
        uint32_t lo=pll_s.counts[2+k],hi=lo;
        for(unsigned n=1;n<3;n++){uint32_t c=pll_s.counts[2+k+n*2];if(c<lo)lo=c;if(c>hi)hi=c;}
        if(hi-lo>2){pll_s.last_register=4;pll_s.last_value=hi;return ERANGE;}
    }
    pll_snapshot(pll_s.pll_active);pll_s.flags|=X9PLLMeasured;return 0;
}
static int pll_restore(void)
{
    if(!pj.captured)return 0;
    if(pj.stuck || j.stuck)return EIO;
    int e=meter_restore();if(!e)e=pll_consumers();if(e)return e;
    if(pll_s.mux_owned&2){e=pll_idle();if(!e)e=pll_h_parent(0);if(e)return e;pll_s.mux_owned&=~2u;}
    if(pll_s.mux_owned&1) {
        e=pll_refuse(j.vlp,0x80,0x8300,pj.gen_expected);if(e)return e;
        wr(j.vlp,0x84,0x8000);e=pll_wait(j.vlp,0x270,4,0,1000);if(e)return e;
        pj.gen_fenc=0;e=pll_gen_parent(pll_s.consumers[0]);if(e)return e;pll_s.mux_owned&=~1u;
    }
    e=pll_consumers();if(e)return e;
    for(unsigned n=3;n>0;n--)if(pll_s.dirty_mask&(1u<<(n-1))) {
        e=pll_change(n-1,pf[n-1].saved);if(e)return e;pll_s.dirty_mask&=~(1u<<(n-1));
    }
    if(pj.pll_written){e=pll_wait(j.apll,0x34,16,0,1000);if(e)return e;}
    pll_snapshot(pll_s.pll_after);
    for(unsigned n=0;n<7;n++) {
        /* FENC unrelated bits can change; only bit4 belongs to this PLL. */
        uint32_t mask=n==3?16:UINT32_MAX;
        e=pll_refuse(j.apll,pll_regs[n],mask,pll_s.pll_before[n]&mask);if(e)return e;
    }
    pj.captured=false;return 0;
}
