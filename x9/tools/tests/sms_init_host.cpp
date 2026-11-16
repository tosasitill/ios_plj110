/* Compile-only synthetic vectors for the actual SMS owner and MD transport.
 * No device PDU, vendor bytes, network, test runner or executable invocation. */
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
#define LCK_GRP_DECLARE(n, s) int n
#define LCK_MTX_DECLARE(n, g) int n
static void lck_mtx_lock(int *){assert(!lock_depth);lock_depth=1;if(during_md_lock)during_md_lock();}
static void lck_mtx_unlock(int *){assert(lock_depth);lock_depth=0;}
static int x9_at_enabled(){return 1;}
static int sweet_x9_arg(const char *,int){return 1;}
static uint64_t fake_ms;
static uint64_t mach_absolute_time(){return fake_ms*1000000;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t;}
static bool at_online=true;
int x9_md_at_online(){assert(!lock_depth);return at_online;}
static unsigned writes;
static void (*during_write)();
static int x9md_tx(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,const uint8_t *,unsigned n){
 assert(!lock_depth&&a==5&&b==217&&c==0&&d==16+n&&e==0);writes++;if(during_write)during_write();return 0;
}
#include "../../kernel/src/bsd/dev/x9_mipc_bridge.inc"
/* This fixture owns no CALL allocation. Keep the shared transport's END
 * boundary fail-closed; never invoke its commit callback with invented voice
 * identity. This is compile-only infrastructure, not a SMS behavior change. */
extern "C" int coachz_baseband_call_guard(const struct x9_mipc_call_owner *,
                                         x9_mipc_call_owner_cb,void *){
 assert(lock_depth==1);return 0;
}
static const unsigned X9BB_PORT_BASE=0x2000,X9BB_NODE=1,CBQ_PORT_BASE=0x4000,CBQ_MAX_CLIENTS=8,X9Q_REQ=1024,CBQ_HDR=6;
static const uint16_t X9Q_INTERNAL=3,X9Q_NOT_READY=16,X9Q_ARG=48,X9Q_BUSY=2,X9Q_UNSUPPORTED=94;
static struct {uint64_t generation;uint8_t sim_slot;} x9q_clients[CBQ_MAX_CLIENTS];
static bool x9w_control_ready;
static uint64_t x9q_ms(){return fake_ms;}
static uint16_t x9q_u16(const uint8_t *p){return (uint16_t)x9cw_u16(p);}
static uint32_t x9q_u32(const uint8_t *p){return x9cw_u32(p);}
struct x9q_view {const uint8_t *p;unsigned n;};
static x9q_view x9q_tlv(const uint8_t *p,unsigned n,unsigned tag){
 for(unsigned at=7;at+3<=n;){unsigned k=x9q_u16(p+at+1);if(k>n-at-3)break;
  if(p[at]==tag)return {p+at+3,k};at+=3+k;}return {nullptr,0};
}
static unsigned replies,last_error;
static uint8_t last_packet[1536];static unsigned last_packet_len;
struct x9q_packet {
 uint8_t p[1536];unsigned n;bool good;uint16_t error;
 x9q_packet(const uint8_t *req,uint16_t e=0):n(14),good(true),error(e){bzero(p,sizeof(p));p[0]=2;memcpy(p+1,req+1,4);
  p[7]=2;p[8]=4;p[10]=(uint8_t)(e!=0);x9cw_put16(p+12,e);}
 void tlv(unsigned tag,const void *v,unsigned k){assert(k<=sizeof(p)-n-3);p[n]=(uint8_t)tag;x9cw_put16(p+n+1,k);memcpy(p+n+3,v,k);n+=3+k;}
 void send(uint32_t,uint32_t,bool=false,uint16_t=0){replies++;last_error=error;memcpy(last_packet,p,n);last_packet_len=n;}
};
struct x9w_frame {
 uint8_t p[1024];unsigned n;bool good;
 x9w_frame(unsigned id,unsigned mask=1):n(16),good(true){x9cw_header(p,sizeof(p),id,mask,0);}
 ~x9w_frame(){bzero(p,sizeof(p));}
 void tlv(unsigned tag,const void *v,unsigned k){unsigned size=(k+11)&~7u;assert(k&&size<=sizeof(p)-n);
  x9cw_put16(p+n,tag);x9cw_put16(p+n+2,k);memcpy(p+n+4,v,k);n+=size;}
 void byte(unsigned tag,uint8_t v){tlv(tag,&v,1);}
 uint64_t submit(){x9cw_put16(p+14,n-16);return x9_md_mipc_submit(p,n);}
};
static bool x9w_wire(const uint8_t *p,unsigned n){const uint8_t *v;unsigned k;
 return x9cw_frame(p,n,x9cw_u16(p+10))&&x9cw_tlv(p,n,0,&v,&k);}
static x9q_view x9w_tlv(const uint8_t *p,unsigned n,unsigned tag){const uint8_t *v;unsigned k;
 return x9cw_tlv(p,n,tag,&v,&k)?x9q_view{v,k}:x9q_view{nullptr,0};}
