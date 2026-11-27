/* Stock-mode (x9_haptic=3) PCM event engine: pure per-sample logic, no
 * kernel dependencies, so host tests drive the exact production code.
 *
 * Mode 3 treats Apple PCM as an event cue and answers with recorded ColorOS
 * stock waveforms. The original detector fired one full-scale tap per onset
 * (arm after 1 ms below 512, HAL START intensity used verbatim), which threw
 * away every strength distinction iOS had rendered into the PCM -- a
 * selection tick and a heavy impact felt identical ("scattered" feel:
 * A1 in docs/2026-11-27_haptic-feel-stock-mode.md). With X9_HS_FEEL_GRADE
 * the onset peak is captured over a 2 ms window and mapped through a
 * square-root (perceptual) curve to an output peak; quiet events take the
 * gentle stepable_regulate stock wave, loud ones the standard tap.
 *
 * feel flags (boot arg x9_haptic_feel, default all on; 0 = legacy):
 *   GRADE  peak-graded wave/intensity instead of fixed full-scale tap
 *   HOLD   keep the session alive 200 ms after a stream drains (no EPIPE
 *          dead zone for the next HAL packet)
 *   ENV    sustained PCM (>30 ms hot) follows the PCM envelope on the
 *          looping stock buzz instead of firing a single tap
 *   QUEUE  0.25 ms re-arm + one queued onset during playback + a
 *          peak-fall tail guard against re-triggering on a pulse's tail
 */
#ifndef X9_HAPTIC_STOCK_H
#define X9_HAPTIC_STOCK_H
#include <stdint.h>

#define X9_HS_ONSET 512u          /* |pcm| at/above this counts as signal */
#define X9_HS_ARM_LEGACY 48u      /* legacy arm: 1 ms of quiet at 48 kHz */
#define X9_HS_ARM_FEEL 12u        /* feel arm: 0.25 ms of quiet */
#define X9_HS_PEAK_WINDOW 96u     /* 2 ms peak capture after an onset */
#define X9_HS_LIGHT_MAX_LEVEL 40u /* output peak < 40/127 -> light wave */
#define X9_HS_SUSTAIN 1440u       /* 30 ms continuously hot -> buzz */
#define X9_HS_BUZZ_OFF_QUIET 240u /* 5 ms below onset ends the buzz */
#define X9_HS_TAIL_WINDOW 960u    /* 20 ms tail-guard window after a fire */
#define X9_HS_HOLD_NS 200000000ULL /* 200 ms stream-end hold */

#define X9_HS_FEEL_GRADE 1u
#define X9_HS_FEEL_HOLD 2u
#define X9_HS_FEEL_ENV 4u
#define X9_HS_FEEL_QUEUE 8u
#define X9_HS_FEEL_ALL 15u

#define X9_HS_GRADE_STANDARD 0u
#define X9_HS_GRADE_LIGHT 1u

#define X9_HS_EV_FIRE 1u
#define X9_HS_EV_QUEUED 2u
#define X9_HS_EV_BUZZ_ON 4u
#define X9_HS_EV_BUZZ_OFF 8u

