/* Host harness for the X9 CPU offline policy header
 * (x9/kernel/src/bsd/dev/x9_cpuoff_policy.h). Drives synthetic scene /
 * util / time sequences through the exact kernel logic and asserts the
 * drain order, floors, hysteresis, backoff and accounting rules. Time
 * is synthetic milliseconds; no MT6993 hardware behaviour is claimed. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "x9_cpuoff_policy.h"

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; \
	printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

#define IDLE X9CPUOFF_SCENE_IDLE
#define LIGHT X9CPUOFF_SCENE_LIGHT
#define INTERACT X9CPUOFF_SCENE_INTERACT

static struct x9cpuoff_decision
decide(struct x9cpuoff_mgr *m, const struct x9cpuoff_cfg *cfg,
    int scene, unsigned util, uint64_t now_ms, int quiescing)
{
	struct x9cpuoff_input in;
	in.scene = scene;
	in.big_util = util;
	in.now_ms = now_ms;
	in.quiescing = quiescing;
	return x9cpuoff_decide(m, cfg, &in);
}

/* Drive an offline decision through its full confirmed lifecycle. */
static void
drive_off(struct x9cpuoff_mgr *m, const struct x9cpuoff_cfg *cfg,
    int scene, unsigned util, uint64_t now_ms, unsigned want_cpu)
{
	struct x9cpuoff_decision d = decide(m, cfg, scene, util, now_ms, 0);
	CHECK(d.action == X9CPUOFF_ACT_OFFLINE && d.cpu == want_cpu,
	    "offline want cpu%u, got action=%d cpu=%u", want_cpu, d.action, d.cpu);
	if (d.action != X9CPUOFF_ACT_OFFLINE) {
		return;
	}
	const unsigned issued = d.cpu;
	x9cpuoff_note_offline_issued(m, issued, now_ms);
	/* In-flight: no further decisions. */
	d = decide(m, cfg, scene, util, now_ms + 100, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE,
	    "in-flight blocks decisions, got action=%d", d.action);
	x9cpuoff_note_off_confirmed(m, issued, now_ms + 200);
}

static void
drive_on(struct x9cpuoff_mgr *m, const struct x9cpuoff_cfg *cfg,
    int scene, unsigned util, uint64_t now_ms, unsigned want_cpu)
{
	struct x9cpuoff_decision d = decide(m, cfg, scene, util, now_ms, 0);
	CHECK(d.action == X9CPUOFF_ACT_ONLINE && d.cpu == want_cpu,
	    "online want cpu%u, got action=%d cpu=%u", want_cpu, d.action, d.cpu);
	if (d.action != X9CPUOFF_ACT_ONLINE) {
		return;
	}
	x9cpuoff_note_online_issued(m, d.cpu, now_ms);
	x9cpuoff_note_online_confirmed(m, d.cpu, now_ms + 300);
}

static void
fresh(struct x9cpuoff_mgr *m, struct x9cpuoff_cfg *cfg,
    int allow_prime, int drain_big)
{
	x9cpuoff_cfg_defaults(cfg, allow_prime, drain_big);
	x9cpuoff_mgr_init(m, 0);
}

