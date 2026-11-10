/* PLJ110 v062 MCU controller and sole deferred HIF worker.
 * Direct accesses match gps_drv_dl_v062.ko, not the alternative ATF-MCU
 * variant. conninfra global EMI remap remains the supplier's secure-world
 * responsibility (actual conninfra SMC c2000534/op70, HW02050800/ADie6686).
 * Missing supplier is an observable wait, never a fake successful lease.
 * No backend registration here: transport ACK is not MNL/PDS readiness. */
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_types.h>
#include "x9_ufs/sha256.h" /* existing linked SHA256, no duplicate crypto */
#include "x9_gps_native_power.h"
#include "x9_gps_flow.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
static LCK_MTX_DECLARE(gps_runtime_lock, &gps_group);
_Static_assert(__builtin_offsetof(struct x9_gps_connv2_ops,pos_clock_schematic)==64,
    "connv2 version-1 descriptor prefix");
_Static_assert(__builtin_offsetof(struct x9_gps_connv2_ops,pos_afe)==88,
    "connv2 version-2 descriptor prefix");
_Static_assert(__builtin_offsetof(struct x9_gps_connv2_ops,pos_radio)==96,
    "connv2 version-3 descriptor prefix");
_Static_assert(__builtin_offsetof(struct x9_gps_connv2_ops,pos_afe_verify)==120,
    "connv2 version-4 descriptor prefix");
_Static_assert(sizeof(struct x9_gps_connv2_ops)==128,"connv2 version-5 descriptor");
_Static_assert(sizeof(struct x9_gps_handoff)==X9_GPS_HANDOFF_BYTES,
    "GPS LK handoff remains 64B");
struct gps_mcu_field { uint32_t physical, mask, shift; };
enum gps_mcu_field_id {
 F_bgf_state,
 F_wake,
 F_version,
 F_ready,
 F_reset,
 F_function,
 F_sample,
 F_auto_dis,
 F_ahb,
 F_osc_state,
 F_power_state,
 F_owner,
 F_clear_owner,
 F_emi,
 F_mon,
 F_system,
 F_pll,
 F_div_enable,
 F_div,
 F_hclk,
 F_c_rx,
 F_c_rx_dis,
 F_c_tx,
 F_g_rx,
 F_g_tx,
 F_g_tx_dis,
 F_a_rx,
 F_a_tx,
 F_a_tx_dis,
 F_c_rx_st,
 F_c_tx_st,
 F_g_rx_st,
 F_g_tx_st,
 F_a_rx_st,
 F_a_tx_st,
};
/* v062 CODA fields; GPS bus addresses translated through the actual module
 * NEW_AP_BASE table (80000000->40c00000, not legacy 18c00000). */
static const struct gps_mcu_field gps_mcu_fields[] = {
 { 0x40060a10u, 0xc0000u, 18 }, /* bgf_state */
 { 0x400601acu, 0x1u, 0 }, /* wake */
 { 0x40011000u, 0xffffffffu, 0 }, /* CONN_CFG_IP_VERSION, not RGU */
 { 0x40001210u, 0x10000u, 16 }, /* ready */
 { 0x40000130u, 0x1u, 0 }, /* reset */
 { 0x4000120cu, 0x1u, 0 }, /* function */
 { 0x40c16038u, 0x10u, 4 }, /* sample */
 { 0x40c00100u, 0x80000000u, 31 }, /* auto_dis */
 { 0x40c00104u, 0x4u, 2 }, /* ahb */
 { 0x40c2100cu, 0xfu, 0 }, /* osc_state */
 { 0x40c2102cu, 0xfu, 0 }, /* power_state */
 { 0x40060030u, 0x4u, 2 }, /* owner */
 { 0x40060030u, 0x2u, 1 }, /* clear_owner */
 { 0x40068354u, 0xfffffu, 0 }, /* emi */
 { 0x40c12030u, 0x1u, 0 }, /* mon */
 { 0x40c12054u, 0xffffffffu, 0 }, /* system */
 { 0x40c12104u, 0x1u, 0 }, /* pll */
 { 0x40c12104u, 0x20u, 5 }, /* div_enable */
 { 0x40c12104u, 0x3f00u, 8 }, /* div */
 { 0x40c12100u, 0x3u, 0 }, /* hclk */
 { 0x40001470u, 0x10u, 4 }, /* c_rx */
 { 0x40001470u, 0x20u, 5 }, /* c_rx_dis */
 { 0x40001470u, 0x1u, 0 }, /* c_tx */
 { 0x40001480u, 0x10u, 4 }, /* g_rx */
 { 0x40001480u, 0x1u, 0 }, /* g_tx */
 { 0x40001480u, 0x2u, 1 }, /* g_tx_dis */
 { 0x40001488u, 0x10u, 4 }, /* a_rx */
 { 0x40001488u, 0x1u, 0 }, /* a_tx */
 { 0x40001488u, 0x2u, 1 }, /* a_tx_dis */
 { 0x40001474u, 0x400000u, 22 }, /* c_rx_st */
 { 0x40001474u, 0x800000u, 23 }, /* c_tx_st */
 { 0x40001484u, 0x400000u, 22 }, /* g_rx_st */
 { 0x40001484u, 0x800000u, 23 }, /* g_tx_st */
 { 0x40001490u, 0x400000u, 22 }, /* a_rx_st */
 { 0x40001490u, 0x800000u, 23 }, /* a_tx_st */
};
struct gps_mcu_bank { uint32_t physical; volatile uint32_t *p; };
/* Actual v062 bus2 version read is CONN_CFG+0. Keep CONN_RGU_ON mapped
 * separately: its +0x28 keyed reset control is still used below. */
