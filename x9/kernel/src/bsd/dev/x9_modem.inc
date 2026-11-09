/* X9 modem (MediaTek MD1, gen 6299 "MOLY.NR17") bring-up, stage 1.
 *
 * Goal: the lower half of a QcomBaseband-style bridge. QcomBaseband talks QMI
 * to a Qualcomm modem over QRTR/IPA; on MT6993 the modem speaks CCCI over
 * CCIF (control/AT/FSD ring buffers) + DPMAIF (IP data). Before any protocol
 * work we must prove that XNU can power the MD and receive its first message.
 *
 * Source contract (OPPO kernel_device_modules-6.12):
 *   eccci/fsm/md_sys1_platform.c  md_start_platform / md_cd_power_on / md_cd_let_md_go
 *   eccci/fsm/ccci_fsm.c          fsm_routine_start (HS1 -> runtime data -> HS2)
 *   eccci/hif/ccci_hif_ccif.c     ccif_start / ccci_reset_ccif_hw / md_ccif_sram_rx_work
 * Captured PLJ110 DT (.local/x9/modem/live.dts):
 *   mddriver: load-md-stage="bl2_ext_load_md", power-flow-config=0x10 (DPSW only),
 *             ccci-ctrl-mtcmos=1, md-generation=0x189b, ap-plat-info=0x1b51 (6993)
 *   ccifdriver: AP 0x142c0000, MD 0x142c1000, reset 0x1443f000+0xf30 bit 20 (ver 2),
 *             sram 0x200, clk free-run
 * Android ccci_dump of the same boot (.local/x9/modem/ccci_dump.txt):
 *   MTCMOS ON ret=0 -> vmodem 815 mV -> rf_pmic_lp -> MD_KERNEL_BOOT_UP
 *   (boot_slave=1) -> runtime data 147 ms later (HS1) -> READY at 9.64 s.
 *   md1_rf_voter_enable runs after MTCMOS (mt6661-3@3 / mt6661-6@6 exist).
 *
 * BL2 loaded and verified md1img into the md_mem_usage mblocks; the shim
 * drops every live reserved-memory range from XNU's RAM map, so the MD
 * image and the AP/MD shared memory (0xec000000 nc, 0xf0000000 c) are not
 * XNU pages. Nothing here writes DRAM.
 *
 * x9_modem=0  off (default)
 * x9_modem=1  read-only probe: LK tag table, TF-A MD status, CCIF registers
 * x9_modem=2  + power on (TF-A SMCs only), let MD go, wait for HS1 in CCIF
 *               SRAM and dump it. No runtime data is sent, so the MD will
 *               time out waiting for HS2 and assert; that is expected here.
 * x9_modem=4  stage 3: stage 2 plus kernel RPC (ch32), RAM-backed FSD (ch14),
 *               sysmsg replies; listen 120 s. See x9_modem_s3.inc.
 * x9_modem=3  stage 2: before boot-up parse the LK smem tables, clear nc smem,
 *               build the CCISM ringbufs; after HS1 write the AP runtime data
 *               (58 features, Android algorithm) + AP query into CCIF SRAM,
 *               ring H2D_SRAM, then poll the rings for HS2 -> READY and log
 *               every MD packet for 30 s. Nothing answers FSD/RPC yet, so the
 *               MD may assert later; that is stage 3.
 */

#define X9MD_SIP_CCCI 0xc2000505ull   /* MTK_SIP_KERNEL_CCCI_CONTROL: SMC64 fast SiP 0x505 */
/* CCCI_SECURE_REQ_ID (inc/modem_secure_base.h) */
#define X9MD_CLOCK_REQUEST 5
#define X9MD_POWER_CONFIG 6
#define X9MD_FLIGHT_MODE_SET 7
#define X9MD_CCIF_CLK_REQUEST 14
#define X9MD_CCIF_CLK_RELEASE 15
#define X9MD_MTCMOS_ENABLE 18
#define X9MD_MTCMOS_FAIL_DUMP 20
/* MD_POWER_CONFIG_ID */
#define X9MD_KERNEL_BOOT_UP 0
#define X9MD_CHECK_FLAG 2
#define X9MD_CHECK_DONE 3
#define X9MD_BOOT_STATUS 4
#define X9MD_LK_SET_DPSW 9

#define X9MD_LK_TAG_PA 0xbffd0000ull
#define X9MD_CCIF_AP_PA 0x142c0000ull
#define X9MD_CCIF_MD_PA 0x142c1000ull
#define X9MD_CCIF_RST_PA 0x1443f000ull
#define X9MD_CCIF_RST_OFF 0xf30u
#define X9MD_CCIF_RST_BIT 20u
#define X9MD_SRAM_SIZE 0x200u

/* APCCIF registers (hif/ccif_hif_reg.h) */
#define X9MD_CON 0x00
#define X9MD_BUSY 0x04
#define X9MD_START 0x08
#define X9MD_TCHNUM 0x0c
#define X9MD_RCHNUM 0x10
#define X9MD_ACK 0x14
#define X9MD_IRQ0_MASK 0x20
#define X9MD_IRQ1_MASK 0x24
#define X9MD_CHDATA 0x100
#define X9MD_D2H_SRAM 15u
#define X9MD_FEATURE_QUERY 0x49434343u  /* MD_FEATURE_QUERY_PATTERN "CCCI" */

static vm_offset_t x9md_ap,x9md_md;
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr,vm_size_t size);
extern void read_random(void *buffer,u_int numbytes);

static uint64_t x9md_smc(uint64_t req,uint64_t a2,uint64_t a3,uint64_t out[4]) {
 register uint64_t x0 __asm__("x0")=X9MD_SIP_CCCI;
 register uint64_t x1 __asm__("x1")=req;
 register uint64_t x2 __asm__("x2")=a2;
 register uint64_t x3 __asm__("x3")=a3;
 register uint64_t x4 __asm__("x4")=0;
 register uint64_t x5 __asm__("x5")=0;
 register uint64_t x6 __asm__("x6")=0;
 register uint64_t x7 __asm__("x7")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::"memory");
 if(out){out[0]=x0;out[1]=x1;out[2]=x2;out[3]=x3;}
 return x0;
}
static uint32_t x9md_rd(vm_offset_t b,unsigned off){return *(volatile uint32_t *)(b+off);}
static void x9md_wr(vm_offset_t b,unsigned off,uint32_t v){*(volatile uint32_t *)(b+off)=v;__asm__ volatile("dsb sy":::"memory");}
static void x9md_sleep_ms(unsigned ms){delay_for_interval(ms,1000*NSEC_PER_USEC);}

/* LK/BL2 tag table (ccci_util_lib_fo.c lk_tag_inf_parsing_to_args_table).
 * Header layout comes from "ccci,modem_info_v2"; on this LK the table sits
 * at 0xbffd0000, 0x1a10 bytes, 0x1c tags (Android INIT LOG). v2 tag = 64-byte
 * name + data_offset + data_size + next_tag_offset. Print names and sizes so
 * the next stage can take smem_layout/ccb_info/md_mem from here instead of
 * hard-coding them. */
static vm_offset_t x9md_tag;
static void x9md_dump_tags(void) {
 vm_offset_t t=x9md_tag=ml_io_map(X9MD_LK_TAG_PA,0x4000);
 if(!t){printf("X9 MODEM: tag map failed\n");return;}
 unsigned off=0;
 for(unsigned i=0;i<0x1c && off<0x3f00;i++){
  char name[65];
  for(unsigned k=0;k<64;k++)name[k]=*(volatile char *)(t+off+k);
  name[64]=0;
  uint32_t doff=x9md_rd(t,off+64),dsz=x9md_rd(t,off+68),next=x9md_rd(t,off+72);
  uint32_t w0=doff+4<=0x4000?x9md_rd(t,doff&~3u):0,w1=doff+8<=0x4000?x9md_rd(t,(doff&~3u)+4):0;
  printf("X9 MODEM: tag %2u '%s' data@0x%x size %u [%08x %08x] next 0x%x\n",i,name,doff,dsz,w0,w1,next);
  if(next<=off||next>=0x4000)break;
  off=next;
 }
 /* Full table (0x1a10 bytes on this LK) so stage 2 can be written from data:
  * smem_layout, ccb_info, md mem/MPU, nc/c smem item lists. */
 for(unsigned o=0;o<0x1a20;o+=32)
  printf("X9 MODEM: TAGHEX +%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",o,
   x9md_rd(t,o),x9md_rd(t,o+4),x9md_rd(t,o+8),x9md_rd(t,o+12),x9md_rd(t,o+16),x9md_rd(t,o+20),x9md_rd(t,o+24),x9md_rd(t,o+28));
}

