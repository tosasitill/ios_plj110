/* Synthetic vectors only: no device numbers, vendor/Apple bytes or network.
 * Compile the production transport, not a parallel Python implementation. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <strings.h>
#include "../../kernel/src/bsd/dev/x9_modem_bridge.h"
static int lock_depth;
static void (*during_md_lock)();
static unsigned md_lock_until_hook;
static unsigned bb_guard_depth;
static void (*during_bb_guard)();
static void (*during_physical_tx)();
#define LCK_GRP_DECLARE(n, s) int n
#define LCK_MTX_DECLARE(n, g) int n
static void lck_mtx_lock(int *){
 assert(!lock_depth);
 if(during_md_lock){
  assert(md_lock_until_hook);
  if(!--md_lock_until_hook){auto hook=during_md_lock;during_md_lock=nullptr;hook();}
 }
 lock_depth=1;
}
static void lck_mtx_unlock(int *){assert(lock_depth);lock_depth=0;}
static int x9_at_enabled(){return 1;}
static int sweet_x9_arg(const char *,int){return 1;}
static uint64_t fake_ms;
static uint64_t mach_absolute_time(){return fake_ms*1000000;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t;}
int x9_md_at_online(){return 1;}
static unsigned writes,last_length;
static int tx_result;
static uint8_t last_frame[X9_MIPC_FRAME_MAX];
static int x9md_tx(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,const uint8_t *p,unsigned n){
 assert(!lock_depth&&!bb_guard_depth&&a==5&&b==217&&c==0&&d==16+n&&e==0);
 if(during_physical_tx){auto hook=during_physical_tx;during_physical_tx=nullptr;hook();}
 writes++;last_length=n;memcpy(last_frame,p,n);return tx_result;
}
#include "../../kernel/src/bsd/dev/x9_mipc_bridge.inc"
static const unsigned X9BB_PORT_BASE=0x2000;
static const unsigned X9BB_NODE=1,CBQ_PORT_BASE=0x4000,CBQ_MAX_CLIENTS=8;
static const uint16_t X9Q_UNSUPPORTED=94,X9Q_INTERNAL=3,X9Q_NOT_READY=16,X9Q_ARG=48,X9Q_BUSY=2;
static struct {uint64_t generation;uint8_t sim_slot;} x9q_clients[CBQ_MAX_CLIENTS];
static bool x9w_control_ready;
static uint64_t x9q_ms(){return fake_ms;}
static unsigned qmi_replies,qmi_error;
static uint8_t qmi_response[64];static unsigned qmi_response_len;
static unsigned qmi_indications;static uint8_t qmi_indication_states[32];
static void coachz_baseband_qrtr(uint32_t local,uint32_t node,uint32_t port,const void *p,unsigned n){
 assert(local>=CBQ_PORT_BASE&&node==X9BB_NODE&&port==X9BB_PORT_BASE+9&&n<=sizeof(qmi_response));
 memcpy(qmi_response,p,n);qmi_response_len=n;qmi_replies++;
 const uint8_t *q=static_cast<const uint8_t *>(p);
 if(n==11&&q[0]==4&&q[3]==0x54){
  assert(qmi_indications<sizeof(qmi_indication_states));qmi_indication_states[qmi_indications++]=q[10];
 }
}
struct x9q_packet {
 x9q_packet(const uint8_t *,uint16_t error){qmi_error=error;}
 void send(uint32_t,uint32_t){qmi_replies++;}
};
static const unsigned CBQ_HDR=6;
struct bb_x9_call_frame {const uint8_t *sdu;uint32_t size;uint8_t *owned;};
static bool call_batch_accept=true,call_alloc_fail,call_alloc_poison,call_alloc_expire;
static bool call_bb_expire;
static unsigned call_allocs,call_frees,call_batches,call_batch_count;
static unsigned call_alloc_fail_at;
static uint64_t call_bridge_generation[CBQ_MAX_CLIENTS]={11,12,13,14,15,16,17,18};
static uint64_t bb_x9_call_identity(uint32_t local,uint32_t node){
 assert(!lock_depth||lock_depth==1);
 return node==X9BB_NODE&&local>=CBQ_PORT_BASE&&local-CBQ_PORT_BASE<CBQ_MAX_CLIENTS?
  call_bridge_generation[local-CBQ_PORT_BASE]:0;
}
/* Stand-in for actual MD -> bb_lock guard; not an IOKit/runtime test. The
 * hook models owner/clock change while acquiring bb_lock, before validation. */
extern "C" int coachz_baseband_call_guard(const struct x9_mipc_call_owner *owner,
                                         x9_mipc_call_owner_cb callback,void *context){
 assert(lock_depth==1&&!bb_guard_depth);
 if(during_bb_guard){auto hook=during_bb_guard;during_bb_guard=nullptr;hook();}
 bb_guard_depth=1;
 int result=owner&&callback&&owner->generation&&owner->deadline_ms&&fake_ms<owner->deadline_ms&&
  bb_x9_call_identity(owner->local_port,owner->node)==owner->generation?callback(context):0;
 bb_guard_depth=0;return result;
}
static void *IOMallocData(unsigned n){
 assert(!lock_depth);call_allocs++;
 if(call_alloc_poison)x9_mc_poisoned=1;
 if(call_alloc_expire)fake_ms=25000;
 return call_alloc_fail||(call_alloc_fail_at&&call_allocs==call_alloc_fail_at)?nullptr:malloc(n);
}
static void IOFreeData(void *p,unsigned){assert(!lock_depth);call_frees++;free(p);}
static bool bb_x9_call_result(uint32_t local,uint32_t node,uint64_t generation,const uint8_t *sdu,
                              unsigned size,uint8_t *owned,unsigned room,uint64_t deadline){
 assert(!lock_depth&&owned&&room==CBQ_HDR+size&&(size==14||size==18));
 const unsigned id=x9cw_u16(sdu+3);
 assert(id==3||id==0x20||id==0x21||id==0x22||id==0x31||id==0x44);
 assert(size!=18||id==0x20);
 if(call_bb_expire)fake_ms=deadline;
 if(!call_batch_accept||generation!=bb_x9_call_identity(local,node)||
    (!x9cw_u16(sdu+10)&&fake_ms>=deadline))return false;
 qmi_error=x9cw_u16(sdu+12);coachz_baseband_qrtr(local,node,X9BB_PORT_BASE+9,sdu,size);
 free(owned);return true; // stand-in for later host read; not MD completion
}
static bool bb_x9_call_registration(uint32_t local,uint32_t node,uint64_t generation,bb_x9_call_frame *f,uint32_t count,uint64_t deadline){
 assert(lock_depth==1&&count&&count<=9);call_batches++;call_batch_count=count;
 if(call_bb_expire)fake_ms=deadline;
 if(!call_batch_accept||fake_ms>=deadline||generation!=bb_x9_call_identity(local,node))return false;
 for(unsigned i=0;i<count;i++){
  assert(f[i].owned&&f[i].size==(i?11u:14u));
  if(!i){assert(f[i].sdu[0]==2&&x9cw_u16(f[i].sdu+3)==3&&!x9cw_u32(f[i].sdu+10));qmi_error=0;}
  coachz_baseband_qrtr(local,node,X9BB_PORT_BASE+9,f[i].sdu,f[i].size);
  free(f[i].owned); // stand-in for later host read/free, ownership transferred
 }
 return true;
}
static bool bb_x9_call_handover(uint32_t local,uint32_t node,uint64_t generation,bb_x9_call_frame *f,uint32_t count){
 assert(lock_depth==1&&count&&count<=8);call_batches++;call_batch_count=count;
 if(!call_batch_accept||generation!=bb_x9_call_identity(local,node))return false;
 for(unsigned i=0;i<count;i++){
  assert(f[i].owned&&f[i].size==11&&f[i].sdu[0]==4&&x9cw_u16(f[i].sdu+3)==0x54);
  assert(!x9cw_u16(f[i].sdu+1)&&x9cw_u16(f[i].sdu+5)==4&&f[i].sdu[7]==1&&x9cw_u16(f[i].sdu+8)==1);
  assert(f[i].sdu[10]>=1&&f[i].sdu[10]<=4);
  coachz_baseband_qrtr(local,node,X9BB_PORT_BASE+9,f[i].sdu,f[i].size);
  free(f[i].owned); // successful all-or-none transfer; not a physical/network ACK
 }
 return true;
}
static unsigned calllist_batches,calllist_count;static uint8_t calllist_slots[CBQ_MAX_CLIENTS];
static uint8_t calllist_last[CBQ_MAX_CLIENTS][25];static unsigned calllist_sizes[CBQ_MAX_CLIENTS];
static bool bb_x9_call_calllist(uint32_t local,uint32_t node,uint64_t generation,bb_x9_call_frame *f,uint32_t count){
 assert(lock_depth==1&&node==X9BB_NODE&&count==1&&f->owned&&f->size>=18&&f->size<=25);
 unsigned slot=local-CBQ_PORT_BASE;assert(slot<CBQ_MAX_CLIENTS&&generation==bb_x9_call_identity(local,node));
 assert(f->sdu[0]==4&&!x9cw_u16(f->sdu+1)&&x9cw_u16(f->sdu+3)==0x2e);
 assert(calllist_count<sizeof(calllist_slots));calllist_slots[calllist_count++]=slot;
 calllist_sizes[slot]=f->size;memcpy(calllist_last[slot],f->sdu,f->size);
 calllist_batches++;free(f->owned);return true;
}
#include "../../kernel/src/iokit/Kernel/arm/X9Calls.inc"
// Explicit synthetic bound/registered VOICE owner for staging-only cases;
// NAS's sim_slot is no longer used as the independently bound CALL context.
static void dial_owner_fixture(){
 x9w_control_ready=true;x9mi_online(1);
 struct x9c_client &c=x9c_clients[0];c.bound=c.registered=true;c.sim=1;
 c.generation=x9q_clients[0].generation;c.bridge_generation=call_bridge_generation[0];
 c.binding=++x9c_binding_next;
}
static void calllist_fixture(){
 dial_owner_fixture();assert(x9c_calllist_announced);
 x9c_voice_rat[1].valid=true;x9c_voice_rat[1].networkType=4096;
 x9_mc_outbound={};x9_mc_outbound.epoch=1;x9_mc_outbound.revision=1;
 x9_mc_outbound.native_id=7;x9_mc_outbound.seen=1;x9_mc_outbound.ios_state=1;
 struct x9c_client &c=x9c_clients[1];c.bound=c.registered=true;c.sim=1;
 c.generation=x9q_clients[1].generation;c.bridge_generation=call_bridge_generation[1];
 c.binding=++x9c_binding_next;
}
static void calllist_client_isolation(){
 calllist_fixture();
 x9c_calllist_flush(0);assert(calllist_count==1&&calllist_slots[0]==0);
 assert(calllist_last[0][11]==7&&calllist_last[0][12]==1&&calllist_last[0][14]==1&&calllist_last[0][15]==4);
 x9c_calllist_flush(0);assert(calllist_count==1);
 x9c_calllist_flush(1);assert(calllist_count==2&&calllist_slots[1]==1);
 x9c_clients[1].sim=0;x9c_voice_rat[0].valid=true;x9c_voice_rat[0].networkType=4096;
 x9c_calllist_flush(1);assert(calllist_count==2);
 x9c_clients[1].sim=1;x9c_clients[1].binding=++x9c_binding_next;
 x9c_clients[1].registered=true;x9c_clients[1].calllist_valid=false;
 x9c_calllist_flush(1);assert(calllist_count==3&&calllist_slots[2]==1);
}
static void calllist_voice_rat_refresh(){
 calllist_fixture();
 x9c_cmd_init.held=false;
 x9c_cmd_init.confirmed=true;
 x9c_init[1].held=0;
 x9c_init[1].stage=X9_CI_DONE;
 x9c_init[1].registered=1;
 x9c_calllist_flush(0);
 assert(calllist_count==1&&calllist_last[0][15]==4);
 x9c_voice_rat_refresh(1);
 uint8_t frame[X9_MIPC_FRAME_MAX]={};
 x9c_init_poll(frame);
 uint64_t token=x9c_rat_query[1].token;
 assert(token&&!x9c_voice_rat[1].valid);
 x9mi_tick();
 x9c_voice_rat_refresh(1);
 auto confirm=[](uint64_t tx,unsigned rat){
  uint8_t reply[40];
  x9cw_header(reply,sizeof(reply),0x430,2,(unsigned)tx);
  x9cw_scalar(reply+16,0,0,4);
  x9cw_put16(reply+24,0x8100);
  x9cw_put16(reply+26,10);
  x9cw_put16(reply+29,rat);
  x9mi_rx(reply,sizeof(reply));
 };
 confirm(token,4096);
 x9c_init_poll(frame);
 assert(!x9c_rat_query[1].token&&!x9c_voice_rat[1].valid);
 x9c_calllist_flush(0);
 assert(calllist_count==1);
 fake_ms+=10000;
 x9c_init_poll(frame);
 token=x9c_rat_query[1].token;
 assert(token);
 x9mi_tick();
 confirm(token,32768);
 x9c_init_poll(frame);
 x9c_calllist_flush(0);
 assert(calllist_count==2&&calllist_last[0][15]==6);
 x9c_calllist_flush(0);
 assert(calllist_count==2);
}
static void calllist_voice_rat_recovery(){
 calllist_fixture();
 x9c_cmd_init.held=false;
 x9c_cmd_init.confirmed=true;
 x9c_init[1].held=0;
 x9c_init[1].stage=X9_CI_DONE;
 x9c_calllist_flush(0);
 x9c_voice_rat_refresh(1);
 uint64_t filled[X9_MI_SLOTS];
 for(uint64_t &token:filled){token=x9c_voice_rat_submit(1);assert(token);}
 uint8_t frame[X9_MIPC_FRAME_MAX]={};
 x9c_init_poll(frame);
 assert(!x9c_rat_query[1].token&&!x9c_rat_query[1].held&&x9c_rat_query[1].next_ms==10000);
 for(uint64_t token:filled)x9_md_mipc_voice_rat_abandon(token);
 fake_ms=10000;
 x9c_init_poll(frame);
 uint64_t timed_out=x9c_rat_query[1].token;
 assert(timed_out);
 x9mi_tick();
 x9_mc_outbound.ios_state=9;
 x9_mc_outbound.revision++;
 x9c_calllist_flush(0);
 x9c_calllist_flush(1);
 assert(calllist_count==2&&calllist_last[0][12]==9&&calllist_last[0][15]==4);
 fake_ms=35000;
 x9c_init_poll(frame);
 assert(!x9c_rat_query[1].token&&!x9c_rat_query[1].held);
 fake_ms=45000;
 x9c_init_poll(frame);
 uint64_t replacement=x9c_rat_query[1].token;
 assert(replacement>timed_out);
 x9mi_tick();
 uint8_t reply[40];
 x9cw_header(reply,sizeof(reply),0x430,2,(unsigned)timed_out);
 x9cw_scalar(reply+16,0,0,4);
 x9cw_put16(reply+24,0x8100);
 x9cw_put16(reply+26,10);
 x9cw_put16(reply+29,32768);
 x9mi_rx(reply,sizeof(reply));
 x9c_init_poll(frame);
 assert(!x9c_voice_rat[1].valid&&x9c_rat_query[1].token==replacement);
 x9cw_put16(reply+12,(unsigned)replacement);
 x9mi_rx(reply,sizeof(reply));
 x9c_init_poll(frame);
 assert(x9c_voice_rat[1].valid&&x9c_voice_rat[1].networkType==32768);
 x9c_calllist_flush(0);
 assert(calllist_count==2);
}
static void calllist_stale_publication(){
 calllist_fixture();x9_mc_outbound.revision=2;
 md_lock_until_hook=4;during_md_lock=[](){x9_mc_outbound.revision=3;};
 x9c_calllist_flush(0);assert(!calllist_count&&!x9c_clients[0].calllist_valid);
 x9c_calllist_flush(0);assert(calllist_count==1&&x9c_clients[0].calllist_revision==3);
}
static void calllist_generation_fence(){
 calllist_fixture();md_lock_until_hook=4;during_md_lock=[](){x9q_clients[0].generation++;};
 x9c_calllist_flush(0);assert(!calllist_count&&!x9c_clients[0].calllist_valid);
 x9q_clients[0].generation--;x9c_calllist_flush(0);assert(calllist_count==1);
}

