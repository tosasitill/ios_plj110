/* Calibration-only PLJ110 AFE resource journal. Included after board MMIO
 * helpers; serialized by afe_mic_lock. OPPO mt6993-afe-{clk,pcm,gpio}.c,
 * clk-mt6993{,-pd,-adsp}.c, hwccf_api.c and pinctrl-paris.c are the inputs.
 * AUDIO must be off at the AO voter surface before any AFE access. ADSP
 * AO/INFRA can be shared: add/clear only our absent AP votes, never SPM or
 * other XPU votes. No DMA/IRQ clear, SRAM loads, APLL, ADC or global reset.
 * These clocks are the active direct-PDN VLP mux branch, NOT HWV muxes.
 */
struct afe_journal {
    vm_offset_t hwv, vlp, afe, gpio, ioc[2];
    struct x9_mic_afe_status *s;
    uint32_t votes, borrowed, mux_owned, pin_owned;
    uint32_t saved[5];
    bool snapshot, engine_owned, sram, stuck, pins_saved;
};
static struct afe_journal calibration;
static const unsigned afe_regs[]={0x10,0,0x60,0x4380,0x14};
static const uint32_t afe_masks[]={3,1u<<25,4,1,1};
static const unsigned cal_pins[]={98,100,101,102,103};
static const unsigned cal_ioc[]={0,0,1,0,0}, cal_bits[]={0,3,2,1,2};
static uint32_t afe_foreign_votes(struct afe_journal *a)
{
    uint32_t v=0;
    for(unsigned i=1;i<=9;i++) {
        unsigned base=i<5?0x20000+i*0x10000:0x70000+(i-5)*0x1000;
        if(rr(a->hwv,base+0x708)&1)v|=1u<<i;
    }
    return v;
}
static unsigned pin_reg(unsigned i,unsigned field)
{
    /* IES, SMT, PU, PD. GPIO101 is LB2, all other cal pins are LB1. */
    static const unsigned regs[2][4]={{0x70,0x120,0xc0,0xa0},{0x70,0xe0,0xb0,0xa0}};
    return regs[cal_ioc[i]][field];
}
static uint32_t cal_pin_state(struct afe_journal *a,unsigned i)
{
    unsigned n=cal_pins[i];uint32_t v=(rr(a->gpio,0x300+(n/4)*16)>>((n%4)*8))&31;
    v|=((rr(a->gpio,(n/32)*16)>>(n%32))&1)<<8;
    for(unsigned k=0;k<4;k++)v|=((rr(a->ioc[cal_ioc[i]],pin_reg(i,k))>>cal_bits[i])&1)<<(9+k);
    return v;
}
static int afe_smc(struct afe_journal *a,unsigned op,unsigned slot)
{
    register uint64_t x0 __asm__("x0")=0xc2000517ULL;
    register uint64_t x1 __asm__("x1")=op,x2 __asm__("x2")=0,x3 __asm__("x3")=0;
    register uint64_t x4 __asm__("x4")=0,x5 __asm__("x5")=0,x6 __asm__("x6")=0,x7 __asm__("x7")=0;
    __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::"memory");
    a->s->smc_result[slot]=(int64_t)x0;
    return x0==0?0:((int64_t)x0==-1?ENOTSUP:EIO);
}
static int afe_vote(struct afe_journal *a,uint32_t bit,bool on)
{
    int e=wait_bits(a->hwv,0x1132c,bit,bit,10000);if(e)return e;
    if(on)a->votes|=bit; /* journal BEFORE issuing a possibly late vote */
    wr(a->hwv,on?0x20700:0x20704,bit);
    e=wait_bits(a->hwv,0x20708,bit,on?bit:0,10000);
    if(!e)e=wait_bits(a->hwv,0x1132c,bit,bit,10000);
    if(!e && on)e=wait_bits(a->hwv,0x12900,bit,bit,10000);
    if(e)a->stuck=true; /* do not unvote uncertain/in-flight requests */
    else if(!on)a->votes&=~bit;
    return e;
}
static int afe_mux_parent(struct afe_journal *a,unsigned i,uint32_t parent)
{
    uint32_t mask=3u<<(i*8);
    wr(a->vlp,0x98,mask);wr(a->vlp,0x94,parent&mask);
    wr(a->vlp,8,1u<<(i+1));
    int e=wait_bits(a->vlp,0x334,1u<<(30-i),0,1000);
    if(!e && (rr(a->vlp,0x90)&mask)!=(parent&mask))e=EIO;
    if(e)a->stuck=true;
    return e;
}
static int afe_mux_take(struct afe_journal *a,unsigned i)
{
    uint32_t pdn=1u<<(7+i*8),fenc=1u<<(31-i);
    if(!(rr(a->vlp,0x90)&pdn) || (rr(a->vlp,0x274)&fenc))return EBUSY;
    a->mux_owned|=1u<<i;
    /* Reparent ONLY while off; unlike Linux we cannot borrow an active PLL. */
    int e=afe_mux_parent(a,i,0);if(e)return e;
    wr(a->vlp,0x98,pdn); /* vendor fenc_enable_setclr: no update pulse */
    e=wait_bits(a->vlp,0x274,fenc,fenc,1000);
    if(!e && (rr(a->vlp,0x90)&pdn))e=EIO;
    if(e)a->stuck=true;
    return e;
}
static int afe_access(struct afe_journal *a)
{
    if((rr(a->hwv,0x12900)&0xd)!=0xd || !(rr(a->hwv,0x20708)&1) ||
       (rr(a->vlp,0x90)&0x8383) || (rr(a->vlp,0x274)&0xc0000000)!=0xc0000000 ||
       afe_foreign_votes(a)) {
        a->stuck=true;return EBUSY;
    }
    return 0;
}
static int afe_idle(struct afe_journal *a,bool engine_on)
{
    int e=afe_access(a);if(e)return e;
    /* Reject inherited/live engines, UL SRC, phase clocks, test, pad TX,
     * sine and every DL/VUL memif in the selected register family. */
    static const unsigned engines[]={0x14,0x18,0x1c};
    for(unsigned i=0;i<3;i++)if((rr(a->afe,engines[i])&15)!=(i==0 && engine_on?1u:0u)) {
        a->s->last_register=engines[i];return EBUSY;
    }
    static const unsigned src[]={0xbd0,0xc50,0xcd0,0x98,0x1204};
    for(unsigned i=0;i<5;i++)if(rr(a->afe,src[i])&1){a->s->last_register=src[i];return EBUSY;}
    if(rr(a->afe,0xf14)&0xfff){a->s->last_register=0xf14;return EBUSY;}
    if(rr(a->afe,0x20)&(1u<<26)){a->s->last_register=0x20;return EBUSY;}
    static const unsigned memifs[]={0x4460,0x4490,0x44c0,0x44f0,0x4520,0x4550,
        0x4580,0x45b0,0x45e0,0x46a0,0x46d0,0x4700,0x4730,
        0x4d80,0x4db0,0x4de0,0x4e10,0x4e40,0x4e70,0x4ea0,0x4ed0,0x4f00,0x4f30,0x4f60};
    for(unsigned i=0;i<sizeof(memifs)/sizeof(memifs[0]);i++)if(rr(a->afe,memifs[i])&(1u<<28)) {
        a->s->last_register=memifs[i];return EBUSY;
    }
    return 0;
}
static int afe_pins_take(struct afe_journal *a)
{
    for(unsigned i=0;i<5;i++) {
        uint32_t state=cal_pin_state(a,i);a->s->pins_before[i]=state;
        /* func1 is only borrowable with AUDIO cold/off proven earlier and
         * all AFE engines idle; a foreign mux/output GPIO is not reset. */
        if((state&31)!=1 && ((state&31) || (state&(1u<<8))))return EBUSY;
    }
    a->pins_saved=true;
    for(unsigned i=0;i<5;i++) {
        unsigned n=cal_pins[i],shift=(n%4)*8;uint32_t bit=1u<<cal_bits[i];
        a->pin_owned|=1u<<i;
        wr(a->gpio,(n/32)*16+8,1u<<(n%32)); /* input-schmitt: DIR=0 */
        int e=update(a->ioc[cal_ioc[i]],pin_reg(i,1),bit,bit);
        if(!e)e=update(a->ioc[cal_ioc[i]],pin_reg(i,2),bit,0);
        if(!e)e=update(a->ioc[cal_ioc[i]],pin_reg(i,3),bit,0);
        if(!e)e=update(a->gpio,0x300+(n/4)*16,31u<<shift,1u<<shift);
        uint32_t expected=(a->s->pins_before[i]&(1u<<9))|1|(1u<<10);
        if(!e && cal_pin_state(a,i)!=expected)e=EIO;
        if(e){a->stuck=true;return e;}
    }
    return 0;
}
static void afe_ao_snapshot(struct afe_journal *a)
{
    if(!a->hwv || !a->vlp)return;
    a->s->ap_vote_after=rr(a->hwv,0x20708);a->s->all_vote_after=rr(a->hwv,0x13700);
    a->s->ack_after=rr(a->hwv,0x12900);a->s->done=rr(a->hwv,0x1132c);
    a->s->foreign_vote_after=afe_foreign_votes(a);
    a->s->vlp_after=rr(a->vlp,0x90);a->s->fenc_after=rr(a->vlp,0x274);
    if(a->pins_saved)for(unsigned i=0;i<5;i++)a->s->pins_after[i]=cal_pin_state(a,i);
}
static void afe_reg_snapshot(struct afe_journal *a)
{
    /* Call only before mux/domain release. Never read an off-domain AFE. */
    if(!a->snapshot || afe_access(a))return;
    a->s->top4_after=rr(a->afe,0x10);a->s->top0_after=rr(a->afe,0);
    a->s->spm_after=rr(a->afe,0x60);a->s->cbip_after=rr(a->afe,0x4380);
    a->s->engine_after=rr(a->afe,0x14);a->s->engine_monitor=rr(a->afe,0x7c);
}
static int afe_take(struct afe_journal *a,vm_offset_t gpio,struct x9_mic_afe_status *s)
{
    bzero(a,sizeof(*a));bzero(s,sizeof(*s));a->s=s;a->gpio=gpio;
    s->version=1;s->stage=X9_MIC_AFE_BASELINE;s->flags=X9_MIC_AFE_ATTEMPTED;
    for(unsigned i=0;i<4;i++)s->smc_result[i]=INT64_MIN; /* not issued */
    a->hwv=ml_io_map(0x1c040000,0x75000);a->vlp=ml_io_map(0x1c200000,0x1000);
    a->afe=ml_io_map(0x1a110000,0x10000);
    a->ioc[0]=ml_io_map(0x12840000,0x1000);a->ioc[1]=ml_io_map(0x12860000,0x1000);
    if(!a->hwv || !a->vlp || !a->afe || !a->ioc[0] || !a->ioc[1])return ENOMEM;
    s->ap_vote_before=rr(a->hwv,0x20708);s->all_vote_before=rr(a->hwv,0x13700);
    s->ack_before=rr(a->hwv,0x12900);s->done=rr(a->hwv,0x1132c);
    s->vlp_before=rr(a->vlp,0x90);s->fenc_before=rr(a->vlp,0x274);
    s->foreign_vote_before=afe_foreign_votes(a);
    if(s->foreign_vote_before || ((s->ap_vote_before|s->all_vote_before|s->ack_before|
       rr(a->hwv,0x1131c)|rr(a->hwv,0x13704))&1))return EBUSY;
    if((s->vlp_before&0x8080)!=0x8080 || (s->fenc_before&0xc0000000))return EBUSY;
    s->stage=X9_MIC_AFE_SMC_INIT;s->flags|=X9_MIC_AFE_INIT_ISSUED;
    int e=afe_smc(a,0,0);if(e)return e;
    s->stage=X9_MIC_AFE_DOMAINS;
    static const uint32_t bits[]={8,4,1}; /* AO -> infra -> AUDIO */
    for(unsigned i=0;i<3;i++) {
        if(bits[i]==1 && (((rr(a->hwv,0x13700)|rr(a->hwv,0x12900))&1) || afe_foreign_votes(a))) {
            a->stuck=true;return EBUSY;
        }
        if(rr(a->hwv,0x20708)&bits[i]) {
            if(bits[i]==1)return EBUSY;
            a->borrowed|=bits[i];e=wait_bits(a->hwv,0x12900,bits[i],bits[i],10000);
        } else e=afe_vote(a,bits[i],true);
        if(e)return e;
    }
    s->stage=X9_MIC_AFE_MUX;
    for(unsigned i=0;i<2;i++){e=afe_mux_take(a,i);if(e)return e;}
    /* This is now the first AFE access. Preserve unrelated top clocks. */
    e=afe_idle(a,false);if(e){a->stuck=true;return e;}
    for(unsigned i=0;i<5;i++)a->saved[i]=rr(a->afe,afe_regs[i]);
    s->top4_before=a->saved[0];s->top0_before=a->saved[1];s->spm_before=a->saved[2];
    s->cbip_before=a->saved[3];s->engine_before=a->saved[4];a->snapshot=true;
    s->stage=X9_MIC_AFE_GATES;
    e=update(a->afe,0x10,3,0);
    if(!e) {s->flags|=X9_MIC_AFE_SIDEBANDS_ISSUED;e=afe_smc(a,7,1);}
    if(e)return e;
    s->stage=X9_MIC_AFE_SMC_SRAM;
    a->sram=true;s->flags|=X9_MIC_AFE_SRAM_HELD; /* request may mutate even on error */
    e=afe_smc(a,3,2);if(e){a->stuck=true;return e;}
    s->stage=X9_MIC_AFE_ENGINE;
    e=update(a->afe,0x60,4,4);
    if(!e)e=update(a->afe,0,1u<<25,0);
    if(!e)e=update(a->afe,0x4380,1,1);
    if(!e){a->engine_owned=true;e=update(a->afe,0x14,1,1);}
    if(!e){s->stage=X9_MIC_AFE_PADS;e=afe_pins_take(a);}
    if(!e)s->flags|=X9_MIC_AFE_ACQUIRED;
    return e;
}
static int afe_restore(struct afe_journal *a)
{
    if(!a || !a->s)return 0;
    if(a->stuck){afe_ao_snapshot(a);return EIO;}
    int e=0;
    if(a->snapshot) {
        e=afe_idle(a,a->engine_owned);if(e){a->stuck=true;return e;}
        for(unsigned i=5;i>0;i--) {
            unsigned k=i-1;if(!(a->pin_owned&(1u<<k)))continue;
            unsigned n=cal_pins[k];uint32_t bit=1u<<cal_bits[k],old=a->s->pins_before[k];
            if(cal_pin_state(a,k)!=((old&(1u<<9))|1|(1u<<10)))return EBUSY;
            e=update(a->gpio,0x300+(n/4)*16,31u<<((n%4)*8),(old&31)<<((n%4)*8));
            for(unsigned field=1;field<4 && !e;field++)
                e=update(a->ioc[cal_ioc[k]],pin_reg(k,field),bit,(old&(1u<<(9+field)))?bit:0);
            if(!e)wr(a->gpio,(n/32)*16+((old&(1u<<8))?4:8),1u<<(n%32));
            if(!e && cal_pin_state(a,k)!=old)e=EIO;
            if(e)return e;a->pin_owned&=~(1u<<k);
        }
        e=update(a->afe,0x14,1,a->saved[4]);
        if(!e && a->engine_owned)e=wait_bits(a->afe,0x7c,UINT32_MAX,0,10000);
        if(e)return e;a->engine_owned=false;
        /* Narrow symmetric runtime_suspend: no IRQ toggles/sine reset. */
        for(unsigned i=4;i>0;i--) {
            unsigned k=i-1;e=update(a->afe,afe_regs[k],afe_masks[k],a->saved[k]);if(e)return e;
        }
        afe_reg_snapshot(a);if(a->stuck)return EIO;
        a->snapshot=false;
    }
    if(a->sram) {
        e=afe_smc(a,4,3);if(e){a->stuck=true;return e;}
        a->sram=false;a->s->flags&=~X9_MIC_AFE_SRAM_HELD;
    }
    for(unsigned i=2;i>0;i--) {
        unsigned k=i-1;if(!(a->mux_owned&(1u<<k)))continue;
        uint32_t pdn=1u<<(7+k*8),fenc=1u<<(31-k);
        wr(a->vlp,0x94,pdn);e=wait_bits(a->vlp,0x274,fenc,0,1000);if(e)return e;
        e=afe_mux_parent(a,k,a->s->vlp_before);if(e)return e;
        if((rr(a->vlp,0x90)&(pdn|(3u<<(k*8))))!=(a->s->vlp_before&(pdn|(3u<<(k*8)))))return EIO;
        a->mux_owned&=~(1u<<k);
    }
    static const uint32_t bits[]={1,4,8};
    for(unsigned i=0;i<3;i++)if(a->votes&bits[i]) {
        e=afe_vote(a,bits[i],false);if(e)return e;
        /* AUDIO was cold; an unexpected new aggregate vote/power prevents
         * releasing parents. Shared AO/infra need not become powered-off. */
        if(bits[i]==1) {
            e=wait_bits(a->hwv,0x12900,1,0,10000);
            if(!e && (rr(a->hwv,0x13700)&1))e=EBUSY;
            if(e){a->stuck=true;return e;}
        }
    }
    afe_ao_snapshot(a);return 0;
}
