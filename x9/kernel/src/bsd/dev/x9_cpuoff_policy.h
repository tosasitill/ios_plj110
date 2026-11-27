/*
 * X9 CPU offline policy: pure decision logic for draining and restoring
 * the MT6993 big/prime cores on top of the x9cpuoff facility. No kernel
 * deps; the manager glue in sweet_x9_gpu_dependencies.c feeds measured
 * inputs (x9sched scene, big-domain util, SweetCPU ground truth),
 * executes the returned single transition via processor_exit() /
 * processor_start(), and confirms it against PSCI AFFINITY_INFO.
 * Host-tested by x9/cpuoff/.
 *
 * Modelled on the factory core_ctl (kernel_device_modules-6.12
 * drivers/misc/mediatek/sched/core_ctl): default_min_cpus = {4, 0, 0}
 * keeps the little cluster fully online and lets the big and prime
 * clusters drain. This policy is more conservative by default: the
 * prime core and one big core stay online unless the boot arg opts in
 * (x9cpuoff bits 2 and 3). The little cluster (cpu0-3) is never a
 * candidate, and cpu0 is the boot CPU.
 *
 * One action per call: transitions are serialized by construction, and
 * the caller re-evaluates on the next tick.
 */
#ifndef X9_CPUOFF_POLICY_H
#define X9_CPUOFF_POLICY_H

#include <stdint.h>

#define X9CPUOFF_NCPU 8
#define X9CPUOFF_FIRST_BIG 4
#define X9CPUOFF_PRIME_CPU 7

/* Manager-view CPU states (distinct from the SweetCPU PSCI states). */
enum x9cpuoff_mstate {
	X9CPUOFF_M_ONLINE = 0,
	X9CPUOFF_M_GOING_OFF = 1,	/* processor_exit issued, awaiting OFF confirm */
	X9CPUOFF_M_OFF = 2,		/* confirmed off via AFFINITY_INFO */
	X9CPUOFF_M_COMING_ON = 3,	/* processor_start issued, awaiting initCPU */
	X9CPUOFF_M_LOST = 4		/* parked/blacklisted: never touch again */
};

enum x9cpuoff_action {
	X9CPUOFF_ACT_NONE = 0,
	X9CPUOFF_ACT_OFFLINE = 1,
	X9CPUOFF_ACT_ONLINE = 2
};

struct x9cpuoff_cfg {
	uint32_t idle_off_ms;		/* IDLE scene sustain before draining */
	uint32_t light_off_ms;		/* LIGHT + low big util sustain before draining bigs */
	uint32_t light_util_max;	/* big util below this counts as low (%) */
	uint32_t light_util_wake;	/* big util at/above this restores all (%) */
	uint32_t min_online_ms;		/* anti-flap: min online residency before an offline */
	uint32_t min_offline_ms;	/* load-driven online needs this offline residency */
	uint32_t transition_gap_ms;	/* min spacing between transitions */
	uint32_t backoff_ms;		/* per-CPU retry backoff after a failed transition */
	uint32_t max_failures;		/* consecutive failures before blacklist */
	int allow_prime;		/* x9cpuoff bit 2: cpu7 is a candidate */
	int drain_big;			/* x9cpuoff bit 3: big floor is 0, not 1 */
};

static inline void
x9cpuoff_cfg_defaults(struct x9cpuoff_cfg *cfg, int allow_prime, int drain_big)
{
	cfg->idle_off_ms = 10000;
	cfg->light_off_ms = 20000;
	cfg->light_util_max = 10;
	cfg->light_util_wake = 30;
	cfg->min_online_ms = 30000;
	cfg->min_offline_ms = 10000;
	cfg->transition_gap_ms = 2000;
	cfg->backoff_ms = 60000;
	cfg->max_failures = 3;
	cfg->allow_prime = allow_prime;
	cfg->drain_big = drain_big;
}

struct x9cpuoff_cpu {
	uint32_t mstate;
	uint32_t by_policy;		/* this manager took it offline */
	uint64_t state_since_ms;	/* when mstate was entered */
	uint64_t off_since_ms;		/* when the current offline period began */
	uint64_t off_total_ms;		/* completed offline residency */
	uint32_t failures;		/* consecutive transition failures */
	uint64_t backoff_until_ms;
};

struct x9cpuoff_mgr {
	struct x9cpuoff_cpu cpu[X9CPUOFF_NCPU];
	uint64_t idle_since_ms;		/* UINT64_MAX when scene != IDLE */
	uint64_t light_low_since_ms;	/* UINT64_MAX when not (LIGHT && low util) */
	uint64_t last_action_ms;
	uint32_t transitions;
	uint32_t failures_total;
};

