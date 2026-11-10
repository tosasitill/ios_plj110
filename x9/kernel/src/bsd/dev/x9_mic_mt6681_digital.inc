/* SPDX-License-Identifier: GPL-2.0
 * OPPO mt6681.c AIFTX supplies, set_ul_src and R-MISO1 TX. This diagnostic
 * intentionally bypasses codec IIR (no assumed reset/probe coefficients).
 * No efuse, VD105 change, second ADC/UL SRC, playback, sine or loopback. */
#include "x9_mic_mt6681_digital_regs.h"
_Static_assert(sizeof(struct x9_mic_digital_status)==192,"digital ABI");

static int digital_update(struct x9_mic_mt6681 *c,uint16_t reg,uint8_t mask,uint8_t value)
{
    c->digital->last_register=reg;
    if(mach_absolute_time()>=c->digital_deadline)return ETIMEDOUT;
    if(c->digital_pending>=64)return EOVERFLOW;
    uint8_t old=0;int e=read8(c,reg,&old);if(e)return e;
    unsigned n=c->digital_pending++;
    c->digital_saved[n].reg=reg;c->digital_saved[n].mask=mask;c->digital_saved[n].old=old&mask;
    c->digital->codec_journal_count=c->digital_pending;
    return update8(c,reg,mask,value);
}
#define DIGITAL_BYTE(reg,mask,value) do {e=digital_update(c,(reg),(mask),(value));if(e)return e;} while(0)
static int digital_pack(struct x9_mic_mt6681 *c,const uint16_t *regs,unsigned n,uint32_t *out)
{
    uint32_t v=0;
    for(unsigned i=0;i<n;i++) {uint8_t b=0;int e=read8(c,regs[i],&b);if(e)return e;v=(v<<8)|b;}
    *out=v;return 0;
}
static int digital_codec_snapshot(struct x9_mic_mt6681 *c,bool active)
{
    struct x9_mic_digital_status *s=c->digital;
    const uint16_t src[]={MT6681_AFE_ADDA_UL_SRC_CON0_2,MT6681_AFE_ADDA_UL_SRC_CON0_1,MT6681_AFE_ADDA_UL_SRC_CON0_0};
    const uint16_t top[]={MT6681_AUDIO_TOP_CON0,MT6681_AFE_TOP_CON0};
    int e=digital_pack(c,src,3,active?&s->codec_src:&s->codec_after_src);
    if(!e)e=digital_pack(c,top,2,active?&s->codec_top:&s->codec_after_top);
    if(!e)e=analog_read(c,MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,active?&s->codec_tx[0]:&s->codec_after_tx[0]);
    if(!e)e=analog_read(c,MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,active?&s->codec_tx[1]:&s->codec_after_tx[1]);
    if(!e)e=analog_read(c,MT6681_AFE_AUD_PAD_TOP,active?&s->codec_pad:&s->codec_after_pad);
    if(!e)e=analog_read(c,MT6681_GPIO_MODE4,active?&s->codec_mode4:&s->codec_after_mode4);
    if(active && !e)e=analog_read(c,MT6681_AFE_MTKAIF_MUX_CFG,&s->codec_mux[0]);
    if(active && !e)e=analog_read(c,MT6681_AFE_MTKAIF_MUX_CFG_H,&s->codec_mux[1]);
    return e;
}
int x9_mic_mt6681_digital_stop(struct x9_mic_mt6681 *c)
{
    if(!c->digital_owned)return 0;
    struct x9_mic_digital_status *s=c->digital;
    /* All four enables were observed off. Quiesce both TXs (vendor's
     * R-MISO1 disable only stops TX2) before restoring supplies/GPIO. */
    int e=update8(c,MT6681_AFE_ADDA_UL_SRC_CON0_0,ADDA_UL_SRC_ON_TMP_CTL_MASK_SFT,0);
    if(!e)e=update8(c,MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,1,0);
    if(!e)e=update8(c,MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,1,0);
    if(!e)e=update8(c,MT6681_AFE_AUD_PAD_TOP,9,0);
    if(!e)delay(125);
    while(!e && c->digital_pending) {
        unsigned n=c->digital_pending-1;
        s->last_register=c->digital_saved[n].reg;
        e=update8(c,c->digital_saved[n].reg,c->digital_saved[n].mask,c->digital_saved[n].old);
        if(!e){--c->digital_pending;++s->codec_journal_restored;}
    }
    if(!e)e=digital_codec_snapshot(c,false);
    if(!e && ((s->codec_after_src&1) || (s->codec_after_tx[0]&1) ||
       (s->codec_after_tx[1]&1) || (s->codec_after_pad&9)))e=EIO;
    if(e){s->restore_errno=e;s->flags|=X9_MIC_DIGITAL_RETAINED;
        (void)restore_failure(c,e,X9_MIC_HW_RESTORE_DIGITAL);}
    else c->digital_owned=false;
    return e;
}
int x9_mic_mt6681_digital_l(struct x9_mic_mt6681 *c,struct x9_mic_digital_status *s)
{
    if(!c || !s || c->digital || c->poisoned || c->io.flags!=X9_MT6681_REQUIRED ||
       !c->analog_owned || !(c->analog.flags&X9_MIC_ANALOG_RC_OK))return EINVAL;
    c->digital=s;s->stage=X9_MIC_DIGITAL_BASELINE;
    uint64_t ticks;nanoseconds_to_absolutetime(1000000000ULL,&ticks);
    c->digital_deadline=mach_absolute_time()+ticks;
    /* No inherited SRC/DMIC/AFE/test/TX or alternate protocol. */
    const struct {uint16_t reg;uint8_t mask;} off[]={
        {MT6681_AFE_ADDA_UL_SRC_CON0_0,1},{MT6681_AFE_ADDA6_UL_SRC_CON0_0,1},
        {MT6681_AFE_ADDA7_UL_SRC_CON0_0,1},{MT6681_AFE_ADDA_UL_DL_CON0_0,3},
        {MT6681_AFE_TOP_CON0,3},{MT6681_AFE_SINEGEN_CON0,SINEGEN_DAC_EN_MASK_SFT},
        {MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,1},{MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,1},
        {MT6681_AFE_MTKAIFV4_TX_CFG,MT6681_MTKAIFV4_LOOPBACK2_MASK_SFT},
        {MT6681_AFE_ADDA_MTKAIFV4_RX_CFG0,5},{MT6681_AFE_ADDA6_MTKAIFV4_RX_CFG0,5}};
    int e=0;uint8_t v=0;
    for(unsigned i=0;i<sizeof(off)/sizeof(off[0]);i++) {
        s->last_register=off[i].reg;e=read8(c,off[i].reg,&v);if(e)return e;
        if(v&off[i].mask)return EBUSY;
    }
    s->last_register=MT6681_AFE_AUD_PAD_TOP;e=read8(c,MT6681_AFE_AUD_PAD_TOP,&v);
    if(e)return e;
    if(v&9)return EBUSY;
    /* Fresh reset + completed phase scan must retain no codec loopback. */
    const uint16_t phases[]={MT6681_AUDIO_DIG_CFG,MT6681_AUDIO_DIG_CFG_H,MT6681_AUDIO_DIG_CFG1};
    for(unsigned i=0;i<3;i++){s->last_register=phases[i];e=read8(c,phases[i],&v);if(e)return e;
        if(v&0x80)return EBUSY;}
    c->digital_owned=true;s->stage=X9_MIC_DIGITAL_CODEC_CLOCKS;
    /* codec_init_reg's ordinary MTKAIFv4 clock source is internal DCXO. */
    DIGITAL_BYTE(MT6681_AUD_TOP_CKSEL_CON0,0x0c,0);
    DIGITAL_BYTE(MT6681_AUD_TOP_SRAM_CON,AUDIO_CO_MEM_PDN_MASK_SFT|AUDIO_CO_MEM_PDN_SEL_MASK_SFT,0);
    delay(250);
    /* Match all AIFTX digital ADC supplies, not ADC_R/3..6 converter power.
     * Vendor's PDN_ADC_CTL naming aliases bit5 here; keep its exact route. */
    DIGITAL_BYTE(MT6681_AUDIO_TOP_CON0,PDN_ADC_CTL_MASK_SFT|PDN_ADDA6_ADC_CTL_MASK_SFT|
        PDN_ADDA7_ADC_CTL_MASK_SFT|PDN_AFE_CTL_MASK_SFT|PWR_CLK_DIS_CTL_MASK_SFT,0);
    DIGITAL_BYTE(MT6681_AUDIO_TOP_CON1,0xff,0xdf);
    DIGITAL_BYTE(MT6681_AUDIO_TOP_CON2,0xff,0);
    DIGITAL_BYTE(MT6681_AUDIO_TOP_CON3,0xff,0xc1);
    DIGITAL_BYTE(MT6681_AFE_TOP_CON0,AFE_ON_MASK_SFT,1);
    DIGITAL_BYTE(MT6681_AFE_ADDA_UL_DL_CON0_0,MT6681_ADDA_AFE_ON_MASK_SFT,1);
    /* R-MISO1 ordinary capture uses MODE4, not the calibration ch56 GPIO.
     * Keep CLK_MOSI and MOSI0/1 for the AP PAD_CLK; no third MISO. */
    DIGITAL_BYTE(MT6681_GPIO_MODE2,0x77,0x11);
    DIGITAL_BYTE(MT6681_GPIO_MODE3,0x77,0x11);
    DIGITAL_BYTE(MT6681_GPIO_MODE4,0x70,0x20);
    DIGITAL_BYTE(MT6681_AFE_MTKAIF_MUX_CFG,RG_ADDA_CH1_SEL_MASK_SFT|RG_ADDA_CH2_SEL_MASK_SFT,0);
    DIGITAL_BYTE(MT6681_AFE_MTKAIF_MUX_CFG_H,RG_ADDA7_CH1_SEL_MASK_SFT|RG_ADDA7_CH2_SEL_MASK_SFT,0);
    s->stage=X9_MIC_DIGITAL_CODEC_SRC;
    DIGITAL_BYTE(MT6681_AFE_ADDA_UL_SRC_CON0_3,ADDA_ULCF_CFG_EN_CTL_MASK_SFT,0);
    DIGITAL_BYTE(MT6681_AFE_ADDA_UL_SRC_CON0_2,0x1f,3); /* AMIC / voice48k */
    DIGITAL_BYTE(MT6681_AFE_ADDA_UL_SRC_CON0_1,0x6f,0); /* explicit IIR bypass */
    DIGITAL_BYTE(MT6681_AFE_ADDA_UL_SRC_CON0_0,7,0); /* AMIC, no loopback */
    DIGITAL_BYTE(MT6681_AFE_TOP_DEBUG1,0xff,0x11);
    DIGITAL_BYTE(MT6681_AFE_ADDA_UL_SRC_CON0_0,1,1);
    s->stage=X9_MIC_DIGITAL_CODEC_TX;
    DIGITAL_BYTE(MT6681_AFE_AUD_PAD_TOP,0xff,0x39);
    DIGITAL_BYTE(MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,0xff,0x29);
    DIGITAL_BYTE(MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,0xff,0x2b);
    /* VD105 is strap +0xc at probe then -0xc at TX: preserve, don't guess. */
    e=x9_mic_mt6681_verify_capture(c);
    if(!e)e=digital_codec_snapshot(c,true);
    if(!e)s->flags|=X9_MIC_DIGITAL_CODEC_ON;
    return e;
}
#undef DIGITAL_BYTE
