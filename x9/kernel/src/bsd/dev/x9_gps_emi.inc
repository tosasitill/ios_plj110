/* Retained SR/AID policy logic; NO operational secure READ entry.
 * Saved tee_a registry78680 ->43e3c does implement0415/op2 ->43a64,
 * but only BL dispatch309e0/309f8 selects it. Saved LK14d98 ->224f0
 * ->2227c ->15654 sends c2000115 BEFORE kernel entry; ATF c810 ->bfa0
 * sets132080=1. Getter22954 selects kernel dispatch, whose primary and
 * secondary whitelists BOTH reject0415. Source-only050b/op2 returns -2.
 * Existence of a registered handler does not establish kernel reachability.
 * The read function below therefore returns ENOTSUP WITHOUT emitting SMC,
 * even if internal v6 state were accidentally armed. No alternate caller
 * bits, BL/HYP ID, SET/CLEAR, dump/ACK or op70 is tried. Saved slotA is not
 * current slotB proof; a real kernel READ route still needs establishing.
 * Retained decoding: SA/EA43978 return80000000+(u32 page<<12), with EA
 * the LAST PAGE BASE. AID439b8 is packed2bit and WRITES selector1105d9bc.
 */
#define GPS_EMI_CPU_AID 241u
#define GPS_EMI_CPU_SR 47u
static int gps_emi_cpu_epoch(void)
{
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version!=6 ||
     gps_rt.ops.secure_profile!=X9_GPS_SECURE_0415_4K_V1 || !gps_rt.lease)return EPERM;
 struct x9_gps_status s;x9_gps_copy_status(&s);
 if(s.hardware_generation!=gps_rt.generation || s.fault)return ESTALE;
 /* Profile1 is retired BL-only, not a supplier-qualified kernel service. */
 return ENOTSUP;
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
 st.emi_cpu_abi=gps_rt.ops.secure_profile;
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
 if(!value)return EINVAL;
 /* Leave output untouched on error. No BL-only SMC, probing or synthetic
  * all-RW data. Replacing this requires a proved kernel-callable ABI. */
 return ENOTSUP;
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
   /* The decoded ABI uses 4KiB page bases, regardless of XNU's 16KiB
    * VM granule. Validate BEFORE canonicalizing the inclusive bound.
    * Do not expand protected bounds by a guessed XNU page size. */
   if(r->start<0x80000000ULL || r->start>r->end ||
       (r->start&0xfff) || (r->end&0xfff) ||
       r->end>0x000010007ffff000ULL)return EBADMSG;
   r->end|=0xfff;
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
  * Use the selected ABI's canonical byte bounds; no 16KiB endpoint fixup. */
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
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version!=6 ||
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
