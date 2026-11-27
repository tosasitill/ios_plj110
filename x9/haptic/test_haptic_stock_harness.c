/* Host harness for the production stock-mode event engine
 * (x9/kernel/src/bsd/dev/x9_haptic_stock.h) and the shipped stock waves.
 * Drives synthetic 48 kHz PCM through the exact kernel logic and asserts
 * behavior: which wave fires, at what output level, and what is dropped,
 * queued or suppressed. Playback busy is emulated at wave duration. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "x9_haptic_stock.h"
#include "x9_haptic_stock_waves.h"

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if(!(cond)) { ++failures; \
    printf("FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); } } while(0)

#define MAXS (48000*4)
static int16_t pcm[MAXS];
static uint32_t npcm;
static void gen_clear(void) { npcm=0; }
static void gen_silence(uint32_t ms)
{ uint32_t n=ms*48u; while(n-- && npcm<MAXS) pcm[npcm++]=0; }
static void gen_burst(uint32_t peak, uint32_t ms)
{
    uint32_t n=ms*48u;
    for(uint32_t i=0;i<n && npcm<MAXS;i++) {
        double t=(double)i/48000.0;
        double env=1.0;
        if(i<24) env=(double)i/24.0;                 /* 0.5 ms attack */
        else if(n-i<=24) env=(double)(n-i)/24.0;     /* 0.5 ms release */
        double v=(double)peak*sin(2.0*3.141592653589793*173.0*t)*env;
        if(v>32767.0)v=32767.0; if(v< -32767.0)v= -32767.0;
        pcm[npcm++]=(int16_t)lround(v);
    }
}

struct fire { uint32_t at, grade, level; };
static struct fire fires[128];
static int nfires, nbuzz_on, nbuzz_off;
static uint32_t buzz_on_at, buzz_off_at, buzz_level_mid1, buzz_level_mid2;
static int emulate_busy; /* 1: a fire makes the player busy for the wave */

static uint32_t wave_samples(uint32_t grade)
{ return grade==X9_HS_GRADE_LIGHT ? x9_stock_wave_tap_light.len : x9_stock_wave_tap.len; }

static void run_reset(void)
{ nfires=nbuzz_on=nbuzz_off=0; buzz_on_at=buzz_off_at=0; buzz_level_mid1=buzz_level_mid2=0; }

/* Feed the generated stream. probe1/probe2: absolute sample indices at
 * which e->buzz_level is sampled (0 = unused). */
static void run(struct x9_haptic_stock *e, uint32_t probe1, uint32_t probe2)
{
    uint32_t busy_until=0;
    for(uint32_t i=0;i<npcm;i++) {
        if(emulate_busy && i>=busy_until && e->pending) {
            uint32_t level, grade;
            if(x9_haptic_stock_take_pending(e, &level, &grade)) {
                if(nfires<128) fires[nfires]=(struct fire){i, grade, level};
                ++nfires;
                busy_until=i+wave_samples(grade)*4u;
            }
        }
        int busy=emulate_busy && i<busy_until;
        uint32_t ev=x9_haptic_stock_feed(e, pcm[i], busy);
        if(ev&X9_HS_EV_FIRE) {
            if(nfires<128) fires[nfires]=(struct fire){i, e->fire_grade, e->fire_level};
            ++nfires;
            if(emulate_busy) busy_until=i+wave_samples(e->fire_grade)*4u;
        }
        if(ev&X9_HS_EV_BUZZ_ON) { ++nbuzz_on; buzz_on_at=i; }
        if(ev&X9_HS_EV_BUZZ_OFF) { ++nbuzz_off; buzz_off_at=i; }
        if(i==probe1) buzz_level_mid1=e->buzz_level;
        if(i==probe2) buzz_level_mid2=e->buzz_level;
    }
}

