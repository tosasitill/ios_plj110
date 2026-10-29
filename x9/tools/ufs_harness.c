/* Host-only UFSHCI completion model. Not a PHY, link, SMMU or LK emulator. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include "ufs_ro.h"
#include "ufs_smmu.h"
static struct {
 uint32_t regs[256];uint64_t clock;int mode;unsigned writes,commands,forbidden;
 uint8_t dma[X9U_DMA_SIZE] __attribute__((aligned(16384)));
 uint8_t device_dma[X9U_DMA_SIZE];uint64_t sync_bytes;
 uint8_t disk[3][0x200000];size_t disk_size[3];uint64_t blocks[3];
 struct x9u_host host;unsigned init_flag,init_reads,init_stage,ready_calls,sense_calls;
} fake;
static uint32_t little(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t big(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void putbig(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static uint32_t read32(void *c,unsigned o){(void)c;return fake.regs[o/4];}
static void write32(void *c,unsigned o,uint32_t v)
{
 (void)c;fake.writes++;
 if(o==0x20){fake.regs[o/4]&=~v;return;}
 if(o==0x5c){if(fake.mode!=6)fake.regs[0x58/4]&=v;return;}
 if(o!=0x24&&o!=0x50&&o!=0x54&&o!=0x58&&o!=0x60){fake.forbidden++;return;}
 fake.regs[o/4]=v;if(o!=0x58||!v)return;
 fake.commands++;if(fake.mode==5||fake.mode==6)return;
 uint8_t *tr=fake.device_dma,*cmd=tr+0x1000,*response=tr+0x1200,*prdt=tr+0x1400,*data=tr+X9U_DATA_OFFSET;
 if(cmd[0]==0||cmd[0]==0x16) {
  if(little(tr)!=0x11000000||little(tr+24)!=0x00800080||little(tr+28)!=0x01000000||cmd[1]||cmd[2]){fake.forbidden++;return;}
  memset(response,0,512);response[0]=cmd[0]==0?0x20:0x36;response[3]=cmd[3];put(tr+8,0);
  if(cmd[0]==0) {if(fake.mode==7)response[0]=0x21;}
  else {
   if(cmd[13]!=1||cmd[14]||cmd[15]||(cmd[12]!=5&&cmd[12]!=6)||cmd[5]!=(cmd[12]==6?0x81:1)){fake.forbidden++;return;}
   response[12]=cmd[12];response[13]=1;
   if(cmd[12]==6){fake.init_flag=1;if(fake.mode==10)response[6]=1;}
   else {fake.init_reads++;if(fake.init_reads>=3&&fake.mode!=9)fake.init_flag=0;putbig(response+20,fake.init_flag);}
   if(fake.mode==8)response[13]=2;
  }
  fake.regs[0x58/4]=0;fake.regs[0x20/4]|=1;return;
 }
 unsigned lun=cmd[2];uint8_t *cdb=cmd+16;uint32_t bytes=big(cmd+12);
 unsigned iswrite=cdb[0]==0x2a||cdb[0]==0x8a;
 if((little(tr)&0x17000000)!=(bytes?(iswrite?0x13000000U:0x15000000U):0x11000000U)||cmd[1]!=(bytes?(iswrite?0x20:0x40):0)||cmd[0]!=1||lun>=3||
    little(tr+24)!=0x00800080||little(tr+28)!=(bytes?0x01000001U:0x01000000U)||
    little(prdt+12)!=(bytes?bytes-1:0)){fake.forbidden++;return;}
 if(cdb[0]!=0&&cdb[0]!=3&&cdb[0]!=0x12&&cdb[0]!=0x9e&&cdb[0]!=0x28&&cdb[0]!=0x88&&cdb[0]!=0x2a&&cdb[0]!=0x8a&&cdb[0]!=0x35){fake.forbidden++;return;}
 memset(response,0,512);response[0]=0x21;response[2]=lun;response[3]=cmd[3];put(tr+8,0);
 if(cdb[0]==0){
  fake.ready_calls++;
  if((fake.mode==11&&fake.ready_calls==1)||fake.mode==12||fake.mode==13||
     ((fake.mode==14||fake.mode==15||fake.mode==17||fake.mode==18||fake.mode==24)&&fake.ready_calls==1)||
     fake.mode==16||fake.mode==19||fake.mode==22||fake.mode==27||fake.mode==28)response[7]=2;
  if((fake.mode==14||fake.mode==15||fake.mode==19||fake.mode==22||fake.mode==24||fake.mode==27||fake.mode==28)&&response[7]==2) {
   unsigned length=fake.mode==15?8:18;response[11]=length+2;response[33]=length;
   uint8_t *sense=response+34;sense[0]=fake.mode==15?0x72:0x70;
   if(fake.mode==15){sense[1]=6;sense[2]=0x29;}
   else {sense[2]=fake.mode==19?2:(fake.mode==22||fake.mode==28)?5:6;sense[7]=10;sense[12]=fake.mode==19?4:fake.mode==22?0x20:0x29;}
  }
  if((fake.mode==24||fake.mode==27||fake.mode==28)&&response[7]==2)response[6]=1;
  if(fake.mode==24&&response[7]==2) {
   /* Exact observed sense prefix; unreported trailing bytes are zero fixture
    * bytes, not a reconstruction of the hardware ASC/ASCQ. */
   const uint8_t prefix[8]={0x70,0,6,0x40,0,5,0xa0,0x0a};
   memcpy(response+34,prefix,8);memset(response+42,0,10);
  }
  if(fake.mode==25)response[6]=1;
  if(fake.mode==26){response[6]=2;response[7]=2;}
  if(fake.mode==16){response[11]=20;response[32]=1;response[33]=255;}
  if(fake.mode==20)put(tr+8,1);
  if(fake.mode==21)response[3]^=1;
  if(fake.mode==23&&fake.ready_calls==1)response[7]=8;
 }
 else if(cdb[0]==3){
  fake.sense_calls++;memset(data,0,18);data[0]=0x70;
  data[2]=fake.mode==17?0:fake.mode==11?6:fake.mode==12?2:5;
  data[7]=10;data[12]=fake.mode==12?4:0;
  if(fake.mode==18){data[0]=0x72;data[1]=6;data[2]=0x29;response[1]=0x20;putbig(response+12,10);}
 }
 else if(cdb[0]==0x12){memset(data,0,36);memcpy(data+8,"SAMSUNG KLUFG4NHHB-F0G1   0001",28);}
 else if(cdb[0]==0x9e){uint64_t last=fake.blocks[lun]-1;putbig(data,(uint32_t)(last>>32));putbig(data+4,(uint32_t)last);putbig(data+8,4096);}
 else if(cdb[0]!=0x35) {
  uint64_t lba=(cdb[0]==0x28||cdb[0]==0x2a)?big(cdb+2):(uint64_t)big(cdb+2)<<32|big(cdb+6);
  uint32_t count=(cdb[0]==0x28||cdb[0]==0x2a)?(unsigned)cdb[7]<<8|cdb[8]:big(cdb+10);
  if((uint64_t)count*4096!=bytes||lba>UINT64_MAX/4096||lba*4096>fake.disk_size[lun]||
     bytes>fake.disk_size[lun]-lba*4096)response[7]=2;
  else if(iswrite)memcpy(fake.disk[lun]+lba*4096,data,bytes);
  else memcpy(data,fake.disk[lun]+lba*4096,bytes);
 }
 if(fake.mode==29)response[6]=1;
 if(fake.mode==1)response[3]^=1;
 if(fake.mode==2)put(tr+8,1);
 if(fake.mode==3)response[7]=2;
 if(fake.mode==4){response[1]=0x20;putbig(response+12,1);}
 fake.regs[0x58/4]=0;fake.regs[0x20/4]|=1;
}
static void syncbuf(void *c,unsigned o,size_t n,int to_cpu)
{
 (void)c;assert(o<=X9U_DMA_SIZE && n<=X9U_DMA_SIZE-o);fake.sync_bytes+=n;
 if(to_cpu)memcpy(fake.dma+o,fake.device_dma+o,n);
 else memcpy(fake.device_dma+o,fake.dma+o,n);
}
uint64_t test_sync_bytes(void){return fake.sync_bytes;}
static uint64_t now(void *c){(void)c;return fake.clock;}
static void delay(void *c,unsigned us){(void)c;fake.clock+=us;}
void test_reset(void)
{
 memset(&fake,0,sizeof(fake));fake.regs[0]=1U<<24;fake.regs[2]=0x300;
 fake.regs[0x24/4]=0x777;fake.regs[0x30/4]=3;fake.regs[0x34/4]=1;
 fake.regs[0x50/4]=0x90000000;fake.regs[0x60/4]=1;
 for(unsigned i=0;i<3;i++)fake.blocks[i]=512;
}
void test_mode(int n){fake.mode=n;}
void test_reg(unsigned o,uint32_t v){fake.regs[o/4]=v;}
uint32_t test_get(unsigned o){return fake.regs[o/4];}
unsigned test_writes(void){return fake.writes;}
unsigned test_commands(void){return fake.commands;}
unsigned test_forbidden(void){return fake.forbidden;}
int test_open(int verified)
{struct x9u_io io={0,read32,write32,syncbuf,now,delay};return x9u_open(&fake.host,&io,fake.dma,0x140100000ULL,verified);}
int test_close(void){return x9u_close(&fake.host);}
int test_capacity(unsigned lun){return x9u_capacity(&fake.host,lun);}
int test_read(unsigned lun,uint64_t lba,uint32_t count,void *out){return x9u_read(&fake.host,lun,lba,count,out);}
int test_gpt(unsigned lun,const char *name,struct x9u_partition *out){return x9u_gpt_find(&fake.host,lun,name,out);}
void test_disk(unsigned lun,const void *data,size_t n,uint64_t blocks)
{if(lun<3&&n<=sizeof(fake.disk[0])){memcpy(fake.disk[lun],data,n);fake.disk_size[lun]=n;fake.blocks[lun]=blocks;}}
int test_image(unsigned lun,const struct x9u_partition *p,const struct x9u_extent *map,size_t n,uint64_t blocks,uint64_t offset,void *out,size_t length)
{return x9u_image_read(&fake.host,lun,p,map,n,blocks,offset,out,length);}
int test_cdb(void){return fake.dma[0x1010];}