struct x9_haptic_stock {
    uint32_t feel;             /* X9_HS_FEEL_* config, set at init */
    uint32_t legacy_intensity; /* START intensity (0..1000), legacy fire */
    uint32_t quiet;            /* consecutive samples below X9_HS_ONSET */
    uint32_t in_peak;          /* max |sample| since init (diagnostics) */
    uint32_t hot;              /* span of the current sustained event in
                                * samples; zero-crossing dips under 1 ms do
                                * not break it (iOS continuous events are
                                * enveloped sines, below onset near every
                                * zero crossing for tens of microseconds) */
    uint32_t cold;             /* consecutive samples below onset (buzz) */
    uint32_t env;              /* buzz envelope peak tracker, 0..32767 */
    uint32_t cap_peak;         /* peak captured in the current window */
    uint32_t cap_left;         /* samples left in the capture window */
    uint32_t prev_peak;        /* PCM peak of the last fired event */
    uint32_t since_fire;       /* samples since the last fire (saturating) */
    uint32_t fire_level;       /* EV_FIRE: desired output peak, 0..127 */
    uint32_t fire_grade;       /* EV_FIRE: X9_HS_GRADE_* */
    uint32_t buzz_level;       /* while buzzing: envelope, 0..1000 */
    uint32_t dropped;          /* onsets consumed while busy, no queue */
    uint32_t tail_dropped;     /* onsets suppressed by the tail guard */
    uint32_t pending_level;    /* queued onset: output peak, 0..127 */
    uint32_t pending_grade;    /* queued onset: X9_HS_GRADE_* */
    uint64_t hold_until;       /* stream-end hold deadline (ns) */
    uint8_t armed;             /* detector may accept an onset */
    uint8_t capturing;         /* peak window in flight */
    uint8_t buzzing;           /* envelope follow active */
    uint8_t pending;           /* one onset queued behind playback */
    uint8_t hold;              /* stream-end hold active */
};

static inline uint32_t x9_hs_isqrt(uint32_t v)
{
    uint32_t r=0,bit=1u<<30;
    while(bit>v)bit>>=2;
    while(bit) {
        if(v>=r+bit){v-=r+bit;r=(r>>1)+bit;}
        else r>>=1;
        bit>>=2;
    }
    return r;
}
/* Perceptual strength curve: PCM peak (0..32767) -> desired RTP output
 * peak (0..127) with square-root spacing, so the quiet half of the iOS
 * intensity range stays distinguishable instead of being crushed by a
 * linear map (and full-scale stays full-scale). */
static inline uint32_t x9_hs_level(uint32_t peak)
{
    if(peak>32767)peak=32767;
    return x9_hs_isqrt((peak*16129u)/32767u); /* 16129 = 127^2 */
}
static inline void x9_haptic_stock_init(struct x9_haptic_stock *e,
    uint32_t feel,uint32_t legacy_intensity)
{
    *e=(struct x9_haptic_stock){.feel=feel,.legacy_intensity=legacy_intensity};
}
/* Decide one captured event. Returns the event bits for a fire/queue;
 * tail-guard and busy handling live here so feed() stays linear. */
static inline uint32_t x9_hs_decide(struct x9_haptic_stock *e,int busy)
{
    uint32_t level,grade;
    if(e->feel&X9_HS_FEEL_GRADE) {
        level=x9_hs_level(e->cap_peak);
        grade=level<X9_HS_LIGHT_MAX_LEVEL?X9_HS_GRADE_LIGHT:X9_HS_GRADE_STANDARD;
    } else {
        level=(e->legacy_intensity*127u)/1000u;
        grade=X9_HS_GRADE_STANDARD;
    }
    /* Tail guard (QUEUE): a candidate whose peak fell below a quarter of
     * the previous event's peak within 20 ms of that fire is the same
     * pulse's decaying tail (e.g. the last half-cycle of a FullTap), not
     * a new event. Genuine double taps have comparable peaks, and later
     * quiet events pass on the time clause alone. */
    if((e->feel&X9_HS_FEEL_QUEUE) && e->prev_peak &&
       e->cap_peak*4u<e->prev_peak && e->since_fire<X9_HS_TAIL_WINDOW) {
        ++e->tail_dropped;
        return 0;
    }
    e->prev_peak=e->cap_peak;e->since_fire=0;
    if(busy) {
        if((e->feel&X9_HS_FEEL_QUEUE) && !e->pending) {
            e->pending=1;e->pending_level=level;e->pending_grade=grade;
            return X9_HS_EV_QUEUED;
        }
        ++e->dropped;
        return 0;
    }
    e->fire_level=level;e->fire_grade=grade;
    return X9_HS_EV_FIRE;
}
/* Feed one 48 kHz PCM sample. busy != 0 while a stock scene is playing or
 * draining. Returns X9_HS_EV_* bits; on EV_FIRE the caller starts the wave
 * selected by fire_grade scaled to fire_level. */