static void test_grade_light(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_GRADE, 1000);
    gen_clear(); gen_silence(20); gen_burst(2048, 10); gen_silence(40);
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==1, "fires=%d want 1", nfires);
    if(nfires==1) {
        CHECK(fires[0].grade==X9_HS_GRADE_LIGHT, "grade=%u want LIGHT", fires[0].grade);
        CHECK(fires[0].level>=24 && fires[0].level<=38,
            "level=%u want ~31 (sqrt curve of 2048)", fires[0].level);
    }
    CHECK(nbuzz_on==0, "buzz_on=%d want 0", nbuzz_on);
}
static void test_grade_medium(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_GRADE, 1000);
    gen_clear(); gen_silence(20); gen_burst(8192, 12); gen_silence(40);
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==1, "fires=%d want 1", nfires);
    if(nfires==1) {
        CHECK(fires[0].grade==X9_HS_GRADE_STANDARD, "grade=%u want STANDARD", fires[0].grade);
        CHECK(fires[0].level>=56 && fires[0].level<=70,
            "level=%u want ~63 (sqrt curve of 8192)", fires[0].level);
    }
}
static void test_grade_heavy(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_GRADE, 1000);
    gen_clear(); gen_silence(20); gen_burst(32767, 15); gen_silence(40);
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==1, "fires=%d want 1", nfires);
    if(nfires==1) {
        CHECK(fires[0].grade==X9_HS_GRADE_STANDARD, "grade=%u want STANDARD", fires[0].grade);
        CHECK(fires[0].level>=120, "level=%u want ~127", fires[0].level);
    }
}
static void test_grade_monotonic(void)
{
    static const uint32_t peaks[3]={1024, 4096, 16384};
    uint32_t levels[3]={0,0,0};
    for(int k=0;k<3;k++) {
        struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_GRADE, 1000);
        gen_clear(); gen_silence(20); gen_burst(peaks[k], 10); gen_silence(40);
        emulate_busy=1; run_reset(); run(&e, 0, 0);
        CHECK(nfires==1, "peak=%u fires=%d want 1", peaks[k], nfires);
        if(nfires==1) levels[k]=fires[0].level;
    }
    CHECK(levels[0]<levels[1] && levels[1]<levels[2],
        "levels %u<%u<%u want strictly increasing", levels[0], levels[1], levels[2]);
    CHECK(levels[0]<X9_HS_LIGHT_MAX_LEVEL, "peak 1024 level=%u want LIGHT band", levels[0]);
}
static void test_legacy_fixed_full_scale(void)
{
    static const uint32_t peaks[2]={2048, 32767};
    for(int k=0;k<2;k++) {
        struct x9_haptic_stock e; x9_haptic_stock_init(&e, 0, 1000);
        gen_clear(); gen_silence(20); gen_burst(peaks[k], 10); gen_silence(40);
        emulate_busy=1; run_reset(); run(&e, 0, 0);
        CHECK(nfires==1, "feel=0 peak=%u fires=%d want 1", peaks[k], nfires);
        if(nfires==1) {
            CHECK(fires[0].grade==X9_HS_GRADE_STANDARD, "feel=0 grade=%u want STANDARD", fires[0].grade);
            CHECK(fires[0].level==127, "feel=0 level=%u want 127 (START intensity verbatim)", fires[0].level);
        }
    }
}
static void test_legacy_arm_1ms(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, 0, 1000);
    gen_clear(); gen_silence(20); gen_burst(20000, 8);
    /* 0.5 ms of quiet: below the legacy 1 ms arm, so no second onset. */
    for(int i=0;i<24;i++) pcm[npcm++]=0;
    gen_burst(20000, 8); gen_silence(40);
    emulate_busy=0; run_reset(); run(&e, 0, 0);
    CHECK(nfires==1, "feel=0 fires=%d want 1 (0.5 ms gap must not re-arm)", nfires);
}

