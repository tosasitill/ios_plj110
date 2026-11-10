/* Native power part of the future complete CONNV2 supplier.
 * Actual KO 53b7c/PMIC and 60490/60544, clk-mt6993-pd and v1 AP hwccf:
 * VGPS18 -> AP2CONN_HOST selector -> CONN HWV18 -> minimum5ms settle.
 * Never use the alternate generated SPM table or MM's voter algorithm.
 *
 * Reject an inherited/live CONN domain, including a non-AP aggregate vote.
 * This helper owns ONLY its new power request, not old BGF DMA or EMI MPU.
 * No supplier/backend registration, reset, RAM clear or SMC takes place.
 * It is callable only by the sole physical worker; not a user ioctl.
 */
enum { GPS_NP_MAPPED=1, GPS_NP_RAIL=2, GPS_NP_SELECTOR=4,
    GPS_NP_HWV=8, GPS_NP_COMPLETE=16 };
#define GPS_NP_CONN (1u<<18)
#define GPS_NP_SELECTOR_BIT (1u<<24)
static struct {
 struct x9_gps_handoff layout;
 volatile uint32_t *pmif,*hwv,*top,*rc;
 struct gps_power_sample prior, expected;
 uint64_t generation, lease;
 unsigned attempted,phase,clock,rc_mode,pmif_pending,pmif_fault;
 unsigned vsel_write,control_write,selector_write;
 uint32_t selector_prior,ap_prior;
 int error;
} gps_np;
static void gps_np_journal(int error)
{
 if(error && !gps_np.error)gps_np.error=error;
 lck_mtx_lock(&gps_lock);
 st.native_power_phase=gps_np.phase;
 st.native_power_vsel_write=gps_np.vsel_write;
 st.native_power_control_write=gps_np.control_write;
 st.native_power_selector_write=gps_np.selector_write;
 st.native_power_clock=gps_np.clock;st.native_power_rc=gps_np.rc_mode;
 st.native_power_vsel_prior=gps_np.prior.voltage;
 st.native_power_control_prior=gps_np.prior.control;
 st.native_power_selector_prior=gps_np.selector_prior;
 st.native_power_ap_prior=gps_np.ap_prior;
 st.native_power_errno=gps_np.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_np_generation(void)
{
 if(current_thread()!=gps_rt.worker || !gps_np.generation)return EPERM;
 struct x9_gps_status s;x9_gps_copy_status(&s);
 return s.generation!=gps_np.generation || s.fault ? ESTALE : 0;
}
static int gps_np_xfer(unsigned reg,uint8_t *value,int write)
{
 /* Caller holds the common PMIF mutex. No RC aliases or broad writes:
  * acquisition may modify only SID3 LNC0_8 selector and SW_EN/LP field. */
 if(write && reg!=0x24b && reg!=0x2087)return EPERM;
 if(gps_np.pmif_fault || gps_np.pmif_pending)return EIO;
 int e=gps_np_generation();if(e)return e;
 if(!(gps_power_read(gps_np.pmif,0)&1))return ENXIO;
 e=gps_power_pmif_wait(gps_np.pmif,0);if(e)return e;
 e=gps_np_generation();if(e)return e;
 gps_np.pmif_pending=1;
 if(write)gps_power_write(gps_np.pmif,0x884,*value);
 gps_power_write(gps_np.pmif,0x880,(3u<<30)|(write?1u<<29:0)|(3u<<24)|reg);
 e=gps_power_pmif_wait(gps_np.pmif,write?0:6);
 if(!e && !write) {
  *value=(uint8_t)gps_power_read(gps_np.pmif,0x894);
  gps_power_write(gps_np.pmif,0x8a4,1);
  e=gps_power_pmif_wait(gps_np.pmif,0);
 }
 if(!e)gps_np.pmif_pending=0;
 else if(!write) {
  /* Drain only this request's late read, still under the common lock.
   * Never clear an inherited error/other VALID or retry an uncertain write. */
  uint32_t state=gps_power_read(gps_np.pmif,0x8a8);
  if(!(state&(1u<<18)) && ((state>>1)&7)==6) {
   (void)gps_power_read(gps_np.pmif,0x894);
   gps_power_write(gps_np.pmif,0x8a4,1);
   if(!gps_power_pmif_wait(gps_np.pmif,0))gps_np.pmif_pending=0;
  }
 }
 if(e)gps_np.pmif_fault=1;
 return e;
}
static int gps_np_snapshot(struct gps_power_sample *s)
{
 int e=gps_np_xfer(8,&s->hwcid,0);
 if(!e && s->hwcid!=0x10 && s->hwcid!=0x20)e=ENODEV;
 if(!e)e=gps_np_xfer(0x24b,&s->voltage,0);
 if(!e)e=gps_np_xfer(0x2087,&s->control,0);
 if(!e)e=gps_np_xfer(0x20a2,&s->da,0);
 if(!e)e=gps_np_xfer(0x20af,&s->hw_en,0);
 if(!e)e=gps_np_xfer(0x20a7,&s->rc_mux,0);
 return e;
}
static int gps_np_domain(int on)
{
 int e=gps_np_generation();if(e)return e;
 uint32_t ap=gps_power_read(gps_np.hwv,0x20708);
 uint32_t global=gps_power_read(gps_np.hwv,0x13700);
 uint32_t busy=gps_power_read(gps_np.hwv,0x13704);
 uint32_t done=gps_power_read(gps_np.hwv,0x1132c);
 uint32_t ack=gps_power_read(gps_np.hwv,0x12900);
 uint32_t want=on?GPS_NP_CONN:0;
 /* ALL_EN/ALL_STA includes autolink and other XPUs. AP=0 alone is not a
  * cold domain. DONE must also be ready; it is not an enable/READY bit. */
 return (ap&GPS_NP_CONN)==want && (global&GPS_NP_CONN)==want &&
     !(busy&GPS_NP_CONN) && (done&GPS_NP_CONN) &&
     (ack&GPS_NP_CONN)==want ? 0 : EBUSY;
}
static int gps_np_wait(unsigned off,uint32_t mask,uint32_t want)
{
 uint64_t deadline;clock_interval_to_deadline(20,NSEC_PER_MSEC,&deadline);
 for(unsigned i=0;i<=40;i++) {
  int e=gps_np_generation();if(e)return e;
  if((gps_power_read(gps_np.hwv,off)&mask)==want)return 0;
  if(i==40 || mach_absolute_time()>=deadline)break;
  gps_runtime_pause(500);
 }
 return ETIMEDOUT;
}
static int gps_np_rail_take(void)
{
 if(X9PMIFAcquire())return EBUSY;
 struct gps_power_sample second={0};
 int e=gps_np_snapshot(&gps_np.prior);
 if(!e)e=gps_np_snapshot(&second);
 if(!e && !gps_power_same(&gps_np.prior,&second))e=EAGAIN;
 if(!e && (second.voltage&15)>9)e=ERANGE;
 if(e)goto done;
 gps_np.expected=second;
 /* 500mV + VOSEL*100mV + VOCAL*10mV => 1800mV is D0, NOT hex18.
  * Changing a live rail, or a disabled rail with autonomous selectors,
  * needs a shared regulator policy, not just the common bus mutex. */
 if(second.voltage!=0xd0 && ((second.control&1) || (second.da&1) ||
     second.hw_en || second.rc_mux)){e=EBUSY;goto done;}
 int sw_enable=gps_np.clock==2 || !gps_np.rc_mode;
 if(sw_enable && (second.control&1) && (second.control&2)){e=EBUSY;goto done;}
 /* The token records even a partially accepted power attempt. It is NOT
  * the aggregate CONNV2 lease or permission to clear RAM/release reset. */
 gps_np.lease=gps_np.generation;
 gps_np.phase|=GPS_NP_RAIL;gps_np_journal(0);
 if(second.voltage!=0xd0) {
  e=gps_np_domain(0);if(e)goto done;
  uint8_t value=0xd0;gps_np.vsel_write=1;gps_np.expected.voltage=value;
  gps_np_journal(0);e=gps_np_xfer(0x24b,&value,1);
  if(!e)e=gps_np_xfer(0x24b,&value,0);
  if(!e && value!=0xd0)e=EIO;
  if(e)goto done;
 }
 if(sw_enable && !(second.control&1)) {
  e=gps_np_domain(0);if(e)goto done;
  uint8_t value=(uint8_t)((second.control&~3u)|1u);
  gps_np.control_write=1;gps_np.expected.control=value;gps_np_journal(0);
  e=gps_np_xfer(0x2087,&value,1);
  if(!e)e=gps_np_xfer(0x2087,&value,0);
  if(!e && value!=gps_np.expected.control)e=EIO;
 }
done:
 X9PMIFRelease();return e;
}
static int gps_np_rail_check(void)
{
 if(X9PMIFAcquire())return EBUSY;
 struct gps_power_sample s={0};int e=gps_np_snapshot(&s);
 if(!e && (s.voltage!=0xd0 || s.control!=gps_np.expected.control ||
     s.hw_en!=gps_np.expected.hw_en || s.rc_mux!=gps_np.expected.rc_mux ||
     s.hwcid!=gps_np.expected.hwcid))e=ESTALE;
 /* Actual DA EN/LP must be normal under the held active request.
  * SW_EN=1, a sticky AP vote, or a configured voltage is insufficient. */
 if(!e && (s.da&5)!=1)e=ENXIO;
 X9PMIFRelease();return e;
}
static int gps_np_live(void)
{
 int e=gps_np_generation();if(e)return e;
 if((gps_power_read(gps_np.rc,4)&1)!=gps_np.rc_mode)return ESTALE;
 e=gps_np_domain(1);if(e)return e;
 if(!(gps_power_read(gps_np.top,0x70)&GPS_NP_SELECTOR_BIT))return ESTALE;
 e=gps_np_rail_check();if(e)return e;
 return gps_np_domain(1);
}
int x9_gps_native_power_acquire(const struct x9_gps_handoff *h,uint64_t generation,
    unsigned clock,uint64_t *lease)
{
 if(!lease)return EINVAL;
 *lease=0;
 if(!h || !generation || clock>3 || current_thread()!=gps_rt.worker)return EINVAL;
 if(gps_np.attempted){*lease=gps_np.lease;return EBUSY;}
 struct x9_gps_handoff actual;int e=x9_gps_copy_handoff(&actual);if(e)return e;
 unsigned flags=X9_GPS_POWER_LAYOUT_VERIFIED|X9_GPS_CLOCK_LAYOUT_VERIFIED|
     X9_GPS_LP_LAYOUT_VERIFIED;
 if(memcmp(h,&actual,sizeof(actual)) || (h->flags&flags)!=flags)return ENXIO;
 gps_np.layout=*h;gps_np.generation=generation;gps_np.clock=clock;
 e=gps_np_generation();if(e)return e;
 gps_np.attempted=1;
 gps_np.pmif=(volatile uint32_t *)ml_io_map(0x1c018000,0x2000);
 gps_np.hwv=(volatile uint32_t *)ml_io_map(0x1c040000,0x74900);
 gps_np.top=(volatile uint32_t *)ml_io_map(0x1c210000,0x1000);
 gps_np.rc=(volatile uint32_t *)ml_io_map(0x1c011000,0x1000);
 if(!gps_np.pmif || !gps_np.hwv || !gps_np.top || !gps_np.rc){e=ENOMEM;goto done;}
 gps_np.phase=GPS_NP_MAPPED;
 gps_np.ap_prior=gps_power_read(gps_np.hwv,0x20708);
 gps_np.selector_prior=gps_power_read(gps_np.top,0x70);
 gps_np.rc_mode=gps_power_read(gps_np.rc,4)&1;
 e=gps_np_domain(0);if(e)goto done;
 e=gps_np_rail_take();if(e)goto done;
 if((gps_power_read(gps_np.rc,4)&1)!=gps_np.rc_mode){e=ESTALE;goto done;}
 if((gps_power_read(gps_np.top,0x70)^gps_np.selector_prior)&GPS_NP_SELECTOR_BIT){e=ESTALE;goto done;}
 gps_np.phase|=GPS_NP_SELECTOR;
 if(!(gps_np.selector_prior&GPS_NP_SELECTOR_BIT)) {
  e=gps_np_generation();if(e)goto done;
  gps_np.selector_write=1;gps_np_journal(0);
  gps_power_write(gps_np.top,0x74,GPS_NP_SELECTOR_BIT);delay(20);
 }
 if(!(gps_power_read(gps_np.top,0x70)&GPS_NP_SELECTOR_BIT)){e=EIO;goto done;}
 e=gps_np_domain(0);if(e)goto done;
 gps_np.phase|=GPS_NP_HWV;gps_np_journal(0);
 e=gps_np_generation();if(e)goto done;
 gps_power_write(gps_np.hwv,0x20700,GPS_NP_CONN);
 e=gps_np_wait(0x20708,GPS_NP_CONN,GPS_NP_CONN);if(e)goto done;
 e=gps_np_wait(0x1132c,GPS_NP_CONN,GPS_NP_CONN);if(e)goto done;
 /* Actual conninfra clock API wrapper has a minimum 5ms domain settle. */
 delay(5000);
 e=gps_np_wait(0x13700,GPS_NP_CONN,GPS_NP_CONN);if(e)goto done;
 e=gps_np_wait(0x13704,GPS_NP_CONN,0);if(e)goto done;
 e=gps_np_wait(0x12900,GPS_NP_CONN,GPS_NP_CONN);if(e)goto done;
 e=gps_np_live();if(!e)gps_np.phase|=GPS_NP_COMPLETE;
done:
 *lease=gps_np.lease;gps_np_journal(e);
 printf("X9 GPS native power sublease: phase=%x errno=%d; secure EMI/DMA/POS/ROM/MNL still required\n",gps_np.phase,e);
 return e;
}
int x9_gps_native_power_validate(const struct x9_gps_handoff *h,uint64_t generation,
    uint64_t lease)
{
 if(!h || !lease || generation!=gps_np.generation || lease!=gps_np.lease ||
     memcmp(h,&gps_np.layout,sizeof(*h)))return ESTALE;
 if(gps_np.error)return gps_np.error;
 if(!(gps_np.phase&GPS_NP_COMPLETE))return ENXIO;
 int e=gps_np_live();if(e)gps_np_journal(e);
 return e;
}
#undef GPS_NP_CONN
#undef GPS_NP_SELECTOR_BIT