static void x9md_ccif_dump(const char *when) {
 printf("X9 MODEM: CCIF %s AP con=%08x busy=%08x start=%08x rch=%08x mask0=%08x mask1=%08x | MD busy=%08x start=%08x rch=%08x\n",
  when,x9md_rd(x9md_ap,X9MD_CON),x9md_rd(x9md_ap,X9MD_BUSY),x9md_rd(x9md_ap,X9MD_START),x9md_rd(x9md_ap,X9MD_RCHNUM),
  x9md_rd(x9md_ap,X9MD_IRQ0_MASK),x9md_rd(x9md_ap,X9MD_IRQ1_MASK),
  x9md_rd(x9md_md,X9MD_BUSY),x9md_rd(x9md_md,X9MD_START),x9md_rd(x9md_md,X9MD_RCHNUM));
}
static void x9md_sram_dump(vm_offset_t b,const char *which,unsigned bytes) {
 for(unsigned o=0;o<bytes;o+=32)
  printf("X9 MODEM: SRAM %s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",which,o,
   x9md_rd(b,X9MD_CHDATA+o),x9md_rd(b,X9MD_CHDATA+o+4),x9md_rd(b,X9MD_CHDATA+o+8),x9md_rd(b,X9MD_CHDATA+o+12),
   x9md_rd(b,X9MD_CHDATA+o+16),x9md_rd(b,X9MD_CHDATA+o+20),x9md_rd(b,X9MD_CHDATA+o+24),x9md_rd(b,X9MD_CHDATA+o+28));
}

static void x9md_status(const char *when) {
 uint64_t r[4];
 x9md_smc(X9MD_POWER_CONFIG,X9MD_CHECK_DONE,0,r);
 printf("X9 MODEM: %s CHECK_DONE a0=%llx (0=BROM pass)\n",when,r[0]);
 x9md_smc(X9MD_POWER_CONFIG,X9MD_CHECK_FLAG,0,r);
 printf("X9 MODEM: %s CHECK_FLAG %llx %llx %llx %llx\n",when,r[0],r[1],r[2],r[3]);
 x9md_smc(X9MD_POWER_CONFIG,X9MD_BOOT_STATUS,0,r);
 printf("X9 MODEM: %s BOOT_STATUS %llx %llx %llx %llx (expect boot-status-value 544300f1 when up)\n",when,r[0],r[1],r[2],r[3]);
}

/* ccif_start: hw reset (clears busy/wch/irq, not SRAM), clear both SRAMs. */
static void x9md_ccif_reset(void) {
 vm_offset_t rst=ml_io_map(X9MD_CCIF_RST_PA,0x1000);
 if(rst){
  x9md_wr(rst,X9MD_CCIF_RST_OFF,1u<<X9MD_CCIF_RST_BIT);
  x9md_wr(rst,X9MD_CCIF_RST_OFF+4,1u<<X9MD_CCIF_RST_BIT);
 } else printf("X9 MODEM: CCIF reset map failed\n");
 for(unsigned o=0;o<X9MD_SRAM_SIZE;o+=4){x9md_wr(x9md_ap,X9MD_CHDATA+o,0);x9md_wr(x9md_md,X9MD_CHDATA+o,0);}
}


/* ---- MT6661 rails over PMIF SWINF2 (same channel/lock as touch + gauge) ----
 * spmi-dev-mask 0x55a8: SID3/SID5 on PMIF-P 0x1c018000, SID6 on PMIF-M 0x1c01a000.
 * mt6661-regulator.h: BUCK1 (vmodem, SID6) vosel 0x240 en 0x1487 da 0x14c5;
 * BUCK3 (vdigrf, SID6) 0x242/0x1587/0x15c5; BUCK6 (vsram, SID5) 0x245/0x1707/0x1745;
 * vosel = uV/5000. Both vmodem and vsram are regulator-always-on in the DT, so on
 * Android the regulator core keeps them enabled; XNU has no regulator driver.
 * 112.txt: MTCMOS_ENABLE returned PWR_ACK fail (0xfffffff1) 50 times. */
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
static vm_offset_t x9md_pmif_m,x9md_pmif_p;
static vm_offset_t x9md_bus(unsigned sid){return (0x55a8u>>sid)&1?x9md_pmif_p:x9md_pmif_m;}
static int x9md_pmif_wait(vm_offset_t b,unsigned state){
 for(unsigned i=0;i<10000;i++){uint32_t v=x9md_rd(b,0x8a8);if(((v>>1)&7)==state)return (v&(1u<<18))?-1:0;delay(1);}
 return -1;
}
/* 113.txt: after one failed PMIF-P write the error bit stayed set and every
 * later SID3/SID5 access failed. Clear it (VLDCLR) before a single retry. */
static void x9md_pmif_clear(vm_offset_t b){x9md_wr(b,0x8a4,1);delay(10);}
static int x9md_pmic_rd(unsigned sid,unsigned addr,uint8_t *val){
 vm_offset_t b=x9md_bus(sid);int rc=-1;
 if(!b||X9PMIFAcquire())return -1;
 if(!x9md_pmif_wait(b,0)){
  x9md_wr(b,0x880,(3u<<30)|(sid<<24)|addr);
  if(!x9md_pmif_wait(b,6)){*val=(uint8_t)x9md_rd(b,0x894);x9md_wr(b,0x8a4,1);rc=x9md_pmif_wait(b,0);}
 }
 X9PMIFRelease();return rc;
}
static int x9md_pmic_wr_once(unsigned sid,unsigned addr,uint8_t val);
static int x9md_pmic_wr(unsigned sid,unsigned addr,uint8_t val){
 int rc=x9md_pmic_wr_once(sid,addr,val);
 if(rc){x9md_pmif_clear(x9md_bus(sid));rc=x9md_pmic_wr_once(sid,addr,val);
  printf("X9 MODEM: PMIC SID%u %04x write retried after error clear rc=%d sta=%08x\n",sid,addr,rc,x9md_rd(x9md_bus(sid),0x8a8));}
 return rc;
}
static int x9md_pmic_wr_once(unsigned sid,unsigned addr,uint8_t val){
 /* whitelist: vosel/en of the three MD rails and the three MD RF voter SET regs */
 int ok=(sid==6&&(addr==0x240||addr==0x1487||addr==0x242||addr==0x1808||addr==0x1788))||
        (sid==5&&(addr==0x245||addr==0x1707))||(sid==3&&addr==0x1788)||
        /* md1_set_rf_pmic_lp: BUCK3/5/6 en, LNC0_8, LN60_5, LN60_7, LNC0_9, PMRC_CON0, LNC0_S_11/12, LNC0_10 */
        (sid==6&&(addr==0x1587||addr==0x1687||addr==0x1707||addr==0x2087||addr==0x1f87||addr==0x2047||
                  addr==0x20c7||addr==0x190||addr==0x2107||addr==0x2147||addr==0x2007));
 vm_offset_t b=x9md_bus(sid);int rc=-1;
 if(!ok||!b||X9PMIFAcquire())return -1;
 if(!x9md_pmif_wait(b,0)){
  x9md_wr(b,0x884,val);x9md_wr(b,0x880,(3u<<30)|(1u<<29)|(sid<<24)|addr);rc=x9md_pmif_wait(b,0);
 }
 X9PMIFRelease();return rc;
}
struct x9md_rail{const char *name;unsigned sid,vosel,en,da;uint8_t target;int must_on;};
static const struct x9md_rail x9md_rails[]={
 {"vmodem(6_buck1)",6,0x240,0x1487,0x14c5,163,1},   /* 815000 uV */
 /* vsram: Android's DPSW flow never writes it (113.txt: 0x93 = 735 mV, on);
  * target 0x93 keeps it untouched. A write to SID5 0x245 failed on PMIF-P. */
 {"vsram(5_buck6)",5,0x245,0x1707,0x1745,0x93,0},
 {"vdigrf(6_buck3)",6,0x242,0x1587,0x15c5,140,0},   /* 700000 uV, DPSW sets voltage only */
};
static void x9md_rails_dump(const char *when){
 for(unsigned i=0;i<sizeof(x9md_rails)/sizeof(x9md_rails[0]);i++){
  const struct x9md_rail *r=&x9md_rails[i];uint8_t id=0,v=0,en=0,da=0;
  int e=x9md_pmic_rd(r->sid,8,&id)|x9md_pmic_rd(r->sid,r->vosel,&v)|x9md_pmic_rd(r->sid,r->en,&en)|x9md_pmic_rd(r->sid,r->da,&da);
  printf("X9 MODEM: rail %s %s SID%u hwcid=%02x vosel=%02x(%u uV) en=%02x da=%02x%s\n",when,r->name,r->sid,id,v,v*5000u,en,da,e?" (PMIF error)":"");
 }
}
/* Bring the always-on MD rails up the way the Linux regulator core would. */
static void x9md_rails_on(void){
 for(unsigned i=0;i<sizeof(x9md_rails)/sizeof(x9md_rails[0]);i++){
  const struct x9md_rail *r=&x9md_rails[i];uint8_t v=0,en=0;
  if(x9md_pmic_rd(r->sid,r->vosel,&v)||x9md_pmic_rd(r->sid,r->en,&en))continue;
  if(v!=r->target){int rc=x9md_pmic_wr(r->sid,r->vosel,r->target);printf("X9 MODEM: rail %s vosel %02x -> %02x rc=%d\n",r->name,v,r->target,rc);}
  if(r->must_on&&!(en&1)){int rc=x9md_pmic_wr(r->sid,r->en,(uint8_t)(en|1));printf("X9 MODEM: rail %s enable %02x -> %02x rc=%d\n",r->name,en,en|1,rc);x9md_sleep_ms(1);}
 }
}
/* md1_rf_voter_enable (step 5, after MTCMOS on 6993): VOTER_MD_BIT into
 * SID3 BUCK5_VOTER0_CON0_SET, SID6 BUCK6_VOTER0_CON0_SET, SID6 BUCK5_VOTER0_CON0_SET. */
