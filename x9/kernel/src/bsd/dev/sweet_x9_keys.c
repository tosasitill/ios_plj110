/* MT6993 / MT6661 SID4 buttons. Captured DT mt6661keys power/home/home2
 * maps to Linux 116/115/114. Stock mtk-spmi-pmic.c acknowledges W1C
 * PSC_TOP_INT_STATUS0; core.h IRQ64..69 are three presses then releases.
 * PMIF-M SWINF2, serialized with RTC/modem/camera through X9PMIF.
 * Never touch the touch driver's PMIF-P.
 * No long-press reset, regulator, pinmux or global interrupt writes. */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/reboot.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOPlatformExpert.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *name, int def);
extern int reboot_kernel(int, char *);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
extern void sweet_hid_key(int, int);
extern kern_return_t thread_terminate(thread_t);
static vm_offset_t key_pmif;
static unsigned key_down;

#define X9_BOOTLOADER_MAGIC 3u
#define X9_WATCHDOG_REBOOT_MODE_OFF 0x24u
#define X9_WATCHDOG_SWRST_OFF 0x14u
#define X9_WATCHDOG_MODE_KEY 0x22000000u
#define X9_WATCHDOG_MODE_IRQ_EN (1u << 3)
#define X9_WATCHDOG_SWRST_KEY 0x1209u
static vm_offset_t reboot_mode_mmio;
static int reboot_bootloader_armed;
static int (*prior_halt_restart)(unsigned int);
/* x9_fb_on_reset=1 (debug): every reset of this boot lands in fastboot.
 * 0x24 is pre-armed at init, so a silent TOPRGU watchdog reset carries the
 * bootloader mode too, and a panic restart takes the same PSCI path as an
 * orderly bootloader reboot instead of the plain watchdog reset. */
static int reboot_fb_on_reset;

static uint32_t reboot_mode_read(void)
{
 return *(volatile uint32_t *)(reboot_mode_mmio + X9_WATCHDOG_REBOOT_MODE_OFF);
}

static void reboot_mode_write(uint32_t value)
{
 *(volatile uint32_t *)(reboot_mode_mmio + X9_WATCHDOG_REBOOT_MODE_OFF) = value;
 __asm__ volatile("dsb sy" ::: "memory");
}

static void reboot_watchdog_reset(void)
{
 uint32_t mode=*(volatile uint32_t *)reboot_mode_mmio;
 *(volatile uint32_t *)reboot_mode_mmio=(mode & ~X9_WATCHDOG_MODE_IRQ_EN) |
     X9_WATCHDOG_MODE_KEY;
 __asm__ volatile("dsb sy" ::: "memory");
 for (;;) {
  *(volatile uint32_t *)(reboot_mode_mmio + X9_WATCHDOG_SWRST_OFF)=X9_WATCHDOG_SWRST_KEY;
  __asm__ volatile("dsb sy" ::: "memory");
  delay(5);
 }
}

/* OPPO MT6993 Android (mt6993.dts reboot-mode on toprgu 0x24, mask 0xf;
 * aee/hangdet.c WDT_NONRST_REG2=0x24: stage bits 29-31, KERNEL=3, reboot
 * flag bit 11 cleared by the restart notifier before every orderly reset).
 * Android then resets through PSCI SYSTEM_RESET (priority 129 > mtk_wdt 128);
 * a TOPRGU SWRST is the abnormal-reset path LK classifies via these bits. */
static uint32_t reboot_mode_value(uint32_t before)
{
 return (before & ~(0xfu | (1u << 11) | (7u << 29))) |
     X9_BOOTLOADER_MAGIC | (3u << 29);
}

