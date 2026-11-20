/* Integer-only 48kHz Actuator -> 12kHz signed RTP, OPPO RichTap stream
 * style (haptic_hv.c aw_stream_mode): the PCM is the finished drive
 * waveform and goes to RTP unchanged; strength is the chip gain/boost.
 *
 * iOS 20H392 D221 haptic engine (Library/Audio/Tunings/D221/Haptics):
 * transients are pre-shaped full-scale drive pulses, MicroTap ~1/2 cycle
 * (one-sided push), MiniTap ~1 cycle, FullTap ~2 cycles, at 80..300 Hz
 * selected by sharpness; continuous events are enveloped sines. Logs
 * 327..330 rebuilt everything as an envelope-retuned 173 Hz sine with
 * 6 ms hold + kick/brake: a 7 ms MicroTap became a ~30 ms buzz, sharpness
 * was discarded and x6 clipped every tap (log 330 in_peak 17695..32509)
 * -- "late, loose, wrong for the scene". Now linear 32767 -> 127.
 * No DC removal: MicroTaps are intentionally one-sided.
 */
#ifndef X9_HAPTIC_CONVERT_H
#define X9_HAPTIC_CONVERT_H
#include "x9_haptic.h"
struct x9_haptic_converter {
    uint32_t phase, step, conversion, intensity, sharpness, decimator;
    int32_t lowpass, sum, in_peak;
    /* Stock mode (x9_haptic=3) onset detector: arm after 1 ms of silence. */
    uint32_t stock_quiet, stock_armed;
};
/* Q15 sine approximation on a full unsigned 32-bit turn (scene synthesis). */
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
    {int32_t a=pcm<0?-(int32_t)pcm:pcm;if(a>c->in_peak)c->in_peak=a;}
    /* One-pole lowpass (~2 kHz) + four-sample average: content is <=300 Hz
     * drive, group delay ~0.1 ms. */
    c->lowpass+=((int32_t)pcm-c->lowpass)/4;c->sum+=c->lowpass;
    if(++c->decimator<4)return 0;
    c->decimator=0;int32_t value=c->sum/4;c->sum=0;
    c->phase+=c->step; /* scene synthesis continues the same phase */
    value=(int32_t)((int64_t)value*c->intensity/1000);
    int32_t limit=c->conversion==X9_HAPTIC_RETUNE?127:96; /* raw: legacy <=96 */
    value=(int32_t)(((int64_t)value*limit+(value<0?-16384:16384))/32768);
    if(value>limit)value=limit;if(value< -limit)value= -limit;
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
