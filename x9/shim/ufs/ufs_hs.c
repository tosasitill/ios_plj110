/* MT6993 HS bring-up: OPPO ufs_mtk_pre_pwr_change + standard ufshcd PA
 * sequence. Deliberately G1 (no G4/5 adapt/vcore/clock scaling assumptions).
 * UCCS alone is not a completed power transition: UPMS and UPMCRS required.
 */
#include "ufs_hs.h"
#define UCCS (1U<<10)
#define UPMS (1U<<4)
static uint32_t rd(const struct x9u_io *io,unsigned o){return io->read32(io->context,o);}
static void wr(const struct x9u_io *io,unsigned o,uint32_t v){io->write32(io->context,o,v);}
static int wait(const struct x9u_io *io,unsigned o,uint32_t mask,uint32_t value,unsigned us)
{
 uint64_t start=io->now_us(io->context);
 for(unsigned n=0;n<=us/10;n++){
  if((rd(io,o)&mask)==value)return 0;
  if(io->now_us(io->context)-start>=us)break;
  io->delay_us(io->context,10);
 }
 return X9U_TIMEOUT;
}
static int ready(const struct x9u_io *io)
{
 if(!io||!io->read32||!io->write32||!io->delay_us||!io->now_us)return X9U_ARGUMENT;
 if(!(rd(io,0x34)&1)||(rd(io,0x30)&0xb)!=0xb)return X9U_NOT_READY;
 if(rd(io,0x58)||rd(io,0x78)||(rd(io,8)>=0x400&&(rd(io,0x300)&1)))return X9U_BUSY;
 return 0;
}
static int cmd(const struct x9u_io *io,struct x9u_hs_state *s,unsigned op,unsigned attr,uint32_t *value)
{
 s->last_command=op;s->last_attribute=attr;
 int e=wait(io,0x30,8,8,100000);if(e)return e;
 if(rd(io,0x20)&UCCS)return X9U_BUSY;
 wr(io,0x94,attr<<16);wr(io,0x98,0);wr(io,0x9c,*value);wr(io,0x90,op);
 e=wait(io,0x20,UCCS,UCCS,500000);if(e)return e;
 s->uic_result=rd(io,0x98)&255;*value=rd(io,0x9c);wr(io,0x20,UCCS);
 return s->uic_result?X9U_RESPONSE:0;
}
static int get(const struct x9u_io *io,struct x9u_hs_state *s,unsigned op,unsigned attr,uint32_t *v)
{*v=0;return cmd(io,s,op,attr,v);}
static int set(const struct x9u_io *io,struct x9u_hs_state *s,unsigned attr,uint32_t v)
{return cmd(io,s,2,attr,&v);}
int x9u_hs_read(const struct x9u_io *io,struct x9u_hs_state *s,uint32_t out[6])
{
 if(!s||!out)return X9U_ARGUMENT;
 int e=ready(io);if(e)return e;
 const unsigned attrs[]={0x1571,0x1568,0x1583,0x1560,0x1580,0x156a};
 for(unsigned n=0;n<6;n++){e=get(io,s,1,attrs[n],out+n);if(e)return e;}
 return 0;
}
int x9u_hs_valid(const uint32_t p[6])
{
 unsigned rx=p[0]>>4,tx=p[0]&15;
 return (rx==1||rx==4)&&(tx==1||tx==4)&&p[1]>=1&&p[1]<=5&&p[2]>=1&&p[2]<=5&&
        p[3]>=1&&p[3]<=2&&p[4]>=1&&p[4]<=2&&(p[5]==1||p[5]==2);
}
static unsigned minimum(unsigned a,unsigned b){unsigned n=a<b?a:b;return n<2?n:2;}
int x9u_hs_enable(const struct x9u_io *io,struct x9u_hs_state *s,
                  int (*notify)(void *,unsigned,unsigned))
{
 if(!s)return X9U_ARGUMENT;
 *s=(struct x9u_hs_state){0};s->stage=1;
 int e=ready(io);if(e)return e;
 if(rd(io,0x2240)!=0x10480000)return X9U_UNSUPPORTED;
 e=x9u_hs_read(io,s,s->before);if(e)return e;
 s->refclk_before=rd(io,0x144);
 /* Retain an already-qualified HS link without lowering its gear. */
 if(x9u_hs_valid(s->before)){
  for(unsigned n=0;n<6;n++)s->after[n]=s->before[n];
  s->refclk_after=s->refclk_before;s->stage=10;return 0;
 }
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 s->stage=2;
 uint32_t tx,rx,peer_tx,peer_rx;
 if((e=get(io,s,1,0x1587,&s->max_rx_gear))||
    (e=get(io,s,3,0x1587,&s->peer_max_rx_gear))||
    (e=get(io,s,1,0x1561,&tx))||(e=get(io,s,1,0x1581,&rx))||
    (e=get(io,s,3,0x1561,&peer_tx))||(e=get(io,s,3,0x1581,&peer_rx)))return e;
 if(!s->max_rx_gear||s->max_rx_gear>5||!s->peer_max_rx_gear||s->peer_max_rx_gear>5||
    !tx||tx>4||!rx||rx>4||!peer_tx||peer_tx>4||!peer_rx||peer_rx>4)return X9U_UNSUPPORTED;
 s->tx_lanes=minimum(tx,peer_rx);s->rx_lanes=minimum(rx,peer_tx);
 s->stage=3;
 /* Exact OPPO REFCLK_REQUEST/ACK contract; no guessed PHY MMIO. */
 if((s->refclk_before&3)!=3){
  if(!notify)return X9U_NOT_READY;
  if((e=notify(io->context,1,0)))return e;
  wr(io,0x144,1);
  if((e=wait(io,0x144,3,3,3000))){(void)notify(io->context,0,1);return e;}
  io->delay_us(io->context,32);
  if((e=notify(io->context,1,1)))return e;
 }
 s->refclk_after=rd(io,0x144);
 s->stage=4;
 /* G1 uses NO_ADAPT, as ufshcd_dme_configure_adapt specifies below G4. */
 if((e=set(io,s,0x15d4,3)))return e;
 const unsigned attrs[]={0x1583,0x1580,0x1584,0x1568,0x1560,0x1569,0x156a};
 const uint32_t values[]={1,s->rx_lanes,1,1,s->tx_lanes,1,2};
 for(unsigned n=0;n<7;n++)if((e=set(io,s,attrs[n],values[n])))return e;
 s->stage=5;
 const unsigned timeouts[]={0x15b0,0x15b1,0x15b2,0x15b3,0x15b4,0x15b5,0xd041,0xd042,0xd043};
 const uint32_t limits[]={8191,65535,32767,8191,65535,32767,8191,65535,32767};
 for(unsigned n=0;n<9;n++)if((e=set(io,s,timeouts[n],limits[n])))return e;
 s->stage=6;
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 s->changed=1;
 if((e=set(io,s,0x1571,0x44)))return e; /* FASTAUTO/FASTAUTO */
 s->stage=7;
 if((e=wait(io,0x20,UPMS,UPMS,500000)))return e;
 s->upmcrs=(rd(io,0x30)>>8)&7;wr(io,0x20,UPMS);
 if(s->upmcrs!=1)return X9U_RESPONSE; /* PWR_LOCAL, not PWR_OK=0 */
 s->stage=8;
 if((e=x9u_hs_read(io,s,s->after)))return e;
 if(!x9u_hs_valid(s->after)||s->after[0]!=0x44||s->after[1]!=1||s->after[2]!=1||
    s->after[3]!=s->tx_lanes||s->after[4]!=s->rx_lanes||s->after[5]!=2)return X9U_RESPONSE;
 s->stage=10;return 0;
}