static void x9md_rf_voters(void){
 int a=x9md_pmic_wr(3,0x1788,0x8),b=x9md_pmic_wr(6,0x1808,0x8),c=x9md_pmic_wr(6,0x1788,0x8);
 printf("X9 MODEM: RF voters VS1=%d VS2_2=%d VS3_2=%d\n",a,b,c);
}

/* md1_set_rf_pmic_lp (md_sys1_platform.c, ap_plat 6993, first power on; Android
 * logged "[POWER ON] md1_set_rf_pmic_lp done"). 115.txt without it: MD EE
 * 49 ms after runtime data, offender DIGRF1st in digrf_platform/md99/src/digrf_mipi.c.
 * Supplies (live.dts mddriver -> mt6661-6 regulators): vdigrf BUCK3, vs3-2 BUCK5,
 * vs2-2 BUCK6, vsram-digrf LNC0_8, vrfio12 LN60_5, vrfio18 LNC0_9, va10 LN60_7,
 * vrf09 LNC0_S_11, vrf12 LNC0_S_12, vrf18 LNC0_10. DT ranges are wide, so the
 * regulator core only sets the EN bit (bit 0); voltages stay at PMIC defaults.
 * chipid sw_ver = 1 (B0, atag,chipid) -> clear LN60_7 EN instead of enabling va10. */
#define X9MD_SET_RF_PMIC_LP 10
static void x9md_ldo_on(const char *name,unsigned addr){
 uint8_t v=0;int rc=x9md_pmic_rd(6,addr,&v);
 if(rc){printf("X9 MODEM: RF %s %04x read failed\n",name,addr);return;}
 int w=0;if(!(v&1))w=x9md_pmic_wr(6,addr,(uint8_t)(v|1));
 uint8_t a=0;x9md_pmic_rd(6,addr,&a);
 printf("X9 MODEM: RF %s en %04x %02x -> %02x%s\n",name,addr,v,a,w?" (write failed)":"");
}
static int x9md_rf_step(unsigned step,unsigned us){
 uint64_t r[4];x9md_smc(X9MD_POWER_CONFIG,X9MD_SET_RF_PMIC_LP,step,r);
 printf("X9 MODEM: RF tfa step %u a0=%llx\n",step,r[0]);
 if(us)delay(us);
 return r[0]==step?0:-1;
}
static void x9md_rf_pmic_lp(void){
 x9md_ldo_on("vdigrf(buck3)",0x1587);
 x9md_ldo_on("vs3_2(buck5)",0x1687);
 x9md_ldo_on("vs2_2(buck6)",0x1707);
 if(x9md_rf_step(1,200))return;
 x9md_ldo_on("vsram_digrf(lnc0_8)",0x2087);
 x9md_ldo_on("vrfio12(ln60_5)",0x1f87);
 {uint8_t v=0;if(!x9md_pmic_rd(6,0x2047,&v)){int w=(v&1)?x9md_pmic_wr(6,0x2047,(uint8_t)(v&~1u)):0;printf("X9 MODEM: RF ln60_7(va10) B0 clear en %02x rc=%d\n",v,w);}}
 if(x9md_rf_step(2,200))return;
 x9md_ldo_on("vrfio18(lnc0_9)",0x20c7);
 if(x9md_rf_step(3,500))return;
 uint8_t pm=0;
 if(x9md_pmic_rd(6,0x190,&pm)){printf("X9 MODEM: RF PMRC_CON0 read failed\n");return;}
 int w1=x9md_pmic_wr(6,0x190,(uint8_t)(pm&0xef));
 if(x9md_rf_step(4,0))return;
 int w2=x9md_pmic_wr(6,0x190,(uint8_t)(pm|0x10));
 printf("X9 MODEM: RF PMRC_CON0 %02x mask rc=%d restore rc=%d\n",pm,w1,w2);
 x9md_ldo_on("vrf09(lnc0_s_11)",0x2107);
 x9md_ldo_on("vrf12(lnc0_s_12)",0x2147);
 x9md_ldo_on("vrf18(lnc0_10)",0x2007);
 if(x9md_rf_step(5,200))return;
 printf("X9 MODEM: RF md1_set_rf_pmic_lp done\n");
}

/* md_cd_topclkgen_on: cksys_top (ccci-topckgen) +0 clear bits 8,9 (Android read back 0). */
static void x9md_topclk(void){
 vm_offset_t ck=ml_io_map(0x1c210000,0x1000);
 if(!ck){printf("X9 MODEM: topckgen map failed\n");return;}
 uint32_t v=x9md_rd(ck,0);
 if(v&0x300)x9md_wr(ck,0,v&~0x300u);
 printf("X9 MODEM: topckgen md1_clk_mod %08x -> %08x\n",v,x9md_rd(ck,0));
}

/* md_cd_power_on for power-flow-config=0x10 + md_cd_let_md_go (gen > 6295). */
static int x9md_power_on(void) {
 uint64_t r[4];
 x9md_smc(X9MD_CCIF_CLK_REQUEST,0,0,r);
 printf("X9 MODEM: CCIF_CLK_REQUEST a0=%llx\n",r[0]);
 x9md_ccif_reset();
 x9md_ccif_dump("after-reset");
 x9md_rails_dump("before");
 x9md_rails_on();
 x9md_rails_dump("after-on");
 x9md_topclk();
 x9md_smc(X9MD_FLIGHT_MODE_SET,0,0,r);
 printf("X9 MODEM: FLIGHT_MODE_SET(0) %llx %llx %llx %llx\n",r[0],r[1],r[2],r[3]);
 /* ccci_md_mtcmos_on: a0 bit for PWR_ACK fail -> retry with arg 1 up to 50 ms */
 x9md_smc(X9MD_MTCMOS_ENABLE,0,0,r);
 unsigned tries=0;
 /* md_sys1_platform.h: ON_PWR_ACK_FAIL 0xfffffff1, EMI_BUS_PROT_FAIL 0xfffffff3 */
 while(r[0]&&tries<50){
  uint32_t a=(uint32_t)r[0];
  printf("X9 MODEM: MTCMOS_ENABLE a0=%llx retry %u\n",r[0],tries);
  if(a==0xfffffff3u||(a&0xfffffff1u)!=0xfffffff1u)break;  /* EMI protect / unknown: stop */
  x9md_sleep_ms(1);
  x9md_smc(X9MD_MTCMOS_ENABLE,1,0,r);
  tries++;
 }
 printf("X9 MODEM: MTCMOS_ENABLE final a0=%llx after %u retries\n",r[0],tries);
 if(r[0]){
  x9md_rails_dump("mtcmos-fail");
  x9md_smc(X9MD_MTCMOS_FAIL_DUMP,1,0,NULL);  /* MTCMOS_ON_FAIL_DUMP */
  return -1;
 }
 /* md_cd_dpsw_setting: SMC(0) / vmodem 815 mV (already set by x9md_rails_on) / SMC(1). */
 x9md_smc(X9MD_POWER_CONFIG,X9MD_LK_SET_DPSW,0,r);
 printf("X9 MODEM: DPSW(0) a0=%llx DPSW_CTRL=%llx\n",r[0],r[3]);
 x9md_sleep_ms(1);
 x9md_smc(X9MD_POWER_CONFIG,X9MD_LK_SET_DPSW,1,r);
 printf("X9 MODEM: DPSW(1) a0=%llx DPSW_CTRL=%llx\n",r[0],r[3]);
 x9md_rf_voters();
 /* step 6: md1_set_rf_pmic_lp when !md_first_power_on, i.e. on the first power on */
 x9md_rf_pmic_lp();
 x9md_smc(X9MD_POWER_CONFIG,X9MD_KERNEL_BOOT_UP,0,r);
 printf("X9 MODEM: KERNEL_BOOT_UP ret=%llu boot_status_0=%llu boot_status_1=%llu boot_slave=%llu (Android: 0 0 0 1)\n",
  r[0],r[1],r[2],r[3]);
 return 0;
}

/* Poll for D2H_SRAM (channel 15) = HS1; IRQ wiring comes with stage 2. */
static int x9md_wait_hs1(void) {
 uint64_t t0=mach_absolute_time();
 uint32_t seen=0;
 for(unsigned ms=0;ms<15000;ms+=5){
  uint32_t ch=x9md_rd(x9md_ap,X9MD_RCHNUM);
  if(ch&~seen){
   uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);
   printf("X9 MODEM: RCHNUM %08x at %llu ms\n",ch,ns/1000000);
   seen|=ch;
  }
  if(ch&(1u<<X9MD_D2H_SRAM)){
   x9md_wr(x9md_ap,X9MD_ACK,1u<<X9MD_D2H_SRAM);
   uint32_t d0=x9md_rd(x9md_ap,X9MD_CHDATA),d1=x9md_rd(x9md_ap,X9MD_CHDATA+4),
            chw=x9md_rd(x9md_ap,X9MD_CHDATA+8),rsv=x9md_rd(x9md_ap,X9MD_CHDATA+12),
            head=x9md_rd(x9md_ap,X9MD_CHDATA+16);
   printf("X9 MODEM: HS1 header data0=%08x data1=%08x ch/seq=%08x reserved=%08x head=%08x %s\n",
    d0,d1,chw,rsv,head,head==X9MD_FEATURE_QUERY?"(MD feature query OK)":"(unexpected pattern)");
   x9md_sram_dump(x9md_ap,"AP",X9MD_SRAM_SIZE);
   x9md_status("after-HS1");
   return 0;
  }
  x9md_sleep_ms(5);
 }
 printf("X9 MODEM: no HS1 within 15 s\n");
 x9md_ccif_dump("hs1-timeout");
 x9md_sram_dump(x9md_ap,"AP",0x60);
 x9md_sram_dump(x9md_md,"MD",0x60);
 x9md_status("hs1-timeout");
 return -1;
}

