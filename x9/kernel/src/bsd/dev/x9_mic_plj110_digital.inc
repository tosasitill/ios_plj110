/* SPDX-License-Identifier: GPL-2.0
 * Narrow ordinary 48k ADDA capture from OPPO mt6993-dai-adda.c and
 * clk-mt6993-adsp.c. UL0_ADC is TOP_CON2 bit23, parent aud_vlpwire:
 * the cold-owned parent0/26M resource path is sufficient, not APLL2.
 * No IRQ, connection matrix, memif/base address, DMA or PCM publication. */
enum {
    RX_TOP2=0x4, RX_ENGEN=0x14, RX_UL0=0xbd0, RX_UL1=0xc50,
    RX_FIFO=0xbd4, RX_COEF=0xbec, RX_CFG0=0x1190, RX_CFG1=0x11f0,
    RX_DATA0=0x1198, RX_DATA1=0x11f8, RX_DELAY=0x11a0,
    RX_PAD=0x1204, RX_CLK=0x1280, RX0=0x1288, RX1=0x1290,
    RX_MON0=0x12a0, RX_MON1=0x12a8
};
struct digital_afe_journal {
    struct x9_mic_digital_status *s;
    struct { uint32_t reg, mask, old; } saved[48];
    unsigned pending;
    uint64_t deadline;
    bool owned, quiesced;
};
static struct digital_afe_journal digital_afe;
static int digital_dma_snapshot(struct afe_journal *a,struct x9_mic_dma_status *s,bool after)
{
    /* Read only under the same cold AFE lease. No SRAM mapping, allocation,
     * CPU loads, address writes, IRQ clear or memif enable. SMC power status
     * cannot authorize any of those operations. */
    int e=afe_access(a);if(e)return e;
    if(!after) {
        bzero(s,sizeof(*s));s->version=1;s->stage=X9_MIC_DMA_BASELINE;
        s->flags=X9_MIC_DMA_ATTEMPTED;
        s->blockers=X9_MIC_DMA_NEED_MAP|X9_MIC_DMA_NEED_OWNER|X9_MIC_DMA_NEED_COPY|
            X9_MIC_DMA_NEED_QUIESCE|X9_MIC_DMA_NEED_SOURCE_ID;
        s->ring_bytes=65536;s->adc_channel=0; /* proposal, NOT allocated */
        s->memif_con0=rr(a->afe,MEMIF_CON0);
        s->con0_before=rr(a->afe,VUL1_CON0);s->monitor_before=rr(a->afe,VUL1_MON0);
        for(unsigned i=0;i<32;i++)if(rr(a->afe,x9_mic_memif_regs[i])&VUL1_ON)s->active_memifs|=1u<<i;
        for(unsigned c=0;c<2;c++)for(unsigned w=0;w<7;w++)
            s->route[c][w]=rr(a->afe,(c?CONN021:CONN020)+x9_mic_route_words[w]*4);
    } else {
        s->con0_after=rr(a->afe,VUL1_CON0);s->monitor_after=rr(a->afe,VUL1_MON0);
    }
    uint32_t hi=0,lo=0;bool stable=false;
    for(unsigned i=0;i<3;i++) {
        hi=rr(a->afe,VUL1_CUR_MSB);lo=rr(a->afe,VUL1_CUR);
        if(hi==rr(a->afe,VUL1_CUR_MSB)){stable=true;break;}
    }
    s->last_register=VUL1_CUR;if(!stable || (hi&~0x1ffu))return EIO;
    uint64_t cur=((uint64_t)hi<<32)|lo;
    if(after){s->cur_after=cur;return 0;}
    s->cur_before=cur;s->flags|=X9_MIC_DMA_REGISTERS_VALID;
    if(!s->active_memifs && !(s->monitor_before&(VUL1_MON_BUSY|VUL1_MON_FULL)))s->flags|=X9_MIC_DMA_IDLE;
    bool empty=true;
    for(unsigned c=0;c<2;c++)for(unsigned w=0;w<7;w++)if(s->route[c][w])empty=false;
    if(empty)s->flags|=X9_MIC_DMA_ROUTE_EMPTY;
    s->last_register=VUL1_MON0;
    if(!(s->flags&X9_MIC_DMA_IDLE))return EBUSY;
    s->last_register=CONN020;if(!empty)return EBUSY;
    s->last_register=MEMIF_CON0;
    if(s->memif_con0&4)return EBUSY; /* CPU compact SRAM is not normal S16 */
    s->last_register=VUL1_CON0;
    if(s->con0_before&(1u<<7))return EBUSY; /* do not replay a clear strobe */
    s->eligible=1;s->last_register=0;
    return 0; /* local format/idle only; all provider blockers stay asserted */
}
static int digital_afe_update(struct afe_journal *a,unsigned reg,uint32_t mask,uint32_t value)
{
    struct digital_afe_journal *d=&digital_afe;
    d->s->last_register=0x80000000u|reg;
    if(mach_absolute_time()>=d->deadline)return ETIMEDOUT;
    int e=afe_access(a);if(e)return e;
    if(d->pending>=48)return EOVERFLOW;
    unsigned n=d->pending++;
    d->saved[n].reg=reg;d->saved[n].mask=mask;d->saved[n].old=rr(a->afe,reg)&mask;
    d->s->afe_journal_count=d->pending;
    return update(a->afe,reg,mask,value);
}
#define RX(reg,mask,value) do {e=digital_afe_update(a,(reg),(mask),(value));if(e)return e;} while(0)
static void digital_afe_snapshot(struct afe_journal *a,bool active)
{
    struct x9_mic_digital_status *s=digital_afe.s;
    if(active) {
        s->afe_ul=rr(a->afe,RX_UL0);s->afe_rx[0]=rr(a->afe,RX0);s->afe_rx[1]=rr(a->afe,RX1);
        s->afe_pad=rr(a->afe,RX_PAD);s->afe_clock=rr(a->afe,RX_CLK);s->afe_gate=rr(a->afe,RX_TOP2);
        s->afe_monitor[0]=rr(a->afe,RX_MON0);s->afe_monitor[1]=rr(a->afe,RX_MON1);
    } else {
        s->afe_after_ul=rr(a->afe,RX_UL0);s->afe_after_rx[0]=rr(a->afe,RX0);s->afe_after_rx[1]=rr(a->afe,RX1);
        s->afe_after_pad=rr(a->afe,RX_PAD);s->afe_after_clock=rr(a->afe,RX_CLK);
        s->afe_after_gate=rr(a->afe,RX_TOP2);
    }
}
static int digital_afe_preflight(struct afe_journal *a,struct x9_mic_digital_status *s,
    const struct x9_mic_hardware_status *h)
{
    bzero(&digital_afe,sizeof(digital_afe));digital_afe.s=s;
    s->stage=X9_MIC_DIGITAL_BASELINE;
    int e=afe_idle(a,true);if(e)return e;
    /* Engine/PAD/SRC were already checked. Capture RX and PAD_CLK must also
     * be off; UL1 stays disabled even though R-MISO1 uses the ADDA6 RXIF. */
    const unsigned regs[]={RX0,RX1,RX_CLK,RX_UL1};
    for(unsigned i=0;i<4;i++){s->last_register=0x80000000u|regs[i];if(rr(a->afe,regs[i])&1)return EBUSY;}
    if(h->cycle[0]<0 || h->cycle[1]<0 || h->chosen_phase[0]<0 || h->chosen_phase[1]<0)return EINVAL;
    uint32_t diff=h->cycle[0]>=h->cycle[1]?(uint32_t)(h->cycle[0]-h->cycle[1]):(uint32_t)(h->cycle[1]-h->cycle[0]);
    if(diff>15)return ERANGE; /* RG_MTKAIF0_RXIF_DELAY_CYCLE is four bits */
    s->delay_cycle=diff;s->delay_data=h->cycle[0]>=h->cycle[1]?0:1;
    return 0;
}
static int digital_afe_enable(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    struct x9_mic_digital_status *s=d->s;
    uint64_t ticks;nanoseconds_to_absolutetime(1000000000ULL,&ticks);
    d->deadline=mach_absolute_time()+ticks;d->owned=true;
    s->stage=X9_MIC_DIGITAL_AFE_CONFIG;int e=0;
    RX(RX_TOP2,1u<<23,0); /* regular UL0 ADC gate; leave hires/APLL gates alone */
    /* protocol2+inverted calibration latch; preserve other fields, no reset */
    RX(RX_CFG0,(1u<<31)|(1u<<17)|(1u<<16),(1u<<31)|(1u<<16));
    RX(RX_CFG1,(1u<<31)|(1u<<17)|(1u<<16),(1u<<31)|(1u<<16));
    RX(RX_DELAY,0xf100,(s->delay_cycle<<12)|(s->delay_data<<8));
    RX(RX_DATA0,1,0);RX(RX_DATA1,1,0); /* AMIC */
    /* R-MISO1 RX0=2ch, RX1=4ch, rate code5, enable-select1.
     * Only UL1_CH1 codec data is duplicated; no ADC_R/UL1 engine. */
    RX(RX0,0x100f11f3,0x00031050);
    RX(RX1,0x100011f3,0x00001052);
    static const uint32_t coeff[]={0,0x3fb8,0x3fb80000,0x3fb80000,0xc048};
    for(unsigned i=0;i<5;i++)RX(RX_COEF+i*4,UINT32_MAX,coeff[i]);
    /* voice48k3, IIR mode0/on, AMIC/non-VOW/non-loopback/DCXO.
     * Clear alternate APLL/DMIC/ULCF selectors without touching gain fields. */
    RX(RX_UL0,0xffffc7ff,(3u<<17)|(1u<<10));
    /* Release the vendor ADDA FIFO reset field; no reset pulse. */
    RX(RX_FIFO,1u<<4,0);
    s->stage=X9_MIC_DIGITAL_AFE_ENABLE;
    RX(RX_ENGEN,2,2); /* ADDA 3.25M, independent of existing 26M bit0 */
    RX(RX_CLK,0x1ff3,0x0851); /* MTKAIFv4/PAD_CLK 48k; DL SRC stays off */
    RX(RX_PAD,0xff,0xb9); /* protocol2 + vendor FIFO response + RX_FIFO_ON */
    RX(RX0,1,1);RX(RX1,1,1);
    RX(RX_UL0,1,1);
    s->flags|=X9_MIC_DIGITAL_AFE_ON;s->stage=X9_MIC_DIGITAL_OBSERVE;
    /* Bounded observation only. No spinning for nonzero samples/PCM and no
     * stale fail clear. A sync-search failure is decisive; no auto retry. */
    delay(1000);e=afe_access(a);if(e)return e;
    digital_afe_snapshot(a,true);
    if((s->afe_monitor[0]|s->afe_monitor[1])&(1u<<21))return EIO;
    s->flags|=X9_MIC_DIGITAL_READBACK;
    return 0;
}
static int digital_afe_quiesce(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    if(!d->owned || d->quiesced)return 0;
    int e=afe_access(a);if(e)return e;
    d->s->last_register=0x80000000u|RX_UL0;e=update(a->afe,RX_UL0,1,0);
    if(!e)delay(125); /* vendor UL post-PMD minimum before AFE off */
    if(!e){d->s->last_register=0x80000000u|RX0;e=update(a->afe,RX0,1,0);}
    if(!e){d->s->last_register=0x80000000u|RX1;e=update(a->afe,RX1,1,0);}
    if(!e){d->s->last_register=0x80000000u|RX_PAD;e=update(a->afe,RX_PAD,1,0);}
    if(!e)d->quiesced=true;
    return e;
}
static int digital_afe_restore(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    if(!d->owned)return 0;
    int e=digital_afe_quiesce(a);
    while(!e && d->pending) {
        unsigned n=d->pending-1;
        d->s->last_register=0x80000000u|d->saved[n].reg;
        e=afe_access(a);
        if(!e)e=update(a->afe,d->saved[n].reg,d->saved[n].mask,d->saved[n].old);
        if(!e){--d->pending;++d->s->afe_journal_restored;}
    }
    if(!e){e=afe_access(a);if(!e)digital_afe_snapshot(a,false);}
    if(!e)d->owned=false;
    return e;
}
#undef RX