static unsigned incoming(uint8_t *p,unsigned mask=0x22,unsigned tx=0x2345){
 x9cw_header(p,64,0x8901,mask,tx);
 x9cw_scalar(p+16,0x100,0x12345678,4);
 x9cw_put16(p+24,0x8101);x9cw_put16(p+26,2);p[28]='X';
 x9cw_scalar(p+32,0x102,129,4);x9cw_scalar(p+40,0x103,0x90abcdef,4);
 x9cw_scalar(p+48,0x104,20,4);x9cw_scalar(p+56,0x105,0,1);return 64;
}
static void rx_incoming(unsigned tx=0x2345){uint8_t p[64];incoming(p,0x22,tx);x9mi_rx(p,sizeof(p));}
static uint32_t event(){
 uint8_t out[X9_MIPC_FRAME_MAX];uint32_t n,token;
 assert(x9_md_mipc_call_event(out,sizeof(out),&n,&token)==1&&n==64&&token);
 assert(out[8]==0x22);return token;
}
static void scalar_ind(unsigned id){uint8_t p[24];x9cw_header(p,sizeof(p),id,1,0);x9cw_scalar(p+16,0x100,1,1);x9mi_rx(p,sizeof(p));}
static void unchanged_other_inboxes(){
 scalar_ind(0x4601);scalar_ind(0x4202);
 uint8_t out[24];uint32_t n,token;
 assert(x9_md_mipc_message_event(out,sizeof(out),&n,&token)==1&&n==24&&!token&&x9cw_u16(out+10)==0x4601);
 assert(x9_md_mipc_event(out,sizeof(out),&n)==1&&n==24&&x9cw_u16(out+10)==0x4202);
 assert(!x9_mm_poisoned&&!x9_mi_poisoned);
}
static void codec_golden(){
 uint8_t cmd[64],rsp[48];incoming(cmd);
 struct x9cw_incoming c;assert(x9cw_incoming(cmd,sizeof(cmd),&c));
 assert(c.call_id==0x12345678&&c.sequence==0x90abcdef&&c.mask==0x22&&c.response_mask==2&&c.sim==1&&c.txid==0x2345&&c.number_length==2);
 assert(x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),1,0,0)==40);
 const uint8_t golden[]={
  0x84,0x19,0x54,0x24,0,0,0,0,2,0,0x02,0x89,0x45,0x23,0x18,0,
  0x00,0x01,0x01,0,1,0,0,0,
  0x02,0x01,0x04,0,0x78,0x56,0x34,0x12,
  0x03,0x01,0x04,0,0xef,0xcd,0xab,0x90};
 assert(sizeof(golden)==40&&!memcmp(rsp,golden,sizeof(golden)));
 assert(x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),0,1,0x11223344)==48);
 assert(rsp[20]==0&&x9cw_u16(rsp+24)==0x101&&x9cw_u32(rsp+28)==0x11223344);
 assert(x9cw_u16(rsp+32)==0x102&&x9cw_u32(rsp+36)==0x12345678);
 assert(!x9cw_approve(cmd,sizeof(cmd),rsp,39,1,0,0));
 assert(!x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),2,0,0));
}
static void codec_invalid(){
 uint8_t p[80],rsp[48];struct x9cw_incoming c;
 for(unsigned n=0;n<64;n++){incoming(p);assert(!x9cw_incoming(p,n,&c));}
 incoming(p);p[29]='Y';assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[28]=0;assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[8]=0x30;assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[18]=3;assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[18]=255;assert(!x9cw_incoming(p,64,&c));
 incoming(p);x9cw_scalar(p+64,0x8100,1,4);x9cw_put16(p+14,56);assert(!x9cw_incoming(p,72,&c));
 incoming(p);x9cw_scalar(p+64,0x106,1,4);x9cw_put16(p+14,56);assert(!x9cw_incoming(p,72,&c));
 assert(!x9cw_approve(p,72,rsp,sizeof(rsp),1,0,0));
 /* libmipc ignores these received fields. They are not SIM selectors. */
 incoming(p);p[0]=0;p[4]=1;p[9]=7;p[23]=0x5a;assert(x9cw_incoming(p,64,&c));
}
static void incoming_routes(){
 uint8_t p[64],rsp[48];struct x9cw_incoming c;
 /* Artificial numbers; live evidence proves route22 -> slot1 -> response02.
  * Change only the header mask, keeping the transaction/identity unchanged. */
 for(unsigned high=0;high<=0xf0;high+=0x10){
  for(unsigned low=1;low<=2;low++){
   incoming(p,high|low);
   assert(x9cw_incoming(p,64,&c)&&c.mask==(high|low)&&c.sim==low-1&&c.response_mask==low);
   assert(x9cw_approve(p,64,rsp,48,1,0,0)==40&&rsp[8]==low&&x9cw_u16(rsp+12)==0x2345);
  }
 }
 const unsigned rejected[]={0,0x10,0x20,0x30,3,0x23,4,8,0xff};
 for(unsigned mask:rejected){incoming(p,mask);assert(!x9cw_incoming(p,64,&c));assert(!x9cw_approve(p,64,rsp,48,1,0,0));}
 incoming(p,0x22);x9mi_rx(p,64);uint32_t token=event();
 assert(x9_mc_slots[0].mask==0x22&&x9_md_mipc_call_reply(token,1,0,0)==1);
 x9mi_tick();assert(writes==1&&last_frame[8]==2&&x9cw_u16(last_frame+12)==0x2345);
}
static void action_codec(){
 uint8_t req[]={0,0x12,0x34,0x21,0,4,0,1,1,0,7},out[48];unsigned id,call;
 assert(x9cw_qmi_action(req,sizeof(req),&id,&call)&&id==0x21&&call==7);
 assert(x9cw_action(req,sizeof(req),out,sizeof(out),7,0x12345678,1,1)==32);
 assert(out[8]==2&&x9cw_u16(out+10)==0x905&&x9cw_u32(out+20)==0&&x9cw_u32(out+28)==0x12345678);
 assert(x9cw_request(out,32,0x905,2)&&!x9cw_request(out,32,0x905,1));
 assert(!x9cw_action(req,sizeof(req),out,sizeof(out),6,0x12345678,1,1));
 assert(!x9cw_action(req,sizeof(req),out,sizeof(out),7,0x12345678,1,2));
 assert(!x9cw_action(req,sizeof(req),out,sizeof(out),7,0,1,1));
 req[3]=0x22;assert(x9cw_action(req,sizeof(req),out,sizeof(out),7,0x12345678,0,1)==16);
 assert(x9cw_u16(out+10)==0x907&&x9cw_u16(out+14)==0&&x9cw_request(out,16,0x907,2));
 req[8]=4;assert(!x9cw_qmi_action(req,sizeof(req),&id,&call));
 req[8]=1;req[3]=0x20;assert(!x9cw_action(req,sizeof(req),out,sizeof(out),7,1,0,1));
}
static const struct x9cw_dial_profile measured_route={1,2,1,0,0,0};
static unsigned dial_qmi(uint8_t *p,unsigned digits=5,bool call_type=true){
 unsigned n=10+digits+(call_type?4:0);bzero(p,n);
 p[1]=0x34;p[2]=0x12;p[3]=0x20;x9cw_put16(p+5,n-7);
 p[7]=1;x9cw_put16(p+8,digits);
 for(unsigned i=0;i<digits;i++)p[10+i]=(uint8_t)('1'+i%9);
 if(call_type){p[10+digits]=0x10;p[11+digits]=1;}
 return n;
}
static void dial_golden(){
 uint8_t q[64],p[104];unsigned n=dial_qmi(q);
 struct x9cw_dial_number d;assert(x9cw_qmi_dial(q,n,&d)&&d.length==5);
 assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==72);
 const uint8_t golden[]={
  0x84,0x19,0x54,0x24,0,0,0,0,2,0,1,9,0,0,0x38,0,
  0,0x81,6,0,'1','2','3','4','5',0,0,0,0,0,0,0,
  8,1,1,0,0,0,0,0,1,1,4,0,2,0,0,0,
  2,1,4,0,1,0,0,0,3,1,4,0,0,0,0,0,4,1,4,0,0,0,0,0};
 assert(sizeof(golden)==72&&!memcmp(p,golden,72));
 assert(x9cw_native_dial(p,72,2)&&x9cw_request(p,72,0x901,2));
 assert(!x9cw_request(p,72,0x901,1));
 assert(!x9cw_dial(q,n,p,71,&measured_route,0));
 assert(!x9cw_dial(q,n,p,sizeof(p),&measured_route,1));
 assert(!x9cw_dial(q,n,p,sizeof(p),NULL,0));
 struct x9cw_dial_profile wrong=measured_route;wrong.domain=5;
 assert(!x9cw_dial(q,n,p,sizeof(p),&wrong,0));
 wrong=measured_route;wrong.sim=0;assert(!x9cw_dial(q,n,p,sizeof(p),&wrong,0));
 /* Preserve already-owned explicit Android CLIR, without interpreting an
  * Apple privacy byte or choosing a vendor/network default. Only108 changes. */
 for(unsigned clir=1;clir<=2;clir++){
  struct x9cw_dial_profile explicit_route=measured_route;explicit_route.clir=clir;
  assert(x9cw_dial(q,n,p,sizeof(p),&explicit_route,0)==72&&p[36]==clir);
  assert(!memcmp(p,golden,36)&&!memcmp(p+37,golden+37,72-37));
  assert(x9cw_native_dial(p,72,2)&&x9cw_request(p,72,0x901,2));
 }
 wrong=measured_route;wrong.clir=3;
 assert(!x9cw_dial(q,n,p,sizeof(p),&wrong,0));
 wrong.clir=0x100;assert(!x9cw_dial(q,n,p,sizeof(p),&wrong,0));
 p[36]=3;assert(!x9cw_native_dial(p,72,2));
 /* QMI's string has no NUL; MIPC adds one. Optional type can be omitted. */
 n=dial_qmi(q,5,false);assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==72);
 n=dial_qmi(q,40);assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==104);
 assert(x9cw_request(p,104,0x901,2)&&p[60]==0);
 n=dial_qmi(q,1);assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==64);
 /* Same-buffer translation reads all private digits before writing header. */
 uint8_t alias[104];n=dial_qmi(alias);assert(x9cw_dial(alias,n,alias,104,&measured_route,0)==72);
 assert(!memcmp(alias,golden,72));
}
static void dial_invalid(){
 uint8_t q[80],p[104];struct x9cw_dial_number d;
 for(unsigned k=0;k<19;k++){dial_qmi(q);assert(!x9cw_qmi_dial(q,k,&d));}
 unsigned n=dial_qmi(q,0);assert(!x9cw_qmi_dial(q,n,&d));
 n=dial_qmi(q,41);assert(!x9cw_qmi_dial(q,n,&d));
 const uint8_t invalid[]={0,'+',',',';','*','#',0xff};
 for(uint8_t c:invalid){n=dial_qmi(q);q[12]=c;assert(!x9cw_qmi_dial(q,n,&d));}
 n=dial_qmi(q);q[0]=2;assert(!x9cw_qmi_dial(q,n,&d));
 n=dial_qmi(q);q[3]=0x22;assert(!x9cw_qmi_dial(q,n,&d));
 n=dial_qmi(q);q[18]=9;assert(!x9cw_qmi_dial(q,n,&d)); // emergency
 n=dial_qmi(q);q[15]=0x11;assert(!x9cw_qmi_dial(q,n,&d)); // CLIR not silently ignored
 n=dial_qmi(q);q[15]=1;assert(!x9cw_qmi_dial(q,n,&d)); // duplicate number
 n=dial_qmi(q);q[16]=4;assert(!x9cw_qmi_dial(q,n,&d)); // truncated TLV
 n=dial_qmi(q);q[n]=0xff;x9cw_put16(q+5,n+1-7);assert(!x9cw_qmi_dial(q,n+1,&d));
 assert(!x9cw_dial(q,n+1,p,sizeof(p),&measured_route,0));
}
static void dial_whitelist(){
 uint8_t q[64],p[112];unsigned n=dial_qmi(q);assert(x9cw_dial(q,n,p,112,&measured_route,0)==72);
 assert(x9_md_mipc_submit(p,72));
 /* Reject each changed byte in header route, tag, value, string or padding. */
 const unsigned offsets[]={8,16,18,20,25,26,31,32,34,36,37,40,42,44,48,50,52,56,58,60,64,66,68};
 for(unsigned at:offsets){p[at]^=0x80;assert(!x9_md_mipc_submit(p,72));p[at]^=0x80;}
 for(unsigned k=0;k<72;k++)assert(!x9cw_native_dial(p,k,2));
 x9cw_scalar(p+72,0x105,1,2);x9cw_put16(p+14,64);assert(!x9_md_mipc_submit(p,80));
 assert(writes==0&&x9_mi_next==1); // invalid attempts did not enqueue
}
static void dial_roundtrip(){
 uint8_t q[64],p[104],cnf[24],out[104];unsigned n=dial_qmi(q);
 assert(x9cw_dial(q,n,p,104,&measured_route,0)==72);
 uint64_t token=x9_md_mipc_submit(p,72);assert(token);
 x9mi_tick();assert(writes==1&&last_length==72&&last_frame[8]==2&&x9cw_u16(last_frame+10)==0x901);
 assert(x9cw_u16(last_frame+12)==token);
 /* Host timeout/cancel cannot release or repeat a sent dial. */
 x9_md_sim_at_cancel(token);uint32_t len,result;
 for(unsigned i=0;i<100;i++){assert(x9_md_mipc_take(token,out,104,&len)==0);x9mi_tick();}
 assert(writes==1&&x9_mi_slots[0].state==2);
 x9cw_header(cnf,24,0x902,2,(unsigned)token);cnf[9]=2;x9cw_scalar(cnf+16,0,0,4);
 x9mi_rx(cnf,9);assert(x9_md_mipc_take(token,out,104,&len)==0);
 x9mi_rx(cnf+9,15);assert(x9_md_mipc_take(token,out,104,&len)==1&&len==24);
 assert(x9cw_dial_cnf(out,len,&result)&&result==0);
 assert(!x9cw_qmi_action(out,len,&n,&result)); // native CNF is not a QMI success/id
 assert(x9_md_mipc_take(token,out,104,&len)==-1);
 x9mi_rx(cnf,24);for(unsigned i=0;i<10;i++)x9mi_tick();assert(writes==1);
}
static void dial_tx_failure(){
 uint8_t q[64],p[104],out[104];unsigned n=dial_qmi(q);uint32_t len;
 assert(x9cw_dial(q,n,p,104,&measured_route,0)==72);
 uint64_t token=x9_md_mipc_submit(p,72);assert(token);tx_result=-1;x9mi_tick();
 assert(writes==1&&x9_mi_slots[0].state==2&&x9_mi_poisoned);
 x9_md_sim_at_cancel(token);x9mi_online(1);
 assert(x9_md_mipc_take(token,out,104,&len)==0);
 for(unsigned i=0;i<100;i++)x9mi_tick();assert(writes==1);
 assert(!x9_md_mipc_submit(p,72));
}
static void dial_cnf_invalid(){
 uint8_t p[40];uint32_t result=0x12345678;
 x9cw_header(p,24,0x902,2,77);x9cw_scalar(p+16,0,0,4);
 for(unsigned n=0;n<24;n++)assert(!x9cw_dial_cnf(p,n,&result));
 assert(result==0x12345678&&x9cw_dial_cnf(p,24,&result)&&result==0);
 x9cw_put32(p+20,42);assert(x9cw_dial_cnf(p,24,&result)&&result==42); // native error remains native
 p[16]=1;assert(!x9cw_dial_cnf(p,24,&result));p[16]=0;
 p[18]=1;assert(!x9cw_dial_cnf(p,24,&result));p[18]=4;
 x9cw_scalar(p+24,0x100,1,1);x9cw_put16(p+14,16);assert(!x9cw_dial_cnf(p,32,&result));
 assert(!x9cw_dial_cnf(p,32,NULL));
}
static void request_whitelist(){
 uint8_t req[]={0,1,0,0x21,0,4,0,1,1,0,7},p[48];
 assert(x9cw_action(req,sizeof(req),p,sizeof(p),7,0x12345678,1,1)==32);
 assert(x9_md_mipc_submit(p,32));
 p[20]=1;assert(!x9_md_mipc_submit(p,32));p[20]=0;
 x9cw_put32(p+28,0);assert(!x9_md_mipc_submit(p,32));x9cw_put32(p+28,1);
 x9cw_scalar(p+32,0x102,16,4);x9cw_put16(p+14,24);assert(!x9_md_mipc_submit(p,40));
 x9cw_header(p,16,0x907,2,0);assert(x9_md_mipc_submit(p,16));
 x9cw_header(p,16,0x901,2,0);assert(!x9_md_mipc_submit(p,16));
 x9cw_header(p,16,0x8902,0x20,0x2345);assert(!x9_md_mipc_submit(p,16));
 x9cw_header(p,16,0x907,3,0);assert(!x9_md_mipc_submit(p,16));
}
static void no_auto_policy(){
 rx_incoming();for(unsigned i=0;i<10;i++)x9mi_tick();assert(!writes);
 uint32_t token=event();assert(x9_md_mipc_call_reply(token,2,0,0)==-1);
 for(unsigned i=0;i<10;i++)x9mi_tick();assert(!writes);
}
static void command_roundtrip(){
 rx_incoming();uint32_t token=event();int result=-1;
 assert(x9_md_mipc_call_reply(token,1,0,0)==1);
 assert(x9_md_mipc_call_reply_take(token,&result)==0);
 assert(x9_md_mipc_call_reply(token,1,0,0)==-1);
 x9mi_tick();assert(writes==1&&last_length==40&&last_frame[8]==2);
 assert(x9cw_u16(last_frame+10)==0x8902&&x9cw_u16(last_frame+12)==0x2345);
 assert(last_frame[20]==1&&x9cw_u32(last_frame+28)==0x12345678&&x9cw_u32(last_frame+36)==0x90abcdef);
 assert(x9_md_mipc_call_reply_take(token,&result)==1&&result==X9_AT_OK);
 assert(x9_md_mipc_call_reply_take(token,&result)==0&&x9_mc_slots[0].state==6);
 for(unsigned i=0;i<10;i++)x9mi_tick();assert(writes==1);
 for(uint8_t b:x9_mc_slots[0].frame)assert(!b);
}
static void failure_retained(){
 rx_incoming();uint32_t token=event();assert(x9_md_mipc_call_reply(token,1,0,0)==1);
 tx_result=-1;x9mi_tick();int result;
 assert(x9_md_mipc_call_reply_take(token,&result)==1&&result==X9_AT_OFFLINE);
 assert(x9_md_mipc_call_reply_take(token,&result)==0&&x9_mc_slots[0].state==6);
 assert(x9_md_mipc_call_reply(token,1,0,0)==-1);
 x9mi_online(1);for(unsigned i=0;i<10;i++)x9mi_tick();assert(writes==1);
}
static void duplicate_pending(){
 rx_incoming();rx_incoming();uint32_t token=event();rx_incoming();
 uint8_t out[64];uint32_t n,t;
 assert(x9_md_mipc_call_event(out,sizeof(out),&n,&t)==0);
 assert(!x9_mc_poisoned&&token==1&&x9_mc_next==1);
}
static void changed_duplicate(){
 rx_incoming();event();uint8_t p[64];incoming(p);p[28]='Y';x9mi_rx(p,sizeof(p));
 assert(x9_mc_poisoned);unchanged_other_inboxes();x9mi_tick();assert(!writes);
}
static void duplicate_replied(){
 command_roundtrip();rx_incoming();assert(x9_mc_poisoned);x9mi_tick();assert(writes==1);
 unchanged_other_inboxes();
}
static void overflow_isolated(){
 /* Valid MD RING event6 has only100/u32. A malformed progress frame
  * would quarantine at the first input, never exercising FIFO capacity. */
 uint8_t p[24];x9cw_header(p,24,0x4902,1,0);x9cw_scalar(p+16,0x100,6,4);
 for(unsigned i=0;i<X9_MC_SLOTS;i++){x9mi_rx(p,24);assert(!x9_mc_poisoned);}
 x9mi_rx(p,24);
 assert(x9_mc_poisoned);unchanged_other_inboxes();
}
static void bounded_tombstones(){
 for(unsigned i=0;i<X9_MC_SLOTS;i++){
  rx_incoming(100+i);uint32_t token=event();int r;
  assert(x9_md_mipc_call_reply(token,1,0,0)==1);x9mi_tick();assert(x9_md_mipc_call_reply_take(token,&r)==1&&!r);
 }
 assert(writes==X9_MC_SLOTS);rx_incoming(999);assert(x9_mc_poisoned);unchanged_other_inboxes();
}
static void fifo(){
 rx_incoming(100);rx_incoming(101);uint32_t first=event(),second=event();
 assert(x9_md_mipc_call_reply(second,1,0,0)==1&&x9_md_mipc_call_reply(first,1,0,0)==1);
 x9mi_tick();assert(writes==1&&x9cw_u16(last_frame+12)==100);
 x9mi_tick();assert(writes==2&&x9cw_u16(last_frame+12)==101);
}
static void fragmented(){
 uint8_t p[64],out[64];incoming(p);uint32_t n,token;
 for(unsigned i=0;i<63;i++){
  x9mi_rx(p+i,1);assert(x9_md_mipc_call_event(out,sizeof(out),&n,&token)==0);
 }
 x9mi_rx(p+63,1);event();assert(!writes);
}
static void late_cnf(){
 uint8_t p[24],out[24];x9cw_header(p,16,0x907,2,0);
 uint64_t token=x9_md_mipc_submit(p,16);assert(token);x9mi_tick();
 uint32_t n;for(unsigned i=0;i<100;i++){
  assert(x9_md_mipc_take(token,out,sizeof(out),&n)==0);x9mi_tick();
 }
 assert(writes==1&&x9_mi_slots[0].state==2);
 x9cw_header(p,24,0x908,0x20,(unsigned)token);x9cw_scalar(p+16,0,0,4);p[9]=7;
 x9mi_rx(p,sizeof(p));assert(x9_md_mipc_take(token,out,sizeof(out),&n)==1&&n==24&&!memcmp(p,out,n));
 assert(x9_md_mipc_take(token,out,sizeof(out),&n)==-1&&writes==1);
}
static void sms_priority(){
 rx_incoming();uint32_t call=event();assert(x9_md_mipc_call_reply(call,1,0,0)==1);
 uint8_t p[24],out[24];x9cw_header(p,24,0x8601,1,321);x9cw_scalar(p+16,0x100,3,1);x9mi_rx(p,24);
 uint32_t n,sms;assert(x9_md_mipc_message_event(out,24,&n,&sms)==1&&sms);
 assert(x9_md_mipc_message_reply(sms,1,0)==1);
 x9mi_tick();assert(writes==1&&x9cw_u16(last_frame+10)==0x8602);
 x9mi_tick();assert(writes==2&&x9cw_u16(last_frame+10)==0x8902);
}
static void empty_baseline();
static void rx_progress(unsigned,unsigned,unsigned);
static void staged_ios(){
 uint8_t qmi[]={0,1,0,0x22,0,4,0,1,1,0,7};
 assert(!x9c_request(1,X9BB_PORT_BASE+5,qmi,sizeof(qmi))&&!qmi_replies);
 assert(x9c_request(1,X9BB_PORT_BASE+9,qmi,sizeof(qmi))&&qmi_replies==1&&qmi_error==94);
 uint8_t dial[64];unsigned n=dial_qmi(dial);
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,dial,n)&&qmi_replies==2&&qmi_error==16);
 x9mi_tick();assert(!writes&&!x9_mi_next&&!x9_mc_next);
}
static void voice20_dispatch(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);dial_owner_fixture();const unsigned before=writes;
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n)&&!qmi_replies);
 assert(x9c_dial_job.used&&x9c_dial_job.transaction==0x1234&&x9c_dial_job.len==n);
 const uint64_t token=x9c_dial_job.token;assert(token);
 uint8_t malformed[]={0,0x34,0x12,0x20,0,8,0,0xff};
 assert(x9c_dial_repeated_wire(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+9,malformed,sizeof(malformed)));
 assert(!x9c_dial_repeated_wire(CBQ_PORT_BASE,X9BB_NODE+1,X9BB_PORT_BASE+9,malformed,sizeof(malformed)));
 assert(!x9c_dial_repeated_wire(CBQ_PORT_BASE+1,X9BB_NODE,X9BB_PORT_BASE+9,malformed,sizeof(malformed)));
 malformed[1]++;assert(!x9c_dial_repeated_wire(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+9,malformed,sizeof(malformed)));
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n)&&!qmi_replies);
 q[10]='9';assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n)&&!qmi_replies);
 q[10]='*';assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n)&&!qmi_replies);
 assert(x9c_dial_job.token==token&&x9c_dial_job.req[10]=='1');
 x9mi_tick();assert(writes==before+1&&last_length==72&&last_frame[20]=='1');
 x9cw_header(p,24,0x902,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);x9mi_rx(p,24);
 rx_progress(130,7,0);x9c_poll();
 assert(qmi_replies==1&&!qmi_error&&qmi_response_len==18&&qmi_response[17]==7);
 q[10]='*';assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n)&&qmi_replies==1);
 q[10]='1';x9cw_put16(q+1,0x1235);
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n)&&qmi_replies==2&&qmi_error==X9Q_BUSY);
}
/* Full synthetic MD frames, never padded/reconstructed Android DUMP tails.
 * These cases have been added for future authorized execution; compilation
 * alone does NOT validate the new parser or lifecycle. */
