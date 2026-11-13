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
    RX_MON0=0x12a0, RX_MON1=0x12a8,
    UL_SHADOW0=0xbe4, UL_SHADOW1=0xbe8
};
struct digital_afe_journal {
    struct x9_mic_digital_status *s;
    struct { uint32_t reg, mask, old, value; } saved[48];
    unsigned pending;
    uint64_t deadline, restore_deadline;
    bool owned, quiesced, uncertain, poisoned;
};
static struct digital_afe_journal digital_afe;
_Static_assert(sizeof(struct x9_mic_signal_status)==192,"signal ABI size");
static struct x9_mic_signal_status signal_diagnostic={.version=1};
static void signal_accumulate(struct x9_mic_signal_channel *c,uint32_t raw,bool first)
{
    uint32_t bits=raw&0xffffffu;
    int32_t value=(int32_t)bits;
    if(bits&0x800000u)value-=0x1000000; /* defined arithmetic, no signed shifts */
    if(first) {
        c->raw_first=raw;c->first=value;c->minimum=c->maximum=value;
    } else if(value!=c->last)++c->changes;
    c->raw_last=raw;c->last=value;
    if(value<c->minimum)c->minimum=value;
    if(value>c->maximum)c->maximum=value;
    if(!value)++c->zero_reads;
    c->sum+=value;c->sum_squares+=(uint64_t)((int64_t)value*value);
}
static int digital_signal_observe(struct afe_journal *a)
{
    struct x9_mic_signal_status *s=&signal_diagnostic;
    s->flags=X9_MIC_SIGNAL_ATTEMPTED;s->target_reads=64;s->interval_us=125;
    uint64_t start=mach_absolute_time(),ticks;
    nanoseconds_to_absolutetime(20000000,&ticks);uint64_t deadline=start+ticks;
    int e=0;
    for(unsigned i=0;i<s->target_reads;i++) {
        s->rejected_read=i+1;s->last_register=RX_UL0;
        if(mach_absolute_time()>=deadline){e=ETIMEDOUT;break;}
        /* Check held domains/foreign votes before each MMIO group. This
         * cannot guard a bus fault or provide a sample-ready handshake. */
        e=afe_access(a);if(e)break;
        s->ul_control=rr(a->afe,RX_UL0);
        uint32_t sync0=rr(a->afe,RX_MON0),sync1=rr(a->afe,RX_MON1);
        s->sync_last[0]=sync0;s->sync_last[1]=sync1;
        if(!i){s->sync_first[0]=sync0;s->sync_first[1]=sync1;}
        if((sync0|sync1)&(1u<<21)) {
            s->last_register=(sync0&(1u<<21))?RX_MON0:RX_MON1;e=EIO;break;
        }
        /* Known control fields: SRC on, ordinary voice48k3, no loopback.
         * Do not interpret SDM bit30 or SLT debug counter as PCM/drain. */
        if((s->ul_control&((7u<<17)|5u))!=((3u<<17)|1u)){e=EIO;break;}
        uint32_t raw0=rr(a->afe,UL_SHADOW0),raw1=rr(a->afe,UL_SHADOW1);
        signal_accumulate(&s->channel[0],raw0,!s->reads);
        signal_accumulate(&s->channel[1],raw1,!s->reads);++s->reads;
        if((raw0&((1u<<31)|(1u<<29)))!=(1u<<31)) {
            s->last_register=UL_SHADOW0;e=EIO;break;
        }
        if((raw1>>29)!=3){s->last_register=UL_SHADOW1;e=EIO;break;}
        /* Recheck search failure after the non-atomic pair. No retry or
         * clearing sticky hardware status to force a successful witness. */
        s->sync_last[0]=rr(a->afe,RX_MON0);s->sync_last[1]=rr(a->afe,RX_MON1);
        if((s->sync_last[0]|s->sync_last[1])&(1u<<21)) {
            s->last_register=(s->sync_last[0]&(1u<<21))?RX_MON0:RX_MON1;e=EIO;break;
        }
        if(mach_absolute_time()>=deadline){e=ETIMEDOUT;break;}
        s->rejected_read=0;
        if(i+1<s->target_reads)delay(s->interval_us);
    }
    absolutetime_to_nanoseconds(mach_absolute_time()-start,&s->elapsed_ns);
    s->last_errno=e;
    if(s->channel[0].changes)s->flags|=X9_MIC_SIGNAL_CH1_CHANGED;
    if(s->channel[1].changes)s->flags|=X9_MIC_SIGNAL_CH2_CHANGED;
    if(!e){s->flags|=X9_MIC_SIGNAL_COMPLETE;s->last_register=0;}
    else digital_afe.s->last_register=0x80000000u|s->last_register;
    return e; /* never x9_mic_deliver; reads are not frames */
}
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
    if(!s->active_memifs && !VUL1_MON_ACTIVE(s->monitor_before,s->con0_before) && !(s->monitor_before&VUL1_MON_FULL))s->flags|=X9_MIC_DMA_IDLE;
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
    uint32_t old=rr(a->afe,reg);
    d->saved[n].reg=reg;d->saved[n].mask=mask;d->saved[n].old=old&mask;
    d->saved[n].value=value&mask;
    d->s->afe_journal_count=d->pending;
    /* The write and unwind baseline share the same old control word. */
    d->uncertain=true;
    wr(a->afe,reg,(old&~mask)|(value&mask));
    if((rr(a->afe,reg)&mask)!=(value&mask))return EIO;
    d->uncertain=false;return 0;
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
static int digital_afe_verify(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    if(!d->owned || d->quiesced || !d->pending)return ENXIO;
    for(unsigned i=0;i<d->pending;i++) {
        bool seen=false;
        for(unsigned j=0;j<i;j++)if(d->saved[j].reg==d->saved[i].reg){seen=true;break;}
        if(seen)continue;
        uint32_t mask=0,expected=0;
        for(unsigned j=i;j<d->pending;j++)if(d->saved[j].reg==d->saved[i].reg) {
            expected=(expected&~d->saved[j].mask)|(d->saved[j].value&d->saved[j].mask);
            mask|=d->saved[j].mask;
        }
        d->s->last_register=0x80000000u|d->saved[i].reg;
        if(mach_absolute_time()>=d->deadline)return ETIMEDOUT;
        int e=afe_access(a);if(e)return e;
        if((rr(a->afe,d->saved[i].reg)&mask)!=(expected&mask))return EIO;
        if(mach_absolute_time()>=d->deadline)return ETIMEDOUT;
    }
    return 0; /* non-atomic controls, no new DMA or status writes */
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
    if((s->afe_monitor[0]|s->afe_monitor[1])&(1u<<21)) {
        s->last_register=0x80000000u|((s->afe_monitor[0]&(1u<<21))?RX_MON0:RX_MON1);
        return EIO;
    }
    e=digital_afe_verify(a);if(e)return e;
    s->flags|=X9_MIC_DIGITAL_READBACK;
    return 0;
}
static int digital_afe_restore_failure(struct afe_journal *a,int e)
{
    struct digital_afe_journal *d=&digital_afe;
    if(!d->s->afe_restore_errno) {
        d->s->afe_restore_errno=e;d->s->afe_restore_register=d->s->last_register;
    }
    if(!d->s->restore_errno)d->s->restore_errno=e;
    d->s->flags|=X9_MIC_DIGITAL_RETAINED;
    d->poisoned=true;a->stuck=true;
    return e;
}
static int digital_afe_restore_read(struct afe_journal *a,unsigned reg,
    uint32_t *got,uint32_t *mask)
{
    struct digital_afe_journal *d=&digital_afe;
    uint32_t expected=0,alternate=0;*mask=0;
    for(unsigned i=0;i<d->pending;i++) {
        if(d->saved[i].reg!=reg)continue;
        uint32_t m=d->saved[i].mask;
        expected=(expected&~m)|(d->saved[i].value&m);
        uint32_t v=d->uncertain && i+1==d->pending?d->saved[i].old:d->saved[i].value;
        alternate=(alternate&~m)|(v&m);*mask|=m;
    }
    if(!*mask)return 0; /* never read or write an unissued register */
    d->s->last_register=0x80000000u|reg;
    if(mach_absolute_time()>=d->restore_deadline)return ETIMEDOUT;
    int e=afe_access(a);if(e)return e;
    *got=rr(a->afe,reg);
    if(mach_absolute_time()>=d->restore_deadline)return ETIMEDOUT;
    /* Only the last unconfirmed prepare write may retain its complete old
     * masked value; arbitrary mixtures and previous transient values fail. */
    return (*got&*mask)==(expected&*mask) || (*got&*mask)==(alternate&*mask)?0:EBUSY;
}
static int digital_afe_restore_check(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    for(unsigned i=0;i<d->pending;i++) {
        bool seen=false;
        for(unsigned j=0;j<i;j++)if(d->saved[j].reg==d->saved[i].reg){seen=true;break;}
        if(seen)continue;
        uint32_t got=0,mask=0;
        int e=digital_afe_restore_read(a,d->saved[i].reg,&got,&mask);if(e)return e;
    }
    if(d->pending)d->s->flags|=X9_MIC_DIGITAL_AFE_RESTORE_CHECKED;
    return 0; /* RW conflict filter only, NOT whole DMA write-path drain */
}
static int digital_afe_restore_off(struct afe_journal *a,unsigned reg,uint32_t off_mask)
{
    struct digital_afe_journal *d=&digital_afe;
    uint32_t got=0,mask=0;
    int e=digital_afe_restore_read(a,reg,&got,&mask);if(e)return e;
    off_mask&=mask;if(!off_mask)return 0;
    uint32_t requested=got&~off_mask;
    /* Preserve the original unwind baseline. Fold our STOP into the same
     * issued-field expectations before its possibly effective MMIO write. */
    for(unsigned i=0;i<d->pending;i++)if(d->saved[i].reg==reg)
        d->saved[i].value&=~off_mask;
    wr(a->afe,reg,requested);
    if((rr(a->afe,reg)&mask)!=(requested&mask))return EIO;
    return mach_absolute_time()<d->restore_deadline?0:ETIMEDOUT;
}
static int digital_afe_quiesce(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    if(!d->owned)return 0;
    if(d->poisoned)return d->s->afe_restore_errno?d->s->afe_restore_errno:EIO;
    if(d->quiesced)return 0;
    /* STOP has its own one-second budget, not the expired START deadline.
     * The later restore shares this budget; it cannot restart a failed STOP. */
    uint64_t ticks;nanoseconds_to_absolutetime(1000000000ULL,&ticks);
    d->restore_deadline=mach_absolute_time()+ticks;
    int e=digital_afe_restore_check(a);
    if(!e)e=digital_afe_restore_off(a,RX_UL0,1);
    if(!e && d->pending)delay(125); /* vendor minimum, not a drain witness */
    if(!e)e=digital_afe_restore_off(a,RX0,1);
    if(!e)e=digital_afe_restore_off(a,RX1,1);
    if(!e)e=digital_afe_restore_off(a,RX_PAD,1);
    if(e)return digital_afe_restore_failure(a,e);
    d->quiesced=true;return 0;
}
static int digital_afe_restore(struct afe_journal *a)
{
    struct digital_afe_journal *d=&digital_afe;
    if(!d->owned)return 0;
    int e=digital_afe_quiesce(a);
    while(!e && d->pending) {
        unsigned n=d->pending-1;
        uint32_t got=0,mask=0;
        e=digital_afe_restore_read(a,d->saved[n].reg,&got,&mask);
        mask=d->saved[n].mask;
        if(!e) {
            wr(a->afe,d->saved[n].reg,(got&~mask)|(d->saved[n].old&mask));
            if((rr(a->afe,d->saved[n].reg)&mask)!=(d->saved[n].old&mask))e=EIO;
        }
        if(!e && mach_absolute_time()>=d->restore_deadline)e=ETIMEDOUT;
        if(!e){--d->pending;d->uncertain=false;++d->s->afe_journal_restored;}
    }
    if(!e){e=afe_access(a);if(!e)digital_afe_snapshot(a,false);}
    if(e)return digital_afe_restore_failure(a,e);
    d->owned=false;return 0;
}
#undef RX