static int reboot_halt_restart(unsigned int type)
{
 bool panic_restart = type == kPEPanicRestartCPU ||
     type == kPEPanicRestartCPUNoCallouts;
 if ((type == kPERestartCPU && __atomic_load_n(&reboot_bootloader_armed,
     __ATOMIC_ACQUIRE)) || (panic_restart && reboot_fb_on_reset)) {
  uint32_t before = reboot_mode_read();
  uint32_t wanted = reboot_mode_value(before);
  reboot_mode_write(wanted);
  uint32_t actual = reboot_mode_read();
  printf("X9 BOOTLOADER: 1c010024 %08x -> %08x (read %08x); PSCI SYSTEM_RESET type=%u\n",
      before, wanted, actual, type);
  __asm__ volatile("dsb sy" ::: "memory");
  {
   register uint64_t x0 __asm__("x0") = 0x84000009u;
   __asm__ volatile("smc #0" : "+r"(x0) ::
       "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
       "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  printf("X9 BOOTLOADER: PSCI SYSTEM_RESET returned; watchdog fallback\n");
  reboot_watchdog_reset();
 }
 return prior_halt_restart ? prior_halt_restart(type) : -1;
}

static void reboot_install_hook(void)
{
 if (PE_halt_restart != reboot_halt_restart) {
  prior_halt_restart = PE_halt_restart;
  PE_halt_restart = reboot_halt_restart;
 }
}

static void reboot_request_worker(__unused void *arg, __unused wait_result_t result)
{
 /* Let the USB status stage finish before normal shutdown tears down EP0. */
 delay(100);
 int error=reboot_kernel(RB_AUTOBOOT,NULL);
 __atomic_store_n(&reboot_bootloader_armed,0,__ATOMIC_RELEASE);
 printf("X9 BOOTLOADER: shutdown request returned %d\n",error);
 (void)thread_terminate(current_thread());
}

int sweet_x9_reboot_bootloader_request(void)
{
 if (!sweet_early_is_x9() || !reboot_mode_mmio) {
  return ENXIO;
 }
 reboot_install_hook();
 if (__atomic_exchange_n(&reboot_bootloader_armed,1,__ATOMIC_ACQ_REL)) {
  return EBUSY;
 }
 {
  uint32_t now=reboot_mode_read();
  printf("X9 BOOTLOADER: armed; 1c010024=%08x (mode=%u flag11=%u stage=%u) will write %08x; syncing and shutting down\n",
      now, now & 0xfu, (now >> 11) & 1u, now >> 29, reboot_mode_value(now));
 }
 thread_t t;
 if(kernel_thread_start(reboot_request_worker,NULL,&t)!=KERN_SUCCESS) {
  __atomic_store_n(&reboot_bootloader_armed,0,__ATOMIC_RELEASE);
  return ENOMEM;
 }
 thread_deallocate(t);
 return 0;
}

static int sysctl_x9_reboot_bootloader(__unused struct sysctl_oid *oidp,
    __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
 int value = 0;
 int error = sysctl_handle_int(oidp, &value, 0, req);
 if (error || !req->newptr) {
  return error;
 }
 if (value != 1) {
  return EINVAL;
 }
 return sweet_x9_reboot_bootloader_request();
}
SYSCTL_PROC(_debug, OID_AUTO, x9_reboot_bootloader,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED, 0, 0,
    sysctl_x9_reboot_bootloader, "I", "X9 PLJ110: reboot to MT6993 fastboot");
static uint32_t kr(unsigned off) { return *(volatile uint32_t *)(key_pmif+off); }
static void kw(unsigned off,uint32_t v) {
 *(volatile uint32_t *)(key_pmif+off)=v;
 __asm__ volatile("dsb sy" ::: "memory");
}
static int wait_state(unsigned state) {
 for(unsigned n=0;n<1000;n++) {
  uint32_t v=kr(0x8a8);
  if(v&(1u<<18))return -1;
  if(((v>>1)&7)==state)return 0;
  delay(1);
 }
 return -1;
}
static int key_read(unsigned reg,uint8_t *value) {
 if(reg!=8&&reg!=0x23&&reg!=0x90f&&reg!=0x91b)return -1;
 if(X9PMIFAcquire())return -1;
 int e=wait_state(0);
 if(!e) {
  kw(0x880,(3u<<30)|(4u<<24)|reg);e=wait_state(6);
  if(!e){*value=(uint8_t)kr(0x894);kw(0x8a4,1);e=wait_state(0);}
 }
 X9PMIFRelease();return e;
}
static int key_write(unsigned reg,uint8_t value) {
 /* Only enable register RMW and the six W1C event bits. */
 if(reg!=0x90f&&reg!=0x91b)return -1;
 if(reg==0x91b&&(value&~0x3f))return -1;
 if(X9PMIFAcquire())return -1;
 int e=wait_state(0);
 if(!e){kw(0x884,value);kw(0x880,(3u<<30)|(1u<<29)|(4u<<24)|reg);e=wait_state(0);}
 X9PMIFRelease();return e;
}
static void emit(unsigned key,int down) {
 unsigned bit=1u<<key;
 if(!!(key_down&bit)==!!down)return;
 if(down)key_down|=bit;else key_down&=~bit;
 sweet_hid_key((int)key,down);
 printf("X9 KEYS: key=%u %s HID dispatched\n",key,down?"down":"up");
}
static void run(void) {
 uint8_t id=0,en=0,events=0,raw=0;
 key_pmif=ml_io_map(0x1c01a000,0x1000);
 if(!key_pmif)return;
 printf("X9 KEYS: PMIF-M init=%08x sta=%08x SID4\n",kr(0),kr(0x8a8));
 if(!(kr(0)&1)||key_read(8,&id)|| (id!=0x10&&id!=0x20)) {
  printf("X9 KEYS: unavailable HWCID=%02x; stopped\n",id);return;
 }
 if(key_read(0x90f,&en)||key_read(0x91b,&events)||key_read(0x23,&raw))return;
 printf("X9 KEYS: HWCID=%02x enable=%02x pending=%02x topstatus=%02x\n",id,en,events,raw);
 /* Discard events predating ownership, including the boot key. */
 if(key_write(0x91b,events&0x3f)||key_write(0x90f,en|0x3f)||key_read(0x90f,&en)||(en&0x3f)!=0x3f) {
  printf("X9 KEYS: enable/ack failed; stopped\n");return;
 }
 printf("X9 KEYS: ready power/volume-up/volume-down, latched IRQ polling 8ms\n");
 for(;;) {
  if(key_read(0x91b,&events))break;
  events&=0x3f;
  if(events) {
   /* Acknowledge captured edges before dispatch, do not clear unrelated bits. */
   if(key_write(0x91b,events))break;
   printf("X9 KEYS: edges=%02x previous=%x\n",events,key_down);
   for(unsigned k=0;k<3;k++) {
    unsigned press=events&(1u<<k),release=events&(1u<<(k+3));
    if(press&&release) {
     /* Latches contain no ordering. Do not invent a long hold: report a
      * complete short tap from released state, or release a held key. */
     printf("X9 KEYS: coalesced edges key=%u; ordering unavailable\n",k);
     if(!(key_down&(1u<<k)))emit(k,1);
     emit(k,0);
    }else if(press)emit(k,1);else if(release)emit(k,0);
   }
  }
  uint64_t deadline;clock_interval_to_deadline(8,NSEC_PER_MSEC,&deadline);
  (void)assert_wait_deadline(&key_down,THREAD_UNINT,deadline);
  thread_block(THREAD_CONTINUE_NULL);
 }
 for(unsigned k=0;k<3;k++)emit(k,0);
 printf("X9 KEYS: transport failed sta=%08x; all held keys released, stopped\n",kr(0x8a8));
}
static void worker(void *arg,wait_result_t result) {
 (void)arg;(void)result;run();(void)thread_terminate(current_thread());
}
void sweet_x9_keys_init(void) {
 static int started;
 if(!sweet_early_is_x9()||started)return;
 started=1;
 if(!reboot_mode_mmio)reboot_mode_mmio=ml_io_map(0x1c010000,0x1000);
 if(reboot_mode_mmio) {
  reboot_install_hook();
  printf("X9 BOOTLOADER: watchdog mode mapped at %lx; root sysctl debug.x9_reboot_bootloader ready\n",
      (unsigned long)reboot_mode_mmio);
  if(sweet_x9_arg("x9_fb_on_reset",0)==1) {
   uint32_t before=reboot_mode_read(),wanted=reboot_mode_value(before);
   reboot_mode_write(wanted);
   reboot_fb_on_reset=1;
   printf("X9 BOOTLOADER: x9_fb_on_reset: 1c010024 %08x -> %08x (read %08x); panic/WDT reset goes to fastboot\n",
       before,wanted,reboot_mode_read());
  }
 } else printf("X9 BOOTLOADER: watchdog mapping failed; sysctl unavailable\n");
 if(sweet_x9_arg("x9_pmic_keys",1)!=1)return;
 thread_t t;
 if(kernel_thread_start(worker,NULL,&t)==KERN_SUCCESS)thread_deallocate(t);
 else printf("X9 KEYS: worker creation failed\n");
}