static bool x9w_byte(const uint8_t *p,unsigned n,unsigned tag,unsigned &v){auto t=x9w_tlv(p,n,tag);if(!t.p||t.n!=1)return false;v=t.p[0];return true;}
static bool x9w_word(const uint8_t *p,unsigned n,unsigned tag,unsigned &v){auto t=x9w_tlv(p,n,tag);if(!t.p||t.n!=4)return false;v=x9cw_u32(t.p);return true;}
static unsigned allocations,frees,queue_calls;
static bool queue_accept=true,allocation_accept=true;
static uint8_t *queued_frame;
static void (*during_allocation)();
static void *IOMallocData(unsigned n){
 assert(!lock_depth);if(during_allocation)during_allocation();
 if(!allocation_accept)return nullptr;allocations++;return malloc(n);
}
static void IOFreeData(void *p,unsigned){assert(!lock_depth);frees++;free(p);}
static bool bb_x9_sms_event(uint32_t,uint32_t,const uint8_t *,unsigned n,uint8_t *owned,unsigned room){
 assert(lock_depth&&owned&&room==n+CBQ_HDR);queue_calls++;
 if(!queue_accept)return false;assert(!queued_frame);queued_frame=owned;return true;
}
static uint64_t bridge_ack_generation=1;
static bool bridge_ack_live=true;
static unsigned ack_queue_calls;
static uint64_t bb_x9_sms_ack_identity(uint32_t,uint32_t){assert(!lock_depth);return bridge_ack_live?bridge_ack_generation:0;}
static bool bb_x9_sms_ack_result(uint32_t,uint32_t,uint64_t generation,const uint8_t *p,unsigned n,uint8_t *owned,unsigned room){
 assert(!lock_depth&&owned&&room==n+CBQ_HDR&&n==14&&p[0]==2&&x9q_u16(p+3)==0x37);ack_queue_calls++;
 if(!bridge_ack_live||generation!=bridge_ack_generation||!queue_accept)return false;
 assert(!queued_frame);queued_frame=owned;replies++;last_error=x9q_u16(p+12);memcpy(last_packet,p,n);last_packet_len=n;return true;
}
#include "../../kernel/src/iokit/Kernel/arm/X9Messages.inc"
static void cnf(uint64_t token,unsigned id,unsigned mask=1,bool host=false){
 uint8_t p[32];unsigned n=host?32:24;x9cw_header(p,n,id,mask,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 if(host)x9cw_scalar(p+24,0x102,1,1);x9mi_rx(p,n);
}
static uint64_t begin_global(){
 x9m_sims[0].wanted=true;uint8_t p[X9_MIPC_FRAME_MAX];x9m_init_poll(p,sizeof(p));
 assert(x9m_init_token&&x9m_init_stage==1);x9mi_tick();return x9m_init_token;
}
static void global_exact_deadline(){
 uint64_t token=begin_global();fake_ms=x9m_init_deadline;cnf(token,0x30c,0xff);x9m_poll();
 assert(x9m_init_hold&&!x9m_init_token&&x9m_init_stage==1&&writes==1&&!x9m_sims[0].init_token);
}
static void global_late_cnf(){
 uint64_t token=begin_global();fake_ms=x9m_init_deadline;x9m_poll();assert(x9m_init_hold&&x9m_init_token==token);
 fake_ms++;cnf(token,0x30c,0xff);x9m_poll();assert(x9m_init_hold&&!x9m_init_token&&writes==1&&x9m_init_stage==1);
}
static uint64_t host_ready_pending(unsigned sim,uint64_t deadline){
 x9m_init_stage=2;x9m_sim &s=x9m_sims[sim];s.wanted=s.md_known=s.md_ready=true;s.init=X9M_HOST_READY;
 s.ready_epoch=s.init_epoch=1;s.init_deadline=deadline;x9w_frame f(0x601,1u<<sim);f.byte(0x100,0);f.byte(0x105,1);f.byte(0x106,2);
 s.init_token=f.submit();assert(s.init_token);x9mi_tick();return s.init_token;
}
static void sim_exact_deadline(){
 uint64_t token=host_ready_pending(0,100);fake_ms=100;cnf(token,0x602,1,true);x9m_poll();
 assert(x9m_sims[0].init_hold&&x9m_sims[0].init_timed_out&&!x9m_sims[0].ready&&!x9m_sims[0].init_token);
 uint8_t p[32];x9cw_header(p,32,0x4601,1,0);x9cw_scalar(p+16,0,0,4);x9cw_scalar(p+24,0x102,1,1);
 x9mi_rx(p,32);x9m_poll();assert(x9m_sims[0].md_ready&&x9m_sims[0].init_hold&&!x9m_sims[0].ready);
}
static void sim_independent_deadlines(){
 uint64_t a=host_ready_pending(0,100),b=host_ready_pending(1,200);fake_ms=100;
 cnf(a,0x602,1,true);cnf(b,0x602,2,true);x9m_poll();
 assert(x9m_sims[0].init_hold&&!x9m_sims[0].ready&&!x9m_init_hold);
 assert(x9m_sims[1].ready&&!x9m_sims[1].init_hold&&!x9m_sims[1].init_token);
}
static void init_offline_drain(){
 uint64_t token=host_ready_pending(0,100);cnf(token,0x602,1,true);x9mi_online(0);x9m_poll();
 assert(x9m_init_hold&&x9m_sims[0].init_hold&&!x9m_sims[0].ready&&!x9m_sims[0].init_token);
}
static void job_send_deadline(){
 uint8_t req[7]={0,9,0,0x20};auto *j=x9m_new(X9M_SEND,0,0,req,7);assert(j);
 x9w_frame f(0x605);const uint8_t raw[]={0,1};f.byte(0x100,0);f.tlv(0x8101,raw,2);f.byte(0x102,1);
 j->token=f.submit();assert(j->token);x9mi_tick();uint8_t p[32];x9cw_header(p,32,0x606,1,(unsigned)j->token);
 x9cw_scalar(p+16,0,0,4);x9cw_scalar(p+24,0x103,1,1);fake_ms=j->deadline;x9mi_rx(p,32);x9m_poll();
 assert(replies==1&&last_error==X9Q_NOT_READY&&!j->op&&writes==1);
}
static void job_ack_deadline(){
 auto &s=x9_mm_slots[0];s.token=7;s.state=5;s.result=0;s.id=0x8601;
 uint8_t req[7]={0,9,0,0x37};auto *j=x9m_new(X9M_ACK,0,0,req,7);assert(j);j->token=7;
 x9m_incoming_sms[0].token=7;x9m_incoming_sms[0].delivered=x9m_incoming_sms[0].replying=true;
 fake_ms=j->deadline;x9m_poll();assert(replies==1&&last_error==X9Q_NOT_READY&&!j->op&&!s.state&&!x9m_incoming_sms[0].token);
}
static void queued_control_loss(){
 x9m_init_stage=2;x9m_sims[0].ready=true;x9m_sims[0].init=X9M_INIT_DONE;
 uint8_t query[7]={0,9,0,0x34};auto *smsc=x9m_new(X9M_SMSC,0,0,query,7);assert(smsc);
 uint8_t list[11]={0,10,0,0x31,0,4,0,1,1,0,0};auto *stored=x9m_new(X9M_LIST,0,0,list,11);assert(stored);
 stored->follow=true;stored->store_order=1;x9w_control_ready=false;x9m_poll();
 assert(!writes&&!smsc->token&&!stored->token&&x9m_init_hold&&x9m_sims[0].init_hold&&!x9m_sims[0].ready);
}
/* Native owner itself raises poison: same outstanding CMD identity, changed
 * payload. No hand-set poison flag, real device bytes or test execution. */
static void poison_sms_inbox(){
 uint8_t p[24];x9cw_header(p,24,0x8601,1,55);x9cw_scalar(p+16,0x100,3,1);x9mi_rx(p,24);
 p[20]=4;x9mi_rx(p,24);
 assert(x9_mm_poisoned&&x9_md_mipc_online()&&!x9_md_mipc_message_online());
 bool retained=false;for(auto &s:x9_mm_slots)if(s.state&&s.txid==55)retained=true;assert(retained);
}
static uint64_t sms_send_pending(x9m_job *j){
 x9w_frame f(0x605);const uint8_t raw[]={0,1};f.byte(0x100,0);f.tlv(0x8101,raw,2);f.byte(0x102,1);
 uint64_t token=f.submit();assert(token);if(j)j->token=token;return token;
}
static void sms_poison_admission_scope(){
 poison_sms_inbox();unsigned next=x9_mi_next;
 for(unsigned id:{0x8601u,0x8603u}){x9w_frame f(0x30b,0xff);uint8_t v[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,v,2);assert(!f.submit());}
 for(unsigned id:{0x4601u,0x4a01u}){x9w_frame f(0x307);uint8_t v[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,v,2);assert(!f.submit());}
 {x9w_frame f(0x601);f.byte(0x100,0);f.byte(0x105,1);f.byte(0x106,2);assert(!f.submit());}
 {x9w_frame f(0x601);f.byte(0x102,1);assert(!f.submit());}
 {x9w_frame f(0x605);const uint8_t raw[]={0,1};f.byte(0x100,0);f.tlv(0x8101,raw,2);f.byte(0x102,1);assert(!f.submit());}
 {x9w_frame f(0x607);f.byte(0x100,3);f.byte(0x101,0);f.byte(0x103,1);f.byte(0x104,0);assert(!f.submit());}
 assert(x9_mi_next==next&&!writes);
 {x9w_frame f(0xa04);f.byte(0x100,0);assert(f.submit());}
 {x9w_frame f(0x30b,0xff);uint8_t v[2]={1,0x89};f.tlv(0x100,v,2);assert(f.submit());}
 {x9w_frame f(0x307);uint8_t v[2]={1,0x49};f.tlv(0x100,v,2);assert(f.submit());}
 {x9w_frame f(0x201);assert(f.submit());}
 for(unsigned i=0;i<4;i++)x9mi_tick();assert(writes==4&&!x9_mi_poisoned&&!x9_mc_poisoned);
}
static void sms_poison_global_drain(){
 uint64_t token=begin_global();cnf(token,0x30c,0xff);poison_sms_inbox();x9m_poll();
 assert(x9m_init_hold&&!x9m_init_token&&x9m_init_stage==1&&writes==1);
}
static void sms_poison_host_ready_drain(){
 uint64_t token=host_ready_pending(0,100);cnf(token,0x602,1,true);poison_sms_inbox();x9m_poll();
 assert(x9m_sims[0].init_hold&&!x9m_sims[0].init_token&&!x9m_sims[0].ready&&x9m_sims[0].md_ready);
}
static void sms_poison_global_pre_tx(){
 uint8_t p[X9_MIPC_FRAME_MAX];x9m_sims[0].wanted=true;x9m_init_poll(p,sizeof(p));assert(x9m_init_token);
 poison_sms_inbox();x9mi_tick();assert(!writes);x9m_poll();
 assert(x9m_init_hold&&!x9m_init_token&&x9m_init_stage==1&&x9_mm_poisoned);
}
static void sms_poison_sim_pre_tx(){
 uint8_t p[X9_MIPC_FRAME_MAX];x9m_init_stage=2;x9m_sims[0].wanted=true;x9m_init_poll(p,sizeof(p));assert(x9m_sims[0].init_token);
 poison_sms_inbox();x9mi_tick();assert(!writes);x9m_poll();
 assert(x9m_sims[0].init_hold&&!x9m_sims[0].init_token&&!x9m_sims[0].ready);
}
static void sms_poison_queued_and_ready(){
 x9m_init_stage=2;x9m_sims[0].ready=x9m_sims[0].md_known=x9m_sims[0].md_ready=true;x9m_sims[0].init=X9M_INIT_DONE;
 uint8_t query[7]={0,9,0,0x34};auto *smsc=x9m_new(X9M_SMSC,0,0,query,7);assert(smsc);
 uint8_t list[11]={0,10,0,0x31,0,4,0,1,1,0,0};auto *stored=x9m_new(X9M_LIST,0,0,list,11);assert(stored);
 stored->follow=true;stored->store_order=1;poison_sms_inbox();
 uint8_t status[7]={0,11,0,0x5c};assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,status,7));
 assert(last_packet_len==18&&last_packet[14]==0x11&&!last_packet[17]);
 status[3]=0x30;assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,status,7)&&last_error==X9Q_NOT_READY);
 query[1]=12;assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,query,7)&&last_error==X9Q_NOT_READY);
 x9m_poll();assert(!writes&&!smsc->token&&!stored->token&&!x9m_sims[0].ready&&x9m_sims[0].md_ready);
}
static void sms_poison_send_pre_tx(){
 uint8_t req[7]={0,9,0,0x20};auto *j=x9m_new(X9M_SEND,0,0,req,7);assert(j);sms_send_pending(j);
 poison_sms_inbox();x9mi_tick();assert(!writes);x9m_poll();assert(replies==1&&last_error==X9Q_NOT_READY&&!j->op);
}
static void sms_poison_tag_pre_tx(unsigned step){
 uint8_t req[15]={0,9,0,0x23,0,8,0,1,5,0,0,1,0,0,0};auto *j=x9m_new(X9M_TAG,0,0,req,15);assert(j);
 j->step=step;j->token=x9m_store_submit(*j);assert(j->token);poison_sms_inbox();x9mi_tick();assert(!writes);x9m_poll();
 assert(replies==1&&last_error==X9Q_NOT_READY&&!j->token);
 if(step==2)assert(j->op==X9M_TAG&&j->hold);else assert(!j->op);
}
static void sms_poison_tag_before_mutation(){sms_poison_tag_pre_tx(1);}
static void sms_poison_tag_after_mutation(){sms_poison_tag_pre_tx(2);}
static void sms_poison_sent_mo_drain(){
 uint8_t req[7]={0,9,0,0x20};auto *j=x9m_new(X9M_SEND,0,0,req,7);assert(j);uint64_t token=sms_send_pending(j);x9mi_tick();
 poison_sms_inbox();uint8_t p[32];x9cw_header(p,32,0x606,1,(unsigned)token);x9cw_scalar(p+16,0,0,4);x9cw_scalar(p+24,0x103,1,1);
 x9mi_rx(p,32);x9m_poll();assert(writes==1&&replies==1&&!last_error&&!j->op&&x9_mm_poisoned);
}
static void sms_poison_selected_ack_drain(){
 uint8_t p[24];x9cw_header(p,24,0x8601,1,44);x9cw_scalar(p+16,0x100,3,1);x9mi_rx(p,24);
 uint32_t n,token;assert(x9_md_mipc_message_event(p,24,&n,&token)==1&&token);
 assert(x9_md_mipc_message_reply(token,1,0)==1);
 uint8_t req[7]={0,9,0,0x37};auto *j=x9m_new(X9M_ACK,0,0,req,7);assert(j);j->token=token;
 x9m_incoming_sms[0].token=token;x9m_incoming_sms[0].delivered=x9m_incoming_sms[0].replying=true;
 sms_send_pending(nullptr);during_write=poison_sms_inbox;x9mi_tick();during_write=nullptr;
 assert(writes==1&&x9_mm_poisoned);x9m_poll();assert(replies==1&&!last_error&&!j->op&&!x9m_incoming_sms[0].token);
 x9mi_tick();assert(writes==1); // remaining unsent MO is locally rejected, not transmitted
}
static void sms_poison_unknown_token_retained(){
 x9m_init_stage=1;x9m_init_token=55;x9m_init_deadline=100;poison_sms_inbox();x9m_poll();
 assert(x9m_init_hold&&x9m_init_token==55&&!writes);
}
/* Synthetic CMD8603: these vectors compile the real RX/event/publish gate.
 * Host queue stub records ownership only; allocation/free stay outside MD. */