static void test_hold(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_HOLD, 1000);
    CHECK(!x9_hs_hold_expired(&e, 1000000000ULL), "expired with no hold");
    x9_hs_hold_begin(&e, 1000000000ULL);
    CHECK(e.hold==1, "hold not set");
    CHECK(!x9_hs_hold_expired(&e, 1000000000ULL+X9_HS_HOLD_NS-1),
        "expired before 200 ms");
    CHECK(x9_hs_hold_expired(&e, 1000000000ULL+X9_HS_HOLD_NS),
        "not expired at 200 ms");
    x9_hs_hold_cancel(&e);
    CHECK(!x9_hs_hold_expired(&e, 2000000000ULL), "expired after cancel");
    x9_hs_hold_begin(&e, 5000ULL);
    CHECK(x9_hs_hold_expired(&e, 5000ULL+X9_HS_HOLD_NS),
        "re-begin after cancel broken");
}

static void test_envelope_continuous(void)
{
    struct x9_haptic_stock e;
    x9_haptic_stock_init(&e, X9_HS_FEEL_GRADE|X9_HS_FEEL_ENV, 1000);
    gen_clear(); gen_silence(20); gen_burst(16000, 250); gen_silence(60);
    emulate_busy=1; run_reset();
    uint32_t probe=960+4800; /* 100 ms into the sustained segment */
    run(&e, probe, 0);
    CHECK(nfires==1, "fires=%d want 1 (the onset tap only)", nfires);
    if(nfires==1)
        CHECK(fires[0].grade==X9_HS_GRADE_STANDARD, "grade=%u want STANDARD", fires[0].grade);
    CHECK(nbuzz_on==1, "buzz_on=%d want 1", nbuzz_on);
    /* onset ~ sample 970; sustain fires at +30 ms of hot span */
    CHECK(buzz_on_at>960+1200 && buzz_on_at<960+1800,
        "buzz_on_at=%u want ~onset+30 ms", buzz_on_at);
    CHECK(buzz_level_mid1>300, "buzz level mid=%u want >300 (env of 16000)", buzz_level_mid1);
    CHECK(nbuzz_off==1, "buzz_off=%d want 1", nbuzz_off);
    uint32_t silence_at=960+12000;
    CHECK(buzz_off_at>=silence_at && buzz_off_at<=silence_at+300,
        "buzz_off_at=%u want silence+~5 ms (%u)", buzz_off_at, silence_at);
}
static void test_envelope_tracks_amplitude(void)
{
    struct x9_haptic_stock e;
    x9_haptic_stock_init(&e, X9_HS_FEEL_GRADE|X9_HS_FEEL_ENV, 1000);
    gen_clear(); gen_silence(20); gen_burst(6000, 120); gen_burst(24000, 120); gen_silence(40);
    emulate_busy=1; run_reset();
    run(&e, 960+2880, 960+8640); /* mid of each 120 ms segment */
    CHECK(nbuzz_on==1, "buzz_on=%d want 1 (one unbroken span)", nbuzz_on);
    CHECK(buzz_level_mid1>=100 && buzz_level_mid1<=280,
        "level1=%u want ~183 (env of 6000)", buzz_level_mid1);
    CHECK(buzz_level_mid2>=600 && buzz_level_mid2<=830,
        "level2=%u want ~732 (env of 24000)", buzz_level_mid2);
    CHECK(buzz_level_mid2>buzz_level_mid1+200,
        "level did not follow the amplitude step: %u -> %u", buzz_level_mid1, buzz_level_mid2);
}
static void test_envelope_off_in_legacy(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, 0, 1000);
    gen_clear(); gen_silence(20); gen_burst(16000, 250); gen_silence(40);
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==1, "feel=0 fires=%d want 1", nfires);
    CHECK(nbuzz_on==0, "feel=0 buzz_on=%d want 0", nbuzz_on);
}