static inline uint32_t x9_haptic_stock_feed(struct x9_haptic_stock *e,
    int16_t sample,int busy)
{
    uint32_t a=sample<0?-(int32_t)sample:(int32_t)sample;
    uint32_t arm_need=(e->feel&X9_HS_FEEL_QUEUE)?X9_HS_ARM_FEEL:X9_HS_ARM_LEGACY;
    if(a>e->in_peak)e->in_peak=a;
    if(e->since_fire<0xffffffu)++e->since_fire;
    if(e->buzzing) {
        /* Envelope follow: instant attack, ~5 ms release peak tracker.
         * The tap detector stays suppressed; the buzz is the event. */
        if(a>=X9_HS_ONSET) {
            e->cold=0;
            if(a>e->env)e->env=a;
            else if(e->env)e->env-=((e->env>>8)+1u);
        } else {
            if(e->env)e->env-=((e->env>>8)+1u);
            if(++e->cold>=X9_HS_BUZZ_OFF_QUIET) {
                e->buzzing=0;e->env=0;e->buzz_level=0;e->hot=0;e->cold=0;
                return X9_HS_EV_BUZZ_OFF;
            }
        }
        e->buzz_level=(e->env*1000u)/32767u;
        return 0;
    }
    if(a<X9_HS_ONSET) {
        if(e->quiet<arm_need)++e->quiet;
        if(e->quiet>=arm_need)e->armed=1;
        if(e->quiet>=X9_HS_ARM_LEGACY)e->hot=0; /* 1 ms true silence ends it */
    } else {
        e->quiet=0;++e->hot;
        if(!e->capturing && e->armed) {
            e->armed=0;e->capturing=1;e->cap_peak=a;
            e->cap_left=(e->feel&X9_HS_FEEL_GRADE)?X9_HS_PEAK_WINDOW:0;
            if(!e->cap_left){e->capturing=0;return x9_hs_decide(e,busy);}
        }
        if(e->capturing && a>e->cap_peak)e->cap_peak=a;
        /* Sustained signal: hand the event to the envelope path. Any tap
         * captured at onset has already fired (window is 2 ms), so the
         * buzz adds a body behind the attack instead of replacing it. */
        if((e->feel&X9_HS_FEEL_ENV) && e->hot>=X9_HS_SUSTAIN) {
            e->buzzing=1;e->env=e->cap_peak>e->env?e->cap_peak:e->env;
            if(a>e->env)e->env=a;
            e->cold=0;e->buzz_level=(e->env*1000u)/32767u;
            return X9_HS_EV_BUZZ_ON;
        }
    }
    if(e->capturing && !--e->cap_left) {
        e->capturing=0;
        return x9_hs_decide(e,busy);
    }
    return 0;
}
/* Take the queued onset, if any. The caller plays it immediately (the
 * previous scene just finished filling, so it appends gaplessly). */
static inline int x9_haptic_stock_take_pending(struct x9_haptic_stock *e,
    uint32_t *level,uint32_t *grade)
{
    if(!e->pending)return 0;
    e->pending=0;*level=e->pending_level;*grade=e->pending_grade;
    return 1;
}
/* Stream-end hold (HOLD): after a stream drains, keep the session alive
 * until the deadline instead of retiring the generation. */
static inline void x9_hs_hold_begin(struct x9_haptic_stock *e,uint64_t now_ns)
{e->hold=1;e->hold_until=now_ns+X9_HS_HOLD_NS;}
static inline void x9_hs_hold_cancel(struct x9_haptic_stock *e)
{e->hold=0;e->hold_until=0;}
static inline int x9_hs_hold_expired(const struct x9_haptic_stock *e,uint64_t now_ns)
{return e->hold && now_ns>=e->hold_until;}
#endif
