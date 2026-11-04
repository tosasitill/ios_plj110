/* X9 S3910 LBP bringup. Sources: OPPO 513bc9adab35f1e7e38b413a199f4f59e5b93144
 * drivers/spi/spi-mt65xx.c (mt6899_compat), tp/hbp/chips/synaptics/s3910.
 * Captured DT: spi6 0x161d0000, pad1, tickdly19, mode0, max19MHz.
 * Initializes SPI6 infra, clock gates, 26MHz parent, timing and pinmux.
 * Releases captured GPIO3 touch reset once and waits vendor 100ms.
 * MT6661 touch rails configured via captured PMIF-P; existing app firmware
 * selected without flash writes. Bounded startup; no hardware reset loops.
 * 16 KiB independent, FIFO only; existing iOS16 IOHIDEvent ABI retained. */
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *name, int def);
extern void sweet_hid_touch(int,int,int,int);
extern kern_return_t thread_terminate(thread_t);
static volatile uint32_t *tp;
static uint32_t command;
static vm_offset_t touch_gpio, touch_iocfg;
static unsigned downmask;
static int lastx[10],lasty[10];
static unsigned le16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void wr(unsigned off,uint32_t value){tp[off/4]=value;__asm__ volatile("dsb sy":::"memory");}
static uint32_t rd(unsigned off){return tp[off/4];}
/* Exact MT6993 DT/source registers; only SPI6 fields are changed. */
static uint32_t mmrd(vm_offset_t base,unsigned off){return *(volatile uint32_t *)(base+off);}
static void mmwr(vm_offset_t base,unsigned off,uint32_t value)
{*(volatile uint32_t *)(base+off)=value;__asm__ volatile("dsb sy":::"memory");}
static int bit_wait(vm_offset_t base,unsigned off,uint32_t mask,int set)
{
 for(unsigned i=0;i<1000;i++){if(!!(mmrd(base,off)&mask)==set)return 0;delay(1);}
 return -1;
}
static int spi_prepare(void)
{
 vm_offset_t top=ml_io_map(0x1c210000,0x1000);
 vm_offset_t peri=ml_io_map(0x16640000,0x1000);
 vm_offset_t gpio=ml_io_map(0x1002d000,0x1000);
 touch_gpio=gpio;touch_iocfg=ml_io_map(0x12820000,0x1000);
 if(!top||!peri||!gpio||!touch_iocfg){printf("X9 TOUCH: platform map failed\n");return -1;}
 /* spi_req_infra(bus6), MTK_SIP_KERNEL_SPI_CONTROL, OP_REQ_INFRA=0. */
 register uint64_t x0 __asm__("x0")=0xc2000542;
 register uint64_t x1 __asm__("x1")=0;
 register uint64_t x2 __asm__("x2")=6;
 register uint64_t x3 __asm__("x3")=0;
 register uint64_t x4 __asm__("x4")=0;
 register uint64_t x5 __asm__("x5")=0;
 register uint64_t x6 __asm__("x6")=0;
 register uint64_t x7 __asm__("x7")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::"memory");
 printf("X9 TOUCH: SPI6 infra request result=%lld\n",(long long)x0);
 if(x0)return -1;
 printf("X9 TOUCH: clocks before top=%08x peri=%08x reset=%08x pins=%08x\n",
   mmrd(top,0xd0),mmrd(peri,0x14),mmrd(peri,0),mmrd(gpio,0x440));
 /* CLK_CFG_12 SPI6 select[18:16]=0: tck_26m, UPDATE1 bit19. */
 mmwr(top,0xd8,7u<<16);mmwr(top,8,1u<<19);
 if(bit_wait(top,0x334,1u<<12,0)){printf("X9 TOUCH: SPI6 parent switch busy\n");return -1;}
 mmwr(top,0xd8,1u<<23);
 if(bit_wait(top,0x274,1u<<13,1)){printf("X9 TOUCH: SPI6 top clock fence timeout\n");return -1;}
 /* clk-mt6993-peri GATE_HWV_PERAO1 DMA/direct setclr path. */
 mmwr(peri,0x30,1u<<7);
 if(bit_wait(peri,0x14,1u<<7,0)){printf("X9 TOUCH: SPI6 peripheral clock gated\n");return -1;}
 /* Deassert only the SPI6 peripheral reset, captured reset-bit=13. */
 mmwr(peri,8,1u<<13);
 /* Captured spi_active pinmux GPIO80..83, mode2; preserve reserved bits. */
 mmwr(gpio,0x440,(mmrd(gpio,0x440)&~0x1f1f1f1fu)|0x02020202u);
 /* pinctrl bias-disable for GPIO80..83: PU/PD fields in IOCFG_LB0. */
 mmwr(touch_iocfg,0xb0,mmrd(touch_iocfg,0xb0)&~(0xfu<<9));
 mmwr(touch_iocfg,0x90,mmrd(touch_iocfg,0x90)&~(0xfu<<9));
 /* SPI6 MI GPIO83 IES bit17; IRQ GPIO4 IES bit3. IRQ sampled, not routed. */
 mmwr(touch_iocfg,0x60,mmrd(touch_iocfg,0x60)|(1u<<17)|(1u<<3));
 mmwr(gpio,0x310,mmrd(gpio,0x310)&~31u);mmwr(gpio,8,1u<<4);
 uint32_t topcfg=mmrd(top,0xd0),pins=mmrd(gpio,0x440);
 if((topcfg&((7u<<16)|(1u<<23)))||(pins&0x1f1f1f1fu)!=0x02020202u){
  printf("X9 TOUCH: clock/pin readback rejected top=%08x pins=%08x\n",topcfg,pins);return -1;
 }
 /* mtk_spi_reset: peripheral software reset, not a touch IC reset. */
 wr(0x18,1u<<2);wr(0x18,0);
 /* 26MHz / (32+32) = 406250Hz. setup/hold=64 ticks, idle=64. */
 wr(0,0x003f003f);wr(4,63);wr(0x28,0x001f001f);
 wr(0x40,0);wr(0x68,0);wr(0x24,1);
 wr(0x5c,(rd(0x5c)&~0x3fu)|0x12u); /* pad1 RX/TX slice */
 printf("X9 TOUCH: SPI6 ready cfg0=%08x cfg2=%08x slice=%08x pins=%08x reset=%08x clock=406250Hz\n",
   rd(0),rd(0x28),rd(0x5c),pins,mmrd(peri,0));
 if(rd(0)!=0x003f003f||rd(0x28)!=0x001f001f||(rd(0x5c)&0x3f)!=0x12||
   !(mmrd(touch_iocfg,0x60)&(1u<<17)))return -1;
 return 0;
}
/* MT6993 spmi-mtk-pmif-core.c: soc_chan=2, SWINF2 offsets 0x880.
 * Captured spmi-dev-mask=0x55a8 routes SID3 and SID5 to PMIF-P. */