/* ======================= stage 2: runtime data -> HS2 =======================
 * x9_modem=3. Mirrors what the Android eccci driver does between ccif_start and
 * READY (OPPO kernel_device_modules-6.12):
 *   ccci_util_md_mem.c  LK "nc_smem_layout"/"c_smem_layout" tag -> region table
 *                       (hash = last entry per id; md view = md_offset+0x40000000)
 *   ap_md_mem.c         init_smem_regions (static table order, md_phy carried
 *                       over for ids the LK table lacks), post_cfg_for_ccb,
 *                       first-boot clear of every region without SMF_NCLR_FIRST
 *   ccci_hif_ccif.c     16 normal + 16 exception ringbufs in CCISM_MCU(_EXP),
 *                       gen>=6298 sizes; ccif_sram_layout (dl hdr, MD query,
 *                       up hdr, AP query) at APCCIF_CHDATA; send = BUSY+TCHNUM
 *   ccci_fsm.c          ccci_md_prepare_runtime_data / config_ap_side_feature
 *   modem_sys1.c        md_cd_send_runtime_data_v2 (v2_1 when MULTI_MD_MPU)
 *   ccci_ringbuf.c      packet = AABBAABB,len | ccci_header+payload | CCDDEEFF x2
 * Values Android computed from sources XNU does not have are copied from the
 * Android ccci_dump of this phone (.local/x9/modem/ccci_dump.txt) and marked.
 * HS2 = CCCI_CONTROL_RX packet with data[1]=MD_INIT_START_BOOT(0) and
 * reserved != MD_INIT_CHK_ID (ccci_fsm_recv_control_packet). */

#define X9MD_NC_PA 0xec000000ull       /* ap_md_nc_smem, smem_layout tag */
#define X9MD_NC_SIZE 0x140000u
#define X9MD_SMEM_MAX 47               /* SMEM_USER_MAX */
#define X9MD_CHK_ID 0x5555ffffu        /* MD_INIT_CHK_ID */
#define X9MD_AP_QUERY 0x43434349u      /* AP_FEATURE_QUERY_PATTERN */

static vm_offset_t x9md_nc;            /* write-combine map of the whole nc smem */
struct x9md_reg{int exists,in_lk;uint32_t md,size,ap_off,flags;};
static struct x9md_reg x9md_regs[X9MD_SMEM_MAX];
static uint32_t x9md_nc_total,x9md_c_total,x9md_c_md,x9md_nc_md,x9md_img_size;
static uint64_t x9md_bank0;   /* LK md_bank0_base: moves every boot (113 1e0430000, 114 1e0a60000) */

static uint32_t x9md_m32(uint32_t off){return *(volatile uint32_t *)(x9md_nc+off);}
static void x9md_w32(uint32_t off,uint32_t v){*(volatile uint32_t *)(x9md_nc+off)=v;}
static void x9md_zero(uint32_t off,uint32_t size){for(uint32_t o=0;o+4<=size;o+=4)x9md_w32(off+o,0);}

static int x9md_tag_find(const char *want,uint32_t *doff,uint32_t *dsz){
 if(!x9md_tag)return -1;
 unsigned off=0;
 for(unsigned i=0;i<0x1c&&off<0x3f00;i++){
  unsigned k=0;
  for(;k<64;k++){char c=*(volatile char *)(x9md_tag+off+k);if(c!=want[k])break;if(!c){k=64;break;}}
  uint32_t next=x9md_rd(x9md_tag,off+72);
  if(k==64){*doff=x9md_rd(x9md_tag,off+64);*dsz=x9md_rd(x9md_tag,off+68);return 0;}
  if(next<=off||next>=0x4000)break;
  off=next;
 }
 return -1;
}

/* rt_smem_region_lk_fmt: u64 ap_phy, u64 ap_vir, {int id, ap_offset, size, align, flags, md_offset} */
struct x9md_cr{uint64_t pa;uint32_t sz;int id;};
static struct x9md_cr x9md_crs[8];
static unsigned x9md_ncr;
static volatile uint32_t *x9md_cmap[8];
static int x9md_lk_table(const char *name,const char *numname,int cached,uint32_t *total,uint32_t *md0){
 uint32_t doff,dsz,noff,nsz;
 if(x9md_tag_find(name,&doff,&dsz)||x9md_tag_find(numname,&noff,&nsz)){printf("X9 MODEM: S2 tag %s missing\n",name);return -1;}
 uint32_t n=x9md_rd(x9md_tag,noff);
 if(n*40>dsz){printf("X9 MODEM: S2 %s n=%u size %u mismatch\n",name,n,dsz);return -1;}
 *total=0;
 for(uint32_t i=0;i<n;i++){
  uint32_t e=doff+i*40;
  uint64_t ap=x9md_rd(x9md_tag,e)|((uint64_t)x9md_rd(x9md_tag,e+4)<<32);
  int id=(int)x9md_rd(x9md_tag,e+16);
  uint32_t apo=x9md_rd(x9md_tag,e+20),sz=x9md_rd(x9md_tag,e+24),fl=x9md_rd(x9md_tag,e+32),mdo=x9md_rd(x9md_tag,e+36);
  if(cached&&x9md_ncr<8)x9md_crs[x9md_ncr++]=(struct x9md_cr){ap,sz,id};
  if(i==0)*md0=mdo+0x40000000u;
  *total+=sz;
  if(id>=0&&id<X9MD_SMEM_MAX){
   struct x9md_reg *r=&x9md_regs[id];
   r->in_lk=1;r->md=mdo+0x40000000u;r->size=sz;r->flags=fl;r->ap_off=cached?0xffffffffu:apo;
   if(!cached&&ap!=X9MD_NC_PA+apo)printf("X9 MODEM: S2 nc id %d ap %llx != base+%x\n",id,ap,apo);
  }
  printf("X9 MODEM: S2 %s[%2u] id=%2d ap=%llx md=%08x size=%06x flags=%x\n",cached?"c ":"nc",i,id,ap,mdo+0x40000000u,sz,fl);
 }
 return 0;
}

/* ap_md_mem.c md1_noncacheable_tbl / md1_cacheable_tbl order (BIGDATA ids are
 * #ifdef MTK_TC10_FEATURE_BIG_DATA, off on this build). */
static const uint8_t x9md_nc_order[]={10,27,31,34,12,13,37,18,14,15,33,0,16,4,7,8,9,19,20,40,41,11,23,29,30,42};
static const uint8_t x9md_c_order[]={22,32,1,2,3,5,6,46,28,24};
static const uint8_t x9md_nclr_first[]={34,19,20,41,23,29,42,22,24};   /* SMF_NCLR_FIRST */

static void x9md_regions_emulate(const uint8_t *ord,unsigned n){
 uint32_t md=0;  /* init_smem_regions: md_phy keeps the last found value */
 for(unsigned i=0;i<n;i++){
  struct x9md_reg *r=&x9md_regs[ord[i]];
  r->exists=1;
  if(r->in_lk)md=r->md;else{r->md=md;r->size=0;}
 }
}

