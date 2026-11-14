/* ETDM IN4 profile: the I2S4 pad-top clock master paired with OUT4.
 * OPPO mt6993-dai-i2s.c: I2SOUT4 share_i2s_id defaults to MT6993_DAI_I2S_IN4
 * (no "i2sout4-share" in the live DT), so mtk_dai_i2s_config(OUT4) also runs
 * the IN branch for IN4, the I2SOUT4 route powers I2SIN4_GPIO (en_event:
 * ch/ip_mode/sync/slave) and I2SIN4_EN. GPIO108/109 are I2SIN4_BCK/LRCK: with
 * IN4 off the PAs never see BCK/WS (log 285: PA34 CLKS timeout).
 * Initializers are {off,mask,value,saved,expected}. No IN4 EN bit is set. */
#ifndef X9_SPEAKER_IN4_H
#define X9_SPEAKER_IN4_H
#define X9_SPEAKER_IN4_CON0 0x13c0u
#define X9_SPEAKER_IN4_MON 0x13e8u
/* CON0: EN0 SYNC1 SLAVE5 FMT6-8 BIT_LENGTH11-15 WORD_LENGTH16-20 CH_NUM23-27
 * RELATCH_DOMAIN28-30. DT etdm-in-sync IN4=1, slave 0, ch 2 -> 1, I2S fmt 0,
 * S16 valid 15 / slot 31, relatch domain APLL=1. */
#define X9_SPEAKER_IN4_DISABLED_CONFIG \
    {X9_SPEAKER_IN4_CON0,1u|2u|32u|(7u<<6)|(31u<<11)|(31u<<16)|(31u<<23)|(7u<<28), \
        2u|(15u<<11)|(31u<<16)|(1u<<23)|(1u<<28),0,0}, \
    /* CON1: INITIAL_COUNT=5, INITIAL_POINT=3 (pad top, master, no lpbk), LRCK_RESET=1 */ \
    {0x13c4,31u|(31u<<5)|(1u<<15),5u|(3u<<5)|(1u<<15),0,0}, \
    /* CON2: CLOCK_SOURCE_SEL=APLL(1), CK_EN_SEL_AUTO=1, MULTI_IP_MODE=DT 0 */ \
    {0x13c8,(7u<<10)|(1u<<14)|(1u<<31),(1u<<10)|(1u<<14),0,0}, \
    /* CON3: FS_TIMING_SEL=ETDM_RATE_48K(5) */ \
    {0x13cc,31u<<26,5u<<26,0,0}, \
    /* CON4: RELATCH_1X_EN_SEL=ETDM_CONN_48K(10) */ \
    {0x13d0,31u<<20,10u<<20,0,0}, \
    /* CON8: USE_AFIFO=0, AFIFO_MODE=0 */ \
    {0x13e0,(1u<<8)|31u,0,0,0}, \
    /* CON9: ALMOST_END_CH=0, ALMOST_END_BIT=0, OUT2LATCH_TIME=6 (pad top) */ \
    {0x13e4,31u|(31u<<5)|(31u<<10),6u<<10,0,0}
/* ETDM_4_7_COWORK_CON0: OUT4_SLAVE_SEL[11:8]=ETDMIN4_MASTER(0),
 * IN4_SLAVE_SEL[27:24]=ETDMOUT4_MASTER(8). */
#define X9_SPEAKER_I2S4_COWORK \
    {0x1690,(15u<<8)|(15u<<24),8u<<24,0,0}
/* AUD_TOP_CFG_VLP_RG: RG_I2S4_PAD_TOP_CK_EN5=1, OUT/IN BCK_NEG_EG_LATCH20/21=0. */
#define X9_SPEAKER_I2S4_PAD_TOP \
    {0x0098,(1u<<5)|(1u<<20)|(1u<<21),1u<<5,0,0}
#endif
