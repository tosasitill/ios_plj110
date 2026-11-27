extern void sweet_x9_render_bootstrap(void);
/* MT6993 one-shot GPUEB mailbox power/ID bring-up. Source contract:
 * gpufreq_v2.c / gpufreq_ipi.h; gpueb_ipi.c; mtk-mbox.c; captured PLJ110 DT.
 * Uses an already resumed GPUEB only. No GHPM reset, rail/clock guessing,
 * OPP changes, shader job submission or display changes. The CSF extension
 * performs its own bounded MCU boot/stop under the retained power reference.
 * All writes require this boot's shim-validated ADT contract and boot opt-in.
 */
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <kern/cpu_number.h>
#include <machine/machine_routines.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <stddef.h>
#include "gpu_shared_prefix.h"
#include "x9_sched_policy.h"
#include "x9_cpuoff_policy.h"
#include "x9_ufs/ufs_platform.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *name, int def);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_x9_csf_prepare(void);
extern int sweet_x9_csf_boot(vm_offset_t);
extern int sweet_x9_csf_user(vm_offset_t,void *);
extern void sweet_x9_compute_publish(void);

#define ACK 0xc0000000u
#define CHANNEL_BIT 2u
#define CMD_INIT 0u
#define CMD_GET_FREQ_BY_IDX 1u
#define CMD_POWER 6u
#define CMD_ACTIVE 7u
#define CMD_DUAL_COMMIT 9u
#define TARGET_GPU 1u
#define SHARED_PA 0xf1080000ULL
#define SHARED_SIZE 0x4000u
struct gpu_ipi {
 uint32_t magic,cmd,target,padding;
 union {int32_t result;int32_t oppidx;uint32_t freq;uint32_t power;struct {uint64_t base;uint32_t size,pad;} mem;
  struct {int32_t gpu_oppidx,stack_oppidx;} dual;} u;
};
_Static_assert(sizeof(struct gpu_ipi)==32,"GPUEB IPI size");
_Static_assert(offsetof(struct gpu_ipi,u)==16,"GPUEB union offset");
_Static_assert(offsetof(struct gpu_shared_prefix,power_count)==28,"GPUEB power count");
_Static_assert(offsetof(struct gpu_shared_prefix,power_control)==204,"GPUEB power control");
_Static_assert(offsetof(struct gpu_shared_prefix,active_sleep_control)==208,"GPUEB active control");
static vm_offset_t spm,rpc,gpr,mbox,ctrl,shared;
static uint32_t magic;
static unsigned wait_cookie;
static int channel_uncertain;
static uint32_t rd(vm_offset_t b,unsigned off){return *(volatile uint32_t *)(b+off);}
static void wr(vm_offset_t b,unsigned off,uint32_t value){
 *(volatile uint32_t *)(b+off)=value;__asm__ volatile("dsb sy" ::: "memory");
}
static void pause_ms(void) {
 uint64_t until;clock_interval_to_deadline(1,NSEC_PER_MSEC,&until);
 (void)assert_wait_deadline(&wait_cookie,THREAD_UNINT,until);
 thread_block(THREAD_CONTINUE_NULL);
}
static int expired(uint64_t end){return mach_absolute_time()>=end;}
static int on(uint32_t value){return value!=0xffffffffu&&(value&ACK)==ACK;}
static int eb_ready(void) {
 return on(rd(spm,0xe1c))&&on(rd(rpc,0x504))&&rd(gpr,40)==1;
}
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p){return le32(p)|(uint64_t)le32(p+4)<<32;}
static int handoff(void) {
 DTEntry chosen;const void *property;unsigned size=0;
 if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess||
    SecureDTGetProperty(chosen,"x9-gpu-handoff",&property,&size)!=kSuccess||size!=64)return 0;
 const uint8_t *p=property;
 return le32(p)==3&&le32(p+4)==1&&le64(p+8)==SHARED_PA&&le64(p+16)==0x180000&&
  le32(p+24)==SHARED_SIZE&&le32(p+28)==4&&le32(p+32)==55&&le32(p+36)==8&&
  le64(p+40)==0x4b09fd80&&le64(p+48)==0x4b170000&&le64(p+56)==0x4b09fc80;
}
static void snapshot(const char *stage) {
 printf("X9 GPU: %s MFG0=%08x MFG1=%08x MFG2=%08x MFG37=%08x GPR10=%08x send=%08x recv=%08x\n",
  stage,rd(rpc,0x504),rd(rpc,0x500),rd(rpc,0x508),rd(rpc,0x594),rd(gpr,40),rd(ctrl,0),rd(ctrl,0x78));
}
/* A single transmit and bounded reply wait, no resending or stale-ACK discard.
 * Hardware clear-before-read matches mtk_mbox_polling, ACK permits sender reuse.
 * The firmware reply's command must match; reply magic is logged, not assumed
 * to echo request magic (the AP driver does not require an echo). */
static int ipi_quiet;
static int exchange_unlocked(struct gpu_ipi *request,struct gpu_ipi *reply);
/* The DVFS governor thread and the power/ID requests share one mailbox
 * channel: serialize every exchange. Governor commits set ipi_quiet. */
static LCK_GRP_DECLARE(x9_gpu_ipi_grp,"x9_gpu_ipi");
static LCK_MTX_DECLARE(x9_gpu_ipi_lock,&x9_gpu_ipi_grp);
static int exchange(struct gpu_ipi *request,struct gpu_ipi *reply) {
 lck_mtx_lock(&x9_gpu_ipi_lock);
 int r=exchange_unlocked(request,reply);
 lck_mtx_unlock(&x9_gpu_ipi_lock);
 return r;
}
static int exchange_unlocked(struct gpu_ipi *request,struct gpu_ipi *reply) {
 uint64_t until;clock_interval_to_deadline(10000,NSEC_PER_MSEC,&until);
 if(channel_uncertain||!eb_ready())return -1;
 if((rd(ctrl,0)|rd(ctrl,0x78))&CHANNEL_BIT){
  printf("X9 GPU: command=%u channel1 not idle; no transmit\n",request->cmd);return -1;
 }
 request->magic=magic;
 const uint32_t *tx=(const uint32_t *)request;
 for(unsigned i=0;i<8;i++)wr(mbox,16+i*4,tx[i]);
 if(!ipi_quiet)printf("X9 GPU: TX command=%u target=%u union=%08x,%08x,%08x,%08x\n",
  request->cmd,request->target,tx[4],tx[5],tx[6],tx[7]);
 /* W1S channel1; never touch other send/receive bits. */
 wr(ctrl,4,CHANNEL_BIT);
 channel_uncertain=1;
 unsigned ticks=0;
 for(;;) {
  uint32_t received=rd(ctrl,0x78);
  if(received==0xffffffffu)break;
  if(received&CHANNEL_BIT) {
   __asm__ volatile("dsb sy" ::: "memory");
   wr(ctrl,0x74,CHANNEL_BIT);
   uint32_t *rx=(uint32_t *)reply;
   for(unsigned i=0;i<8;i++)rx[i]=rd(mbox,220+i*4);
   __asm__ volatile("dsb sy" ::: "memory");
   if(!ipi_quiet)printf("X9 GPU: RX words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
    rx[0],rx[1],rx[2],rx[3],rx[4],rx[5],rx[6],rx[7]);
   if(reply->cmd!=request->cmd) {
    printf("X9 GPU: reply command mismatch expected=%u actual=%u; channel abandoned\n",request->cmd,reply->cmd);return -1;
   }
   while(rd(ctrl,0)&CHANNEL_BIT) {
    if(expired(until))goto failed;
    pause_ms();
   }
   channel_uncertain=0;return 0;
  }
  if(expired(until))break;
  if(++ticks%1000==0) {
   printf("X9 GPU: waiting command=%u send=%08x recv=%08x\n",request->cmd,rd(ctrl,0),received);
   if(!eb_ready())break;
  }
  pause_ms();
 }
failed:
 printf("X9 GPU: command=%u completion timeout/state failure; no retry and no speculative rollback\n",request->cmd);
 snapshot("failed");return -1;
}
static void status(struct gpu_shared_prefix *s) {
 uint32_t *p=(uint32_t *)s;
 for(unsigned i=0;i<sizeof(*s)/4;i++)p[i]=rd(shared,i*4);
 __asm__ volatile("dsb sy" ::: "memory");
}
static void log_status(const char *stage,const struct gpu_shared_prefix *s) {
 printf("X9 GPU: shared %s magic=%08x opp=%d/%d power=%d active=%d buck=%d mtcmos=%d cg=%d controls=%u/%u freq=%u/%u volt=%u/%u shaders=%08x\n",
  stage,(unsigned)s->magic,s->opp_num_gpu,s->opp_num_stack,s->power_count,s->active_count,
  s->buck_count,s->mtcmos_count,s->cg_count,s->power_control,s->active_sleep_control,
  s->cur_fgpu,s->cur_fstack,s->cur_vgpu,s->cur_vstack,s->shader_present);
}
static int valid_status(const struct gpu_shared_prefix *s) {
 return s->opp_num_gpu>0&&s->opp_num_gpu<=256&&s->opp_num_stack>=0&&s->opp_num_stack<=256&&
  s->power_count>=0&&s->power_count<=16&&s->active_count>=0&&s->active_count<=16&&
  s->power_control<=1&&s->active_sleep_control<=1;
}
static int power_command(unsigned command,unsigned state,int32_t *count) {
 struct gpu_ipi req={0},resp={0};req.cmd=command;req.u.power=state;
 if(exchange(&req,&resp))return -1;
 *count=resp.u.result;
 printf("X9 GPU: command=%u state=%u firmware-return=%d\n",command,state,*count);
 return *count>=0&&*count<=16?0:-1;
}
/* Nothing on this kernel runs MediaTek's GED DVFS policy, which on Android
 * commits OPPs by load through gpufreq_dual_commit(): the GPU stayed at the
 * boot OPP (390/260 MHz GPU/stack observed) and frames were GPU-bound. Once,
 * with the GPU powered: read every GPU OPP's frequency (CMD_GET_FREQ_BY_IDX,
 * index 0 is the fastest) and dual-commit the fastest at or below a cap, the
 * same index for GPU and stack as their tables have equal length.
 * CMD_DUAL_COMMIT goes through GPUEB's limiter (ceiling/floor), unlike the
 * debug CMD_FIX_*. The software governor and GPUEB both retain thermal caps.
 * x9_gpu_khz=<kHz> sets the ceiling; 0 disables the load governor.
 * Sources: OnePlus/OPPO MT6993 gpufreq_ipi.h and gpufreq_v2.c. */
/* Timer fallback, not a port of Android FPSGO/PELT. OPPO UAG targets 80%
 * load; GED scales frequency from measured work, not touch state. The 900MHz
 * value is an active-render floor, not an idle floor. Thermal policy retains
 * the 85..95C ramp and >=100C slowest cap; GPUEB's own limits win too.
 * x9_gpu_gov=0 keeps the single fixed commit at the floor. */
