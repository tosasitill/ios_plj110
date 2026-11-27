/*
 * X9 scheduler policy v2: pure decision logic for the MT6993 CPU frequency
 * domains, scene classification and thermal capping. No kernel deps; the
 * governor glue in sweet_x9_gpu_dependencies.c feeds measured inputs and
 * commits the returned OPP index. Host-tested by x9/sched/.
 *
 * Modelled on the vendor stack in the OPPO MT6993 kernel sources
 * (kernel_device_modules-6.12): schedutil's util/target ratio with headroom
 * (drivers/misc/mediatek/sched/sugov), touch_boost's little-domain floor
 * (performance/touch_boost), FPSGO's frame-driven floors for the big
 * domains (performance/fpsgo_v3) and PPM-style thermal caps that converge
 * before the wall instead of braking at it. XNU has no EAS/PELT here (one
 * SMP cluster, MAX_CPU_CLUSTERS=1), so util is the governor's measured
 * busiest-CPU busy percent per domain, peak-decayed to approximate the
 * frequency-invariant demand PELT would give.
 */
#ifndef X9_SCHED_POLICY_H
#define X9_SCHED_POLICY_H

#include <stdint.h>

enum x9sched_scene {
	X9SCENE_IDLE = 0,	/* display dark: consolidate, drop fast */
	X9SCENE_LIGHT = 1,	/* lit, no recent interaction */
	X9SCENE_INTERACT = 2,	/* touch or presented frames within 100 ms */
	X9SCENE_CAMERA = 3,	/* camera capture session hint active */
	X9SCENE_LOAD = 4,	/* sustained GPU+CPU load (game-like) */
	X9SCENE_COUNT = 5
};

static inline const char *x9sched_scene_name(int scene)
{
	switch (scene) {
	case X9SCENE_IDLE: return "idle";
	case X9SCENE_INTERACT: return "interact";
	case X9SCENE_CAMERA: return "camera";
	case X9SCENE_LOAD: return "load";
	default: return "light";
	}
}

/* ---- OPP table helpers (table order is NOT assumed) ---- */

/* Index of the lowest-frequency OPP that is >= khz; the top OPP if none. */
static inline unsigned
x9sched_opp_at_least(const uint16_t *mhz, unsigned n, uint32_t khz)
{
	int found = -1;
	unsigned top = 0, i;
	for (i = 0; i < n; i++) {
		if (mhz[i] > mhz[top]) {
			top = i;
		}
		if ((uint32_t)mhz[i] * 1000u >= khz &&
		    (found < 0 || mhz[i] < mhz[found])) {
			found = (int)i;
		}
	}
	return found >= 0 ? (unsigned)found : top;
}

/* Index of the highest-frequency OPP that is <= khz; the bottom OPP if none. */
static inline unsigned
x9sched_opp_at_most(const uint16_t *mhz, unsigned n, uint32_t khz)
{
	int found = -1;
	unsigned bottom = 0, i;
	for (i = 0; i < n; i++) {
		if (mhz[i] < mhz[bottom]) {
			bottom = i;
		}
		if ((uint32_t)mhz[i] * 1000u <= khz &&
		    (found < 0 || mhz[i] > mhz[found])) {
			found = (int)i;
		}
	}
	return found >= 0 ? (unsigned)found : bottom;
}

static inline unsigned
x9sched_opp_top(const uint16_t *mhz, unsigned n)
{
	unsigned top = 0, i;
	for (i = 0; i < n; i++) {
		if (mhz[i] > mhz[top]) {
			top = i;
		}
	}
	return top;
}

/* ---- Thermal cap ----
 * Smooth convergence: full speed up to 70 C, linear derate to the idle OPP
 * at 95 C, idle OPP at/above 95 C. Unknown temperature fails safe to the
 * boot (LK) frequency, exactly like governor v1. Never below idle_khz. */
#define X9SCHED_THERMAL_START_C 70
#define X9SCHED_THERMAL_FULL_C 95

