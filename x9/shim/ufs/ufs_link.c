/* X9 LK references (RVA): stop c28a8 writes HCE=0; enable c5518 writes
 * HCE=1 then waits 1000us; UIC c2964 uses HCS[3], ARG1/2/3, command and
 * IS[10] completion; c4414 submits DME_LINK_STARTUP 0x16 and checks HCS[0].
 * Platform device-reset helpers in LK are deliberately NOT copied here.
 * Runtime CAP/VER from pan73 proved HCI reachable, not PHY/link readiness.
 */
#include "ufs_link.h"
#define HCE 0x34U
#define HCS 0x30U
#define IS 0x20U
#define IE 0x24U
#define UIC_DONE (1U<<10)
static uint32_t rd(const struct x9u_io *io,unsigned reg){return io->read32(io->context,reg);}
static void wr(const struct x9u_io *io,unsigned reg,uint32_t value){io->write32(io->context,reg,value);}
static int wait(const struct x9u_io *io,unsigned reg,uint32_t mask,uint32_t value,unsigned timeout)
{
 uint64_t start=io->now_us(io->context);
 for(unsigned i=0;i<=timeout/10;i++) {
  if((rd(io,reg)&mask)==value)return 0;
  if(io->now_us(io->context)-start>=timeout)break;
  io->delay_us(io->context,10);
 }
 return X9U_TIMEOUT;
}
static int snapshot(const struct x9u_io *io,struct x9u_link *state,int error)
{
 state->hce=rd(io,HCE);state->hcs=rd(io,HCS);state->is=rd(io,IS);
 state->phy_error=rd(io,0x38);state->dl_error=rd(io,0x3c);state->nl_error=rd(io,0x40);
 state->tl_error=rd(io,0x44);state->dme_error=rd(io,0x48);
 return error;
}
/* Exact MT6993 vendor pre-link flow, source blob f0f71e00183f725699df5f54f6de338e34fa1b5f:
 * unipro_set_lpm(false), disable_host_tx_lcc(), VS_SAVEPOWERCONTROL &= ~BIT(6).
 * Attributes are local UniPro settings, not media writes or device flag writes. */
static int uic(const struct x9u_io *io,struct x9u_link *state,unsigned command,unsigned attribute,uint32_t value)
{
 state->last_uic_command=command;state->last_uic_attribute=attribute;
 int error=wait(io,HCS,8,8,100000);if(error)return error;
 if(rd(io,IS)&UIC_DONE)return X9U_BUSY;
 wr(io,0x94,(uint32_t)attribute<<16);wr(io,0x98,0);wr(io,0x9c,value);wr(io,0x90,command);
 error=wait(io,IS,UIC_DONE,UIC_DONE,500000);
 state->uic_result=rd(io,0x98)&255;state->uic_arg3=rd(io,0x9c);
 if(error)return error;
 wr(io,IS,UIC_DONE);
 return state->uic_result?X9U_RESPONSE:0;
}
static int mtk_prelink(const struct x9u_io *io,struct x9u_link *state)
{
 state->stage=6;
 int error=uic(io,state,2,0xd0a8,0);if(error)return error;state->prelink_completed=1;
 state->stage=7;error=uic(io,state,2,0x1585,0);if(error)return error;state->prelink_completed=2;
 state->stage=8;error=uic(io,state,1,0xd0a6,0);if(error)return error;
 state->savepower_before=state->uic_arg3;state->savepower_after=state->uic_arg3&~(1U<<6);state->prelink_completed=3;
 state->stage=9;error=uic(io,state,2,0xd0a6,state->savepower_after);if(error)return error;
 state->prelink_completed=4;return 0;
}
static int full_prelink(const struct x9u_io *io,struct x9u_link *state)
{
 int error=mtk_prelink(io,state);if(error)return error;
 state->stage=20;error=uic(io,state,1,0xd0a6,0);if(error)return error;
 uint32_t value=state->uic_arg3&~0xdU;
 state->stage=21;error=uic(io,state,2,0xd0a6,value);if(error)return error;
 state->stage=22;error=uic(io,state,1,0xd0a1,0);if(error)return error;
 value=state->uic_arg3|0x20;
 state->stage=23;return uic(io,state,2,0xd0a1,value);
}
static int start(const struct x9u_io *io,struct x9u_link *state,int verified,int mediatek,int (*reset)(void *,unsigned))

