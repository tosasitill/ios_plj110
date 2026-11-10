/* MT6991-compatible speed_ver=1 I2C6 standard-mode configuration math.
 * OPPO i2c-mt65xx.c and clk-mt6993.c; own bounded implementation.
 * No MMIO, ownership, measured frequency/rise-time or Ready authorization.
 * C and C++14 (including own-code iOS) share exactly the same arithmetic. */
#ifndef X9_I2C6_TIMING_H
#define X9_I2C6_TIMING_H
#include <stdint.h>
#ifdef __cplusplus
#define X9_I2C6_MATH static constexpr
#else
#define X9_I2C6_MATH static inline
#endif
struct x9_i2c6_timing {
    uint32_t timing,low,ext,div,hs,sda,io,mis;
};
/* TOP I2C_WEST parents: 26M, UNIVPLL/20, MAINPLL/16, UNIVPLL/10.
 * Ceil ONCE across PCW postdiv AND fixed divider, never ceil then floor.
 * Parent/PLL stability and actual FENC are the native owner's obligation. */
X9_I2C6_MATH uint64_t x9_i2c6_source_hz(uint32_t parent,uint32_t pcw)
{
    if(parent>3)return 0;
    if(!parent)return pcw?0:26000000;
    uint32_t fixed=parent==1?20u:parent==2?16u:10u;
    uint64_t div=(uint64_t)16384*(1u<<((pcw>>24)&7))*fixed;
    uint64_t numerator=(uint64_t)(pcw&0x3fffff)*26000000;
    uint64_t hz=(numerator+div-1)/div;
    return hz>=1000000 && hz<=500000000?hz:0;
}
/* Return conservative encoded SCL rate UPPER bound, or zero for rejection.
 * Separate 1..8 sample / 1..64 step fields, duplicated five-bit dividers,
 * HS/SDA/MIS zero, open-drain IO3, EXT LS enable. Charge one count against
 * the complete LS period AND each phase: tHIGH>=4us / tLOW>=4.7us,
 * EXT>=5us. Verify rational inequalities, not a truncated Hz estimate.
 * These are the owned bounded profile, NOT arbitrary vendor timing or a
 * measured electrical qualification. Delay/START are separate actions. */
X9_I2C6_MATH uint32_t x9_i2c6_timing_rate(uint64_t hz,
                                         const struct x9_i2c6_timing *p)
{
    if(!p || hz<1000000 || hz>500000000 ||
       (p->timing&~0x073fu) || (p->low&~0x01ffu) || (p->ext&~0xff01u) ||
       (p->div&~0x1f1fu) || p->hs || p->sda || p->io!=3 || p->mis ||
       !(p->ext&1))return 0;
    uint32_t div=(p->div&31)+1;
    if(((p->div>>8)&31)+1!=div)return 0;
    uint32_t high=(((p->timing>>8)&7)+1)*((p->timing&63)+1);
    uint32_t low=(((p->low>>6)&7)+1)*((p->low&63)+1);
    uint32_t counts=high+low;
    if(high<2 || low<2)return 0;
    uint64_t period=(uint64_t)div*(counts-1);
    if(hz>period*100000 || hz-1<(uint64_t)div*counts*20000 ||
       (uint64_t)div*(high-1)*1000000<hz*4 ||
       (uint64_t)div*(low-1)*10000000<hz*47 ||
       (uint64_t)div*(p->ext>>8)*1000000<hz*5)return 0;
    return (uint32_t)((hz+period-1)/period);
}
X9_I2C6_MATH uint32_t x9_i2c6_timing_factor(uint32_t n)
{
    for(uint32_t sample=1;sample<=8;sample++)
        if(n%sample==0 && n/sample>=1 && n/sample<=64)
            return (sample<<8)|(n/sample);
    return 0;
}
/* Generate one complete checked <=98kHz profile BEFORE any register write.
 * Prefer 45% duty within [41%,49%); reject a factorable pair if phase/EXT
 * bounds fail, then continue to another candidate. No partial output on
 * failure. C callers must not alias output with unrelated owner state. */
X9_I2C6_MATH int x9_i2c6_timing_make(uint64_t hz,struct x9_i2c6_timing *out)
{
    if(!out || hz<1000000 || hz>500000000)return 0;
    for(uint32_t div=1;div<=32;div++) {
        uint64_t denominator=(uint64_t)div*1000000;
        uint32_t ext=(uint32_t)((hz*5+denominator-1)/denominator);
        if(ext>255)continue;
        denominator=(uint64_t)div*98000;
        uint32_t counts=(uint32_t)((hz+denominator-1)/denominator)+1;
        if(counts<4 || counts>1024)continue;
        uint32_t ideal=counts*45/100;
        for(uint32_t delta=0;delta<=counts/2;delta++) {
            for(uint32_t side=0;side<2;side++) {
                if(side && delta>ideal)continue;
                uint32_t high=side?ideal-delta:ideal+delta;
                if(high<2 || high>=counts-1 || high*1000<counts*410 ||
                   high*1000>=counts*490)continue;
                uint32_t h=x9_i2c6_timing_factor(high);
                uint32_t l=x9_i2c6_timing_factor(counts-high);
                if(!h || !l)continue;
                struct x9_i2c6_timing p={
                    (((h>>8)-1)<<8)|((h&255)-1),
                    (((l>>8)-1)<<6)|((l&255)-1),
                    (ext<<8)|1,((div-1)<<8)|(div-1),0,0,3,0
                };
                uint32_t rate=x9_i2c6_timing_rate(hz,&p);
                if(rate && rate<=98000){*out=p;return 1;}
            }
        }
    }
    return 0;
}
#undef X9_I2C6_MATH
#endif
