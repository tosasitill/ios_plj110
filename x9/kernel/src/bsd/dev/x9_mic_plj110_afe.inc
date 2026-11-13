/* Calibration-only PLJ110 AFE resource journal. Included after board MMIO
 * helpers; serialized by afe_mic_lock. OPPO mt6993-afe-{clk,pcm,gpio}.c,
 * clk-mt6993{,-pd,-adsp}.c, hwccf_api.c and pinctrl-paris.c are the inputs.
 * AUDIO must be off at the AO voter surface before any AFE access. ADSP
 * AO/INFRA can be shared: add/clear only our absent AP votes, never SPM or
 * other XPU votes. No DMA/IRQ clear, SRAM loads, APLL, ADC or global reset.
 * These clocks are the active direct-PDN VLP mux branch, NOT HWV muxes.
 */
struct afe_journal {
    vm_offset_t hwv, vlp, afe, gpio, ioc[2], spm_pbus;
    struct x9_mic_afe_status *s;
    uint32_t votes, borrowed, mux_owned, pin_owned;
    uint32_t saved[5];
    bool snapshot, engine_owned, sram, stuck, pins_saved;
    bool dram_owned, dram_uncertain;
};
static struct afe_journal calibration;
/* Vendor INIT is a device-probe operation, not a runtime-resume operation.
 * Once successful it remains installed for this boot; repeated cold capture
 * must not replay global secure initialization. A failed INIT is terminal. */
static bool afe_init_attempted,afe_init_ready;
static const unsigned afe_regs[]={0x10,0,0x60,0x4380,0x14};
static const uint32_t afe_masks[]={3,1u<<25,4,1,1};
static const unsigned cal_pins[]={98,100,101,102,103};
static const unsigned cal_ioc[]={0,0,1,0,0}, cal_bits[]={0,3,2,1,2};
/* OPPO v6993/spm_reg.h: AUDIO_SSYSPM_CON = SPM+0x903c.
 * These are the actual domain ACK and protection state, not HWV request/DONE.
 * Observe only: no SSYSPM/global protection writes, borrowed protection ticket
 * or claimed write-path drain. The firmware may control both bus directions;
 * an enabled/pending protection state must never be followed by AFE access.
 */
#define AUDIO_SSYSPM_PBUS_OFFSET 0x3cu
#define AUDIO_SSYSPM_PWR_ACK (1u<<31)
#define AUDIO_SSYSPM_PROTECT ((1u<<2)|(1u<<3))
static int afe_spm_wait(struct afe_journal *a,uint32_t mask,uint32_t expected)
{
    int e=wait_bits(a->spm_pbus,AUDIO_SSYSPM_PBUS_OFFSET,mask,expected,10000);
    if(e) {
        a->s->last_register=X9_MIC_AFE_REGISTER_SPM|0x903cu;
        a->stuck=true;
    }
    return e;
}
static uint32_t afe_foreign_votes(struct afe_journal *a)
{
    /* Log 268: per-XPU offsets on ap_hwv hang the SoC (they are mm_hwv's).
     * Foreign AUDIO vote = PD_ALL bit0 not explained by our AP EN bit0. */
    return (rr(a->hwv,0x13700)&~rr(a->hwv,0x20708))&1;
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
    /* Last AO-only qualification before reading/writing the powered domain.
     * A matching AP vote does not prove power-up/protection release finished.
     * This is a non-atomic access prerequisite, not a firmware ownership grant.
     */
    if(!a->spm_pbus ||
       (rr(a->spm_pbus,AUDIO_SSYSPM_PBUS_OFFSET)&
        (AUDIO_SSYSPM_PWR_ACK|AUDIO_SSYSPM_PROTECT))!=AUDIO_SSYSPM_PWR_ACK) {
        a->s->last_register=X9_MIC_AFE_REGISTER_SPM|0x903cu;
        a->stuck=true;return EBUSY;
    }
    return 0;
}
/* OPPO mt6993_afe_dram_request/release: AFE_SPM_CONTROL_REQ APSRC bit3,
 * DDREN_REQ bit6 and DDREN_URG bit7. These are power requests, NOT a DMA
 * mapping, completion counter or write-path drain acknowledgement. Never
 * borrow an inherited request; keep ours through the provider's real drain.
 */