static int x9md_smem_init(void){
 for(unsigned i=0;i<X9MD_SMEM_MAX;i++)x9md_regs[i]=(struct x9md_reg){0};
 if(x9md_lk_table("nc_smem_layout","nc_smem_layout_num",0,&x9md_nc_total,&x9md_nc_md))return -1;
 if(x9md_lk_table("c_smem_layout","c_smem_layout_num",1,&x9md_c_total,&x9md_c_md))return -1;
 {uint32_t o,z;
  if(!x9md_tag_find("md_bank0_base",&o,&z)&&z>=8)x9md_bank0=x9md_rd(x9md_tag,o)|((uint64_t)x9md_rd(x9md_tag,o+4)<<32);
  if(!x9md_tag_find("md1img",&o,&z)&&z>=4)x9md_img_size=x9md_rd(x9md_tag,o);
  printf("X9 MODEM: S2 md bank0 %llx img_size %x\n",x9md_bank0,x9md_img_size);}
 x9md_regions_emulate(x9md_nc_order,sizeof(x9md_nc_order));
 x9md_regions_emulate(x9md_c_order,sizeof(x9md_c_order));
 /* post_cfg_for_ccb with md1_ccb_gear_ver=1 (live.dts) -> 1 MiB control part */
 struct x9md_reg *ccb=&x9md_regs[1];
 uint32_t ccb_size=ccb->size,ctrl=0x100000,raw=ccb_size>ctrl?ccb_size-ctrl:0;
 if(!raw)ctrl=0;
 for(unsigned id=5;id<=6;id++){x9md_regs[id].size=raw;x9md_regs[id].md=ccb->md+(raw?ctrl:0);}
 printf("X9 MODEM: S2 nc total %x md %08x | c total %x md %08x | CCB %08x+%x raw %08x+%x\n",
  x9md_nc_total,x9md_nc_md,x9md_c_total,x9md_c_md,ccb->md,ccb_size,x9md_regs[5].md,raw);
 x9md_nc=ml_io_map_wcomb(X9MD_NC_PA,X9MD_NC_SIZE);
 if(!x9md_nc){printf("X9 MODEM: S2 nc smem map failed\n");return -1;}
 /* first boot: clear every nc region without SMF_NCLR_FIRST (cacheable regions
  * are left alone; Android clears them through its own wc mapping) */
 uint32_t cleared=0;
 for(unsigned i=0;i<sizeof(x9md_nc_order);i++){
  unsigned id=x9md_nc_order[i];int keep=0;
  for(unsigned k=0;k<sizeof(x9md_nclr_first);k++)if(x9md_nclr_first[k]==id)keep=1;
  struct x9md_reg *r=&x9md_regs[id];
  if(keep||!r->in_lk||!r->size||r->ap_off+r->size>X9MD_NC_SIZE)continue;
  x9md_zero(r->ap_off,r->size);cleared+=r->size;
 }
 printf("X9 MODEM: S2 cleared %x bytes of nc smem\n",cleared);
 /* libccci_util ccci_ccb_init_users() (run by ccci_mdinit BEFORE the MD boots, disassembled from the Android
  * vendor lib): for every ccb_configs_table_1M entry it fills a 64-byte buffer_header in the CCB control region:
  * dl half  [+0x14]=page size [+0x18]=buffer size [+0x1c]=0x1100ffee, ul half the same at +0x34/+0x38/+0x3c;
  * everything else zero. The MD's ccci_shm_srv reads these (a zero header -> ccci_shm_bm.c:633 / dhl_channel_ccb.c:200). */
 {
  static const uint32_t cfg[20][4]={   /* dl_page, ul_page, dl_buf, ul_buf (port_smem.c ccb_configs_table_1M) */
   {1024,1024,1024*16,1024*16},{20*1024,128,1024*20*6,128},{512,1024,512*296,1024*32},{512,128,512*8,128},
   {128,128,128,128},{128,128,128,128},{128,128,128,128},{128,128,128,128},{128,128,128,128},{128,128,128,128},
   {128,128,128,128},{128,128,128,128},{128,128,128,128},{128,128,128,128},{128,128,128,128},{128,128,128,128},
   {128,128,128,128},{128,128,128,128*3},{512,1024,512*32,1024*16},{65*1024,65*1024,65*1024*4,65*1024*4}};
  struct x9md_reg *cc=&x9md_regs[4];
  if(cc->in_lk&&cc->ap_off+20*64<=X9MD_NC_SIZE&&cc->size>=20*64){
   for(unsigned e=0;e<20;e++){
    uint32_t o=cc->ap_off+e*64;
    for(unsigned w=0;w<16;w++)x9md_w32(o+w*4,0);
    x9md_w32(o+0x14,cfg[e][0]);x9md_w32(o+0x18,cfg[e][2]);x9md_w32(o+0x1c,0x1100ffeeu);
    x9md_w32(o+0x34,cfg[e][1]);x9md_w32(o+0x38,cfg[e][3]);x9md_w32(o+0x3c,0x1100ffeeu);
   }
   printf("X9 MODEM: S2 CCB control headers initialised (20 entries at nc+%x)\n",cc->ap_off);
  }else printf("X9 MODEM: S2 CCB control region missing/too small (%x)\n",cc->size);
 }
 /* clear_smem_region(first_boot) also wipes the cacheable regions (CCB etc.) except
  * the NCLR_FIRST ones (CONSYS id22, USIP id24): Android left stale CCB/DHL state there */
 uint32_t cc=0;
 for(unsigned i=0;i<x9md_ncr;i++){
  struct x9md_cr *c=&x9md_crs[i];
  if(c->id==22||c->id==24||!c->sz||(c->pa&0xfff))continue;
  /* ml_io_map VA is a permanent 8 MiB io_submap (123.txt panic: an 11.5 MiB map failed in kmem_alloc):
  * only wipe the first 1 MiB of each region, which is the CCB control part */
  uint32_t sz=c->sz>0x100000?0x100000:c->sz;
  volatile uint64_t *m=(volatile uint64_t *)ml_io_map_wcomb((vm_offset_t)c->pa,(sz+0xfff)&~0xfffu);
  if(!m){printf("X9 MODEM: S2 c smem %llx map failed\n",c->pa);continue;}
  x9md_cmap[i]=(volatile uint32_t *)m;
  for(uint32_t o=0;o<sz/8;o++)m[o]=0;
  __asm__ volatile("dsb sy":::"memory");
  cc+=sz;
 }
 printf("X9 MODEM: S2 cleared %x bytes of cacheable smem\n",cc);
 return 0;
}

/* ---- ringbufs ---- */
#define X9MD_QN 16
static const uint16_t x9md_rxq_k[X9MD_QN]={80,80,40,80,20,20,48,0,8,16};   /* rx_queue_buffer_size_up_98 (KiB) */
static const uint16_t x9md_txq_k[X9MD_QN]={128,40,8,40,20,20,48,0,8,16};
static const uint16_t x9md_rxe_k[X9MD_QN]={12,32,8,0,0,0,8};               /* rx/tx_exp_buffer_size_up_95 */
struct x9md_q{uint32_t ctl,rx,tx;};   /* ctl = nc offset of struct ccci_ringbuf */
static struct x9md_q x9md_q[X9MD_QN],x9md_qe[X9MD_QN];
#define X9MD_RB_CTL 40u               /* CCCI_RINGBUF_CTL_LEN = 8 + 24 + 8 */

static uint32_t x9md_ring_create(uint32_t off,uint32_t rx,uint32_t tx,struct x9md_q *q){
 uint32_t len=X9MD_RB_CTL+rx+tx;
 x9md_zero(off,len);
 x9md_w32(off,0xee0000eeu);x9md_w32(off+4,0xee0000eeu);
 x9md_w32(off+len-4,0xff0000ffu);x9md_w32(off+len-8,0xff0000ffu);
 q->ctl=off+8;q->rx=rx;q->tx=tx;
 x9md_w32(q->ctl+0,0);x9md_w32(q->ctl+4,0);x9md_w32(q->ctl+8,rx);
 x9md_w32(q->ctl+12,0);x9md_w32(q->ctl+16,0);x9md_w32(q->ctl+20,tx);
 return len;
}
static int x9md_rings_init(void){
 struct x9md_reg *n=&x9md_regs[19],*e=&x9md_regs[20];
 if(!n->in_lk||!e->in_lk||n->ap_off+n->size>X9MD_NC_SIZE||e->ap_off+e->size>X9MD_NC_SIZE){printf("X9 MODEM: S2 CCISM regions missing\n");return -1;}
 x9md_zero(n->ap_off,n->size);x9md_zero(e->ap_off,e->size);
 uint32_t off=0;
 for(unsigned i=0;i<X9MD_QN;i++){
  uint32_t len=X9MD_RB_CTL+x9md_rxq_k[i]*1024u+x9md_txq_k[i]*1024u;
  if(off+len>n->size){printf("X9 MODEM: S2 CCISM_MCU too small (%x)\n",n->size);return -1;}
  off+=x9md_ring_create(n->ap_off+off,x9md_rxq_k[i]*1024u,x9md_txq_k[i]*1024u,&x9md_q[i]);
 }
 n->size=off;   /* md_ccif_normal_ring_buf_init: ccism->size = total_smem_size */
 uint32_t eo=0;
 for(unsigned i=0;i<X9MD_QN;i++)
  eo+=x9md_ring_create(e->ap_off+eo,x9md_rxe_k[i]*1024u,x9md_rxe_k[i]*1024u,&x9md_qe[i]);
 printf("X9 MODEM: S2 rings normal %x/%x (q0 rx %x tx %x) exp %x/%x\n",off,0xb4400u,x9md_q[0].rx,x9md_q[0].tx,eo,e->size);
 return 0;
}

static void x9md_ring_copy(uint32_t buf,uint32_t len,uint32_t pos,uint8_t *dst,uint32_t n){
 for(uint32_t i=0;i<n;i++){dst[i]=*(volatile uint8_t *)(x9md_nc+buf+pos);if(++pos>=len)pos=0;}
}
/* ccci_ringbuf_readable + read + move_rpointer on the RX half. Returns payload
 * length (header included), 0 if empty, <0 on a corrupt ring. */
static int x9md_ring_read(struct x9md_q *q,uint8_t *out,uint32_t cap,uint32_t *plen){
 uint32_t rd=x9md_m32(q->ctl),wr=x9md_m32(q->ctl+4),len=q->rx,buf=q->ctl+24;
 if(!len)return 0;
 if(rd>=len||wr>=len)return -1;
 uint32_t size=wr>=rd?wr-rd:wr+len-rd;
 if(size<16+16)return 0;
 uint32_t h[2],f[2];
 x9md_ring_copy(buf,len,rd,(uint8_t *)h,8);
 if(h[0]!=0xaabbaabbu)return -2;
 uint32_t pkg=h[1],ccif=(8+pkg+8+7)&~7u;
 if(ccif>size)return 0;
 uint32_t fp=rd+ccif-8;if(fp>=len)fp-=len;
 x9md_ring_copy(buf,len,fp,(uint8_t *)f,8);
 if(f[0]!=0xccddeeffu||f[1]!=0xccddeeffu)return -3;
 uint32_t dp=rd+8;if(dp>=len)dp-=len;
 x9md_ring_copy(buf,len,dp,out,pkg<cap?pkg:cap);
 uint32_t nr=rd+ccif;if(nr>=len)nr-=len;
 __asm__ volatile("dmb sy":::"memory");
 x9md_w32(q->ctl,nr);
 *plen=pkg;
 return 1;
}