static inline void
x9cpuoff_mgr_init(struct x9cpuoff_mgr *m, uint64_t now_ms)
{
	unsigned i;
	for (i = 0; i < X9CPUOFF_NCPU; i++) {
		m->cpu[i].mstate = X9CPUOFF_M_ONLINE;
		m->cpu[i].by_policy = 0;
		m->cpu[i].state_since_ms = now_ms;
		m->cpu[i].off_since_ms = 0;
		m->cpu[i].off_total_ms = 0;
		m->cpu[i].failures = 0;
		m->cpu[i].backoff_until_ms = 0;
	}
	m->idle_since_ms = UINT64_MAX;
	m->light_low_since_ms = UINT64_MAX;
	m->last_action_ms = 0;
	m->transitions = 0;
	m->failures_total = 0;
}

/* Scene values mirror enum x9sched_scene in x9_sched_policy.h. */
#define X9CPUOFF_SCENE_IDLE 0
#define X9CPUOFF_SCENE_LIGHT 1
#define X9CPUOFF_SCENE_INTERACT 2

struct x9cpuoff_input {
	int scene;
	unsigned big_util;	/* busiest-CPU util of the big domain, % */
	uint64_t now_ms;
	int quiescing;		/* system sleep in progress: never act */
};

struct x9cpuoff_decision {
	int action;		/* enum x9cpuoff_action */
	unsigned cpu;
};

/* Is this CPU in the desired offline set for the current rule?
 * order_rank: drain order (prime first, then bigs high to low). */
static inline int
x9cpuoff_desired(const struct x9cpuoff_cfg *cfg, int deep_idle_rule, unsigned cpu)
{
	if (cpu == X9CPUOFF_PRIME_CPU) {
		return deep_idle_rule && cfg->allow_prime;
	}
	if (cpu >= X9CPUOFF_FIRST_BIG && cpu < X9CPUOFF_PRIME_CPU) {
		if (cpu == X9CPUOFF_FIRST_BIG && !cfg->drain_big) {
			return 0;	/* keep one big core online */
		}
		return 1;
	}
	return 0;	/* little cluster and cpu0: never */
}

static inline int
x9cpuoff_backoff_ok(const struct x9cpuoff_cpu *c, uint64_t now_ms)
{
	return now_ms >= c->backoff_until_ms;
}

/*
 * Update scene timers and decide at most one transition. The caller
 * owns execution and confirmation; it reports outcomes back through
 * x9cpuoff_note_*() below.
 */
static inline struct x9cpuoff_decision
x9cpuoff_decide(struct x9cpuoff_mgr *m, const struct x9cpuoff_cfg *cfg,
    const struct x9cpuoff_input *in)
{
	struct x9cpuoff_decision d;
	unsigned cpu;
	d.action = X9CPUOFF_ACT_NONE;
	d.cpu = 0;

	/* Scene sustain timers (maintained even when we cannot act). */
	if (in->scene == X9CPUOFF_SCENE_IDLE) {
		if (m->idle_since_ms == UINT64_MAX) {
			m->idle_since_ms = in->now_ms;
		}
	} else {
		m->idle_since_ms = UINT64_MAX;
	}
	if (in->scene == X9CPUOFF_SCENE_LIGHT &&
	    in->big_util < cfg->light_util_max) {
		if (m->light_low_since_ms == UINT64_MAX) {
			m->light_low_since_ms = in->now_ms;
		}
	} else {
		m->light_low_since_ms = UINT64_MAX;
	}

	if (in->quiescing) {
		return d;
	}
	/* One transition at a time, with a minimum gap between them. */
	for (cpu = 0; cpu < X9CPUOFF_NCPU; cpu++) {
		if (m->cpu[cpu].mstate == X9CPUOFF_M_GOING_OFF ||
		    m->cpu[cpu].mstate == X9CPUOFF_M_COMING_ON) {
			return d;
		}
	}
	if (in->now_ms - m->last_action_ms < cfg->transition_gap_ms &&
	    m->last_action_ms != 0) {
		return d;
	}

	/* Demand: any interactive-class scene, or big util recovery in
	 * LIGHT, restores every policy-offlined core immediately (scene
	 * demand ignores the load-path min offline residency). */
	int want_online = 0;
	if (in->scene >= X9CPUOFF_SCENE_INTERACT) {
		want_online = 1;
	} else if (in->scene == X9CPUOFF_SCENE_LIGHT &&
	    in->big_util >= cfg->light_util_wake) {
		want_online = 1;
	}
	if (want_online) {
		int scene_demand = in->scene >= X9CPUOFF_SCENE_INTERACT;
		/* Reverse drain order: bigs low-to-high, prime last. */
		for (cpu = X9CPUOFF_FIRST_BIG; cpu < X9CPUOFF_NCPU; cpu++) {
			const struct x9cpuoff_cpu *c = &m->cpu[cpu];
			if (c->mstate != X9CPUOFF_M_OFF || !c->by_policy ||
			    !x9cpuoff_backoff_ok(c, in->now_ms)) {
				continue;
			}
			if (!scene_demand && c->off_since_ms != 0 &&
			    in->now_ms - c->off_since_ms < cfg->min_offline_ms) {
				continue;	/* load path: min offline residency */
			}
			d.action = X9CPUOFF_ACT_ONLINE;
			d.cpu = cpu;
			return d;
		}
		return d;
	}

	/* Drain rules. */
	int deep = in->scene == X9CPUOFF_SCENE_IDLE &&
	    m->idle_since_ms != UINT64_MAX &&
	    in->now_ms - m->idle_since_ms >= cfg->idle_off_ms;
	int light = in->scene == X9CPUOFF_SCENE_LIGHT &&
	    m->light_low_since_ms != UINT64_MAX &&
	    in->now_ms - m->light_low_since_ms >= cfg->light_off_ms;
	if (!deep && !light) {
		return d;
	}
	/* Drain order: prime first (deep rule only), then bigs high to low. */
	static const unsigned drain_order[4] = { 7, 6, 5, 4 };
	for (unsigned oi = 0; oi < 4; oi++) {
		cpu = drain_order[oi];
		const struct x9cpuoff_cpu *c = &m->cpu[cpu];
		if (!x9cpuoff_desired(cfg, deep, cpu)) {
			continue;
		}
		if (c->mstate != X9CPUOFF_M_ONLINE || c->by_policy) {
			continue;
		}
		if (!x9cpuoff_backoff_ok(c, in->now_ms)) {
			continue;
		}
		if (in->now_ms - c->state_since_ms < cfg->min_online_ms) {
			continue;	/* anti-flap */
		}
		d.action = X9CPUOFF_ACT_OFFLINE;
		d.cpu = cpu;
		return d;
	}
	return d;
}