{
 if(!io||!state||!io->read32||!io->write32||!io->now_us||!io->delay_us)return X9U_ARGUMENT;
 *state=(struct x9u_link){0};
 state->original_hce=rd(io,HCE);state->original_ie=rd(io,IE);
 if(!verified)return snapshot(io,state,X9U_DMA_UNVERIFIED);
 uint32_t cap=rd(io,0),version=rd(io,8);
 if(!cap||cap==UINT32_MAX||version<0x200||version>0x410)return snapshot(io,state,X9U_UNSUPPORTED);
 if(rd(io,0x58)||rd(io,0x78)||(version>=0x400&&(rd(io,0x300)&1)))return snapshot(io,state,X9U_BUSY);
 if((state->original_hce&1)&&(rd(io,HCS)&0xb)==0xb)return snapshot(io,state,0);
 /* Never consume a UIC completion owned by an unknown earlier command. */
 if(rd(io,IS)&UIC_DONE)return snapshot(io,state,X9U_BUSY);
 if(mediatek==2) {
  state->stage=10;state->ip_version=rd(io,0x2240);
  if(state->ip_version!=0x10480000||!reset||state->original_hce&1)return snapshot(io,state,X9U_UNSUPPORTED);
  state->stage=15;
  int host_reset=reset(io->context,2);
  if(host_reset)return snapshot(io,state,X9U_RESPONSE);
  state->original_ahit=rd(io,0x18);
  state->original_xoufs=rd(io,0x140);state->original_opt=rd(io,0x160);
  state->original_coherence=rd(io,0x2308);state->platform_changed=1;
  wr(io,IE,0);wr(io,0x18,0);
  wr(io,0x140,state->original_xoufs|0x80);
  uint32_t opt=(state->original_opt&~0x007fff00U)|0x00453000U;
  opt=(opt|(1U<<25))&~(1U<<28);wr(io,0x160,opt);
  wr(io,0x2308,state->original_coherence|1);
  state->stage=11;int status=reset(io->context,0);state->reset_low_status=(uint32_t)status;
  if(status) {
   state->reset_high_status=(uint32_t)reset(io->context,1);
   return snapshot(io,state,X9U_RESPONSE);
  }
  wr(io,HCE,0);state->activated=1;io->delay_us(io->context,10);
  state->stage=12;status=reset(io->context,1);state->reset_high_status=(uint32_t)status;
  if(status)return snapshot(io,state,X9U_RESPONSE);
  io->delay_us(io->context,10000);
 }
 if(!(state->original_hce&1)) {
  state->stage=1;state->activated=1;
  wr(io,IE,0);wr(io,HCE,1);io->delay_us(io->context,1000);
  int error=wait(io,HCE,1,1,100000);if(error)return snapshot(io,state,error);
 }
 if(mediatek==2) {
  state->stage=13;int ack=wait(io,0x2308,2,2,10000);state->coherence=rd(io,0x2308);
  if(ack)return snapshot(io,state,ack);
 }
 state->stage=2;
 int error=wait(io,HCS,8,8,100000);if(error)return snapshot(io,state,error);
 /* At most two LINK_STARTUP attempts; second only after the observed
  * result1 + generic LINERESET, following Linux hba_enable recovery. */
 if(!(rd(io,HCS)&1)) {
  for(unsigned attempt=0;attempt<(mediatek==2?2U:1U);attempt++) {
   state->attempts=attempt+1;
   if(mediatek) {
    error=mediatek==2?full_prelink(io,state):mtk_prelink(io,state);
    if(error)return snapshot(io,state,error);
   }
   state->stage=3;error=uic(io,state,0x16,0,0);
   if(!error)break;
   uint32_t phy=rd(io,0x38);state->phy_error=phy;
   if(mediatek!=2||attempt||error!=X9U_RESPONSE||state->uic_result!=1||!(phy&0x10)||rd(io,HCS)&1)
    return snapshot(io,state,error);
   state->stage=14;wr(io,HCE,0);
   error=wait(io,HCE,1,0,100000);if(error)return snapshot(io,state,error);
   io->delay_us(io->context,1000);wr(io,HCE,1);io->delay_us(io->context,1000);
   error=wait(io,HCE,1,1,100000);if(error)return snapshot(io,state,error);
  }
  if(error)return snapshot(io,state,error);
 }
 state->stage=4;
 error=wait(io,HCS,3,3,100000);
 if(error)return snapshot(io,state,error);
 if(mediatek==2) {
  /* pan76 proved first startup succeeds (HCS=0xf), but a second
   * LINK_STARTUP returns1 while leaving HCS=0xf. Do not restart a live link.
   * The next required gate is actual NOP/fDeviceInit via the private queue,
   * not Linux's cached dev_pwr_mode heuristic (absent in this loader). */
  state->ready_hcs=rd(io,HCS);
  if(!(rd(io,HCE)&1)||(state->ready_hcs&0xb)!=0xb)return snapshot(io,state,X9U_NOT_READY);
  state->repeat_startup_skipped=1;
  state->stage=24;error=uic(io,state,1,0xd09e,0);if(error)return snapshot(io,state,error);
  uint32_t value=state->uic_arg3|0x100;
  state->stage=25;error=uic(io,state,2,0xd09e,value);if(error)return snapshot(io,state,error);
  state->stage=26;error=uic(io,state,1,0xd0a6,0);if(error)return snapshot(io,state,error);
  value=state->uic_arg3|0xd;
  state->stage=27;error=uic(io,state,2,0xd0a6,value);if(error)return snapshot(io,state,error);
  state->stage=28;error=uic(io,state,1,0xd0a1,0);if(error)return snapshot(io,state,error);
  value=state->uic_arg3&~0x20U;
  state->stage=29;error=uic(io,state,2,0xd0a1,value);if(error)return snapshot(io,state,error);
 }
 state->stage=5;return snapshot(io,state,0);
}
int x9u_link_start(const struct x9u_io *io,struct x9u_link *state,int verified)
{return start(io,state,verified,0,0);}
int x9u_link_start_mtk(const struct x9u_io *io,struct x9u_link *state,int verified)
{return start(io,state,verified,1,0);}
int x9u_link_start_full(const struct x9u_io *io,struct x9u_link *state,int verified,int (*reset)(void *,unsigned))
{return start(io,state,verified,2,reset);}
int x9u_link_stop(const struct x9u_io *io,struct x9u_link *state)
{
 if(!io||!state)return X9U_ARGUMENT;
 if(!state->activated&&!state->platform_changed)return 0;
 if(rd(io,0x58)||rd(io,0x78))return X9U_DMA_PENDING;
 /* No queue may be running when returning HCE to the disabled LK state. */
 if(rd(io,0x60)&1)return X9U_BUSY;
 wr(io,HCE,state->original_hce);
 if(wait(io,HCE,1,state->original_hce&1,100000))return X9U_TIMEOUT;
 if(state->platform_changed) {
  wr(io,0x2308,state->original_coherence);wr(io,0x160,state->original_opt);wr(io,0x140,state->original_xoufs);wr(io,0x18,state->original_ahit);
  state->platform_changed=0;
 }
 wr(io,IE,state->original_ie);state->activated=0;return 0;
}

/* Native-kernel handoff keeps trained link/vendor coherence active, but
 * disconnects all loader lists and interrupts. No DMA to shim RAM remains. */
int x9u_link_handoff(const struct x9u_io *io,struct x9u_link *state)
{
 if(!io||!state)return X9U_ARGUMENT;
 if(rd(io,0x58)||rd(io,0x78))return X9U_DMA_PENDING;
 if(rd(io,0x60)&1)return X9U_BUSY;
 if(!(rd(io,HCE)&1)||(rd(io,HCS)&0xb)!=0xb)return X9U_NOT_READY;
 wr(io,IE,0);wr(io,0x50,0);wr(io,0x54,0);
 return (rd(io,0x50)||rd(io,0x54))?X9U_RESPONSE:0;
}