static vm_offset_t touch_pmif;
static unsigned touch_max_write=515; /* local bound; stock default is 1024 */
static int pmif_wait(unsigned state)
{
 for(unsigned i=0;i<10000;i++){
  uint32_t s=mmrd(touch_pmif,0x8a8);
  if(((s>>1)&7)==state)return (s&(1u<<18))?-1:0;
  delay(1);
 }
 printf("X9 TOUCH: PMIF-P timeout state=%u sta=%08x\n",state,mmrd(touch_pmif,0x8a8));return -1;
}
static int pmic_read_unlocked(unsigned sid,unsigned addr,uint8_t *value)
{
 if((sid!=3&&sid!=5)||addr>0xffff||pmif_wait(0))return -1;
 mmwr(touch_pmif,0x880,(3u<<30)|(sid<<24)|addr);
 if(pmif_wait(6))return -1;
 *value=(uint8_t)mmrd(touch_pmif,0x894);mmwr(touch_pmif,0x8a4,1);
 return pmif_wait(0);
}
/* Allow only the two touch voltage fields and enable SET registers. */
static int pmic_write_unlocked(unsigned sid,unsigned addr,uint8_t value)
{
 if(!((sid==3&&(addr==0x1e60||addr==0x1ec8))||
      (sid==5&&(addr==0x24a||addr==0x2048)))||pmif_wait(0))return -1;
 mmwr(touch_pmif,0x884,value);
 mmwr(touch_pmif,0x880,(3u<<30)|(1u<<29)|(sid<<24)|addr);
 return pmif_wait(0);
}
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
static int pmic_read(unsigned sid,unsigned addr,uint8_t *value) {
 if(X9PMIFAcquire())return -1;
 int rc=pmic_read_unlocked(sid,addr,value);X9PMIFRelease();return rc;
}
static int pmic_write(unsigned sid,unsigned addr,uint8_t value) {
 if(X9PMIFAcquire())return -1;
 int rc=pmic_write_unlocked(sid,addr,value);X9PMIFRelease();return rc;
}
static int touch_rail(unsigned sid,unsigned vaddr,uint8_t mask,uint8_t target,
 unsigned enable,unsigned da,const char *name)
{
 uint8_t id=0,v=0,en=0,state=0;
 if(pmic_read(sid,8,&id)||pmic_read(sid,vaddr,&v)||pmic_read(sid,enable,&en)||pmic_read(sid,da,&state))return -1;
 printf("X9 TOUCH: rail %s SID%u hwcid=%02x voltage=%02x enable=%02x state=%02x\n",name,sid,id,v,en,state);
 if(id!=0x10&&id!=0x20){printf("X9 TOUCH: unknown MT6661 revision; rail stopped\n");return -1;}
 uint8_t next=(uint8_t)((v&~mask)|target);
 if(next!=v&&pmic_write(sid,vaddr,next))return -1;
 if(!(en&1)&&pmic_write(sid,enable+1,1))return -1;
 delay(10000);
 if(pmic_read(sid,vaddr,&v)||pmic_read(sid,enable,&en)||pmic_read(sid,da,&state))return -1;
 printf("X9 TOUCH: rail %s readback voltage=%02x enable=%02x state=%02x\n",name,v,en,state);
 return ((v&mask)==target&&(en&1)&&(state&1))?0:-1;
}
static int touch_power(void)
{
 touch_pmif=ml_io_map(0x1c018000,0x2000);
 if(!touch_pmif)return -1;
 printf("X9 TOUCH: PMIF-P inherited init=%08x inf=%08x arb=%08x chan2=%08x\n",
  mmrd(touch_pmif,0),mmrd(touch_pmif,0x24),mmrd(touch_pmif,0x150),mmrd(touch_pmif,0x8a8));
 if(!(mmrd(touch_pmif,0)&1)){printf("X9 TOUCH: PMIF-P not initialized by platform\n");return -1;}
 /* hbp power_on_default: BUS10ms -> AVDD10ms -> VDDI10ms -> RESET100ms.
  * AVDD table selector10=3.0V. LN60 SN-LDO: 0.5V+13*0.1V, vocal0. */
 delay(10000);
 if(touch_rail(3,0x1e60,0x0f,10,0x1ec7,0x1ed4,"AVDD3V"))return -1;
 if(touch_rail(5,0x24a,0xff,0xd0,0x2047,0x2062,"VDDI1V8"))return -1;
 return 0;
}
/* Captured reset1_active: GPIO3 mode0/output-high. hbp_power.h waits
 * 100ms after POWER_RESET=true; no low pulse or repeated reset here. */
