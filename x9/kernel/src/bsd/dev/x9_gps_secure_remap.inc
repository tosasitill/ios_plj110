/* Saved PLJ110 ATF: e800 initializes 126c48 with CFG=4004b000,
 * HOST=40060000; op70 ebc4/ebd0 shifts LK's three bases by16 and writes
 * HOST+8354/835c/8360, CFG+364..370 and six HOST policy words below.
 * The selected v6 supplier must prove this SAME running ABI/secure route
 * and actual LK allocations. This code never calls op70 or rewrites a
 * mismatch. AP readback cannot grant secure-world or BGF DMA ownership.
 * Only the FW base is in our handoff. MD/GPS base authority stays with the
 * supplier: no invented zero/nonzero expectations for their optional LK
 * registrations (op70 skips absent bases, leaving previous words intact).
 */
static const struct {
 uint32_t address,mask,value;
} gps_secure_remap_fields[11]={
 {0x40068354,0x000fffff,0}, /* actual FW base supplied dynamically */
 {0x4004b364,0x000fffff,0x1000},
 {0x4004b368,0x000fffff,0x1300},
 {0x4004b36c,0x000fffff,0x1c00},
 {0x4004b370,0x000fffff,0x1de0},
 {0x400683c0,UINT32_MAX,1},
 {0x400683c8,UINT32_MAX,1},
 {0x400683cc,UINT32_MAX,1},
 {0x400683dc,UINT32_MAX,1},
 {0x400683e0,UINT32_MAX,1},
 {0x400683e4,UINT32_MAX,1},
};
static void gps_secure_remap_journal(int error)
{
 if(error && !gps_rt.secure_remap.error)gps_rt.secure_remap.error=error;
 if(error)gps_rt.secure_remap.valid=0;
 lck_mtx_lock(&gps_lock);
 st.secure_remap_started=gps_rt.secure_remap.started;
 st.secure_remap_valid=gps_rt.secure_remap.valid;
 st.secure_remap_address=gps_rt.secure_remap.address;
 st.secure_remap_checks=gps_rt.secure_remap.checks;
 st.secure_remap_errno=gps_rt.secure_remap.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_secure_remap_snapshot(uint32_t *view)
{
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version!=6 ||
     gps_rt.ops.secure_profile!=X9_GPS_SECURE_0415_4K_V1)return EPERM;
 for(unsigned i=0;i<11;i++) {
  /* No recursion through gps_mcu_guard: qualify power/upstream/bus/clock
   * before EVERY read, then observe only the already mapped CSR. */
  int e=gps_mcu_platform_guard();if(e)return e;
  gps_rt.secure_remap.address=gps_secure_remap_fields[i].address;
  volatile uint32_t *p=gps_mcu_pointer(gps_rt.secure_remap.address);
  if(!p)return ENXIO;
  view[i]=*p&gps_secure_remap_fields[i].mask;
  __asm__ volatile("dsb sy" ::: "memory");
 }
 return gps_mcu_platform_guard();
}
static int gps_secure_remap_policy(const uint32_t *view)
{
 for(unsigned i=0;i<11;i++) {
  uint32_t expected=i ? gps_secure_remap_fields[i].value :
      gps_rt.layout.reserve_base>>16;
  if(view[i]!=expected) {
   gps_rt.secure_remap.address=gps_secure_remap_fields[i].address;
   return EACCES;
  }
 }
 return 0;
}
static int gps_secure_remap_validate(void)
{
 if(gps_rt.secure_remap.error)return gps_rt.secure_remap.error;
 if(!gps_rt.secure_remap.valid)return ENXIO;
 int e=gps_secure_remap_snapshot(gps_rt.secure_remap.sample);
 if(!e)for(unsigned i=0;i<11;i++)
  if(gps_rt.secure_remap.sample[i]!=gps_rt.secure_remap.baseline[i]) {
   gps_rt.secure_remap.address=gps_secure_remap_fields[i].address;
   e=ESTALE;break;
  }
 if(!e)gps_rt.secure_remap.checks++;
 gps_secure_remap_journal(e);return e;
}
static int gps_secure_remap_take(void)
{
 if(gps_rt.secure_remap.started)return EBUSY;
 if(gps_rt.ops.version!=6 ||
     gps_rt.ops.secure_profile!=X9_GPS_SECURE_0415_4K_V1 ||
     !gps_rt.bus.complete)return ENXIO;
 gps_rt.secure_remap.started=1;
 int e=gps_secure_remap_snapshot(gps_rt.secure_remap.baseline);
 if(!e)e=gps_secure_remap_policy(gps_rt.secure_remap.baseline);
 if(!e)e=gps_secure_remap_snapshot(gps_rt.secure_remap.sample);
 if(!e)for(unsigned i=0;i<11;i++)
  if(gps_rt.secure_remap.sample[i]!=gps_rt.secure_remap.baseline[i]) {
   gps_rt.secure_remap.address=gps_secure_remap_fields[i].address;
   e=ESTALE;break;
  }
 if(!e){gps_rt.secure_remap.valid=1;gps_rt.secure_remap.checks++;}
 gps_secure_remap_journal(e);return e;
}
