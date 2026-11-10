/* Integer-only 48kHz Actuator -> 12kHz signed RTP conversion.
 * No allocation, float, packet-dependent carrier reset or hidden ID guessing.
 * Retuning preserves the time envelope while translating D22's carrier to
 * the actual LRA. Raw mode is available for already tuned custom waveforms.
 */
#ifndef X9_HAPTIC_CONVERT_H
#define X9_HAPTIC_CONVERT_H
#include "x9_haptic.h"
struct x9_haptic_converter {
    uint32_t phase, step, conversion, intensity, sharpness, decimator;
    int32_t dc, envelope, lowpass, sum;
};
/* Q15 sine approximation on a full unsigned 32-bit turn. Bhaskara's
 * approximation, bounded 64-bit intermediates, exact zero at half turns. */
static inline int32_t x9_haptic_sine(uint32_t phase)
{
    uint32_t x=(phase&0x7fffffff)>>16;
    int64_t a=(int64_t)x*(32768-x);
    int32_t y=(int32_t)((16*a*32767)/(5LL*32768*32768-4*a));
    return phase&0x80000000?-y:y;
}
static inline void x9_haptic_converter_init(struct x9_haptic_converter *c,
    uint32_t f0_millihz,uint32_t conversion,uint32_t intensity,uint32_t sharpness)
{
    *c=(struct x9_haptic_converter){.step=(uint32_t)(((uint64_t)f0_millihz<<32)/
        (X9_HAPTIC_RTP_RATE*1000ULL)),.conversion=conversion,
        .intensity=intensity,.sharpness=sharpness};
}
static inline int x9_haptic_convert_sample(struct x9_haptic_converter *c,int16_t pcm,int8_t *out)
{
    /* Remove DC before rectification. Attack ~0.25ms, release ~2ms;
     * a zero input eventually yields exactly zero, not a vibrating noise floor. */
    int32_t dc_delta=(int32_t)pcm-c->dc;
    c->dc+=dc_delta/512;
    if(!pcm && c->dc && c->dc<512 && c->dc> -512)c->dc+=c->dc>0?-1:1;
    int32_t input=(int32_t)pcm-c->dc;
    int32_t magnitude=input<0?-input:input;
    int32_t delta=magnitude-c->envelope;
    c->envelope+=delta/(delta>0?12:96);
    if(magnitude<16 && c->envelope<96)c->envelope=0;
    /* Two-state lowpass + four-sample average is the raw decimator, not a
     * sufficient anti-alias filter for arbitrary audio. Actuator content is
     * low-frequency haptic PCM; this is not a full-band audio transport. */
    c->lowpass+=(input-c->lowpass)/4;c->sum+=c->lowpass;
    if(++c->decimator<4)return 0;
    c->decimator=0;int32_t value=c->sum/4;c->sum=0;
    if(c->conversion==X9_HAPTIC_RETUNE) {
        /* Approximate sine peak from rectified sine mean, with a limiter.
         * Sharpness adds a bounded 3rd harmonic, not a change in pattern time. */
        int32_t env=c->envelope*157/100;if(env>32767)env=32767;
        int32_t sine=x9_haptic_sine(c->phase);
        int32_t harmonic=x9_haptic_sine(c->phase*3);
        sine=(sine*(4000-(int32_t)c->sharpness)+harmonic*(int32_t)c->sharpness)/4000;
        value=(int32_t)((int64_t)env*sine/32768);
    }
    c->phase+=c->step;
    value=(int32_t)((int64_t)value*c->intensity/1000);
    value=value*96/32768; /* signed8 peak <=96, additional hardware gain cap */
    if(value>96)value=96;if(value< -96)value= -96;
    *out=(int8_t)value;return 1;
}
/* Scene synthesis shares the native converter's continuous phase. This
 * adapter is for explicit scene clients only; no UIKit/Apple API swizzling. */