#define X9_GOV_FLOOR_KHZ 900000u
volatile uint64_t sweet_x9_gpu_busy_ns;
static uint32_t opp_khz[256];static int opp_count,gov_floor=-1,gov_ceiling=-1,gov_slowest=-1,gov_index=-1;
static int render_active_refs=-1;
static unsigned gov_cookie;
static uint64_t gov_events,gov_work;
static int gov_idle_sleep;
static void gov_wake(void);
void sweet_x9_gpu_activity(void);
void sweet_x9_gpu_activity(void) {
 __atomic_fetch_add(&gov_work,1,__ATOMIC_RELEASE);
 if(__atomic_exchange_n(&gov_idle_sleep,0,__ATOMIC_ACQ_REL))gov_wake();
}
static void gov_wake(void) {
 __atomic_fetch_add(&gov_events,1,__ATOMIC_RELEASE);
 thread_wakeup(&gov_cookie);
}
static void gov_sleep_ms(unsigned ms,uint64_t observed,uint64_t work) {
 uint64_t until;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&until);
 (void)assert_wait_deadline(&gov_cookie,THREAD_UNINT,until);
 /* Covers a display event between policy sampling and assert_wait. */
 if(__atomic_load_n(&gov_events,__ATOMIC_ACQUIRE)!=observed||
    (ms>50&&__atomic_load_n(&gov_work,__ATOMIC_ACQUIRE)!=work))
  clear_wait(current_thread(),THREAD_AWAKENED);
 thread_block(THREAD_CONTINUE_NULL);
}
static int gov_commit(int index) {
 struct gpu_ipi req={0},resp={0};req.cmd=CMD_DUAL_COMMIT;req.u.dual.gpu_oppidx=index;req.u.dual.stack_oppidx=index;
 lck_mtx_lock(&x9_gpu_ipi_lock);ipi_quiet=1;
 int r=exchange_unlocked(&req,&resp);
 ipi_quiet=0;lck_mtx_unlock(&x9_gpu_ipi_lock);
 if(!r&&resp.u.result<0)r=-1;
 if(!r)gov_index=index;
 return r;
}
/* DRAM bandwidth (DVFSRC, MT6993). 109.txt: with clock and cache fixed, a
 * backboardd frame is 15-35 full-screen passes (14-33M pixels) and each job
 * still waits 6-15 ms at 1716 MHz - memory-bound. Nothing in this OS votes
 * DRAM up (Android's mtk_gpu_qos/dvfsrc do). Register map: OPPO MT6993
 * kernel_device_modules-6.12 drivers/soc/mediatek/mtk-dvfsrc.c mt6993_regs:
 * BASIC_CONTROL 0x0 (OPP count-1 in [26:20]), SW_REQ 0x18 (DRAM level [15:12],
 * EMI level [3:0]; mt6993_set_dram_level/set_emi_level), LEVEL 0x5F0,
 * CUR_TAR_GEAR 0x6AC (DRAM gear [3:0], vcore [10:8]). DT: 10 DRAM OPPs
 * (opp0 fastest), so request levels 0..9, higher = faster. Read-only unless
 * x9_ddr_level=N / x9_emi_level=N (1..15) asks for a level. */
static vm_offset_t dvfsrc;
static LCK_GRP_DECLARE(x9_dvfsrc_grp,"x9_dvfsrc");
static LCK_MTX_DECLARE(x9_dvfsrc_lock,&x9_dvfsrc_grp);
static unsigned dvfsrc_ready,ufs_vcore_floor;
/* MT6993 regulator selectors: 575/600/650/725/825/950 mV. UFS high clocks
 * require selector2 (650 mV). Own only SW_REQ[6:4]; preserve DRAM/EMI and any
 * stronger inherited AP vote. A one-time high current gear is not a floor. */
static int vcore_ok_locked(uint32_t snapshot[3])
{
 if(!dvfsrc){if(snapshot)memset(snapshot,0,3*sizeof(*snapshot));return 0;}
 uint32_t req=rd(dvfsrc,0x18),cur=rd(dvfsrc,0x6ac),basic=rd(dvfsrc,0);
 if(snapshot){snapshot[0]=basic;snapshot[1]=req;snapshot[2]=cur;}
 if(!dvfsrc_ready||!ufs_vcore_floor)return 0;
 return basic!=0xffffffffu&&(basic&1)&&((basic>>20)&0x7f)>0&&
  req!=0xffffffffu&&cur!=0xffffffffu&&
  ((req>>4)&7)>=ufs_vcore_floor&&((req>>4)&7)<=5&&
  ((cur>>8)&7)>=ufs_vcore_floor&&((cur>>8)&7)<=5;
}
int sweet_x9_vcore_ufs_ok(void)
{
 return sweet_x9_vcore_ufs_check(NULL);
}
int sweet_x9_vcore_ufs_check(uint32_t snapshot[3])
{
 lck_mtx_lock(&x9_dvfsrc_lock);int ok=vcore_ok_locked(snapshot);
 lck_mtx_unlock(&x9_dvfsrc_lock);return ok;
}
int sweet_x9_vcore_ufs_hold(uint32_t snapshot[3])
{
 if(!snapshot)return -1;
 lck_mtx_lock(&x9_dvfsrc_lock);int error=-1;
 if(!dvfsrc_ready)goto done;
 uint32_t req=rd(dvfsrc,0x18),floor=(req>>4)&7;
 if(req==0xffffffffu||floor>5)goto done;
 snapshot[0]=req;if(floor<2)floor=2;
 if(floor<ufs_vcore_floor)floor=ufs_vcore_floor;
 ufs_vcore_floor=floor; /* retain even on failure: never lower an uncertain vote */
 wr(dvfsrc,0x18,(req&~0x70u)|(floor<<4));
 snapshot[1]=rd(dvfsrc,0x18);
 if(snapshot[1]!=((req&~0x70u)|(floor<<4)))goto done;
 /* mt6989_wait_for_vcore_level: bounded current VCORE >= requested. */
 for(unsigned i=0;i<=100;i++) {
  snapshot[2]=rd(dvfsrc,0x6ac);
  if(vcore_ok_locked(NULL)){error=0;break;}
  if(i<100)delay(10);
 }
done:
 lck_mtx_unlock(&x9_dvfsrc_lock);return error;
}
/* 110.txt (rev252db): basic=00000008 opp_count=1 level=0 gear=0 and the
 * SW_REQ write changed nothing - DVFSRC was never started. Linux brings it up
 * through TF-A: mtk_dvfsrc_probe SMC MTK_SIP_VCOREFS_CONTROL (0xC2000506,
 * SMC64 fast SiP 0x506) cmd MTK_SIP_DVFSRC_INIT=0 with dvfsrc-flag/dvfsrc-vmode
 * (absent in the captured DT -> 0,0; a0=0 ok, a1=dram_type). Per
 * mtk-dvfsrc-start.c the INIT stage "locks high opp" until late-init sends
 * MTK_SIP_DVFSRC_START=1, after which DVFSRC free-runs on the votes.
 * x9_dvfsrc=1: INIT only (DRAM held at the high OPP). x9_dvfsrc=2: INIT +
 * START (then x9_ddr_level/x9_emi_level votes apply). With no explicit
 * x9_dvfsrc/dv argument, ug=4/5 defaults to INIT-only as qualified in log180;
 * ug=0 keeps the previous no-INIT default. Explicit DVFSRC settings win. */
static uint64_t dvfsrc_smc(uint64_t cmd,uint64_t a2,uint64_t a3,uint64_t *a1_out) {
 register uint64_t x0 __asm__("x0")=0xc2000506;
 register uint64_t x1 __asm__("x1")=cmd;
 register uint64_t x2 __asm__("x2")=a2;
 register uint64_t x3 __asm__("x3")=a3;
 register uint64_t x4 __asm__("x4")=0;
 register uint64_t x5 __asm__("x5")=0;
 register uint64_t x6 __asm__("x6")=0;
 register uint64_t x7 __asm__("x7")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::"memory");
 if(a1_out)*a1_out=x1;
 return x0;
}
static void dvfsrc_dump(const char *when) {
 uint32_t basic=rd(dvfsrc,0x0),req=rd(dvfsrc,0x18),level=rd(dvfsrc,0x5f0),gear=rd(dvfsrc,0x6ac);
 printf("X9 DDR: DVFSRC %s basic=%08x opp_count=%u sw_req=%08x (dram %u emi %u) level=%08x gear=%08x (dram %u vcore %u)\n",
  when,basic,((basic>>20)&0x7f)+1,req,(req>>12)&0xf,req&0xf,level,gear,gear&0xf,(gear>>8)&7);
}
static void dvfsrc_setup(void) {
 lck_mtx_lock(&x9_dvfsrc_lock);
 dvfsrc=ml_io_map(0x1c100000,0x1000);
 if(!dvfsrc){lck_mtx_unlock(&x9_dvfsrc_lock);printf("X9 DDR: DVFSRC map failed\n");return;}
 dvfsrc_dump("boot");
 int high=sweet_x9_arg("ug",X9U_DEFAULT_HIGH_GEAR);
 uint32_t mode=(high==4||high==5)?1:0;
 /* Legacy name takes priority over the alias, including an explicit zero. */
 if(!PE_parse_boot_argn("x9_dvfsrc",&mode,sizeof(mode)))PE_parse_boot_argn("dv",&mode,sizeof(mode));
 if(mode==1||mode==2) {
  uint64_t a1=0,r=dvfsrc_smc(0,0,0,&a1);
  for(int i=0;i<20;i++)pause_ms();
  printf("X9 DDR: SMC VCOREFS INIT -> a0=%lld dram_type=%llu\n",(long long)r,a1);
  dvfsrc_dump("after-init");
  if(mode==2&&!r) {
   r=dvfsrc_smc(1,0,0,NULL);
   for(int i=0;i<20;i++)pause_ms();
   printf("X9 DDR: SMC VCOREFS START -> a0=%lld\n",(long long)r);
   dvfsrc_dump("after-start");
  }
 }
 uint32_t req=rd(dvfsrc,0x18),gear;
 uint32_t ddr=0,emi=0;
 PE_parse_boot_argn("x9_ddr_level",&ddr,sizeof(ddr));PE_parse_boot_argn("x9_emi_level",&emi,sizeof(emi));
 if((ddr&&ddr<=15)||(emi&&emi<=15)) {
  uint32_t v=req;
  if(ddr&&ddr<=15)v=(v&~(0xfu<<12))|(ddr<<12);
  if(emi&&emi<=15)v=(v&~0xfu)|emi;
  wr(dvfsrc,0x18,v);
  for(int i=0;i<20;i++)pause_ms();
  gear=rd(dvfsrc,0x6ac);
  printf("X9 DDR: SW_REQ %08x -> %08x (x9_ddr_level=%u x9_emi_level=%u) gear now %08x (dram %u vcore %u)\n",
   req,rd(dvfsrc,0x18),ddr,emi,gear,gear&0xf,(gear>>8)&7);
 }
 uint32_t basic=rd(dvfsrc,0);
 dvfsrc_ready=basic!=0xffffffffu&&(basic&1)&&((basic>>20)&0x7f)>0;
 lck_mtx_unlock(&x9_dvfsrc_lock);
}
/*
 * CPU governor / touch floor (x9g bit32). OPPO uad/touch_boost.c defaults:
 * little=1300000kHz, big/prime=0, duration100ms, input interval150ms.
 * These are public driver defaults, not captured ColorOS userspace tuning.
 * Swap rate is NOT a boost source: 330 showed ui_boost=39/39 at 4..18% GPU.
 */
