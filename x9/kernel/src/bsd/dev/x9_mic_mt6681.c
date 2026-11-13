/* SPDX-License-Identifier: GPL-2.0
 * MT6681 register sequences adapted from MediaTek (c) 2023:
 * drivers/mfd/mt6681-core.c InitSetting/Suspend_Setting;
 * sound/soc/codecs/mt6681.c keylock_reset, MTKAIFV4_SUPPORT calibration;
 * sound/soc/mediatek/mt6993/mt6993-mt6681.c phase scan.
 * XNU orchestration/timeout/error handling is independent. This port rejects
 * vendor's silent phase=0 fallback; no calibration loopback reaches VUL1.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include "x9_mic_mt6681.h"
#include "x9_mic_mt6681_regs.h"

enum { PAD_CFG=0x1204, TEST_CFG=0x98, TEST_MON=0x9c };
struct setting { uint16_t reg; uint8_t value; };
static const struct setting keys[] = {
    {MT6681_TOP_DIG_WPK,0x81},{MT6681_TOP_DIG_WPK_H,0x66},
    {MT6681_TOP_TMA_KEY,0x7e},{MT6681_TOP_TMA_KEY_H,0x99},
    {MT6681_PSC_WPK_L,0x29},{MT6681_PSC_WPK_H,0x47},
    {MT6681_HK_TOP_WKEY_L,0x81},{MT6681_HK_TOP_WKEY_H,0x66}
};
/* These include hardware-trigger writes, not a replayable register journal.
 * Only a fresh, exclusively owned codec reset permits their execution. */
static const struct setting cold_init[] = {
    {MT6681_TOP_CON,7},{MT6681_TEST_CON0,0x1f},{MT6681_SMT_CON0,3},
    {MT6681_GPIO_PULLEN0,0xf9},{MT6681_GPIO_PULLEN1,0x1f},
    {MT6681_TOP_CKPDN_CON0,0x5b},{MT6681_HK_TOP_CLK_CON0,0x15},
    {MT6681_DA_INTF_STTING3,0x0c},{MT6681_MTC_CTL0,0x13},
    {MT6681_PLT_CON0,0},{MT6681_PLT_CON1,0},
    {MT6681_HK_TOP_CLK_CON0,0x15},{MT6681_AUXADC_CON0,0},
    {MT6681_AUXADC_TRIM_SEL2,0x40},{MT6681_TOP_TOP_CKHWEN_CON0,0x0f},
    {MT6681_LDO_TOP_CLK_DCM_CON0,1},{MT6681_LDO_TOP_VR_CLK_CON0,0},
    {MT6681_LDO_VAUD18_CON2,0x1c},{MT6681_PLT_CON0,0x3a},
    {MT6681_PLT_CON1,0x0c},{MT6681_LDO_VAUD18_MULTI_SW_0,1},
    {MT6681_MTC_CTL0,0x10},{MT6681_MTC_CTL0,0x11},{MT6681_MTC_CTL0,0x13},
    {MT6681_DA_INTF_STTING3,8},{MT6681_LDO_VAUD18_CON2,0x1c},
    {MT6681_LDO_VAUD18_OP_EN0,1},{MT6681_LDO_VAUD18_OP_CFG0,1},
    {MT6681_DA_INTF_STTING1,0x64},{MT6681_DA_INTF_STTING1,0x66},
    {MT6681_DA_INTF_STTING1,0x76}
};
static const struct setting active_init[] = {
    {MT6681_TOP_CON,7},{MT6681_TEST_CON0,0x1f},{MT6681_TOP_CKPDN_CON0,0x5b},
    {MT6681_DA_INTF_STTING3,8},{MT6681_MTC_CTL0,0x13},
    {MT6681_PLT_CON0,0x3a},{MT6681_PLT_CON1,0x0c},
    {MT6681_HK_TOP_CLK_CON0,0x15},{MT6681_AUXADC_CON0,0},
    {MT6681_AUXADC_TRIM_SEL2,0x40},{MT6681_TOP_TOP_CKHWEN_CON0,0x0f},
    {MT6681_LDO_TOP_CLK_DCM_CON0,1},{MT6681_LDO_TOP_VR_CLK_CON0,0},
    {MT6681_LDO_VAUD18_CON2,0x1c},{MT6681_DA_INTF_STTING1,0x76},
    {MT6681_DA_INTF_STTING3,8},{MT6681_TSBG_PMU_CON2,0},
    {MT6681_STRUP_ELR_3,1}
};
/* MT6681_MTC_VOW_CTL0 is deliberately excluded: this is ordinary capture,
 * not VOW, and no VOW/SCP session is owned by this sequencer. */
