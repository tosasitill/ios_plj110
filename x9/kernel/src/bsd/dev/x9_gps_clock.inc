/* Actual PLJ110 conninfra.ko clock buffer 60490/a4: CKSYS_TOP 1c210000,
 * SET74/CLR78 bit24, 20us settle. clk-mt6993 CLK_CFG_6 is readable at70.
 * AP CLK_PD_CONN is HWV bit18; hwccf_api v1 AP uses EN20708/DONE1132c,
 * unlike MM's STA1131c. is_enabled uses ALL_EN13700/ALL_STA13704.
 *
 * This transaction is downstream of an actual retained supplier lease,
 * not that missing supplier. No HWV/rail/PLL acquisition is inferred here.
 * It cannot run with only a topology flag, Android READY or a snapshot.
 * The upstream PLL/exclusive selector remain the supplier's responsibility.
 * No CLR/unvote or rollback of an ambiguous SET while DMA may depend on it.
 */
#define GPS_CLOCK_SELECTOR (1u<<24)
#define GPS_CLOCK_CONN (1u<<18)
static void gps_clock_journal(int error)
{
 lck_mtx_lock(&gps_lock);
 st.clock_started=gps_rt.clock.started;
 st.clock_set_started=gps_rt.clock.set_started;
 st.clock_complete=gps_rt.clock.complete;
 st.clock_borrowed=gps_rt.clock.borrowed;
 st.clock_prior=gps_rt.clock.prior;st.clock_selector=gps_rt.clock.selector;
 st.clock_ap=gps_rt.clock.ap;st.clock_global=gps_rt.clock.global;
 st.clock_busy=gps_rt.clock.busy;st.clock_done=gps_rt.clock.done;
 st.clock_ack=gps_rt.clock.ack;st.clock_errno=error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_clock_authority(void)
{
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version<2 ||
    !(gps_rt.layout.flags&X9_GPS_CLOCK_LAYOUT_VERIFIED) ||
    !(gps_rt.layout.flags&X9_GPS_POWER_LAYOUT_VERIFIED) ||
    !gps_rt.ops.pos_validate)return ENXIO;
 int e=gps_mcu_supplier_guard();if(e)return e;
 /* Cold acquisition additionally needs the POS-exclusive boundary. Once
  * taken, the regular validate callback retains the upstream/selector
  * lease even after MCU reset release. Do not apply a cold-only verifier
  * to live HIF traffic. Never recurse through the native MCU guard. */
 if(gps_rt.clock.complete)return 0;
 return gps_rt.ops.pos_validate(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease);
}
static int gps_clock_read(volatile uint32_t *p,unsigned off,uint32_t *out)
{
 int e=gps_clock_authority();if(e)return e;
 if(!p)return ENXIO;
 *out=gps_power_read(p,off);return 0;
}
static int gps_clock_power(void)
{
 int e=gps_clock_read(gps_rt.clock.hwv,0x20708,&gps_rt.clock.ap);
 if(!e)e=gps_clock_read(gps_rt.clock.hwv,0x13700,&gps_rt.clock.global);
 if(!e)e=gps_clock_read(gps_rt.clock.hwv,0x13704,&gps_rt.clock.busy);
 if(!e)e=gps_clock_read(gps_rt.clock.hwv,0x1132c,&gps_rt.clock.done);
 if(!e)e=gps_clock_read(gps_rt.clock.hwv,0x12900,&gps_rt.clock.ack);
 if(e)return e;
 /* The AP vote and actual domain effect must both remain held. An SCP
  * aggregate vote or sticky DONE by itself never replaces our AP holder.
  * Other HWV bits may change; do not compare the complete shared word. */
 if(!(gps_rt.clock.ap&GPS_CLOCK_CONN) ||
    !(gps_rt.clock.global&GPS_CLOCK_CONN) ||
    (gps_rt.clock.busy&GPS_CLOCK_CONN) ||
    !(gps_rt.clock.done&GPS_CLOCK_CONN) ||
    !(gps_rt.clock.ack&GPS_CLOCK_CONN))return ENXIO;
 return gps_clock_authority();
}
static int gps_clock_effect(void)
{
 int e=gps_clock_power();if(e)return e;
 e=gps_clock_read(gps_rt.clock.top,0x70,&gps_rt.clock.selector);
 if(!e && !(gps_rt.clock.selector&GPS_CLOCK_SELECTOR))e=ESTALE;
 return e;
}
static int gps_clock_check(void)
{
 int e=gps_clock_effect();
 if(e)gps_clock_journal(e);
 return e;
}
static int gps_clock_take(void)
{
 if(gps_rt.clock.started)return EBUSY;
 int e=gps_clock_authority();if(e)return e;
 gps_rt.clock.started=1;gps_clock_journal(0);
 gps_rt.clock.top=(volatile uint32_t *)ml_io_map(0x1c210000,0x1000);
 e=gps_clock_authority();if(e)goto done;
 gps_rt.clock.hwv=(volatile uint32_t *)ml_io_map(0x1c040000,0x74900);
 if(!gps_rt.clock.top || !gps_rt.clock.hwv){e=ENOMEM;goto done;}
 e=gps_clock_power();if(e)goto done;
 e=gps_clock_read(gps_rt.clock.top,0x70,&gps_rt.clock.prior);if(e)goto done;
 gps_rt.clock.borrowed=!!(gps_rt.clock.prior&GPS_CLOCK_SELECTOR);
 if(!gps_rt.clock.borrowed) {
  /* Recheck before the one potentially accepted selector write. Never
   * RMW the SET alias: its readable bits are not the owned write mask. */
  uint32_t now;
  e=gps_clock_power();if(e)goto done;
  e=gps_clock_read(gps_rt.clock.top,0x70,&now);if(e)goto done;
  if((now^gps_rt.clock.prior)&GPS_CLOCK_SELECTOR){e=ESTALE;goto done;}
  e=gps_clock_authority();if(e)goto done;
  gps_rt.clock.set_started=1;gps_clock_journal(0);
  gps_power_write(gps_rt.clock.top,0x74,GPS_CLOCK_SELECTOR);
  /* Match the measured minimum settle on this unlocked worker. An event
   * wake must not shorten the 20us hardware delay. This is not a guessed
   * RF oscillator rate. Any later guard failure retains the accepted SET. */
  delay(20);
 }
 e=gps_clock_effect();if(e)goto done;
 gps_rt.clock.complete=1;
done:
 gps_clock_journal(e);
 printf("X9 GPS clock: selector transaction errno=%d set=%u complete=%u borrowed=%u; upstream supplier lease retained, not GPS readiness\n",
     e,gps_rt.clock.set_started,gps_rt.clock.complete,gps_rt.clock.borrowed);
 return e;
}
#undef GPS_CLOCK_SELECTOR
#undef GPS_CLOCK_CONN
