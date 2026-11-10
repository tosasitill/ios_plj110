/* PLJ110 power observation, not connv2 acquisition.
 * Actual conninfra.ko 54738 -> 60544 calls clk_prepare/clk_enable, NOT
 * consys_conninfra_on_power_ctrl_mt6993_gen. The 6.12 CONN parent is HWV
 * bit18. CCF_XPU(0) adds 0x20000 to per-AP vote offsets, not global ones.
 * LK must prove SID3 -> PMIF-P and the regulator/clock phandle edges.
 * Never mutate rail voltage/mode/SW_EN/RC mux, HWV SET/CLR, SMC or EMI.
 * RC ownership, MT6686/clock setup, EMI MPU and old DMA remain unproven. */
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
struct gps_power_sample {
 uint8_t hwcid, voltage, control, da, hw_en, rc_mux;
 uint32_t ap, global, busy, done, ack;
};
static uint32_t gps_power_read(volatile uint32_t *p,unsigned off)
{
 uint32_t v=p[off/4]; __asm__ volatile("dsb sy" ::: "memory"); return v;
}
static void gps_power_write(volatile uint32_t *p,unsigned off,uint32_t v)
{
 p[off/4]=v; __asm__ volatile("dsb sy" ::: "memory");
}
static int gps_power_pmif_wait(volatile uint32_t *p,unsigned state)
{
 uint64_t ticks; nanoseconds_to_absolutetime(1000000,&ticks);
 uint64_t start=mach_absolute_time();
 for(unsigned i=0;i<1000;i++) {
  uint32_t v=gps_power_read(p,0x8a8);
  if(v&(1u<<18))return EIO;
  if(((v>>1)&7)==state)return 0;
  if(mach_absolute_time()-start>=ticks)break;
  delay(1);
 }
 return ETIMEDOUT;
}
static int gps_power_pmic_read(volatile uint32_t *p,unsigned reg,uint8_t *value)
{
 /* Caller holds the common PMIF lock over the entire double snapshot.
  * Do not clear someone else's inherited VALID/error or retry a NACK. */
 int e=gps_power_pmif_wait(p,0); if(e)return e;
 gps_power_write(p,0x880,(3u<<30)|(3u<<24)|reg);
 e=gps_power_pmif_wait(p,6);
 if(!e) {
  *value=(uint8_t)gps_power_read(p,0x894);
  gps_power_write(p,0x8a4,1);return gps_power_pmif_wait(p,0);
 }
 /* Drain only our own late read if it is visibly VALID while locked.
  * No rail writes or blanket clears. An unresolved response ends this
  * observation; no later transaction is issued by this worker. */
 uint32_t s=gps_power_read(p,0x8a8);
 if(!(s&(1u<<18)) && ((s>>1)&7)==6) {
  (void)gps_power_read(p,0x894);gps_power_write(p,0x8a4,1);
  (void)gps_power_pmif_wait(p,0);
 }
 return e;
}
static void gps_power_hwv(volatile uint32_t *p,struct gps_power_sample *s)
{
 s->ap=gps_power_read(p,0x20708);
 s->global=gps_power_read(p,0x13700);s->busy=gps_power_read(p,0x13704);
 s->done=gps_power_read(p,0x1132c);s->ack=gps_power_read(p,0x12900);
}
static int gps_power_pmic(volatile uint32_t *p,struct gps_power_sample *s)
{
 int e=gps_power_pmic_read(p,0x8,&s->hwcid);
 if(!e && s->hwcid!=0x10 && s->hwcid!=0x20)return ENODEV;
 if(!e)e=gps_power_pmic_read(p,0x24b,&s->voltage);
 if(!e)e=gps_power_pmic_read(p,0x2087,&s->control);
 if(!e)e=gps_power_pmic_read(p,0x20a2,&s->da);
 if(!e)e=gps_power_pmic_read(p,0x20af,&s->hw_en);
 if(!e)e=gps_power_pmic_read(p,0x20a7,&s->rc_mux);
 return e;
}
static int gps_power_same(const struct gps_power_sample *a,
    const struct gps_power_sample *b)
{
 /* Other domains may change concurrently; compare only CONN bit18.
  * Compare all six PMIC values: RC/DA changes invalidate the snapshot. */
 uint32_t mask=1u<<18;
 return a->hwcid==b->hwcid && a->voltage==b->voltage &&
     a->control==b->control && a->da==b->da && a->hw_en==b->hw_en &&
     a->rc_mux==b->rc_mux && !((a->ap^b->ap)&mask) &&
     !((a->global^b->global)&mask) && !((a->busy^b->busy)&mask) &&
     !((a->done^b->done)&mask) && !((a->ack^b->ack)&mask);
}
static void gps_power_observe(void)
{
 struct x9_gps_handoff h; struct gps_power_sample a={0},b={0};
 int e=x9_gps_copy_handoff(&h);
 if(!e && !(h.flags&X9_GPS_POWER_LAYOUT_VERIFIED))e=ENXIO;
 volatile uint32_t *pmif=NULL,*hwv=NULL;
 if(!e) {
  pmif=(volatile uint32_t *)ml_io_map(0x1c018000,0x2000);
  hwv=(volatile uint32_t *)ml_io_map(0x1c040000,0x74900);
  if(!pmif || !hwv)e=ENOMEM;
 }
 if(!e) {
  e=X9PMIFAcquire()?ENOMEM:0;
  if(!e) {
   if(!(gps_power_read(pmif,0)&1))e=ENXIO;
   if(!e) {
    gps_power_hwv(hwv,&a);e=gps_power_pmic(pmif,&a);
    if(!e)e=gps_power_pmic(pmif,&b);
    if(!e)gps_power_hwv(hwv,&b);
    if(!e && !gps_power_same(&a,&b))e=EAGAIN;
    /* SN-LDO vocal 10..15 is outside the documented +90mV range. */
    if(!e && (b.voltage&15)>9)e=ERANGE;
   }
   X9PMIFRelease();
  }
 }
 uint64_t now=mach_absolute_time();
 uint32_t uv=500000u+(uint32_t)(b.voltage>>4)*100000u+
     (uint32_t)(b.voltage&15)*10000u;
 lck_mtx_lock(&gps_lock);
 st.power_sample_valid=!e;st.power_errno=e;st.power_sample_time=now;
 /* No partially read fields are published as legitimate power readings. */
 if(!e) {
  st.power_uv=uv;st.power_control=b.control;st.power_da=b.da;
  st.power_hw_en=b.hw_en;st.power_rc_mux=b.rc_mux;
  st.power_ap_vote=b.ap;st.power_global_vote=b.global;st.power_busy=b.busy;
  st.power_done=b.done;st.power_ack=b.ack;
 }
 lck_mtx_unlock(&gps_lock);
 printf("X9 GPS power observation: errno=%d stable=%u SID3 uv=%u control=%x da=%x hw_en=%x mux=%x ap=%x global=%x busy=%x done=%x ack=%x; no power/DMA lease\n",
     e,!e,!e?uv:0,!e?b.control:0,!e?b.da:0,!e?b.hw_en:0,!e?b.rc_mux:0,
     !e?b.ap:0,!e?b.global:0,!e?b.busy:0,!e?b.done:0,!e?b.ack:0);
 /* One observation only, no polling, fault-generation mutation or READY.
  * Keep Device mappings for boot lifetime; no cached aliases of HWV/PMIF. */
}
