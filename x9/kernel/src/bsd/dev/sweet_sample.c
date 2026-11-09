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
#include <sys/uio.h>
#include <sys/vnode.h>
#include <kern/kalloc.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern int hz;
extern task_t proc_task(proc_t);
extern void x9_sample_task(const char *, int, task_t);
/* x9_sample_procs=a,b,c replaces the built-in target list (names as proc_name gives them). */
static char sample_names[160];
static int sample_listed(const char *name) {
 size_t n=strlen(name);
 for(const char *t=sample_names;*t;) {
  const char *end=strchr(t,',');size_t tl=end?(size_t)(end-t):strlen(t);
  if(tl==n&&n&&!strncmp(t,name,n))return 1;
  if(!end)break;t=end+1;
 }
 return 0;
}
static int sample_proc(proc_t p, __unused void *arg) {
 char name[32]; proc_name(proc_pid(p), name, sizeof(name));
 if (sample_names[0] ? sample_listed(name) :
     (!strcmp(name,"SpringBoard") || !strcmp(name,"backboardd") || !strcmp(name,"wifid") || !strcmp(name,"lsd") || !strcmp(name,"com.apple.datami") || !strcmp(name,"com.apple.migrat"))) {
  task_t task=proc_task(p); if(task != TASK_NULL) x9_sample_task(name,proc_pid(p),task);
 }
 return PROC_RETURNED;
}
/* 31/32.txt: the assistant.support seed printed errno=0 at 2.7 s, yet the app
 * main thread stays in the dictation-availability loop. Ask the filesystem
 * again from this thread: if cfprefsd replaced our XML plist (size/mtime
 * change or a bplist00 header) the preference was ignored downstream rather
 * than lost, and the fix has to move to the services. x9_prefcheck=0 disables. */
static void
x9_prefdump(const char *path, int dump_bytes) {
 vnode_t vp=NULLVP;
 vfs_context_t ctx=vfs_context_kernel();
 /* rev252v seeds 26 language spellings into DictationLanguagesEnabled, which
  * makes our own XML about 1 KB; the old 1024-byte window cut the tail off, i.e.
  * exactly the KeyboardsCurrentAndNext entry we need to watch. */
 static char buf[2048];
 struct vnode_attr va;
 errno_t error=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);
 if(error) { printf("X9 PREF: %s lookup errno=%d\n",path,error); return; }
 VATTR_INIT(&va);
 VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_modify_time);
 VATTR_WANTED(&va, va_mode); VATTR_WANTED(&va, va_uid);
	if(vnode_getattr(vp,&va,ctx)==0)
	 printf("X9 PREF: %s size=%llu mtime=%lld mode=0%o uid=%d\n",path,
	  (unsigned long long)va.va_data_size,(long long)va.va_modify_time.tv_sec,
	  (unsigned int)va.va_mode,(int)va.va_uid);
	else printf("X9 PREF: %s getattr failed\n",path);
	if(dump_bytes) {
	 uio_t auio=uio_create(1,0,UIO_SYSSPACE,UIO_READ);
	 size_t got=sizeof(buf);
	 if(auio==NULL) printf("X9 PREF: uio_create failed\n");
	 else {
	  if(uio_addiov(auio,CAST_USER_ADDR_T(buf),sizeof(buf))!=0 ||
	      VNOP_READ(vp,auio,0,ctx)!=0) printf("X9 PREF: read failed\n");
	  else {
	   user_ssize_t left=uio_resid(auio);
	   got=left<0?0:sizeof(buf)-(size_t)left;
	  }
	  uio_free(auio);
	  for(size_t start=0;start<got;start+=96) {
	   char line[100]; size_t n=0;
	   for(size_t i=start;i<got&&i<start+96;i++)
	    line[n++]=(buf[i]>=0x20&&buf[i]<0x7f)?buf[i]:'.';
	   line[n]=0;
	   printf("X9 PREF +%lu: %s\n",(unsigned long)start,line);
	  }
	 }
	}
 vnode_put(vp);
}
static void
x9_prefcheck(int dump_bytes) {
 int enabled=1;
 (void)PE_parse_boot_argn("x9_prefcheck",&enabled,sizeof(enabled));
 if(!enabled) return;
 /* 42.txt: the live keyboard preferences went from our 540-byte XML seed to a
  * 224-byte file at 50 s and only the first pass was byte-dumped, so what
  * cfprefsd actually kept is still unknown. Dump the live files on every pass
  * (each is under 1 KB) and the backups only once. */
 x9_prefdump("/private/var/mobile/Library/Preferences/com.apple.assistant.support.plist",1);
 x9_prefdump("/private/var/mobile/Library/Preferences/com.apple.assistant.support.x9-before-seed.plist",dump_bytes);
 /* 41.txt: the keyboard preference file is a 767-byte bplist00, so rev252s
  * declined to seed it and the app main thread kept recomputing
  * DictationLanguagesEnabled every frame. rev252t replaces that file (hard-linked
  * backup first), so dump both: the live copy proves the XML seed survived and
  * the .x9-before-seed copy carries the original binary contents for reading the
  * keys UIKit had stored. */
 x9_prefdump("/private/var/mobile/Library/Preferences/com.apple.keyboard.preferences.plist",1);
 x9_prefdump("/private/var/mobile/Library/Preferences/com.apple.keyboard.preferences.x9-before-seed.plist",dump_bytes);
}
/* Route B needs the live launchd.plist byte layout before anything may rewrite
 * it in place (recovery has no hfsplus, so no dd by name; the recorded v5
 * offset is stale after the v8/v13 plistlib rewrites). Read-only probe:
 * size, bplist magic, trailer fields and every occurrence of the Siri/
 * assistant job strings with a short escaped context. x9_xpcscan=1 enables. */