static unsigned progress_frame(uint8_t *p,unsigned event,unsigned id=7,unsigned mode=20,unsigned mask=2){
 x9cw_header(p,512,0x4901,mask,0);unsigned at=16;
 const unsigned tags[]={0x100,0x101,0x102,0x103},values[]={id,0,mode,129};
 for(unsigned i=0;i<4;i++,at+=8)x9cw_scalar(p+at,tags[i],values[i],4);
 x9cw_put16(p+at,0x8104);x9cw_put16(p+at+2,1);at+=8;
 x9cw_scalar(p+at,0x105,0x100000,4);at+=8;
 x9cw_put16(p+at,0x8106);x9cw_put16(p+at+2,408);p[at+4]=0xff;at+=416;
 x9cw_scalar(p+at,0x107,0,4);at+=8;x9cw_scalar(p+at,0x109,event,4);at+=8;
 x9cw_scalar(p+at,0x10a,16,4);at+=8;
 x9cw_put16(p+at,0x810b);x9cw_put16(p+at+2,1);at+=8;
 assert(at==512);return at;
}
static void rx_progress(unsigned event,unsigned id=7,unsigned mode=20){
 uint8_t p[512];progress_frame(p,event,id,mode);x9mi_rx(p,sizeof(p));
}
static void progress_framing(){
 uint8_t p[520];struct x9cw_progress c;progress_frame(p,130,7,0);
 assert(x9cw_progress(p,512,&c)&&c.valid&&c.id==7&&c.event==130&&c.mask==2&&c.has_cause&&c.cause==16);
 for(unsigned n=0;n<512;n++)assert(!x9cw_progress(p,n,&c));
 p[50]=2;assert(!x9cw_progress(p,512,&c));progress_frame(p,130,7,0);
 x9cw_scalar(p+512,0x8100,1,4);x9cw_put16(p+14,504);assert(!x9cw_progress(p,520,&c));
}
static void outbound_states(){
 struct x9cw_outbound_state s={};struct x9cw_progress c={};unsigned id;
 assert(!x9cw_outbound_id(&s,&id)&&!s.count_known);
 c.id=7;c.mask=2;c.event=130;assert(x9cw_outbound_apply(&s,&c)==1&&s.ios_state==1&&s.epoch==1);
 c.mode=20;c.event=2;assert(x9cw_outbound_apply(&s,&c)==1&&s.ios_state==5);
 c.event=132;assert(x9cw_outbound_apply(&s,&c)==1&&s.ios_state==3);
 c.event=6;assert(x9cw_outbound_apply(&s,&c)==1&&x9cw_outbound_apply(&s,&c)==1);
 assert(x9cw_outbound_id(&s,&id)&&id==7);
 c.event=133;assert(x9cw_outbound_apply(&s,&c)==1&&s.ended&&s.ios_state==9&&!s.count_known);
 c.event=6;assert(x9cw_outbound_apply(&s,&c)==0&&s.ended&&!x9cw_outbound_id(&s,&id));
 c.event=130;c.mode=0;assert(x9cw_outbound_apply(&s,&c)==1&&s.epoch==2);
 c.id=8;assert(x9cw_outbound_apply(&s,&c)==-1&&s.blocked);
 s={};c.id=256;assert(x9cw_outbound_apply(&s,&c)==1&&!x9cw_outbound_id(&s,&id));
 s={};c.mask=0x20;assert(x9cw_outbound_apply(&s,&c)==0&&!s.seen); // no CMD mask normalization
}
/* Complete artificial producer frames, not reconstructed logger prefixes.
 * Removing optional10a changes the declared body and moves the final TLV. */