static void test_queue_double_tap(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_ALL, 1000);
    gen_clear(); gen_silence(20); gen_burst(20000, 8); gen_silence(1);
    gen_burst(20000, 8); gen_silence(60);
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==2, "fires=%d want 2 (second tap queued, not lost)", nfires);
    if(nfires==2) {
        uint32_t want=x9_stock_wave_tap.len*4u;
        CHECK(fires[1].at>fires[0].at && fires[1].at-fires[0].at+24>=want &&
              fires[1].at-fires[0].at<=want+24,
            "second fire at +%u samples, want right at playback end (~%u)",
            fires[1].at-fires[0].at, want);
        CHECK(fires[1].grade==X9_HS_GRADE_STANDARD, "grade=%u want STANDARD", fires[1].grade);
    }
    CHECK(e.dropped==0, "dropped=%u want 0", e.dropped);
    CHECK(e.pending==0, "pending left set");
}
static void test_queue_rapid_sequence(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_ALL, 1000);
    gen_clear(); gen_silence(20);
    for(int k=0;k<6;k++) { gen_burst(12000, 8); gen_silence(25); }
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==6, "fires=%d want 6 (typing burst loses none)", nfires);
    CHECK(e.dropped==0, "dropped=%u want 0", e.dropped);
}
static void test_queue_full_drop(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_ALL, 1000);
    gen_clear(); gen_silence(20);
    gen_burst(20000, 3); gen_silence(1); gen_burst(20000, 3); gen_silence(1);
    gen_burst(20000, 3); gen_silence(60);
    emulate_busy=1; run_reset(); run(&e, 0, 0);
    CHECK(nfires==2, "fires=%d want 2 (one playing + one queued)", nfires);
    CHECK(e.dropped==1, "dropped=%u want 1 (queue holds at most one)", e.dropped);
}
static void test_tail_guard(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_ALL, 1000);
    gen_clear(); gen_silence(20); gen_burst(32767, 12); gen_silence(6);
    gen_burst(2000, 4); gen_silence(40);
    emulate_busy=0; run_reset(); run(&e, 0, 0);
    CHECK(nfires==1, "fires=%d want 1 (tail blip suppressed)", nfires);
    CHECK(e.tail_dropped==1, "tail_dropped=%u want 1", e.tail_dropped);
    /* Control: the same blip 40 ms later is a genuine quiet event. */
    x9_haptic_stock_init(&e, X9_HS_FEEL_ALL, 1000);
    gen_clear(); gen_silence(20); gen_burst(32767, 12); gen_silence(40);
    gen_burst(2000, 4); gen_silence(40);
    emulate_busy=0; run_reset(); run(&e, 0, 0);
    CHECK(nfires==2, "control fires=%d want 2 (late blip is real)", nfires);
    CHECK(e.tail_dropped==0, "control tail_dropped=%u want 0", e.tail_dropped);
}
static void test_rearm_feel(void)
{
    struct x9_haptic_stock e; x9_haptic_stock_init(&e, X9_HS_FEEL_QUEUE, 1000);
    gen_clear(); gen_silence(20); gen_burst(12000, 8);
    for(int i=0;i<24;i++) pcm[npcm++]=0; /* 0.5 ms gap */
    gen_burst(12000, 8); gen_silence(40);
    emulate_busy=0; run_reset(); run(&e, 0, 0);
    CHECK(nfires==2, "QUEUE arm fires=%d want 2 (0.5 ms gap re-arms)", nfires);
}

