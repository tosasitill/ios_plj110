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
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <stddef.h>
#include "gpu_shared_prefix.h"
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
 * debug CMD_FIX_*. There is no thermal governor here, hence a moderate
 * default cap. x9_gpu_khz=<kHz> sets it; 0 leaves the OPP alone.
 * Sources: OnePlus/OPPO MT6993 gpufreq_ipi.h and gpufreq_v2.c. */
/* Load governor (2026-11-03). The fixed commit below is the FLOOR (old
 * behaviour, 858 MHz with the old 900 MHz cap); a kernel thread then raises the
 * OPP up to x9_gpu_khz (default 1300 MHz) from GPU busy time, measured by the
 * synchronous render submit path (doorbell -> fence) in sweet_x9_gpu_busy_ns.
 * Thermal (log 312): MTK ATM holds Tj at 85 C and throttles gradually, GPUEB
 * keeps its own thermal limiter and soc_max is critical at 118.5 C. The old
 * hard 80 C cliff dropped GPU to 858 MHz and CPU to ~800 MHz at 79-81 C (UI
 * 30 fps, flapping every tick). Now: GPU ceiling steps from the fastest OPP
 * at 85 C to the floor at 95 C, slowest at >= 100 C; the UI boost (CPU + core
 * steer + GPU fastest) turns off at >= 90 C and back on below 85 C. An
 * implausible reading never allows more than the floor.
 * x9_gpu_gov=0 keeps the single fixed commit at the floor. */