/* IDLE drain: order 6,5 (prime excluded, cpu4 is the floor). */
static void test_idle_drain_defaults(void)
{
	struct x9cpuoff_mgr m;
	struct x9cpuoff_cfg cfg;
	struct x9cpuoff_decision d;
	fresh(&m, &cfg, 0, 0);

	d = decide(&m, &cfg, LIGHT, 50, 0, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "lit busy: no action");
	d = decide(&m, &cfg, IDLE, 0, 1000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "idle just started");
	/* Deep at 11s but min_online (30s from init) still blocks. */
	d = decide(&m, &cfg, IDLE, 0, 11000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "min_online blocks at 11s");
	d = decide(&m, &cfg, IDLE, 0, 29999, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "min_online blocks at 29999");
	drive_off(&m, &cfg, IDLE, 0, 30000, 6);
	/* Transition gap: 2s between issued transitions. */
	d = decide(&m, &cfg, IDLE, 0, 31000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "gap blocks at +1s");
	drive_off(&m, &cfg, IDLE, 0, 32000, 5);
	/* cpu4 is the floor, cpu7 not allowed: nothing left to drain. */
	d = decide(&m, &cfg, IDLE, 0, 40000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "floor reached in idle");
	CHECK(m.cpu[4].mstate == X9CPUOFF_M_ONLINE, "cpu4 stayed online");
	CHECK(m.cpu[7].mstate == X9CPUOFF_M_ONLINE, "cpu7 stayed online");
	/* Scene demand restores immediately, bigs low-to-high. */
	drive_on(&m, &cfg, INTERACT, 90, 41000, 5);
	drive_on(&m, &cfg, INTERACT, 90, 44000, 6);
	CHECK(m.transitions == 4, "transitions counted: %u", m.transitions);
	/* Offline residency accounting: cpu5 off 32.2s -> on at 41.3s. */
	CHECK(m.cpu[5].off_total_ms == 9100, "cpu5 off_total %llu",
	    (unsigned long long)m.cpu[5].off_total_ms);
	CHECK(m.cpu[6].off_total_ms == 14100, "cpu6 off_total %llu",
	    (unsigned long long)m.cpu[6].off_total_ms);
}

/* Anti-flap: after a restore, a new idle period must re-earn both the
 * idle sustain and the per-CPU min online residency. */
static void test_antiflap(void)
{
	struct x9cpuoff_mgr m;
	struct x9cpuoff_cfg cfg;
	struct x9cpuoff_decision d;
	fresh(&m, &cfg, 0, 0);
	(void)decide(&m, &cfg, IDLE, 0, 0, 0);	/* idle sustain timer starts */
	drive_off(&m, &cfg, IDLE, 0, 30000, 6);
	drive_on(&m, &cfg, INTERACT, 90, 33000, 6);
	/* Idle again from 34s: deep at 44s, but cpu6 online only since
	 * 33.3s -> min_online pushes eligibility to 63.3s. */
	d = decide(&m, &cfg, IDLE, 0, 44000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "anti-flap blocks re-drain");
	/* At 63299 cpu6 has been back for 29999ms: still blocked, so the
	 * drain takes cpu5 (online since boot) instead. */
	drive_off(&m, &cfg, IDLE, 0, 63299, 5);
	/* cpu6 crosses its min online residency at 63300; with the
	 * transition gap from the cpu5 issue it drains at 65300. */
	drive_off(&m, &cfg, IDLE, 0, 65300, 6);
}

/* LIGHT low-util drain: bigs only, prime never; util wake honours the
 * load-path min offline residency. */
static void test_light_rule(void)
{
	struct x9cpuoff_mgr m;
	struct x9cpuoff_cfg cfg;
	struct x9cpuoff_decision d;
	fresh(&m, &cfg, 0, 0);
	d = decide(&m, &cfg, LIGHT, 5, 0, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "light-low just started");
	/* Sustained at 20s, min_online releases at 30s. */
	drive_off(&m, &cfg, LIGHT, 5, 30000, 6);
	drive_off(&m, &cfg, LIGHT, 5, 32000, 5);
	d = decide(&m, &cfg, LIGHT, 5, 40000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "light floor reached");
	/* Util jumps: load-path online needs 10s offline residency.
	 * cpu6 confirmed off at 30.2s, cpu5 at 32.2s: at 40s neither has
	 * 10s yet; cpu6 becomes eligible first. */
	d = decide(&m, &cfg, LIGHT, 50, 40000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE,
	    "min_offline blocks load wake at 40s, got %d/%u", d.action, d.cpu);
	drive_on(&m, &cfg, LIGHT, 50, 41000, 6);
	drive_on(&m, &cfg, LIGHT, 50, 45000, 5);
	/* Util in the hysteresis band (10..29) holds state. */
	d = decide(&m, &cfg, LIGHT, 20, 46000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "util band holds");
}

/* Failure handling: backoff skips the CPU, repeated failures blacklist,
 * a lost CPU (still_online=0) blacklists immediately. */