/* ---- runtime data ---- */
enum{X9F_NOT_EXIST=0,X9F_NOT=1,X9F_MUST=2,X9F_OPT=3,X9F_BACK=4};
/* config_ap_side_feature for this build: FEATURE_SCP_CCCI_SUPPORT on, TC10 /
 * FAST_HEADER / AP_MD_SECURE / TC1_CUSTOMER_VAL / SYNC_C2K_MEID off, no PHY
 * capture/SIB, no UDC, no AMMS POS, no MD_DATA region, gen 6299. */
static const uint8_t x9md_ap_mask[64]={
 [0]=2,[1]=2,[2]=2,[3]=1,[4]=1,[5]=2,[6]=2,[7]=2,[8]=2,[9]=2,[10]=2,[11]=2,[12]=2,[13]=3,
 [14]=2,[15]=2,[16]=2,[17]=2,[18]=2,[19]=2,[20]=2,[21]=1,[24]=1,[25]=2,[26]=2,[27]=2,[28]=2,
 [29]=1,[30]=2,[31]=3,[32]=3,[33]=1,[34]=1,[35]=3,[36]=2,[37]=1,[38]=3,[45]=2,[49]=1,
 [54]=3,[55]=3,[56]=3,[57]=3};
/* mediatek,spm-resource-setting setting-value-table (live.dts) */
static const uint32_t x9md_spm_tbl[14]={0x53504d00,0x8b1,0xa0,0x8b7,0x01,0x8b1,0x01,0x8b2,0x01,0x1f5,0x01,0x16f,0x1a5,0x1c9};

static uint32_t x9md_rt_off,x9md_rt_pos;
static void x9md_rt_append(uint8_t id,uint8_t sup,uint32_t len,const uint32_t *data){
 x9md_w32(x9md_rt_off+x9md_rt_pos,id|((uint32_t)sup<<8));
 x9md_w32(x9md_rt_off+x9md_rt_pos+4,len);
 x9md_rt_pos+=8;
 if(data){for(uint32_t i=0;i<len/4;i++)x9md_w32(x9md_rt_off+x9md_rt_pos+i*4,data[i]);x9md_rt_pos+=len;}
}
static void x9md_rt_shm(uint8_t id,uint8_t sup,unsigned user){
 uint32_t d[2]={0,0};
 if(user<X9MD_SMEM_MAX&&x9md_regs[user].exists){d[0]=x9md_regs[user].md;d[1]=x9md_regs[user].size;}
 x9md_rt_append(id,sup,8,d);
}
static inline uint64_t x9md_cntvct(void){uint64_t v;__asm__ volatile("isb; mrs %0, cntvct_el0":"=r"(v));return v;}

/* returns 0 when every MD MUST feature was satisfied */
static int x9md_prepare_runtime(const uint8_t md_ft[64]){
 struct x9md_reg *rt=&x9md_regs[14];
 if(!rt->in_lk||rt->size<0x1000){printf("X9 MODEM: S2 runtime region missing\n");return -1;}
 x9md_rt_off=rt->ap_off;x9md_rt_pos=0;
 x9md_zero(x9md_rt_off,0x1000);
 int bad=0;
 char line[200];unsigned lp=0;
 for(unsigned i=0;i<64;i++){
  uint8_t mm=md_ft[i]&15,mv=md_ft[i]>>4,am=x9md_ap_mask[i],av=(i==11&&mv==1)?1:0;
  if(i<32&&(((uint32_t)sweet_x9_arg("x9_md_nof",0)>>i)&1))am=X9F_NOT;   /* experiment: AP says NOT_SUPPORT for these features */
  uint8_t sm=mm,sv=mv;   /* NOT_EXIST / MUST: copy MD's byte */
  if(mm==X9F_MUST&&am<X9F_MUST){printf("X9 MODEM: S2 feature %u is MUST on MD but not on AP\n",i);bad=1;}
  if(mm==X9F_OPT){if(mv==av&&am>=X9F_MUST){sm=X9F_MUST;sv=av;}else{sm=X9F_NOT;sv=av;}}
  else if(mm==X9F_BACK){if(mv>=av){sm=X9F_MUST;sv=av;}else{sm=X9F_NOT;sv=av;}}
  uint8_t sup=(uint8_t)(sm|(sv<<4));
  lp+=snprintf(line+lp,sizeof(line)-lp," %u:%x%x>%x%x",i,mv,mm,sv,sm);
  if((i&15)==15){printf("X9 MODEM: S2 ft%s\n",line);lp=0;}
  if(sm!=X9F_MUST){x9md_rt_append((uint8_t)i,sup,0,NULL);continue;}
  uint32_t d[8]={0};
  switch(i){
  case 0:   /* BOOT_INFO: CCCI_CONTROL_RX, booting_start_id 0xf0000 and ADC "EVB" as Android logged */
   d[0]=0;d[1]=0xf0000;d[2]=0;d[3]=0;x9md_rt_append(i,sup,16,d);break;
  case 1:   /* EXCEPTION_SHARE_MEMORY: MDCCCI_DBG, CCCI_EE_SMEM_TOTAL_SIZE */
   if(x9md_regs[12].exists){d[0]=x9md_regs[12].md;d[1]=64*1024;x9md_rt_append(i,sup,8,d);}break;
  case 2:  x9md_rt_shm(i,sup,19);break;    /* CCIF_SHARE_MEMORY = CCISM_MCU (ring total) */
  case 3:  x9md_rt_shm(i,sup,21);break;
  case 4:  x9md_rt_shm(i,sup,17);break;
  case 14: x9md_rt_shm(i,sup,16);break;    /* CCISM_SCP */
  case 15:  /* CCB: ctrl region + CCB data base/size (0xDF000 = Android CCB user 1..3 total) */
   if(x9md_regs[4].exists){d[0]=x9md_regs[4].md;d[1]=x9md_regs[4].size;}
   d[2]=x9md_regs[1].md;d[3]=0xdf000;x9md_rt_append(i,sup,16,d);break;
  case 16: x9md_rt_shm(i,sup,5);break;     /* RAW_DHL */
  case 25: x9md_rt_shm(i,sup,11);break;    /* LWA */
  case 49: if(x9md_regs[42].exists&&x9md_regs[42].size)x9md_rt_shm(i,sup,42);break;
  case 27: x9md_rt_append(i,sup,0,NULL);break;   /* MULTI_MD_MPU */
  case 17: x9md_rt_shm(i,sup,7);break;
  case 18: x9md_rt_shm(i,sup,8);break;
  case 5: case 6: case 8: case 19: x9md_rt_append(i,sup,16,d);break;
  case 7: read_random(&d[0],4);x9md_rt_append(i,sup,16,d);break;
  case 9: d[0]=0;d[1]=0x79;x9md_rt_append(i,sup,16,d);break;   /* sbp 0, wmid 0x79 (Android log) */
  case 10: d[0]=3;x9md_rt_append(i,sup,16,d);break;            /* seq check + MD status polling */
  case 11: {
   clock_sec_t sec;clock_usec_t usec;clock_get_calendar_microtime(&sec,&usec);
   uint64_t s64=(uint64_t)sec,cnt=x9md_cntvct();
   d[0]=(uint32_t)s64;d[1]=(uint32_t)(s64>>32);d[2]=0;d[3]=0;
   if(sv==1){d[4]=(uint32_t)usec;d[5]=0;d[6]=(uint32_t)cnt;d[7]=(uint32_t)(cnt>>32);x9md_rt_append(i,sup,32,d);}
   else x9md_rt_append(i,sup,16,d);
   break;}
  case 12: x9md_rt_append(i,sup,16,d);break;   /* C2K flags 0 (unlwtg, no 'C') */
  case 13: d[0]=(uint32_t)x9md_bank0;d[1]=x9md_img_size;x9md_rt_append(i,sup,8,d);break; /* img_info[IMG_MD]: bank0 phy (u32) + img size */
  case 20: d[0]=3584-16;x9md_rt_append(i,sup,4,d);break;     /* NET_RX_BUF - ccci_header */
  case 26: x9md_rt_shm(i,sup,9);break;
  case 28: x9md_rt_shm(i,sup,20);break;
  case 29: x9md_rt_append(i,sup,8,d);break;    /* SIB: addr 0 size 0 */
  case 34: d[0]=x9md_regs[27].md;d[1]=x9md_regs[27].size;d[2]=x9md_regs[28].md;d[3]=x9md_regs[28].size;x9md_rt_append(i,sup,16,d);break;
  case 30: x9md_rt_shm(i,sup,22);break;
  case 31: x9md_rt_shm(i,sup,24);break;
  case 32: x9md_rt_append(i,sup,4,d);break;    /* MTEE 0 */
  case 33: x9md_rt_shm(i,sup,29);break;
  case 35: x9md_rt_shm(i,sup,31);break;
  case 56: x9md_rt_shm(i,sup,33);break;
  case 57: x9md_rt_shm(i,sup,0);break;
  case 36: x9md_rt_shm(i,sup,32);break;
  case 38: {   /* MD_MEM_AP_VIEW_INF: bank0, bank4 nc, bank4 c (Android: md MemStart / smem_layout / csmem) */
   uint32_t m[12]={0,0x1e000000u,(uint32_t)x9md_bank0,(uint32_t)(x9md_bank0>>32), 0x40000000u,X9MD_NC_SIZE,(uint32_t)X9MD_NC_PA,0,
                   0x42000000u,x9md_c_total,0xf0000000u,0};
   x9md_rt_append(i,sup,48,m);break;}
  case 45: x9md_rt_shm(i,sup,41);break;
  case 54: x9md_rt_append(i,sup,sizeof(x9md_spm_tbl),x9md_spm_tbl);break;
  case 55: x9md_rt_shm(i,sup,46);break;
  default: break;   /* Android appends nothing either */
  }
 }
 printf("X9 MODEM: S2 AP runtime data %u bytes at md %08x\n",x9md_rt_pos,rt->md);
 for(uint32_t o=0;o<x9md_rt_pos;o+=32)
  printf("X9 MODEM: RT +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",o,
   x9md_m32(x9md_rt_off+o),x9md_m32(x9md_rt_off+o+4),x9md_m32(x9md_rt_off+o+8),x9md_m32(x9md_rt_off+o+12),
   x9md_m32(x9md_rt_off+o+16),x9md_m32(x9md_rt_off+o+20),x9md_m32(x9md_rt_off+o+24),x9md_m32(x9md_rt_off+o+28));
 return bad;
}