static inline uint32_t
x9sched_thermal_cap_khz(int temp_c, int temp_valid, uint32_t top_khz,
    uint32_t idle_khz, uint32_t boot_khz)
{
	uint32_t cap;
	if (!temp_valid) {
		cap = boot_khz;
	} else if (temp_c <= X9SCHED_THERMAL_START_C) {
		cap = top_khz;
	} else if (temp_c >= X9SCHED_THERMAL_FULL_C) {
		cap = idle_khz;
	} else {
		uint32_t span = (uint32_t)(X9SCHED_THERMAL_FULL_C -
		    X9SCHED_THERMAL_START_C);
		uint32_t over = (uint32_t)(temp_c - X9SCHED_THERMAL_START_C);
		cap = top_khz - (top_khz - idle_khz) * over / span;
	}
	if (cap < idle_khz) {
		cap = idle_khz;
	}
	if (cap > top_khz) {
		cap = top_khz;
	}
	return cap;
}

/* ---- Scene classification ---- */

struct x9sched_signals {
	int dark;		/* display dark */
	int camera_active;	/* capture session hint */
	uint32_t touch_age_ms;	/* UINT32_MAX when never */
	uint32_t swap_age_ms;	/* age of last presented frame */
	unsigned gpu_util;	/* 0..100, last governor window */
	unsigned cpu_peak;	/* 0..100, busiest CPU, previous tick */
};

struct x9sched_scene_state {
	unsigned load_ticks;	/* consecutive qualifying ticks */
	int scene;
};

/* Load entry needs ~1 s of sustained GPU+CPU pressure at the 50 ms tick;
 * exit is immediate below the holding band (hysteresis 75/70 in, 60/50 hold,
 * below that out). Priority: dark > camera > load > interact > light. */
#define X9SCHED_LOAD_ENTER_TICKS 20

static inline int
x9sched_scene_update(struct x9sched_scene_state *st,
    const struct x9sched_signals *s)
{
	int scene;
	if (s->dark) {
		st->load_ticks = 0;
		st->scene = X9SCENE_IDLE;
		return st->scene;
	}
	if (s->camera_active) {
		st->load_ticks = 0;
		st->scene = X9SCENE_CAMERA;
		return st->scene;
	}
	if (st->scene == X9SCENE_LOAD) {
		if (s->gpu_util >= 60 && s->cpu_peak >= 50) {
			return st->scene;	/* hold */
		}
		st->load_ticks = 0;
	} else if (s->gpu_util >= 75 && s->cpu_peak >= 70) {
		if (++st->load_ticks >= X9SCHED_LOAD_ENTER_TICKS) {
			st->scene = X9SCENE_LOAD;
			return st->scene;
		}
	} else {
		st->load_ticks = 0;
	}
	scene = (s->touch_age_ms <= 100 || s->swap_age_ms <= 100) ?
	    X9SCENE_INTERACT : X9SCENE_LIGHT;
	st->scene = scene;
	return scene;
}

/* ---- Per-domain frequency decision ---- */

struct x9sched_domain {
	/* configuration, set once by the glue */
	const uint16_t *opp_mhz;
	unsigned n_opp;
	unsigned domain_id;	/* 0 little, 1 big, 2 prime */
	unsigned idle_idx;	/* lowest OPP >= 500 MHz (glue-chosen) */
	unsigned boot_idx;	/* OPP closest to the LK/boot request */
	/* state */
	unsigned cur_idx;
	unsigned peak_util;	/* decayed peak of measured util */
	uint64_t down_since_ns;	/* first pending down-step time, 0 = none */
	/* observability */
	uint32_t up_commits;
	uint32_t down_commits;
	uint32_t capped_ticks;
};

struct x9sched_input {
	unsigned util;		/* 0..100 busiest CPU of the domain */
	int temp_c;
	int temp_valid;
	int scene;
	int dark;		/* skip the down-hold while dark */
	uint64_t now_ns;
};

/* Util target (divisor) per scene: lower = more headroom. */
static inline unsigned
x9sched_target_pct(int scene)
{
	switch (scene) {
	case X9SCENE_LOAD: return 65;
	case X9SCENE_INTERACT: return 70;
	case X9SCENE_CAMERA: return 70;
	default: return 80;
	}
}

/* Frequency floor per scene/domain: fixed kHz or percent of domain top,
 * whichever is higher. INTERACT mirrors vendor touch_boost (little only);
 * CAMERA/LOAD add big/prime floors like FPSGO does for the render path. */
