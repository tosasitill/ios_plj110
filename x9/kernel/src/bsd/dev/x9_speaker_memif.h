/* Production stereo DL24 / DL_24CH -> OUT4 layouts. Own-code descriptors
 * from OPPO mt6993-reg.h, mt6993-afe-pcm.c and mt6993-dai-i2s.c.
 * Selecting a layout is NOT firmware routing, DMA/IRQ ownership or Ready. */
#ifndef X9_SPEAKER_MEMIF_H
#define X9_SPEAKER_MEMIF_H
#include "x9_speaker_dl24.h"
#define X9_SPEAKER_DL_24CH_MEMIF 14u
#define X9_SPEAKER_MEMIF_ADDR_MSB_MASK 0x1ffu
#define X9_SPEAKER_MEMIF_ADDR_LOW_MASK 0xfffffff0u
struct x9_speaker_memif_layout {
    uint32_t id,base_msb,base,cur_msb,cur,end_msb,end,con;
    uint32_t enable,clear,inactive,format_mask,format_value;
    uint32_t route[2],source[2];
};
/* No defaults or name-based substitution. Output unchanged on rejection.
 * The provider must independently prove the same explicit C memif id and
 * its real firmware task/IRQ/DMA route. DL24 != DL_24CH != scene32. */
#ifdef __cplusplus
static constexpr int
#else
static inline int
#endif
x9_speaker_memif_layout(uint32_t id,struct x9_speaker_memif_layout *out)
{
    if(!out || (id!=X9_SPEAKER_DL24_MEMIF && id!=X9_SPEAKER_DL_24CH_MEMIF))return 0;
#ifdef __cplusplus
    struct x9_speaker_memif_layout m={};
#else
    struct x9_speaker_memif_layout m={0};
#endif
    m.id=id;
    m.format_mask=(3u<<20)|(3u<<16)|(31u<<8)|(3u<<5)|(1u<<3)|(1u<<2)|3u;
    m.format_value=(3u<<16)|(10u<<8)|(1u<<3); /* MAX64, FS48k, normal S16 */
    if(id==X9_SPEAKER_DL24_MEMIF) {
        m.base_msb=0x46b0;m.base=0x46b4;m.cur_msb=0x46b8;m.cur=0x46bc;
        m.end_msb=0x46c0;m.end=0x46c4;m.con=X9_SPEAKER_DL24_CON0;
        m.enable=1u<<28;m.clear=X9_SPEAKER_DL24_CLEAR_MASK;
        m.format_mask|=m.enable|(1u<<4); /* MONO ordinary RW, stereo=0 */
        m.format_value|=(3u<<20)|(3u<<5); /* source trigger: MIN64/PBUF256 */
        m.route[0]=0x2f08;m.route[1]=0x2f28;
        m.source[0]=1u<<16;m.source[1]=1u<<17; /* I_DL24 80/81, bank2 */
    } else {
        m.base_msb=0x4620;m.base=0x4624;m.cur_msb=0x4628;m.cur=0x462c;
        m.end_msb=0x4630;m.end=0x4634;m.con=0x4640;
        m.enable=1u<<31;
        m.clear=(1u<<18)|(1u<<7)|(1u<<4); /* HANG_CLR is ACTION, not MONO */
        m.format_mask|=m.enable|(63u<<24);
        m.format_value|=(2u<<24)|(1u<<20)|(1u<<5); /* direct CH_NUM2, MIN16/PBUF64 */
        m.route[0]=0x2f04;m.route[1]=0x2f24;
        m.source[0]=1u<<22;m.source[1]=1u<<23; /* I_DL_24CH 54/55, bank1 */
    }
    /* Keep foreign domain/one-heart/auto-mute and all actions inactive;
     * none is silently cleared or included in ordinary RW replay. */
    m.inactive=m.enable|(3u<<22)|(7u<<13)|(1u<<19)|m.clear;
    *out=m;return 1;
}
#endif