struct x9_haptic_pulse {uint16_t at_ms,length_ms,amplitude,sharpness;};
struct x9_haptic_pattern {uint32_t duration_ms,count;struct x9_haptic_pulse pulse[6];};
static inline int x9_haptic_scene_pattern(uint32_t scene,uint32_t duration,
    struct x9_haptic_pattern *p)
{
    *p=(struct x9_haptic_pattern){0};
    switch(scene) {
    case X9_HAPTIC_SELECTION:*p=(struct x9_haptic_pattern){12,1,{{0,12,420,650}}};break;
    case X9_HAPTIC_LIGHT:*p=(struct x9_haptic_pattern){18,1,{{0,18,500,600}}};break;
    case X9_HAPTIC_MEDIUM:*p=(struct x9_haptic_pattern){26,1,{{0,26,700,650}}};break;
    case X9_HAPTIC_HEAVY:*p=(struct x9_haptic_pattern){36,1,{{0,36,950,750}}};break;
    case X9_HAPTIC_SOFT:*p=(struct x9_haptic_pattern){32,1,{{0,32,450,0}}};break;
    case X9_HAPTIC_RIGID:*p=(struct x9_haptic_pattern){14,1,{{0,14,850,1000}}};break;
    case X9_HAPTIC_SUCCESS:*p=(struct x9_haptic_pattern){140,2,{{0,22,600,550},{85,36,850,650}}};break;
    case X9_HAPTIC_WARNING:*p=(struct x9_haptic_pattern){170,2,{{0,32,800,600},{100,32,800,600}}};break;
    case X9_HAPTIC_ERROR:*p=(struct x9_haptic_pattern){270,3,{{0,35,900,800},{95,35,650,800},{190,50,900,800}}};break;
    case X9_HAPTIC_KEYBOARD:*p=(struct x9_haptic_pattern){10,1,{{0,10,350,750}}};break;
    case X9_HAPTIC_HOME:*p=(struct x9_haptic_pattern){20,1,{{0,20,750,850}}};break;
    case X9_HAPTIC_LONG_PRESS:*p=(struct x9_haptic_pattern){30,1,{{0,30,700,400}}};break;
    case X9_HAPTIC_NOTIFICATION:*p=(struct x9_haptic_pattern){220,2,{{0,32,800,650},{135,45,750,650}}};break;
    case X9_HAPTIC_RINGTONE:*p=(struct x9_haptic_pattern){800,4,{{0,90,850,500},{130,90,850,500},{400,90,850,500},{530,90,850,500}}};break;
    case X9_HAPTIC_ALARM:*p=(struct x9_haptic_pattern){800,3,{{0,140,850,400},{260,140,850,400},{520,140,850,400}}};break;
    case X9_HAPTIC_ACCESSIBILITY:*p=(struct x9_haptic_pattern){18,1,{{0,18,550,750}}};break;
    case X9_HAPTIC_GAME_TRANSIENT:*p=(struct x9_haptic_pattern){25,1,{{0,25,800,650}}};break;
    case X9_HAPTIC_GAME_CONTINUOUS:
        if(!duration || duration>1000)return -1;
        *p=(struct x9_haptic_pattern){duration,1,{{0,(uint16_t)duration,700,350}}};break;
    default:return -1;
    }
    return 0;
}
static inline int8_t x9_haptic_scene_sample(const struct x9_haptic_pattern *p,
    uint32_t frame,uint32_t phase,uint32_t intensity,uint32_t sharpness)
{
    int32_t value=0;
    for(uint32_t i=0;i<p->count;i++) {
        const struct x9_haptic_pulse *q=&p->pulse[i];
        uint32_t first=q->at_ms*12u,length=q->length_ms*12u;
        if(frame<first || frame-first>=length)continue;
        uint32_t n=frame-first,edge=q->sharpness<300?72:24;
        uint32_t ramp=n<edge?n:edge;
        if(length-1-n<ramp)ramp=length-1-n;
        int32_t amp=(int32_t)((uint64_t)q->amplitude*intensity*ramp/(1000u*edge));
        uint32_t sh=q->sharpness*sharpness/1000;
        int32_t sine=(x9_haptic_sine(phase)*(4000-(int32_t)sh)+
            x9_haptic_sine(phase*3)*(int32_t)sh)/4000;
        value+=(int32_t)((int64_t)sine*amp*96/(32768LL*1000));
    }
    if(value>96)value=96;if(value< -96)value= -96;return (int8_t)value;
}
#endif