#define AFE_MIC_DRAM_REQ ((1u<<3)|(1u<<6)|(1u<<7))
static int afe_dram_guard(struct afe_journal *a)
{
    if(!a->dram_owned)return 0;
    int e=afe_access(a);if(e)return e;
    a->s->last_register=0x60;
    uint32_t v=rr(a->afe,0x60)&AFE_MIC_DRAM_REQ;
    return (v==AFE_MIC_DRAM_REQ || (a->dram_uncertain && !v))?0:EBUSY;
}
static int afe_dram_take(struct afe_journal *a)
{
    if(!a || !a->snapshot || a->dram_owned)return EINVAL;
    int e=afe_access(a);if(e)return e;
    a->s->last_register=0x60;uint32_t v=rr(a->afe,0x60);
    if(v&AFE_MIC_DRAM_REQ)return EBUSY;
    a->dram_owned=a->dram_uncertain=true; /* before possibly effective request */
    wr(a->afe,0x60,v|AFE_MIC_DRAM_REQ);
    if((rr(a->afe,0x60)&AFE_MIC_DRAM_REQ)!=AFE_MIC_DRAM_REQ)return EIO;
    a->dram_uncertain=false;return 0;
}
static int afe_dram_release(struct afe_journal *a)
{
    if(!a->dram_owned)return 0;
    int e=afe_dram_guard(a);if(e)return e;
    uint32_t v=rr(a->afe,0x60),owned=v&AFE_MIC_DRAM_REQ;
    if(owned!=AFE_MIC_DRAM_REQ && !(a->dram_uncertain && !owned))return EBUSY;
    /* Full drain is the engine/provider's prerequisite for board unprepare.
     * Preserve SRCCLKENA and every unrelated power request in this same read. */
    a->dram_uncertain=true;wr(a->afe,0x60,v&~AFE_MIC_DRAM_REQ);
    if(rr(a->afe,0x60)&AFE_MIC_DRAM_REQ)return EIO;
    a->dram_owned=a->dram_uncertain=false;return 0;
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
    for(unsigned i=0;i<sizeof(x9_mic_memif_regs)/sizeof(x9_mic_memif_regs[0]);i++)if(rr(a->afe,x9_mic_memif_regs[i])&VUL1_ON) {
        a->s->last_register=x9_mic_memif_regs[i];return EBUSY;
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
static int afe_take(struct afe_journal *a,vm_offset_t gpio,struct x9_mic_afe_status *s,
    volatile uint32_t *mapped_afe)
{
    bzero(a,sizeof(*a));bzero(s,sizeof(*s));a->s=s;a->gpio=gpio;
    s->version=1;s->stage=X9_MIC_AFE_BASELINE;s->flags=X9_MIC_AFE_ATTEMPTED;
    for(unsigned i=0;i<4;i++)s->smc_result[i]=INT64_MIN; /* not issued */
    a->hwv=mic_map_once(MIC_MAP_HWV);a->vlp=mic_map_once(MIC_MAP_CLOCK);
    a->spm_pbus=mic_map_once(MIC_MAP_SPM_PBUS);
    a->afe=mapped_afe?(vm_offset_t)mapped_afe:mic_map_once(MIC_MAP_AFE);
    a->ioc[0]=mic_map_once(MIC_MAP_IOC0);a->ioc[1]=mic_map_once(MIC_MAP_IOC1);
    if(!a->hwv || !a->vlp || !a->afe || !a->ioc[0] || !a->ioc[1] || !a->spm_pbus)return ENOMEM;
    s->ap_vote_before=rr(a->hwv,0x20708);s->all_vote_before=rr(a->hwv,0x13700);
    s->ack_before=rr(a->hwv,0x12900);s->done=rr(a->hwv,0x1132c);
    s->vlp_before=rr(a->vlp,0x90);s->fenc_before=rr(a->vlp,0x274);
    s->foreign_vote_before=afe_foreign_votes(a);
    if(s->foreign_vote_before || ((s->ap_vote_before|s->all_vote_before|s->ack_before|
       rr(a->hwv,0x1131c)|rr(a->hwv,0x13704))&1))return EBUSY;
    /* No AFE access even for baseline inspection while AUDIO is inherited on.
     * Protection bits in the cold state are not used as a DMA drain receipt.
     */
    if(rr(a->spm_pbus,AUDIO_SSYSPM_PBUS_OFFSET)&AUDIO_SSYSPM_PWR_ACK) {
        s->last_register=X9_MIC_AFE_REGISTER_SPM|0x903cu;return EBUSY;
    }
    if((s->vlp_before&0x8080)!=0x8080 || (s->fenc_before&0xc0000000))return EBUSY;
    s->stage=X9_MIC_AFE_SMC_INIT;
    int e=0;
    if(!afe_init_ready) {
        if(afe_init_attempted)return EIO;
        afe_init_attempted=true;s->flags|=X9_MIC_AFE_INIT_ISSUED;
        e=afe_smc(a,0,0);if(e)return e;
        afe_init_ready=true;
    }
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
        if(bits[i]==1) {
            e=afe_spm_wait(a,AUDIO_SSYSPM_PWR_ACK|AUDIO_SSYSPM_PROTECT,AUDIO_SSYSPM_PWR_ACK);
            if(e)return e; /* retain uncertain votes; do not touch AFE */
        }
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
        e=afe_dram_release(a);if(e){a->stuck=true;return e;}
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
            if(!e)e=afe_spm_wait(a,AUDIO_SSYSPM_PWR_ACK,0);
            if(e){a->stuck=true;return e;}
        }
    }
    afe_ao_snapshot(a);return 0;
}