/* The caller issued processor_exit() for this CPU. */
static inline void
x9cpuoff_note_offline_issued(struct x9cpuoff_mgr *m, unsigned cpu,
    uint64_t now_ms)
{
	struct x9cpuoff_cpu *c = &m->cpu[cpu];
	c->mstate = X9CPUOFF_M_GOING_OFF;
	c->state_since_ms = now_ms;
	m->last_action_ms = now_ms;
}

/* AFFINITY_INFO confirmed the CPU is off. */
static inline void
x9cpuoff_note_off_confirmed(struct x9cpuoff_mgr *m, unsigned cpu,
    uint64_t now_ms)
{
	struct x9cpuoff_cpu *c = &m->cpu[cpu];
	c->mstate = X9CPUOFF_M_OFF;
	c->by_policy = 1;
	c->state_since_ms = now_ms;
	c->off_since_ms = now_ms;
	c->failures = 0;
	m->transitions++;
}

/* The caller issued processor_start() for this CPU. */
static inline void
x9cpuoff_note_online_issued(struct x9cpuoff_mgr *m, unsigned cpu,
    uint64_t now_ms)
{
	struct x9cpuoff_cpu *c = &m->cpu[cpu];
	c->mstate = X9CPUOFF_M_COMING_ON;
	c->state_since_ms = now_ms;
	m->last_action_ms = now_ms;
}

/* initCPU publication confirmed the CPU is back. */
static inline void
x9cpuoff_note_online_confirmed(struct x9cpuoff_mgr *m, unsigned cpu,
    uint64_t now_ms)
{
	struct x9cpuoff_cpu *c = &m->cpu[cpu];
	if (c->off_since_ms != 0 && now_ms >= c->off_since_ms) {
		c->off_total_ms += now_ms - c->off_since_ms;
	}
	c->off_since_ms = 0;
	c->mstate = X9CPUOFF_M_ONLINE;
	c->by_policy = 0;
	c->state_since_ms = now_ms;
	c->failures = 0;
	m->transitions++;
}

/* A transition failed or timed out. still_online != 0 means ground
 * truth says the CPU never left (or is back); otherwise it is lost. */
static inline void
x9cpuoff_note_failure(struct x9cpuoff_mgr *m, const struct x9cpuoff_cfg *cfg,
    unsigned cpu, uint64_t now_ms, int still_online)
{
	struct x9cpuoff_cpu *c = &m->cpu[cpu];
	c->failures++;
	m->failures_total++;
	c->backoff_until_ms = now_ms + cfg->backoff_ms;
	c->state_since_ms = now_ms;
	if (!still_online || c->failures >= cfg->max_failures) {
		c->mstate = X9CPUOFF_M_LOST;
	} else {
		c->mstate = X9CPUOFF_M_ONLINE;
		c->by_policy = 0;
	}
}

#endif /* X9_CPUOFF_POLICY_H */