static void test_tap_tail_cropped(void)
{
    const struct x9_haptic_stock_wave *w=&x9_stock_wave_tap;
    /* The [0,15ms) source pulse ends in ~5.6 ms of <=2/127 residue; the
     * shipped wave is cropped to the |x|>2 convention, so the busy window
     * (ring drain) tracks the content, not the residue. */
    CHECK(w->len<=120, "tap len=%u want <=120 (residue tail cropped)", w->len);
    int tail_ok=1;
    for(uint32_t i=w->len-12;i<w->len;i++) {
        int v=w->samples[i]; if(v<0)v=-v; if(v>2)tail_ok=0;
    }
    CHECK(tail_ok, "tap last 12 samples exceed 2/127");
}
static void test_light_wave_factory_shape(void)
{
    const struct x9_haptic_stock_wave *w=&x9_stock_wave_tap_light;
    /* Factory yardstick (wave_he_short_f0_174_4 @24 kHz): 8.75 ms total.
     * The old stepable_regulate light wave ran 182 samples (15.2 ms) of
     * rolling low cycles -- the diffuse light-grade feel. */
    CHECK(w->len>=96 && w->len<=120,
        "light len=%u want 96..120 samples (8..10 ms)", w->len);
    uint32_t pk=0;
    for(uint32_t i=0;i<w->len;i++) {
        int v=w->samples[i]; if(v<0)v=-v; if((uint32_t)v>pk)pk=(uint32_t)v;
    }
    CHECK(pk==X9_STOCK_TAP_LIGHT_PEAK,
        "light peak=%u want %u (define must match table)", pk,
        (uint32_t)X9_STOCK_TAP_LIGHT_PEAK);
    CHECK(pk>=100, "light peak=%u want >=100 (down-only scaling)", pk);
    /* Energy concentration: the span at/above quarter peak stays under
     * 7 ms (factory _4: 6.0 ms; old light: 10.8 ms of rolling cycles). */
    uint32_t a25=0;
    for(uint32_t i=0;i<w->len;i++) {
        int v=w->samples[i]; if(v<0)v=-v; if((uint32_t)v*4>=pk)++a25;
    }
    CHECK(a25<=84, "light quarter-peak span=%u want <=84 samples (7 ms)",
        a25);
    /* Clean end so the hardware brake stops a still LRA: the wave ends
     * at zero (final sample and last-4 window ~zero; the final decay
     * slope just before them is the source's own). */
    int tail_ok=1;
    for(uint32_t i=w->len-4;i<w->len;i++) {
        int v=w->samples[i]; if(v<0)v=-v; if(v>8)tail_ok=0;
    }
    { int v=w->samples[w->len-1]; if(v<0)v=-v; if(v>2)tail_ok=0; }
    CHECK(tail_ok, "light does not end at ~zero");
    /* Decay after the peak half-cycle is monotonic (factory _4 half-cycle
     * peaks 103/116/126/116/88/1): no second swell in the tail. */
    uint32_t peaks[32]; int np=0; int sign=0; uint32_t cur=0;
    for(uint32_t i=0;i<w->len;i++) {
        int v=w->samples[i];
        int sg=v>0?1:(v<0?-1:0);
        if(sg) {
            if(sign && sg!=sign) { if(np<32)peaks[np++]=cur; cur=0; }
            sign=sg;
            uint32_t a=(uint32_t)(v<0?-v:v); if(a>cur)cur=a;
        }
    }
    if(np<32)peaks[np++]=cur;
    int argmax=0;
    for(int i=1;i<np;i++) if(peaks[i]>peaks[argmax])argmax=i;
    int decay_ok=1;
    for(int i=argmax+1;i<np;i++) if(peaks[i]>peaks[i-1])decay_ok=0;
    CHECK(np>=4, "light half-cycles=%d want >=4", np);
    CHECK(decay_ok, "light half-cycle peaks swell again after the peak");
}

int main(void)
{
    test_grade_light();
    test_grade_medium();
    test_grade_heavy();
    test_grade_monotonic();
    test_legacy_fixed_full_scale();
    test_legacy_arm_1ms();
    test_hold();
    test_envelope_continuous();
    test_envelope_tracks_amplitude();
    test_envelope_off_in_legacy();
    test_queue_double_tap();
    test_queue_rapid_sequence();
    test_queue_full_drop();
    test_tail_guard();
    test_rearm_feel();
    test_tap_tail_cropped();
    test_light_wave_factory_shape();
    printf("RESULT %s checks=%d failures=%d\n", failures?"FAIL":"PASS", checks, failures);
    return failures?1:0;
}