static inline uint32_t
x9sched_floor_khz(int scene, unsigned domain_id, uint32_t top_khz)
{
	uint32_t fixed = 0, pct = 0;
	switch (scene) {
	case X9SCENE_INTERACT:
		if (domain_id == 0) {
			fixed = 1300000u;
		}
		break;
	case X9SCENE_CAMERA:
		if (domain_id == 0) {
			fixed = 1300000u;
		} else {
			pct = 50;
		}
		break;
	case X9SCENE_LOAD:
		pct = 60;
		break;
	default:
		break;
	}
	{
		uint32_t by_pct = top_khz * pct / 100;
		return fixed > by_pct ? fixed : by_pct;
	}
}

#define X9SCHED_DOWN_HOLD_NS 100000000ull

/*
 * Decide the next OPP index for one domain. Up-steps are immediate;
 * down-steps wait out the hold (unless dark) and move geometrically
 * (at most half the current frequency per hold window) so a bursty
 * workload does not fall off a cliff between bursts.
 */
static inline unsigned
x9sched_domain_decide(struct x9sched_domain *d,
    const struct x9sched_input *in)
{
	const uint16_t *mhz = d->opp_mhz;
	unsigned top_idx = x9sched_opp_top(mhz, d->n_opp);
	uint32_t top_khz = (uint32_t)mhz[top_idx] * 1000u;
	uint32_t idle_khz = (uint32_t)mhz[d->idle_idx] * 1000u;
	uint32_t boot_khz = (uint32_t)mhz[d->boot_idx] * 1000u;
	uint32_t cur_khz = (uint32_t)mhz[d->cur_idx] * 1000u;
	unsigned util_eff, target;
	uint32_t demand, wanted, cap;
	unsigned next;

	/* Peak-decayed demand: bursts hold the effective util up for
	 * roughly half a second instead of collapsing between windows. */
	if (in->util > d->peak_util) {
		d->peak_util = in->util;
	} else if (d->peak_util > 0) {
		unsigned decay = d->peak_util / 8;
		d->peak_util -= decay ? decay : 1;
	}
	util_eff = in->util;
	if (d->peak_util * 3 / 4 > util_eff) {
		util_eff = d->peak_util * 3 / 4;
	}
	if (util_eff > 100) {
		util_eff = 100;
	}

	target = x9sched_target_pct(in->scene);
	demand = (uint32_t)((uint64_t)cur_khz * util_eff / target);
	wanted = demand;
	{
		uint32_t floor_khz = x9sched_floor_khz(in->scene,
		    d->domain_id, top_khz);
		if (floor_khz > wanted) {
			wanted = floor_khz;
		}
	}
	cap = x9sched_thermal_cap_khz(in->temp_c, in->temp_valid, top_khz,
	    idle_khz, boot_khz);
	if (cap < wanted) {
		d->capped_ticks++;
		wanted = cap;
	}
	if (wanted < idle_khz) {
		wanted = idle_khz;
	}
	next = x9sched_opp_at_least(mhz, d->n_opp, wanted);
	if (next == d->cur_idx) {
		d->down_since_ns = 0;
		return next;
	}
	if (mhz[next] > mhz[d->cur_idx]) {
		d->down_since_ns = 0;
		d->up_commits++;
		d->cur_idx = next;
		return next;
	}
	/* Down-step: geometric limit, then the hold. */
	{
		uint32_t limit_khz = cur_khz / 2;
		if (wanted < limit_khz) {
			next = x9sched_opp_at_least(mhz, d->n_opp, limit_khz);
			if (mhz[next] >= mhz[d->cur_idx]) {
				next = x9sched_opp_at_most(mhz, d->n_opp,
				    cur_khz - 1);
			}
		}
	}
	if (next == d->cur_idx) {
		d->down_since_ns = 0;
		return next;
	}
	if (!in->dark) {
		if (!d->down_since_ns) {
			d->down_since_ns = in->now_ns;
			return d->cur_idx;
		}
		if (in->now_ns - d->down_since_ns < X9SCHED_DOWN_HOLD_NS) {
			return d->cur_idx;
		}
	}
	d->down_since_ns = 0;
	d->down_commits++;
	d->cur_idx = next;
	return next;
}

#endif /* X9_SCHED_POLICY_H */