extern int sweet_x9_perf(unsigned bit);
volatile uint64_t sweet_x9_ui_swaps;        /* SweetFramebuffer swap count */
volatile uint64_t sweet_x9_ui_touch_abs;    /* throttled touch-floor start */
/* Display requests define logical sleep even with sweetfb_stayon. Flush the
 * touch lease on BOTH edges so a pre-lock finger cannot boost the wake path.
 * Runtime sleep only suspends the render queues/MCU and retains resources;
 * it does not power off UFS, the modem, SCP, or the whole SoC. */
volatile int sweet_x9_display_dark;
static uint64_t display_epoch;
void sweet_x9_set_display_dark(int dark);
void sweet_x9_set_display_dark(int dark) {
 if(__atomic_exchange_n(&sweet_x9_display_dark,dark?1:0,__ATOMIC_ACQ_REL)==(dark?1:0))return;
 __atomic_store_n(&sweet_x9_ui_touch_abs,0,__ATOMIC_RELAXED);
 __atomic_fetch_add(&display_epoch,1,__ATOMIC_RELEASE);
 gov_wake();
}
void sweet_x9_ui_touch(void) {
 if(__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_ACQUIRE))return;
 const uint64_t now=mach_absolute_time(),epoch=__atomic_load_n(&display_epoch,__ATOMIC_ACQUIRE);
 uint64_t prev=__atomic_load_n(&sweet_x9_ui_touch_abs,__ATOMIC_RELAXED),ns=0;
 if(prev&&now>=prev){absolutetime_to_nanoseconds(now-prev,&ns);if(ns<150000000ull)return;}
 if(!__atomic_compare_exchange_n(&sweet_x9_ui_touch_abs,&prev,now,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return;
 if(epoch!=__atomic_load_n(&display_epoch,__ATOMIC_ACQUIRE)||__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_ACQUIRE)) {
  uint64_t expected=now;
  __atomic_compare_exchange_n(&sweet_x9_ui_touch_abs,&expected,0,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);return;
 }
 gov_wake();
}
static int ui_tick(void) {
 if(__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_ACQUIRE))return 0;
 uint64_t t=__atomic_load_n(&sweet_x9_ui_touch_abs,__ATOMIC_RELAXED),now=mach_absolute_time(),ns=0;
 if(!t||now<t)return 0;
 absolutetime_to_nanoseconds(now-t,&ns);return ns<100000000ull;
}
/*
 * Dimensity 9500 (MT6993) CPU topology, 1+3+4, from the OPPO mt6993.dts cpu
 * nodes (performance-domains <&performance 0/1/2>) and the captured MPIDRs
 * (one DSU cluster, Aff1 = core): cpu0-3 C1-Pro (domain 0), cpu4-6
 * C1-Premium (domain 1), cpu7 C1-Ultra (domain 2). cpufreq-hw domain d is
 * cluster d. The scheduler still sees one SMP cluster: this XNU is built
 * with MAX_CPU_CLUSTERS=1 and no __AMP__ (a second ADT cluster overruns its
 * cluster arrays), so the split is applied here: cpufreq domains, core
 * recommendation (x9g 64) and window-server binding to the prime core
 * (x9g 128). mkadt.py publishes the same map as x9-cluster per cpu node.
 */
enum {X9_LITTLE=0,X9_BIG=1,X9_PRIME=2,X9_NCLUSTER=3};
#define X9_NCPU 8
#define X9_PRIME_CPU 7
static const uint8_t x9_topo_cluster[X9_NCPU]={X9_LITTLE,X9_LITTLE,X9_LITTLE,X9_LITTLE,X9_BIG,X9_BIG,X9_BIG,X9_PRIME};
static const char *const x9_cluster_name[X9_NCLUSTER]={"little","big","prime"};
static const char *const x9_cluster_core[X9_NCLUSTER]={"C1-Pro","C1-Premium","C1-Ultra"};
static const int x9_cluster_probe_cpu[X9_NCLUSTER]={0,4,7};   /* measured per cluster */
static uint32_t x9_topo_mask(unsigned cl){uint32_t m=0;for(unsigned i=0;i<X9_NCPU;i++)if(x9_topo_cluster[i]==cl)m|=1u<<i;return m;}
static void x9_topo_check(void) {
 unsigned adt=0,bad=0,mpidr_bad=0;int first_bad=-1;
 for(unsigned i=0;i<X9_NCPU;i++) {
  char path[16]="/cpus/cpu0";path[9]=(char)('0'+i);
  DTEntry e;const void *p=NULL;unsigned size=0;
  if(SecureDTLookupEntry(NULL,path,&e)!=kSuccess)continue;
  if(SecureDTGetProperty(e,"reg",&p,&size)==kSuccess&&size>=4){uint32_t v;memcpy(&v,p,4);if(v!=(i<<8))mpidr_bad++;}
  if(SecureDTGetProperty(e,"x9-cluster",&p,&size)==kSuccess&&size==4) {
   uint32_t v;memcpy(&v,p,4);adt++;
   if(v!=x9_topo_cluster[i]){bad++;if(first_bad<0)first_bad=(int)i;}
  }
 }
 printf("X9 TOPO: 1+3+4 little=cpu0-3(%s,dom0) big=cpu4-6(%s,dom1) prime=cpu7(%s,dom2) mpidr=%s adt=%s",
  x9_cluster_core[0],x9_cluster_core[1],x9_cluster_core[2],mpidr_bad?"MISMATCH":"ok",
  !adt?"absent":bad?"MISMATCH":"match");
 if(bad)printf(" first_bad=cpu%d",first_bad);
 printf(";scheduler one SMP cluster (MAX_CPU_CLUSTERS=1)\n");
}
extern processor_t thread_bind(processor_t processor);
extern processor_t cpu_to_processor(int cpu);
static unsigned cpu_measure_mhz(int *cpu);
/* Governor thread only (not realtime): run the probe on one core of the cluster. */
static unsigned cpu_measure_cluster(unsigned cl) {
 const int want=x9_cluster_probe_cpu[cl];
 processor_t p=cpu_to_processor(want);
 if(p==PROCESSOR_NULL)return 0;
 thread_bind(p);thread_block(THREAD_CONTINUE_NULL);
 int cpu=-1;unsigned mhz=0;
 if(cpu_number()==want){mhz=cpu_measure_mhz(&cpu);if(cpu!=want)mhz=0;}
 thread_bind(PROCESSOR_NULL);
 return mhz;
}
/*
 * MT6993 CPU DVFS, as the vendor driver does it
 * (kernel_device_modules-6.12 drivers/cpufreq/mediatek-cpufreq-hw_main.c +
 * mediatek-cpufreq-hw_fdvfs.c, mt6993.dts cpuhvfs / performance-controller /
 * fdvfs nodes). Log 309 used the upstream-only path (PERF_STATE index) and
 * the clocks never moved:
 *  - csram_sys @0x11bc00 must read 0x55AA55AA and csram_sys+0x10+0x8c != 0
 *    (cpufreq hardware enabled by MCUPM);
 *  - USRAM @0x51c1400 + 0xbfc = pwr_tbl_ver; 2 => LUT MHz in bits 13:0
 *    (309's prime "114" is 4210 MHz cut to 12 bits);
 *  - fdvfs support word @0x51c1124 == 0xABCD0001 => frequency requests go to
 *    the per-CPU FDVFS registers @0x5150400 + 4*cpu as ceil(kHz / 26000),
 *    PERF_STATE is not used at all;
 *  - otherwise CSRAM+0x4cc != 0 => PERF_STATE takes kHz, else an index.
 * Domains (cpufreq-hw @0x51c0010, 0x120 apart): 0 = cpu0-3, 1 = cpu4-6,
 * 2 = cpu7. MCUPM owns voltage; the kernel only requests frequencies.
 * LUT OPPs feed a load governor; LK values are only the unknown-temperature cap.
 */
