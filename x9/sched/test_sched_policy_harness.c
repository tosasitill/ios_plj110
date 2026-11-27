/* Host harness for the X9 scheduler policy v2 header
 * (x9/kernel/src/bsd/dev/x9_sched_policy.h). Drives synthetic OPP tables
 * and signal sequences through the exact kernel logic and asserts the
 * decisions: OPP selection, thermal cap curve, scene classification and
 * the per-domain up/down commit rules. Tables are synthetic shapes, not
 * captured MT6993 LUTs. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "x9_sched_policy.h"

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; \
	printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static const uint16_t little_mhz[] = {500, 800, 1100, 1400, 1700, 2000, 2300, 2700};
static const uint16_t big_mhz[] = {500, 900, 1300, 1800, 2300, 2900, 3500};
static const uint16_t prime_mhz[] = {500, 1200, 2000, 2800, 3600, 4210};
static const uint16_t unordered_mhz[] = {2000, 500, 2700, 1100};

static void
domain_init(struct x9sched_domain *d, const uint16_t *mhz, unsigned n,
    unsigned id, unsigned idle, unsigned boot, unsigned cur)
{
	memset(d, 0, sizeof(*d));
	d->opp_mhz = mhz; d->n_opp = n; d->domain_id = id;
	d->idle_idx = idle; d->boot_idx = boot; d->cur_idx = cur;
}

static struct x9sched_input
input(unsigned util, int temp, int valid, int scene, int dark, uint64_t now)
{
	struct x9sched_input in;
	in.util = util; in.temp_c = temp; in.temp_valid = valid;
	in.scene = scene; in.dark = dark; in.now_ns = now;
	return in;
}

static void test_opp_helpers(void)
{
	CHECK(x9sched_opp_at_least(unordered_mhz, 4, 1000000) == 3,
	    "at_least 1000MHz -> 1100 idx3, got %u",
	    x9sched_opp_at_least(unordered_mhz, 4, 1000000));
	CHECK(x9sched_opp_at_least(unordered_mhz, 4, 2800000) == 2,
	    "at_least above top -> top idx2, got %u",
	    x9sched_opp_at_least(unordered_mhz, 4, 2800000));
	CHECK(x9sched_opp_at_least(unordered_mhz, 4, 500000) == 1,
	    "at_least exact bottom -> idx1, got %u",
	    x9sched_opp_at_least(unordered_mhz, 4, 500000));
	CHECK(x9sched_opp_at_most(unordered_mhz, 4, 1000000) == 1,
	    "at_most 1000MHz -> 500 idx1, got %u",
	    x9sched_opp_at_most(unordered_mhz, 4, 1000000));
	CHECK(x9sched_opp_at_most(unordered_mhz, 4, 400000) == 1,
	    "at_most below bottom -> bottom idx1, got %u",
	    x9sched_opp_at_most(unordered_mhz, 4, 400000));
	CHECK(x9sched_opp_top(unordered_mhz, 4) == 2, "top idx2, got %u",
	    x9sched_opp_top(unordered_mhz, 4));
}

static void test_thermal_cap(void)
{
	const uint32_t top = 4210000, idle = 500000, boot = 1200000;
	CHECK(x9sched_thermal_cap_khz(90, 0, top, idle, boot) == boot,
	    "invalid temp -> boot cap");
	CHECK(x9sched_thermal_cap_khz(25, 1, top, idle, boot) == top,
	    "25C -> top");
	CHECK(x9sched_thermal_cap_khz(70, 1, top, idle, boot) == top,
	    "70C -> top (curve start)");
	CHECK(x9sched_thermal_cap_khz(95, 1, top, idle, boot) == idle,
	    "95C -> idle");
	CHECK(x9sched_thermal_cap_khz(110, 1, top, idle, boot) == idle,
	    "110C -> idle");
	CHECK(x9sched_thermal_cap_khz(82, 1, top, idle, boot) ==
	    top - (top - idle) * 12 / 25, "82C -> linear mid, got %u",
	    x9sched_thermal_cap_khz(82, 1, top, idle, boot));
	{
		uint32_t prev = top;
		int t;
		for (t = 60; t <= 110; t++) {
			uint32_t cap = x9sched_thermal_cap_khz(t, 1, top, idle, boot);
			CHECK(cap <= prev && cap >= idle && cap <= top,
			    "cap monotone/bounded at %dC: %u", t, cap);
			prev = cap;
		}
	}
}

static struct x9sched_signals
signals(int dark, int cam, uint32_t touch, uint32_t swap,
    unsigned gpu, unsigned cpu)
{
	struct x9sched_signals s;
	s.dark = dark; s.camera_active = cam; s.touch_age_ms = touch;
	s.swap_age_ms = swap; s.gpu_util = gpu; s.cpu_peak = cpu;
	return s;
}

static void test_scene(void)
{
	struct x9sched_scene_state st;
	struct x9sched_signals s;
	int i, scene;

	memset(&st, 0, sizeof(st));
	s = signals(1, 1, 0, 0, 100, 100);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_IDLE,
	    "dark beats camera/load");

	memset(&st, 0, sizeof(st));
	s = signals(0, 1, UINT32_MAX, UINT32_MAX, 0, 0);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_CAMERA,
	    "camera hint -> camera");

	memset(&st, 0, sizeof(st));
	s = signals(0, 0, 50, UINT32_MAX, 0, 0);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_INTERACT,
	    "fresh touch -> interact");
	s = signals(0, 0, UINT32_MAX, 50, 0, 0);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_INTERACT,
	    "fresh swap -> interact");
	s = signals(0, 0, UINT32_MAX, UINT32_MAX, 0, 0);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_LIGHT,
	    "stale signals -> light");

	memset(&st, 0, sizeof(st));
	s = signals(0, 0, UINT32_MAX, UINT32_MAX, 80, 80);
	for (i = 0; i < X9SCHED_LOAD_ENTER_TICKS - 1; i++) {
		scene = x9sched_scene_update(&st, &s);
	}
	CHECK(scene != X9SCENE_LOAD, "load needs sustained pressure");
	scene = x9sched_scene_update(&st, &s);
	CHECK(scene == X9SCENE_LOAD, "load after %d ticks, got %s",
	    X9SCHED_LOAD_ENTER_TICKS, x9sched_scene_name(scene));
	s = signals(0, 0, UINT32_MAX, UINT32_MAX, 65, 55);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_LOAD,
	    "load holds inside hysteresis band");
	s = signals(0, 0, UINT32_MAX, UINT32_MAX, 40, 30);
	CHECK(x9sched_scene_update(&st, &s) == X9SCENE_LIGHT,
	    "load exits below band");
	s = signals(0, 0, UINT32_MAX, UINT32_MAX, 74, 80);
	for (i = 0; i < X9SCHED_LOAD_ENTER_TICKS + 2; i++) {
		scene = x9sched_scene_update(&st, &s);
	}
	CHECK(scene != X9SCENE_LOAD, "gpu 74%% never enters load");
}

static void test_domain_ramp(void)
{
	struct x9sched_domain d;
	struct x9sched_input in;
	domain_init(&d, little_mhz, 8, 0, 0, 2, 0);

	in = input(0, 25, 1, X9SCENE_LIGHT, 0, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 0, "idle util stays at idle OPP");
	CHECK(d.up_commits == 0, "no spurious up commit");

	in = input(100, 25, 1, X9SCENE_LIGHT, 0, 1000000);
	CHECK(x9sched_domain_decide(&d, &in) == 1,
	    "full util steps 500->800, got idx %u", d.cur_idx);
	in.now_ns += 1000000;
	CHECK(x9sched_domain_decide(&d, &in) == 2,
	    "sustained full util steps 800->1100, got idx %u", d.cur_idx);
	in.now_ns += 1000000;
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "sustained full util steps 1100->1400, got idx %u", d.cur_idx);
	CHECK(d.up_commits == 3, "3 up commits, got %u", d.up_commits);
}

static void test_domain_floors(void)
{
	struct x9sched_domain d;
	struct x9sched_input in;

	domain_init(&d, little_mhz, 8, 0, 0, 2, 0);
	in = input(0, 25, 1, X9SCENE_INTERACT, 0, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "interact floor lifts little to 1400 (first OPP >= 1300), got %u",
	    d.cur_idx);

	domain_init(&d, big_mhz, 7, 1, 0, 2, 0);
	in = input(0, 25, 1, X9SCENE_INTERACT, 0, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 0,
	    "interact does not floor big (vendor touch parity), got %u",
	    d.cur_idx);

	domain_init(&d, big_mhz, 7, 1, 0, 2, 0);
	in = input(0, 25, 1, X9SCENE_CAMERA, 0, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "camera floor lifts big to 1800 (50%% of 3500), got %u", d.cur_idx);

	domain_init(&d, prime_mhz, 6, 2, 0, 2, 0);
	in = input(0, 25, 1, X9SCENE_LOAD, 0, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "load floor lifts prime to 2800 (60%% of 4210), got %u", d.cur_idx);
}

static void test_domain_thermal(void)
{
	struct x9sched_domain d;
	struct x9sched_input in;

	/* Hot: cap at idle OPP; the drop is geometric and held, not a cliff. */
	domain_init(&d, prime_mhz, 6, 2, 0, 1, 5);
	in = input(100, 100, 1, X9SCENE_LIGHT, 0, 1000000000ull);
	CHECK(x9sched_domain_decide(&d, &in) == 5,
	    "first hot tick starts the down-hold, stays at top");
	CHECK(d.capped_ticks == 1, "cap counted");
	in.now_ns += 150000000ull;
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "after hold, geometric step 4210->2800, got %u", d.cur_idx);
	CHECK(d.down_commits == 1, "one down commit");

	/* Unknown temperature: fail safe to the boot OPP. */
	domain_init(&d, prime_mhz, 6, 2, 0, 1, 5);
	in = input(100, 0, 0, X9SCENE_LIGHT, 1, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "unknown temp caps toward boot (1200): geometric 4210->2800, got %u",
	    d.cur_idx);
	in.now_ns += 1000000;
	CHECK(x9sched_domain_decide(&d, &in) == 2,
	    "geometric again 2800->2000, got %u", d.cur_idx);
	in.now_ns += 1000000;
	CHECK(x9sched_domain_decide(&d, &in) == 1,
	    "then reaches boot OPP 1200, got %u", d.cur_idx);
}

