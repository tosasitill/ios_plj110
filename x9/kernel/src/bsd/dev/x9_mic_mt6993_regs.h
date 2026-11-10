/* SPDX-License-Identifier: GPL-2.0
 * Narrow register subset from OPPO mt6993-reg.h and mt6993-afe-pcm.c.
 * In particular VUL24..26/CM0..2 are NOT a continuation of the 0x30 stride.
 */
#ifndef X9_MIC_MT6993_REGS_H
#define X9_MIC_MT6993_REGS_H
enum { VUL1_BASE_MSB=0x4d90, VUL1_BASE=0x4d94, VUL1_CUR_MSB=0x4d98,
    VUL1_CUR=0x4d9c, VUL1_END_MSB=0x4da0, VUL1_END=0x4da4,
    VUL1_CON0=0x4db0, VUL1_MON0=0x4db4, MEMIF_CON0=0x4400,
    CONN020=0x2300, CONN021=0x2320 };
#define VUL1_ON (1u<<28)
#define VUL1_MON_BUSY ((0xfu<<20)|(1u<<19)|(1u<<17)|(1u<<16))
#define VUL1_MON_FULL (1u<<18)
#define VUL1_FORMAT_MASK ((0x1fu<<8)|(1u<<6)|(1u<<5)|(1u<<4)|(1u<<3)|(1u<<2)|3u)
/* Index defines GET_DMA.active_memifs bits. 15 DL followed by 17 VUL. */
static const unsigned x9_mic_memif_regs[]={
    0x4460,0x4490,0x44c0,0x44f0,0x4520,0x4550,0x4580,0x45b0,0x45e0,
    0x4610,0x4640,0x46a0,0x46d0,0x4700,0x4730,
    0x4d80,0x4db0,0x4de0,0x4e10,0x4e40,0x4e70,0x4ea0,0x4ed0,0x4f00,0x4f30,0x4f60,
    0x4fb8,0x4fd8,0x4ff8,0x51d8,0x51f8,0x5218
};
static const unsigned x9_mic_route_words[]={0,1,2,4,5,6,7}; /* word3 reserved */
#endif