/* ccif_hif_fill_rt_header + config_ap_runtime_data_v2_1 into CCIF SRAM, then H2D_SRAM */
static void x9md_send_runtime(void){
 const unsigned up=0xf0,q=0x100;    /* sizeof(ccci_header)+sizeof(md_query_ap_feature) */
 uint32_t rt_md=x9md_regs[14].md;
 x9md_wr(x9md_ap,X9MD_CHDATA+up,0);
 x9md_wr(x9md_ap,X9MD_CHDATA+up+4,16+156);
 x9md_wr(x9md_ap,X9MD_CHDATA+up+8,1);              /* CCCI_CONTROL_TX, seq 0 */
 x9md_wr(x9md_ap,X9MD_CHDATA+up+12,X9MD_CHK_ID);
 uint32_t v[39]={0};
 v[0]=X9MD_AP_QUERY;
 v[1]=0x1000;                 /* feature_set[1] = mask 0, version 1 */
 v[17]=2;                     /* MULTI_MD_MPU_SUPPORT */
 v[18]=rt_md;v[19]=0x800;v[20]=rt_md+0x800;v[21]=0x800;
 v[22]=x9md_nc_md;v[23]=x9md_nc_total;v[24]=x9md_c_md;v[25]=x9md_c_total;
 v[38]=X9MD_AP_QUERY;
 for(unsigned i=0;i<39;i++)x9md_wr(x9md_ap,X9MD_CHDATA+q+i*4,v[i]);
 uint32_t busy=x9md_rd(x9md_ap,X9MD_BUSY);
 if(busy&(1u<<X9MD_D2H_SRAM))printf("X9 MODEM: S2 H2D_SRAM busy %08x\n",busy);
 else{x9md_wr(x9md_ap,X9MD_BUSY,1u<<X9MD_D2H_SRAM);x9md_wr(x9md_ap,X9MD_TCHNUM,X9MD_D2H_SRAM);}
 printf("X9 MODEM: S2 runtime data sent: rt md %08x nc mpu %08x+%x c mpu %08x+%x start=%08x\n",
  rt_md,x9md_nc_md,x9md_nc_total,x9md_c_md,x9md_c_total,x9md_rd(x9md_ap,X9MD_START));
}


/* mdee_dumper_v6 (gen>=6299): struct ex_overview_t at the start of MDSS_DBG */
static uint8_t x9md_b8(uint32_t off){return *(volatile uint8_t *)(x9md_nc+off);}
static uint32_t x9md_u32u(uint32_t off){return x9md_b8(off)|(x9md_b8(off+1)<<8)|(x9md_b8(off+2)<<16)|((uint32_t)x9md_b8(off+3)<<24);}
static void x9md_str(uint32_t off,unsigned max,char *out){unsigned i=0;for(;i<max;i++){char c=(char)x9md_b8(off+i);if(!c)break;out[i]=(c>=32&&c<127)?c:'.';}out[i]=0;}
static void x9md_ee_decode(const char *when){
 struct x9md_reg *ss=&x9md_regs[13],*cc=&x9md_regs[12];
 if(!ss->in_lk||!cc->in_lk)return;
 uint32_t b=ss->ap_off;char str[260];
 printf("X9 MODEM: EE[%s] seqerr=%08x overview_verno=%u core_num=%u\n",when,x9md_u32u(cc->ap_off+0x34),x9md_u32u(b),x9md_u32u(b+4));
 for(unsigned c=0;c<5;c++){x9md_str(b+8+c*12,11,str);printf("X9 MODEM: EE core %u '%s' offender=%u\n",c,str,x9md_b8(b+8+c*12+11));}
 uint32_t e=b+0x44;
 unsigned type=x9md_b8(e)|(x9md_b8(e+1)<<8),cls=x9md_b8(e+3);
 printf("X9 MODEM: EE ex_type=0x%x fmt=%u class=%u (0 assert,1 fatal,2 custom) elm=%u vpe=%u tc=%u\n",type,x9md_b8(e+2),cls,x9md_b8(e+4),x9md_b8(e+5),x9md_b8(e+6));
 x9md_str(e+8,256,str);
 printf("X9 MODEM: EE assert file='%s' line=%u p=%08x %08x %08x lr=%08x\n",str,x9md_u32u(e+8+256),x9md_u32u(e+8+260),x9md_u32u(e+8+264),x9md_u32u(e+8+268),x9md_u32u(e+8+272));
 char off8[9];x9md_str(e+8+12,8,off8);x9md_str(e+8+0x34,200,str);
 printf("X9 MODEM: EE fatal code=%08x %08x %08x offender='%s' status=%08x sp=%08x pc=%08x lr=%08x addr=%08x cause=%08x file='%s'\n",
  x9md_u32u(e+8),x9md_u32u(e+12),x9md_u32u(e+16),off8,x9md_u32u(e+8+0x1c),x9md_u32u(e+8+0x20),x9md_u32u(e+8+0x24),x9md_u32u(e+8+0x28),x9md_u32u(e+8+0x2c),x9md_u32u(e+8+0x30),str);
 for(uint32_t o=0;o<0x180;o+=32)
  printf("X9 MODEM: MDSS +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",o,x9md_m32(b+o),x9md_m32(b+o+4),x9md_m32(b+o+8),x9md_m32(b+o+12),x9md_m32(b+o+16),x9md_m32(b+o+20),x9md_m32(b+o+24),x9md_m32(b+o+28));
 for(uint32_t o=0;o<0x80;o+=32)
  printf("X9 MODEM: MDCCCI +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",o,x9md_m32(cc->ap_off+o),x9md_m32(cc->ap_off+o+4),x9md_m32(cc->ap_off+o+8),x9md_m32(cc->ap_off+o+12),x9md_m32(cc->ap_off+o+16),x9md_m32(cc->ap_off+o+20),x9md_m32(cc->ap_off+o+24),x9md_m32(cc->ap_off+o+28));
 if(when[0]=='A'){   /* ALLQ_RESET: peek at the other shared regions the MD may have been using */
  static const uint8_t ids[]={31,16,12};
  for(unsigned k=0;k<sizeof(ids);k++){
   struct x9md_reg *r=&x9md_regs[ids[k]];
   if(!r->in_lk||r->ap_off==0xffffffffu||r->ap_off+0x40>X9MD_NC_SIZE)continue;
   for(uint32_t o=0;o<0x40;o+=32)
    printf("X9 MODEM: REG%u +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n",ids[k],o,x9md_m32(r->ap_off+o),x9md_m32(r->ap_off+o+4),x9md_m32(r->ap_off+o+8),x9md_m32(r->ap_off+o+12),x9md_m32(r->ap_off+o+16),x9md_m32(r->ap_off+o+20),x9md_m32(r->ap_off+o+24),x9md_m32(r->ap_off+o+28));
  }
 }
 if(when[0]=='A'){   /* CCB control headers (guard 0xddbbccaa) are written by the MD itself: how far did it get? */
  struct x9md_reg *r=&x9md_regs[4];
  if(r->in_lk&&r->ap_off+0x400<=X9MD_NC_SIZE)
   for(uint32_t o=0;o<0x200;o+=32)
    printf("X9 MODEM: CCBCTRL +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",o,x9md_m32(r->ap_off+o),x9md_m32(r->ap_off+o+4),x9md_m32(r->ap_off+o+8),x9md_m32(r->ap_off+o+12),x9md_m32(r->ap_off+o+16),x9md_m32(r->ap_off+o+20),x9md_m32(r->ap_off+o+24),x9md_m32(r->ap_off+o+28));
  for(unsigned k=0;k<x9md_ncr;k++)if(x9md_cmap[k]){
   volatile uint32_t *c=x9md_cmap[k];
   printf("X9 MODEM: CSMEM[%u] pa %llx +00: %08x %08x %08x %08x %08x %08x %08x %08x\n",k,x9md_crs[k].pa,c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7]);

   {uint32_t sz=x9md_crs[k].sz>0x100000?0x100000:x9md_crs[k].sz,nz=0,shown=0;
    for(uint32_t w=0;w<sz/4;w++)if(c[w]){nz++;if(shown<6){printf("X9 MODEM: CSMEM[%u] nonzero +%x = %08x\n",k,w*4,c[w]);shown++;}}
    printf("X9 MODEM: CSMEM[%u] nonzero words in first %x bytes: %u\n",k,sz,nz);}
  }
 }
 /* printable strings anywhere in MDSS_DBG (assert text, task names) */
 unsigned ns=0;
 for(uint32_t o=0;o<ss->size&&ns<40;){
  unsigned n=0;while(o+n<ss->size&&n<200){uint8_t c=x9md_b8(b+o+n);if(c<32||c>126)break;n++;}
  if(n>=6){x9md_str(b+o,n,str);str[n<259?n:259]=0;printf("X9 MODEM: EE str +%04x '%s'\n",o,str);ns++;}
  o+=n+1;
 }
}
static void x9md_send_ch(unsigned ch){
 uint32_t busy=x9md_rd(x9md_ap,X9MD_BUSY);
 if(busy&(1u<<ch)){printf("X9 MODEM: ch%u busy %08x\n",ch,busy);return;}
 x9md_wr(x9md_ap,X9MD_BUSY,1u<<ch);x9md_wr(x9md_ap,X9MD_TCHNUM,ch);
}

