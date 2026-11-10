/* MT6993 / MT6661 SID4 buttons. Captured DT mt6661keys power/home/home2
 * maps to Linux 116/115/114. Stock mtk-spmi-pmic.c acknowledges W1C
 * PSC_TOP_INT_STATUS0; core.h IRQ64..69 are three presses then releases.
 * PMIF-M SWINF2, serialized with RTC/modem/camera through X9PMIF.
 * Never touch the touch driver's PMIF-P.
 * No long-press reset, regulator, pinmux or global interrupt writes. */
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *name, int def);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
extern void sweet_hid_key(int, int);
extern kern_return_t thread_terminate(thread_t);
static vm_offset_t key_pmif;
static unsigned key_down;
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
 int enabled=0;
 if(!sweet_early_is_x9()||started||(enabled=sweet_x9_arg("x9_pmic_keys",1))!=1)return;
 started=1;thread_t t;
 if(kernel_thread_start(worker,NULL,&t)==KERN_SUCCESS)thread_deallocate(t);
 else printf("X9 KEYS: worker creation failed\n");
}