static const char *const x9_xpc_needles[] = {
 "com.apple.assistantd", "com.apple.siriactionsd", "com.apple.SiriTTSService",
 "SiriTTSSynthesizer", "MacinTalkAUSP", "KonaSynthesizer", "/usr/libexec/assistantd"
};
#define X9_XPC_NEEDLES (sizeof(x9_xpc_needles)/sizeof(x9_xpc_needles[0]))
static void
x9_xpcescape(char *out, size_t cap, const unsigned char *p, size_t n)
{
 size_t o = 0;
 for (size_t i = 0; i < n && o + 1 < cap; i++) {
  out[o++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
 }
 out[o] = 0;
}
static void
x9_xpcscan(void)
{
 int enabled = 0;
 static const char path[] = "/System/Library/xpc/launchd.plist";
 vnode_t vp = NULLVP;
 vfs_context_t ctx = vfs_context_kernel();
 struct vnode_attr va;
 uint64_t size = 0;
 size_t cap = 2u << 20, got = 0;
 char *buf;
 errno_t error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);

 if (!(PE_parse_boot_argn("x9_xpcscan", &enabled, sizeof(enabled)) && enabled)) {
  return;
 }
 if (error) {
  printf("X9 XPC: lookup errno=%d\n", error);
  return;
 }
 VATTR_INIT(&va);
 VATTR_WANTED(&va, va_data_size);
 if (vnode_getattr(vp, &va, ctx) == 0) {
  size = va.va_data_size;
 }
 buf = kalloc_data(cap, Z_WAITOK);
 if (buf == NULL) {
  printf("X9 XPC: no memory for %lu bytes\n", (unsigned long)cap);
  vnode_put(vp);
  return;
 }
 while (got < cap && (uint64_t)got < size) {
  size_t want = cap - got, r = 0;
  uio_t auio;
  if (want > (256u << 10)) {
   want = 256u << 10;
  }
  auio = uio_create(1, (off_t)got, UIO_SYSSPACE, UIO_READ);
  if (auio == NULL) {
   break;
  }
  if (uio_addiov(auio, CAST_USER_ADDR_T(buf + got), want) == 0 &&
      VNOP_READ(vp, auio, 0, ctx) == 0) {
   user_ssize_t left = uio_resid(auio);
   if (left >= 0 && (size_t)left <= want) {
    r = want - (size_t)left;
   }
  }
  uio_free(auio);
  if (r == 0) {
   break;
  }
  got += r;
 }
 printf("X9 XPC: %s size=%llu got=%lu\n", path,
     (unsigned long long)size, (unsigned long)got);
 if (got >= 8) {
  char magic[9];
  memcpy(magic, buf, 8);
  magic[8] = 0;
  printf("X9 XPC: magic=%s head=%02x %02x %02x %02x %02x %02x\n", magic,
      (unsigned char)buf[8], (unsigned char)buf[9], (unsigned char)buf[10],
      (unsigned char)buf[11], (unsigned char)buf[12], (unsigned char)buf[13]);
 }
 if (got >= 32) {
  const unsigned char *t = (const unsigned char *)buf + got - 32;
  uint64_t num = 0, top = 0, table = 0;
  for (int i = 0; i < 8; i++) {
   num = (num << 8) | t[8 + i];
   top = (top << 8) | t[16 + i];
   table = (table << 8) | t[24 + i];
  }
  printf("X9 XPC: trailer offSize=%u refSize=%u numObjects=%llu top=%llu table=%llu\n",
      (unsigned)t[6], (unsigned)t[7], (unsigned long long)num,
      (unsigned long long)top, (unsigned long long)table);
 }
 for (size_t k = 0; k < X9_XPC_NEEDLES; k++) {
  const char *needle = x9_xpc_needles[k];
  size_t n = strlen(needle);
  unsigned count = 0, shown = 0;
  for (size_t i = 0; i + n <= got; i++) {
   if (buf[i] != needle[0] || memcmp(buf + i, needle, n) != 0) {
    continue;
   }
   count++;
   if (shown < 4) {
    size_t pre = i < 16 ? i : 16;
    size_t post = (got - i - n) < 16 ? (size_t)(got - i - n) : 16;
    char before[20], after[20];
    x9_xpcescape(before, sizeof(before), (const unsigned char *)buf + i - pre, pre);
    x9_xpcescape(after, sizeof(after), (const unsigned char *)buf + i + n, post);
    printf("X9 XPC: %lu #%u off=%lu ctx=[%s]%s[%s]\n", (unsigned long)k, count,
        (unsigned long)i, before, needle, after);
    shown++;
   }
  }
  printf("X9 XPC: needle %lu \"%s\" count=%u\n", (unsigned long)k, needle, count);
 }
 kfree_data(buf, cap);
 vnode_put(vp);
}
/* x9_thread_sample=1: 60 s, then every 120 s, 4 samples. Bare token x9bl
 * (short sampler for diagnostics; AppleARMBacklight is default-on now):
 * 40 s, then every 15 s, 6 samples, to catch backboardd while it stalls. */