static int x9md_serve,x9md_clk_released;
extern void x9_baseband_start(void);
extern void x9_baseband_prepare(void);
#include "x9_modem_s3.inc"

/* Poll RCHNUM, drain rings, log every packet; HS2 -> READY, then keep
 * listening so stage 3 knows which ports the MD opens first (FSD, RPC...). */
static void x9md_listen(unsigned ms_total,int serve){
 uint64_t t0=mach_absolute_time();
 int ready=0,exp=0;unsigned pk=0,ready_ms=0;uint32_t hi_seen=0;
 static uint8_t pkt[X9MD_PKT_MAX];
 for(unsigned ms=0;ms<ms_total;ms+=2){
  uint32_t ch=x9md_rd(x9md_ap,X9MD_RCHNUM);
  if(ch)x9md_wr(x9md_ap,X9MD_ACK,ch);
  if(ch>>16){
   uint64_t ens;absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ens);
   printf("X9 MODEM: S2 exception RCHNUM %08x at %u ms\n",ch,(unsigned)(ens/1000000));
   x9md_tx_report("exception");
   hi_seen|=ch>>16;
   /* ccci_fsm_ee.c md_cd_exception */
   if(ch&(1u<<16)){x9md_ee_decode("EX_INIT");x9md_send_ch(16);printf("X9 MODEM: EE -> H2D_EXCEPTION_ACK\n");}
   if(ch&(1u<<18)){
    for(unsigned qi=0;qi<X9MD_QN;qi++){x9md_w32(x9md_qe[qi].ctl,0);x9md_w32(x9md_qe[qi].ctl+4,0);x9md_w32(x9md_qe[qi].ctl+12,0);x9md_w32(x9md_qe[qi].ctl+16,0);}
    exp=1;x9md_send_ch(17);printf("X9 MODEM: EE -> H2D_EXCEPTION_CLEARQ_ACK, switched to exception rings\n");
   }
   if(ch&(1u<<19)){x9md_status("ALLQ_RESET");x9md_ee_decode("ALLQ_RESET");}
  }
  for(unsigned qi=0;qi<X9MD_QN;qi++){
   for(int guard=0;guard<64;guard++){
    uint32_t len=0;struct x9md_q *qq=exp?&x9md_qe[qi]:&x9md_q[qi];
    int r=x9md_ring_read(qq,pkt,sizeof(pkt),&len);
    if(r==0)break;
    uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);
    if(r<0){printf("X9 MODEM: S2 %sq%u ring error %d rd=%x wr=%x\n",exp?"exp ":"",qi,r,x9md_m32(qq->ctl),x9md_m32(qq->ctl+4));
     x9md_w32(qq->ctl,x9md_m32(qq->ctl+4));break;}
    uint32_t *h=(uint32_t *)pkt;
    unsigned chn=h[2]&0xffff,seq=(h[2]>>16)&0x7fff;
    pk++;
    if(pk<=400&&(pk<=60||(chn!=14&&chn!=32))||(chn!=14&&chn!=32&&pk<=3000))
     printf("X9 MODEM: PKT %u ms %sq%u len %u ch %u seq %u d0=%08x d1=%08x rsv=%08x | %08x %08x %08x %08x\n",
      (unsigned)(ns/1000000),exp?"exp ":"",qi,len,chn,seq,h[0],h[1],h[3],
      len>16?h[4]:0,len>20?h[5]:0,len>24?h[6]:0,len>28?h[7]:0);
    if(serve&&!exp)x9md_s3_rx(qi,chn,pkt,len);
    if(!ready&&chn==0&&h[1]==0&&h[3]!=X9MD_CHK_ID){
     ready=1;x9md_assert_bit=0;ready_ms=ms;
     x9md_smc(X9MD_CCIF_CLK_RELEASE,0,0,NULL);printf("X9 MODEM: CCIF_CLK_RELEASE after READY\n");x9md_clk_released=1;
     x9_baseband_start();   /* iOS-side bridge (X9Baseband.cpp), boot-arg x9_bb=1 */
     printf("X9 MODEM: HS2 -> READY at %u ms (q%u, reserved=%08x)\n",(unsigned)(ns/1000000),qi,h[3]);
     x9md_status("READY");
     x9md_tx_report("READY");
     x9fs_usage("READY");
    }
   }
  }
  if(ready&&serve)x9md_ready_tick(ms-ready_ms);
  if(ms%5000==0&&ms){
   printf("X9 MODEM: S2 listen %u ms ready=%d packets=%u rch=%08x\n",ms,ready,pk,x9md_rd(x9md_ap,X9MD_RCHNUM));
   x9md_status("listen");
   x9md_tx_report("listen");
  }
  x9md_sleep_ms(2);
 }
 printf("X9 MODEM: S2 listen end ready=%d packets=%u exception_bits=%x\n",ready,pk,hi_seen);
 if(hi_seen)x9md_ee_decode("end");
 if(!ready){x9md_ccif_dump("no-hs2");x9md_sram_dump(x9md_ap,"AP",0x80);}
}

static void x9md_stage2(void){
 uint8_t md_ft[64];
 for(unsigned i=0;i<64;i+=4){
  uint32_t w=x9md_rd(x9md_ap,X9MD_CHDATA+0x14+i);
  md_ft[i]=(uint8_t)w;md_ft[i+1]=(uint8_t)(w>>8);md_ft[i+2]=(uint8_t)(w>>16);md_ft[i+3]=(uint8_t)(w>>24);
 }
 uint32_t tail=x9md_rd(x9md_ap,X9MD_CHDATA+0x54);
 if(tail!=X9MD_FEATURE_QUERY)printf("X9 MODEM: S2 MD query tail %08x (expected 49434343)\n",tail);
 int bad=x9md_prepare_runtime(md_ft);
 if(bad<0)return;
 x9md_send_runtime();
 x9md_listen(x9md_serve?240000:30000,x9md_serve);
}

static void x9md_thread(void *arg,wait_result_t wr) {
 (void)wr;
 int mode=(int)(uintptr_t)arg;
 printf("X9 MODEM: build=rev252hl-md fs128-bb-fwtrace-v1 mode=%d bb=%d\n",mode,sweet_x9_arg("x9_bb",0));
 printf("X9 MODEM: stage-1 bring-up mode=%d\n",mode);
 x9md_pmif_m=ml_io_map(0x1c01a000,0x1000);
 x9md_pmif_p=ml_io_map(0x1c018000,0x1000);
 x9md_serve=mode>=4;
 x9md_gpio=ml_io_map(0x1002d000,0x1000);
 x9md_ap=ml_io_map(X9MD_CCIF_AP_PA,0x1000);
 x9md_md=ml_io_map(X9MD_CCIF_MD_PA,0x1000);
 if(!x9md_ap||!x9md_md){printf("X9 MODEM: CCIF map failed\n");goto out;}
 x9md_dump_tags();
 x9md_status("probe");
 x9md_ccif_dump("probe");
 x9md_sram_dump(x9md_ap,"AP",0x40);
 if(mode>=3&&x9md_smem_init()==0&&x9md_rings_init()==0)printf("X9 MODEM: S2 smem + rings ready\n");
 else if(mode>=3){printf("X9 MODEM: S2 smem setup failed, staying at stage 1\n");mode=2;}
 if(mode>=2){
  if(x9md_serve)x9fs_preload();
  if(x9md_serve)x9md_sim_pinmux();
  if(x9md_power_on()==0&&x9md_wait_hs1()==0&&mode>=3)x9md_stage2();
  if(!x9md_clk_released)x9md_smc(X9MD_CCIF_CLK_RELEASE,0,0,NULL);
 }
out:
 printf("X9 MODEM: stage-1 done\n");
 thread_terminate(current_thread());
}

static void x9_modem_start(void) {
 static int started;
 int mode=sweet_x9_arg("x9_modem",0);
 if(started||mode<=0)return;
 started=1;
 if(mode>=4)x9_baseband_prepare();
 thread_t t;
 if(kernel_thread_start(x9md_thread,(void *)(uintptr_t)mode,&t)==KERN_SUCCESS)thread_deallocate(t);
}
