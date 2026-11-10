/* Native MT6993 cold D-die phase, actual conninfra.ko 547a0 -> 55960 /
 * 55a28 / 55a6c. Not a connv2 supplier or a runtime power probe. Only a
 * version-2 supplier may expose the exclusive cold SRAM/DMA boundary.
 * No caller/default supplier is fabricated here; v1 never enters this.
 * Do not move the 8KiB clear before acquire or after pos_finalize/HIF. */
static int gps_pos_guard(void)
{
 if(gps_rt.ops.version!=2 || !gps_rt.ops.pos_validate ||
    !(gps_rt.layout.flags&X9_GPS_POS_LAYOUT_VERIFIED))return ENXIO;
 int e=gps_mcu_guard();if(e)return e;
 e=gps_rt.ops.pos_validate(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease);
 if(e)return e;
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
 printf("X9 GPS native D-die: cold SRAM phase completed; POS finalize/MCU/MNL still required\n");
 return 0;
}
