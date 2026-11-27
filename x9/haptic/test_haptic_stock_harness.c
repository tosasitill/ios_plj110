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

int main(void)
{
    test_grade_light();
    test_grade_medium();
    test_grade_heavy();
    test_grade_monotonic();
    test_legacy_fixed_full_scale();
    test_legacy_arm_1ms();
    test_hold();
    printf("RESULT %s checks=%d failures=%d\n", failures?"FAIL":"PASS", checks, failures);
    return failures?1:0;
}