#include "ufs_root.h"
#include "sha256.h"
int test_root(unsigned lun,const struct x9u_partition *p,const struct x9u_root_manifest *m,void *out,size_t capacity)
{return x9u_root_read(&fake.host,lun,p,m,out,capacity,0);}
void test_sha256(const void *data,size_t size,uint8_t out[32])
{struct sha256 hash;sha256_init(&hash);sha256_update(&hash,data,size);sha256_final(&hash,out);}

int test_device_init(void){return x9u_device_init(&fake.host,&fake.init_stage);}
unsigned test_init_stage(void){return fake.init_stage;}
unsigned test_init_reads(void){return fake.init_reads;}

int test_ready(unsigned lun){return x9u_lun_ready(&fake.host,lun);}

unsigned test_sense_calls(void){return fake.sense_calls;}
unsigned test_sense_valid(void){return fake.host.sense_valid;}
unsigned test_sense_category(void){return fake.host.sense_key;}
unsigned test_failure_phase(void){return fake.host.tur_failure_phase;}

unsigned test_unit_attentions(void){return fake.host.ready_unit_attentions;}

int test_image_mapped(unsigned lun,const struct x9u_partition *p,const struct x9u_extent *map,size_t n,uint64_t blocks,uint64_t offset,void *out,size_t length)
{return x9u_image_read_mapped(&fake.host,lun,p,map,n,blocks,offset,out,length);}

#if defined(X9U_EXTENT_WRITES)
int test_bind_write(unsigned lun,const struct x9u_partition *p,const struct x9u_extent *m,size_t n,uint64_t blocks)
{return x9u_bind_extent_writes(&fake.host,lun,p,m,n,blocks);}
int test_write_image(uint64_t offset,const void *src,size_t n){return x9u_image_write(&fake.host,offset,src,n);}
int test_sync_image(void){return x9u_sync_image(&fake.host);}
#endif
void test_disk_copy(unsigned lun,void *out,size_t size)
{assert(lun<3&&size<=sizeof(fake.disk[lun]));memcpy(out,fake.disk[lun],size);}
