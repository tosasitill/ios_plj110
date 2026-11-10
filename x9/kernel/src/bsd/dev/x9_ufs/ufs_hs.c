/* MT6993 HS bring-up: OPPO ufs_mtk_pre_pwr_change + standard ufshcd PA
 * sequence. Early enable remains G1; high gears require the late platform gate.
 * UCCS alone is not a completed power transition: UPMS and UPMCRS required.
 */
#include "ufs_hs.h"
#define UCCS (1U<<10)
#define UPMS (1U<<4)
#define LINK_ERRORS 0x70884U /* UIC/link-lost/device/controller/bus/crypto fatal */
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
 e=wait(io,0x20,UCCS,UCCS,500000);if(e){s->uncertain=1;return e;}
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

int x9u_hs_prepare_high(const struct x9u_io *io,struct x9u_hs_state *s,unsigned requested)
{
 if(!s||(requested!=4&&requested!=5))return X9U_ARGUMENT;
 *s=(struct x9u_hs_state){0};s->stage=40;
 int e=ready(io);if(e)return e;
 if(rd(io,0x2240)!=0x10480000||rd(io,0x18))return X9U_UNSUPPORTED;
 /* Neither clear stale completions nor mistake a pending UIC for this request. */
 if(rd(io,0x20)&(UCCS|UPMS|LINK_ERRORS))return X9U_BUSY;
 if((e=x9u_hs_read(io,s,s->before)))return e;
 for(unsigned i=0;i<6;i++)s->after[i]=s->before[i];
 if(!x9u_hs_valid(s->before)||(s->before[0]!=0x44&&s->before[0]!=0x11)||
    s->before[1]!=s->before[2]||s->before[1]<3||s->before[3]!=2||
    s->before[4]!=2||s->before[5]!=2)return X9U_UNSUPPORTED;
 s->stage=41;
 if((e=get(io,s,1,0x15a9,&s->local_version))||
    (e=get(io,s,1,0x1587,&s->max_rx_gear))||
    (e=get(io,s,3,0x1587,&s->peer_max_rx_gear)))return e;
 /* ufs_mtk_get_controller_version: UniPro1.8 enum=5 -> hw major>=3.
  * Do not infer ADAPT support from vendor HCI version bits. */
 if(s->local_version<5||s->local_version>0xff||s->max_rx_gear<4||
    s->max_rx_gear>5||s->peer_max_rx_gear<4||s->peer_max_rx_gear>5)return X9U_UNSUPPORTED;
 s->target_gear=requested;
 if(s->target_gear>s->max_rx_gear)s->target_gear=s->max_rx_gear;
 if(s->target_gear>s->peer_max_rx_gear)s->target_gear=s->peer_max_rx_gear;
 /* Never downclock an inherited faster link. */
 if(s->target_gear<s->before[1])return X9U_UNSUPPORTED;
 s->tx_lanes=s->rx_lanes=2;s->stage=42;return 0;
}

int x9u_hs_apply_high(const struct x9u_io *io,struct x9u_hs_state *s,unsigned gear)
{
 if(!s||s->local_version<5||gear<4||gear>s->target_gear||gear>5)return X9U_ARGUMENT;
 int e=ready(io);if(e)return e;
 uint32_t cur[6];s->stage=43;
 if((e=x9u_hs_read(io,s,cur)))return e;
 for(unsigned i=0;i<6;i++)if(cur[i]!=s->after[i])return X9U_RESPONSE;
 if(gear<cur[1]||gear<cur[2])return X9U_ARGUMENT;
 if(cur[0]==0x11&&cur[1]==gear&&cur[2]==gear){s->stage=49;return 0;}
 if(rd(io,0x20)&(UCCS|UPMS|LINK_ERRORS))return X9U_BUSY;
 /* RateB stays unchanged: the captured pmc-via-fastauto workaround only
  * requires a G1 detour when HS series changes. Here it does not. */
 s->stage=44;s->changed=1;
 if((e=set(io,s,0x15d4,1))|| /* INITIAL_ADAPT for G4/G5 */
    (e=set(io,s,0x1583,gear))||(e=set(io,s,0x1580,2))||
    (e=set(io,s,0x1584,1))||(e=set(io,s,0x1568,gear))||
    (e=set(io,s,0x1560,2))||(e=set(io,s,0x1569,1))||
    (e=set(io,s,0x156a,2)))return e;
 if(rd(io,0x20)&UPMS)return X9U_BUSY;
 s->stage=45;
 if((e=set(io,s,0x1571,0x11)))return e; /* Android FAST/FAST, not FASTAUTO */
 s->stage=46;
 if((e=wait(io,0x20,UPMS,UPMS,500000))){s->uncertain=1;return e;}
 s->upmcrs=(rd(io,0x30)>>8)&7;wr(io,0x20,UPMS);
 if(s->upmcrs!=1)return X9U_RESPONSE; /* require PWR_LOCAL */
 if(rd(io,0x20)&LINK_ERRORS)return X9U_RESPONSE;
 s->stage=47;
 if((e=x9u_hs_read(io,s,s->after)))return e;
 if(s->after[0]!=0x11||s->after[1]!=gear||s->after[2]!=gear||
    s->after[3]!=2||s->after[4]!=2||s->after[5]!=2)return X9U_RESPONSE;
 s->stage=49;return 0;
}
