/* Native MT6993 cold D-die phase, actual conninfra.ko 547a0 -> 55960 /
 * 55a28 / 55a6c. Not a connv2 supplier or a runtime power probe. Only a
 * version-2..4 supplier may expose the exclusive cold SRAM/DMA boundary.
 * No caller/default supplier is fabricated here; v1 never enters this.
 * Do not move the 8KiB clear before acquire or after pos_finalize/HIF. */
static int gps_pos_guard(void)
{
 if(gps_rt.ops.version<2 || gps_rt.ops.version>4 || !gps_rt.ops.pos_validate ||
    !(gps_rt.layout.flags&X9_GPS_POS_LAYOUT_VERIFIED))return ENXIO;
 int e=gps_mcu_guard();if(e)return e;
 e=gps_rt.ops.pos_validate(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease);
 if(e)return e;
 if(gps_rt.ops.version==4) {
  /* CMDBT engine is not covered by merely holding the GPS MCU reset.
   * Keep TOP wake and cold HW-control disabled throughout SRAM writes.
   * Phase1 journals the one potentially accepted final HW-control write;
   * phase2 then requires its readback for all remaining POS accesses. */
  e=gps_mcu_raw_expect(0x400601a0,1,1);if(e)return e;
  if(gps_rt.pos_lp_hw_phase!=1) {
   e=gps_mcu_raw_expect(0x40001200,1,gps_rt.pos_lp_hw_phase==2 ? 1 : 0);
   if(e)return e;
  }
 }
 /* Recheck the cold MCU boundary for every word, not only before clear. */
 e=gps_mcu_expect(F_reset,0);if(e)return e;
 return gps_mcu_expect(F_function,0);
}
static int gps_pos_read(uint32_t address,uint32_t *value)
{
 int e=gps_pos_guard();if(e)return e;
 return gps_mcu_read(address,value);
}
static int gps_pos_write(uint32_t address,uint32_t value,uint32_t mask)
{
 int e=gps_pos_guard();if(e)return e;
 e=gps_mcu_write(address,value);if(e)return e;
 uint32_t got;e=gps_pos_read(address,&got);if(e)return e;
 return (got&mask)==(value&mask) ? 0 : EIO;
}
static int gps_pos_d_die(void)
{
 if(gps_rt.pos_started || gps_rt.pos_complete)return EBUSY;
 MCU_TRY(gps_pos_guard());
 MCU_TRY(gps_mcu_poll(F_version,GPS_MCU_MT6993_VERSION));
 /* Do not reset an unknown running MCU to manufacture a cold baseline. */
 MCU_TRY(gps_mcu_expect(F_reset,0));MCU_TRY(gps_mcu_expect(F_function,0));
 if(gps_rt.ops.pos_clock_schematic>3)return EINVAL;
 gps_rt.pos_started=1; /* journal before first possibly accepted SRAM write */
 /* Each word, including its readback, checks the retained lease and
  * exclusive SRAM/DMA qualification. Bound work/yield on the sole worker,
  * never in QRTR, an IRQ, a spinlock or a supplier callback. */
 for(unsigned off=0;off<0x2000;off+=4) {
  MCU_TRY(gps_pos_write(0x40050000+off,0,UINT32_MAX));
  if((off&0xff)==0xfc)gps_runtime_pause(1);
 }
 MCU_TRY(gps_pos_write(0x4004b070,0x08000000,UINT32_MAX));
 MCU_TRY(gps_pos_write(0x4004b074,0xbfffffff,UINT32_MAX));
 /* Match actual D-die schema: enums 0..2 become 1..3; enum3 becomes0.
  * This encoding is not a claim that either oscillator is running. */
 uint32_t clock=gps_rt.ops.pos_clock_schematic;
 MCU_TRY(gps_pos_write(0x4005103c,clock<3 ? clock+1 : 0,UINT32_MAX));
 uint32_t efuse;MCU_TRY(gps_pos_read(0x40011020,&efuse));
 MCU_TRY(gps_pos_write(0x40051020,efuse,UINT32_MAX));
 MCU_TRY(gps_pos_write(0x40000050,0,UINT32_MAX));
 MCU_TRY(gps_pos_write(0x40000054,1,UINT32_MAX));
 MCU_TRY(gps_pos_write(0x400000b0,0x1f,UINT32_MAX));
 MCU_TRY(gps_pos_write(0x400000b4,0,UINT32_MAX));
 uint32_t rgu;MCU_TRY(gps_pos_read(0x40000000,&rgu));
 /* 0x494e is the write key, not a readable ACK. Validate only low16. */
 MCU_TRY(gps_pos_write(0x40000000,(rgu&0xffbf)|0x494e0000,0xffff));
 MCU_TRY(gps_pos_write(0x40051038,3,UINT32_MAX));
 MCU_TRY(gps_mcu_expect(F_reset,0));MCU_TRY(gps_mcu_expect(F_function,0));
 MCU_TRY(gps_pos_guard());
 gps_rt.pos_d_die_complete=1;
 printf("X9 GPS native D-die: cold SRAM phase completed; POS finalize/MCU/MNL still required\n");
 return 0;
}
/* Actual conninfra.ko 55b20/9c and 55bc4/a0, through the cold route
 * 103b8 -> 1041c -> 1046c. These are register configuration stages,
 * NOT an RF transaction, PLL-lock ACK, ROM qualification or GPS fix. */
