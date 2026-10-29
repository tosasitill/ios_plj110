/* Model boundaries of actual HS implementation; no PHY or storage emulation. */
#include <stdint.h>
#include <string.h>
#include "ufs_hs.h"
static struct {
 uint32_t regs[0x2400/4],local[0x10000],peer[0x10000],event[512][3];
 unsigned n,mode,notifies,sets;uint64_t clock,upms_at,uccs_at;struct x9u_hs_state hs;
} m;
static uint32_t rd(void *ctx,unsigned off){(void)ctx;return m.regs[off/4];}
static void wr(void *ctx,unsigned off,uint32_t value)
{
 (void)ctx;
 if(m.n<512){m.event[m.n][0]=off;m.event[m.n][1]=value;m.event[m.n][2]=m.regs[0x94/4]>>16;m.n++;}
 if(off==0x20){m.regs[off/4]&=~value;return;}
 m.regs[off/4]=value;
 if(off==0x144){if(m.mode!=10)m.regs[off/4]=3;return;}
 if(off!=0x90)return;
 unsigned a=m.regs[0x94/4]>>16;uint32_t v=m.regs[0x9c/4];m.regs[0x98/4]=0;
 if(m.mode==13&&value==2&&a==0x1571)return;
 m.regs[0x20/4]|=1U<<10;
 if(value==3){if(m.mode==16)m.regs[0x98/4]=1;else m.regs[0x9c/4]=m.peer[a];return;}
 if(value==1){m.regs[0x9c/4]=m.local[a];return;}
 if(value!=2){m.regs[0x98/4]=1;return;}
 m.sets++;
 if(m.mode==4&&a==0x1568){m.regs[0x98/4]=1;return;}
 if(a!=0x1571){m.local[a]=v;return;}
 if(m.mode!=1)m.upms_at=m.clock+10;
 if(m.mode==14){m.regs[0x20/4]&=~(1U<<10);m.uccs_at=m.clock+30;}
}
static uint64_t now(void *ctx){(void)ctx;return m.clock;}
static void delay(void *ctx,unsigned us)
{
 (void)ctx;m.clock+=us;
 if(m.upms_at&&m.clock>=m.upms_at){m.upms_at=0;m.regs[0x20/4]|=1U<<4;
  m.regs[0x30/4]=(m.mode==2?0:0x100)|0xf;
  m.local[0x1571]=m.mode==3?0x55:0x44;
  if(m.mode==17)m.local[0x1560]=3;
 }
 if(m.uccs_at&&m.clock>=m.uccs_at){m.uccs_at=0;m.regs[0x20/4]|=1U<<10;}
}
static int notify(void *ctx,unsigned on,unsigned stage)
{
 (void)ctx;m.notifies++;
 if((m.mode==11&&on&&stage==0)||(m.mode==12&&on&&stage==1))return X9U_RESPONSE;
 return 0;
}
static const struct x9u_io io={0,rd,wr,0,now,delay};
int hs_run(unsigned mode)
{
 memset(&m,0,sizeof(m));m.mode=mode;m.regs[8/4]=0x410;m.regs[0x34/4]=1;m.regs[0x30/4]=0xf;m.regs[0x2240/4]=0x10480000;m.regs[0x20/4]=1U<<16;
 m.local[0x1571]=0x55;m.local[0x1568]=m.local[0x1583]=1;m.local[0x1560]=m.local[0x1580]=1;
 m.local[0x1587]=m.peer[0x1587]=4;m.local[0x1561]=m.local[0x1581]=m.peer[0x1561]=m.peer[0x1581]=2;
 if(mode==5)m.regs[0x20/4]|=1U<<4;
 if(mode==6)m.regs[0x58/4]=1;
 if(mode==7)m.peer[0x1587]=0;
 if(mode==8)m.peer[0x1581]=1;
 if(mode==9){m.local[0x1571]=0x11;m.local[0x1568]=m.local[0x1583]=4;m.local[0x1560]=m.local[0x1580]=2;m.local[0x156a]=2;}
 if(mode==15)m.regs[0x2240/4]=0;
 return x9u_hs_enable(&io,&m.hs,notify);
}
unsigned hs_stage(void){return m.hs.stage;}
unsigned hs_changed(void){return m.hs.changed;}
unsigned hs_value(unsigned n){return n<6?m.hs.after[n]:0;}
unsigned hs_events(void){return m.n;}
unsigned hs_event(unsigned n,unsigned field){return n<m.n&&field<3?m.event[n][field]:0xffffffff;}
unsigned hs_sets(void){return m.sets;}
unsigned hs_notifies(void){return m.notifies;}
unsigned hs_is(void){return m.regs[0x20/4];}
uint64_t hs_time(void){return m.clock;}
