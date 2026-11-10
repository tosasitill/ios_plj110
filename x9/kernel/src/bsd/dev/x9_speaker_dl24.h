/* Independent DL24/OUT4 descriptors from OPPO mt6993-reg.h,
 * mt6993-afe-pcm.c and mt6993-dai-i2s.c. No DMA address or pad writes.
 * Initializers are {off,mask,value,saved,expected}, matching both journals. */
#ifndef X9_SPEAKER_DL24_H
#define X9_SPEAKER_DL24_H
#include <stdint.h>
#define X9_SPEAKER_DL24_CON0 0x46d0u
/* SW_CLEAR_MEM_RAN_OUT/SW_CLEAR_BUF_EMPTY are actions, not ordinary RW.
 * Never replay an inherited clear bit via an unrelated format RMW. */
#define X9_SPEAKER_DL24_CLEAR_MASK ((1u<<18)|(1u<<7))
#define X9_SPEAKER_DL24_INACTIVE_MASK \
    ((1u<<28)|(3u<<22)|(7u<<13)|X9_SPEAKER_DL24_CLEAR_MASK)
#define X9_SPEAKER_OUT4_COWORK_DISABLED \
    {0x1690,15u<<8,0,0,0}
/* Normal mode, stereo S16, FS10; PBUF/MIN/MAX encodings0.
 * These encodings do not establish SRAM ownership or physical latency. */
#define X9_SPEAKER_DL24_DISABLED_FORMAT \
    {X9_SPEAKER_DL24_CON0,(1u<<28)|(3u<<20)|(3u<<16)|(31u<<8)|(3u<<5)|(1u<<4)|(1u<<3)|(1u<<2)|3u, \
        (10u<<8)|(1u<<3),0,0}
/* AFE_CONN116_2/117_2, I_DL24_CH1=80-64 / CH2=81-64. */
#define X9_SPEAKER_DL24_DISABLED_ROUTES \
    {0x2f08,UINT32_MAX,1u<<16,0,0}, \
    {0x2f28,UINT32_MAX,1u<<17,0,0}
#endif