/* Run only after the proven G1 path and an initial stable root read. */
int x9u_hs_upgrade_g2(const struct x9u_io *io,struct x9u_hs_state *s)
{
 if(!s)return X9U_ARGUMENT;
 int e=ready(io);if(e)return e;
 uint32_t cur[6];if((e=x9u_hs_read(io,s,cur)))return e;
 for(unsigned n=0;n<6;n++)s->after[n]=cur[n];
 /* Never lower an inherited faster link or alter an unexpected mode. */
 if(!x9u_hs_valid(cur))return X9U_UNSUPPORTED;
 if(cur[1]>=2 && cur[2]>=2){s->stage=20;return 0;}
 if(cur[0]!=0x44||cur[1]!=1||cur[2]!=1||cur[3]!=2||cur[4]!=2||cur[5]!=2){s->stage=21;return 0;}
 s->stage=22;
 if((e=get(io,s,1,0x1587,&s->max_rx_gear))||
    (e=get(io,s,3,0x1587,&s->peer_max_rx_gear)))return e;
 if(s->max_rx_gear<2||s->peer_max_rx_gear<2){s->stage=21;return 0;}
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 /* Linux ufshcd: NO_ADAPT for negotiated gear below G4. */
 s->stage=23;
 if((e=set(io,s,0x15d4,3))||(e=set(io,s,0x1583,2))||
    (e=set(io,s,0x1568,2)))return e;
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 s->stage=24;s->changed=1;
 if((e=set(io,s,0x1571,0x44)))return e;
 s->stage=25;
 if((e=wait(io,0x20,UPMS,UPMS,500000)))return e;
 s->upmcrs=(rd(io,0x30)>>8)&7;wr(io,0x20,UPMS);
 if(s->upmcrs!=1)return X9U_RESPONSE;
 s->stage=26;
 if((e=x9u_hs_read(io,s,s->after)))return e;
 if(s->after[0]!=cur[0]||s->after[1]!=2||s->after[2]!=2||
    s->after[3]!=cur[3]||s->after[4]!=cur[4]||s->after[5]!=cur[5])return X9U_RESPONSE;
 s->stage=20;return 0;
}