#define CPUHW_PA 0x051c0000ULL     /* cpuhvfs CSRAM (reg[1]); USRAM follows at +0x1400 */
#define CSRAM_SYS_PA 0x00118000ULL /* page holding csram_sys @0x11bc00 */
#define FDVFS_PA 0x05150000ULL     /* page holding fdvfs regs @0x5150400 / cci @0x5150430 */
#define FDVFS_MAGIC 0xABCD0001u
#define FDVFS_UNIT_KHZ 26000u
static vm_offset_t cpuhw,csram_sys,fdvfs;
static struct {unsigned n,top,boot,cur,idle,util;uint16_t mhz[32];uint64_t down_since;} cpud[3];
static int cpu_boost_ready,cpu_boosted,cpu_mode;   /* mode 1 = FDVFS, 2 = PERF_STATE kHz, 3 = PERF_STATE index */
static uint32_t fdvfs_boot[X9_NCPU],perf_boot[3];
/* Request state and XNU accumulated CPU-time baseline. */
static int cpu_dark;
static natural_t cpu_ticks[X9_NCPU][CPU_STATE_MAX];
extern void processor_cpu_load_info(processor_t,natural_t [CPU_STATE_MAX]);
static unsigned cpu_opp(unsigned d,unsigned khz) {
 unsigned best=cpud[d].top;
 for(unsigned i=0;i<cpud[d].n;i++)
  if(cpud[d].mhz[i]*1000u>=khz&&cpud[d].mhz[i]<cpud[d].mhz[best])best=i;
 return best;
}
static vm_offset_t cpud_base(unsigned d){return cpuhw+0x10u+d*0x120u;}
static const char *cpu_mode_name(int m){return m==1?"fdvfs":m==2?"perf_state kHz":m==3?"perf_state index":"none";}
static unsigned cpu_measure_mhz(int *cpu) {
 /* 8 dependent adds per iteration: >= 8 cycles each on any core. Log 308:
  * 800k cycles with interrupts off took 1.007 ms on cpu4 (~795 MHz) and hit
  * the 852 us interrupts-disabled panic. Interrupts stay on now: five short
  * runs of 100k cycles, keep the fastest one that stayed on one CPU. */
 unsigned best=0;*cpu=-1;
 for(unsigned r=0;r<5;r++) {
  uint64_t x=0,n=12500;
  const int c0=cpu_number();
  const uint64_t t0=mach_absolute_time();
  __asm__ volatile("1:\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
   " add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n subs %1,%1,#1\n b.ne 1b\n"
   :"+r"(x),"+r"(n)::"cc");
  const uint64_t t1=mach_absolute_time();
  if(cpu_number()!=c0)continue;
  uint64_t ns=0;absolutetime_to_nanoseconds(t1-t0,&ns);
  const unsigned mhz=ns?(unsigned)(100000ull*1000ull/ns):0;
  if(mhz>best){best=mhz;*cpu=c0;}
 }
 return best;
}
static void sched_setup(void);
static void cpu_boost_setup(void) {
 cpuhw=ml_io_map(CPUHW_PA,0x4000);
 csram_sys=ml_io_map(CSRAM_SYS_PA,0x4000);
 fdvfs=ml_io_map(FDVFS_PA,0x4000);
 if(!cpuhw||!csram_sys||!fdvfs){printf("X9 CPUFREQ: map failed csram=%lx sys=%lx fdvfs=%lx\n",(unsigned long)cpuhw,(unsigned long)csram_sys,(unsigned long)fdvfs);return;}
 const uint32_t sys_magic=rd(csram_sys,0x3c00),sys_hw=rd(csram_sys,0x3c00+0x10+0x8c);
 const uint32_t tbl_ver=rd(cpuhw,0x1400+0xbfc),scaling=rd(cpuhw,0x4cc),fd_magic=rd(cpuhw,0x1124);
 const uint32_t lut_mask=tbl_ver==2?0x3fffu:0xfffu;
 printf("X9 CPUFREQ: csram_sys=%08x hw=%x pwr_tbl_ver=%u freq_scaling=%x fdvfs_magic=%08x cci_req=%x\n",
  sys_magic,sys_hw,tbl_ver,scaling,fd_magic,rd(fdvfs,0x430));
 int sane=1;
 for(unsigned d=0;d<3;d++) {
  const vm_offset_t b=cpud_base(d);unsigned prev=0;cpud[d].n=0;cpud[d].top=0;
  for(unsigned i=0;i<32;i++){const unsigned f=rd(b,i*4)&lut_mask;if(!f||f==prev)break;cpud[d].mhz[i]=(uint16_t)f;prev=f;cpud[d].n++;
   if(f>cpud[d].mhz[cpud[d].top])cpud[d].top=i;}
  perf_boot[d]=rd(b,0x88);cpud[d].boot=cpud[d].cur=0;
  printf("X9 CPUFREQ: domain%u(%s %s) opps=%u lut[0]=%u lut[last]=%u MHz top=%u(%u MHz) perf_state=%x enable=%x hw_state=%x latency=%u\n",
   d,x9_cluster_name[d],x9_cluster_core[d],cpud[d].n,cpud[d].n?cpud[d].mhz[0]:0,cpud[d].n?cpud[d].mhz[cpud[d].n-1]:0,
   cpud[d].top,cpud[d].n?cpud[d].mhz[cpud[d].top]:0,perf_boot[d],rd(b,0x84),rd(b,0x8c),rd(b,0x114)&0xffffu);
  if(cpud[d].n<2||cpud[d].mhz[cpud[d].top]<500||cpud[d].mhz[cpud[d].top]>5000)sane=0;
 }
 /* Initial request / unknown-temperature cap = what LK left. Zero becomes
  * ~1200MHz, close to 309. Normal idle uses the domain's lowest >=500MHz OPP. */
 for(unsigned c=0;c<X9_NCPU;c++){fdvfs_boot[c]=rd(fdvfs,0x400+4*c);if(!fdvfs_boot[c])fdvfs_boot[c]=(1200000u+FDVFS_UNIT_KHZ-1u)/FDVFS_UNIT_KHZ;}
 printf("X9 CPUFREQ: fdvfs idle requests cpu0-7 = %u %u %u %u %u %u %u %u (x26 MHz; 0 at boot -> 47)\n",
  fdvfs_boot[0],fdvfs_boot[1],fdvfs_boot[2],fdvfs_boot[3],fdvfs_boot[4],fdvfs_boot[5],fdvfs_boot[6],fdvfs_boot[7]);
 x9_topo_check();
 printf("X9 CPUFREQ: boot clocks little/big/prime ~%u/%u/%u MHz (cpu0/4/7, dependent-add loop)\n",
  cpu_measure_cluster(X9_LITTLE),cpu_measure_cluster(X9_BIG),cpu_measure_cluster(X9_PRIME));
 cpu_mode=fd_magic==FDVFS_MAGIC?1:scaling?2:3;
 if(!sweet_x9_perf(32)){printf("X9 CPUFREQ: UI boost off (x9g bit 32); read-only, mode would be %s\n",cpu_mode_name(cpu_mode));return;}
 if(sys_magic!=0x55AA55AAu||!sys_hw){printf("X9 CPUFREQ: cpufreq hardware not enabled by MCUPM; read-only\n");return;}
 if(!sane){printf("X9 CPUFREQ: LUT not sane; read-only\n");return;}
 for(unsigned d=0;d<3;d++)if((rd(cpud_base(d),0x8c)&3u)!=3u){printf("X9 CPUFREQ: domain%u hw_state=%x (needs cpufreq+SVS); read-only\n",d,rd(cpud_base(d),0x8c));return;}
 for(unsigned d=0;d<X9_NCLUSTER;d++) {
  cpud[d].idle=cpu_opp(d,500000u);
  unsigned c=(unsigned)x9_cluster_probe_cpu[d];
  unsigned khz=cpu_mode==1?fdvfs_boot[c]*FDVFS_UNIT_KHZ:
   cpu_mode==2?perf_boot[d]:perf_boot[d]<cpud[d].n?cpud[d].mhz[perf_boot[d]]*1000u:500000u;
  cpud[d].boot=cpud[d].cur=cpu_opp(d,khz);
 }
 for(unsigned c=0;c<X9_NCPU;c++) {
  processor_t p=cpu_to_processor(c);
  if(p!=PROCESSOR_NULL)processor_cpu_load_info(p,cpu_ticks[c]);
 }
 cpu_boost_ready=1;
 printf("X9 CPUFREQ: load governor via %s target=80%% touch=1300/0/0MHz 100ms/150ms down-hold=100ms idle=%u/%u/%uMHz;no touch-to-max\n",
  cpu_mode_name(cpu_mode),cpud[0].mhz[cpud[0].idle],cpud[1].mhz[cpud[1].idle],cpud[2].mhz[cpud[2].idle]);
 sched_setup();
}
/* XNU accounting is accumulated CPU time, not frequency-invariant PELT.
 * Scale the CURRENT request by busy/target, choose a real LUT OPP, use the
 * busiest CPU of each frequency domain (not the average of all eight).
 * Background work remains load-governed while dark. No frequency probe loop
 * is run periodically: that used to wake/bind 3 CPUs every two seconds. */
static unsigned cpu_govern(int touch,int temp,int dark) {
 unsigned util[X9_NCLUSTER]={0,0,0},peak=0;
 for(unsigned c=0;c<X9_NCPU;c++) {
  processor_t p=cpu_to_processor(c);natural_t ticks[CPU_STATE_MAX]={0};
  if(p==PROCESSOR_NULL)continue;
  processor_cpu_load_info(p,ticks);
  uint64_t busy=(natural_t)(ticks[CPU_STATE_USER]-cpu_ticks[c][CPU_STATE_USER]);
  busy+=(natural_t)(ticks[CPU_STATE_SYSTEM]-cpu_ticks[c][CPU_STATE_SYSTEM]);
  uint64_t idle=(natural_t)(ticks[CPU_STATE_IDLE]-cpu_ticks[c][CPU_STATE_IDLE]);
  memcpy(cpu_ticks[c],ticks,sizeof(ticks));
  unsigned u=busy+idle?(unsigned)(busy*100/(busy+idle)):0;
  unsigned d=x9_topo_cluster[c];if(u>util[d])util[d]=u;if(u>peak)peak=u;
 }
 if(!cpu_boost_ready)return peak;
 uint64_t now=mach_absolute_time();const int lease_expired=cpu_boosted&&!touch;cpu_boosted=0;
 if(dark!=cpu_dark)printf("X9 CPUFREQ: display %s; touch floor cleared, background load stays governed\n",dark?"dark":"lit");
 cpu_dark=dark;
 for(unsigned d=0;d<X9_NCLUSTER;d++) {
  cpud[d].util=util[d];
  unsigned cur=cpud[d].cur,khz=cpud[d].mhz[cur]*1000u;
  unsigned wanted=(unsigned)((uint64_t)khz*util[d]/80);
  unsigned min=cpud[d].mhz[cpud[d].idle]*1000u;
  unsigned cap=cpud[d].mhz[cpud[d].top]*1000u;
  if(temp<1||temp>150)cap=cpud[d].mhz[cpud[d].boot]*1000u;
  else if(temp>=85)cap=min+(cap-min)*(temp>=95?0:95-temp)/10;
  if(touch&&!dark&&d==X9_LITTLE&&temp>=1&&temp<85&&min<1300000u)min=1300000u;
  if(wanted<min)wanted=min;
  unsigned next=cpu_opp(d,wanted);
  /* Cap by OPP frequency, not by index: CPU LUT ordering is not assumed. */
  if(cpud[d].mhz[next]*1000u>cap) {
   next=cpud[d].idle;
   for(unsigned i=0;i<cpud[d].n;i++)
    if(cpud[d].mhz[i]*1000u<=cap&&cpud[d].mhz[i]>cpud[d].mhz[next])next=i;
  }
  if(cpud[d].mhz[next]<cpud[d].mhz[cur]&&khz<=cap&&!dark&&!(lease_expired&&d==X9_LITTLE)) {
   uint64_t ns=0;
   if(!cpud[d].down_since)cpud[d].down_since=now;
   absolutetime_to_nanoseconds(now-cpud[d].down_since,&ns);
   if(ns<100000000ull)continue;
  }else cpud[d].down_since=0;
  if(next==cur)continue;
  unsigned req=cpud[d].mhz[next]*1000u;
  if(cpu_mode==1) {
   req=(req+FDVFS_UNIT_KHZ-1u)/FDVFS_UNIT_KHZ;
   for(unsigned c=0;c<X9_NCPU;c++)if(x9_topo_cluster[c]==d)wr(fdvfs,0x400+4*c,req);
  }else wr(cpud_base(d),0x88,cpu_mode==2?req:next);
  cpud[d].cur=next;
 }
 cpu_boosted=touch&&!dark&&temp>=1&&temp<85;
 return peak;
}
/*
 * Scheduler policy v2 glue (boot arg x9sched, default 0 = legacy v1
 * behaviour everywhere):
 *   bit 1: cpu_govern2() replaces the v1 per-domain decision with
 *          x9_sched_policy.h (scene targets/floors, peak-decayed util,
 *          70..95C smooth thermal cap, geometric down-steps);
 *   bit 2: scene-driven recommended-cores steering replaces the x9g
 *          bit-64 touch-only rule, and the prime render-thread boost
 *          window also covers the camera scene;
 *   bit 4: CPU/GPU coordination: in load/camera scenes the GPU governor
 *          targets 70% instead of 80% util.
 * Layered on the x9g bit-32 governor: with bit 32 off (or the cpufreq
 * hardware not MCUPM-enabled) everything here is inert, exactly like v1.
 * Observable via the periodic X9 SCHED line and per-domain counters.
 */
static int sched_mask = -1;
static struct x9sched_domain sched_d[X9_NCLUSTER];
static struct x9sched_scene_state sched_scene_state;
static uint32_t sched_scene_residency[X9SCENE_COUNT];
static int sched_scene = X9SCENE_LIGHT;
static unsigned sched_last_peak;
static uint64_t sched_last_swap_count, sched_last_swap_abs;
static volatile int sched_camera_hint;
static int sched_govern2_dark;

static int sched_bit(unsigned bit) {
 if (sched_mask < 0) {
  int v = 0;
  PE_parse_boot_argn("x9sched", &v, sizeof(v));
  sched_mask = v;
 }
 return (sched_mask & (int)bit) != 0;
}

/* Camera capture hint for the scene classifier. No in-tree caller yet:
 * the camera session start/stop in sweet_x9_camera.c is the intended
 * single call site, left unwired here to avoid colliding with the
 * parallel camera work. Until then the camera scene is unreachable and
 * the classifier behaves as if no session ever runs. */