static void test_domain_down_rules(void)
{
	struct x9sched_domain d;
	struct x9sched_input in;

	/* Dark: no hold, but still geometric. */
	domain_init(&d, little_mhz, 8, 0, 0, 2, 7);
	in = input(0, 25, 1, X9SCENE_IDLE, 1, 0);
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "dark drop 2700->1400 (half), got %u", d.cur_idx);
	in.now_ns += 1000000;
	CHECK(x9sched_domain_decide(&d, &in) == 1,
	    "dark drop 1400->800, got %u", d.cur_idx);
	in.now_ns += 1000000;
	CHECK(x9sched_domain_decide(&d, &in) == 0,
	    "dark drop 800->500 idle, got %u", d.cur_idx);

	/* Lit: 100 ms hold before the first down-step. */
	domain_init(&d, little_mhz, 8, 0, 0, 2, 7);
	in = input(0, 25, 1, X9SCENE_LIGHT, 0, 500000000ull);
	CHECK(x9sched_domain_decide(&d, &in) == 7, "hold starts, no step");
	in.now_ns += 50000000ull;
	CHECK(x9sched_domain_decide(&d, &in) == 7, "still holding at +50ms");
	in.now_ns += 60000000ull;
	CHECK(x9sched_domain_decide(&d, &in) == 3,
	    "step after 110ms, got %u", d.cur_idx);
}

static void test_domain_peak_decay(void)
{
	struct x9sched_domain d;
	struct x9sched_input in;

	/* One saturated tick, then silence: peak decay keeps the domain up
	 * through the hold instead of collapsing with the window. */
	domain_init(&d, little_mhz, 8, 0, 0, 2, 3);
	in = input(100, 25, 1, X9SCENE_LIGHT, 0, 0);
	(void)x9sched_domain_decide(&d, &in);
	{
		unsigned after_spike = d.cur_idx;
		in = input(0, 25, 1, X9SCENE_LIGHT, 0, 50000000ull);
		CHECK(x9sched_domain_decide(&d, &in) == after_spike,
		    "silent tick after spike holds %u, got %u",
		    after_spike, d.cur_idx);
		CHECK(d.peak_util >= 80, "peak still elevated: %u", d.peak_util);
	}
}

int main(void)
{
	test_opp_helpers();
	test_thermal_cap();
	test_scene();
	test_domain_ramp();
	test_domain_floors();
	test_domain_thermal();
	test_domain_down_rules();
	test_domain_peak_decay();
	printf("sched policy harness: %d checks, %d failures\n", checks, failures);
	printf("RESULT %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