static unsigned reason_frame(uint8_t *p,unsigned event,unsigned direction=0,
                             unsigned cause=16,bool has_cause=true,unsigned id=7,unsigned mode=20){
 unsigned n=progress_frame(p,event,id,mode);x9cw_scalar(p+24,0x101,direction,4);
 x9cw_scalar(p+496,0x10a,cause,4);
 if(!has_cause){memmove(p+496,p+504,8);n-=8;x9cw_put16(p+14,n-16);}
 return n;
}
static void end_reason_provenance(){
 uint8_t p[512];struct x9cw_progress c={};
 unsigned n=reason_frame(p,133);assert(!x9cw_progress(p,n-1,&c)&&!c.valid);
 assert(!x9cw_end_reason_metadata(&c).known&&!x9cw_end_reason_metadata(nullptr).known);
 assert(x9cw_progress(p,n,&c)&&c.valid);
 auto r=x9cw_end_reason_metadata(&c);assert(r.known&&r.apple==145);
 struct x9cw_progress retained=c;
 assert(!x9cw_progress(p,n-1,&c)&&!memcmp(&c,&retained,sizeof(c))); // failed parse does not grant new provenance
 for(unsigned event:{0u,2u,6u,130u,132u}){
  n=reason_frame(p,event);assert(x9cw_progress(p,n,&c)&&c.valid&&!x9cw_end_reason_metadata(&c).known);
 }
 n=reason_frame(p,133,0,17);assert(x9cw_progress(p,n,&c)&&!x9cw_end_reason_metadata(&c).known);
 n=reason_frame(p,133,0,0,false);assert(x9cw_progress(p,n,&c)&&!c.has_cause&&!x9cw_end_reason_metadata(&c).known);
 n=reason_frame(p,133,0,16,true,7,0);assert(x9cw_progress(p,n,&c)&&!x9cw_end_reason_metadata(&c).known);
 c={};c.event=133;c.mode=20;c.has_cause=1;c.cause=16;
 assert(!x9cw_end_reason_metadata(&c).known); // a manually constructed object is not a parsed frame
}
static void outbound_end_reason(){
 uint8_t p[512];struct x9cw_progress c;struct x9cw_outbound_state s;
 auto begin=[&](){s={};unsigned n=reason_frame(p,130,0,16,true,7,0);
  assert(x9cw_progress(p,n,&c)&&x9cw_outbound_apply(&s,&c)==1&&!s.end_reason.known);};
 for(unsigned which=0;which<4;which++){
  begin();unsigned n=reason_frame(p,133,0,which==1?17:16,which!=2);
  assert(x9cw_progress(p,n,&c));if(which==3)c.valid=0;
  assert(x9cw_outbound_apply(&s,&c)==1&&s.ended&&s.ios_state==9);
  assert(s.end_reason.known==(which==0)&&s.end_reason.apple==(which==0?145:0));
  auto reason=s.end_reason;uint64_t rev=s.revision;
  for(unsigned cause:{17u,16u}){
   n=reason_frame(p,133,0,cause);assert(x9cw_progress(p,n,&c));
   assert(x9cw_outbound_apply(&s,&c)==0&&s.revision==rev&&s.end_reason.known==reason.known&&s.end_reason.apple==reason.apple);
  }
  n=reason_frame(p,130,0,16,true,8,0);assert(x9cw_progress(p,n,&c));
  assert(x9cw_outbound_apply(&s,&c)==1&&s.epoch==2&&!s.end_reason.known&&!s.end_reason.apple);
 }
 for(unsigned which=0;which<5;which++){
  begin();unsigned n=reason_frame(p,133);assert(x9cw_progress(p,n,&c));
  if(which==0)c.id=8;else if(which==1)c.direction=1;else if(which==2)c.mode=22;
  else if(which==3)c.mask=1;else s.revision=UINT64_MAX;
  int result=x9cw_outbound_apply(&s,&c);
  assert(result==(which==3?0:-1)&&!s.end_reason.known&&!s.end_reason.apple);
 }
 {
  begin();
  unsigned n=reason_frame(p,129,0,0,true,7,0);assert(x9cw_progress(p,n,&c));
  assert(x9cw_outbound_apply(&s,&c)==0&&!s.blocked&&!s.ended&&s.ios_state==1);
  n=reason_frame(p,133,0,2174,true,7,0);assert(x9cw_progress(p,n,&c));
  assert(x9cw_outbound_apply(&s,&c)==1&&!s.blocked&&s.ended&&s.ios_state==9&&!s.end_reason.known);
 }
}
static void incoming_end_reason(){
 uint8_t p[512];struct x9cw_progress c;struct x9cw_incoming cmd;
 incoming(p);assert(x9cw_incoming(p,64,&cmd));cmd.call_id=7;
 struct x9cw_inbound_state s;
 auto begin=[&](){s={};assert(x9cw_inbound_begin(&s,&cmd)==1);
  unsigned n=reason_frame(p,0,1);assert(x9cw_progress(p,n,&c));
  assert(x9cw_inbound_apply(&s,&c)==1&&!s.end_reason.known);};
 for(unsigned which=0;which<4;which++){
  begin();unsigned n=reason_frame(p,133,1,which==1?17:16,which!=2);
  assert(x9cw_progress(p,n,&c));if(which==3)c.valid=0;
  assert(x9cw_inbound_apply(&s,&c)==1&&s.ended&&s.ios_state==9);
  assert(s.end_reason.known==(which==0)&&s.end_reason.apple==(which==0?145:0));
  auto reason=s.end_reason;uint64_t rev=s.revision;
  for(unsigned cause:{17u,16u}){
   n=reason_frame(p,133,1,cause);assert(x9cw_progress(p,n,&c));
   assert(x9cw_inbound_apply(&s,&c)==0&&s.revision==rev&&s.end_reason.known==reason.known&&s.end_reason.apple==reason.apple);
  }
  cmd.sequence++;assert(x9cw_inbound_begin(&s,&cmd)==1&&s.pending&&!s.end_reason.known&&!s.end_reason.apple);
 }
 for(unsigned which=0;which<5;which++){
  begin();unsigned n=reason_frame(p,133,1);assert(x9cw_progress(p,n,&c));
  if(which==0)c.id=8;else if(which==1)c.direction=0;else if(which==2)c.mode=0;
  else if(which==3)c.mask=1;else s.revision=UINT64_MAX;
  int result=x9cw_inbound_apply(&s,&c);
  assert(result==((which==1||which==3)?0:-1)&&!s.end_reason.known&&!s.end_reason.apple);
 }
 // Complete END cannot attach a cause to an MT that has not observed ECPI0.
 s={};assert(x9cw_inbound_begin(&s,&cmd)==1);unsigned n=reason_frame(p,133,1);
 assert(x9cw_progress(p,n,&c)&&x9cw_inbound_apply(&s,&c)==-1&&!s.end_reason.known);
}
static unsigned current_frame(uint8_t *p,unsigned count,unsigned result=0,
                              unsigned mode=20,unsigned state=0){
 assert(count<=1);x9cw_header(p,704,0x90a,2,0);unsigned at=32;
 x9cw_scalar(p+16,0,result,4);x9cw_scalar(p+24,0x100,count,4);
 const unsigned tags[]={0x10b,0x10c,0x10d,0x10e,0x10f,0x110,0x8111,0x112,0x113,0x8114,0x115};
 const uint32_t words[]={7,0,mode,state,0,129,0,0,0x100000,0,0};
 if(count)for(unsigned j=0;j<11;j++){
  unsigned base=(0x7000-j*8)|(tags[j]&0x8000);
  x9cw_put16(p+at,tags[j]);x9cw_put16(p+at+12,base);x9cw_put16(p+at+14,count);at+=16;
  if(j==6){x9cw_put16(p+at,base);x9cw_put16(p+at+2,1);at+=8;}
  else if(j==9){x9cw_put16(p+at,base);x9cw_put16(p+at+2,408);p[at+4]=0xff;at+=416;}
  else{x9cw_scalar(p+at,base,words[j],4);at+=8;}
 }
 x9cw_put16(p+14,at-16);return at;
}
static void current_arrays(){
 uint8_t p[712];struct x9cw_current c;unsigned n=current_frame(p,1);
 assert(n==704&&x9cw_current(p,n,&c)&&c.count==1&&c.calls[0].id==7&&c.calls[0].mode==20);
 for(unsigned k=0;k<n;k++)assert(!x9cw_current(p,k,&c));
 p[44]^=0x80;assert(!x9cw_current(p,n,&c));n=current_frame(p,1);
 p[46]=2;assert(!x9cw_current(p,n,&c));n=current_frame(p,1);
 p[36]=1;assert(!x9cw_current(p,n,&c));n=current_frame(p,1);
 x9cw_scalar(p+n,0x7001,1,4);x9cw_put16(p+14,n-8);assert(!x9cw_current(p,n+8,&c));
 n=current_frame(p,0);assert(n==32&&x9cw_current(p,n,&c)&&!c.count&&!c.result);
 n=current_frame(p,0,42);assert(x9cw_current(p,n,&c)&&c.result==42);
}
static void current_number_owner(){
 uint8_t p[704],out[704];struct x9cw_current c;unsigned n=current_frame(p,1);
 struct x9cw_number missing={};unsigned untouched=99;
 assert(!x9cw_number_presentation(&missing,&untouched)&&untouched==99);
 const uint8_t *descriptor,*number,*detail;unsigned k;
 assert(x9cw_tlv(p,n,0x8111,&descriptor,&k)&&descriptor&&!k);
 assert(x9cw_tlv(p,n,x9cw_u16(descriptor+8),&number,&k)&&number&&k==1);
 unsigned at=(unsigned)(number-p);x9cw_put16(p+at-2,4);memcpy(p+at,"123",4);
 assert(x9cw_tlv(p,n,0x8114,&descriptor,&k)&&descriptor&&!k);
 assert(x9cw_tlv(p,n,x9cw_u16(descriptor+8),&detail,&k)&&detail&&k==408);
 p[detail-p]=1;assert(x9cw_current(p,n,&c));
 assert(c.calls[0].number.length==3&&!memcmp(c.calls[0].number.value,"123",4));
 unsigned presentation=99;assert(x9cw_number_presentation(&c.calls[0].number,&presentation)&&presentation==1);
 bzero(p,sizeof(p));assert(!memcmp(c.calls[0].number.value,"123",4)); // no borrowed RX bytes
 for(unsigned raw=0;raw<256;raw++){
  c.calls[0].number.presentation=(uint8_t)raw;presentation=99;
  int known=x9cw_number_presentation(&c.calls[0].number,&presentation);
  assert(known==(raw<=4));assert(presentation==(raw<=4?(raw<2?raw:2):99));
 }
 // The production owned-current path, not an unsolicited or stale query.
 x9cw_header(p,16,0x909,2,0);uint64_t token=x9_md_mipc_submit(p,16);assert(token);x9mi_tick();
 n=current_frame(p,1);x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);
 uint32_t got;assert(x9_md_mipc_take(token,out,sizeof(out),&got)==1);
 assert(x9_mc_current.count==1&&x9_mc_current.calls[0].number.presentation==255);
 assert(!x9cw_number_presentation(&x9_mc_current.calls[0].number,&presentation));
}
static void progress_number_owner(){
 uint8_t p[512];struct x9cw_progress c;progress_frame(p,130,7,0);
 x9cw_put16(p+50,4);memcpy(p+52,"456",4);p[68]=2; //8106/detail_info[0], not TYPE105
 assert(x9cw_progress(p,sizeof(p),&c)&&c.number.length==3&&!memcmp(c.number.value,"456",4));
 unsigned presentation;assert(x9cw_number_presentation(&c.number,&presentation)&&presentation==2);
 bzero(p,sizeof(p));assert(!memcmp(c.number.value,"456",4));
 progress_frame(p,130,7,0);p[50]=5; // would consume next TLV: malformed whole frame
 assert(!x9cw_progress(p,sizeof(p),&c));assert(!memcmp(c.number.value,"456",4));
}
/* Raw-source owner only. These artificial cases are compiled, not executed;
 * they neither normalize PAU/TOA nor publish a caller identity. */