void sweet_x9_sched_camera_active(int active);
void sweet_x9_sched_camera_active(int active) {
 if (__atomic_exchange_n(&sched_camera_hint, active ? 1 : 0,
     __ATOMIC_ACQ_REL) != (active ? 1 : 0)) {
  gov_wake();
 }
}

static void sched_setup(void) {
 if (!sched_bit(1) && !sched_bit(2) && !sched_bit(4)) {
  return;
 }
 for (unsigned d = 0; d < X9_NCLUSTER; d++) {
  struct x9sched_domain *sd = &sched_d[d];
  memset(sd, 0, sizeof(*sd));
  sd->opp_mhz = cpud[d].mhz;
  sd->n_opp = cpud[d].n;
  sd->domain_id = d;
  sd->idle_idx = cpud[d].idle;
  sd->boot_idx = cpud[d].boot;
  sd->cur_idx = cpud[d].cur;
 }
 sched_last_swap_count = __atomic_load_n(&sweet_x9_ui_swaps,
     __ATOMIC_RELAXED);
 printf("X9 SCHED: x9sched=%d cpu_policy2=%d scene_steer=%d gpu_coord=%d "
     "(0 = legacy v1)\n", sched_mask, !!sched_bit(1), !!sched_bit(2),
     !!sched_bit(4));
}

static int sched_update_scene(int dark, int touch, unsigned gpu_util,
    uint64_t now_abs) {
 struct x9sched_signals s;
 uint64_t swaps = __atomic_load_n(&sweet_x9_ui_swaps, __ATOMIC_RELAXED);
 if (swaps != sched_last_swap_count) {
  sched_last_swap_count = swaps;
  sched_last_swap_abs = now_abs;
 }
 uint64_t age_ns = 0;
 if (sched_last_swap_abs && now_abs >= sched_last_swap_abs) {
  absolutetime_to_nanoseconds(now_abs - sched_last_swap_abs, &age_ns);
 }
 s.dark = dark;
 s.camera_active = __atomic_load_n(&sched_camera_hint, __ATOMIC_RELAXED);
 s.touch_age_ms = touch ? 0 : UINT32_MAX;
 s.swap_age_ms = sched_last_swap_abs ?
     (uint32_t)(age_ns / 1000000ull) : UINT32_MAX;
 s.gpu_util = gpu_util;
 s.cpu_peak = sched_last_peak;	/* previous tick: 50 ms lag is fine */
 sched_scene = x9sched_scene_update(&sched_scene_state, &s);
 sched_scene_residency[sched_scene]++;
 return sched_scene;
}


/*
 * CPU offline manager (boot arg x9cpuoff; x9/cpuoff/README.md). Bit 0
 * arms the SweetCPU PSCI facility (probe + quiesce/resume); bit 1
 * enables this policy. The pure decisions live in x9_cpuoff_policy.h;
 * this glue executes them through XNU's own processor_exit() /
 * processor_start() and confirms every transition against ground truth
 * (PSCI AFFINITY_INFO for off, the initCPU publication for on), because
 * cpu_exit_wait() is a no-op in this build: processor_exit() returning
 * success does not by itself prove the core powered down.
 * SweetCPU state values (SweetCPU.cpp): 0 online, 1 quiescing, 2 off,
 * 3 suspended, 4 onlining, 5 parked.
 */
extern int sweet_x9_cpuoff_facility(void);
extern uint32_t sweet_x9_cpuoff_get_state(unsigned int cpu);
extern void sweet_x9_cpuoff_note_state(unsigned int cpu, uint32_t state);
extern int64_t sweet_x9_psci_affinity_info(unsigned int cpu);
extern uint32_t sweet_x9_cpu_is_initialized(unsigned int cpu);
extern kern_return_t processor_exit(processor_t processor);
extern kern_return_t processor_start(processor_t processor);
#define X9CPUOFF_SW_PARKED 5u
#define X9CPUOFF_SW_OFF 2u
#define X9CPUOFF_SW_SUSPENDED 3u

static int cpuoff_mask_cache2 = -1;
static int cpuoff_ready;
static struct x9cpuoff_mgr cpuoff_mgr;
static struct x9cpuoff_cfg cpuoff_cfg;
static unsigned cpuoff_trans_cpu;
static int cpuoff_trans_dir;	/* 0 none, 1 going off, 2 coming on */
static uint64_t cpuoff_trans_deadline_ms;

static int cpuoff_bit(unsigned bit) {
 if (cpuoff_mask_cache2 < 0) {
  int v = 0;
  PE_parse_boot_argn("x9cpuoff", &v, sizeof(v));
  cpuoff_mask_cache2 = v;
 }
 return (cpuoff_mask_cache2 & (int)bit) != 0;
}

static uint64_t cpuoff_ms(uint64_t abs_time) {
 uint64_t ns = 0;
 absolutetime_to_nanoseconds(abs_time, &ns);
 return ns / 1000000ull;
}

static void cpuoff_setup(uint64_t now) {
 if (cpuoff_ready || !cpuoff_bit(2) || !sweet_x9_cpuoff_facility()) {
  return;
 }
 x9cpuoff_cfg_defaults(&cpuoff_cfg, cpuoff_bit(4), cpuoff_bit(8));
 x9cpuoff_mgr_init(&cpuoff_mgr, cpuoff_ms(now));
 cpuoff_ready = 1;
 printf("X9 CPUOFF: manager on allow_prime=%d drain_big=%d depth=%s "
     "idle_off=%ums light_off=%ums min_online=%ums\n",
     cpuoff_cfg.allow_prime, cpuoff_cfg.drain_big,
     cpuoff_bit(16) ? "suspend" : "off", cpuoff_cfg.idle_off_ms,
     cpuoff_cfg.light_off_ms, cpuoff_cfg.min_online_ms);
}

static void cpuoff_tick(int scene, uint64_t now) {
 if (!cpuoff_ready) {
  cpuoff_setup(now);
  if (!cpuoff_ready) {
   return;
  }
 }
 const uint64_t now_ms = cpuoff_ms(now);
 /* Ground-truth sync: a core SweetCPU reports parked is lost. */
 for (unsigned cpu = X9CPUOFF_FIRST_BIG; cpu < X9CPUOFF_NCPU; cpu++) {
  if (cpuoff_mgr.cpu[cpu].mstate != X9CPUOFF_M_LOST &&
      sweet_x9_cpuoff_get_state(cpu) == X9CPUOFF_SW_PARKED) {
   printf("X9 CPUOFF: cpu%u parked by platform layer; blacklisted\n", cpu);
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, cpu, now_ms, 0);
   if (cpuoff_trans_dir != 0 && cpuoff_trans_cpu == cpu) {
    cpuoff_trans_dir = 0;
   }
  }
 }
 /* Confirm an in-flight transition before deciding anything new. */
 if (cpuoff_trans_dir == 1) {
  const unsigned cpu = cpuoff_trans_cpu;
  const int64_t aff = sweet_x9_psci_affinity_info(cpu);
  if (aff == 1) {
   const uint64_t took = now_ms - cpuoff_mgr.cpu[cpu].state_since_ms;
   sweet_x9_cpuoff_note_state(cpu,
       cpuoff_bit(16) ? X9CPUOFF_SW_SUSPENDED : X9CPUOFF_SW_OFF);
   x9cpuoff_note_off_confirmed(&cpuoff_mgr, cpu, now_ms);
   printf("X9 CPUOFF: cpu%u off confirmed after %llums\n", cpu,
       (unsigned long long)took);
   cpuoff_trans_dir = 0;
  } else if (now_ms >= cpuoff_trans_deadline_ms) {
   printf("X9 CPUOFF: cpu%u offline confirm timeout aff=%lld\n", cpu,
       (long long)aff);
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, cpu, now_ms, aff == 0);
   cpuoff_trans_dir = 0;
  }
  return;
 }
 if (cpuoff_trans_dir == 2) {
  const unsigned cpu = cpuoff_trans_cpu;
  if (sweet_x9_cpu_is_initialized(cpu)) {
   const uint64_t took = now_ms - cpuoff_mgr.cpu[cpu].state_since_ms;
   x9cpuoff_note_online_confirmed(&cpuoff_mgr, cpu, now_ms);
   printf("X9 CPUOFF: cpu%u online confirmed after %llums\n", cpu,
       (unsigned long long)took);
   cpuoff_trans_dir = 0;
  } else if (now_ms >= cpuoff_trans_deadline_ms) {
   const int64_t aff = sweet_x9_psci_affinity_info(cpu);
   printf("X9 CPUOFF: cpu%u online confirm timeout aff=%lld\n", cpu,
       (long long)aff);
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, cpu, now_ms, aff == 0);
   cpuoff_trans_dir = 0;
  }
  return;
 }
 struct x9cpuoff_input in;
 in.scene = scene;
 /* Without live util accounting, never treat the big domain as idle:
  * UINT_MAX also forces the LIGHT wake path to restore cores. */
 in.big_util = cpu_boost_ready ? cpud[X9_BIG].util : UINT_MAX;
 in.now_ms = now_ms;
 in.quiescing = ml_is_quiescing();
 const struct x9cpuoff_decision d =
     x9cpuoff_decide(&cpuoff_mgr, &cpuoff_cfg, &in);
 if (d.action == X9CPUOFF_ACT_OFFLINE) {
  processor_t p = cpu_to_processor((int)d.cpu);
  if (p == PROCESSOR_NULL) {
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, d.cpu, now_ms, 1);
   printf("X9 CPUOFF: cpu%u offline: no processor\n", d.cpu);
   return;
  }
  x9cpuoff_note_offline_issued(&cpuoff_mgr, d.cpu, now_ms);
  cpuoff_trans_cpu = d.cpu;
  cpuoff_trans_dir = 1;
  cpuoff_trans_deadline_ms = now_ms + 5000;
  const kern_return_t kr = processor_exit(p);
  printf("X9 CPUOFF: cpu%u offline issued (scene=%s) kr=%d\n", d.cpu,
      x9sched_scene_name(scene), (int)kr);
  if (kr != KERN_SUCCESS) {
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, d.cpu, now_ms, 1);
   cpuoff_trans_dir = 0;
  }
 } else if (d.action == X9CPUOFF_ACT_ONLINE) {
  processor_t p = cpu_to_processor((int)d.cpu);
  if (p == PROCESSOR_NULL) {
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, d.cpu, now_ms, 0);
   printf("X9 CPUOFF: cpu%u online: no processor\n", d.cpu);
   return;
  }
  x9cpuoff_note_online_issued(&cpuoff_mgr, d.cpu, now_ms);
  cpuoff_trans_cpu = d.cpu;
  cpuoff_trans_dir = 2;
  cpuoff_trans_deadline_ms = now_ms + 10000;
  const kern_return_t kr = processor_start(p);
  printf("X9 CPUOFF: cpu%u online issued (scene=%s) kr=%d\n", d.cpu,
      x9sched_scene_name(scene), (int)kr);
  if (kr != KERN_SUCCESS) {
   const int64_t aff = sweet_x9_psci_affinity_info(d.cpu);
   x9cpuoff_note_failure(&cpuoff_mgr, &cpuoff_cfg, d.cpu, now_ms,
       aff == 0);
   cpuoff_trans_dir = 0;
  }
 }
}

