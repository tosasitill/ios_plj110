/* Fake MMIO state machine: HCI/UIC register contract only, not PHY hardware. */
#include <stdint.h>
#include <string.h>
#include "ufs_link.h"
static struct {uint32_t r[0x2400/4],events[256][2];unsigned n;uint64_t clock;unsigned mode;uint32_t powerdown,lcc,savepower,debugclock,debugomc;unsigned reset_calls,link_startups;struct x9u_link link;} model;
static uint32_t read32(void *ctx,unsigned off){(void)ctx;return model.r[off/4];}
static void write32(void *ctx,unsigned off,uint32_t value)
{
 (void)ctx;
 if(model.n<256){model.events[model.n][0]=off;model.events[model.n][1]=value;model.n++;}
 if(off==0x20){model.r[off/4]&=~value;return;}
 model.r[off/4]=value;
 if(off==0x2308) {model.r[off/4]=(value&~2U)|((value&1)&&model.mode!=14?2:0);}
 if(off==0x34) {
  if(value&1) {
   if(model.mode==1)model.r[off/4]=0;
   else model.r[0x30/4]=model.mode==2?0:model.mode==7?0xb:8;
  }else model.r[0x30/4]=0;
 }
 if(off==0x90) {
  if(model.mode==3)return;
  model.r[0x20/4]|=1U<<10;
  unsigned attr=model.r[0x94/4]>>16;
  model.r[0x98/4]=0;
  if(value==1||value==2) {
   uint32_t *setting=attr==0xd0a8?&model.powerdown:attr==0x1585?&model.lcc:attr==0xd0a6?&model.savepower:attr==0xd0a1?&model.debugclock:attr==0xd09e?&model.debugomc:0;
   if(!setting||(model.mode==8&&attr==0xd0a8)||(model.mode==9&&attr==0xd0a6&&value==1)) {
    model.r[0x98/4]=1;return;
   }
   if(value==1)model.r[0x9c/4]=*setting;
   else *setting=model.r[0x9c/4];
   return;
  }
  if(value!=0x16){model.r[0x98/4]=99;return;}
  model.link_startups++;
  if(model.mode==18&&model.link_startups>1){model.r[0x30/4]=0xf;model.r[0x98/4]=1;model.r[0x38/4]=0x80000010;return;}
  unsigned blocked=(model.mode==10||model.mode==13)&&(model.powerdown||model.lcc||(model.savepower&0x40));
  if(model.mode==13&&model.reset_calls<2)blocked=1;
  if(model.mode==17){model.r[0x38/4]=0x80000010;model.r[0x98/4]=1;return;}
  if(model.mode==15) {model.r[0x38/4]=0x80000010;model.r[0x98/4]=1;model.mode=13;return;}
  model.r[0x98/4]=(model.mode==4||blocked)?1:0;
  if(model.mode!=4&&model.mode!=5&&!blocked)model.r[0x30/4]|=model.mode==18?7:3;
 }
}
static uint64_t now(void *ctx){(void)ctx;return model.clock;}
static void delay(void *ctx,unsigned us){(void)ctx;model.clock+=us;}
static const struct x9u_io io={0,read32,write32,0,now,delay};
void link_reset(unsigned mode)
{memset(&model,0,sizeof(model));model.mode=mode;model.r[0]=0x55c7051f;model.r[8/4]=0x410;model.r[0x24/4]=0x1234;model.r[0x2240/4]=0x10480000;model.powerdown=1;model.lcc=1;model.savepower=0xabcdefef;}
void link_set(unsigned off,uint32_t value){model.r[off/4]=value;}
uint32_t link_get(unsigned off){return model.r[off/4];}
int link_start(int verified){return x9u_link_start(&io,&model.link,verified);}
int link_stop(void){return x9u_link_stop(&io,&model.link);}
unsigned link_stage(void){return model.link.stage;}
unsigned link_events(void){return model.n;}
unsigned link_event(unsigned i,unsigned field){return i<model.n&&field<2?model.events[i][field]:0xffffffff;}
uint64_t link_time(void){return model.clock;}

int link_start_mtk(int verified){return x9u_link_start_mtk(&io,&model.link,verified);}
unsigned link_attribute(void){return model.link.last_uic_attribute;}
unsigned link_completed(void){return model.link.prelink_completed;}
uint32_t link_savepower(void){return model.savepower;}

static int reset_device(void *ctx,unsigned high)
{
 (void)ctx;if(high==2)return model.mode==16?-7:0;model.reset_calls++;
 if(model.n<256){model.events[model.n][0]=0xffff0000U;model.events[model.n][1]=high;model.n++;}
 return (model.mode==11&&!high)||(model.mode==12&&high)?-7:0;
}
int link_start_full(int verified){return x9u_link_start_full(&io,&model.link,verified,reset_device);}
unsigned link_attempts(void){return model.link.attempts;}
unsigned link_resets(void){return model.reset_calls;}

int link_handoff(void){return x9u_link_handoff(&io,&model.link);}
