/* Compile-only synthetic vectors for the actual SMS owner and MD transport.
 * No device PDU, vendor bytes, network, test runner or executable invocation. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "../../kernel/src/bsd/dev/x9_modem_bridge.h"
static int lock_depth;
#define LCK_GRP_DECLARE(n, s) int n
#define LCK_MTX_DECLARE(n, g) int n
static void lck_mtx_lock(int *){assert(!lock_depth);lock_depth=1;}
static void lck_mtx_unlock(int *){assert(lock_depth);lock_depth=0;}
static int x9_at_enabled(){return 1;}
static int sweet_x9_arg(const char *,int){return 1;}
int x9_md_at_online(){return 1;}
static unsigned writes;
static int x9md_tx(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,const uint8_t *,unsigned n){
 assert(!lock_depth&&a==5&&b==217&&c==0&&d==16+n&&e==0);writes++;return 0;
}
#include "../../kernel/src/bsd/dev/x9_mipc_bridge.inc"
static const unsigned X9BB_PORT_BASE=0x2000,X9BB_NODE=1,CBQ_PORT_BASE=0x4000,CBQ_MAX_CLIENTS=8,X9Q_REQ=1024;
static const uint16_t X9Q_INTERNAL=3,X9Q_NOT_READY=16,X9Q_ARG=48,X9Q_BUSY=2,X9Q_UNSUPPORTED=94;
static struct {uint64_t generation;uint8_t sim_slot;} x9q_clients[CBQ_MAX_CLIENTS];
static uint64_t fake_ms;
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
struct x9q_packet {
 uint8_t p[1536];unsigned n;bool good;uint16_t error;
 x9q_packet(const uint8_t *req,uint16_t e=0):n(14),good(true),error(e){bzero(p,sizeof(p));memcpy(p+1,req+1,4);}
 void tlv(unsigned tag,const void *v,unsigned k){assert(k<=sizeof(p)-n-3);p[n]=(uint8_t)tag;x9cw_put16(p+n+1,k);memcpy(p+n+3,v,k);n+=3+k;}
 void send(uint32_t,uint32_t,bool=false,uint16_t=0){replies++;last_error=error;}
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
static bool bb_x9_sms_event(uint32_t,uint32_t,const void *,unsigned){return true;}
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
int main(int argc,char **argv){
 assert(argc==2);x9mi_online(1);x9w_control_ready=true;
 struct {const char *name;void (*run)();} cases[]={
  {"global_exact_deadline",global_exact_deadline},{"global_late_cnf",global_late_cnf},
  {"sim_exact_deadline",sim_exact_deadline},{"sim_independent_deadlines",sim_independent_deadlines},
  {"init_offline_drain",init_offline_drain},{"job_send_deadline",job_send_deadline},{"job_ack_deadline",job_ack_deadline},
  {"queued_control_loss",queued_control_loss}};
 for(auto &c:cases)if(!strcmp(argv[1],c.name)){c.run();return 0;}return 2;
}