static void cpuoff_stat_line(uint64_t now) {
 if (!cpuoff_ready) {
  return;
 }
 const uint64_t now_ms = cpuoff_ms(now);
 unsigned off_mask = 0, lost_mask = 0;
 uint64_t tot[4];
 for (unsigned i = 0; i < 4; i++) {
  const unsigned cpu = X9CPUOFF_FIRST_BIG + i;
  const struct x9cpuoff_cpu *c = &cpuoff_mgr.cpu[cpu];
  tot[i] = c->off_total_ms;
  if (c->mstate == X9CPUOFF_M_OFF) {
   off_mask |= 1u << cpu;
   if (c->off_since_ms != 0 && now_ms >= c->off_since_ms) {
    tot[i] += now_ms - c->off_since_ms;
   }
  }
  if (c->mstate == X9CPUOFF_M_LOST) {
   lost_mask |= 1u << cpu;
  }
 }
 printf("X9 CPUOFF STAT: off_mask=%x lost_mask=%x trans=%u fail=%u "
     "off_ms=%llu/%llu/%llu/%llu\n", off_mask, lost_mask,
     cpuoff_mgr.transitions, cpuoff_mgr.failures_total,
     (unsigned long long)tot[0], (unsigned long long)tot[1],
     (unsigned long long)tot[2], (unsigned long long)tot[3]);
}

/* Same util accounting as cpu_govern (busiest CPU per domain from XNU
 * accumulated ticks); the decision itself is the policy header's. */
static unsigned cpu_govern2(int touch __unused, int temp, int dark,
    int scene) {
 unsigned util[X9_NCLUSTER] = {0, 0, 0}, peak = 0;
 for (unsigned c = 0; c < X9_NCPU; c++) {
  processor_t p = cpu_to_processor(c);
  natural_t ticks[CPU_STATE_MAX] = {0};
  if (p == PROCESSOR_NULL) {
   continue;
  }
  processor_cpu_load_info(p, ticks);
  uint64_t busy = (natural_t)(ticks[CPU_STATE_USER] -
      cpu_ticks[c][CPU_STATE_USER]);
  busy += (natural_t)(ticks[CPU_STATE_SYSTEM] -
      cpu_ticks[c][CPU_STATE_SYSTEM]);
  uint64_t idle = (natural_t)(ticks[CPU_STATE_IDLE] -
      cpu_ticks[c][CPU_STATE_IDLE]);
  memcpy(cpu_ticks[c], ticks, sizeof(ticks));
  unsigned u = busy + idle ? (unsigned)(busy * 100 / (busy + idle)) : 0;
  unsigned d = x9_topo_cluster[c];
  if (u > util[d]) {
   util[d] = u;
  }
  if (u > peak) {
   peak = u;
  }
 }
 if (!cpu_boost_ready) {
  return peak;
 }
 if (dark != sched_govern2_dark) {
  printf("X9 CPUFREQ: display %s; policy v2 governs background load, "
      "no touch floor while dark\n", dark ? "dark" : "lit");
  sched_govern2_dark = dark;
 }
 const int known = temp >= 1 && temp <= 150;
 uint64_t now_ns = 0;
 absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
 for (unsigned d = 0; d < X9_NCLUSTER; d++) {
  cpud[d].util = util[d];
  struct x9sched_input in;
  in.util = util[d];
  in.temp_c = temp;
  in.temp_valid = known;
  in.scene = scene;
  in.dark = dark;
  in.now_ns = now_ns;
  unsigned next = x9sched_domain_decide(&sched_d[d], &in);
  if (next == cpud[d].cur) {
   continue;
  }
  unsigned req = cpud[d].mhz[next] * 1000u;
  if (cpu_mode == 1) {
   req = (req + FDVFS_UNIT_KHZ - 1u) / FDVFS_UNIT_KHZ;
   for (unsigned c = 0; c < X9_NCPU; c++) {
    if (x9_topo_cluster[c] == d) {
     wr(fdvfs, 0x400 + 4 * c, req);
    }
   }
  } else {
   wr(cpud_base(d), 0x88, cpu_mode == 2 ? req : next);
  }
  cpud[d].cur = next;
 }
 return peak;
}

/* Scene steering (x9sched bit 2): idle consolidates onto the little
 * domain so big/prime can sit at their idle OPP in WFI; interact/camera
 * steer to cpu0+big+prime like the bit-64 rule; light/load allow all.
 * The scheduler's own failsafe still re-recommends every core if a
 * realtime thread starves. */
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
static int scene_steered_mask = -1;
static void core_steer_scene(int scene, int boost_ok) {
 unsigned mask;
 switch (scene) {
 case X9SCENE_IDLE:
  mask = x9_topo_mask(X9_LITTLE);
  break;
 case X9SCENE_INTERACT:
 case X9SCENE_CAMERA:
  mask = boost_ok ?
      (1u | x9_topo_mask(X9_BIG) | x9_topo_mask(X9_PRIME)) : 0xffu;
  break;
 default:
  mask = 0xffu;
  break;
 }
 if ((int)mask == scene_steered_mask) {
  return;
 }
 sched_perfcontrol_update_recommended_cores(mask);
 scene_steered_mask = (int)mask;
}

/* x9g bit 64: steer to cpu0 (boot processor) + big + prime while the UI is active. Preemption and
 * interrupts enabled here (governor thread), as the scheduler requires; the
 * scheduler's own failsafe re-recommends all cores if a realtime thread
 * starves. */
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
static int cores_steered;
static void core_steer_set(int steer) {
 if(steer==cores_steered)return;
 sched_perfcontrol_update_recommended_cores(steer?(1u|x9_topo_mask(X9_BIG)|x9_topo_mask(X9_PRIME)):0xffu);
 cores_steered=steer;
}
/* x9g bit 128: the thread that ends a framebuffer swap is backboardd's
 * CoreAnimation render-server thread (kSweetFBSwapEnd runs in the caller).
 * During the UI boost window it is bound to the prime core, like OPPO's
 * animation-thread placement; the next swap after the window unbinds it.
 * thread_bind takes effect at its next block (GPU/vsync wait). Realtime
 * threads are skipped: thread_bind_internal asserts sched_pri < RT. */
#include <mach/thread_policy.h>
extern kern_return_t thread_policy_get(thread_t thread,thread_policy_flavor_t flavor,thread_policy_t policy_info,
 mach_msg_type_number_t *count,boolean_t *get_default);