static void test_failures(void)
{
	struct x9cpuoff_mgr m;
	struct x9cpuoff_cfg cfg;
	struct x9cpuoff_decision d;
	fresh(&m, &cfg, 0, 0);
	(void)decide(&m, &cfg, IDLE, 0, 0, 0);	/* idle sustain timer starts */
	d = decide(&m, &cfg, IDLE, 0, 30000, 0);
	CHECK(d.action == X9CPUOFF_ACT_OFFLINE && d.cpu == 6, "first drain cpu6");
	x9cpuoff_note_offline_issued(&m, 6, 30000);
	x9cpuoff_note_failure(&m, &cfg, 6, 30100, 1);
	CHECK(m.cpu[6].mstate == X9CPUOFF_M_ONLINE, "failure returns to online");
	CHECK(m.cpu[6].failures == 1, "failure counted");
	/* cpu6 in backoff (60s): drain continues with cpu5. */
	drive_off(&m, &cfg, IDLE, 0, 33000, 5);
	/* Past backoff, cpu6 fails twice more -> blacklisted at 3. */
	d = decide(&m, &cfg, IDLE, 0, 100000, 0);
	CHECK(d.action == X9CPUOFF_ACT_OFFLINE && d.cpu == 6,
	    "cpu6 retried after backoff, got %d/%u", d.action, d.cpu);
	x9cpuoff_note_offline_issued(&m, 6, 100000);
	x9cpuoff_note_failure(&m, &cfg, 6, 100100, 1);
	d = decide(&m, &cfg, IDLE, 0, 200000, 0);
	CHECK(d.action == X9CPUOFF_ACT_OFFLINE && d.cpu == 6, "cpu6 third try");
	x9cpuoff_note_offline_issued(&m, 6, 200000);
	x9cpuoff_note_failure(&m, &cfg, 6, 200100, 1);
	CHECK(m.cpu[6].mstate == X9CPUOFF_M_LOST, "cpu6 blacklisted at 3 failures");
	d = decide(&m, &cfg, IDLE, 0, 300000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "nothing drainable left");
	/* Lost ground truth blacklists at once. */
	x9cpuoff_note_failure(&m, &cfg, 5, 300100, 0);
	CHECK(m.cpu[5].mstate == X9CPUOFF_M_LOST, "cpu5 lost immediately");
}

/* Opt-ins: prime drains first in the deep rule; drain_big empties the
 * big cluster. Quiescing blocks everything. */
static void test_optins_and_quiesce(void)
{
	struct x9cpuoff_mgr m;
	struct x9cpuoff_cfg cfg;
	struct x9cpuoff_decision d;
	fresh(&m, &cfg, 1, 1);
	(void)decide(&m, &cfg, IDLE, 0, 20000, 0);	/* idle sustain timer starts */
	d = decide(&m, &cfg, IDLE, 0, 30000, 1);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "quiescing blocks");
	drive_off(&m, &cfg, IDLE, 0, 30000, 7);
	drive_off(&m, &cfg, IDLE, 0, 32000, 6);
	drive_off(&m, &cfg, IDLE, 0, 34000, 5);
	drive_off(&m, &cfg, IDLE, 0, 36000, 4);
	d = decide(&m, &cfg, IDLE, 0, 40000, 0);
	CHECK(d.action == X9CPUOFF_ACT_NONE, "all drained");
	/* Restore order: 4,5,6,7. */
	drive_on(&m, &cfg, INTERACT, 90, 41000, 4);
	drive_on(&m, &cfg, INTERACT, 90, 44000, 5);
	drive_on(&m, &cfg, INTERACT, 90, 47000, 6);
	drive_on(&m, &cfg, INTERACT, 90, 50000, 7);
	/* Little cluster is never a candidate in any rule. */
	for (unsigned cpu = 0; cpu < 4; cpu++) {
		CHECK(!x9cpuoff_desired(&cfg, 1, cpu),
		    "cpu%u never desired", cpu);
	}
}

int main(void)
{
	test_idle_drain_defaults();
	test_antiflap();
	test_light_rule();
	test_failures();
	test_optins_and_quiesce();
	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("RESULT PASS\n");
		return 0;
	}
	return 1;
}