static const uint32_t gps_mcu_bases[]={0x40000000,0x40001000,0x40011000,
    0x40060000,0x40068000,0x40c00000,0x40c12000,0x40c16000,0x40c21000};
enum { GPS_MCU_BANK_COUNT=sizeof(gps_mcu_bases)/sizeof(gps_mcu_bases[0]) };
/* Mapped only for a topology-qualified version-2..5 cold POS supplier. */
static const uint32_t gps_pos_bases[]={0x4004b000,0x40050000,0x40051000};
enum { GPS_POS_BANK_COUNT=sizeof(gps_pos_bases)/sizeof(gps_pos_bases[0]) };
/* WT_SLP and AFE are NEVER mapped for the old v1/v2 suppliers. */
static const uint32_t gps_rf_bases[]={0x40003000,0x40041000};
enum { GPS_RF_BANK_COUNT=sizeof(gps_rf_bases)/sizeof(gps_rf_bases[0]) };
/* Only v4/v5 map these; the RC bank is read-only in the native POS path. */
static const uint32_t gps_lp_bases[]={0x4000e000,0x40012000,0x4004d000,0x1c011000};
enum { GPS_LP_BANK_COUNT=sizeof(gps_lp_bases)/sizeof(gps_lp_bases[0]) };
#define GPS_MCU_MT6993_VERSION 0x02050800u
struct gps_runtime_packet { uint64_t time; unsigned length; uint8_t bytes[4096]; };
struct gps_emi_cpu_region {
 uint64_t start,end,aid_set;
 uint32_t enabled;
};
static struct {
 struct x9_gps_connv2_ops provider, ops;
 struct x9_gps_handoff layout;
 struct gps_mcu_bank banks[GPS_MCU_BANK_COUNT];
 struct gps_mcu_bank pos_banks[GPS_POS_BANK_COUNT];
 struct gps_mcu_bank rf_banks[GPS_RF_BANK_COUNT];
 struct gps_mcu_bank lp_banks[GPS_LP_BANK_COUNT];
 struct x9_gps_hif hif; /* ~70KiB, never on a kernel/QRTR stack */
 volatile uint8_t *ap, *mcu, *ccif;
 uint64_t generation, lease;
 unsigned launched, held_reset, remapped, image, booted;
 unsigned pos_started, pos_complete;
 unsigned pos_d_die_complete, pos_spi_started, pos_spi_complete;
 unsigned pos_afe_started, pos_afe_complete, pos_pll_started, pos_pll_complete;
 unsigned pos_radio_started, pos_radio_complete, pos_lp_started, pos_lp_complete;
 unsigned pos_rc_valid, pos_rc_mode, pos_cmdbt_valid, pos_lp_hw_phase;
 struct x9_gps_afe_smc_result afe_result; /* raw returns, NOT qualification */
 struct {
  volatile uint32_t *top, *hwv;
  unsigned started, set_started, complete, borrowed;
  uint32_t prior, selector, ap, global, busy, done, ack;
 } clock;
 struct {
  volatile uint32_t *host, *cfg_on, *debug, *cfg;
  unsigned started, complete, wake_started, wake_borrowed, detect_attempts;
  uint32_t wake_prior, wake, detect, version, irq, restore_required, restore;
  int error;
 } bus;
 struct {
  /* Off-stack snapshots, read only under the sole physical worker. */
  struct gps_emi_cpu_region baseline[64], sample[64];
  unsigned started,valid,region,intersections;
  uint64_t checks;
  int error;
 } emi_cpu;
 uint8_t cmdbt[4096]; /* immutable registered snapshot, never on stack */
 thread_t worker;
 unsigned in_callback, out_head[2], out_count[2];
 struct gps_runtime_packet outbox[2][2];
 struct gps_native_flow flow;
} gps_rt;
static void gps_runtime_pause(unsigned us)
{
 uint64_t deadline; clock_interval_to_deadline(us,NSEC_PER_USEC,&deadline);
 (void)assert_wait_deadline((event_t)&gps_rt.hif,THREAD_UNINT,deadline);
 thread_block(THREAD_CONTINUE_NULL);
}
#include "x9_gps_power.inc"
static void gps_runtime_stage(unsigned stage, int error)
{
 lck_mtx_lock(&gps_lock);
 st.physical_stage=stage; st.physical_errno=error;
 lck_mtx_unlock(&gps_lock);
 printf("X9 GPS physical: stage=%u errno=%d\n",stage,error);
}
static int gps_mcu_supplier_guard(void)
{
 struct x9_gps_status status; x9_gps_copy_status(&status);
 if (!gps_rt.lease || !gps_rt.ops.validate) return ENXIO;
 if (status.hardware_generation!=gps_rt.generation || status.fault) return ESTALE;
 return gps_rt.ops.validate(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease);
}
#include "x9_gps_clock.inc"
#include "x9_gps_bus.inc"
static int gps_mcu_guard(void)
{
 int e=gps_mcu_supplier_guard();if(e)return e;
 if(gps_rt.bus.started) {e=gps_bus_validate();if(e)return e;}
 /* After the native selector transaction, every MCU/POS/HIF access also
  * needs its live clock effect. Never repair a lost vote or selector. */
 return gps_rt.clock.complete ? gps_clock_check() : 0;
}
#include "x9_gps_emi.inc"
static volatile uint32_t *gps_mcu_pointer(uint32_t physical)
{
 for (unsigned i=0;i<GPS_MCU_BANK_COUNT;i++)
  if (physical>=gps_rt.banks[i].physical &&
      physical-gps_rt.banks[i].physical<0x1000 && !(physical&3))
   return gps_rt.banks[i].p ?
       gps_rt.banks[i].p+(physical-gps_rt.banks[i].physical)/4 : NULL;
 if(gps_rt.ops.version>=2)for(unsigned i=0;i<GPS_POS_BANK_COUNT;i++)
  if(physical>=gps_rt.pos_banks[i].physical &&
     physical-gps_rt.pos_banks[i].physical<0x1000 && !(physical&3))
   return gps_rt.pos_banks[i].p ?
       gps_rt.pos_banks[i].p+(physical-gps_rt.pos_banks[i].physical)/4 : NULL;
 if(gps_rt.ops.version>=3)for(unsigned i=0;i<GPS_RF_BANK_COUNT;i++)
  if(physical>=gps_rt.rf_banks[i].physical &&
     physical-gps_rt.rf_banks[i].physical<0x1000 && !(physical&3))
   return gps_rt.rf_banks[i].p ?
       gps_rt.rf_banks[i].p+(physical-gps_rt.rf_banks[i].physical)/4 : NULL;
 if(gps_rt.ops.version>=4)for(unsigned i=0;i<GPS_LP_BANK_COUNT;i++)
  if(physical>=gps_rt.lp_banks[i].physical &&
     physical-gps_rt.lp_banks[i].physical<0x1000 && !(physical&3))
   return gps_rt.lp_banks[i].p ?
       gps_rt.lp_banks[i].p+(physical-gps_rt.lp_banks[i].physical)/4 : NULL;
 return NULL;
}
static int gps_mcu_read(uint32_t physical, uint32_t *out)
{
 int e=gps_mcu_guard(); if (e) return e;
 volatile uint32_t *p=gps_mcu_pointer(physical); if (!p) return EINVAL;
 *out=*p; __asm__ volatile("dsb sy" ::: "memory"); return 0;
}
static int gps_mcu_write(uint32_t physical, uint32_t value)
{
 int e=gps_mcu_guard(); if (e) return e;
 volatile uint32_t *p=gps_mcu_pointer(physical); if (!p) return EINVAL;
 *p=value; __asm__ volatile("dsb sy" ::: "memory"); return 0;
}
static int gps_mcu_set(enum gps_mcu_field_id id, uint32_t value)
{
 const struct gps_mcu_field *f=&gps_mcu_fields[id]; uint32_t v;
 if (value>(f->mask>>f->shift)) return EINVAL;
 int e=gps_mcu_read(f->physical,&v); if (e) return e;
 return gps_mcu_write(f->physical,(v&~f->mask)|((value<<f->shift)&f->mask));
}
static int gps_mcu_expect(enum gps_mcu_field_id id, uint32_t value)
{
 const struct gps_mcu_field *f=&gps_mcu_fields[id]; uint32_t v;
 int e=gps_mcu_read(f->physical,&v); if (e) return e;
 return ((v&f->mask)>>f->shift)==value ? 0 : EIO;
}
static int gps_mcu_poll(enum gps_mcu_field_id id, uint32_t value)
{
 /* v062 default hardware polls are bounded; never inside an IRQ/spinlock. */
 uint64_t deadline; clock_interval_to_deadline(20,NSEC_PER_MSEC,&deadline);
 for (unsigned i=0;i<=20;i++) {
  if (i && mach_absolute_time()>=deadline) return ETIMEDOUT;
  int e=gps_mcu_expect(id,value);
  if (!e) return 0;
  if (e!=EIO) return e;
  if (i!=20) gps_runtime_pause(1000);
 }
 return ETIMEDOUT;
}
static int gps_mcu_raw_expect(uint32_t address, uint32_t mask, uint32_t value)
{
 uint32_t v; int e=gps_mcu_read(address,&v); if (e) return e;
 return (v&mask)==value ? 0 : EIO;
}
static int gps_mcu_map(void)
{
 int e=gps_mcu_guard(); if (e) return e;
 if(gps_rt.ops.version>=4) {
  gps_runtime_stage(X9_GPS_PHYSICAL_BUS,0);
  e=gps_bus_take();if(e)return e;
 }
 for (unsigned i=0;i<GPS_MCU_BANK_COUNT;i++) {
  e=gps_mcu_guard();if(e)return e;
  gps_rt.banks[i].physical=gps_mcu_bases[i];
  volatile uint32_t *p=NULL;
  if(gps_rt.bus.complete) {
   if(gps_mcu_bases[i]==0x40060000)p=gps_rt.bus.host;
   else if(gps_mcu_bases[i]==0x40001000)p=gps_rt.bus.cfg_on;
   else if(gps_mcu_bases[i]==0x40011000)p=gps_rt.bus.cfg;
  }
  gps_rt.banks[i].p=p ? p :
      (volatile uint32_t *)ml_io_map(gps_mcu_bases[i],0x1000);
  if (!gps_rt.banks[i].p) return ENOMEM;
 }
 if(gps_rt.ops.version>=2) {
  if(!(gps_rt.layout.flags&X9_GPS_POS_LAYOUT_VERIFIED))return ENXIO;
  for(unsigned i=0;i<GPS_POS_BANK_COUNT;i++) {
   e=gps_mcu_guard();if(e)return e;
   gps_rt.pos_banks[i].physical=gps_pos_bases[i];
   gps_rt.pos_banks[i].p=(volatile uint32_t *)ml_io_map(gps_pos_bases[i],0x1000);
   if(!gps_rt.pos_banks[i].p)return ENOMEM;
  }
 }
 if(gps_rt.ops.version>=3) {
  if(!(gps_rt.layout.flags&X9_GPS_RF_LAYOUT_VERIFIED))return ENXIO;
  for(unsigned i=0;i<GPS_RF_BANK_COUNT;i++) {
   e=gps_mcu_guard();if(e)return e;
   gps_rt.rf_banks[i].physical=gps_rf_bases[i];
   gps_rt.rf_banks[i].p=(volatile uint32_t *)ml_io_map(gps_rf_bases[i],0x1000);
   if(!gps_rt.rf_banks[i].p)return ENOMEM;
  }
 }
 if(gps_rt.ops.version>=4) {
  if(!(gps_rt.layout.flags&X9_GPS_LP_LAYOUT_VERIFIED))return ENXIO;
  for(unsigned i=0;i<GPS_LP_BANK_COUNT;i++) {
   e=gps_mcu_guard();if(e)return e;
   gps_rt.lp_banks[i].physical=gps_lp_bases[i];
   gps_rt.lp_banks[i].p=(volatile uint32_t *)ml_io_map(gps_lp_bases[i],0x1000);
   if(!gps_rt.lp_banks[i].p)return ENOMEM;
  }
 }
 /* Device attributes throughout; no cached aliases of shared HIF RAM.
  * The 48MiB advertised total is deliberately not mapped as one reserve. */
 if(gps_rt.ops.version==5) {
  gps_runtime_stage(X9_GPS_PHYSICAL_EMI_CPU,0);
  e=gps_emi_cpu_take();if(e)return e;
 }
 e=gps_mcu_guard();if(e)return e;
 gps_rt.ap=(volatile uint8_t *)ml_io_map(gps_rt.layout.reserve_base+
     gps_rt.layout.ap2mcu_offset,gps_rt.layout.region_size);
 e=gps_mcu_guard();if(e)return e;
 gps_rt.mcu=(volatile uint8_t *)ml_io_map(gps_rt.layout.reserve_base+
     gps_rt.layout.mcu2ap_offset,gps_rt.layout.region_size);
 e=gps_mcu_guard();if(e)return e;
 gps_rt.ccif=(volatile uint8_t *)ml_io_map(gps_rt.layout.ccif_base,
     gps_rt.layout.ccif_size);
 return gps_rt.ap && gps_rt.mcu && gps_rt.ccif ? 0 : ENOMEM;
}
#define MCU_TRY(call) do { int e_=(call); if(e_)return e_; } while(0)
#include "x9_gps_pos.inc"
#include "x9_gps_low_power.inc"
static int gps_mcu_wake(void)
{
 MCU_TRY(gps_mcu_set(F_wake,1));
 MCU_TRY(gps_mcu_expect(F_wake,1));
 /* Vendor wake qualifies version and ready with separate bounded polls.
  * PLJ110 requires MT6993; do not accept the module's other-board version. */
 MCU_TRY(gps_mcu_poll(F_version,GPS_MCU_MT6993_VERSION));
 MCU_TRY(gps_mcu_poll(F_ready,1));
 /* Keep force-wake and driver ownership for the full session. No FW-own
  * relinquishment timer until the complete sleep/DMA state machine exists. */
 int e=gps_mcu_expect(F_owner,0);
 if (e && e!=EIO) return e;
 if (e) {
  MCU_TRY(gps_mcu_set(F_clear_owner,1));
  MCU_TRY(gps_mcu_poll(F_owner,0));
 }
 return 0;
}
static int gps_mcu_hold_reset(void)
{
 uint32_t v;
 MCU_TRY(gps_mcu_read(0x4000110c,&v));
 MCU_TRY(gps_mcu_write(0x4000110c,v&~0x20000u)); /* MCU mode, not AP */
 MCU_TRY(gps_mcu_set(F_reset,0));
 MCU_TRY(gps_mcu_expect(F_reset,0)); gps_rt.held_reset=1;
 MCU_TRY(gps_mcu_read(0x40000028,&v));
 MCU_TRY(gps_mcu_write(0x40000028,0x42540000|(v&0xffff)|0x80));
 MCU_TRY(gps_mcu_poll(F_bgf_state,1));
 MCU_TRY(gps_mcu_set(F_function,1));
 /* Disable and read back each direction independently in the vendor order. */
 MCU_TRY(gps_mcu_set(F_c_rx_dis,1));
 MCU_TRY(gps_mcu_set(F_c_rx,0)); MCU_TRY(gps_mcu_poll(F_c_rx_st,0));
 MCU_TRY(gps_mcu_set(F_c_rx_dis,0));
 MCU_TRY(gps_mcu_set(F_c_tx,0)); MCU_TRY(gps_mcu_poll(F_c_tx_st,0));
 MCU_TRY(gps_mcu_set(F_g_rx,0)); MCU_TRY(gps_mcu_poll(F_g_rx_st,0));
 MCU_TRY(gps_mcu_set(F_g_tx_dis,1));
 MCU_TRY(gps_mcu_set(F_g_tx,0)); MCU_TRY(gps_mcu_poll(F_g_tx_st,0));
 MCU_TRY(gps_mcu_set(F_g_tx_dis,0));
 MCU_TRY(gps_mcu_set(F_a_rx,0)); MCU_TRY(gps_mcu_poll(F_a_rx_st,0));
 MCU_TRY(gps_mcu_set(F_a_tx_dis,1));
 MCU_TRY(gps_mcu_set(F_a_tx,0)); MCU_TRY(gps_mcu_poll(F_a_tx_st,0));
 MCU_TRY(gps_mcu_set(F_a_tx_dis,0));
 MCU_TRY(gps_mcu_raw_expect(0x40c12010,UINT32_MAX,0x03040000));
 MCU_TRY(gps_mcu_set(F_sample,1));
 MCU_TRY(gps_mcu_set(F_auto_dis,0)); MCU_TRY(gps_mcu_set(F_ahb,1));
 MCU_TRY(gps_mcu_read(0x40c21010,&v));
 MCU_TRY(gps_mcu_write(0x40c21010,0xaa000000|(v&0xffff)|0x20));
 MCU_TRY(gps_mcu_poll(F_osc_state,1));
 MCU_TRY(gps_mcu_poll(F_power_state,2));
 return gps_mcu_expect(F_reset,0);
}
static int gps_mcu_remap(void)
{
 MCU_TRY(gps_mcu_expect(F_reset,0));
 MCU_TRY(gps_mcu_set(F_emi,gps_rt.layout.reserve_base>>16));
 MCU_TRY(gps_mcu_expect(F_emi,gps_rt.layout.reserve_base>>16));
 MCU_TRY(gps_mcu_write(0x40c16014,0x02200000));
 MCU_TRY(gps_mcu_raw_expect(0x40c16014,UINT32_MAX,0x02200000));
 gps_rt.remapped=1; return 0;
}
static int gps_mcu_pll(void)
{
 MCU_TRY(gps_mcu_set(F_mon,1)); MCU_TRY(gps_mcu_set(F_system,0));
 MCU_TRY(gps_mcu_write(0x40c12034,0xc)); MCU_TRY(gps_mcu_set(F_pll,1));
 static const uint32_t selects[4]={0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c};
 for (unsigned i=0;i<4;i++) MCU_TRY(gps_mcu_write(0x40c12034+i*4,selects[i]));
 unsigned ready=0;
 uint64_t deadline; clock_interval_to_deadline(10,NSEC_PER_MSEC,&deadline);
 for (unsigned i=0;i<30;i++) {
  if (i && mach_absolute_time()>=deadline) break;
  uint32_t v; MCU_TRY(gps_mcu_read(0x40c12018,&v));
  if (v&((1u<<28)|(1u<<12))) { ready=1; break; }
  gps_runtime_pause(200);
 }
 MCU_TRY(gps_mcu_set(F_mon,0)); MCU_TRY(gps_mcu_set(F_system,0));
 MCU_TRY(gps_mcu_write(0x40c12034,0));
 /* Android warns and continues; this bring-up fails closed instead of
  * releasing reset with an unqualified RF/MCU clock. */
 if (!ready) return ETIMEDOUT;
 MCU_TRY(gps_mcu_set(F_div_enable,1)); MCU_TRY(gps_mcu_set(F_div,5));
 MCU_TRY(gps_mcu_set(F_hclk,2));
 MCU_TRY(gps_mcu_expect(F_div_enable,1)); MCU_TRY(gps_mcu_expect(F_div,5));
 return gps_mcu_expect(F_hclk,2);
}
static int gps_mcu_boot(void)
{
 if (!gps_rt.image || !gps_rt.remapped || !gps_rt.held_reset) return ENXIO;
 MCU_TRY(gps_mcu_expect(F_reset,0));
 /* No blanket CCIF ACK/reset. The exclusive supplier must provide a fresh
  * quiesced baseline; unknown outstanding channels require their handlers. */
 MCU_TRY(gps_mcu_guard());
 if (hif_read(gps_rt.ccif,4) || hif_read(gps_rt.ccif,0x10)) return EBUSY;
 MCU_TRY(gps_mcu_set(F_reset,1));
 gps_rt.held_reset=0; MCU_TRY(gps_mcu_expect(F_reset,1));
 uint64_t deadline; clock_interval_to_deadline(500,NSEC_PER_MSEC,&deadline);
 for (unsigned i=0;i<=500;i++) {
  if (i && mach_absolute_time()>=deadline) return ETIMEDOUT;
  uint32_t a,b;
  MCU_TRY(gps_mcu_read(0x40c0016c,&a));
  MCU_TRY(gps_mcu_read(0x40c0016c,&b));
  if (a==0x1d1e || b==0x1d1e) { gps_rt.booted=1; return 0; }
  if (i!=500) gps_runtime_pause(1000);
 }
 return ETIMEDOUT;
}
static int gps_runtime_owned(void *ctx, uint64_t generation, int reset)
{
 (void)ctx;
 if (generation!=gps_rt.generation || !gps_rt.remapped ||
     (reset && !gps_rt.held_reset) || (!reset && !gps_rt.booted)) return ESTALE;
 MCU_TRY(gps_mcu_guard());
 MCU_TRY(gps_mcu_expect(F_wake,1)); MCU_TRY(gps_mcu_expect(F_ready,1));
 MCU_TRY(gps_mcu_expect(F_owner,0));
 MCU_TRY(gps_mcu_expect(F_reset,reset ? 0 : 1));
 MCU_TRY(gps_mcu_expect(F_function,1)); MCU_TRY(gps_mcu_expect(F_bgf_state,1));
 MCU_TRY(gps_mcu_expect(F_emi,gps_rt.layout.reserve_base>>16));
 MCU_TRY(gps_mcu_raw_expect(0x40c16014,UINT32_MAX,0x02200000));
 MCU_TRY(gps_mcu_expect(F_c_rx_st,0)); MCU_TRY(gps_mcu_expect(F_c_tx_st,0));
 /* Per-I/O ownership qualification must fail immediately, not sleep/retry. */
 MCU_TRY(gps_mcu_expect(F_version,GPS_MCU_MT6993_VERSION));
 MCU_TRY(gps_mcu_raw_expect(0x40c12010,UINT32_MAX,0x03040000));
 MCU_TRY(gps_mcu_expect(F_osc_state,1)); MCU_TRY(gps_mcu_expect(F_power_state,2));
 if(gps_rt.ops.version==5)MCU_TRY(gps_emi_cpu_validate());
 /* The lease holds resources across this check and subsequent HIF accesses. */
 if ((hif_read(gps_rt.ccif,4)|hif_read(gps_rt.ccif,0x10))&~0xffu) return EIO;
 return 0;
}
#include "x9_gps_flow.inc"
static int gps_runtime_control(void *ctx,unsigned ch,uint8_t type,
    const uint8_t *p,unsigned n,uint64_t now)
{
 if(gps_rt.ops.version==5 && ch==X9_GPS_HIF_NORMAL &&
     (type==0x35 || type==0x36)) {
  int e=gps_flow_control(type,p,n);gps_flow_journal(e);return e;
 }
 (void)ctx; gps_rt.in_callback=1;
 int e=gps_rt.ops.control(gps_rt.ops.context,ch,type,p,n,now);
 gps_rt.in_callback=0; return e;
}
static int gps_runtime_mnl(void *ctx,const uint8_t *p,unsigned n,uint64_t now)
{
 /* Captured at RX POST and first inner fragment, not at callback arrival.
  * A late old/mixed PVT must not publish or retire the new session's fix. */
 uint64_t application=gps_rt.hif.stream[0].mnl_application_generation;
 (void)ctx; gps_rt.in_callback=1;
 int e=gps_rt.ops.mnl(gps_rt.ops.context,p,n,now);
 gps_rt.in_callback=0;
 /* The physical handler must first accept this report in its real MNL
  * session. Do not turn unsolicited event2/management traffic into READY.
  * Unknown events still require that handler; no blanket acknowledgement. */
 if (e || n<2 || gps_pvt_u16(p)!=2) return e;
 uint8_t wire[X9_GPS_PDS_POSITION_BYTES];
 e=x9_gps_mnl_position_wire(p,n,wire,sizeof(wire));
 int no_fix=e==ENOENT;
 lck_mtx_lock(&gps_lock);
 if(st.hardware_generation!=gps_rt.generation || st.fault) {
  lck_mtx_unlock(&gps_lock); return ESTALE;
 }
 st.pvt_received++;
 if(!application || st.generation!=application) {
  st.pvt_retired++;
  lck_mtx_unlock(&gps_lock);return 0;
 }
 if(no_fix) {
  st.pvt_no_fix++;
  /* A newly accepted no-fix report retires queued OLD fixes immediately.
   * Preserve the sequence/client cursors and the acknowledged tracking
   * session: loss of a fix is not receiver/DMA shutdown or a fault. */
  position_head=position_count=0;
 }
 else if(e)st.pvt_rejected++;
 st.pvt_errno=e; st.pvt_valid_mask=e ? 0 : gps_pvt_u32(wire);
 int publish=!e && st.ready && st.tracking && st.position_supported;
 lck_mtx_unlock(&gps_lock);
 if(no_fix)return 0; /* legitimate no-fix does not poison the HIF */
 if(e)return e;
 if(!publish)return 0; /* parsed bytes cannot grant a backend/output lease */
 e=x9_gps_deliver_position(application,wire,sizeof(wire),now);
 lck_mtx_lock(&gps_lock);
 int retired=st.hardware_generation==gps_rt.generation && !st.fault &&
     (e==ESTALE || e==ENXIO);
 if(retired)st.pvt_retired++;
 if(st.generation==application && !st.fault) {
  st.pvt_errno=e;
  if(!e)st.pvt_delivered++;
 }
 lck_mtx_unlock(&gps_lock);
 return retired ? 0 : e;
}
static int gps_runtime_ccif(void *ctx,unsigned ch,uint64_t now)
{
 (void)ctx; gps_rt.in_callback=1;
 int e=gps_rt.ops.other_ccif(gps_rt.ops.context,ch,now);
 gps_rt.in_callback=0; return e;
}
int x9_gps_runtime_queue(uint64_t generation,unsigned ch,const void *p,unsigned n)
{
 if (!p || !n || n>4096 || ch<1 || ch>2) return EINVAL;
 if (current_thread()!=gps_rt.worker || !gps_rt.in_callback) return EPERM;
 if (generation!=gps_rt.generation || gps_rt.hif.fault) return ESTALE;
 if(gps_rt.ops.version==5) {
  int e=gps_flow_queue(ch-1,p,n,0);gps_flow_journal(0);return e;
 }
 unsigned c=ch-1; if (gps_rt.out_count[c]==2) return ENOBUFS;
 struct gps_runtime_packet *packet=&gps_rt.outbox[c][
     (gps_rt.out_head[c]+gps_rt.out_count[c])%2];
 packet->length=n; packet->time=mach_absolute_time(); memcpy(packet->bytes,p,n);
 gps_rt.out_count[c]++; return 0;
}
static int gps_runtime_drain(void)
{
 int ack=x9_gps_hif_management_result(&gps_rt.hif,1);
 if (ack && ack!=EAGAIN) return ack;
 if(gps_rt.ops.version==5) {
  int e=gps_flow_drain(ack,mach_absolute_time());gps_flow_journal(e);return e;
 }
 for (unsigned c=0;c<2;c++) {
  if (!gps_rt.out_count[c]) continue;
  struct gps_runtime_packet *p=&gps_rt.outbox[c][gps_rt.out_head[c]];
  uint64_t now=mach_absolute_time(),ns;
  absolutetime_to_nanoseconds(now-p->time,&ns);
  if (ns>1500000000ULL) return ETIMEDOUT;
  if (ack) continue;
  int e=x9_gps_hif_send(&gps_rt.hif,c+1,p->bytes,p->length,now);
  if (e==EAGAIN || e==EBUSY) continue; /* never block the other channel */
  if (e) return e;
  gps_rt.out_head[c]=(gps_rt.out_head[c]+1)%2; gps_rt.out_count[c]--;
 }
 return 0;
}
static int gps_runtime_bringup(void)
{
 struct x9_gps_status status; x9_gps_copy_status(&status);
 gps_rt.generation=status.hardware_generation;
 MCU_TRY(x9_gps_copy_handoff(&gps_rt.layout));
 gps_runtime_stage(X9_GPS_PHYSICAL_ACQUIRE,0);
 MCU_TRY(gps_rt.ops.acquire(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,&gps_rt.lease));
 MCU_TRY(gps_mcu_map());
 if(gps_rt.ops.version>=2) {
  if(gps_rt.layout.flags&X9_GPS_CLOCK_LAYOUT_VERIFIED) {
   gps_runtime_stage(X9_GPS_PHYSICAL_POS_CLOCK,0);
   MCU_TRY(gps_mcu_expect(F_reset,0));MCU_TRY(gps_mcu_expect(F_function,0));
   MCU_TRY(gps_clock_take());
  }
  gps_runtime_stage(X9_GPS_PHYSICAL_D_DIE,0);
  MCU_TRY(gps_pos_d_die());
  if(gps_rt.ops.version>=3) {
   gps_runtime_stage(X9_GPS_PHYSICAL_POS_SPI,0);
   MCU_TRY(gps_pos_spi());
   gps_runtime_stage(X9_GPS_PHYSICAL_POS_AFE,0);
   MCU_TRY(gps_pos_afe());
   gps_runtime_stage(X9_GPS_PHYSICAL_POS_PLL,0);
   MCU_TRY(gps_pos_pll());
  }
  if(gps_rt.ops.version>=4) {
   gps_runtime_stage(X9_GPS_PHYSICAL_POS_RADIO,0);
   MCU_TRY(gps_pos_radio());
   gps_runtime_stage(X9_GPS_PHYSICAL_POS_LOW_POWER,0);
   MCU_TRY(gps_pos_low_power());
  }
  gps_runtime_stage(X9_GPS_PHYSICAL_POS_FINALIZE,0);
  MCU_TRY(gps_rt.ops.pos_finalize(gps_rt.ops.context,&gps_rt.layout,
      gps_rt.generation,gps_rt.lease));
  MCU_TRY(gps_pos_guard());
  if(gps_rt.ops.version>=4)MCU_TRY(gps_pos_lp_guard());
  MCU_TRY(gps_mcu_expect(F_reset,0));MCU_TRY(gps_mcu_expect(F_function,0));
  gps_rt.pos_complete=1;
 }
 gps_runtime_stage(X9_GPS_PHYSICAL_WAKE,0); MCU_TRY(gps_mcu_wake());
 gps_runtime_stage(X9_GPS_PHYSICAL_RESET,0); MCU_TRY(gps_mcu_hold_reset());
 gps_runtime_stage(X9_GPS_PHYSICAL_REMAP,0); MCU_TRY(gps_mcu_remap());
 gps_runtime_stage(X9_GPS_PHYSICAL_IMAGE,0);
 MCU_TRY(gps_rt.ops.provision(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease));
 MCU_TRY(gps_mcu_expect(F_reset,0)); gps_rt.image=1;
 const struct x9_gps_hif_ops ops={gps_runtime_owned,gps_runtime_control,
     gps_runtime_mnl,gps_runtime_ccif,NULL};
 /* Prepare host RAM BEFORE releasing reset. Never clear MCU-owned RAM. */
 MCU_TRY(x9_gps_hif_prepare(&gps_rt.hif,&ops,gps_rt.ap,gps_rt.mcu,
     gps_rt.ccif,gps_rt.generation));
 if(gps_rt.ops.version==5)MCU_TRY(gps_flow_init());
 gps_runtime_stage(X9_GPS_PHYSICAL_PLL,0); MCU_TRY(gps_mcu_pll());
 gps_runtime_stage(X9_GPS_PHYSICAL_IDLE,0); MCU_TRY(gps_mcu_boot());
 gps_runtime_stage(X9_GPS_PHYSICAL_HIF,0);
 MCU_TRY(x9_gps_hif_start(&gps_rt.hif,mach_absolute_time()));
 gps_runtime_stage(X9_GPS_PHYSICAL_TRANSPORT,0);
 /* This worker does NOT publish ready on idle, fresh HIF, or command1 ACK.
  * Complete MCUSYS/MNL application state and measured PDS translation still
  * belong to a future capable backend. No live-rebind of posted DMA. */
 for (;;) {
  MCU_TRY(x9_gps_hif_poll(&gps_rt.hif,mach_absolute_time()));
  MCU_TRY(gps_runtime_drain()); gps_runtime_pause(1000);
 }
}
#undef MCU_TRY
static void gps_runtime_worker(void *unused, wait_result_t result)
{
 (void)unused; (void)result;
 gps_rt.worker=current_thread();
 /* Explicit diagnostic, default OFF. It observes only AON HWV and SID3
  * via serialized PMIF-P reads, never takes power or touches CONN/EMI.
  * A stable inherited vote cannot replace the actual connv2 supplier. */
 if(sweet_x9_arg("x9_gps_power",0)==1)gps_power_observe();
 lck_mtx_lock(&gps_runtime_lock);
 while (!gps_rt.provider.version)
  lck_mtx_sleep(&gps_runtime_lock,LCK_SLEEP_DEFAULT,
      (event_t)&gps_rt.provider.version,THREAD_UNINT);
 gps_rt.ops=gps_rt.provider;
 lck_mtx_unlock(&gps_runtime_lock);
 int e=gps_runtime_bringup();
 if(gps_rt.ops.version==5)gps_flow_journal(e);
 /* Never auto-retry, clear descriptors, release power, or unmap uncertain
  * DMA. Even a pre-HIF partial power failure retains the supplier's lease. */
 gps_runtime_stage(X9_GPS_PHYSICAL_QUARANTINE,e);
 x9_gps_hardware_fault(gps_rt.generation,e);
 thread_terminate(current_thread());
}
static void gps_runtime_start(void)
{
 lck_mtx_lock(&gps_runtime_lock);
 if (!gps_rt.launched) {
  gps_rt.launched=1; thread_t thread=THREAD_NULL;
  kern_return_t k=kernel_thread_start(gps_runtime_worker,NULL,&thread);
  if (k==KERN_SUCCESS) thread_deallocate(thread);
  else gps_runtime_stage(X9_GPS_PHYSICAL_QUARANTINE,ENOMEM);
 }
 lck_mtx_unlock(&gps_runtime_lock);
}
int x9_gps_register_connv2(const struct x9_gps_connv2_ops *ops)
{
 if (!ops || ops->version<1 || ops->version>5 || !ops->acquire || !ops->validate ||
     !ops->provision || !ops->control || !ops->mnl || !ops->other_ccif) return EINVAL;
 if(ops->version>=2) {
  if(ops->pos_clock_schematic>3 || !ops->pos_validate || !ops->pos_finalize)return EINVAL;
  struct x9_gps_handoff layout;int e=x9_gps_copy_handoff(&layout);if(e)return e;
  unsigned flags=X9_GPS_POWER_LAYOUT_VERIFIED|X9_GPS_POS_LAYOUT_VERIFIED;
  if(ops->version>=3) {
   if(ops->version==5 ? !ops->pos_afe_verify : !ops->pos_afe)return EINVAL;
   flags|=X9_GPS_RF_LAYOUT_VERIFIED;
  }
  if(ops->version>=4) {
   if(!ops->pos_radio || !ops->pos_cmdbt || ops->pos_cmdbt_bytes!=4096)return EINVAL;
   flags|=X9_GPS_LP_LAYOUT_VERIFIED;
  }
  if(ops->version==5)flags|=X9_GPS_SMPU_LAYOUT_VERIFIED;
  if((layout.flags&flags)!=flags)return ENXIO;
 }
 lck_mtx_lock(&gps_runtime_lock);
 int e=gps_rt.provider.version ? EBUSY : 0;
 if(!e && ops->version>=4) {
  /* Snapshot under the registration lock, BEFORE arming the worker.
   * Caller bytes are never read from the physical MMIO loop. */
  memcpy(gps_rt.cmdbt,ops->pos_cmdbt,sizeof(gps_rt.cmdbt));
  e=gps_pos_cmdbt_identity();
  if(!e)gps_rt.pos_cmdbt_valid=1;
 }
 if (!e) {
  /* Read ONLY declared prefixes: v1=64, v2=88, v3=96, v4=120, v5=128. */
  size_t n=ops->version==5 ? sizeof(*ops) : ops->version==4 ?
      __builtin_offsetof(struct x9_gps_connv2_ops,pos_afe_verify) : ops->version==3 ?
      __builtin_offsetof(struct x9_gps_connv2_ops,pos_radio) : ops->version==2 ?
      __builtin_offsetof(struct x9_gps_connv2_ops,pos_afe) :
      __builtin_offsetof(struct x9_gps_connv2_ops,pos_clock_schematic);
  memcpy(&gps_rt.provider,ops,n);
  if(ops->version>=4)gps_rt.provider.pos_cmdbt=gps_rt.cmdbt;
  wakeup((event_t)&gps_rt.provider.version);
 }
 lck_mtx_unlock(&gps_runtime_lock); return e;
}
#include "x9_gps_native_power.inc"