#define X9_GOV_FLOOR_KHZ 900000u
volatile uint64_t sweet_x9_gpu_busy_ns;
static uint32_t opp_khz[256];static int opp_count,gov_floor=-1,gov_ceiling=-1,gov_slowest=-1,gov_index=-1;
static int render_active_refs=-1;
static unsigned gov_cookie;
static void gov_sleep_ms(unsigned ms) {
 uint64_t until;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&until);
 (void)assert_wait_deadline(&gov_cookie,THREAD_UNINT,until);thread_block(THREAD_CONTINUE_NULL);
}
static int gov_commit(int index) {
 struct gpu_ipi req={0},resp={0};req.cmd=CMD_DUAL_COMMIT;req.u.dual.gpu_oppidx=index;req.u.dual.stack_oppidx=index;
 lck_mtx_lock(&x9_gpu_ipi_lock);ipi_quiet=1;
 int r=exchange_unlocked(&req,&resp);
 ipi_quiet=0;lck_mtx_unlock(&x9_gpu_ipi_lock);
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
 * UI boost (x9g bit 32). Log 307: frames were CPU bound (backboardd ~14 ms
 * of encoding per heavy frame, GPU 5-10 ms), so GPU util sat at 6-26% and
 * the util governor dropped to 858 MHz mid-scroll; CPU frequency was never
 * touched at all (whatever LK left). Activity = finger down in the last
 * 250 ms, or >= 6 swaps in the last 250 ms (an idle desktop still commits
 * ~10/s). Boost holds 500 ms after the last activity.
 */
extern int sweet_x9_perf(unsigned bit);
volatile uint64_t sweet_x9_ui_swaps;        /* SweetFramebuffer swap count */
volatile uint64_t sweet_x9_ui_touch_abs;    /* last touch sample with a finger down */
void sweet_x9_ui_touch(void){__atomic_store_n(&sweet_x9_ui_touch_abs,mach_absolute_time(),__ATOMIC_RELAXED);}
/*
 * Display dark (log 319, phone hot with the screen off). SweetFramebuffer
 * sets this on iOS's display-off request (power key / auto-lock), also when
 * sweetfb_stayon keeps the panel lit, and clears it on display-on. The touch
 * panel keeps reporting while the screen is off: a hand holding the phone
 * (319: four contacts at 84.2 s, 0.9 s after the power key) armed the UI
 * boost, i.e. CPU 2.7/3.5/4.2 GHz and GPU 1.7 GHz with nothing on screen; in
 * 318 touches at 115 s and 124 s did it again after the lock. While dark the
 * boost ignores touches and swaps, the GPU governor runs at its slowest OPP
 * and the CPU requests drop below LK's boot clocks (see cpu_dark_req).
 */
volatile int sweet_x9_display_dark;
void sweet_x9_set_display_dark(int dark);
void sweet_x9_set_display_dark(int dark){__atomic_store_n(&sweet_x9_display_dark,dark?1:0,__ATOMIC_RELAXED);}
static uint64_t ui_ring[5];static unsigned ui_pos;static uint64_t ui_until;
static int ui_tick(void) {   /* called every 50 ms by the governor */
 const uint64_t now=mach_absolute_time(),swaps=__atomic_load_n(&sweet_x9_ui_swaps,__ATOMIC_RELAXED);
 const uint64_t recent=swaps-ui_ring[ui_pos];ui_ring[ui_pos]=swaps;ui_pos=(ui_pos+1)%5;
 const uint64_t t=__atomic_load_n(&sweet_x9_ui_touch_abs,__ATOMIC_RELAXED);
 uint64_t ns=0;if(t&&now>t)absolutetime_to_nanoseconds(now-t,&ns);
 const int touch=t&&(now<=t||ns<250000000ull);
 if(__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_RELAXED)){ui_until=0;return 0;}
 if(touch||recent>=6){uint64_t hold;nanoseconds_to_absolutetime(500000000ull,&hold);ui_until=now+hold;}
 return now<ui_until;
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
 * Boot values of every request register are restored when the UI is idle.
 */
#define CPUHW_PA 0x051c0000ULL     /* cpuhvfs CSRAM (reg[1]); USRAM follows at +0x1400 */
#define CSRAM_SYS_PA 0x00118000ULL /* page holding csram_sys @0x11bc00 */
#define FDVFS_PA 0x05150000ULL     /* page holding fdvfs regs @0x5150400 / cci @0x5150430 */
#define FDVFS_MAGIC 0xABCD0001u
#define FDVFS_UNIT_KHZ 26000u
static vm_offset_t cpuhw,csram_sys,fdvfs;
static struct {unsigned n,top,boot,cur;uint16_t mhz[32];} cpud[3];
static int cpu_boost_ready,cpu_boosted,cpu_mode;   /* mode 1 = FDVFS, 2 = PERF_STATE kHz, 3 = PERF_STATE index */
static uint32_t fdvfs_boot[X9_NCPU],perf_boot[3];
/* Display-dark FDVFS request per CPU: the lowest LUT OPP of its domain at or
 * above 500 MHz, never above the boot request. Idle (screen on) still
 * restores LK's boot requests (~1.0/0.8/1.1 GHz, 309), which kept 318's
 * screen-off GPUEB temperature at 54-56 C. */
static uint32_t fdvfs_dark[X9_NCPU];static int cpu_dark;
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
 /* Idle request = what LK left; a zero (no request) becomes ~1200 MHz, close
  * to the clocks measured in 309, rather than the lowest OPP. */
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
 for(unsigned c=0;c<X9_NCPU;c++) {
  const unsigned d=x9_topo_cluster[c];unsigned mhz=0;
  for(unsigned i=0;i<cpud[d].n;i++)if(cpud[d].mhz[i]>=500&&(!mhz||cpud[d].mhz[i]<mhz))mhz=cpud[d].mhz[i];
  uint32_t req=mhz?(mhz*1000u+FDVFS_UNIT_KHZ-1u)/FDVFS_UNIT_KHZ:fdvfs_boot[c];
  fdvfs_dark[c]=req<fdvfs_boot[c]?req:fdvfs_boot[c];
 }
 cpu_boost_ready=1;
 printf("X9 CPUFREQ: UI boost armed via %s (top %u/%u/%u MHz; idle restores boot requests; display dark requests cpu0/4/7=%u/%u/%u x26 MHz)\n",cpu_mode_name(cpu_mode),
  cpud[0].mhz[cpud[0].top],cpud[1].mhz[cpud[1].top],cpud[2].mhz[cpud[2].top],fdvfs_dark[0],fdvfs_dark[4],fdvfs_dark[7]);
}
static void cpu_boost_set(int boost) {
 const int dark=!boost&&cpu_mode==1&&__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_RELAXED);
 if(!cpu_boost_ready||(boost==cpu_boosted&&dark==cpu_dark))return;
 if(dark!=cpu_dark)printf("X9 CPUFREQ: display %s: idle requests cpu0/4/7=%u/%u/%u x26 MHz\n",dark?"dark":"lit",
  dark?fdvfs_dark[0]:fdvfs_boot[0],dark?fdvfs_dark[4]:fdvfs_boot[4],dark?fdvfs_dark[7]:fdvfs_boot[7]);
 cpu_dark=dark;
 if(cpu_mode==1) {
  for(unsigned c=0;c<X9_NCPU;c++) {
   const unsigned d=x9_topo_cluster[c];
   const uint32_t req=boost?(cpud[d].mhz[cpud[d].top]*1000u+FDVFS_UNIT_KHZ-1u)/FDVFS_UNIT_KHZ:dark?fdvfs_dark[c]:fdvfs_boot[c];
   wr(fdvfs,0x400+4*c,req);
  }
 } else {
  for(unsigned d=0;d<3;d++) {
   const uint32_t req=!boost?perf_boot[d]:cpu_mode==2?cpud[d].mhz[cpud[d].top]*1000u:cpud[d].top;
   wr(cpud_base(d),0x88,req);
  }
 }
 for(unsigned d=0;d<3;d++)cpud[d].cur=boost?cpud[d].top:0;
 cpu_boosted=boost;
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
static void gov_thread(void *arg,wait_result_t result) {
 (void)arg;(void)result;
 const uint64_t period_ns=50ull*1000*1000;
 uint64_t last_busy=sweet_x9_gpu_busy_ns,last_log=mach_absolute_time(),commits=0,busy_acc=0,ticks=0,restores=0;
 int last_temp=0;bool boost_cool=false;
 printf("X9 GPU GOV: start floor=%d(%u kHz) ceiling=%d(%u kHz) slowest=%d period=50ms up>=20%% down<5%%\n",
  gov_floor,opp_khz[gov_floor],gov_ceiling,opp_khz[gov_ceiling],gov_slowest);
 dvfsrc_setup();
 sweet_x9_ufs_late_high(); /* no DVFSRC lock held across the UFS mutex */
 cpu_boost_setup();
 uint64_t ui_ticks=0,boost_ticks=0;int ui=0;
 for(;;) {
  gov_sleep_ms(50);
  ui=sweet_x9_perf(32)&&ui_tick();ui_ticks++;if(ui)boost_ticks++;
  if(channel_uncertain){printf("X9 GPU GOV: mailbox uncertain; governor stops\n");break;}
  const uint64_t busy=sweet_x9_gpu_busy_ns,delta=busy-last_busy;last_busy=busy;
  if(render_active_refs<0)continue;      // no resident session: GPU may be off
  const unsigned util=delta>=period_ns?100:(unsigned)(delta*100/period_ns);
  struct gpu_shared_prefix st;status(&st);
  const int temp=st.temperature;last_temp=temp;
  const bool known=temp>=1&&temp<=150;
  if(!known||temp>=90)boost_cool=false;else if(temp<85)boost_cool=true;   /* hysteresis */
  int fastest=gov_ceiling;               // smaller index is faster
  if(!known)fastest=gov_floor;
  else if(temp>=85&&gov_floor>gov_ceiling)fastest=gov_ceiling+(gov_floor-gov_ceiling)*(temp>=95?10:temp-85)/10;
  cpu_boost_set(ui&&boost_cool);         /* hot or unknown: CPU back to its boot index */
  core_steer_set(sweet_x9_perf(64)&&ui&&boost_cool);
  __atomic_store_n(&ui_boost_now,ui&&boost_cool,__ATOMIC_RELAXED);
  if(known&&temp>=100)fastest=gov_slowest;
  int target=gov_index<0?gov_floor:gov_index;
  /* Submissions are synchronous and serialized: frame time is the sum of
   * per-job GPU latency, so any sustained load benefits from a higher clock
   * (rev244: 23% util, 10 ms wait per job, 9 frames/s). */
  if(util>=20)target-=4;else if(util<5)target+=2;
  if(ui)target=fastest;   /* UI boost: frame time is latency bound, util understates it */
  if(target<fastest)target=fastest;
  const int slowest_allowed=fastest>gov_floor?fastest:gov_floor;
  if(target>slowest_allowed)target=slowest_allowed;
  if(__atomic_load_n(&sweet_x9_display_dark,__ATOMIC_RELAXED)&&gov_slowest>=0)target=gov_slowest;   /* screen off: nothing to draw */
  if(target!=gov_index){if(gov_commit(target)){printf("X9 GPU GOV: commit %d failed; governor stops\n",target);break;}commits++;}
  /* 105/106.txt: with the floor pinned at the ceiling (x9_gpu_floor_khz=1716000)
   * the boot commit read back gpu=stack=1716 MHz, but every later status showed
   * stack=260 MHz (its slowest) and the governor never committed again, so the
   * shader stack ran at 260 MHz for the whole session. GPUEB lowers the OPP on
   * its own; re-commit whenever its current indices differ from ours. */
  else if(gov_index>=0&&(st.cur_oppidx_gpu!=gov_index||st.cur_oppidx_stack!=gov_index)) {
   if(++restores<=6||restores%200==0)
    printf("X9 GPU GOV: firmware OPP drifted to %d/%d (%u/%u kHz), restoring %d (restore #%llu)\n",
     st.cur_oppidx_gpu,st.cur_oppidx_stack,st.cur_fgpu,st.cur_fstack,gov_index,restores);
   if(gov_commit(gov_index)){printf("X9 GPU GOV: restore commit %d failed; governor stops\n",gov_index);break;}
  }
  busy_acc+=util;ticks++;
  uint64_t now=mach_absolute_time(),ns=0;absolutetime_to_nanoseconds(now-last_log,&ns);
  if(ns>=2000000000ull) {
   const uint32_t gear=dvfsrc?rd(dvfsrc,0x6ac):0;
   printf("X9 GPU GOV: util_avg=%llu%% temp=%d idx=%d gpu=%u kHz stack=%u kHz commits=%llu limiter=%u/%u ceiling=%u floor=%u actual=%u/%u kHz oppidx=%d/%d restores=%llu ddr_gear=%u vcore=%u\n",
    ticks?busy_acc/ticks:0,last_temp,gov_index,gov_index>=0?opp_khz[gov_index]:0,st.cur_fstack,commits,st.cur_c_limiter,st.cur_f_limiter,st.cur_ceiling,st.cur_floor,
    st.cur_fgpu,st.cur_fstack,st.cur_oppidx_gpu,st.cur_oppidx_stack,restores,gear&0xf,(gear>>8)&7);
   if(cpu_boost_ready||sweet_x9_perf(32)){
    const unsigned lm=cpu_measure_cluster(X9_LITTLE),bm=cpu_measure_cluster(X9_BIG),pm=cpu_measure_cluster(X9_PRIME);
    printf("X9 CPUFREQ: ui_boost=%llu/%llu ticks steered=%d cpu_boosted=%d mode=%s fdvfs_req cpu0/4/7=%u/%u/%u perf=%x/%x/%x hw_state=%x/%x/%x measured little/big/prime ~%u/%u/%u MHz prime_bind=%d binds=%llu rt_skips=%llu\n",
     boost_ticks,ui_ticks,cores_steered,cpu_boosted,cpu_mode_name(cpu_mode),
     fdvfs?rd(fdvfs,0x400):0,fdvfs?rd(fdvfs,0x410):0,fdvfs?rd(fdvfs,0x41c):0,
     cpuhw?rd(cpud_base(0),0x88):0,cpuhw?rd(cpud_base(1),0x88):0,cpuhw?rd(cpud_base(2),0x88):0,
     cpuhw?rd(cpud_base(0),0x8c):0,cpuhw?rd(cpud_base(1),0x8c):0,cpuhw?rd(cpud_base(2),0x8c):0,lm,bm,pm,
     __atomic_load_n(&ui_bound_thread,__ATOMIC_RELAXED)!=NULL,ui_binds,ui_bind_rt);
    boost_ticks=ui_ticks=0;}
   last_log=now;busy_acc=ticks=0;
  }
 }
 (void)thread_terminate(current_thread());
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
 if(exchange(&req,&resp)){printf("X9 GPU DVFS: commit failed\n");return;}
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
  render_active_refs=active;
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
 render_active_refs=-1;return 1;
}