static uint32_t publication_cmd(unsigned mask=1){
 uint8_t p[24],out[24];x9cw_header(p,24,0x8603,mask,77);
 const uint8_t pdu[]={0,2,0};x9cw_put16(p+16,0x8100);x9cw_put16(p+18,3);memcpy(p+20,pdu,3);
 x9mi_rx(p,24);uint32_t n=0,token=0;assert(x9_md_mipc_message_event(out,24,&n,&token)==1&&token&&n==24);
 return token;
}
static void publication_route(unsigned sim=0){
 x9q_clients[0].generation=9;x9q_clients[0].sim_slot=(uint8_t)sim;
 auto &c=x9m_clients[0];c.events=c.routes=c.status_reports=true;c.route_mask=1u<<4;c.route[4]=1;
}
static x9m_incoming &publication_pending(uint32_t token,unsigned sim=0){
 auto &in=x9m_incoming_sms[0];in.token=token;in.sim=sim;in.len=3;in.pdu[0]=0;in.pdu[1]=2;in.pdu[2]=0;return in;
}
static void publication_release(){assert(queued_frame);free(queued_frame);queued_frame=nullptr;}
static int publication_receipt(void *context){assert(lock_depth);++*static_cast<unsigned *>(context);return 1;}
static void sms_publish_native_receipt(){
 uint32_t token=publication_cmd();auto original=x9_mm_slots[0];unsigned called=0;
 assert(x9_md_mipc_message_publish(token,0,publication_receipt,&called)==1&&called==1&&!writes);
 assert(!memcmp(&original,&x9_mm_slots[0],sizeof(original))); // no CMD mutation/ACK/retirement
}
static void sms_publish_native_invalid(){
 uint32_t token=publication_cmd();unsigned called=0;
 assert(!x9_md_mipc_message_publish(token+1,0,publication_receipt,&called));
 assert(!x9_md_mipc_message_publish(token,1,publication_receipt,&called));
 assert(!x9_md_mipc_message_publish(token,2,publication_receipt,&called));
 assert(!x9_md_mipc_message_publish(token,0,nullptr,&called));
 for(unsigned state:{1u,3u,4u,5u,6u}){
  x9_mm_slots[0].state=state;assert(!x9_md_mipc_message_publish(token,0,publication_receipt,&called));
 }
 assert(!called&&!writes);
}
static void sms_publish_native_non_cs(){
 uint8_t p[24],out[24];x9cw_header(p,24,0x8601,1,78);x9cw_scalar(p+16,0x100,4,1);x9mi_rx(p,24);
 uint32_t n=0,token=0;assert(x9_md_mipc_message_event(out,24,&n,&token)==1&&token);unsigned called=0;
 assert(!x9_md_mipc_message_publish(token,0,publication_receipt,&called)&&!called&&x9_mm_slots[0].state==2&&!writes);
}
static void publication_changed_cmd(){
 uint8_t p[24];x9cw_header(p,24,0x8603,1,77);x9cw_put16(p+16,0x8100);x9cw_put16(p+18,3);
 p[20]=0;p[21]=2;p[22]=1;x9mi_rx(p,24);assert(x9_mm_poisoned);
}
static void sms_publish_event_poison(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);publication_changed_cmd();
 x9m_deliver(in,2);assert(!queue_calls&&!in.delivered&&in.token==token&&in.pdu[1]==2&&allocations==frees&&x9_mm_slots[0].state==2);
}
static void sms_publish_allocation_poison(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);during_allocation=publication_changed_cmd;
 x9m_deliver(in,2);during_allocation=nullptr;
 assert(!queue_calls&&!in.delivered&&in.token==token&&in.pdu[1]==2&&allocations==frees&&x9_mm_slots[0].state==2);
}
static void sms_publish_control_loss(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);x9w_control_ready=false;
 x9m_poll();assert(!queue_calls&&!allocations&&!in.delivered&&in.token==token&&in.pdu[1]==2&&x9_mm_slots[0].state==2&&!writes);
}
static void sms_publish_transport_loss(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);x9mi_online(0);
 x9m_deliver(in,2);assert(!queue_calls&&!in.delivered&&in.token==token&&in.pdu[1]==2&&allocations==frees);
}
static void sms_publish_at_loss(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);at_online=false;
 x9m_deliver(in,2);assert(!queue_calls&&!in.delivered&&in.token==token&&in.pdu[1]==2&&allocations==frees);
}
static void sms_publish_queue_retry(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);queue_accept=false;x9m_deliver(in,2);
 assert(queue_calls==1&&!in.delivered&&in.token==token&&in.pdu[1]==2&&allocations==frees);
 fake_ms=in.next_delivery;queue_accept=true;x9m_deliver(in,2);
 assert(queue_calls==2&&in.delivered&&in.token==token&&in.generation==9&&!in.pdu[1]&&allocations==frees+1&&x9_mm_slots[0].state==2&&!writes);
 publication_release();
}
static void sms_publish_queue_retry_poison(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);queue_accept=false;x9m_deliver(in,2);
 fake_ms=in.next_delivery;queue_accept=true;publication_changed_cmd();x9m_poll();
 assert(queue_calls==1&&!in.delivered&&in.token==token&&in.pdu[1]==2&&allocations==frees&&x9_mm_slots[0].state==2&&!writes);
}
static void sms_publish_oom(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);allocation_accept=false;x9m_deliver(in,2);
 assert(!queue_calls&&!allocations&&!frees&&!in.delivered&&in.token==token&&in.pdu[1]==2&&!writes);
}
static void sms_publish_high_mask(){
 uint32_t token=publication_cmd(0x22);publication_route(1);auto &in=publication_pending(token,1);x9m_deliver(in,2);
 assert(queue_calls==1&&in.delivered&&x9_mm_slots[0].state==2&&x9_mm_slots[0].mask==0x22&&!writes);publication_release();
}
static void sms_publish_delivered_poison(){
 uint32_t token=publication_cmd();publication_route();auto &in=publication_pending(token);x9m_deliver(in,2);assert(in.delivered);
 publication_changed_cmd();x9m_del(0);publication_route();x9q_clients[0].generation++;x9m_poll();
 assert(queue_calls==1&&in.delivered&&in.generation==9&&in.token==token&&x9_mm_slots[0].state==2&&!writes);
 assert(x9_md_mipc_message_reply(token,1,0)==0&&x9_mm_slots[0].state==2);publication_release();
}
/* Full synthetic Apple23 -> native indexed pre-read -> guarded mark-read.
 * No captured PDU. Native record is a bounded unread MT with index1. */
