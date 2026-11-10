/* SPDX-License-Identifier: GPL-2.0
 * Ordinary non-VOW/non-HiFi ADC_L DAPM sequence from OPPO mt6681.c.
 * Diagnostic only. AIN0 is a codec pin name, NOT a physical mic position.
 * No UL SRC, MTKAIF TX, test/sine, DMA, PLL208M or right ADC activation. */
#include "x9_mic_mt6681_analog_regs.h"
_Static_assert(sizeof(struct x9_mic_analog_status)==128,"analog ABI");

static int analog_update(struct x9_mic_mt6681 *c,uint16_t reg,uint8_t mask,uint8_t value)
{
    c->status.last_register=reg;
    if(mach_absolute_time()>=c->analog_deadline)return ETIMEDOUT;
    if(c->analog_pending>=80)return EOVERFLOW;
    uint8_t old=0;int e=read8(c,reg,&old);if(e)return e;
    /* Journal BEFORE a potentially successful-but-timed-out transaction. */
    unsigned n=c->analog_pending++;
    c->analog_saved[n].reg=reg;c->analog_saved[n].mask=mask;
    c->analog_saved[n].old=old&mask;c->analog_saved[n].value=value&mask;
    c->analog.journal_count=c->analog_pending;
    e=write8(c,reg,(old&~mask)|(value&mask));
    uint8_t got=0;if(!e)e=read8(c,reg,&got);
    if(!e && (got&mask)!=(value&mask))e=EIO;
    return e;
}
#define ANALOG(reg,field,value) do { \
    e=analog_update(c,(reg),field##_MASK_SFT,(value)<<field##_SFT); \
    if(e)goto failed; \
} while(0)
#define ANALOG_BYTE(reg,value) do { \
    e=analog_update(c,(reg),0xff,(value));if(e)goto failed; \
} while(0)
static int analog_read(struct x9_mic_mt6681 *c,uint16_t reg,uint32_t *out)
{
    uint8_t v=0;int e=read8(c,reg,&v);if(!e)*out=v;return e;
}
static int analog_snapshot(struct x9_mic_mt6681 *c,bool active)
{
    struct x9_mic_analog_status *s=&c->analog;int e=0;
    first_error(&e,analog_read(c,MT6681_AUDENC_PMU_CON0,active?&s->pga_active:&s->pga_after));
    first_error(&e,analog_read(c,MT6681_AUDENC_PMU_CON1,active?&s->adc_active:&s->adc_after));
    first_error(&e,analog_read(c,MT6681_AUDENC_PMU_CON59,active?&s->bias0_active:&s->bias0_after));
    first_error(&e,analog_read(c,MT6681_AUDENC_PMU_CON63,active?&s->bias2_active:&s->bias2_after));
    first_error(&e,analog_read(c,MT6681_AFE_DCCLK1_CFG0,active?&s->dcc_active:&s->dcc_after));
    if(active) {
        first_error(&e,analog_read(c,MT6681_CLKSQ_PMU_CON0,&s->clksq_active));
        first_error(&e,analog_read(c,MT6681_AUD_TOP_CKPDN_CON0,&s->clocks_active));
    }
    return e;
}
static int analog_restore(struct x9_mic_mt6681 *c)
{
    if(!c->analog_owned)return 0;
    struct x9_mic_analog_status *s=&c->analog;
    if(!s->last_errno)s->stage=X9_MIC_ANALOG_RESTORE;
    /* Never restore clocks/bias while a converter or preamp may be live.
     * These off writes are not journaled: all enable baselines were zero. */
    int e=update8(c,MT6681_AUDENC_PMU_CON1,RG_AUDADCLPWRUP_MASK_SFT,0);
    if(!e)e=update8(c,MT6681_AUDENC_PMU_CON0,RG_AUDPREAMPLON_MASK_SFT,0);
    if(!e)e=update8(c,MT6681_AUDENC_PMU_CON59,RG_AUDPWDBMICBIAS0_MASK_SFT,0);
    if(!e)e=update8(c,MT6681_AUDENC_PMU_CON63,RG_AUDPWDBMICBIAS2_MASK_SFT,0);
    if(!e)e=update8(c,MT6681_AFE_DCCLK1_CFG0,
        DCCLK1_PDN_MASK_SFT|DCCLK1_GEN_ON_MASK_SFT,DCCLK1_PDN_MASK_SFT);
    while(!e && c->analog_pending) {
        unsigned n=c->analog_pending-1;
        e=update8(c,c->analog_saved[n].reg,c->analog_saved[n].mask,c->analog_saved[n].old);
        if(!e){--c->analog_pending;++s->journal_restored;}
    }
    if(!e)e=analog_snapshot(c,false);
    if(!e && ((s->adc_after&RG_AUDADCLPWRUP_MASK_SFT) ||
       (s->pga_after&RG_AUDPREAMPLON_MASK_SFT) ||
       (s->bias0_after&RG_AUDPWDBMICBIAS0_MASK_SFT) ||
       (s->bias2_after&RG_AUDPWDBMICBIAS2_MASK_SFT) ||
       (s->dcc_after&DCCLK1_GEN_ON_MASK_SFT)))e=EIO;
    s->restore_errno=e;
    if(e) {s->flags|=X9_MIC_ANALOG_RETAINED;s->last_register=c->status.last_register;}
    else {c->analog_owned=false;s->flags|=X9_MIC_ANALOG_RESTORED;
        if(!s->last_errno)s->stage=X9_MIC_ANALOG_OFF;}
    return e;
}
int x9_mic_mt6681_analog_l(struct x9_mic_mt6681 *c)
{
    if(!c || c->io.flags!=X9_MT6681_REQUIRED || !c->touched || c->poisoned ||
       !c->audio_snapshot || !(c->status.flags&X9_MIC_HW_CALIBRATED) ||
       c->analog.flags)return EINVAL;
    struct x9_mic_analog_status *s=&c->analog;
    *s=(struct x9_mic_analog_status){.version=1,.stage=X9_MIC_ANALOG_ID,
        .flags=X9_MIC_ANALOG_ATTEMPTED,.ain=0,.adc=0,.mic_type=3,
        .bias_mv=2700,.gain_x2=0,.rc_sw=4};
    uint64_t start=mach_absolute_time(),ticks;
    nanoseconds_to_absolutetime(2000000000ULL,&ticks);c->analog_deadline=start+ticks;
    c->status.stage=X9_MIC_HW_ADC;
    int e=analog_read(c,MT6681_HWCID_H,&s->hwcid_h);
    if(!e)e=analog_read(c,MT6681_HWCID_L,&s->hwcid_l);
    if(e)goto failed;
    s->stage=X9_MIC_ANALOG_BASELINE;
    /* All six PGA/ADC and four micbias enables must be off. These are
     * observed only; ADC_R..6/bias1/3 remain untouched. No inherited VOW. */
    const uint16_t pga[]={MT6681_AUDENC_PMU_CON0,MT6681_AUDENC_PMU_CON2,
        MT6681_AUDENC_PMU_CON4,MT6681_AUDENC_PMU_CON6,
        MT6681_AUDENC_2_PMU_CON2,MT6681_AUDENC_2_PMU_CON4};
    const uint16_t adc[]={MT6681_AUDENC_PMU_CON1,MT6681_AUDENC_PMU_CON3,
        MT6681_AUDENC_PMU_CON5,MT6681_AUDENC_PMU_CON7,
        MT6681_AUDENC_2_PMU_CON3,MT6681_AUDENC_2_PMU_CON5};
    for(unsigned i=0;i<6;i++) {
        uint8_t v=0;e=read8(c,pga[i],&v);if(!e && (v&1))e=EBUSY;if(e)goto failed;
        e=read8(c,adc[i],&v);if(!e && (v&0x10))e=EBUSY;if(e)goto failed;
    }
    const uint16_t off[]={MT6681_AUDENC_PMU_CON59,MT6681_AUDENC_PMU_CON61,
        MT6681_AUDENC_PMU_CON63,MT6681_AUDENC_PMU_CON65,
        MT6681_AUDENC_PMU_CON55,MT6681_AUDENC_PMU_CON57,MT6681_AFE_DCCLK1_CFG0};
    for(unsigned i=0;i<sizeof(off)/sizeof(off[0]);i++) {
        uint8_t v=0;e=read8(c,off[i],&v);if(!e && (v&1))e=EBUSY;if(e)goto failed;
    }
    c->analog_owned=true;
    s->stage=X9_MIC_ANALOG_INIT;
    /* Only L initialization, no OPPO gain sentinel mistaken for RC data. */
    ANALOG(MT6681_AUDENC_PMU_CON0,RG_AUDPREAMPLDCCEN,1);
    ANALOG(MT6681_AUDENC_PMU_CON9,RG_AUDADC1STSTAGEIDDTEST,0);
    ANALOG(MT6681_AUDENC_PMU_CON17,RG_AUDRADC1STSTAGEIDDTEST,0);
    ANALOG(MT6681_AUDENC_2_2_PMU_CON17,RG_AUDPREAMPLOUTSTAGE0P25BIAS,0);
    ANALOG(MT6681_AUDENC_2_2_PMU_CON17,RG_ADCLFLASHOFFSETCAL,1);
    ANALOG(MT6681_AUDENC_2_2_PMU_CON23,RG_ADCLFLASHOFFSETCAL_TRIM,1);
    ANALOG(MT6681_AUDENC_2_PMU_CON42,RG_AUDLADCFLASHFFCAPVREF_SEL,1);
    s->stage=X9_MIC_ANALOG_CLOCKS;
    ANALOG(MT6681_AUDDEC_PMU_CON57,RG_AUDGLB_PWRDN_VA32,0);
    ANALOG(MT6681_CLKSQ_PMU_CON0,RG_CLKSQ_EN,1);
    ANALOG(MT6681_CLKSQ_PMU_CON0,RG_CLKSQ_AUDENC_EN,1);
    /* Follow the shared LR clockgen supply, but never power ADC_R/PGA_R. */
    ANALOG(MT6681_AUDENC_PMU_CON12,RG_AUDPREAMPLMODE,2);
    ANALOG(MT6681_AUDENC_PMU_CON18,RG_AUDADCLCLKGENMODE,0);
    ANALOG(MT6681_AUDENC_PMU_CON18,RG_AUDADCLCLKSOURCE,5);
    ANALOG(MT6681_AUDENC_PMU_CON12,RG_AUDPREAMPRMODE,2);
    ANALOG(MT6681_AUDENC_PMU_CON19,RG_AUDADCRCLKGENMODE,0);
    ANALOG(MT6681_AUDENC_PMU_CON19,RG_AUDADCRCLKSOURCE,5);
    ANALOG(MT6681_AUDENC_PMU_CON81,RG_ADCL_CLKMODE,1);
    ANALOG(MT6681_AUDENC_PMU_CON81,RG_ADCR_CLKMODE,1);
    ANALOG(MT6681_AUD_TOP_CKPDN_CON0,RG_AUDIF_CK_PDN,0);
    ANALOG(MT6681_AUD_TOP_CKPDN_CON0,RG_AUD_CK_PDN,0);delay(100);
    /* NCP_CK belongs to DL Power Supply, not this ordinary ADC route. */
    s->stage=X9_MIC_ANALOG_BIAS;
    ANALOG(MT6681_AUDENC_PMU_CON59,RG_AUDMICBIAS0VREF,7);
    ANALOG(MT6681_AUDENC_PMU_CON59,RG_AUDMICBIAS0LOWPEN,0);
    ANALOG(MT6681_AUDENC_PMU_CON59,RG_AUDMICBIAS0ULTRALOWPEN,0);
    ANALOG(MT6681_AUDENC_PMU_CON63,RG_AUDMICBIAS2VREF,7);
    ANALOG(MT6681_AUDENC_PMU_CON63,RG_AUDMICBIAS2LOWPEN,0);
    ANALOG(MT6681_AUDENC_PMU_CON63,RG_AUDMICBIAS2ULTRALOWPEN,0);
    ANALOG(MT6681_AUDENC_PMU_CON68,RG_AUDACCDETMICBIAS0PULLLOW,1);
    ANALOG(MT6681_AUDENC_PMU_CON68,RG_AUDACCDETMICBIAS2PULLLOW,1);
    ANALOG_BYTE(MT6681_AUDENC_PMU_CON53,0x10);
    ANALOG(MT6681_AUDENC_PMU_CON59,RG_AUDPWDBMICBIAS0,1);
    ANALOG(MT6681_AUDENC_PMU_CON63,RG_AUDPWDBMICBIAS2,1);
    s->stage=X9_MIC_ANALOG_DCC;
    ANALOG_BYTE(MT6681_AFE_DCCLK1_CFG1,0x20);
    ANALOG(MT6681_AFE_DCCLK1_CFG0,DCCLK1_DIV_L,3);
    ANALOG(MT6681_AFE_DCCLK1_CFG0,DCCLK1_PDN,0);
    ANALOG(MT6681_AFE_DCCLK1_CFG0,DCCLK1_GEN_ON,1);
    ANALOG(MT6681_AFE_DCCLK1_CFG0,DCCLK1_REF_CK_SEL,0);
    ANALOG(MT6681_AFE_DCCLK1_CFG2,DCCLK1_RESYNC_BYPASS,1);
    s->stage=X9_MIC_ANALOG_PGA;
    ANALOG(MT6681_AUDENC_PMU_CON0,RG_AUDPREAMPLINPUTSEL,0);
    ANALOG(MT6681_AUDENC_PMU_CON1,RG_AUDADCLINPUTSEL,2);
    ANALOG(MT6681_AUDENC_PMU_CON17,RG_AUDRADCFLASHIDDTEST,0);
    ANALOG(MT6681_AUDENC_PMU_CON22,RG_AUDSPAREPGA1,0);
    ANALOG(MT6681_AUDENC_PMU_CON0,RG_AUDPREAMPLDCPRECHARGE,1);
    ANALOG(MT6681_AUDENC_PMU_CON54,RG_AUDSPAREVA18_L,1);
    ANALOG(MT6681_AUDENC_2_2_PMU_CON0,RG_AUDPREAMPLGAIN_1P5,0);
    ANALOG_BYTE(MT6681_AUDENC_2_2_PMU_CON7,0);
    ANALOG(MT6681_AUDENC_PMU_CON0,RG_AUDPREAMPLON,1);delay(1000);
    s->stage=X9_MIC_ANALOG_ADC;
    ANALOG(MT6681_AUDENC_2_2_PMU_CON16,RG_AUDLDAC_IDAC_SEL,0);
    ANALOG(MT6681_AUDENC_PMU_CON49,RG_AUDADCHIGHDR_EN,1);
    ANALOG(MT6681_AUDENC_PMU_CON49,RG_AUDADCHIGHDRSW_SEL,1);
    ANALOG(MT6681_AUDENC_PMU_CON49,RG_AUDADCHIGHDRSW_EN,1);
    ANALOG(MT6681_AUDENC_PMU_CON24,RG_AUDADCLWIDECM,1);
    ANALOG(MT6681_AUDENC_PMU_CON45,RG_AUDADCLRINOHM,4);
    ANALOG(MT6681_AUDENC_2_PMU_CON39,RG_VCML_PGA_LPM_SEL,9);
    ANALOG(MT6681_AUDENC_2_PMU_CON36,RG_VCML_PGA_HIFI_SEL,8);
    ANALOG(MT6681_AUDENC_2_PMU_CON42,RG_AUDLADCFLASHFFCAPVREF_SEL,s->hwcid_h==3?3:1);
    ANALOG(MT6681_AUDENC_PMU_CON50,RG_AUDADCLFLASHVREFRES_LPM,0);
    ANALOG(MT6681_AUDENC_PMU_CON50,RG_AUDADCLFLASHVREFRES_LPM2,0);
    ANALOG(MT6681_AUDENC_PMU_CON28,RG_AUDRCTUNEL,4);
    ANALOG(MT6681_AUDENC_PMU_CON28,RG_AUDRCTUNELSEL,0);
    ANALOG(MT6681_AUDENC_PMU_CON18,RG_AUDADCLCLKSEL,3);
    ANALOG(MT6681_AUDENC_PMU_CON44,RG_AUDLADCHALFCLK,0);
    ANALOG(MT6681_AUDENC_PMU_CON44,RG_AUDLADCDACMODE_SEL,1);
    ANALOG(MT6681_AUDENC_PMU_CON8,RG_AUDLADC1STSTAGELPEN_1,1);
    ANALOG(MT6681_AUDENC_PMU_CON52,RG_AUDLADC1STSTAGELPEN_0,1);
    ANALOG(MT6681_AUDENC_PMU_CON14,RG_AUDADCLMODE,2);
    ANALOG(MT6681_AUDENC_PMU_CON1,RG_AUDADCLPWRUP,1);delay(500);
    s->flags|=X9_MIC_ANALOG_PREPARED;s->stage=X9_MIC_ANALOG_RC;
    e=analog_read(c,MT6681_AUDENC_PMU_CON32,&s->rc_monitor);if(e)goto failed;
    /* Vendor retries a 0/31 result once. Fail closed instead of resetting.
     * Ordinary mode keeps SW tune=4; monitor is not a PCM success signal. */
    if((s->rc_monitor&31)==0 || (s->rc_monitor&31)==31){e=EIO;goto failed;}
    s->flags|=X9_MIC_ANALOG_RC_OK;
    ANALOG(MT6681_AUDENC_PMU_CON54,RG_AUDSPAREVA18_L,0);
    ANALOG(MT6681_AUDENC_PMU_CON0,RG_AUDPREAMPLDCPRECHARGE,0);
    e=analog_snapshot(c,true);
    if(!e)e=verify_codec_journal(c,c->analog_saved,c->analog_pending,c->analog_deadline);
    if(!e)s->flags|=X9_MIC_ANALOG_READBACK;
failed:
    s->last_errno=e;s->last_register=c->status.last_register;
    if(e)c->status.last_errno=e;
    absolutetime_to_nanoseconds(mach_absolute_time()-start,&s->elapsed_ns);
    return e;
}
#undef ANALOG
#undef ANALOG_BYTE
