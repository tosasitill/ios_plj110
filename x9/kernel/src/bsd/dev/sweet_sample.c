/*
 * SWEET process sampler: where is a daemon stuck?
 *
 * The phone has no debugger and daemons log nowhere readable. Every
 * interval a kernel thread prints, for each process named in boot-arg
 * sweetsample (comma separated, default "SpringBoard,backboardd"; "-" = off)
 * and for each of its threads, the user pc/lr/sp and a frame-pointer
 * backtrace as "SMP name[pid] tN pc ... lr ... bt ..." (header line with the
 * shared region slide and the main binary's mach header, to symbolicate
 * with tools/sweet-crash.py). A thread blocked in a syscall shows the
 * syscall stub in pc and its caller in lr/bt.
 *
 * First sample 90 s after start, then every 120 s, 40 samples at most.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern task_t proc_task(proc_t);
extern int hz;
extern void sweet_sample_start(void);

static char sweet_sample_names[128];

static bool
sweet_sample_wanted(const char *name)
{
	size_t n = strlen(name);
	for (const char *t = sweet_sample_names; *t;) {
		const char *end = strchr(t, ',');
		size_t tl = end ? (size_t)(end - t) : strlen(t);
		if (tl == n && n > 0 && strncmp(t, name, n) == 0) {
			return true;
		}
		if (!end) {
			break;
		}
		t = end + 1;
	}
	return false;
}

extern void sweet_sample_task(const char *name, int pid, task_t task);  /* osfmk/kern/exception.c */

static int
sweet_sample_proc(proc_t p, __unused void *arg)
{
	char name[32];
	proc_name(proc_pid(p), name, sizeof(name));
	if (proc_pid(p) == 1 || sweet_sample_wanted(name)) {
		task_t task = proc_task(p);
		if (task != TASK_NULL) {
			sweet_sample_task(name, proc_pid(p), task);
		}
	}
	return PROC_RETURNED;
}

static void
sweet_sample_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	printf("SWEET SAMPLE: thread started\n");
	if (!PE_parse_boot_argn("sweetsample", sweet_sample_names, sizeof(sweet_sample_names))) {
		strlcpy(sweet_sample_names, "launchd,dyld,init,SpringBoard,backboardd", sizeof(sweet_sample_names));
	}
	if (sweet_sample_names[0] == '-') {
		return;
	}
	uint32_t delay_s = 60;
	uint32_t count = 120;
	(void)PE_parse_boot_argn("sweetsample_delay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("sweetsample_count", &count, sizeof(count));
	(void)tsleep(&chan, PZERO, "sweetsmp", (int)(delay_s * hz));
	for (uint32_t i = 0; i < count; i++) {
		printf("SMP --- sample %u\n", i);
		proc_iterate(PROC_ALLPROCLIST, sweet_sample_proc, NULL, NULL, NULL);
		(void)tsleep(&chan, PZERO, "sweetsmp", 3 * hz);
	}
}

void
sweet_sample_start(void)
{
	thread_t thread;
	if (kernel_thread_start(sweet_sample_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern int hz;
extern task_t proc_task(proc_t);
extern void x9_sample_task(const char *, int, task_t);
static int sample_proc(proc_t p, __unused void *arg) {
 char name[32]; proc_name(proc_pid(p), name, sizeof(name));
 if (!strcmp(name,"SpringBoard") || !strcmp(name,"backboardd") || !strcmp(name,"lsd") || !strcmp(name,"com.apple.datami") || !strcmp(name,"com.apple.migrat")) {
  task_t task=proc_task(p); if(task != TASK_NULL) x9_sample_task(name,proc_pid(p),task);
 }
 return PROC_RETURNED;
}
static void sample_thread(__unused void *arg, __unused wait_result_t wr) {
 static int chan;
 printf("X9 SAMPLE: enabled delay=60 interval=120 count=4 targets=SpringBoard,lsd,backboardd,datamigrator,migrationplugin\n");
 (void)tsleep(&chan,PZERO,"x9sample",60*hz);
 for(unsigned i=0;i<4;i++) {
  printf("X9 SAMPLE: begin %u\n",i+1);
  proc_iterate(PROC_ALLPROCLIST,sample_proc,NULL,NULL,NULL);
  printf("X9 SAMPLE: end %u\n",i+1);
  if(i<3)(void)tsleep(&chan,PZERO,"x9sample",120*hz);
 }
}
void x9_sample_start(void) {
 int enabled=0; thread_t thread;
 if(!PE_parse_boot_argn("x9_thread_sample",&enabled,sizeof(enabled)) || enabled!=1)return;
 if(kernel_thread_start(sample_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 else printf("X9 SAMPLE: failed to start\n");
}