static unsigned sample_delay=60, sample_interval=120, sample_count=4;
static void sample_thread(__unused void *arg, __unused wait_result_t wr) {
 static int chan;
 printf("X9 SAMPLE: enabled delay=%u interval=%u count=%u targets=%s\n",
  sample_delay,sample_interval,sample_count,sample_names[0]?sample_names:"SpringBoard,backboardd,wifid,lsd,datamigrator,migrationplugin");
 (void)tsleep(&chan,PZERO,"x9sample",(int)(sample_delay*hz));
 for(unsigned i=0;i<sample_count;i++) {
  x9_prefcheck(i==0);
  if(i==0)x9_xpcscan();
  printf("X9 SAMPLE: begin %u\n",i+1);
  proc_iterate(PROC_ALLPROCLIST,sample_proc,NULL,NULL,NULL);
  printf("X9 SAMPLE: end %u\n",i+1);
  if(i+1<sample_count)(void)tsleep(&chan,PZERO,"x9sample",(int)(sample_interval*hz));
 }
}
void x9_sample_start(void) {
 int enabled=0, bl=0; thread_t thread;
 bool on=PE_parse_boot_argn("x9_thread_sample",&enabled,sizeof(enabled)) && enabled==1;
 if(PE_parse_boot_argn("x9bl",&bl,sizeof(bl)) && bl!=0) {
  on=true; sample_delay=40; sample_interval=15; sample_count=6;
 }
 if(PE_parse_boot_argn("x9_sample_procs",sample_names,sizeof(sample_names)) && sample_names[0]) {
  on=true; sample_delay=30; sample_interval=10; sample_count=12;
  /* rev252hj: x9sd / x9si (seconds) / x9sn override the defaults. */
  int v=0;
  if(PE_parse_boot_argn("x9sd",&v,sizeof(v)) && v>=1 && v<=600)sample_delay=v;
  if(PE_parse_boot_argn("x9si",&v,sizeof(v)) && v>=1 && v<=600)sample_interval=v;
  if(PE_parse_boot_argn("x9sn",&v,sizeof(v)) && v>=1 && v<=60)sample_count=v;
 } else sample_names[0]=0;
 if(!on)return;
 if(kernel_thread_start(sample_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 else printf("X9 SAMPLE: failed to start\n");
}