static int gps_pos_rf_guard(void)
{
 if((gps_rt.ops.version!=3 && gps_rt.ops.version!=4) || !gps_rt.ops.pos_afe ||
    !(gps_rt.layout.flags&X9_GPS_RF_LAYOUT_VERIFIED))return ENXIO;
 return gps_pos_guard();
}
static int gps_pos_rf_set(uint32_t address,uint32_t mask,uint32_t value)
{
 if(!mask || (value&~mask))return EINVAL;
 MCU_TRY(gps_pos_rf_guard());
 uint32_t old;MCU_TRY(gps_pos_read(address,&old));
 /* Retained EXCLUSIVE WT_SLP/AFE qualification makes this masked RMW
  * legitimate; a stable register snapshot alone must never grant it. */
 MCU_TRY(gps_pos_rf_guard());
 return gps_pos_write(address,(old&~mask)|value,mask);
}
static int gps_pos_spi(void)
{
 if(!gps_rt.pos_d_die_complete || gps_rt.pos_spi_started ||
    gps_rt.pos_afe_started || gps_rt.pos_pll_started || gps_rt.pos_complete)return EBUSY;
 MCU_TRY(gps_pos_rf_guard());
 gps_rt.pos_spi_started=1; /* journal before any possibly accepted write */
 MCU_TRY(gps_pos_rf_set(0x40003084,0x00000fff,0x00000a00));
 MCU_TRY(gps_pos_rf_set(0x40003088,0x0fff0fff,0x0afc0a0c));
 MCU_TRY(gps_pos_rf_set(0x40003094,0x00000fff,0x000000fc));
 MCU_TRY(gps_pos_rf_set(0x40003098,0x00000fff,0x000000f8));
 MCU_TRY(gps_pos_rf_guard());gps_rt.pos_spi_complete=1;
 return 0;
}
static int gps_pos_afe(void)
{
 if(!gps_rt.pos_spi_complete || gps_rt.pos_afe_started ||
    gps_rt.pos_pll_started || gps_rt.pos_complete)return EBUSY;
 MCU_TRY(gps_pos_rf_guard());
 gps_rt.pos_afe_started=1; /* even a failed SMC may have changed hardware */
 MCU_TRY(gps_rt.ops.pos_afe(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease));
 MCU_TRY(gps_pos_rf_guard());gps_rt.pos_afe_complete=1;
 return 0;
}
static int gps_pos_pll(void)
{
 if(!gps_rt.pos_afe_complete || gps_rt.pos_pll_started || gps_rt.pos_complete)return EBUSY;
 MCU_TRY(gps_pos_rf_guard());
 gps_rt.pos_pll_started=1;
 /* The actual MT6993 wrapper 549d4 calls this vendor table
  * unconditionally. Its name xtal_26000k is NOT oscillator evidence;
  * acquire/validate must still qualify the measured clock schematic. */
 MCU_TRY(gps_pos_rf_set(0x400410f4,0x7fff7fff,0x020d0521));
 MCU_TRY(gps_pos_rf_set(0x40041004,0x000300cf,0x00030047));
 MCU_TRY(gps_pos_rf_set(0x4004100c,0x00078000,0x00068000));
 MCU_TRY(gps_pos_rf_guard());gps_rt.pos_pll_complete=1;
 printf("X9 GPS native POS: WT_SLP/AFE/subsystem PLL configuration completed; radio/low-power/MCU/MNL still required\n");
 return 0;
}