static int touch_reset_release(void)
{
 uint32_t mode=mmrd(touch_gpio,0x300),dir=mmrd(touch_gpio,0),
   dout=mmrd(touch_gpio,0x100),din=mmrd(touch_gpio,0x200);
 printf("X9 TOUCH: reset before mode=%u dir=%u out=%u in=%u irq=%u spi_in=%08x ies=%08x\n",
   (mode>>24)&31,(dir>>3)&1,(dout>>3)&1,(din>>3)&1,(din>>4)&1,
   mmrd(touch_gpio,0x220),mmrd(touch_iocfg,0x60));
 /* Set the latch high before selecting GPIO output, avoid a low glitch. */
 mmwr(touch_gpio,0x104,1u<<3);
 mmwr(touch_gpio,0x300,mode&~(31u<<24));
 mmwr(touch_gpio,4,1u<<3);
 delay(100000);
 mode=mmrd(touch_gpio,0x300);dir=mmrd(touch_gpio,0);
 dout=mmrd(touch_gpio,0x100);din=mmrd(touch_gpio,0x200);
 printf("X9 TOUCH: reset released mode=%u dir=%u out=%u in=%u irq=%u waited=100ms\n",
   (mode>>24)&31,(dir>>3)&1,(dout>>3)&1,(din>>3)&1,(din>>4)&1);
 if(((mode>>24)&31)||!(dir&(1u<<3))||!(dout&(1u<<3))){
  printf("X9 TOUCH: reset output readback rejected\n");return -1;
 }
 return 0;
}
/* Vendor sw_cs: CS_POL=1 asserts low, CS_PIN_SEL selects software CS. */
static int transfer(const uint8_t *tx,uint8_t *rx,unsigned length)
{
 if(!length||length>519)return -1;
 wr(0x18,command|(1u<<7));
 for(unsigned off=0;off<length;){
  unsigned n=length-off;if(n>32)n=32;
  wr(0x04,(rd(0x04)&0xffu)|((n-1)<<16));wr(0x68,0);
  for(unsigned i=0;i<n;i+=4){
   uint32_t v=0;for(unsigned j=0;j<4&&i+j<n;j++)v|=(uint32_t)(tx?tx[off+i+j]:0xff)<<(j*8);
   wr(0x10,v);
  }
  wr(0x18,command|(1u<<7)|1u);
  unsigned wait;for(wait=0;wait<5000&&!rd(0x20);wait++)delay(1);
  if(wait==5000){wr(0x18,command);printf("X9 TOUCH: SPI FIFO timeout status=%08x cmd=%08x\n",rd(0x1c),rd(0x18));return -1;}
  uint32_t status=rd(0x1c);
  if(status&2u){wr(0x18,command);printf("X9 TOUCH: unexpected FIFO pause status=%08x offset=%u\n",status,off);return -1;}
  for(unsigned i=0;i<n;i+=4){uint32_t v=rd(0x14);for(unsigned j=0;j<4&&i+j<n;j++)if(rx)rx[off+i+j]=(uint8_t)(v>>(j*8));}
  off+=n;
 }
 wr(0x18,command);return 0;
}
static int tcm_crc_enabled,tcm_rc_enabled,tcm_tail_known;
static unsigned tcm_messages;
static uint16_t tcm_crc16(const uint8_t *data,unsigned length)
{
 uint16_t crc=0xffff;
 for(unsigned i=0;i<length;i++){
  crc^=(uint16_t)((unsigned)data[i]<<8);
  for(unsigned b=0;b<8;b++)crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));
 }
 return crc;
}
/* OPPO detect starts with CRC+RC+extra EOM enabled, then tests the
 * startup trailer for the no-extension 5A/5A/5A pattern. */
