/* Current NSMPU SR/AID READ, not legacy SA<<addr_align + dram_start/APC.
 * Geometry is LK-qualified separately. memory/mediatek/mtk-emi-mpu-test-v2.c
 * returns absolute u64 SA/EA and two-bit AID permissions using c200050b,
 * op2, type SA0/EA1/ENABLE3/AID4. smpu.c identifies NS CPU as AID241.
 * The legacy copy of the test uses a different node/property spelling;
 * do not use its mt6983 node as PLJ110 runtime evidence.
 *
 * Called AFTER the real v5 supplier acquired secure MPU/remap authority,
 * BEFORE host HIF mappings, and for EVERY HIF ownership check. The supplier
 * must hold this running ABI, immutable whole-table configuration and BGF
 * authority. These reads supplement, never replace that retained lease.
 * No region allocation, permission SET/CLEAR, op70, MMIO MPU dump/clear,
 * firmware ACK, backend registration or fabricated readiness is performed.
 */
#define GPS_EMI_CPU_AID 241u
#define GPS_EMI_CPU_SR 47u
static int gps_emi_cpu_epoch(void)
{
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version!=5 || !gps_rt.lease)return EPERM;
 struct x9_gps_status s;x9_gps_copy_status(&s);
 return s.hardware_generation!=gps_rt.generation || s.fault ? ESTALE : 0;
}
static int gps_emi_cpu_guard(void)
{
 int e=gps_emi_cpu_epoch();if(e)return e;
 e=gps_mcu_guard();if(e)return e;
 return gps_rt.ops.pos_validate(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease);
}
static void gps_emi_cpu_journal(int error)
{
 if(error && !gps_rt.emi_cpu.error)gps_rt.emi_cpu.error=error;
 if(error)gps_rt.emi_cpu.valid=0;
 lck_mtx_lock(&gps_lock);
 st.emi_cpu_started=gps_rt.emi_cpu.started;
 st.emi_cpu_valid=gps_rt.emi_cpu.valid;
 st.emi_cpu_region=gps_rt.emi_cpu.region;
 st.emi_cpu_aid=GPS_EMI_CPU_AID;
 st.emi_cpu_intersections=gps_rt.emi_cpu.intersections;
 st.emi_cpu_checks=gps_rt.emi_cpu.checks;
 st.emi_cpu_errno=gps_rt.emi_cpu.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_emi_cpu_read(unsigned type,unsigned region,uint64_t *value)
{
 if(region<1 || region>63 || (type!=0 && type!=1 && type!=3 && type!=4))return EINVAL;
 /* The read-only batch is bracketed by the full live supplier guard.
  * Check its hardware epoch on every SMC as well. There is no CONN MMIO
  * here: do not repeat hundreds of PMIF/clock snapshots for one table or
  * recurse through HIF ownership. No HIF effect occurs until final guard. */
 int e=gps_emi_cpu_epoch();if(e)return e;
 register uint64_t x0 __asm__("x0")=0xc200050bULL;
 register uint64_t x1 __asm__("x1")=2,x2 __asm__("x2")=type;
 register uint64_t x3 __asm__("x3")=region;
 register uint64_t x4 __asm__("x4")=type==4 ? GPS_EMI_CPU_AID/32 : 0;
 register uint64_t x5 __asm__("x5")=0,x6 __asm__("x6")=0,x7 __asm__("x7")=0;
 __asm__ volatile("dsb sy\nsmc #0\ndsb sy\nisb" :
     "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) ::
     "memory","cc","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17");
 e=gps_emi_cpu_epoch();if(e)return e;
 /* a0 is DATA, not a generic success status. In particular a full-width
  * all-RW AID word is legitimate data; do not truncate to u32 or mistake
  * its upper AID241 bits for APC domain permissions. ABI authority must
  * already be held by the supplier, not inferred from this word. */
 *value=x0;return 0;
}
static int gps_emi_cpu_snapshot(struct gps_emi_cpu_region *table)
{
 int e=gps_emi_cpu_guard();if(e)return e;
 uint64_t deadline;clock_interval_to_deadline(250,NSEC_PER_MSEC,&deadline);
 for(unsigned i=1;i<=63;i++) {
  /* Host work is bounded between secure calls; an individual firmware SMC
   * must itself be bounded by the qualified running ABI. No timeout retry. */
  if(mach_absolute_time()>=deadline)return ETIMEDOUT;
  gps_rt.emi_cpu.region=i;
  uint64_t enabled,after;
  e=gps_emi_cpu_read(3,i,&enabled);if(e)return e;
  /* Reject SMCCC/legacy negative returns and unrecognized enable masks.
   * Do not convert any nonzero value (including NOT_SUPPORTED) to true. */
  if(enabled>1)return ENOTSUP;
  struct gps_emi_cpu_region *r=&table[i];
  memset(r,0,sizeof(*r));r->enabled=(uint32_t)enabled;
  if(enabled) {
   e=gps_emi_cpu_read(0,i,&r->start);if(e)return e;
   e=gps_emi_cpu_read(1,i,&r->end);if(e)return e;
   if(r->start>r->end || r->end>0x000fffffffffffffULL)return EBADMSG;
   e=gps_emi_cpu_read(4,i,&r->aid_set);if(e)return e;
  }
  e=gps_emi_cpu_read(3,i,&after);if(e)return e;
  if(after!=enabled)return ESTALE;
 }
 if(mach_absolute_time()>=deadline)return ETIMEDOUT;
 return gps_emi_cpu_guard();
}
static int gps_emi_cpu_policy(const struct gps_emi_cpu_region *table)
{
 const uint64_t lo[2]={
     (uint64_t)gps_rt.layout.reserve_base+gps_rt.layout.ap2mcu_offset,
     (uint64_t)gps_rt.layout.reserve_base+gps_rt.layout.mcu2ap_offset};
 const uint64_t hi[2]={lo[0]+gps_rt.layout.region_size-1,
     lo[1]+gps_rt.layout.region_size-1};
 const struct gps_emi_cpu_region *required=&table[GPS_EMI_CPU_SR];
 /* LK places HIF inside layout region7, protected by its group6..11 SR47.
  * The protected range can span other mblocks; never map it as one reserve.
  * Use absolute bounds, no guessed legacy DRAM offset/alignment/end fixup. */
 if(!required->enabled)return EACCES;
 for(unsigned c=0;c<2;c++)
  if(required->start>lo[c] || required->end<hi[c])return EACCES;
 unsigned intersections=0;
 for(unsigned i=1;i<=63;i++) {
  const struct gps_emi_cpu_region *r=&table[i];if(!r->enabled)continue;
  unsigned touches=0;
  for(unsigned c=0;c<2;c++)
   if(r->start<=hi[c] && r->end>=lo[c])touches=1;
  if(!touches)continue;
  gps_rt.emi_cpu.region=i;intersections++;
  /* Require RW in EVERY intersecting SR, not only SR47 or the first match.
   * This refuses contradictory policies without guessing priority/merge
   * behavior. Permission No0/WO1/RO2 cannot authorize the HIF host writes
   * and completion/data reads. Never clear target-owned MCU RAM. */
  if(((r->aid_set>>((GPS_EMI_CPU_AID%32)*2))&3)!=3)return EACCES;
 }
 gps_rt.emi_cpu.intersections=intersections;
 gps_rt.emi_cpu.region=GPS_EMI_CPU_SR;return 0;
}
static int gps_emi_cpu_same(const struct gps_emi_cpu_region *a,
    const struct gps_emi_cpu_region *b)
{
 for(unsigned i=1;i<=63;i++)
  if(a[i].enabled!=b[i].enabled || a[i].start!=b[i].start ||
     a[i].end!=b[i].end || a[i].aid_set!=b[i].aid_set)return ESTALE;
 return 0;
}
static int gps_emi_cpu_take(void)
{
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version!=5 ||
     !(gps_rt.layout.flags&X9_GPS_SMPU_LAYOUT_VERIFIED))return ENXIO;
 if(gps_rt.emi_cpu.started)return EBUSY;
 gps_rt.emi_cpu.started=1;gps_emi_cpu_journal(0);
 int e=gps_emi_cpu_snapshot(gps_rt.emi_cpu.baseline);
 if(!e)e=gps_emi_cpu_policy(gps_rt.emi_cpu.baseline);
 if(!e)e=gps_emi_cpu_snapshot(gps_rt.emi_cpu.sample);
 if(!e)e=gps_emi_cpu_same(gps_rt.emi_cpu.baseline,gps_rt.emi_cpu.sample);
 if(!e)e=gps_emi_cpu_policy(gps_rt.emi_cpu.sample);
 if(!e){gps_rt.emi_cpu.valid=1;gps_rt.emi_cpu.checks++;}
 gps_emi_cpu_journal(e);return e;
}
static int gps_emi_cpu_validate(void)
{
 if(current_thread()!=gps_rt.worker)return EPERM;
 if(gps_rt.emi_cpu.error)return gps_rt.emi_cpu.error;
 if(!gps_rt.emi_cpu.valid)return ENXIO;
 /* The table is immutable under the retained supplier lease. Re-read ALL
  * regions: a previously disabled/nonintersecting SR could otherwise turn
  * into a new higher-priority deny while the expected SR47 remains RW.
  * No repair/retry or rebinding of posted MCU descriptors on change. */
 int e=gps_emi_cpu_snapshot(gps_rt.emi_cpu.sample);
 if(!e)e=gps_emi_cpu_same(gps_rt.emi_cpu.baseline,gps_rt.emi_cpu.sample);
 if(!e)e=gps_emi_cpu_policy(gps_rt.emi_cpu.sample);
 if(!e)gps_rt.emi_cpu.checks++;
 gps_emi_cpu_journal(e);return e;
}
#undef GPS_EMI_CPU_AID
#undef GPS_EMI_CPU_SR