/* UniPro/Linux use NO_ADAPT for G3 (below G4).
 * Clock/voltage state is inherited from LK: do not guess clock/power MMIO. */
int x9u_hs_upgrade_g3(const struct x9u_io *io,struct x9u_hs_state *s)
{
 if(!s)return X9U_ARGUMENT;
 int e=ready(io);if(e)return e;
 uint32_t cur[6];if((e=x9u_hs_read(io,s,cur)))return e;
 for(unsigned n=0;n<6;n++)s->after[n]=cur[n];
 if(!x9u_hs_valid(cur))return X9U_UNSUPPORTED;
 if(cur[1]>=3 && cur[2]>=3){s->stage=30;return 0;}
 if(cur[0]!=0x44||cur[1]!=2||cur[2]!=2||cur[3]!=2||cur[4]!=2||cur[5]!=2){s->stage=31;return 0;}
 s->stage=32;
 if((e=get(io,s,1,0x1587,&s->max_rx_gear))||
    (e=get(io,s,3,0x1587,&s->peer_max_rx_gear)))return e;
 if(s->max_rx_gear<3||s->peer_max_rx_gear<3){s->stage=31;return 0;}
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 s->stage=33;
 if((e=set(io,s,0x15d4,3))||(e=set(io,s,0x1583,3))||
    (e=set(io,s,0x1568,3)))return e;
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 s->stage=34;s->changed=1;
 if((e=set(io,s,0x1571,0x44)))return e;
 s->stage=35;
 if((e=wait(io,0x20,UPMS,UPMS,500000)))return e;
 s->upmcrs=(rd(io,0x30)>>8)&7;wr(io,0x20,UPMS);
 if(s->upmcrs!=1)return X9U_RESPONSE;
 s->stage=36;
 if((e=x9u_hs_read(io,s,s->after)))return e;
 if(s->after[0]!=cur[0]||s->after[1]!=3||s->after[2]!=3||
    s->after[3]!=cur[3]||s->after[4]!=cur[4]||s->after[5]!=cur[5])return X9U_RESPONSE;
 s->stage=30;return 0;
}