static int message(uint8_t *code,uint8_t *body,unsigned *size)
{
 uint8_t h[4],part[519],tail[5]={0};
 *size=0;
 if(transfer(NULL,h,4))return -1;
 if(++tcm_messages<=16)printf("X9 TOUCH: TCM header=%02x %02x %02x %02x\n",h[0],h[1],h[2],h[3]);
 if(h[0]!=0xa5){printf("X9 TOUCH: invalid TCMv1 marker=%02x code=%02x len=%u\n",h[0],h[1],le16(h+2));return -1;}
 unsigned len=le16(h+2);*code=h[1];
 if(*code==0||*code==2||*code==4)return 0;
 if(len>512){printf("X9 TOUCH: message too large code=%02x len=%u\n",*code,len);return -1;}
 *size=len;
 if(!len)return 0;
 unsigned extra=!tcm_tail_known?5u:1u+(tcm_crc_enabled?2u:0u)+(tcm_rc_enabled?1u:0u)+((tcm_crc_enabled||tcm_rc_enabled)?1u:0u);
 /* Stock RD_CHUNK_SIZE=0: one logical continued read. The FIFO helper
  * keeps CS asserted across its internal 32-byte hardware packets. A FIFO
  * boundary is not a new TCM transaction and needs no repeated A5/03. */
 unsigned remaining=len+extra;
 if(transfer(NULL,part,remaining+2))return -1;
 if(part[0]!=0xa5||part[1]!=3){
  printf("X9 TOUCH: continued header=%02x %02x payload=%u tail=%u\n",part[0],part[1],len,extra);return -1;
 }
 memcpy(body,part+2,len);memcpy(tail,part+2+len,extra);
 if(tail[0]!=0x5a){printf("X9 TOUCH: invalid EOM=%02x code=%02x len=%u\n",tail[0],*code,len);return -1;}
 if(!tcm_tail_known){
  tcm_crc_enabled=le16(tail+1)!=0x5a5a;
  tcm_rc_enabled=tail[3]!=0x5a;tcm_tail_known=1;
  printf("X9 TOUCH: startup payload=%u tail=%02x %02x %02x %02x %02x CRC=%u RC=%u\n",
   len,tail[0],tail[1],tail[2],tail[3],tail[4],tcm_crc_enabled,tcm_rc_enabled);
 }
 return 0;
}
static int request(uint8_t cmd,const uint8_t *body,unsigned n,uint8_t *response,unsigned *size)
{
 uint8_t out[515],code;
 *size=0;
 unsigned crc_bytes=tcm_crc_enabled?2u:0u;
 if(n>512||(n&&!body)||touch_max_write<3+crc_bytes){
  printf("X9 TOUCH: command=%02x invalid write bound=%u payload=%u CRC=%u\n",cmd,touch_max_write,n,crc_bytes);return -1;
 }
 /* CRC belongs to the logical command and is appended intact to the last
  * transaction, never split as if it were command payload. */
 uint8_t logical[515];logical[0]=cmd;logical[1]=(uint8_t)n;logical[2]=(uint8_t)(n>>8);
 if(n)memcpy(logical+3,body,n);
 uint16_t crc=tcm_crc16(logical,n+3);
 unsigned sent=0;int first=1;
 do {
  unsigned header=first?3u:1u;
  unsigned room=touch_max_write-header-crc_bytes;
  unsigned take=n-sent;if(take>room)take=room;
  if(n>sent&&!take)return -1;
  out[0]=first?cmd:1;
  if(first){out[1]=(uint8_t)n;out[2]=(uint8_t)(n>>8);}
  if(take)memcpy(out+header,body+sent,take);
  unsigned length=header+take;sent+=take;
  if(sent==n&&tcm_crc_enabled){out[length++]=(uint8_t)crc;out[length++]=(uint8_t)(crc>>8);}
  if(transfer(out,NULL,length))return -1;
  first=0;
 }while(sent<n);
 for(unsigned i=0;i<250;i++){
  delay(2000);
  if(message(&code,response,size))return -1;
  if(code==1){
   if(cmd==2&&*size<24){printf("X9 TOUCH: IDENTIFY empty/short OK len=%u; awaiting identity\n",*size);continue;}
   return 0;
  }
  if(code==0x10){
   if((cmd==2||cmd==0x14)&&*size>=24)return 0;
   printf("X9 TOUCH: unexpected identify/reset during command=%02x payload=%u\n",cmd,*size);return -1;
  }
  if(code==0||code==2||code==4||code==7)continue;
  if(code<0x10||code>=0xfe){printf("X9 TOUCH: command=%02x rejected code=%02x len=%u\n",cmd,code,*size);return -1;}
 }
 printf("X9 TOUCH: command=%02x response timeout\n",cmd);return -1;
}
static int accept_identity(uint8_t *response,unsigned n)
{
 if(n<24){printf("X9 TOUCH: short identity payload=%u\n",n);return -1;}
 printf("X9 TOUCH: identify version=%u mode=%u part=%.16s write=%u\n",
  response[0],response[1],response+2,le16(response+22));
 unsigned limit=le16(response+22);if(!limit){printf("X9 TOUCH: zero max_write; keeping bounded default=%u\n",touch_max_write);return 0;}if(limit<3){printf("X9 TOUCH: invalid identity max_write=%u\n",limit);return -1;}
 touch_max_write=limit>515?515:limit;
 return 0;
}
static int identify(uint8_t *response,unsigned *n)
{
 int error=request(2,NULL,0,response,n);
 if(error){printf("X9 TOUCH: identify request error=%d payload=%u\n",error,*n);return -1;}
 return accept_identity(response,*n);
}
static int startup_identity(uint8_t *response,unsigned *n)
{
 uint8_t magic=7,code;
 if(transfer(&magic,NULL,1)){printf("X9 TOUCH: detection magic write failed\n");return -1;}
 if(message(&code,response,n))return -1;
 printf("X9 TOUCH: startup code=%02x payload=%u\n",code,*n);
 if(code==0x10)return accept_identity(response,*n);
 return identify(response,n);
}