extern task_t current_task(void);
extern task_t kernel_task;
static volatile int ui_boost_now;
static thread_t ui_bound_thread;
static uint64_t ui_binds,ui_bind_rt;
void sweet_x9_ui_swap(void);
void sweet_x9_ui_swap(void) {
 __atomic_fetch_add(&sweet_x9_ui_swaps,1,__ATOMIC_RELAXED);
 if(!sweet_x9_perf(128)||current_task()==kernel_task)return;
 thread_t self=current_thread();
 const int want=__atomic_load_n(&ui_boost_now,__ATOMIC_RELAXED);
 thread_t cur=__atomic_load_n(&ui_bound_thread,__ATOMIC_ACQUIRE);
 if(want&&cur==NULL) {
  struct thread_time_constraint_policy tc;mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT;boolean_t def=FALSE;
  if(thread_policy_get(self,THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&tc,&n,&def)!=KERN_SUCCESS||!def){ui_bind_rt++;return;}
  processor_t prime=cpu_to_processor(X9_PRIME_CPU);
  if(prime==PROCESSOR_NULL||!__atomic_compare_exchange_n(&ui_bound_thread,&cur,self,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return;
  thread_bind(prime);ui_binds++;
 } else if(!want&&cur==self) {
  thread_bind(PROCESSOR_NULL);
  __atomic_store_n(&ui_bound_thread,(thread_t)NULL,__ATOMIC_RELEASE);
 }
}
extern int sweet_x9_render_idle(int dark);
static int gpu_opp_at_least(uint32_t khz) {
 int best=gov_ceiling;
 for(int i=0;i<opp_count;i++)
  if(opp_khz[i]>=khz&&opp_khz[i]<opp_khz[best])best=i;
 return best;
}
static void gov_thread(void *arg,wait_result_t result) {
 (void)arg;(void)result;
 uint64_t commits=0,restores=0,ui_ticks=0,boost_ticks=0;
 printf("X9 GPU GOV: active-floor=%u ceiling=%u idle=%u kHz period=50ms target=80%% down-hold=100ms;touch never selects GPU max\n",
  opp_khz[gov_floor],opp_khz[gov_ceiling],opp_khz[gov_slowest]);
 dvfsrc_setup();
 sweet_x9_ufs_late_high(); /* no DVFSRC lock held across the UFS mutex */
 cpu_boost_setup();
 uint64_t last_busy=__atomic_load_n(&sweet_x9_gpu_busy_ns,__ATOMIC_RELAXED),last=mach_absolute_time();
 uint64_t last_log=last,down_since=0,idle_since=last,busy_acc=0,elapsed_acc=0;
 int last_temp=0;bool boost_cool=false,gpu_fault=false;int sleep_state=-1;
 for(;;) {
  uint64_t observed=__atomic_load_n(&gov_events,__ATOMIC_ACQUIRE),work=__atomic_load_n(&gov_work,__ATOMIC_ACQUIRE);
  uint64_t now=mach_absolute_time(),elapsed=0;
  absolutetime_to_nanoseconds(now-last,&elapsed);last=now;
  const int dark=__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_ACQUIRE);
  const uint64_t busy=__atomic_load_n(&sweet_x9_gpu_busy_ns,__ATOMIC_RELAXED),delta=busy-last_busy;last_busy=busy;
  const unsigned util=elapsed?(delta>=elapsed?100:(unsigned)(delta*100/elapsed)):0;
  if(delta)idle_since=now;
  uint64_t idle_ns=0;absolutetime_to_nanoseconds(now-idle_since,&idle_ns);
  struct gpu_shared_prefix st={0};
  /* CPU policy is independent of a resident session / mailbox health. */
  if(shared){status(&st);last_temp=st.temperature;}
  const int temp=last_temp;const bool known=temp>=1&&temp<=150;
  if(!known||temp>=90)boost_cool=false;else if(temp<85)boost_cool=true;
  const int touch=sweet_x9_perf(32)&&ui_tick();ui_ticks++;if(touch)boost_ticks++;
  const int scene=sched_update_scene(dark,touch,util,now);
  const unsigned cpu_peak=sched_bit(1)?cpu_govern2(touch,temp,dark,scene):cpu_govern(touch,temp,dark);
  sched_last_peak=cpu_peak;
  cpuoff_tick(scene,now);
  int steer=!dark&&touch&&boost_cool&&cpu_peak>=80;
  if(sched_bit(2)) {
   steer=(scene==X9SCENE_INTERACT||scene==X9SCENE_CAMERA)&&boost_cool;
   core_steer_scene(scene,steer);
  } else {
   core_steer_set(sweet_x9_perf(64)&&steer);
  }
  __atomic_store_n(&ui_boost_now,steer,__ATOMIC_RELAXED);
  int idle=0;
  if(__atomic_load_n(&render_active_refs,__ATOMIC_ACQUIRE)>=0&&!channel_uncertain&&!gpu_fault) {
   int fastest=gov_ceiling;
   if(!known)fastest=gov_floor;
   else if(temp>=85&&gov_floor>gov_ceiling)fastest=gov_ceiling+(gov_floor-gov_ceiling)*(temp>=95?10:temp-85)/10;
   if(known&&temp>=100)fastest=gov_slowest;
   /* 330: thermal limiter ceiling=7 forced index7, but the old governor
    * retried index0 ~600 times. Honour shared limits BEFORE deciding drift. */
   if(st.cur_ceiling<(unsigned)opp_count&&(int)st.cur_ceiling>fastest)fastest=(int)st.cur_ceiling;
   int slowest=gov_slowest;
   if(st.cur_floor<(unsigned)opp_count&&(int)st.cur_floor<slowest)slowest=(int)st.cur_floor;
   if(slowest<fastest)slowest=fastest; /* ceiling wins a conflicting floor */
   uint32_t base=st.cur_fgpu;
   if(st.cur_fstack>base)base=st.cur_fstack;
   if(!base||base>opp_khz[gov_ceiling])base=opp_khz[gov_floor];
   uint32_t wanted=(uint32_t)((uint64_t)base*util/((sched_bit(4)&&(sched_scene==X9SCENE_LOAD||sched_scene==X9SCENE_CAMERA))?70:80));
   if(delta&&!dark&&wanted<opp_khz[gov_floor])wanted=opp_khz[gov_floor];
   int target=gpu_opp_at_least(wanted);
   if(!delta&&(dark||idle_ns>=250000000ull))target=slowest;
   if(target<fastest)target=fastest;if(target>slowest)target=slowest;
   /* Lit downshifts wait 100ms; display-off and thermal caps apply now. */
   if(gov_index>=0&&target>gov_index&&!dark&&gov_index>=fastest) {
    uint64_t ns=0;if(!down_since)down_since=now;
    absolutetime_to_nanoseconds(now-down_since,&ns);
    if(ns<100000000ull)target=gov_index;
   }else down_since=0;
   if(target!=gov_index) {
    if(gov_commit(target)){gpu_fault=true;printf("X9 GPU GOV: commit failed;no more GPU policy commands\n");}else commits++;
   }else if(delta&&(st.cur_oppidx_gpu!=target||st.cur_oppidx_stack!=target)) {
    /* Restore active GPU/stack drift only; no idle re-commit spam. */
    if(gov_commit(target)){gpu_fault=true;printf("X9 GPU GOV: active restore failed\n");}else restores++;
   }
   if(!channel_uncertain&&!gpu_fault)idle=sweet_x9_render_idle(dark&&!delta&&idle_ns>=250000000ull);
   if(idle<0){gpu_fault=true;printf("X9 GPU GOV: retention transition uncertain;GPU owner retains all resources\n");}
  }
  busy_acc+=delta;elapsed_acc+=elapsed;
  unsigned period=dark&&!delta&&cpu_peak<5&&idle==1?1000:50;
  if((int)period!=sleep_state) {
   if(period==1000||sleep_state==1000)printf("X9 POWER: %s governor period=%ums background_peak=%u%%\n",period==1000?"display-idle":"active",period,cpu_peak);
   sleep_state=(int)period;
  }
  uint64_t log_ns=0;absolutetime_to_nanoseconds(now-last_log,&log_ns);
  if(log_ns>=(dark?30000000000ull:2000000000ull)) {
   printf("X9 GPU GOV: util_avg=%llu%% temp=%d idx=%d actual=%u/%u kHz limits=%u/%u commits=%llu restores=%llu dark=%d period=%u\n",
    elapsed_acc?(busy_acc>=elapsed_acc?100:busy_acc*100/elapsed_acc):0,temp,gov_index,st.cur_fgpu,st.cur_fstack,
    st.cur_ceiling,st.cur_floor,commits,restores,dark,period);
   if(cpu_boost_ready)printf("X9 CPUFREQ: load=%u/%u/%u%% request=%u/%u/%uMHz touch_floor=%llu/%llu ticks mode=%s dark=%d\n",
    cpud[0].util,cpud[1].util,cpud[2].util,cpud[0].mhz[cpud[0].cur],cpud[1].mhz[cpud[1].cur],cpud[2].mhz[cpud[2].cur],
    boost_ticks,ui_ticks,cpu_mode_name(cpu_mode),dark);
   cpuoff_stat_line(now);
   if(sched_bit(1)||sched_bit(2)||sched_bit(4))printf("X9 SCHED: scene=%s resid=%u/%u/%u/%u/%u up=%u/%u/%u down=%u/%u/%u capped=%u/%u/%u steer_mask=%d\n",
    x9sched_scene_name(sched_scene),sched_scene_residency[0],sched_scene_residency[1],sched_scene_residency[2],sched_scene_residency[3],sched_scene_residency[4],
    sched_d[0].up_commits,sched_d[1].up_commits,sched_d[2].up_commits,
    sched_d[0].down_commits,sched_d[1].down_commits,sched_d[2].down_commits,
    sched_d[0].capped_ticks,sched_d[1].capped_ticks,sched_d[2].capped_ticks,scene_steered_mask);
   last_log=now;busy_acc=elapsed_acc=ui_ticks=boost_ticks=0;
  }
  __atomic_store_n(&gov_idle_sleep,period>50,__ATOMIC_RELEASE);
  gov_sleep_ms(period,observed,work);
  __atomic_store_n(&gov_idle_sleep,0,__ATOMIC_RELAXED);
 }
}
static void gpu_select_performance(void) {
 static int done;
 if(done)return;
 done=1;
 /* floor; x9_gpu_khz is the governor ceiling. x9_gpu_floor_khz overrides the
  * floor (66.txt: GPU wait 11-13 ms/job at 858-1300 MHz, 40-42 C). */
 uint32_t cap=X9_GOV_FLOOR_KHZ;PE_parse_boot_argn("x9_gpu_floor_khz",&cap,sizeof(cap));
 struct gpu_shared_prefix s;status(&s);
 if(!valid_status(&s)||s.opp_num_stack!=s.opp_num_gpu){printf("X9 GPU DVFS: unexpected OPP tables gpu=%d stack=%d\n",s.opp_num_gpu,s.opp_num_stack);return;}
 int best=-1;uint32_t best_khz=0,max_khz=0,min_khz=0xffffffffu;
 for(int i=0;i<s.opp_num_gpu;i++) {
  struct gpu_ipi req={0},resp={0};req.cmd=CMD_GET_FREQ_BY_IDX;req.target=TARGET_GPU;req.u.oppidx=i;
  if(exchange(&req,&resp)){printf("X9 GPU DVFS: frequency query %d failed\n",i);return;}
  uint32_t khz=resp.u.freq;
  if(!khz||khz>5000000u||(i&&khz>opp_khz[i-1])) {printf("X9 GPU DVFS: invalid/non-descending OPP table;no governor\n");return;}
  if(khz>max_khz)max_khz=khz;
  if(khz&&khz<min_khz)min_khz=khz;
  if(khz&&khz<=cap&&khz>best_khz){best=i;best_khz=khz;}
  opp_khz[i]=khz;
 }
 opp_count=s.opp_num_gpu;
 printf("X9 GPU DVFS: GPU OPPs %d..%d kHz, current %u/%u kHz, cap %u kHz -> index %d (%u kHz)\n",
  (int)min_khz,(int)max_khz,s.cur_fgpu,s.cur_fstack,cap,best,best_khz);
 if(best<0)return;
 struct gpu_ipi req={0},resp={0};req.cmd=CMD_DUAL_COMMIT;req.u.dual.gpu_oppidx=best;req.u.dual.stack_oppidx=best;
 if(exchange(&req,&resp)||resp.u.result<0){printf("X9 GPU DVFS: commit failed\n");return;}
 status(&s);
 printf("X9 GPU DVFS: commit index %d firmware-return=%d now gpu=%u kHz stack=%u kHz volt=%u/%u oppidx=%d/%d ceiling=%u floor=%u\n",
  best,resp.u.result,s.cur_fgpu,s.cur_fstack,s.cur_vgpu,s.cur_vstack,s.cur_oppidx_gpu,s.cur_oppidx_stack,s.cur_ceiling,s.cur_floor);
 gov_index=best;
 printf("X9 GPU DVFS: shared temperature=%d comp_norm=%d comp_high=%d limiter=%u/%u priority=%u/%u dvfs_state=%u\n",
  s.temperature,s.temper_comp_norm_gpu,s.temper_comp_high_gpu,s.cur_c_limiter,s.cur_f_limiter,s.cur_c_priority,s.cur_f_priority,s.dvfs_state);
 int gov=1;PE_parse_boot_argn("x9_gpu_gov",&gov,sizeof(gov));
 uint32_t ceiling_khz=1300000;PE_parse_boot_argn("x9_gpu_khz",&ceiling_khz,sizeof(ceiling_khz));
 if(!gov||!ceiling_khz){printf("X9 GPU GOV: disabled (x9_gpu_gov=%d x9_gpu_khz=%u); fixed index %d\n",gov,ceiling_khz,best);return;}
 int ceiling=-1,slowest=-1;uint32_t ceiling_best=0,slow_khz=0xffffffffu;
 for(int i=0;i<opp_count;i++) {
  if(opp_khz[i]&&opp_khz[i]<=ceiling_khz&&opp_khz[i]>ceiling_best){ceiling=i;ceiling_best=opp_khz[i];}
  if(opp_khz[i]&&opp_khz[i]<slow_khz){slowest=i;slow_khz=opp_khz[i];}
 }
 if(ceiling<0||slowest<0||ceiling>best){printf("X9 GPU GOV: no ceiling above floor; fixed index %d\n",best);return;}
 gov_floor=best;gov_ceiling=ceiling;gov_slowest=slowest;
 thread_t t;
 if(kernel_thread_start(gov_thread,NULL,&t)==KERN_SUCCESS)thread_deallocate(t);
 else printf("X9 GPU GOV: thread creation failed; fixed index %d\n",best);
}
/* pan145 hardware: firmware POWER+ACTIVE produced power=1/active=1,
 * buck=1/mtcmos=1/cg=2, MFG1=c001770d and MFG37=c000110d while
 * MFG2 stayed00001112. The original MT6993 AP platform reads MALI_GPU_ID
 * after the successful power callback without requiring MFG2 ACK.
 * Keep ACK checks on both observed activated domains; MFG2 remains logged,
 * never forced on. This is a conservative ID-read gate, not a complete
 * model of MT6993 internal power-domain topology. */
static int gpu_id_domains_ready(void) {
 return on(rd(rpc,0x500))&&on(rd(rpc,0x594));
}
static int wait_counts(int power,int active,int require_domains) {
 uint64_t until;clock_interval_to_deadline(500,NSEC_PER_MSEC,&until);
 do {
  struct gpu_shared_prefix s;status(&s);
  if(valid_status(&s)&&s.power_count==power&&s.active_count==active&&eb_ready()&&
     (!require_domains||gpu_id_domains_ready()))return 0;
  if(expired(until))break;
  pause_ms();
 }while(1);
 return -1;
}
/* Read-only capability/state capture after power/count/ID gates. Source:
 * a16w CSF regmap and MT6993 gpufreq_reg; SMMUv3 common header offsets.
 * Enabled translation or bypass state does not establish a DMA mapping. */
static void observe_memory_prerequisites(vm_offset_t gpu) {
 printf("X9 CSF HW: L2_FEATURES=%08x MMU_FEATURES=%08x AS_PRESENT=%08x MCU_STATUS=%08x\n",
  rd(gpu,4),rd(gpu,0x14),rd(gpu,0x18),rd(gpu,0x704));
 vm_offset_t smmu=ml_io_map(0x48600000,0x1000);
 if(!smmu){printf("X9 CSF HW: MFG SMMU mapping failed; DMA not enabled\n");return;}
 uint32_t cr0=rd(smmu,0x20),ack=rd(smmu,0x24),gbpa=rd(smmu,0x44);
 printf("X9 CSF HW: SMMU_CR0=%08x CR0ACK=%08x GBPA=%08x enable=%u abort=%u; no SMMU writes\n",
  cr0,ack,gbpa,cr0&1,(gbpa>>20)&1);
 printf("X9 CSF HW: MCU startup uses separate runtime ownership and cache gates\n");
}
static void run(void) {
 int enabled=0;
 if((enabled=sweet_x9_arg("x9_gpu_power_probe",1))!=1)return;
 printf("X9 GPU: pan165 system compute startup INIT -> POWER_ON -> ID -> POWER_OFF\n");
 if(!handoff()){printf("X9 GPU: live FDT handoff absent/mismatched; all writes refused\n");return;}
 spm=ml_io_map(0x1c004000,0x1000);
 if(!spm||!on(rd(spm,0xe1c))){printf("X9 GPU: SPM MFG0 ACK absent; stopped\n");return;}
 rpc=ml_io_map(0x4b800000,0x1000);
 if(!rpc||!on(rd(rpc,0x504))){printf("X9 GPU: RPC MFG0 ACK absent; stopped\n");return;}
 gpr=ml_io_map(0x4b09fc80,0x100);
 if(!gpr||rd(gpr,40)!=1){printf("X9 GPU: GPUEB not resumed; no cold-start guessed\n");return;}
 mbox=ml_io_map(0x4b09fd80,0x280);ctrl=ml_io_map(0x4b170000,0x1000);
 /* This window is a verified no-map firmware carveout below XNU RAM.
  * Device mapping + aligned volatile words avoid cached aliases and SIMD
  * accesses to Device memory. Only the captured 16 KiB GPUFREQ slice is written. */
 shared=ml_io_map(SHARED_PA,SHARED_SIZE);
 if(!mbox||!ctrl||!shared){printf("X9 GPU: mapping failed before ownership\n");return;}
 snapshot("entry");
 magic=rd(gpr,52);
 if(!magic||magic==0xffffffffu||!eb_ready()||
    ((rd(ctrl,0)|rd(ctrl,0x78))&CHANNEL_BIT)) {
  printf("X9 GPU: startup magic/channel unavailable; no writes\n");return;
 }
 /* Verify magic is stable immediately before ownership; never restore it
  * after transmitting: a delayed firmware command may still reference SRAM. */
 if(rd(gpr,52)!=magic)return;
 for(unsigned off=0;off<SHARED_SIZE;off+=4)wr(shared,off,0);
 if(!eb_ready()||rd(gpr,52)!=magic||((rd(ctrl,0)|rd(ctrl,0x78))&CHANNEL_BIT)) {
  printf("X9 GPU: state changed during shared prepare; no command transmitted\n");return;
 }
 wr(gpr,52,0);
 printf("X9 GPU: ownership acquired; shared=%llx size=%x (reserved), magic consumed\n",SHARED_PA,SHARED_SIZE);
 struct gpu_ipi req={0},resp={0};req.cmd=CMD_INIT;req.u.mem.base=SHARED_PA;req.u.mem.size=SHARED_SIZE;
 if(exchange(&req,&resp))return;
 /* Stock ignores the union of INIT's response. Validate firmware-populated
  * shared state instead of interpreting a possibly echoed pointer as errno. */
 struct gpu_shared_prefix s;uint64_t until;clock_interval_to_deadline(500,NSEC_PER_MSEC,&until);
 do {status(&s);if(valid_status(&s))break;if(expired(until))break;pause_ms();}while(1);
 log_status("after-init",&s);
 if(!valid_status(&s)||s.power_count!=0||s.active_count!=0||s.power_control!=1) {
  printf("X9 GPU: shared ABI/initial ownership unsupported; no power command\n");return;
 }
 int32_t count=0;int powered=0,active=0,id_ok=0;
 if(power_command(CMD_POWER,1,&count))return;
 powered=1;
 if(count!=1){printf("X9 GPU: unexpected power refcount=%d; skip ID\n",count);goto cleanup;}
 if(s.active_sleep_control) {
  if(power_command(CMD_ACTIVE,1,&count))goto cleanup;
  active=1;
  if(count!=1){printf("X9 GPU: unexpected active refcount=%d; skip ID\n",count);goto cleanup;}
 }
 if(wait_counts(1,active,1)) {
  status(&s);log_status("not-ready",&s);snapshot("power-not-confirmed");
  printf("X9 GPU: power/count/domain confirmation failed; GPU bank not read\n");goto cleanup;
 }
 snapshot("power-confirmed");status(&s);log_status("power-confirmed",&s);
 printf("X9 GPU: ID gate uses shared counts + MFG1/MFG37 ACK; MFG2 is observation only\n");
 /* Original MT6993 gpufreq_reg_mt6993.h MALI_GPU_ID at0x48000000;
  * Mali CSF regmap GPU_ID is a 32-bit read. No GPU commands are issued. */
 vm_offset_t gpu=ml_io_map(0x48000000,0x4000);
 if(gpu) {
  uint32_t id=rd(gpu,0);__asm__ volatile("dsb sy" ::: "memory");uint32_t again=rd(gpu,0);
  id_ok=id&&id!=0xffffffffu&&id==again;
  printf("X9 GPU: GPU_ID=%08x repeat=%08x stable-valid=%d (before CSF startup; no shader jobs)\n",id,again,id_ok);
  if(id_ok) {
   observe_memory_prerequisites(gpu);
   int csf_result=sweet_x9_csf_boot(gpu);
   printf("X9 GPU: CSF MCU result=%d (1 boot+PING,0 clean skip/failure,-1 retain power)\n",csf_result);
   if(csf_result<0) {
    printf("X9 GPU: CSF cleanup incomplete; outer power/active references retained, no reset\n");return;
   }
  }
 }
cleanup:
 if(channel_uncertain) {
  printf("X9 GPU: mailbox outcome uncertain; retain shared memory, no more commands\n");return;
 }
 if(active) {
  if(power_command(CMD_ACTIVE,0,&count))return;
  if(count!=0){printf("X9 GPU: active release count=%d; stopping\n",count);return;}
 }
 if(powered) {
  if(power_command(CMD_POWER,0,&count))return;
  if(count!=0||wait_counts(0,0,0)) {
   printf("X9 GPU: power release not confirmed; stopping\n");return;
  }
 }
 status(&s);log_status("released",&s);snapshot("released");
 printf("X9 GPU: one-shot finished ID_valid=%d; power reference released, shared carveout retained\n",id_ok);
 if(id_ok) {
  int render=0;
  if((render=sweet_x9_arg("x9_metal_render",1))==1)sweet_x9_render_bootstrap();
  else sweet_x9_compute_publish();
 }
}
static void worker(void *arg,wait_result_t result) {
 (void)arg;(void)result;sweet_x9_csf_prepare();run();(void)thread_terminate(current_thread());
}
/* Replaces the pan143 observer entry point, preserving its UFS startup hook. */
#include "x9_modem.inc"
void sweet_x9_gpu_dependencies(void) {
 int enabled=0;static int started;
 if(sweet_early_is_x9())x9_modem_start();
 if(!sweet_early_is_x9()||started||(enabled=sweet_x9_arg("x9_gpu_power_probe",1))!=1)return;
 started=1;thread_t t;
 if(kernel_thread_start(worker,NULL,&t)==KERN_SUCCESS)thread_deallocate(t);
 else printf("X9 GPU: worker creation failed\n");
}

/* Called only by the single serialized private user-client method, after the
 * startup campaign and its complete release. Reuse the initialized GPUEB ABI;
 * never repeat INIT or consume old startup magic after this boot's ownership. */
extern int sweet_x9_csf_desktop(vm_offset_t,void *);
extern int sweet_x9_csf_resident_begin(vm_offset_t);
extern int sweet_x9_csf_resident_end(void);
static int gpu_owned_request(void *job,int desktop) {
 if(!job||!shared||channel_uncertain||!eb_ready())return -1;
 struct gpu_shared_prefix s;status(&s);
 if(!valid_status(&s)||s.power_count||s.active_count)return -1;
 int32_t count=0;int active=0;
 if(power_command(CMD_POWER,1,&count)||count!=1||wait_counts(1,0,0))return -1;
 if(s.active_sleep_control) {
  if(power_command(CMD_ACTIVE,1,&count)||count!=1)return -1;
  active=1;
 }
 if(wait_counts(1,active,1))return -1;
 if(desktop==2)gpu_select_performance();   // resident render session start
 /* ml_io_map() VA is permanent (fixed io_submap): map once, not per request. */
 static vm_offset_t gpu;
 if(!gpu)gpu=ml_io_map(0x48000000,0x4000);
 if(!gpu||rd(gpu,0)!=0xe8800010u)return -1;
 int result=desktop==2?sweet_x9_csf_resident_begin(gpu):
  (desktop?sweet_x9_csf_desktop(gpu,job):sweet_x9_csf_user(gpu,job));
 if(desktop==2&&result==2) {
  __atomic_store_n(&render_active_refs,active,__ATOMIC_RELEASE);
  gov_wake();
  printf("X9 RENDER: resident session owns GPUEB power1/active%d;no per-request shutdown\n",active);
  return 2;
 }
 if(result<0||channel_uncertain)return -1;
 if(active&&(power_command(CMD_ACTIVE,0,&count)||count!=0))return -1;
 if(power_command(CMD_POWER,0,&count)||count!=0||wait_counts(0,0,0))return -1;
 printf("X9 METAL GPU: request cleanup complete GPUEB active/power=0 result=%d\n",result);
 return result;
}
int sweet_x9_gpu_user_compute(void *job) {return gpu_owned_request(job,0);}
int sweet_x9_gpu_desktop(void *job) {return gpu_owned_request(job,1);}


/* Caller holds the shared GPU owner lock; begin/end are not root RPCs. */
int sweet_x9_gpu_render_begin(void) {
 static int token;
 if(render_active_refs>=0)return -1;
 return gpu_owned_request(&token,2);
}
int sweet_x9_gpu_render_end(void) {
 if(render_active_refs<0||channel_uncertain||sweet_x9_csf_resident_end()!=0)return -1;
 int32_t count=0;
 if(render_active_refs&&(power_command(CMD_ACTIVE,0,&count)||count!=0))return -1;
 if(power_command(CMD_POWER,0,&count)||count!=0||wait_counts(0,0,0))return -1;
 __atomic_store_n(&render_active_refs,-1,__ATOMIC_RELEASE);gov_wake();return 1;
}