static const uint16_t audio_regs[] = {
    MT6681_GPIO_PULLEN0, MT6681_GPIO_MODE2, MT6681_GPIO_MODE3,
    MT6681_GPIO_MODE5, MT6681_AFE_AUD_PAD_TOP,
    MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0, MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,
    MT6681_CLKSQ_PMU_CON0, MT6681_AUDDEC_PMU_CON57,
    MT6681_AUD_TOP_CKPDN_CON0, MT6681_LDO_VAUD18_CON0
};
_Static_assert(sizeof(audio_regs)/sizeof(audio_regs[0])==11,"snapshot size");
static uint32_t afe_read(struct x9_mic_mt6681 *c,uint32_t r)
{
    c->status.last_register=0x80000000u|r;
    uint32_t v=c->afe[r/4];__asm__ volatile("dmb oshld":::"memory");return v;
}
static int afe_write(struct x9_mic_mt6681 *c,uint32_t r,uint32_t v)
{
    c->status.last_register=0x80000000u|r;
    c->afe[r/4]=v;__asm__ volatile("dsb oshst":::"memory");
    return afe_read(c,r)==v?0:EIO;
}
static int read8(struct x9_mic_mt6681 *c,uint16_t r,uint8_t *v)
{
    c->status.last_register=r;return c->io.read8(c->io.cookie,r,v);
}
static int write8(struct x9_mic_mt6681 *c,uint16_t r,uint8_t v)
{
    c->status.last_register=r;return c->io.write8(c->io.cookie,r,v);
}
/* Use only ordinary readable RW registers, never SET/CLR, keys or triggers. */
static int update8(struct x9_mic_mt6681 *c,uint16_t r,uint8_t mask,uint8_t v)
{
    uint8_t old=0,got=0;int e=read8(c,r,&old);
    if(!e)e=write8(c,r,(old&~mask)|(v&mask));
    if(!e)e=read8(c,r,&got);
    if(!e && (got&mask)!=(v&mask))e=EIO;
    return e;
}
static int settings(struct x9_mic_mt6681 *c,const struct setting *s,unsigned n)
{
    for(unsigned i=0;i<n;i++){int e=write8(c,s[i].reg,s[i].value);if(e)return e;}
    return 0;
}
#define SETTINGS(c,a) settings((c),(a),sizeof(a)/sizeof((a)[0]))
static void first_error(int *e,int next){if(!*e && next)*e=next;}
static int verify_codec_journal(struct x9_mic_mt6681 *c,
    const struct x9_mic_codec_saved *saved,unsigned count,uint64_t deadline)
{
    if(c->poisoned || !count)return ENXIO;
    for(unsigned i=0;i<count;i++) {
        bool seen=false;
        for(unsigned j=0;j<i;j++)if(saved[j].reg==saved[i].reg){seen=true;break;}
        if(seen)continue;
        /* Fold repeated/overlapping writes in issue order. Checking an older
         * transient request would reject a correctly completed sequence. */
        uint8_t mask=0,expected=0;
        for(unsigned j=i;j<count;j++)if(saved[j].reg==saved[i].reg) {
            expected=(expected&~saved[j].mask)|(saved[j].value&saved[j].mask);
            mask|=saved[j].mask;
        }
        c->status.last_register=saved[i].reg;
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        uint8_t got=0;int e=read8(c,saved[i].reg,&got);if(e)return e;
        if((got&mask)!=(expected&mask))return EIO;
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
    }
    return 0; /* bounded non-atomic RW checks, not ownership/PCM proof */
}
static int restore_failure(struct x9_mic_mt6681 *c,int e,uint32_t stage)
{
    if(!e)return 0;
    /* Keep the first unwind failure independently of the input failure.
     * Once poisoned no caller may retry or release parent dependencies. */
    if(!c->status.restore_stage) {
        c->status.restore_register=c->status.last_register;
        c->status.restore_stage=stage;
    }
    if(!c->status.restore_errno)c->status.restore_errno=e;
    c->poisoned=true;c->status.flags|=X9_MIC_HW_RESTORE_FAILED;
    return e;
}
#include "x9_mic_mt6681_analog.inc"
#include "x9_mic_mt6681_digital.inc"
static int restore_audio(struct x9_mic_mt6681 *c)
{
    if(c->poisoned)return c->status.restore_errno?c->status.restore_errno:EIO;
    int e=0;
    if(c->afe_snapshot) {
        /* A failed off/readback is a barrier, not best-effort cleanup. */
        e=afe_write(c,TEST_CFG,afe_read(c,TEST_CFG)&~1u);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_TEST);
    }
    if(c->audio_snapshot) {
        /* Stop every loopback/TX before changing either end's pad clock.
         * Full-byte TX off writes now require ordinary RW readback too. */
        e=update8(c,MT6681_AUDIO_DIG_CFG,0x80,0);
        if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG_H,0x80,0);
        if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG1,0x80,0);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_LOOPBACK);
        e=update8(c,MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,0xff,0);
        if(!e)e=update8(c,MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,0xff,0);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_TX);
        e=update8(c,MT6681_AFE_AUD_PAD_TOP,0xff,0x30);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_CODEC_PADS);
    }
    if(c->afe_snapshot) {
        e=afe_write(c,PAD_CFG,c->old_pad);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_AFE_PADS);
        e=afe_write(c,TEST_CFG,c->old_test);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_TEST);
    }
    if(c->audio_snapshot) {
        /* Reverse power dependencies, not merely register address order. */
        const unsigned order[]={9,7,8,6,5,4,3,2,1,0,10};
        for(unsigned i=0;i<sizeof(order)/sizeof(order[0]);i++) {
            unsigned j=order[i];
            e=update8(c,audio_regs[j],0xff,c->audio_saved[j]);
            if(e)return restore_failure(c,e,j==10?X9_MIC_HW_RESTORE_VAUD18:
                j>=7?X9_MIC_HW_RESTORE_CLOCKS:j>=5?X9_MIC_HW_RESTORE_TX:
                X9_MIC_HW_RESTORE_CODEC_PADS);
        }
    }
    return e;
}
int x9_mic_mt6681_unprepare(struct x9_mic_mt6681 *c)
{
    if(c->poisoned)return c->status.restore_errno?c->status.restore_errno:EIO;
    if(!c->touched)return c->poisoned?EIO:0;
    int e=x9_mic_mt6681_digital_stop(c);
    if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_DIGITAL);
    e=analog_restore(c);
    if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_ANALOG);
    /* Do not cut transport/clocks after an uncertain analog shutdown. */
    e=restore_audio(c);if(e)return e;
    /* Actual protection keys, NOT vendor's misleading keylock_set name. */
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
        e=write8(c,keys[i].reg,0);
        if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_KEYS);
    }
    c->touched=c->audio_snapshot=c->afe_snapshot=false;
    c->status.flags&=~X9_MIC_HW_POWERED;
    return 0;
}
static int set_phases(struct x9_mic_mt6681 *c,int p0,int p1)
{
    int e=update8(c,MT6681_AUDIO_DIG_CFG,0x7f,(uint8_t)p0);
    if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG_H,0x7f,(uint8_t)p1);
    if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG1,0x7f,0); /* no third MISO */
    return e;
}
static void vaud18_dump(struct x9_mic_mt6681 *c)
{
    static const uint16_t r[]={0x1b87,0x1b88,0x1b89,0x1b8a,0x1b8b,0x1b8e,0x1b91,0x1b94,
        MT6681_MTC_CTL0,MT6681_DA_INTF_STTING3};
    uint8_t v[sizeof(r)/sizeof(r[0])]={0};
    for(unsigned i=0;i<sizeof(r)/sizeof(r[0]);i++)(void)c->io.read8(c->io.cookie,r[i],&v[i]);
    printf("X9 MIC VAUD18: con0=%02x con1=%02x con2=%02x mon=%02x op_en0=%02x op_en1=%02x op_cfg0=%02x op_cfg1=%02x mtc=%02x intf3=%02x\n",
        v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
}
static int enable_vaud18(struct x9_mic_mt6681 *c)
{
    /* Log 268: EN0 set but MON=0x03, bandgap enabled/stable, LDO EN/STB
     * (bits3/4) never rose within 10 ms. OPPO mt_vaud18_event PRE_PMU also
     * sets LDO_VAUD18_OP_EN1 HW14 (0x1b8f SET, bit6) before EN0 and never
     * clears it; do the same, wait up to 50 ms and dump the LDO on timeout. */
    int e=write8(c,0x1b8f,0x40);if(e)return e;
    e=update8(c,MT6681_LDO_VAUD18_CON0,1,1);if(e)return e;
    uint64_t ticks;nanoseconds_to_absolutetime(50000000,&ticks);
    uint64_t deadline=mach_absolute_time()+ticks;
    for(unsigned i=0;i<500;i++) {
        uint8_t v=0;e=read8(c,MT6681_LDO_VAUD18_MON,&v);
        c->status.vaud18_monitor=v;if(e)return e;
        /* Both bandgap and LDO report actual enable and stable, not just the
         * software request. Do not change VOSEL/VOCAL or copy efuse defaults. */
        if((v&0x1b)==0x1b)return 0;
        if(mach_absolute_time()>=deadline)break;delay(100);
    }
    vaud18_dump(c);
    return ETIMEDOUT;
}
int x9_mic_mt6681_verify_capture(struct x9_mic_mt6681 *c)
{
    if(!c->touched || c->poisoned || !(c->status.flags&X9_MIC_HW_CALIBRATED) ||
       !c->analog_owned || !(c->analog.flags&X9_MIC_ANALOG_READBACK) ||
       !c->digital_owned || !c->digital)return ENXIO;
    /* Requalify the journaled active codec RW fields, not just TX and phase,
     * after downstream preparation may have changed clocks. */
    int e=verify_codec_journal(c,c->analog_saved,c->analog_pending,c->digital_deadline);
    if(!e)e=verify_codec_journal(c,c->digital_saved,c->digital_pending,c->digital_deadline);
    if(e){c->digital->last_register=c->status.last_register;return e;}
    uint8_t v=0;e=read8(c,MT6681_AUDIO_DIG_CFG,&v);
    if(!e && v!=(uint8_t)c->status.chosen_phase[0])e=EIO;
    if(!e)e=read8(c,MT6681_AUDIO_DIG_CFG_H,&v);
    if(!e && v!=(uint8_t)c->status.chosen_phase[1])e=EIO;
    if(!e)e=read8(c,MT6681_AUDIO_DIG_CFG1,&v);
    if(!e && (v&0x80))e=EIO;
    /* PLJ110 R-MISO1 capture: TX1 two channels, TX2 four channels, 48k. */
    if(!e)e=read8(c,MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,&v);
    if(!e && v!=0x29)e=EIO;
    if(!e)e=read8(c,MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,&v);
    if(!e && v!=0x2b)e=EIO;
    if(!e && mach_absolute_time()>=c->digital_deadline)e=ETIMEDOUT;
    return e;
}
static int run_phase(struct x9_mic_mt6681 *c,int cycle[2],uint64_t deadline)
{
    uint32_t off=afe_read(c,TEST_CFG)&~1u;
    int e=afe_write(c,TEST_CFG,off);
    if(e)return restore_failure(c,e,X9_MIC_HW_RESTORE_TEST);
    /* Never accept leftover DONE from the previous test. Hardware must retire
     * it after TEST_ON=0. A platform with different semantics needs evidence,
     * not a success fallback. Both iteration and wall-time bounds apply. */
    unsigned i;
    for(i=0;i<500;i++) {
        if(!(afe_read(c,TEST_MON)&(3u<<28)))break;
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        delay(2);
    }
    if(i==500)return EIO;
    e=afe_write(c,TEST_CFG,off|1u);if(e)return e;
    for(i=0;i<2500;i++) {
        uint32_t m=afe_read(c,TEST_MON);c->status.monitor=m;
        if((m&(3u<<28))==(3u<<28)) {
            cycle[0]=(int)(m&15);cycle[1]=(int)((m>>4)&15);
            e=0;break;
        }
        if(mach_absolute_time()>=deadline){e=ETIMEDOUT;break;}
        delay(2);
    }
    if(i==2500)e=ETIMEDOUT;
    uint32_t failed_register=c->status.last_register;
    int stop=afe_write(c,TEST_CFG,off);
    if(stop)(void)restore_failure(c,stop,X9_MIC_HW_RESTORE_TEST);
    if(e)c->status.last_register=failed_register;
    return e?e:stop;
}
static int calibrate(struct x9_mic_mt6681 *c)
{
    int e=0;uint64_t start=mach_absolute_time(),ticks;
    nanoseconds_to_absolutetime(300000000,&ticks);uint64_t deadline=start+ticks;
    /* Codec 48k rate is 5, unlike the AFE VUL1 48k code 0xa. Calibration TX
     * uses four-channel on both outputs (vendor is_ch56=true branch). */
    e=update8(c,MT6681_GPIO_PULLEN0,0x30,0x30);
    if(!e)e=update8(c,MT6681_GPIO_MODE2,0x7f,0x11);
    if(!e)e=update8(c,MT6681_GPIO_MODE3,0x7f,0x11);
    if(!e)e=update8(c,MT6681_GPIO_MODE5,0x70,0x10);
    if(!e)e=update8(c,MT6681_AFE_AUD_PAD_TOP,0xff,0x39);
    if(!e)e=update8(c,MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,0xff,0x2b);
    if(!e)e=update8(c,MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,0xff,0x2b);
    if(!e)e=update8(c,MT6681_AUDDEC_PMU_CON57,0x10,0);
    if(!e)e=update8(c,MT6681_CLKSQ_PMU_CON0,0xff,0x0a);
    if(!e)e=update8(c,MT6681_AUD_TOP_CKPDN_CON0,0xff,0);
    if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG,0x80,0x80);
    if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG_H,0x80,0x80);
    if(!e)e=update8(c,MT6681_AUDIO_DIG_CFG1,0x80,0x80);
    if(!e)e=afe_write(c,PAD_CFG,(c->old_pad&~0xffu)|0xb8);
    if(!e)e=afe_write(c,PAD_CFG,(c->old_pad&~0xffu)|0xb9);
    if(!e)e=afe_write(c,TEST_CFG,(c->old_test&~0xffffu)|4);
    int previous[2]={-1,-1};
    for(int phase=0;phase<=42 && !e;phase++) {
        if(mach_absolute_time()>=deadline){e=ETIMEDOUT;break;}
        e=set_phases(c,phase,phase);int cycle[2]={-1,-1};
        if(!e)e=run_phase(c,cycle,deadline);
        if(e)break;
        c->status.scanned_phases++;
        for(unsigned pin=0;pin<2;pin++) {
            if(!phase)previous[pin]=cycle[pin];
            else if(cycle[pin]!=previous[pin] && c->status.chosen_phase[pin]<0) {
                c->status.chosen_phase[pin]=phase-1;c->status.cycle[pin]=previous[pin];
            }
        }
    }
    if(!e && (c->status.chosen_phase[0]<0 || c->status.chosen_phase[1]<0))e=ENXIO;
    if(!e) {
        /* Check the selected phase pair itself, not just the scan transition. */
        int cycle[2]={-1,-1};
        e=set_phases(c,c->status.chosen_phase[0],c->status.chosen_phase[1]);
        if(!e)e=run_phase(c,cycle,deadline);
        if(!e && (cycle[0]!=c->status.cycle[0] || cycle[1]!=c->status.cycle[1]))e=EIO;
    }
    int input_error=e;
    uint32_t failed_register=c->status.last_register;
    int off=restore_audio(c);
    if(!e)e=off;
    if(!e) {
        e=set_phases(c,c->status.chosen_phase[0],c->status.chosen_phase[1]);
        /* Re-enable only VAUD18 for the subsequent real ADC preparation. */
        if(!e)e=enable_vaud18(c);
    }
    if(!e)c->status.flags|=X9_MIC_HW_CALIBRATED;
    /* Only an input/scan failure predating restore owns failed_register.
     * An unwind or post-restore phase/VAUD18 error must keep its own address. */
    if(e && input_error)c->status.last_register=failed_register;
    absolutetime_to_nanoseconds(mach_absolute_time()-start,&c->status.calibration_ns);
    return e;
}
int x9_mic_mt6681_power_on(struct x9_mic_mt6681 *c,
    const struct x9_mic_mt6681_io *io)
{
    if(!c || !io || io->version!=1 || !(io->flags&X9_MT6681_COLD_OWNED) ||
       (io->flags&~X9_MT6681_REQUIRED) || !io->read8 || !io->write8)return EINVAL;
    if(c->touched || c->poisoned)return EBUSY;
    bzero(c,sizeof(*c));c->io=*io;c->status.version=1;
    c->status.stage=X9_MIC_HW_ID;
    c->status.chosen_phase[0]=c->status.chosen_phase[1]=-1;
    c->status.cycle[0]=c->status.cycle[1]=-1;
    uint64_t ticks;nanoseconds_to_absolutetime(50000000,&ticks);
    uint64_t deadline=mach_absolute_time()+ticks;
    uint8_t ready=0,id=0;int e=0;
    for(unsigned i=0;i<500;i++) {
        /* OPPO mt6681_retry_wait keeps polling through failed reads for
         * 50 ms; a NACK (EIO, bus recovered) is retried, other errors stop. */
        e=read8(c,MT6681_ACDIF_MON0,&ready);if((e && e!=EIO) || (!e && (ready&2)))break;
        if(mach_absolute_time()>=deadline)break;delay(100);
    }
    c->status.internal_ready=ready;
    if(!e && !(ready&2))e=ETIMEDOUT;
    if(!e)e=read8(c,MT6681_SWCID_H,&id);
    c->status.swcid=id;
    if(!e && id!=0x81)e=ENODEV;
    if(!e) {
        c->touched=true;c->status.stage=X9_MIC_HW_INIT;
        e=SETTINGS(c,keys);
        if(!e)e=SETTINGS(c,cold_init);
        if(!e)e=SETTINGS(c,active_init);
    }
    if(!e) {
        /* Reject any unexpected inherited TX/loopback/test state before
         * saving/restoring it. This port may never reinstate an active source. */
        const uint16_t idle[]={MT6681_AFE_ADDA_MTKAIFV4_TX_CFG0,
            MT6681_AFE_ADDA6_MTKAIFV4_TX_CFG0,MT6681_AUDIO_DIG_CFG,
            MT6681_AUDIO_DIG_CFG_H,MT6681_AUDIO_DIG_CFG1};
        for(unsigned i=0;i<sizeof(idle)/sizeof(idle[0]) && !e;i++) {
            uint8_t v=0;e=read8(c,idle[i],&v);
            if(!e && (v&(i<2?1:0x80)))e=EBUSY;
        }
        for(unsigned i=0;i<11 && !e;i++)e=read8(c,audio_regs[i],&c->audio_saved[i]);
        if(!e)c->audio_snapshot=true;
    }
    if(!e)e=enable_vaud18(c);
    if(!e)c->status.flags|=X9_MIC_HW_POWERED;
    if(e) {
        c->status.last_errno=e;uint32_t r=c->status.last_register;
        (void)x9_mic_mt6681_unprepare(c);c->status.last_register=r;
    }
    return e;
}
int x9_mic_mt6681_power_calibrate(struct x9_mic_mt6681 *c,
    const struct x9_mic_mt6681_io *io,volatile uint32_t *afe)
{
    if(!io || io->flags!=X9_MT6681_REQUIRED || !afe || ((uintptr_t)afe&3))return EINVAL;
    int e=x9_mic_mt6681_power_on(c,io);if(e)return e;
    return x9_mic_mt6681_calibrate_powered(c,afe);
}
int x9_mic_mt6681_calibrate_powered(struct x9_mic_mt6681 *c,volatile uint32_t *afe)
{
    if(!c || !afe || ((uintptr_t)afe&3) || c->io.flags!=X9_MT6681_REQUIRED ||
       !c->touched || !c->audio_snapshot || c->poisoned || c->afe_snapshot ||
       !(c->status.flags&X9_MIC_HW_POWERED) || (c->status.flags&X9_MIC_HW_CALIBRATED))return EINVAL;
    int e=0;
    c->afe=afe;
    c->old_pad=afe_read(c,PAD_CFG);c->old_test=afe_read(c,TEST_CFG);
    if((c->old_pad&1) || (c->old_test&1))e=EBUSY;
    if(!e)c->afe_snapshot=true;
    if(!e) {
        c->status.stage=X9_MIC_HW_CALIBRATION;
        e=calibrate(c);
    }
    if(e) {
        /* Preserve decisive stage/errno/register; cleanup errors are separate. */
        c->status.last_errno=e;uint32_t r=c->status.last_register;
        (void)x9_mic_mt6681_unprepare(c);c->status.last_register=r;
    }
    return e;
}