static void identity_progress(uint8_t *p,unsigned event,unsigned id,unsigned mode,
                              unsigned direction,const char *number,unsigned presentation){
 progress_frame(p,event,id,mode);x9cw_scalar(p+24,0x101,direction,4);
 x9cw_put16(p+50,4);memcpy(p+52,number,4);
 p[68]=(uint8_t)presentation;p[69]=0xa5;p[475]=0x5a;
 x9cw_put16(p+506,4);memcpy(p+508,"pau",4);
}
static void progress_identity_mo(){
 uint8_t p[512];identity_progress(p,130,7,0,0,"123",0);x9mi_rx(p,sizeof(p));
 bzero(p,sizeof(p));x9c_poll();
 const struct x9cw_progress_identity &a=x9c_outbound.identity;
 assert(x9c_observed&&x9c_outbound.epoch==1&&a.known&&a.toa==129&&a.number.valid);
 assert(a.number.length==3&&!memcmp(a.number.value,"123",4));
 assert(a.detail[0]==0&&a.detail[1]==0xa5&&a.detail[407]==0x5a);
 assert(a.pau_length==3&&!memcmp(a.pau,"pau",4)&&!a.pau[511]);
 identity_progress(p,132,7,20,0,"456",1);x9mi_rx(p,sizeof(p));x9c_poll();
 assert(x9c_outbound.epoch==1&&!memcmp(a.number.value,"456",4)&&a.number.presentation==1);
 // Host deletion retires host publication, not the native call epoch owner.
 x9c_del(0);x9q_clients[0].generation++;x9c_poll();
 assert(a.known&&!memcmp(a.number.value,"456",4)&&!qmi_replies&&!writes);
 identity_progress(p,133,7,20,0,"789",2);x9mi_rx(p,sizeof(p));x9c_poll();
 uint64_t rev=x9c_outbound.revision;
 identity_progress(p,6,7,20,0,"999",0);x9mi_rx(p,sizeof(p));x9c_poll();
 assert(x9c_outbound.ended&&x9c_outbound.revision==rev&&!memcmp(a.number.value,"789",4)&&a.number.presentation==2);
 identity_progress(p,130,8,0,0,"234",4);x9mi_rx(p,sizeof(p));x9c_poll();
 assert(x9c_outbound.epoch==2&&x9c_outbound.native_id==8&&!memcmp(a.number.value,"234",4)&&a.number.presentation==4);
}
static void progress_identity_mt(){
 uint8_t p[512];incoming(p);x9cw_scalar(p+16,0x100,7,4);x9mi_rx(p,64);x9c_poll();
 assert(x9c_inbound.pending&&!x9c_inbound.identity.known);
 identity_progress(p,0,7,20,1,"456",1);x9mi_rx(p,sizeof(p));bzero(p,sizeof(p));x9c_poll();
 const struct x9cw_progress_identity &a=x9c_inbound.identity;
 assert(x9c_inbound.epoch==1&&a.known&&a.number.length==3&&!memcmp(a.number.value,"456",4));
 assert(a.toa==129&&a.detail[1]==0xa5&&a.pau_length==3&&!memcmp(a.pau,"pau",4));
 assert(!x9c_outbound.identity.known&&!x9c_outbound.seen&&!writes&&!qmi_replies);
 identity_progress(p,133,7,20,1,"789",2);x9mi_rx(p,sizeof(p));x9c_poll();
 uint64_t rev=x9c_inbound.revision;
 identity_progress(p,0,7,20,1,"999",0);x9mi_rx(p,sizeof(p));x9c_poll();
 assert(x9c_inbound.ended&&x9c_inbound.revision==rev&&!memcmp(a.number.value,"789",4));
 incoming(p,0x22,0x2346);x9cw_scalar(p+16,0x100,8,4);x9cw_scalar(p+40,0x103,101,4);
 x9mi_rx(p,64);x9c_poll();
 assert(x9c_inbound.pending&&x9c_inbound.native_id==8&&!a.known&&!a.number.valid&&!a.pau_length);
 for(uint8_t byte:a.number.value)assert(!byte);
}
static void progress_identity_provenance(){
 uint8_t p[512];struct x9cw_progress c;struct x9cw_outbound_state s={};
 identity_progress(p,130,7,0,0,"123",0);assert(x9cw_progress(p,sizeof(p),&c));
 assert(x9cw_outbound_apply(&s,&c)==1&&s.identity.known);
 struct x9cw_progress_identity saved=s.identity;
 c.mask=1;c.number.value[0]='9';assert(!x9cw_outbound_apply(&s,&c));
 assert(!memcmp(&saved,&s.identity,sizeof(saved)));
 c.mask=2;c.id=8;assert(x9cw_outbound_apply(&s,&c)<0);
 assert(!memcmp(&saved,&s.identity,sizeof(saved)));
 s={};identity_progress(p,130,7,0,0,"123",0);assert(x9cw_progress(p,sizeof(p),&c));
 assert(x9cw_outbound_apply(&s,&c)==1);c.event=132;c.mode=20;c.valid=0;
 assert(x9cw_outbound_apply(&s,&c)==1&&!s.identity.known&&!s.identity.number.valid);
 // Known empty8104 is a new raw snapshot, never a merged display fallback.
 identity_progress(p,6,7,20,0,"456",1);x9cw_put16(p+50,1);bzero(p+52,4);
 assert(x9cw_progress(p,sizeof(p),&c)&&x9cw_outbound_apply(&s,&c)==1);
 assert(s.identity.known&&!s.identity.number.length&&s.identity.number.presentation==1);
 unsigned presentation=99;assert(x9cw_number_presentation(&s.identity.number,&presentation)&&presentation==1);
 c.event=133;s.revision=UINT64_MAX;saved=s.identity;
 assert(x9cw_outbound_apply(&s,&c)<0&&!memcmp(&saved,&s.identity,sizeof(saved)));
}
static void empty_baseline(){
 uint8_t p[704],out[704];x9cw_header(p,16,0x909,2,0);
 uint64_t t=x9_md_mipc_submit(p,16);assert(t);x9mi_tick();
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)t);x9mi_rx(p,n);
 uint32_t k;assert(x9_md_mipc_take(t,out,sizeof(out),&k)==1&&k==32);
 struct x9cw_outbound_state s;assert(x9_md_mipc_call_snapshot(&s)==1&&s.count_known&&!s.native_count);
}
static void status_fence(){
 uint8_t p[704],out[704];x9cw_header(p,16,0x909,2,0);
 uint64_t t=x9_md_mipc_submit(p,16);assert(t);x9mi_tick();rx_progress(130,7,0);
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)t);x9mi_rx(p,n);
 uint32_t k;assert(x9_md_mipc_take(t,out,sizeof(out),&k)==1);
 assert(!x9_mc_outbound.count_known&&!x9_mc_outbound.blocked&&x9_mc_outbound.seen);
}
static void dial_join_order(){
 uint8_t q[64],p[24],r[18];unsigned n=dial_qmi(q);struct x9cw_outbound_state s={};
 struct x9cw_dial_join j={};assert(!x9cw_dial_join_begin(&j,&s,&measured_route));
 s.count_known=1;assert(x9cw_dial_join_begin(&j,&s,&measured_route)&&x9cw_dial_join_sent(&j,77));
 x9cw_header(p,24,0x902,2,76);x9cw_scalar(p+16,0,0,4);
 assert(!x9cw_dial_join_cnf(&j,p,24)&&!j.confirmed);
 x9cw_put16(p+12,77);assert(x9cw_dial_join_cnf(&j,p,24));
 assert(!x9cw_dial_join_response(&j,&s,q,n,r,18)); // CNF alone is not success
 struct x9cw_progress c={};c.id=7;c.mask=2;c.event=130;assert(x9cw_outbound_apply(&s,&c)==1);
 assert(x9cw_dial_join_response(&j,&s,q,n,r,18)==18&&r[0]==2&&r[14]==0x10&&r[17]==7);
 assert(x9cw_u16(r+1)==x9cw_u16(q+1)&&x9cw_u16(r+3)==0x20&&x9cw_u16(r+5)==11);
}
static void ios_join(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 dial_owner_fixture();
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;x9mi_tick();
 assert(!x9_md_mipc_submit(last_frame,last_length)); // staging lease blocks a second mutation
 x9cw_header(p,24,0x902,2,(unsigned)t);x9cw_scalar(p+16,0,0,4);x9mi_rx(p,24);x9c_poll();
 assert(!qmi_replies&&x9c_dial_job.used);rx_progress(130,7,0);x9c_poll();
 assert(qmi_replies==1&&qmi_response_len==18&&qmi_response[17]==7&&x9c_dial_job.used);
 x9c_poll();assert(qmi_replies==1&&!x9c_dial_submit(0,q,n,measured_route));
}
static void ios_timeout(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 dial_owner_fixture();
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;x9mi_tick();
 fake_ms=60001;x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&x9c_dial_job.token==t);
 x9cw_header(p,24,0x902,2,(unsigned)t);x9cw_scalar(p+16,0,0,4);x9mi_rx(p,24);
 rx_progress(130,7,0);x9c_poll();assert(qmi_replies==1&&!x9c_dial_job.token&&x9c_dial_job.held);
}
static void ios_deleted(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 dial_owner_fixture();
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;x9mi_tick();
 x9c_del(0);x9q_clients[0].generation++;x9cw_header(p,24,0x902,2,(unsigned)t);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);rx_progress(130,7,0);x9c_poll();assert(!qmi_replies&&!x9c_dial_job.token&&x9c_dial_job.used);
}
static void dial_presend_fence(){
 empty_baseline();uint8_t q[64],out[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 dial_owner_fixture();
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;unsigned before=writes;
 rx_progress(130,7,0);x9mi_tick();assert(writes==before);uint32_t k;
 assert(x9_md_mipc_take(t,out,sizeof(out),&k)==-3&&!k);
 assert(x9_md_mipc_take(t,out,sizeof(out),&k)==-1&&!x9_mc_dial_token);
}
/* A rejected, never-selected DIAL has no wire-side mutation to retire. */
static void dial_presend_release(){
 empty_baseline();uint8_t q[64];unsigned n=dial_qmi(q);dial_owner_fixture();
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t token=x9c_dial_job.token;unsigned before=writes;
 rx_progress(130,7,0);x9mi_tick();assert(writes==before);
 assert(!x9_mc_dial_token&&token);call_batch_accept=false;x9c_poll();
 assert(!x9c_dial_job.used&&!qmi_replies&&writes==before);
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,n);assert(!qmi_replies&&writes==before);
 call_batch_accept=true;x9c_response_poll();
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&writes==before);
}
static void dial_host_guard(unsigned phase,bool expire){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);dial_owner_fixture();
 const unsigned before=writes;const uint64_t next=x9_mi_next;
 auto invalidate=[](){call_bridge_generation[0]=0;};
 auto timeout=[](){fake_ms=60000;};
 if(phase==0)during_bb_guard=expire?timeout:invalidate;
 bool admitted=x9c_dial_submit(0,q,n,measured_route);
 if(!phase){assert(!admitted&&x9_mi_next==next&&!x9_mc_dial_token&&writes==before);return;}
 assert(admitted);uint64_t token=x9c_dial_job.token;
 if(phase==1){during_bb_guard=expire?timeout:invalidate;x9mi_tick();
  assert(writes==before&&!x9_mc_dial_token);x9c_poll();assert(!x9c_dial_job.used);return;}
 x9mi_tick();assert(writes==before+1&&x9_mc_dial_token==token);
 if(expire)timeout();else invalidate();
 x9c_poll();x9cw_header(p,24,0x902,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(writes==before+1&&x9_mc_dial_token==token&&x9c_dial_job.used);
}
static void dial_guard_admission_release(){dial_host_guard(0,false);}
static void dial_guard_admission_timeout(){dial_host_guard(0,true);}
static void dial_guard_writer_release(){dial_host_guard(1,false);}
static void dial_guard_writer_timeout(){dial_host_guard(1,true);}
static void dial_guard_selected_release(){dial_host_guard(2,false);}
static void dial_guard_selected_timeout(){dial_host_guard(2,true);}
/* Manual vectors compiled with the real production adapter/MD writer.
 * No device identifiers or fixtures; execution is not part of the build. */
static void voice_bind_codec(){
 uint8_t q[14]={0,77,0,0x44,0,4,0,1,1,0,1};unsigned sim=9;
 assert(x9cw_qmi_bind(q,11,&sim)&&sim==1);
 q[10]=0;assert(x9cw_qmi_bind(q,11,&sim)&&!sim);
 q[10]=2;sim=9;assert(!x9cw_qmi_bind(q,11,&sim)&&sim==9);
 q[10]=1;q[8]=4;q[5]=7;assert(!x9cw_qmi_bind(q,14,&sim)); // not u32 NAS/DMS enum
}
static void bind_voice(){
 uint8_t q[]={0,77,0,0x44,0,4,0,1,1,0,1};
 const unsigned before=qmi_replies;
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q))&&qmi_replies==before);
 assert(!x9c_clients[0].bound&&x9c_bind_pending(0,true));
 x9c_response_poll();assert(qmi_replies==before+1&&!qmi_error&&!x9c_bind_pending(0));
 assert(x9c_clients[0].bound&&x9c_clients[0].sim==1&&!x9q_clients[0].sim_slot&&!writes);
 qmi_replies=0;call_allocs=call_frees=0;
}
static void end_request(){
 uint8_t q[]={0,78,0,0x21,0,4,0,1,1,0,7};
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q))&&x9c_end_job.stage==1);
 assert(!qmi_replies);
}
static void end_query_cnf(unsigned mode=20,unsigned state=0){
 uint8_t p[704];unsigned n=current_frame(p,1,0,mode,state);x9cw_put16(p+12,(unsigned)x9c_end_job.token);
 x9mi_rx(p,n);x9c_poll();assert(x9c_end_job.stage==2&&x9c_end_job.token&&x9_mc_end_token);
}
static void end_target_codec(){
 uint8_t p[704];struct x9cw_current c;current_frame(p,1);assert(x9cw_current(p,704,&c));
 struct x9cw_outbound_state s={};struct x9cw_progress e={};struct x9cw_end_target a,b;
 e.mask=2;e.id=7;e.event=130;assert(x9cw_outbound_apply(&s,&e)==1);
 e.event=132;e.mode=20;assert(x9cw_outbound_apply(&s,&e)==1);
 assert(!x9cw_end_target(&s,&c,&a));s.count_known=1;s.native_count=1;
 assert(x9cw_end_target(&s,&c,&a)&&a.native_id==7&&a.sim==1&&a.epoch==1);
 b=a;b.revision++;assert(!x9cw_end_matches(&a,&b));
 c.calls[0].flags|=1;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].flags&=~1u;
 c.calls[0].number_type=2;assert(x9cw_end_target(&s,&c,&b)); // NUMBER is not a multiparty flag
 c.calls[0].id=8;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].id=7;
 c.calls[0].direction=1;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].direction=0;
 c.calls[0].state=3;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].state=0;
 s.ended=1;assert(!x9cw_end_target(&s,&c,&b));
 uint8_t r[24];uint32_t result=99;x9cw_header(r,24,0x906,2,41);x9cw_scalar(r+16,0,0,4);
 assert(x9cw_end_cnf(r,24,41,&result)&&!result);
 assert(!x9cw_end_cnf(r,24,40,&result));r[10]=8;assert(!x9cw_end_cnf(r,24,41,&result));
}
static void current_metadata(){
 uint8_t p[704];struct x9cw_current c;unsigned n=current_frame(p,1);
 /* Change the three actual array element values, keeping descriptors,
  * ID and state unchanged. Each remains a u32 in the MIPC wire. */
 for(unsigned at=16;at<n;){
  unsigned tag=x9cw_u16(p+at),len=x9cw_u16(p+at+2);
  if(tag==0x6fe0)x9cw_put32(p+at+4,2);       //10f NUMBER_TYPE
  if(tag==0x6fc8)x9cw_put32(p+at+4,6);       //112 CALL_RAT
  if(tag==0x6fc0)x9cw_put32(p+at+4,0x100001);//113 TYPE: high bits retained and multiparty
  at+=len?((len+11)&~7u):16;
 }
 assert(x9cw_current(p,n,&c)&&c.calls[0].number_type==2&&c.calls[0].rat==6&&c.calls[0].flags==0x100001);
 struct x9cw_outbound_state s={};struct x9cw_progress e={};struct x9cw_end_target target;
 e.mask=2;e.id=7;e.event=130;assert(x9cw_outbound_apply(&s,&e)==1);
 e.event=132;e.mode=20;assert(x9cw_outbound_apply(&s,&e)==1);s.count_known=1;s.native_count=1;
 assert(!x9cw_end_target(&s,&c,&target));c.calls[0].number_type=0;
 assert(!x9cw_end_target(&s,&c,&target)); // NONE cannot authorize a multiparty end
 c.calls[0].flags&=~1u;c.calls[0].number_type=2;assert(x9cw_end_target(&s,&c,&target));
}
static void ios_end_roundtrip(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();
 assert(writes==1&&x9cw_u16(last_frame+10)==0x909);end_query_cnf();
 uint64_t token=x9c_end_job.token;x9mi_tick();assert(writes==2&&last_length==32&&
  x9cw_u16(last_frame+10)==0x905&&x9cw_u32(last_frame+20)==0&&x9cw_u32(last_frame+28)==7);
 rx_progress(133);x9c_poll();assert(!qmi_replies&&x9c_end_job.stage==2);
 uint8_t p[704];x9cw_header(p,24,0x906,0x22,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(qmi_replies==1&&!qmi_error&&x9c_end_job.stage==4&&x9_mc_end_token==token);
 token=x9c_end_job.token;x9mi_tick();assert(writes==3&&x9cw_u16(last_frame+10)==0x909);
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);x9c_poll();
 assert(qmi_replies==1&&!x9c_end_job.used&&!x9_mc_end_token&&x9_mc_outbound.ended&&x9_mc_outbound.count_known);
}
static void ios_end_all_roundtrip(){
 bind_voice();rx_progress(130,7,0);
 x9c_clients[0].calllist_valid=true;x9c_clients[0].calllist_epoch=x9_mc_outbound.epoch;
 x9c_clients[0].calllist_id=7;
 uint8_t q[]={0,78,0,0x31,0,4,0,1,1,0,8};
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q))&&x9c_end_job.stage==1);
 uint64_t query=x9c_end_job.token;
 q[5]=0;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,7);
 assert(x9c_end_job.token==query&&!qmi_replies);
 x9mi_tick();end_query_cnf(0,2);uint64_t token=x9c_end_job.token;x9mi_tick();
 assert(writes==2&&x9cw_u16(last_frame+10)==0x905&&x9cw_u32(last_frame+20)==0&&x9cw_u32(last_frame+28)==7);
 uint8_t p[24];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,sizeof(p));x9c_poll();assert(qmi_replies==1&&!qmi_error&&qmi_response_len==14&&x9cw_u16(qmi_response+3)==0x31);
}
static void ios_end_all_reject(){
 bind_voice();uint8_t q[]={0,78,0,0x31,0,4,0,1,1,0,8};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!writes&&!x9c_end_job.used);
 q[1]++;q[10]=1;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_replies==2&&qmi_error==X9Q_UNSUPPORTED&&!writes);
 rx_progress(130,7,0);rx_progress(132);
 q[1]++;q[10]=8;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_replies==3&&qmi_error==X9Q_NOT_READY&&!writes&&!x9c_end_job.used);
}
static void ios_end_all_pending_incoming(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);
 x9c_clients[0].calllist_valid=true;x9c_clients[0].calllist_epoch=x9_mc_outbound.epoch;
 x9c_clients[0].calllist_id=7;
 uint8_t q[]={0,78,0,0x31,0,4,0,1,1,0,8};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 x9mi_tick();end_query_cnf();x9_mc_inbound.pending=1;x9mi_tick();x9c_poll();
 assert(writes==1&&qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9_mc_end_token);
}
static void ios_end_stale_query(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();uint64_t token=x9c_end_job.token;x9mi_tick();
 rx_progress(6);uint8_t p[704];unsigned n=current_frame(p,1);x9cw_put16(p+12,(unsigned)token);
 x9mi_rx(p,n);x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9c_end_job.used&&!x9_mc_end_token);
 x9mi_tick();assert(writes==1); // stale query cannot authorize a hangup
}
static void ios_end_presend(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 rx_progress(6);x9mi_tick();x9c_poll();assert(writes==1&&!x9c_end_job.used&&!x9_mc_end_token&&
  qmi_replies==1&&qmi_error==X9Q_NOT_READY);
}
static void end_release_during_md_lock(){call_bridge_generation[0]=0;}
static void end_expire_during_md_lock(){fake_ms=x9c_end_job.deadline;}
/* Compile-only source vectors: model loss while acquiring take (first),
 * end_snapshot (second), or end_submit (third MD lock), without DEL dispatch.
 * Separate vectors below cover the native admission/selection BB guard;
 * none of these source assertions has been executed in this change. */
