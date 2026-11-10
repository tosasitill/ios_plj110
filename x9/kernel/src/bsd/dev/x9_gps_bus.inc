/* Native v4/v5 MT6993 bus gate, actual conninfra.ko 53fe0/53fec/54140,
 * polling55564 and wake56268. Called by gps_mcu_map AFTER aggregate acquire,
 * BEFORE any BGF, SYSRAM, AFE, HIF RAM or remap mapping. No standalone power
 * probe, provider registration, reset, SMC or DMA ownership is invented.
 * AP sleep-protect check in the actual MT6993 KO is a stub; never import
 * another board's SPM write table. The supplier still holds real AON-safe
 * power/upstream clocks and exclusive CMDBT/SYSRAM/all BGF DMA ownership.
 */
static void gps_bus_journal(int error)
{
 if(error && !gps_rt.bus.error)gps_rt.bus.error=error;
 lck_mtx_lock(&gps_lock);
 st.bus_started=gps_rt.bus.started;st.bus_complete=gps_rt.bus.complete;
 st.bus_wake_started=gps_rt.bus.wake_started;
 st.bus_wake_borrowed=gps_rt.bus.wake_borrowed;
 st.bus_wake_prior=gps_rt.bus.wake_prior;st.bus_wake=gps_rt.bus.wake;
 st.bus_detect_attempts=gps_rt.bus.detect_attempts;st.bus_detect=gps_rt.bus.detect;
 st.bus_version=gps_rt.bus.version;st.bus_irq=gps_rt.bus.irq;
 st.bus_restore_required=gps_rt.bus.restore_required;
 st.bus_restore=gps_rt.bus.restore;st.bus_errno=gps_rt.bus.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_bus_read(volatile uint32_t *base,unsigned off,uint32_t *value)
{
 int e=gps_mcu_supplier_guard();if(e)return e;
 if(!base || (off&3) || off>=0x1000)return EINVAL;
 *value=base[off/4];__asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int gps_bus_write(volatile uint32_t *base,unsigned off,uint32_t value)
{
 /* ONLY TOP wake and detection-reset are writable in this stage. */
 if(!((base==gps_rt.bus.host && off==0x1a0) ||
      (base==gps_rt.bus.debug && off==0)))return EPERM;
 int e=gps_mcu_supplier_guard();if(e)return e;
 base[off/4]=value;__asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int gps_bus_poll(volatile uint32_t *base,unsigned off,uint32_t mask,
    uint32_t want,unsigned interval,uint32_t *last)
{
 uint64_t deadline;clock_interval_to_deadline(20,NSEC_PER_MSEC,&deadline);
 for(unsigned i=0;i<=10;i++) {
  int e=gps_bus_read(base,off,last);if(e)return e;
  if((*last&mask)==want)return 0;
  if(i==10 || mach_absolute_time()>=deadline)break;
  gps_runtime_pause(interval);
 }
 return ETIMEDOUT;
}
static int gps_bus_live(void)
{
 /* No recursive gps_mcu_guard, no retrigger, retry, or re-wake in validate.
  * Clock detect may be sticky: it is never authority without the supplier's
  * live AON-safe power/upstream-clock lease and retained TOP awake state. */
 int e=gps_bus_read(gps_rt.bus.host,0x1a0,&gps_rt.bus.wake);if(e)return e;
 if(!(gps_rt.bus.wake&1))return ESTALE;
 e=gps_bus_read(gps_rt.bus.debug,0x400,&gps_rt.bus.irq);if(e)return e;
 if(gps_rt.bus.irq&7)return EIO; /* do NOT clear a timeout IRQ */
 e=gps_bus_read(gps_rt.bus.debug,0,&gps_rt.bus.detect);if(e)return e;
 if((gps_rt.bus.detect&6)!=6)return ENXIO;
 e=gps_bus_read(gps_rt.bus.cfg,0,&gps_rt.bus.version);if(e)return e;
 if(gps_rt.bus.version!=GPS_MCU_MT6993_VERSION)return ENODEV;
 if(gps_rt.bus.restore_required) {
  e=gps_bus_read(gps_rt.bus.cfg_on,0x210,&gps_rt.bus.restore);if(e)return e;
  if(!(gps_rt.bus.restore&(1u<<16)))return ESTALE;
 }
 /* Requalify upstream resources after the off-domain reads as well. */
 return gps_mcu_supplier_guard();
}
static int gps_bus_validate(void)
{
 if(current_thread()!=gps_rt.worker)return EPERM;
 if(gps_rt.bus.error)return gps_rt.bus.error;
 if(!gps_rt.bus.complete)return ENXIO;
 int e=gps_bus_live();if(e)gps_bus_journal(e);
 return e;
}
static int gps_bus_take(void)
{
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version<4 ||
     !(gps_rt.layout.flags&X9_GPS_LP_LAYOUT_VERIFIED))return ENXIO;
 if(gps_rt.bus.started)return EBUSY;
 int e=gps_mcu_supplier_guard();if(e)return e;
 gps_rt.bus.started=1;gps_bus_journal(0);
 gps_rt.bus.host=(volatile uint32_t *)ml_io_map(0x40060000,0x1000);
 gps_rt.bus.cfg_on=(volatile uint32_t *)ml_io_map(0x40001000,0x1000);
 gps_rt.bus.debug=(volatile uint32_t *)ml_io_map(0x40023000,0x1000);
 gps_rt.bus.cfg=(volatile uint32_t *)ml_io_map(0x40011000,0x1000);
 if(!gps_rt.bus.host || !gps_rt.bus.cfg_on || !gps_rt.bus.debug ||
     !gps_rt.bus.cfg){e=ENOMEM;goto done;}
 /* Refuse to wake an inherited autonomous restore engine. Merely observing
  * HW_CONTROL0 is NOT a DMA/CMDBT quiesce lease; aggregate acquire supplied
  * it before this stage. No native CMDBT code is written/started here. */
 uint32_t control;
 e=gps_bus_read(gps_rt.bus.cfg_on,0x200,&control);if(e)goto done;
 if(control&1){e=EBUSY;goto done;}
 gps_rt.bus.restore_required=!!(control&0x40);
 e=gps_bus_read(gps_rt.bus.host,0x1a0,&gps_rt.bus.wake_prior);if(e)goto done;
 if(gps_rt.bus.wake_prior&~1u){e=EBADMSG;goto done;}
 gps_rt.bus.wake_borrowed=!!(gps_rt.bus.wake_prior&1);
 if(!gps_rt.bus.wake_borrowed) {
  uint32_t now;
  e=gps_bus_read(gps_rt.bus.cfg_on,0x200,&now);if(e)goto done;
  if((now^control)&0x41){e=ESTALE;goto done;}
  e=gps_bus_read(gps_rt.bus.host,0x1a0,&now);if(e)goto done;
  if(now!=gps_rt.bus.wake_prior){e=ESTALE;goto done;}
  /* Potentially accepted write remains latched on any failure. No auto
   * release, rollback or second wake attempt; never write GPS wake1ac. */
  gps_rt.bus.wake_started=1;gps_bus_journal(0);
  e=gps_bus_write(gps_rt.bus.host,0x1a0,1);if(e)goto done;
 }
 delay(200); /* actual KO minimum before IP-version polling */
 e=gps_bus_read(gps_rt.bus.host,0x1a0,&gps_rt.bus.wake);if(e)goto done;
 if(!(gps_rt.bus.wake&1)){e=EIO;goto done;}
 /* Never clear/ignore an inherited timeout or use old version comments. */
 e=gps_bus_read(gps_rt.bus.debug,0x400,&gps_rt.bus.irq);if(e)goto done;
 if(gps_rt.bus.irq&7){e=EIO;goto done;}
 for(unsigned i=0;i<4;i++) {
  uint32_t prior;
  e=gps_bus_read(gps_rt.bus.debug,0,&prior);if(e)goto done;
  gps_rt.bus.detect_attempts=i+1;gps_bus_journal(0);
  e=gps_bus_write(gps_rt.bus.debug,0,prior|1);if(e)goto done;
  delay(20);
  e=gps_bus_read(gps_rt.bus.debug,0,&gps_rt.bus.detect);if(e)goto done;
  if((gps_rt.bus.detect&6)==6)break;
  if(i==3){e=ETIMEDOUT;goto done;}
  gps_runtime_pause(1000);
 }
 e=gps_bus_poll(gps_rt.bus.cfg,0,UINT32_MAX,GPS_MCU_MT6993_VERSION,
     1000,&gps_rt.bus.version);if(e)goto done;
 /* A freshly cold pre-LP engine with restore-first bit6 clear has no code
  * to restore. Do not demand a synthetic done bit. When bit6 is set,
  * require actual CFG_PWRCTRL1[16] (vendor only warns on timeout). */
 if(gps_rt.bus.restore_required) {
  e=gps_bus_poll(gps_rt.bus.cfg_on,0x210,1u<<16,1u<<16,500,
      &gps_rt.bus.restore);if(e)goto done;
 }
 e=gps_bus_live();if(e)goto done;
 gps_rt.bus.complete=1;
done:
 gps_bus_journal(e);
 printf("X9 GPS native bus: complete=%u wake=%u/%u detect=%x version=%x irq=%x restore=%u/%x errno=%d; not DMA/EMI/receiver readiness\n",
     gps_rt.bus.complete,gps_rt.bus.wake_started,gps_rt.bus.wake_borrowed,
     gps_rt.bus.detect,gps_rt.bus.version,gps_rt.bus.irq,
     gps_rt.bus.restore_required,gps_rt.bus.restore,e);
 return e;
}
