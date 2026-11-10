/* Native cold MT6993 POS: actual conninfra.ko 54a94 and 55c68/5fc.
 * This KO includes WLA; do not substitute the source's !CONN_WLA branch.
 * Requires an exclusive v4/v5 cold supplier and retained TOP force-wake.
 * No supplier, ROM/MNL firmware, GPS backend or pressure data is invented.
 * Original CMDBT bytes remain a private provider input, never in git. */
static int gps_pos_cmdbt_identity(void)
{
 /* Actual KO .rodata symbol 1ebb8/1000, file24f58, exactly matches the
  * OPPO mt6993 source array. Identity is NOT a secure boot/power lease. */
 static const uint8_t expected[32]={
  0xd3,0x34,0x03,0xec,0x28,0x65,0x7a,0xdc,0x2c,0x74,0xa9,0xad,0xce,0xd9,0x1f,0x11,
  0xee,0x28,0xe1,0x09,0x2c,0xf2,0xe5,0x1d,0x2b,0xe6,0x07,0x1a,0x87,0x3c,0x8b,0xb1};
 struct sha256 hash;uint8_t digest[32];sha256_init(&hash);
 sha256_update(&hash,gps_rt.cmdbt,sizeof(gps_rt.cmdbt));sha256_final(&hash,digest);
 return memcmp(digest,expected,sizeof(expected)) ? EINVAL : 0;
}
static int gps_pos_lp_guard(void)
{
 if((gps_rt.ops.version<4 || gps_rt.ops.version>6) || !gps_rt.ops.pos_radio || !gps_rt.pos_cmdbt_valid ||
    !(gps_rt.layout.flags&X9_GPS_LP_LAYOUT_VERIFIED))return ENXIO;
 MCU_TRY(gps_pos_rf_guard());
 /* TOP wake is 1a0, NOT the v062 GPS wake at1ac. Neither is an ACK.
  * Never let native hardware-control leave us accessing a slept domain. */
 MCU_TRY(gps_mcu_raw_expect(0x400601a0,1,1));
 uint32_t rc;MCU_TRY(gps_mcu_read(0x1c011004,&rc));
 if(gps_rt.pos_rc_valid && (rc&1)!=gps_rt.pos_rc_mode)return EAGAIN;
 if(gps_rt.pos_radio_complete)
  MCU_TRY(gps_mcu_raw_expect(0x40051034,UINT32_MAX,4));
 return 0;
}
static int gps_pos_lp_write(uint32_t address,uint32_t value,uint32_t mask)
{
 MCU_TRY(gps_pos_lp_guard());
 MCU_TRY(gps_pos_write(address,value,mask));
 return gps_pos_lp_guard();
}
static int gps_pos_lp_set(uint32_t address,uint32_t mask,uint32_t value)
{
 if(!mask || (value&~mask))return EINVAL;
 MCU_TRY(gps_pos_lp_guard());
 uint32_t old;MCU_TRY(gps_pos_read(address,&old));
 return gps_pos_lp_write(address,(old&~mask)|value,mask);
}
static int gps_pos_radio(void)
{
 if(!gps_rt.pos_pll_complete || gps_rt.pos_radio_started ||
    gps_rt.pos_lp_started || gps_rt.pos_complete)return EBUSY;
 MCU_TRY(gps_pos_lp_guard());
 uint32_t rc;MCU_TRY(gps_mcu_read(0x1c011004,&rc));
 gps_rt.pos_rc_mode=rc&1;gps_rt.pos_rc_valid=1;
 MCU_TRY(gps_pos_lp_guard());
 /* D-die cleared this shared SW bitmap. Any unexpected radio invalidates
  * the cold baseline; never clear it to manufacture exclusive ownership.
  * Actual CONNDRV_TYPE_GPS=2, set mask4 (bit2), not a READY bit. */
 uint32_t status;MCU_TRY(gps_pos_read(0x40051034,&status));
 if(status)return EBUSY;
 gps_rt.pos_radio_started=1;
 MCU_TRY(gps_pos_lp_write(0x40051034,4,UINT32_MAX));
 MCU_TRY(gps_rt.ops.pos_radio(gps_rt.ops.context,&gps_rt.layout,
     gps_rt.generation,gps_rt.lease));
 MCU_TRY(gps_pos_lp_guard());
 MCU_TRY(gps_mcu_raw_expect(0x40051034,UINT32_MAX,4));
 gps_rt.pos_radio_complete=1;
 return 0;
}
struct gps_pos_lp_step {uint32_t address,mask,value;};
static int gps_pos_lp_steps(const struct gps_pos_lp_step *steps,unsigned count)
{
 for(unsigned i=0;i<count;i++)
  MCU_TRY(gps_pos_lp_set(steps[i].address,steps[i].mask,steps[i].value));
 return 0;
}
static int gps_pos_low_power(void)
{
 if(!gps_rt.pos_radio_complete || !gps_rt.pos_rc_valid || gps_rt.pos_lp_started ||
    gps_rt.pos_complete)return EBUSY;
 MCU_TRY(gps_pos_lp_guard());MCU_TRY(gps_pos_cmdbt_identity());
 gps_rt.pos_lp_started=1; /* journal before the first possibly accepted write */
 MCU_TRY(gps_pos_lp_set(0x40001200,0xf000,0x1000));
 if(gps_rt.pos_rc_mode) {
  MCU_TRY(gps_pos_lp_write(0x40001354,0x02080706,UINT32_MAX));
  MCU_TRY(gps_pos_lp_set(0x40001350,0x8000,0));
  MCU_TRY(gps_pos_lp_write(0x40001384,0x02080706,UINT32_MAX));
  MCU_TRY(gps_pos_lp_set(0x40001380,0x8000,0));
  MCU_TRY(gps_pos_lp_set(0x40001348,8,8));
  gps_runtime_pause(30); /* actual RC source requires at least30us */
  MCU_TRY(gps_pos_lp_set(0x40001348,1,1));
  MCU_TRY(gps_pos_lp_set(0x40001340,0x10,0x10));
  MCU_TRY(gps_pos_lp_set(0x40001348,0x100000,0x100000));
  MCU_TRY(gps_pos_lp_set(0x40001340,0x80000000,0));
  MCU_TRY(gps_pos_lp_set(0x40001340,1,0));
 } else MCU_TRY(gps_pos_lp_set(0x40001340,0x80,0));
 MCU_TRY(gps_pos_lp_set(0x40001300,0xffffff,0x080706));
 MCU_TRY(gps_pos_lp_set(0x40001304,0x10000,0));
 uint32_t rgu;MCU_TRY(gps_pos_read(0x40000020,&rgu));
 /* Key4254 is not an ACK; only the low16 control bits are readable. */
 MCU_TRY(gps_pos_lp_write(0x40000020,(rgu&0xffbf)|0x42540000,0xffff));
 /* Exact compiled order, including separately sequenced DCM/timeout and
  * WLA enables. Do not combine writes or import an FPGA/no-WLA branch. */
 static const struct gps_pos_lp_step steps[]={
  {0x40011100,0x40000000,0x40000000}, {0x40011100,0x40000,0x40000},
  {0x40011100,0x7ff0,0x3ff0}, {0x40011100,0x8000,0x8000},
  {0x4000e110,0x4000,0x4000}, {0x4000e110,0x10000,0x10000},
  {0x4000e104,0x4000,0x4000}, {0x4000e104,0x10000,0x10000},
  {0x4000e120,1,1},
  {0x4004b3d8,0xfffff,0x18050}, {0x4004b3dc,0xfffff,0x18050},
  {0x4004b374,0x1000,0x1000},
  {0x4004b024,0x7f8,0x7f8}, {0x4004b024,1,1},
  {0x4000e038,0x7f8,0x200}, {0x4000e038,1,1},
  {0x4000e024,0x7f8,0x600}, {0x4000e024,1,1},
  {0x4004d000,0x200,0x200}, {0x4004d000,0xffff0000,0x0fde0000},
  {0x4004d000,4,4}, {0x4004d000,8,8}, {0x4004d000,0x10,0x10},
  {0x4004d000,0x200,0},
  {0x40012000,1,1}, {0x40012004,1,1},
  {0x40012038,0x80,0}, {0x40012038,0x40,0x40}, {0x40012038,0x10,0x10},
  {0x40060380,0x80000000,0x80000000}, {0x40060380,1,1},
  {0x40012050,1,0},
  {0x4004b148,0x10,0x10},
  {0x40060128,0x7f,1}, {0x40060128,0x80,0x80},
  {0x40011420,2,2}, {0x40011420,1,0},
  {0x40011424,1,1}, {0x40011428,1,1}, {0x4001142c,1,1},
  {0x40011430,0x3fff0,0x3fff0}, {0x40011430,1,1}
 };
 MCU_TRY(gps_pos_lp_steps(steps,sizeof(steps)/sizeof(steps[0])));
 MCU_TRY(gps_pos_lp_write(0x40011050,0x18050200,UINT32_MAX));
 /* CMDBT restore code occupies the FIRST4KiB, before SW metadata at
  * +1034/+1038/+103c. Check exclusive lease, RC mode, TOP wake and cold
  * reset/function boundary for EVERY word and its readback. */
 for(unsigned off=0;off<sizeof(gps_rt.cmdbt);off+=4) {
  const uint8_t *p=gps_rt.cmdbt+off;
  uint32_t word=p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
  MCU_TRY(gps_pos_lp_write(0x40050000+off,word,UINT32_MAX));
  if((off&0xff)==0xfc)gps_runtime_pause(1);
 }
 MCU_TRY(gps_pos_lp_set(0x40001200,0x40,0x40)); /* wake must restore first */
 gps_rt.pos_lp_hw_phase=1; /* journal before potentially accepted HW control */
 MCU_TRY(gps_pos_lp_set(0x40001200,1,1)); /* HW control; TOP wake stays held */
 gps_rt.pos_lp_hw_phase=2;
 MCU_TRY(gps_pos_lp_guard());gps_rt.pos_lp_complete=1;
 printf("X9 GPS native low-power: RC=%u WLA/CMDBT configuration completed, TOP wake retained; PMIC/MT6686/ROM/MNL still required\n",gps_rt.pos_rc_mode);
 return 0;
}