static void ios_end_postwait(unsigned wait,bool expire){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();
 uint8_t p[704],out[X9_MIPC_FRAME_MAX];unsigned n=current_frame(p,1);
 x9cw_put16(p+12,(unsigned)x9c_end_job.token);x9mi_rx(p,n);
 const uint64_t generation=x9q_clients[0].generation,next=x9_mi_next;
 const uint64_t binding=x9c_clients[0].binding;
 md_lock_until_hook=wait;during_md_lock=expire?end_expire_during_md_lock:end_release_during_md_lock;
 x9c_end_poll(out);assert(!during_md_lock&&!md_lock_until_hook);
 assert(!x9c_end_job.used&&!x9_mc_end_token&&x9_mi_next==next);
 assert(x9q_clients[0].generation==generation&&x9c_clients[0].bound&&x9c_clients[0].binding==binding);
 x9c_response_poll();assert(qmi_replies==(expire?1u:0u));
 if(expire)assert(qmi_error==X9Q_NOT_READY);
 x9mi_tick();assert(writes==1&&x9cw_u16(last_frame+10)==0x909);
}
static void ios_end_take_generation(){ios_end_postwait(1,false);}
static void ios_end_snapshot_generation(){ios_end_postwait(2,false);}
static void ios_end_take_deadline(){ios_end_postwait(1,true);}
static void ios_end_snapshot_deadline(){ios_end_postwait(2,true);}
static void ios_end_submit_generation(){ios_end_postwait(3,false);}
static void ios_end_submit_deadline(){ios_end_postwait(3,true);}
/* Source vectors only, never executed as part of this change. */
static void ios_end_guard_admission(bool expire){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();
 const uint64_t next=x9_mi_next,generation=x9q_clients[0].generation;
 during_bb_guard=expire?end_expire_during_md_lock:end_release_during_md_lock;
 uint8_t p[704],out[X9_MIPC_FRAME_MAX];unsigned n=current_frame(p,1);
 x9cw_put16(p+12,(unsigned)x9c_end_job.token);x9mi_rx(p,n);x9c_end_poll(out);
 assert(!during_bb_guard&&!x9c_end_job.used&&!x9_mc_end_token&&x9_mi_next==next);
 assert(x9q_clients[0].generation==generation&&x9c_clients[0].bound&&writes==1);
 x9c_response_poll();assert(qmi_replies==(expire?1u:0u));
 if(expire)assert(qmi_error==X9Q_NOT_READY);
}
static void ios_end_guard_admission_generation(){ios_end_guard_admission(false);}
static void ios_end_guard_admission_deadline(){ios_end_guard_admission(true);}
static void ios_end_guard_selection(bool expire,bool wait){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 assert(x9c_end_job.stage==2&&x9c_end_job.token);
 const uint64_t token=x9c_end_job.token,epoch=x9c_end_job.epoch;
 auto hook=expire?end_expire_during_md_lock:end_release_during_md_lock;
 if(wait)during_bb_guard=hook;else hook(); // no QMI poll/DEL between enqueue and writer
 x9mi_tick();assert(!during_bb_guard&&writes==1&&!x9_mc_end_token);
 assert(x9_mc_outbound.epoch==epoch&&x9c_end_job.token==token);
 x9c_poll();assert(!x9c_end_job.used&&qmi_replies==(expire?1u:0u));
 if(expire)assert(qmi_error==X9Q_NOT_READY);
}
static void ios_end_guard_selection_generation(){ios_end_guard_selection(false,false);}
static void ios_end_guard_selection_deadline(){ios_end_guard_selection(true,false);}
static void ios_end_guard_selection_bb_generation(){ios_end_guard_selection(false,true);}
static void ios_end_guard_selection_bb_deadline(){ios_end_guard_selection(true,true);}
static void ios_end_guard_selected_release(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 const uint64_t token=x9c_end_job.token,epoch=x9c_end_job.epoch;
 during_physical_tx=end_release_during_md_lock;x9mi_tick();assert(!during_physical_tx);
 x9c_poll();assert(writes==2&&!qmi_replies&&x9c_end_job.stage==2&&x9c_end_job.answered&&x9_mc_end_token==token);
 assert(x9cw_u16(last_frame+10)==0x905&&x9cw_u16(last_frame+12)==token&&x9_mc_outbound.epoch==epoch);
 uint8_t p[24];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,sizeof(p));x9c_poll();
 assert(!qmi_replies&&writes==2&&x9c_end_job.stage==3&&x9_mc_end_token==token&&x9_mc_outbound.epoch==epoch);
}
static void ios_end_duplicate(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();uint64_t query=x9c_end_job.token;
 end_request();assert(x9c_end_job.token==query&&!qmi_replies&&!writes);
 x9mi_tick();end_query_cnf();uint64_t token=x9c_end_job.token;
 uint8_t q[]={0,78,0,0x21,0,4,0,1,1,0,7};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(x9c_end_job.token==token&&!qmi_replies);x9mi_tick();
 uint8_t p[24];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(qmi_replies==1&&!qmi_error&&x9c_end_job.used&&!x9c_end_job.token);
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_replies==1&&writes==2); // no BUSY + late-success pair or second mutation
}
static void ios_end_changed_duplicate(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();
 const uint64_t query=x9c_end_job.token,epoch=x9c_end_job.epoch;
 uint8_t q[]={0,78,0,0x21,0,4,0,1,1,0,8}; // same txn, different valid target
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(!qmi_replies&&!writes&&x9c_end_job.token==query&&x9c_end_job.id==7&&x9c_end_job.epoch==epoch);
 x9mi_tick();end_query_cnf();const uint64_t token=x9c_end_job.token;
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(!qmi_replies&&x9c_end_job.token==token&&x9c_end_job.id==7);x9mi_tick();
 assert(writes==2&&x9cw_u16(last_frame+10)==0x905&&x9cw_u32(last_frame+28)==7);
 uint8_t p[24];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,sizeof(p));x9c_poll();
 assert(qmi_replies==1&&!qmi_error&&x9c_end_job.used&&x9c_end_job.answered&&x9_mc_end_token==token);
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_replies==1&&writes==2&&x9c_end_job.id==7&&x9c_end_job.epoch==epoch);
}
static void ios_end_malformed_duplicate(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();
 const uint64_t token=x9c_end_job.token;
 uint8_t malformed[]={0,78,0,0x21,0,0,0};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(!qmi_replies&&!writes&&x9c_end_job.token==token&&x9c_end_job.id==7);
 malformed[1]=80;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(qmi_replies==1&&qmi_error==X9Q_ARG&&!writes&&x9c_end_job.token==token);
}
static void ios_answer_malformed_duplicate(){
 bind_voice();const auto &c=x9c_clients[0];
 x9c_answer_job.used=true;x9c_answer_job.slot=0;x9c_answer_job.sim=c.sim;
 x9c_answer_job.generation=c.generation;x9c_answer_job.binding=c.binding;
 x9c_answer_job.transaction=79;x9c_answer_job.token=42;x9c_answer_job.id=7;
 uint8_t malformed[]={0,79,0,0x22,0,0,0};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(!qmi_replies&&!writes&&x9c_answer_job.token==42&&x9c_answer_job.id==7);
 malformed[1]=80;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(qmi_replies==1&&qmi_error==X9Q_ARG&&!writes&&x9c_answer_job.token==42);
}
/* Synthetic wire fixtures exercise production reducers/adapter, not a device. */
static void ios_answer_receipt(){
 bind_voice();uint8_t p[704];incoming(p);x9cw_scalar(p+16,0x100,7,4);x9mi_rx(p,64);
 identity_progress(p,0,7,20,1,"456",1);x9mi_rx(p,512);
 assert(x9_mc_inbound.seen&&x9_mc_inbound.ios_state==2);
 uint8_t q[]={0,79,0,0x22,0,4,0,1,1,0,7};
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q))&&x9c_answer_job.stage==1);
 uint64_t token=x9c_answer_job.token;x9mi_tick();assert(writes==1&&x9cw_u16(last_frame+10)==0x909);
 unsigned n=current_frame(p,1);x9cw_scalar(p+72,0x6ff8,1,4); /* second array: MT direction */
 x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);x9c_poll();
 assert(x9c_answer_job.stage==2&&x9c_answer_job.token&&x9_mc_answer_token);
 token=x9c_answer_job.token;x9mi_tick();assert(writes==2&&x9cw_u16(last_frame+10)==0x907);
 x9cw_header(p,24,0x908,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);x9mi_rx(p,24);
 call_batch_accept=false;x9c_poll();
 assert(!qmi_replies&&!x9c_answer_job.used&&x9_mc_answer_token==token&&x9_mc_inbound.ios_state==2);
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));assert(!qmi_replies&&writes==2);
 call_batch_accept=true;x9c_response_poll();
 assert(qmi_replies==1&&!qmi_error&&x9_mc_answer_token==token&&x9_mc_inbound.ios_state==2);
}
static void ios_end_abandon(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 x9c_del(0);x9q_clients[0].generation++;x9mi_tick();x9c_poll();
 assert(writes==1&&!qmi_replies&&!x9c_end_job.used&&!x9_mc_end_token&&!x9c_clients[0].bound);
}
static void ios_end_late_cnf(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 uint64_t token=x9c_end_job.token;x9mi_tick();x9c_del(0);x9q_clients[0].generation++;
 rx_progress(133);uint8_t p[704];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(!qmi_replies&&x9c_end_job.stage==4&&x9_mc_end_token==token);
 token=x9c_end_job.token;x9mi_tick();unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);
 x9mi_rx(p,n);x9c_poll();assert(!qmi_replies&&!x9c_end_job.used&&!x9_mc_end_token);
}
static void incoming_states(){
 uint8_t p[64];incoming(p);struct x9cw_incoming c;
 assert(x9cw_incoming(p,64,&c));struct x9cw_inbound_state s={};unsigned id;
 assert(x9cw_inbound_begin(&s,&c)==1&&s.pending&&!s.seen&&!s.ios_state&&!s.epoch);
 assert(!x9cw_inbound_id(&s,&id));
 struct x9cw_progress e={};e.id=c.call_id;e.direction=1;e.mode=20;e.mask=2;e.event=0;
 assert(x9cw_inbound_apply(&s,&e)==1&&s.seen&&!s.pending&&s.ios_state==2&&s.epoch==1);
 assert(!x9cw_inbound_id(&s,&id)); // real u32 id cannot be truncated to Apple u8
 s.native_id=e.id=7;assert(x9cw_inbound_id(&s,&id)&&id==7);
 uint64_t rev=s.revision;assert(x9cw_inbound_apply(&s,&e)==0&&s.revision==rev);
 e.event=132;assert(x9cw_inbound_apply(&s,&e)==1&&s.ios_state==3);
 e.event=6;assert(x9cw_inbound_apply(&s,&e)==1&&s.ios_state==3);
 assert(x9cw_inbound_apply(&s,&e)==1&&s.ios_state==3);
 e.event=133;e.has_cause=1;e.cause=16;
 assert(x9cw_inbound_apply(&s,&e)==1&&s.ended&&s.ios_state==9&&s.cause==16);
 assert(!x9cw_inbound_id(&s,&id));rev=s.revision;
 for(unsigned evt:{0u,132u,6u,133u}){e.event=evt;assert(x9cw_inbound_apply(&s,&e)==0&&s.revision==rev&&s.ended);}
 c.sequence++;c.call_id=8;
 assert(x9cw_inbound_begin(&s,&c)==1&&s.pending&&!s.seen&&s.epoch==1);
 e.id=8;e.event=0;assert(x9cw_inbound_apply(&s,&e)==1&&s.epoch==2&&s.ios_state==2);
}
static void incoming_state_invalid(){
 uint8_t p[64];incoming(p);struct x9cw_incoming c;assert(x9cw_incoming(p,64,&c));
 struct x9cw_inbound_state s={};struct x9cw_progress e={};
 e.id=c.call_id;e.direction=1;e.mode=20;e.mask=2;e.event=0;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked); // no complete CMD identity
 s={};assert(x9cw_inbound_begin(&s,&c)==1);e.event=132;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked); // no observed INCOMING transition
 s={};assert(x9cw_inbound_begin(&s,&c)==1);e.event=0;e.id++;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked);e.id--;
 s={};assert(x9cw_inbound_begin(&s,&c)==1);assert(x9cw_inbound_begin(&s,&c)<0&&s.blocked);
 s={};c.mode=22;assert(x9cw_inbound_begin(&s,&c)<0&&s.blocked);c.mode=20;
 s={};c.sim=0;assert(x9cw_inbound_begin(&s,&c)==0&&!s.pending&&!s.blocked);c.sim=1;
 s={};assert(x9cw_inbound_begin(&s,&c)==1);assert(x9cw_inbound_apply(&s,&e)==1);
 e.event=132;assert(x9cw_inbound_apply(&s,&e)==1);e.event=0;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked); // no active -> incoming regression
 s={};s.revision=UINT64_MAX;assert(x9cw_inbound_begin(&s,&c)<0&&s.blocked);
}
static void incoming_observer(){
 uint8_t p[512];incoming(p);x9cw_scalar(p+16,0x100,7,4);x9mi_rx(p,64);x9mi_rx(p,64);
 assert(x9_mc_inbound.pending&&x9_mc_next==1&&!x9_mc_inbound.blocked&&!writes);
 struct x9cw_inbound_state s;assert(x9_md_mipc_call_incoming_snapshot(&s)==1&&s.pending);
 for(unsigned evt:{0u,132u,6u,6u,133u}){
  unsigned n=progress_frame(p,evt,7,20);x9cw_scalar(p+24,0x101,1,4);x9mi_rx(p,n);x9c_poll();
  assert(x9c_incoming_observed&&x9c_inbound.native_id==7&&!x9c_inbound.blocked);
 }
 assert(x9c_inbound.ended&&x9c_inbound.ios_state==9&&x9c_inbound.epoch==1);
 assert(!x9_mc_outbound.seen&&x9_mc_outbound.blocked&&!x9_mc_outbound.count_known);
 x9mi_tick();assert(!writes&&!qmi_replies); // no auto-approval, answer or publication
}
static void register_codec(){
 uint8_t q[15]={0,9,0,3,0,8,0,0x13,1,0,0,0x14,1,0,1};struct x9cw_voice_register r;
 assert(x9cw_qmi_register(q,15,&r)&&r.present==3&&!r.call_events&&r.handover==1);
 q[11]=0x13;assert(!x9cw_qmi_register(q,15,&r));q[11]=0x14;
 q[12]=4;assert(!x9cw_qmi_register(q,15,&r));q[12]=1;
 q[14]=2;assert(!x9cw_qmi_register(q,15,&r));
 uint8_t frame[24];assert(x9cw_call_register(frame,24,0x8901,1,2)==24&&frame[8]==0xff);
 assert(x9cw_call_register_request(frame,24,2)&&x9_md_mipc_submit(frame,24));
 frame[8]=2;assert(!x9cw_call_register_request(frame,24,2)&&!x9_md_mipc_submit(frame,24));
 assert(x9cw_call_register(frame,24,0x4901,1,2)==24&&frame[8]==2);
 assert(x9cw_call_register_request(frame,24,2));frame[23]=1;
 assert(!x9cw_call_register_request(frame,24,2)&&!x9_md_mipc_submit(frame,24));
 assert(!x9cw_call_register(frame,24,0x491f,1,2));
}
static void register_transition(){
 uint8_t p[32];x9cw_header(p,24,0x308,0,9);x9cw_scalar(p+16,0,0,4);
 struct x9cw_call_init s={};s.stage=X9_CI_STATUS;s.token=9;s.deadline=100;
 assert(x9cw_call_init_cnf(&s,p,24,1)==1&&s.stage==X9_CI_EVENT&&!s.registered&&!s.token);
 s.token=10;x9cw_put16(p+12,10);assert(x9cw_call_init_cnf(&s,p,24,2)==1&&s.registered&&!s.baseline_known);
 s={};s.stage=X9_CI_STATUS;s.token=9;s.deadline=100;x9cw_put16(p+12,9);
 assert(x9cw_call_init_cnf(&s,p,24,100)<0&&s.held&&!s.registered&&!s.token);
 s={};s.stage=X9_CI_STATUS;s.token=10;s.deadline=100;
 assert(x9cw_call_init_cnf(&s,p,24,1)<0&&s.held); // cannot borrow another token
 x9cw_header(p,32,0x308,0,7);x9cw_scalar(p+16,0,21,4);x9cw_scalar(p+24,0x100,0,1);
 uint32_t r;assert(x9cw_call_register_cnf(p,32,0x307,7,&r)&&r==21);
 p[20]=0;assert(!x9cw_call_register_cnf(p,32,0x307,7,&r)); // failure TLV is not success
}
static void register_begin(unsigned slot=0){
 x9c_cmd_init.held=false;x9c_init[1].held=false;x9w_control_ready=true;uint8_t q[]={0,90,0,3,0,4,0,0x13,1,0,0};
 assert(x9c_request(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+9,q,sizeof(q)));
}
static void register_cnf(uint64_t token,unsigned request,unsigned result=0){
 uint8_t p[24];x9cw_header(p,24,request+1,0,(unsigned)token);x9cw_scalar(p+16,0,result,4);
 x9mi_rx(p,24);x9c_poll();
}
static void register_malformed_duplicate(){
 bind_voice();register_begin();assert(!qmi_replies);
 uint8_t malformed[]={0,90,0,3,0,0,0};
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed)));
 assert(qmi_replies==1&&qmi_error==X9Q_ARG&&x9c_reg_wait[0][0].answered);
 x9c_cmd_init.confirmed=true;x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;
 x9c_poll();assert(qmi_replies==1&&!x9c_clients[0].registered);
}
static void register_error_owner_retained(){
 bind_voice();register_begin();call_batch_accept=false;
 uint8_t malformed[]={0,90,0,3,0,0,0};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(!qmi_replies&&x9c_reg_wait[0][0].answered);
 uint8_t next[]={0,91,0,3,0,4,0,0x14,1,0,1};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,next,sizeof(next));
 assert(x9c_reg_wait[0][1].used&&x9c_reg_wait[0][0].transaction==90);
 next[1]=92;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,next,sizeof(next));
 assert(qmi_replies==1&&qmi_error==X9Q_BUSY&&x9c_reg_wait[0][0].transaction==90);
 qmi_replies=0;x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(!qmi_replies);call_batch_accept=true;x9c_response_poll();
 assert(qmi_replies==1&&qmi_error==X9Q_ARG);
}
static void register_sequence(){
 bind_voice();x9c_poll();x9mi_tick();assert(!writes&&!x9c_cmd_init.token); //44 alone sends nothing
 register_begin();x9c_poll();uint64_t token=x9c_cmd_init.token;assert(token&&!qmi_replies);
 x9mi_tick();assert(writes==1&&last_frame[8]==0xff&&x9cw_u16(last_frame+20)==0x8901);
 register_cnf(token,0x30b);assert(x9c_cmd_init.confirmed&&x9c_init[1].stage==X9_CI_STATUS);
 for(unsigned id:{0x4901u,0x4902u}){
  token=x9c_init[1].token;x9mi_tick();token=x9c_init[1].token;assert(last_frame[8]==2&&x9cw_u16(last_frame+20)==id);
  register_cnf(token,0x307);assert(!qmi_replies);
 }
 assert(x9c_init[1].registered&&!x9c_init[1].baseline_known);
 token=x9c_init[1].token;x9mi_tick();token=x9c_init[1].token;assert(x9cw_u16(last_frame+10)==0x909&&last_length==16);
 uint8_t p[704];unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);
 x9mi_rx(p,n);x9c_poll();assert(qmi_replies==1&&!qmi_error&&x9c_clients[0].registered&&x9c_init[1].baseline_known);
 assert(writes==4&&!x9c_init[0].stage&&!x9c_init[0].registered);
 uint8_t malformed[]={0,90,0,3,0,0,0};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,malformed,sizeof(malformed));
 assert(qmi_replies==1&&x9c_clients[0].registered);
 unsigned before=qmi_replies;register_begin();x9c_poll();x9mi_tick();assert(qmi_replies==before&&writes==5&&x9cw_u16(last_frame+10)==0x42f);
}
static void register_timeout(){
 bind_voice();register_begin();x9c_poll();uint64_t token=x9c_cmd_init.token;x9mi_tick();
 fake_ms=25000;x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&x9c_cmd_init.held);
 register_cnf(token,0x30b);assert(!x9c_cmd_init.token&&!x9c_cmd_init.confirmed&&!x9c_init[1].token);
 x9mi_tick();assert(writes==1&&qmi_replies==1&&!x9c_clients[0].registered);
}
static void register_deleted(){
 bind_voice();register_begin();x9c_poll();uint64_t token=x9c_cmd_init.token;x9mi_tick();
 x9c_del(0);x9q_clients[0].generation++;register_cnf(token,0x30b);x9mi_tick();
 assert(writes==2&&!qmi_replies&&x9c_cmd_init.confirmed&&x9c_init[1].token);
}
static void register_unsupported(){
 uint8_t q[]={0,90,0,3,0,4,0,0x13,1,0,1};bind_voice();
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));assert(qmi_error==X9Q_UNSUPPORTED);
 q[7]=0x13;q[10]=0;x9c_del(0);x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_error==X9Q_NOT_READY);x9c_poll();x9mi_tick();assert(!writes&&!x9c_cmd_init.token);
}
static unsigned handover_frame(uint8_t *p,unsigned state,unsigned mask=2){
 x9cw_header(p,32,0x4902,mask,0);x9cw_scalar(p+16,0x100,3,4);
 x9cw_scalar(p+24,0x115,state,4);return 32;
}
static void handover_codec(){
 uint8_t p[40],q[11];struct x9cw_handover e;
 const unsigned mapped[]={1,3,2,4};
 for(unsigned state=0;state<4;state++){
  handover_frame(p,state);assert(x9cw_handover(p,32,&e)==1&&e.sim==1&&e.native_state==state&&e.ios_state==mapped[state]);
  assert(x9cw_handover_indication(q,11,e.ios_state)==11);
  const uint8_t expected[]={4,0,0,0x54,0,4,0,1,1,0,(uint8_t)mapped[state]};
  assert(!memcmp(q,expected,11));
 }
 for(unsigned high=0;high<=0xf0;high+=0x10)for(unsigned low=1;low<=2;low++){
  handover_frame(p,0,high|low);assert(x9cw_handover(p,32,&e)==1&&e.sim==low-1);
 }
 handover_frame(p,4);assert(x9cw_handover(p,32,&e)<0);
 for(unsigned mask:{0u,0x20u,3u,4u,8u,0xffu}){handover_frame(p,0,mask);assert(x9cw_handover(p,32,&e)<0);}
 handover_frame(p,0);p[26]=1;assert(x9cw_handover(p,32,&e)<0);
 handover_frame(p,0);p[24]=0;assert(x9cw_handover(p,32,&e)<0);
 handover_frame(p,0);x9cw_scalar(p+32,0x116,0,4);x9cw_put16(p+14,24);assert(x9cw_handover(p,40,&e)<0);
 handover_frame(p,0);p[20]=6;assert(x9cw_handover(p,32,&e)==0); // RING is not SRVCC
 assert(!x9cw_handover_indication(q,10,1)&&!x9cw_handover_indication(q,11,0)&&!x9cw_handover_indication(q,11,10));
}
static void handover_begin(unsigned slot=0,unsigned value=1,unsigned transaction=91){
 if(!x9c_reg_wait[slot][0].used)x9c_cmd_init.held=false;
 x9c_init[1].held=false;uint8_t q[]={0,(uint8_t)transaction,0,3,0,4,0,0x14,1,0,(uint8_t)value};
 x9c_request(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+9,q,sizeof(q));
}
static void handover_concurrent_bootstrap(){
 bind_voice();register_begin();handover_begin();assert(!qmi_replies);
 x9c_poll();x9mi_tick();register_cnf(x9c_cmd_init.token,0x30b);
 for(unsigned id:{0x4901u,0x4902u}){
  uint64_t token=x9c_init[1].token;x9mi_tick();token=x9c_init[1].token;assert(x9cw_u16(last_frame+20)==id);register_cnf(token,0x307);
 }
 uint64_t token=x9c_init[1].token;x9mi_tick();
 uint8_t p[704];handover_frame(p,0);x9mi_rx(p,32);handover_frame(p,1);x9mi_rx(p,32);x9c_poll();
 assert(!qmi_replies&&!qmi_indications&&x9c_reg_wait[0][1].event_count==2);
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);x9c_poll();
 assert(qmi_replies==4&&qmi_indications==2&&qmi_indication_states[0]==1&&qmi_indication_states[1]==3);
 assert(x9c_clients[0].registered&&x9c_clients[0].handover&&writes==4);
 assert(!x9c_init[1].baseline_known&&x9_mc_outbound.blocked); // handover crossed snapshot
 handover_begin(0,0,92);x9c_poll();assert(!x9c_clients[0].handover);
 handover_frame(p,2);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==2);
 register_begin();x9c_poll();assert(!x9c_clients[0].handover); //13 alone never enables14
}
static void handover_repeat_enable_cursor(){
 bind_voice();x9w_control_ready=true;x9c_cmd_init.confirmed=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;
 handover_begin();x9c_poll();assert(qmi_replies==1&&x9c_clients[0].handover);
 call_batch_accept=false;handover_begin(0,1,92);x9c_poll();assert(qmi_replies==1);
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);x9c_poll();
 assert(x9c_clients[0].pending.count==1&&!qmi_indications);
 call_batch_accept=true;x9c_poll();
 assert(qmi_replies==3&&qmi_indications==1&&qmi_indication_states[0]==1);
 handover_frame(p,1);x9mi_rx(p,32);x9c_poll();
 assert(qmi_indications==2&&qmi_indication_states[1]==3);
}
static void handover_subscription_fence(){
 bind_voice();x9w_control_ready=true;
 x9c_cmd_init.confirmed=true;x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32); // already queued at acceptance
 handover_begin();x9c_poll();assert(qmi_replies==1&&!qmi_indications&&x9c_clients[0].handover);
 handover_frame(p,1);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==1&&qmi_indication_states[0]==3);
 handover_frame(p,0);x9mi_rx(p,32);x9c_del(0);x9q_clients[0].generation++;bind_voice();
 handover_begin();x9c_poll();assert(qmi_indications==1); // no queued event replay to new generation
 handover_frame(p,3,1);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==1); // independent SIM
 handover_frame(p,3);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==2&&qmi_indication_states[1]==4);
}
static void handover_pending_timeout(){
 bind_voice();x9w_control_ready=true;x9c_init[1].stage=X9_CI_DONE;handover_begin();x9c_poll();x9mi_tick();
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);x9c_poll();assert(!qmi_indications);
 uint64_t token=x9c_cmd_init.token;fake_ms=25000;x9c_poll();register_cnf(token,0x30b);
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!qmi_indications&&!x9c_clients[0].handover);
 assert(!x9c_reg_wait[0][0].event_count&&x9c_cmd_init.held);
}
static void handover_pending_overflow(){
 bind_voice();x9w_control_ready=true;x9c_init[1].stage=X9_CI_DONE;handover_begin();x9c_poll();x9mi_tick();
 uint8_t p[32];
 for(unsigned i=0;i<8;i++){
  handover_frame(p,i&1);x9mi_rx(p,32);x9c_poll();
  assert(!qmi_replies&&!qmi_indications&&x9c_reg_wait[0][0].event_count==i+1);
 }
 handover_frame(p,0);x9mi_rx(p,32);x9c_poll();
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!qmi_indications&&!x9c_clients[0].handover);
 assert(!x9c_reg_wait[0][0].event_count&&!x9_mc_poisoned);
 register_cnf(x9c_cmd_init.token,0x30b);assert(qmi_replies==1&&!x9c_init[1].token);
}
static void handover_transport_loss(){
 bind_voice();x9w_control_ready=true;x9c_init[1].stage=X9_CI_DONE;handover_begin();x9c_poll();x9mi_tick();
 uint64_t token=x9c_cmd_init.token;uint8_t p[32];
 handover_frame(p,0);x9mi_rx(p,32);x9c_poll();assert(x9c_reg_wait[0][0].event_count==1);
 handover_frame(p,4);x9mi_rx(p,32);assert(x9_mc_poisoned&&x9_md_mipc_online());
 x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!qmi_indications&&!x9c_clients[0].registered);
 assert(!x9c_reg_wait[0][0].event_count&&x9c_cmd_init.held);
 register_cnf(token,0x30b);assert(!x9c_cmd_init.token&&!x9c_cmd_init.confirmed&&!x9c_init[1].token);
}
static void handover_request_identity(){
 bind_voice();x9w_control_ready=true;x9c_init[1].stage=X9_CI_DONE;handover_begin();handover_begin();assert(!qmi_replies);
 handover_begin(0,0,92);assert(qmi_replies==1&&qmi_error==X9Q_BUSY);
 handover_begin(0,0,91);assert(qmi_replies==2&&qmi_error==X9Q_ARG);
 assert(x9c_reg_wait[0][0].answered&&!x9c_clients[0].handover);
 x9c_cmd_init.confirmed=true;x9c_rat_query[1].done=true;x9c_poll();x9mi_tick();assert(!writes&&!x9c_cmd_init.token);
}
static void handover_completed_changed(){
 bind_voice();x9w_control_ready=true;x9c_cmd_init.confirmed=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;
 handover_begin();x9c_poll();
 assert(qmi_replies==1&&!qmi_error&&x9c_reg_wait[0][0].answered&&x9c_clients[0].handover);
 const uint64_t binding=x9c_clients[0].binding;
 handover_begin(0,0,91); // changed value under the already-terminal transaction
 assert(qmi_replies==1&&!qmi_error&&x9c_clients[0].handover&&x9c_clients[0].binding==binding);
 x9c_poll();x9mi_tick();assert(qmi_replies==1&&!writes&&call_batches==1);
 handover_begin(0,0,92);x9c_poll(); // a fresh transaction still owns its disable
 assert(qmi_replies==2&&!qmi_error&&!x9c_clients[0].handover&&call_batches==2&&!writes);
}
static void handover_rx_fence(){
 rx_progress(130,7,0);rx_progress(2);rx_progress(132);
 uint8_t current[704],out[704];x9cw_header(current,16,0x909,2,0);
 uint64_t token=x9_md_mipc_submit(current,16);assert(token);x9mi_tick();
 unsigned size=current_frame(current,1);x9cw_put16(current+12,(unsigned)token);x9mi_rx(current,size);
 uint32_t n;assert(x9_md_mipc_take(token,out,sizeof(out),&n)==1);
 uint8_t p[32];struct x9cw_end_target t;assert(x9_md_mipc_call_end_snapshot(&t)==1);
 uint8_t q[]={0,8,0,0x21,0,4,0,1,1,0,7};
 const struct x9_mipc_call_owner owner={CBQ_PORT_BASE,X9BB_NODE,call_bridge_generation[0],25000};
 assert(x9cw_action(q,11,p,32,7,7,1,1)==32);token=x9_md_mipc_call_end_submit(p,32,&t,&owner);assert(token);
 handover_frame(p,0);x9mi_rx(p,32);x9mi_tick();
 assert(x9_md_mipc_take(token,p,32,&n)==-3); // no queued pre-handover END TX
 assert(x9_mc_outbound.blocked&&!x9_mc_outbound.count_known&&x9_mc_inbound.blocked);
}
static void registration_queue_wait(){
 bind_voice();x9w_control_ready=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;handover_begin();
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);handover_frame(p,1);x9mi_rx(p,32);
 call_batch_accept=false;x9c_poll();
 assert(!qmi_replies&&!qmi_indications&&!x9c_clients[0].registered&&!x9c_reg_wait[0][0].answered);
 assert(call_batches==1&&call_batch_count==3&&call_allocs==3&&call_frees==3);
 assert(x9c_reg_wait[0][0].event_count==2);
 call_batch_accept=true;x9c_poll();
 assert(qmi_replies==3&&qmi_indications==2&&qmi_indication_states[0]==1&&qmi_indication_states[1]==3);
 assert(x9c_clients[0].registered&&x9c_clients[0].handover&&x9c_reg_wait[0][0].answered);
 assert(!writes&&!x9c_reg_wait[0][0].event_count);
}
static void registration_queue_timeout(){
 bind_voice();x9w_control_ready=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;handover_begin();
 call_batch_accept=false;x9c_poll();assert(!qmi_replies&&!x9c_clients[0].registered);
 fake_ms=25000;x9c_poll();
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9c_clients[0].registered&&!x9c_clients[0].handover);
 assert(call_batches==1&&x9c_reg_wait[0][0].answered);
}
static void registration_alloc_oom(){
 bind_voice();x9w_control_ready=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;register_begin();
 call_alloc_fail=true;x9c_poll();assert(!call_batches&&!qmi_replies&&!x9c_clients[0].registered);
 call_alloc_fail=false;x9c_poll();assert(call_batches==1&&qmi_replies==1&&x9c_clients[0].registered);
}
static void registration_alloc_poison(){
 bind_voice();x9w_control_ready=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;handover_begin();
 call_alloc_poison=true;x9c_poll();
 assert(!call_batches&&qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9c_clients[0].registered);
 assert(!x9c_clients[0].handover&&call_allocs==call_frees);
}
static void registration_alloc_deadline(){
 bind_voice();x9w_control_ready=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;handover_begin();
 call_alloc_expire=true;x9c_poll();
 assert(!call_batches&&!qmi_replies&&!x9c_clients[0].registered&&call_allocs==call_frees);
 x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9c_clients[0].handover);
}
static int call_publish_count(void *ctx){assert(lock_depth);(*static_cast<unsigned *>(ctx))++;return 1;}
static void registration_queue_lock_deadline(){
 bind_voice();x9w_control_ready=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;handover_begin();
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);
 call_bb_expire=true;x9c_poll();
 assert(call_batches==1&&call_batch_count==2&&!qmi_replies&&!qmi_indications);
 assert(!x9c_clients[0].registered&&call_allocs==call_frees);
 x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9c_clients[0].handover);
}
static void registration_native_fence(){
 unsigned calls=0;assert(x9_md_mipc_call_publish(1,call_publish_count,&calls)==-1&&!calls);
 assert(x9_md_mipc_call_publish(0,nullptr,&calls)==-1&&!calls);
 assert(x9_md_mipc_call_publish(0,call_publish_count,&calls)==1&&calls==1);
 x9_mc_poisoned=1;assert(x9_md_mipc_call_publish(0,call_publish_count,&calls)==-1&&calls==1);
}
static void handover_live_setup(){
 bind_voice();x9w_control_ready=true;x9c_cmd_init.confirmed=true;
 x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;handover_begin();x9c_poll();
 assert(x9c_clients[0].registered&&x9c_clients[0].handover&&!writes);
 qmi_replies=0;call_batches=call_batch_count=call_allocs=call_frees=0;
}
static void handover_live_queue_wait(){
 handover_live_setup();uint8_t p[32];call_batch_accept=false;
 handover_frame(p,0);x9mi_rx(p,32);handover_frame(p,1);x9mi_rx(p,32);x9c_poll();
 assert(!qmi_indications&&!qmi_replies&&x9c_clients[0].pending.count==2);
 assert(call_batches==1&&call_batch_count==2&&call_allocs==2&&call_frees==2);
 call_batch_accept=true;x9c_poll();
 assert(qmi_indications==2&&qmi_indication_states[0]==1&&qmi_indication_states[1]==3);
 assert(!x9c_clients[0].pending.count&&x9c_clients[0].pending.last==x9_mc_next&&!writes);
 x9c_poll();assert(qmi_indications==2&&call_batches==2); // no replay after receipt
}
static void handover_live_repeated_state(){
 handover_live_setup();uint8_t p[32];
 handover_frame(p,0);x9mi_rx(p,32);x9mi_rx(p,32);x9c_poll();
 assert(qmi_indications==2&&qmi_indication_states[0]==1&&qmi_indication_states[1]==1);
 assert(call_batches==1&&call_batch_count==2&&!x9c_clients[0].pending.count);
}
static void handover_live_partial_alloc(){
 handover_live_setup();uint8_t p[32];
 handover_frame(p,0);x9mi_rx(p,32);handover_frame(p,1);x9mi_rx(p,32);
 call_alloc_fail_at=2;x9c_poll();
 assert(!call_batches&&!qmi_indications&&call_allocs==2&&call_frees==1);
 assert(x9c_clients[0].pending.count==2&&x9c_clients[0].handover);
 call_alloc_fail_at=0;x9c_poll();
 assert(call_batches==1&&qmi_indications==2&&qmi_indication_states[0]==1&&qmi_indication_states[1]==3);
 assert(!x9c_clients[0].pending.count&&!writes);
}
static void handover_live_alloc_poison(){
 handover_live_setup();uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);
 call_alloc_poison=true;x9c_poll();
 assert(!call_batches&&!qmi_indications&&call_allocs==call_frees&&call_allocs==1);
 assert(!x9c_clients[0].registered&&!x9c_clients[0].handover&&!x9c_clients[0].pending.count);
 unchanged_other_inboxes();
}
static void handover_live_generation_sim(){
 handover_live_setup();uint8_t p[32];call_batch_accept=false;
 handover_frame(p,0);x9mi_rx(p,32);x9c_poll();assert(x9c_clients[0].pending.count==1);
 x9c_del(0);x9q_clients[0].generation++;call_batch_accept=true;bind_voice();
 handover_begin(0,1,92);x9c_poll();assert(!qmi_indications&&!x9c_clients[0].pending.count);
 handover_frame(p,1,1);x9mi_rx(p,32);x9c_poll();assert(!qmi_indications); // other SIM
 handover_frame(p,1);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==1&&qmi_indication_states[0]==3);
 // Rebinding without QRTR deletion also retires the old SIM's local owner.
 call_batch_accept=false;handover_frame(p,0);x9mi_rx(p,32);x9c_poll();
 uint8_t bind[]={0,93,0,0x44,0,4,0,1,1,0,0};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,bind,sizeof(bind));
 assert(x9c_clients[0].sim==1&&x9c_clients[0].pending.count==1&&x9c_clients[0].handover&&x9c_bind_pending(0,true));
 call_batch_accept=true;x9c_poll();
 assert(x9c_clients[0].sim==0&&!x9c_clients[0].pending.count&&!x9c_clients[0].handover&&!x9c_bind_pending(0));
 assert(qmi_indications==2&&!writes); // old-route event flushed before44 receipt
}
static void handover_live_overflow_disable(){
 handover_live_setup();uint8_t p[32];call_batch_accept=false;
 // This pre-overflow enable waiter must not clear the blocked owner later.
 handover_begin(0,1,92);
 for(unsigned i=0;i<9;i++){handover_frame(p,i&1);x9mi_rx(p,32);x9c_poll();}
 assert(x9c_clients[0].pending.blocked&&x9c_clients[0].pending.count==8);
 assert(!x9c_clients[0].handover&&!qmi_indications&&!x9_mc_poisoned);
 assert(qmi_error==X9Q_NOT_READY&&x9c_reg_wait[0][0].answered);
 handover_begin(0,1,93);assert(qmi_error==X9Q_NOT_READY&&x9c_clients[0].pending.blocked);
 call_batch_accept=true;handover_begin(0,0,94);x9c_poll();
 assert(!x9c_clients[0].pending.blocked&&!x9c_clients[0].pending.count&&!x9c_clients[0].handover);
 handover_begin(0,1,95);x9c_poll();
 assert(x9c_clients[0].handover&&x9c_clients[0].handover_floor==x9_mc_next&&!qmi_indications);
 handover_frame(p,1);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==1&&qmi_indication_states[0]==3&&!writes);
}
static void handover_live_no_request_deadline(){
 handover_live_setup();uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);
 // The allocator crosses the old registration guard; this live event has none.
 call_alloc_expire=true;fake_ms=24999;x9c_poll();
 assert(fake_ms==25000&&qmi_indications==1&&call_batches==1&&x9c_clients[0].handover);
 call_batch_accept=false;handover_frame(p,1);x9mi_rx(p,32);x9c_poll();
 call_alloc_expire=false;fake_ms=uint64_t(1)<<40;call_batch_accept=true;x9c_poll();
 assert(qmi_indications==2&&qmi_indication_states[1]==3&&!x9c_clients[0].pending.count&&!writes);
}
static void handover_live_queue_sequence(){
 struct x9cw_handover_queue q={};
 assert(x9cw_handover_queue_push(&q,5,5,1)==0&&!q.count);
 assert(x9cw_handover_queue_push(&q,5,6,1)==1&&q.count==1&&q.last==6);
 assert(x9cw_handover_queue_push(&q,5,7,1)==1&&q.count==2); // repeated state retained
 x9cw_handover_queue_published(&q);assert(!q.count&&q.last==7);
 assert(x9cw_handover_queue_push(&q,5,7,3)<0&&q.blocked);
 x9cw_handover_queue_published(&q);assert(q.blocked);
 bzero(&q,sizeof(q));assert(x9cw_handover_queue_push(&q,0,1,0)<0&&q.blocked);
}
int main(int argc,char **argv){
 assert(argc==2);x9c_cmd_init.held=true;x9c_init[0].held=x9c_init[1].held=true;x9mi_online(1);
 struct {const char *name;void (*run)();} cases[]={
  {"codec_golden",codec_golden},{"codec_invalid",codec_invalid},{"incoming_routes",incoming_routes},{"action_codec",action_codec},
  {"dial_golden",dial_golden},{"dial_invalid",dial_invalid},{"dial_whitelist",dial_whitelist},
  {"dial_roundtrip",dial_roundtrip},{"dial_tx_failure",dial_tx_failure},{"dial_cnf_invalid",dial_cnf_invalid},
  {"progress_framing",progress_framing},{"outbound_states",outbound_states},{"current_arrays",current_arrays},
  {"status_fence",status_fence},{"dial_join_order",dial_join_order},{"ios_join",ios_join},
  {"ios_timeout",ios_timeout},{"ios_deleted",ios_deleted},{"dial_presend_fence",dial_presend_fence},{"dial_presend_release",dial_presend_release},
  {"dial_guard_admission_release",dial_guard_admission_release},{"dial_guard_admission_timeout",dial_guard_admission_timeout},
  {"dial_guard_writer_release",dial_guard_writer_release},{"dial_guard_writer_timeout",dial_guard_writer_timeout},
  {"dial_guard_selected_release",dial_guard_selected_release},{"dial_guard_selected_timeout",dial_guard_selected_timeout},
  {"voice_bind_codec",voice_bind_codec},{"end_target_codec",end_target_codec},{"ios_end_roundtrip",ios_end_roundtrip},
  {"ios_end_all_roundtrip",ios_end_all_roundtrip},{"ios_end_all_reject",ios_end_all_reject},
  {"ios_end_all_pending_incoming",ios_end_all_pending_incoming},
  {"current_metadata",current_metadata},
  {"current_number_owner",current_number_owner},{"progress_number_owner",progress_number_owner},
  {"progress_identity_mo",progress_identity_mo},{"progress_identity_mt",progress_identity_mt},
  {"progress_identity_provenance",progress_identity_provenance},
  {"end_reason_provenance",end_reason_provenance},{"outbound_end_reason",outbound_end_reason},{"incoming_end_reason",incoming_end_reason},
  {"ios_end_stale_query",ios_end_stale_query},{"ios_end_presend",ios_end_presend},
  {"ios_end_take_generation",ios_end_take_generation},{"ios_end_snapshot_generation",ios_end_snapshot_generation},
  {"ios_end_take_deadline",ios_end_take_deadline},{"ios_end_snapshot_deadline",ios_end_snapshot_deadline},
  {"ios_end_submit_generation",ios_end_submit_generation},{"ios_end_submit_deadline",ios_end_submit_deadline},
  {"ios_end_guard_admission_generation",ios_end_guard_admission_generation},
  {"ios_end_guard_admission_deadline",ios_end_guard_admission_deadline},
  {"ios_end_guard_selection_generation",ios_end_guard_selection_generation},
  {"ios_end_guard_selection_deadline",ios_end_guard_selection_deadline},
  {"ios_end_guard_selection_bb_generation",ios_end_guard_selection_bb_generation},
  {"ios_end_guard_selection_bb_deadline",ios_end_guard_selection_bb_deadline},
  {"ios_end_guard_selected_release",ios_end_guard_selected_release},
  {"ios_end_abandon",ios_end_abandon},{"ios_end_late_cnf",ios_end_late_cnf},
  {"ios_answer_receipt",ios_answer_receipt},{"ios_end_malformed_duplicate",ios_end_malformed_duplicate},{"ios_answer_malformed_duplicate",ios_answer_malformed_duplicate},
  {"ios_end_duplicate",ios_end_duplicate},{"ios_end_changed_duplicate",ios_end_changed_duplicate},
  {"incoming_states",incoming_states},{"incoming_state_invalid",incoming_state_invalid},{"incoming_observer",incoming_observer},
  {"register_error_owner_retained",register_error_owner_retained},{"register_malformed_duplicate",register_malformed_duplicate},{"register_codec",register_codec},{"register_transition",register_transition},{"register_sequence",register_sequence},
  {"register_timeout",register_timeout},{"register_deleted",register_deleted},{"register_unsupported",register_unsupported},
  {"handover_codec",handover_codec},{"handover_concurrent_bootstrap",handover_concurrent_bootstrap},
  {"handover_repeat_enable_cursor",handover_repeat_enable_cursor},{"handover_subscription_fence",handover_subscription_fence},{"handover_pending_timeout",handover_pending_timeout},
  {"handover_pending_overflow",handover_pending_overflow},{"handover_transport_loss",handover_transport_loss},
  {"handover_request_identity",handover_request_identity},{"handover_completed_changed",handover_completed_changed},
  {"handover_rx_fence",handover_rx_fence},
  {"registration_queue_wait",registration_queue_wait},{"registration_queue_timeout",registration_queue_timeout},
  {"registration_alloc_oom",registration_alloc_oom},{"registration_alloc_poison",registration_alloc_poison},
  {"registration_alloc_deadline",registration_alloc_deadline},{"registration_native_fence",registration_native_fence},
  {"registration_queue_lock_deadline",registration_queue_lock_deadline},
  {"handover_live_queue_wait",handover_live_queue_wait},{"handover_live_repeated_state",handover_live_repeated_state},
  {"handover_live_partial_alloc",handover_live_partial_alloc},{"handover_live_alloc_poison",handover_live_alloc_poison},
  {"handover_live_generation_sim",handover_live_generation_sim},{"handover_live_overflow_disable",handover_live_overflow_disable},
  {"handover_live_no_request_deadline",handover_live_no_request_deadline},{"handover_live_queue_sequence",handover_live_queue_sequence},
  {"request_whitelist",request_whitelist},{"no_auto_policy",no_auto_policy},{"command_roundtrip",command_roundtrip},
 {"calllist_client_isolation",calllist_client_isolation},{"calllist_voice_rat_refresh",calllist_voice_rat_refresh},
 {"calllist_voice_rat_recovery",calllist_voice_rat_recovery},{"calllist_stale_publication",calllist_stale_publication},
 {"calllist_generation_fence",calllist_generation_fence},
  {"failure_retained",failure_retained},{"duplicate_pending",duplicate_pending},{"changed_duplicate",changed_duplicate},
  {"duplicate_replied",duplicate_replied},{"overflow_isolated",overflow_isolated},{"bounded_tombstones",bounded_tombstones},
  {"fifo",fifo},{"fragmented",fragmented},{"late_cnf",late_cnf},{"sms_priority",sms_priority},{"staged_ios",staged_ios},
  {"voice20_dispatch",voice20_dispatch}};
 for(auto c:cases)if(!strcmp(argv[1],c.name)){c.run();assert(!lock_depth);return 0;}
 return 2;
}