static void tag_record_cnf(uint64_t token,unsigned tag){
 uint8_t p[312];x9cw_header(p,sizeof(p),0x608,1,(unsigned)token);
 x9cw_scalar(p+16,0,0,4);x9cw_scalar(p+24,0x100,3,1);x9cw_scalar(p+32,0x103,1,2);
 x9cw_put16(p+40,0x8104);x9cw_put16(p+42,264);p[44]=1;p[46]=2;p[47]=(uint8_t)tag;
 x9mi_rx(p,sizeof(p));
}
static x9m_job &tag_pre_read(){
 x9m_init_stage=2;x9m_sims[0].ready=x9m_sims[0].md_known=x9m_sims[0].md_ready=true;x9m_sims[0].init=X9M_INIT_DONE;
 x9q_clients[0].generation=9;
 const uint8_t req[20]={0,9,0,0x23,0,13,0,1,6,0,0,1,0,0,0,0,0x10,1,0,1};
 struct x9ms_storage_request decoded={};assert(x9ms_storage_decode(req,sizeof(req),&decoded)==X9MS_OK&&decoded.modify);
 assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,req,sizeof(req))&&!replies);
 auto &j=x9m_jobs[0];assert(j.op==X9M_TAG&&j.follow);x9m_poll();assert(j.token&&j.step==0);
 x9mi_tick();assert(writes==1);tag_record_cnf(j.token,0);x9m_poll();
 assert(j.op==X9M_TAG&&j.step==1&&!j.token&&j.follow&&j.stored_len==3&&!j.answered);
 return j;
}
static x9m_job &tag_mutation_queued(){
 auto &j=tag_pre_read();x9m_poll();assert(j.token&&j.step==1&&!j.follow&&writes==1);
 const auto &s=x9_mi_slots[0];assert(s.token==j.token&&s.state==1&&s.sms_guard&&s.sms_owner==j.slot&&s.sms_generation==j.generation);
 assert(s.sms_deadline_ms==j.deadline&&x9mm_one(s.frame,s.len,0x103,0));return j;
}
static void sms_tag_guard_admission_deadline(){
 auto &j=tag_pre_read();fake_ms=j.deadline;unsigned next=x9_mi_next;
 assert(!x9m_store_submit(j)&&x9_mi_next==next&&writes==1);x9m_poll();assert(!j.op&&last_error==X9Q_NOT_READY);
}
static void sms_tag_guard_writer_deadline(){
 auto &j=tag_mutation_queued();fake_ms=j.deadline;x9mi_tick(); // no adapter poll: native writer is decisive
 assert(writes==1&&x9_mi_slots[0].state==6);x9m_poll();assert(!j.op&&replies==1&&last_error==X9Q_NOT_READY);
}
static void sms_tag_guard_delete_before_select(){
 auto &j=tag_mutation_queued();x9m_del(0);assert(j.answered&&x9_mi_slots[0].state==6);
 x9q_clients[0].generation++;x9mi_tick();x9m_poll();assert(writes==1&&!j.op&&!replies);
}
static void sms_tag_guard_owner_identity(){
 auto &j=tag_mutation_queued();
 x9_md_mipc_message_mark_read_abandon(j.token+1,j.slot,j.generation);
 x9_md_mipc_message_mark_read_abandon(j.token,j.slot+1,j.generation);
 x9_md_mipc_message_mark_read_abandon(j.token,j.slot,j.generation+1);
 assert(x9_mi_slots[0].state==1);x9mi_tick();assert(writes==2&&x9_mi_slots[0].state==2);
}
static void sms_tag_guard_control_loss(){
 auto &j=tag_mutation_queued();x9w_control_ready=false;x9m_poll();x9mi_tick();
 assert(writes==1&&!j.op&&last_error==X9Q_NOT_READY);
}
static void sms_tag_guard_selected_late(){
 auto &j=tag_mutation_queued();uint64_t token=j.token;x9mi_tick();assert(writes==2&&x9_mi_slots[0].state==2);
 fake_ms=j.deadline;x9m_del(0);x9q_clients[0].generation++;x9m_poll();
 assert(j.token==token&&j.op==X9M_TAG&&x9_mi_slots[0].state==2&&!replies);
 tag_record_cnf(token,0);x9m_poll();assert(j.step==2&&j.follow&&!j.token);
 x9m_poll();assert(j.op==X9M_TAG&&j.hold&&!j.token&&writes==2); // prior physical mutation remains held
}
static x9m_job *tag_during_tx;
static void tag_delete_during_tx(){assert(tag_during_tx);fake_ms=tag_during_tx->deadline;x9m_del(0);}
static void sms_tag_guard_partial_late(){
 auto &j=tag_mutation_queued();uint64_t token=j.token;tag_during_tx=&j;during_write=tag_delete_during_tx;x9mi_tick();during_write=nullptr;
 assert(writes==2&&x9_mi_slots[0].state==2&&j.token==token&&j.answered);
 tag_record_cnf(token,0);x9m_poll();x9m_poll();assert(j.op==X9M_TAG&&j.step==2&&j.hold&&writes==2);
}
static void sms_tag_guard_followup_irrevocable(){
 auto &j=tag_mutation_queued();x9mi_tick();tag_record_cnf(j.token,0);x9m_poll();assert(j.step==2&&!j.token);
 x9m_poll();uint64_t verification=j.token;assert(verification&&x9_mi_slots[0].state==1&&!x9_mi_slots[0].sms_guard);
 fake_ms=j.deadline;x9m_del(0);x9m_poll();assert(j.token==verification&&j.op==X9M_TAG);
 x9mi_tick();assert(writes==3&&x9_mi_slots[0].state==2); // original readback still drains
 tag_record_cnf(verification,1);x9m_poll();assert(!j.op&&!replies&&writes==3);
}
static void sms_tag_guard_generic_unaffected(){
 uint8_t p[56];x9cw_header(p,sizeof(p),0x607,1,0);x9cw_scalar(p+16,0x100,3,1);x9cw_scalar(p+24,0x101,5,1);
 x9cw_scalar(p+32,0x102,1,2);x9cw_scalar(p+40,0x103,1,1);x9cw_scalar(p+48,0x104,0,1);
 assert(!x9_md_mipc_message_mark_read_submit(p,sizeof(p),0,9,100)); // guarded API must not accept a read-only frame
 uint64_t token=x9_md_mipc_submit(p,sizeof(p));assert(token&&!x9_mi_slots[0].sms_guard);
 x9_md_mipc_message_mark_read_abandon(token,0,9);fake_ms=100;x9mi_tick();assert(writes==1&&x9_mi_slots[0].state==2);
}
static x9m_job &ack_result_pending(int result=0){
 auto &s=x9_mm_slots[0];s.token=7;s.state=5;s.result=result;s.id=0x8601;
 uint8_t req[7]={0,9,0,0x37};auto *j=x9m_new(X9M_ACK,0,0,req,7);assert(j);j->token=7;
 auto &in=x9m_incoming_sms[0];in.token=7;in.delivered=in.replying=true;return *j;
}
static void release_ack_receipt(){assert(queued_frame);IOFreeData(queued_frame,20);queued_frame=nullptr;}
static void sms_ack_completion_queue_retry(){
 auto &j=ack_result_pending();queue_accept=false;x9m_poll();
 assert(j.op==X9M_ACK&&j.ack_done&&j.ack_reply_pending&&!j.answered&&!j.ack_error&&!replies&&!writes);
 assert(!x9_mm_slots[0].state&&!x9m_incoming_sms[0].token); // native success retired, pure reply retained
 queue_accept=true;x9m_poll();assert(!j.op&&replies==1&&!last_error&&!writes);release_ack_receipt();
 x9m_poll();assert(replies==1&&!writes);
}
static void sms_ack_completion_oom(){
 auto &j=ack_result_pending();allocation_accept=false;x9m_poll();
 assert(j.ack_done&&j.ack_reply_pending&&!j.answered&&!ack_queue_calls&&!replies&&!writes);
 allocation_accept=true;x9m_poll();assert(!j.op&&replies==1&&!writes);release_ack_receipt();
}
static void sms_ack_completion_original_result(){
 auto &j=ack_result_pending();queue_accept=false;x9m_poll();fake_ms=j.deadline;
 queue_accept=true;x9m_poll();assert(!j.op&&replies==1&&!last_error&&!writes);release_ack_receipt();
}
static void sms_ack_completion_timeout_late(){
 auto &j=ack_result_pending();x9_mm_slots[0].state=4;fake_ms=j.deadline;queue_accept=false;x9m_poll();
 assert(!j.ack_done&&j.ack_reply_pending&&j.ack_error==X9Q_NOT_READY&&!replies);
 x9mm_tx_done(7,0);queue_accept=true;x9m_poll();
 assert(!j.op&&replies==1&&last_error==X9Q_NOT_READY&&!writes);release_ack_receipt();
}
static uint64_t ack_lock_deadline;
static void ack_cross_deadline(){fake_ms=ack_lock_deadline;during_md_lock=nullptr;}
static void sms_ack_completion_lock_deadline(){
 auto &j=ack_result_pending();ack_lock_deadline=j.deadline;during_md_lock=ack_cross_deadline;x9m_poll();
 assert(!j.op&&replies==1&&last_error==X9Q_NOT_READY&&!writes);release_ack_receipt();
}
static void sms_ack_completion_delete(){
 auto &j=ack_result_pending();queue_accept=false;x9m_poll();x9m_del(0);x9q_clients[0].generation++;
 queue_accept=true;x9m_poll();assert(!j.op&&!replies&&!writes&&!queued_frame);
}
static void sms_ack_completion_bridge_reuse(){
 auto &j=ack_result_pending();queue_accept=false;x9m_poll();bridge_ack_generation++;
 queue_accept=true;x9m_poll();assert(j.ack_done&&!j.answered&&!replies&&!writes&&!queued_frame);
 x9m_del(0);x9m_poll();assert(!j.op&&!replies);
}
static void sms_ack_completion_release_pending(){
 auto &j=ack_result_pending();bridge_ack_live=false;x9m_poll();
 assert(j.ack_done&&j.ack_reply_pending&&!j.answered&&!replies&&!queued_frame);
 x9m_del(0);x9m_poll();assert(!j.op&&!writes&&!replies);
}
static void sms_ack_completion_physical_failure(){
 auto &j=ack_result_pending(X9_AT_OFFLINE);queue_accept=false;x9m_poll();
 assert(j.ack_done&&j.ack_error==X9Q_NOT_READY&&x9_mm_slots[0].state==6&&x9m_incoming_sms[0].held);
 queue_accept=true;x9m_poll();assert(!j.op&&replies==1&&last_error==X9Q_NOT_READY&&!writes);release_ack_receipt();
 assert(x9m_incoming_sms[0].token==7&&x9_mm_slots[0].token==7);
}
static x9m_job &ack_command_admitted(uint8_t req[16]){
 uint8_t p[24],out[24];x9cw_header(p,24,0x8601,1,44);x9cw_scalar(p+16,0x100,3,1);x9mi_rx(p,24);
 uint32_t n=0,token=0;assert(x9_md_mipc_message_event(out,24,&n,&token)==1&&token);
 auto &in=x9m_incoming_sms[0];in.token=token;in.delivered=true;in.generation=x9q_clients[0].generation;
 bzero(req,16);req[1]=9;req[3]=0x37;req[5]=9;req[7]=1;req[8]=6;x9cw_put32(req+10,token);req[14]=req[15]=1;
 assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,req,16)&&!replies&&!writes);
 auto &j=x9m_jobs[0];assert(j.op==X9M_ACK&&j.token==token&&x9_mm_slots[0].state==3&&in.replying);return j;
}
static void sms_ack_retry_before_physical(bool changed){
 uint8_t req[16];auto &j=ack_command_admitted(req);auto original=x9_mm_slots[0];
 if(changed)req[15]=0;assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,req,16));
 assert(!replies&&!writes&&j.op==X9M_ACK&&!j.ack_done&&!j.ack_reply_pending&&!j.answered);
 assert(!memcmp(&original,&x9_mm_slots[0],sizeof(original))&&j.req[15]==1&&!x9m_jobs[1].op);
}
static void sms_ack_retry_before_identical(){sms_ack_retry_before_physical(false);}
static void sms_ack_retry_before_changed(){sms_ack_retry_before_physical(true);}
static void sms_ack_retry_after_physical(bool changed){
 uint8_t req[16];auto &j=ack_command_admitted(req);queue_accept=false;x9mi_tick();x9m_poll();
 assert(writes==1&&j.ack_done&&j.ack_reply_pending&&!j.answered&&!x9m_incoming_sms[0].token&&!x9_mm_slots[0].state);
 if(changed)req[15]=0;assert(x9m_request(CBQ_PORT_BASE,X9BB_PORT_BASE+5,req,16));
 assert(!replies&&writes==1&&j.ack_error==0&&j.req[15]==1&&!x9m_jobs[1].op);
 queue_accept=true;x9m_poll();assert(!j.op&&replies==1&&!last_error&&writes==1&&x9q_u16(last_packet+1)==9);
 release_ack_receipt();x9mi_tick();assert(writes==1&&replies==1);
}
static void sms_ack_retry_after_identical(){sms_ack_retry_after_physical(false);}
static void sms_ack_retry_after_changed(){sms_ack_retry_after_physical(true);}
static void sms_ack_retry_malformed_before(){
 uint8_t req[16];auto &j=ack_command_admitted(req);uint8_t malformed[20];memcpy(malformed,req,16);
 malformed[5]=13;malformed[16]=1;malformed[17]=1;malformed[18]=0;malformed[19]=0;
 assert(x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,malformed,sizeof(malformed)));
 assert(!replies&&!writes&&j.op==X9M_ACK&&!j.ack_reply_pending&&j.len==16&&x9_mm_slots[0].state==3);
}
static void sms_ack_retry_malformed_after(){
 uint8_t req[16];auto &j=ack_command_admitted(req);queue_accept=false;x9mi_tick();x9m_poll();req[5]=8;
 assert(x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,req,sizeof(req)));
 assert(!replies&&writes==1&&j.ack_done&&j.ack_error==0);queue_accept=true;x9m_poll();
 assert(!j.op&&replies==1&&!last_error&&writes==1);release_ack_receipt();
}
static void sms_ack_retry_entry_scope(){
 uint8_t req[16];auto &j=ack_command_admitted(req);
 assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE+1,X9BB_PORT_BASE+5,req,16));
 assert(!x9m_ack_repeated(CBQ_PORT_BASE-1,X9BB_NODE,X9BB_PORT_BASE+5,req,16));
 assert(!x9m_ack_repeated(CBQ_PORT_BASE+CBQ_MAX_CLIENTS,X9BB_NODE,X9BB_PORT_BASE+5,req,16));
 assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+0x21,req,16));
 assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,nullptr,16));
 assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,req,6));
 req[0]=2;assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,req,16));req[0]=0;
 req[3]=0x20;assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,req,16));req[3]=0x37;
 req[1]++;assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,req,16));req[1]--;
 x9q_clients[0].generation++;assert(!x9m_ack_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+5,req,16));
 assert(j.op==X9M_ACK&&!replies&&!writes&&x9_mm_slots[0].state==3);
}
int main(int argc,char **argv){
 assert(argc==2);x9mi_online(1);x9w_control_ready=true;
 struct {const char *name;void (*run)();} cases[]={
  {"global_exact_deadline",global_exact_deadline},{"global_late_cnf",global_late_cnf},
  {"sim_exact_deadline",sim_exact_deadline},{"sim_independent_deadlines",sim_independent_deadlines},
  {"init_offline_drain",init_offline_drain},{"job_send_deadline",job_send_deadline},{"job_ack_deadline",job_ack_deadline},
  {"queued_control_loss",queued_control_loss},{"sms_poison_admission_scope",sms_poison_admission_scope},
  {"sms_poison_global_drain",sms_poison_global_drain},{"sms_poison_host_ready_drain",sms_poison_host_ready_drain},
  {"sms_poison_global_pre_tx",sms_poison_global_pre_tx},{"sms_poison_sim_pre_tx",sms_poison_sim_pre_tx},
  {"sms_poison_queued_and_ready",sms_poison_queued_and_ready},{"sms_poison_send_pre_tx",sms_poison_send_pre_tx},
  {"sms_poison_tag_before_mutation",sms_poison_tag_before_mutation},{"sms_poison_tag_after_mutation",sms_poison_tag_after_mutation},
  {"sms_poison_sent_mo_drain",sms_poison_sent_mo_drain},{"sms_poison_selected_ack_drain",sms_poison_selected_ack_drain},
  {"sms_poison_unknown_token_retained",sms_poison_unknown_token_retained},
  {"sms_publish_native_receipt",sms_publish_native_receipt},{"sms_publish_native_invalid",sms_publish_native_invalid},
  {"sms_publish_native_non_cs",sms_publish_native_non_cs},
  {"sms_publish_event_poison",sms_publish_event_poison},{"sms_publish_allocation_poison",sms_publish_allocation_poison},
  {"sms_publish_control_loss",sms_publish_control_loss},{"sms_publish_transport_loss",sms_publish_transport_loss},
  {"sms_publish_at_loss",sms_publish_at_loss},{"sms_publish_queue_retry",sms_publish_queue_retry},
  {"sms_publish_queue_retry_poison",sms_publish_queue_retry_poison},{"sms_publish_oom",sms_publish_oom},
  {"sms_publish_high_mask",sms_publish_high_mask},{"sms_publish_delivered_poison",sms_publish_delivered_poison},
  {"sms_tag_guard_admission_deadline",sms_tag_guard_admission_deadline},{"sms_tag_guard_writer_deadline",sms_tag_guard_writer_deadline},
  {"sms_tag_guard_delete_before_select",sms_tag_guard_delete_before_select},{"sms_tag_guard_owner_identity",sms_tag_guard_owner_identity},
  {"sms_tag_guard_control_loss",sms_tag_guard_control_loss},{"sms_tag_guard_selected_late",sms_tag_guard_selected_late},
  {"sms_tag_guard_partial_late",sms_tag_guard_partial_late},{"sms_tag_guard_followup_irrevocable",sms_tag_guard_followup_irrevocable},
  {"sms_tag_guard_generic_unaffected",sms_tag_guard_generic_unaffected},
  {"sms_ack_completion_queue_retry",sms_ack_completion_queue_retry},{"sms_ack_completion_oom",sms_ack_completion_oom},
  {"sms_ack_completion_original_result",sms_ack_completion_original_result},{"sms_ack_completion_timeout_late",sms_ack_completion_timeout_late},
  {"sms_ack_completion_lock_deadline",sms_ack_completion_lock_deadline},
  {"sms_ack_completion_delete",sms_ack_completion_delete},{"sms_ack_completion_bridge_reuse",sms_ack_completion_bridge_reuse},
  {"sms_ack_completion_release_pending",sms_ack_completion_release_pending},{"sms_ack_completion_physical_failure",sms_ack_completion_physical_failure},
  {"sms_ack_retry_before_identical",sms_ack_retry_before_identical},{"sms_ack_retry_before_changed",sms_ack_retry_before_changed},
  {"sms_ack_retry_after_identical",sms_ack_retry_after_identical},{"sms_ack_retry_after_changed",sms_ack_retry_after_changed},
  {"sms_ack_retry_malformed_before",sms_ack_retry_malformed_before},{"sms_ack_retry_malformed_after",sms_ack_retry_malformed_after},
  {"sms_ack_retry_entry_scope",sms_ack_retry_entry_scope}};
 for(auto &c:cases)if(!strcmp(argv[1],c.name)){c.run();return 0;}return 2;
}