static void release_all(void){for(int i=0;i<10;i++)if(downmask&(1u<<i))sweet_hid_touch(i,0,lastx[i],lasty[i]);downmask=0;}
static void touch_run(void)
{
 uint8_t response[512],code;unsigned n;
 tp=(volatile uint32_t *)ml_io_map(0x161d0000,0x1000);
 if(!tp)return;
 uint32_t timing=rd(0x28),cfg=rd(0),cmd=rd(0x18);
 printf("X9 TOUCH: SPI6 inherited cfg0=%08x cfg2=%08x cmd=%08x pad=%u slice=%08x\n",cfg,timing,cmd,rd(0x24),rd(0x5c));
 if(spi_prepare()){printf("X9 TOUCH: SPI6 platform initialization stopped\n");return;}
 cmd=rd(0x18);
 command=(cmd&~((3u<<8)|(3u<<14)|(3u<<16)|(1u<<21)|0x3fu))|
   (1u<<12)|(1u<<13)|(1u<<18)|(1u<<19);
 command=(command&~(0x7fu<<22))|(19u<<22);
 command&=~(1u<<7);
 wr(0x24,1); /* captured pad */
 /* SPI6 clocks/timing/slice have been configured and read back above. */
 wr(0x18,command);
 if(touch_power()){printf("X9 TOUCH: power chain failed; stopped before identify\n");return;}
 if(touch_reset_release())return;
 if(startup_identity(response,&n)){printf("X9 TOUCH: identify failed after complete power/reset/SPI setup\n");return;}
 if(response[1]!=1){
  printf("X9 TOUCH: switching existing firmware to application mode\n");
  if(request(0x14,NULL,0,response,&n)||identify(response,&n)||response[1]!=1){
   printf("X9 TOUCH: no runnable application firmware; mode switch failed, no flash write\n");return;
  }
 }
 if(request(0x20,NULL,0,response,&n))return;
 if(n<38){printf("X9 TOUCH: short application info=%u\n",n);return;}
 printf("X9 TOUCH: application status=%04x max_payload=%u\n",le16(response+2),le16(response+14));
 if(le16(response+2)){printf("X9 TOUCH: application not ready; stopped\n");return;}
 unsigned maxpayload=le16(response+14);
 unsigned maxcfg=le16(response+12),maxx=le16(response+32),maxy=le16(response+34),objects=le16(response+36);
 if(!objects||objects>10||!maxx||!maxy||maxcfg<9||maxcfg>512||maxpayload<objects*5){printf("X9 TOUCH: unsupported geometry/config\n");return;}
 printf("X9 TOUCH: application max=%ux%u objects=%u config=%u\n",maxx,maxy,objects,maxcfg);
 /* Disable touch report while changing its layout, leave deep sleep. */
 uint8_t report=0x11;
 if(request(6,&report,1,response,&n)||request(0x2d,NULL,0,response,&n)){
  printf("X9 TOUCH: report quiesce/wake failed\n");return;
 }
 uint8_t mode[]={0x11,2,0};
 if(request(0x24,mode,3,response,&n)){printf("X9 TOUCH: LBP mode rejected\n");return;}
 uint8_t mode_key=0x11;
 if(request(0x23,&mode_key,1,response,&n)||n<2||le16(response)!=2){printf("X9 TOUCH: LBP mode readback rejected\n");return;}
 /* Fixed all-object report: status8, X16, Y16 per slot; no host algorithm. */
 uint8_t config[512]={2,7,8,8,16,9,16,3,0};
 if(request(0x26,config,maxcfg,response,&n)){printf("X9 TOUCH: report config rejected\n");return;}
 if(request(0x25,NULL,0,response,&n)||n<9||memcmp(response,config,9)){
  printf("X9 TOUCH: report configuration readback mismatch\n");return;
 }
 if(request(0x27,NULL,0,response,&n)){printf("X9 TOUCH: rezero failed\n");return;}
 if(request(5,&report,1,response,&n)){printf("X9 TOUCH: enable report rejected\n");return;}
 printf("X9 TOUCH: LBP reporting enabled; waiting for actual contacts\n");
 unsigned reports=0,stream_errors=0;uint64_t heartbeat=0;clock_interval_to_deadline(10,NSEC_PER_SEC,&heartbeat);
 for(;;){
  if(message(&code,response,&n)){
   /* A failed continued read invalidates the whole frame. Release contacts
    * and retry from a fresh TCM header, never reuse a partial payload.
    * Only a fully validated touch report replenishes the retry budget;
    * idle replies alone must not hide a persistently broken stream. */
   release_all();
   if(++stream_errors>3){printf("X9 TOUCH: stream failed; recovery budget exhausted; stopped\n");return;}
   printf("X9 TOUCH: stream failed; contacts released; retry=%u/3\n",stream_errors);
   uint64_t retry_deadline;clock_interval_to_deadline(20,NSEC_PER_MSEC,&retry_deadline);
   (void)assert_wait_deadline(&downmask,THREAD_UNINT,retry_deadline);thread_block(THREAD_CONTINUE_NULL);
   continue;
  }
  if(code==0x11){
   if(n!=objects*5){release_all();printf("X9 TOUCH: invalid touch payload len=%u expected=%u\n",n,objects*5);return;}
   if(stream_errors){printf("X9 TOUCH: validated touch stream resumed after %u errors\n",stream_errors);stream_errors=0;}
   unsigned mask=0;
   for(unsigned i=0;i<objects;i++){
    unsigned status=response[i*5],x=le16(response+i*5+1),y=le16(response+i*5+3);
    if((status==1||status==2)&&x<=maxx&&y<=maxy){
     mask|=1u<<i;lastx[i]=(int)((uint64_t)x*1255/maxx);lasty[i]=(int)((uint64_t)y*2759/maxy);
     sweet_hid_touch((int)i,1,lastx[i],lasty[i]);
    }else if(downmask&(1u<<i))sweet_hid_touch((int)i,0,lastx[i],lasty[i]);
   }
   reports++;
   if(mask!=downmask||reports<=3)printf("X9 TOUCH: report=%u contacts=%03x previous=%03x HID dispatched\n",reports,mask,downmask);
   downmask=mask;
  }else if(code==0x10){release_all();printf("X9 TOUCH: controller reset; stopped\n");return;}
  else if((code>=0x08&&code<0x10)||code>=0xfe||code==3){
   release_all();printf("X9 TOUCH: stream error code=%02x len=%u; contacts released\n",code,n);return;
  }
  if(mach_absolute_time()>=heartbeat){
   printf("X9 TOUCH: alive reports=%u contacts=%03x irq=%u\n",reports,downmask,(mmrd(touch_gpio,0x200)>>4)&1);
   clock_interval_to_deadline(10,NSEC_PER_SEC,&heartbeat);
  }
  uint64_t deadline;clock_interval_to_deadline(8,NSEC_PER_MSEC,&deadline);
  (void)assert_wait_deadline(&downmask,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
 }
}
static void touch_thread(void *arg,wait_result_t result)
{
 (void)arg;(void)result;touch_run();
 (void)thread_terminate(current_thread());
}
extern void sweet_x9_keys_init(void);
extern void sweet_x9_gpu_dependencies(void);
void sweet_x9_touch_init(void)
{
 sweet_x9_keys_init();
 sweet_x9_gpu_dependencies();
 if(!sweet_early_is_x9())return;
 int enabled=0;if((enabled=sweet_x9_arg("x9_touch_lbp",1))!=1)return;
 thread_t thread;printf("X9 TOUCH: S3910 pan138: bounded TCM transactions, power/reset, LBP/HID\n");
 if(kernel_thread_start(touch_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 else printf("X9 TOUCH: worker creation failed\n");
}
