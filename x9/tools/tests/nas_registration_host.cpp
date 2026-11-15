/* Compile-only synthetic vectors for the actual NAS03 owner. No test runner,
 * executable invocation, device bytes, MD command or network operation. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <strings.h>
static constexpr unsigned X9Q_JOBS=32,X9Q_REQ=1024,CBQ_MAX_CLIENTS=40,CBQ_HDR=6;
static constexpr unsigned CBQ_PORT_BASE=0x4100,X9BB_PORT_BASE=0x2000,X9BB_NODE=1;
static constexpr uint16_t X9Q_ARG=48,X9Q_BUSY=2,X9Q_NOT_READY=16;
static uint16_t x9q_u16(const uint8_t *p){return (uint16_t)(p[0]|p[1]<<8);}
struct x9q_client {uint64_t generation,next;uint32_t uim_mask;uint8_t sim_slot;bool serving,system,signal,dsd_system;};
static x9q_client x9q_clients[CBQ_MAX_CLIENTS];
struct x9q_packet {
 uint8_t p[1536];unsigned n;bool good;
 x9q_packet(const uint8_t *req,uint16_t error=0):n(7),good(true){
  bzero(p,sizeof(p));p[0]=2;memcpy(p+1,req+1,4);
  uint8_t r[4]={(uint8_t)(error!=0),0,(uint8_t)error,(uint8_t)(error>>8)};tlv(2,r,4);
 }
 void tlv(uint8_t tag,const void *v,unsigned len){
  if(!good||len>sizeof(p)-n-3){good=false;return;}
  p[n]=tag;p[n+1]=(uint8_t)len;p[n+2]=(uint8_t)(len>>8);if(len)memcpy(p+n+3,v,len);n+=3+len;
 }
};
static uint64_t bridge_generation[CBQ_MAX_CLIENTS];
static bool live[CBQ_MAX_CLIENTS],allocation_accept,queue_accept;
static unsigned allocations,frees,receipts,attempts;
static uint16_t received_txn[80];static uint8_t received_sdu[14];
static void (*during_allocation)();
static void *IOMallocData(unsigned n){
 if(during_allocation)during_allocation();
 if(!allocation_accept)return nullptr;allocations++;return malloc(n);
}
static void IOFreeData(void *p,unsigned){frees++;free(p);}
static uint64_t bb_x9_nas_registration_identity(uint32_t local,uint32_t node){
 const unsigned slot=local-CBQ_PORT_BASE;
 return node==X9BB_NODE&&local>=CBQ_PORT_BASE&&slot<CBQ_MAX_CLIENTS&&live[slot]?bridge_generation[slot]:0;
}
static bool bb_x9_nas_registration_result(uint32_t local,uint32_t node,uint64_t generation,
                                          const uint8_t *p,unsigned n,uint8_t *owned,unsigned room){
 attempts++;assert(n==14&&room==CBQ_HDR+n&&p[0]==2&&x9q_u16(p+3)==3&&x9q_u16(p+5)==7);
 assert(p[7]==2&&x9q_u16(p+8)==4&&x9q_u16(p+10)==0&&x9q_u16(p+12)==0);
 if(!queue_accept||bb_x9_nas_registration_identity(local,node)!=generation)return false;
 received_txn[receipts++]=x9q_u16(p+1);memcpy(received_sdu,p,n);IOFreeData(owned,room);return true;
}
#include "../../kernel/src/iokit/Kernel/arm/X9NASRegistration.inc"
struct request {
 uint8_t p[X9Q_REQ];unsigned n;
 explicit request(unsigned txn):n(7){bzero(p,sizeof(p));p[1]=(uint8_t)txn;p[2]=(uint8_t)(txn>>8);p[3]=3;}
 void tag(unsigned t,unsigned value,unsigned width=1){
  assert(n+3+width<=sizeof(p));p[n]=(uint8_t)t;p[n+1]=(uint8_t)width;p[n+2]=(uint8_t)(width>>8);
  if(width)p[n+3]=(uint8_t)value;n+=3+width;p[5]=(uint8_t)(n-7);p[6]=(uint8_t)((n-7)>>8);
 }
};
static unsigned pending(){unsigned n=0;for(const auto &w:x9r_pending)n+=w.used;return n;}
static void baseline(){
 bzero(x9r_pending,sizeof(x9r_pending));bzero(x9r_completed,sizeof(x9r_completed));
 bzero(x9q_clients,sizeof(x9q_clients));bzero(received_txn,sizeof(received_txn));x9r_order=0;
 allocations=frees=receipts=attempts=0;allocation_accept=queue_accept=true;during_allocation=nullptr;
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){live[i]=true;bridge_generation[i]=1;x9q_clients[i].next=100;}
}
static bool repeat(unsigned slot,const request &r,unsigned node=X9BB_NODE,unsigned port=X9BB_PORT_BASE+3){
 return x9r_repeated(CBQ_PORT_BASE+slot,node,port,r.p,r.n);
}
static void atomic_multi_tag_receipt(){
 baseline();request r(1);r.tag(0x13,1);r.tag(0x18,1);r.tag(0x19,1);assert(!x9r_begin(0,r.p,r.n));
 assert(pending()==1&&!receipts&&!x9q_clients[0].system&&x9q_clients[0].next==100);x9r_poll();
 const auto &c=x9q_clients[0];assert(c.serving&&c.system&&c.signal&&!c.next&&!pending()&&receipts==1);
 assert(x9r_completed[0].used&&received_txn[0]==1&&allocations==frees);
}
static void invalid_tail_is_atomic(){
 baseline();request r(1);r.tag(0x18,1);r.tag(0x19,2);assert(x9r_begin(0,r.p,r.n)==X9Q_ARG);
 assert(!pending()&&!x9q_clients[0].system&&x9q_clients[0].next==100&&!attempts);
}
static void widths_zero_two_reject(){
 for(unsigned width:{0u,2u}){baseline();request r(1);r.tag(0x13,1);r.tag(0x18,1,width);
  assert(x9r_begin(0,r.p,r.n)==X9Q_ARG&&!pending()&&!x9q_clients[0].serving);}
}
static void unknown_accepted_unserved(){
 baseline();request r(1);r.tag(0xa0,99,2);r.tag(0xd0,0,0);assert(!x9r_begin(0,r.p,r.n));x9r_poll();
 assert(receipts==1&&!x9q_clients[0].serving&&!x9q_clients[0].system&&!x9q_clients[0].signal);
}
static void empty_patch_receipt(){
 baseline();request r(1);assert(!x9r_begin(0,r.p,r.n));x9r_poll();assert(receipts==1&&!pending());
}
static void oom_preserves_owner(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));allocation_accept=false;x9r_poll();
 assert(pending()==1&&!attempts&&!receipts&&!x9q_clients[0].system&&x9q_clients[0].next==100);
 allocation_accept=true;x9r_poll();assert(!pending()&&receipts==1&&x9q_clients[0].system);
}
static void queue_full_preserves_result(){
 baseline();request r(257);r.tag(0x19,1);assert(!x9r_begin(0,r.p,r.n));queue_accept=false;
 for(unsigned i=0;i<3;i++)x9r_poll();assert(pending()==1&&attempts==3&&!receipts&&allocations==frees&&!x9q_clients[0].signal);
 queue_accept=true;x9r_poll();assert(receipts==1&&received_txn[0]==257&&x9q_clients[0].signal);
}
static void pending_duplicate_reuses_owner(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));queue_accept=false;
 assert(repeat(0,r)&&repeat(0,r)&&pending()==1);x9r_poll();assert(!receipts);queue_accept=true;x9r_poll();assert(receipts==1);
}
static void changed_duplicate_preserves_first(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));request changed=r;changed.p[10]=0;
 assert(repeat(0,changed)&&pending()==1);changed.n=7;assert(repeat(0,changed));x9r_poll();assert(x9q_clients[0].system&&receipts==1);
}
static void malformed_duplicate_scoped_before_validator(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));request malformed=r;malformed.p[5]=255;
 assert(repeat(0,malformed)&&pending()==1);malformed.n=X9Q_REQ+1;assert(repeat(0,malformed));
 assert(!repeat(0,r,2)&&!repeat(0,r,X9BB_NODE,X9BB_PORT_BASE+5));
 assert(!x9r_repeated(CBQ_PORT_BASE-1,X9BB_NODE,X9BB_PORT_BASE+3,r.p,r.n));
 assert(!x9r_repeated(CBQ_PORT_BASE,X9BB_NODE,X9BB_PORT_BASE+3,nullptr,7));
 request other(2);assert(!repeat(0,other));r.p[0]=4;assert(!repeat(0,r));
}
static void same_client_fifo_patch_not_snapshot(){
 baseline();request a(1),b(2),c(3);a.tag(0x18,1);b.tag(0x19,1);c.tag(0x18,0);
 assert(!x9r_begin(0,a.p,a.n)&&!x9r_begin(0,b.p,b.n)&&!x9r_begin(0,c.p,c.n));
 x9r_poll();assert(receipts==1&&received_txn[0]==1&&x9q_clients[0].system&&!x9q_clients[0].signal);
 x9r_poll();assert(receipts==2&&received_txn[1]==2&&x9q_clients[0].system&&x9q_clients[0].signal);
 x9r_poll();assert(receipts==3&&received_txn[2]==3&&!x9q_clients[0].system&&x9q_clients[0].signal&&!pending());
}
static void independent_clients_do_not_block(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n)&&!x9r_begin(1,r.p,r.n));x9r_poll();
 assert(receipts==2&&!pending()&&x9q_clients[0].system&&x9q_clients[1].system);
}
static void completed_duplicate_reply_only(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));x9r_poll();
 x9q_clients[0].next=321;x9q_clients[0].system=false;assert(repeat(0,r));x9r_poll();
 assert(receipts==2&&received_txn[0]==received_txn[1]&&!x9q_clients[0].system&&x9q_clients[0].next==321&&!pending());
}
static void completed_changed_is_not_replayed(){
 baseline();request r(1);r.tag(0x19,1);assert(!x9r_begin(0,r.p,r.n));x9r_poll();request changed=r;changed.p[10]=0;
 assert(repeat(0,changed));x9r_poll();assert(receipts==1&&x9q_clients[0].signal&&!x9r_completed[0].replay);
}
static void completed_oom_queue_retry(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));x9r_poll();assert(repeat(0,r));
 x9q_clients[0].next=456;allocation_accept=false;x9r_poll();assert(x9r_completed[0].replay&&receipts==1);
 allocation_accept=true;queue_accept=false;x9r_poll();assert(x9r_completed[0].replay&&receipts==1);
 queue_accept=true;x9r_poll();assert(!x9r_completed[0].replay&&receipts==2&&x9q_clients[0].next==456);
}
static void completed_caches_do_not_exhaust_pool(){
 baseline();request r(1);for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){
  assert(!x9r_begin(slot,r.p,r.n));x9r_poll();assert(!pending()&&x9r_completed[slot].used);}
 assert(receipts==CBQ_MAX_CLIENTS);
}
static void busy_and_bad_new_txn_preserve_cache(){
 baseline();request r(1);assert(!x9r_begin(0,r.p,r.n));x9r_poll();queue_accept=false;
 for(unsigned i=0;i<X9R_PENDING;i++){request q(10+i);assert(!x9r_begin(1,q.p,q.n));}
 request newtxn(2);newtxn.tag(0x18,1);assert(x9r_begin(0,newtxn.p,newtxn.n)==X9Q_BUSY&&x9r_completed[0].used);
 request bad(3);bad.tag(0x19,2);assert(x9r_begin(0,bad.p,bad.n)==X9Q_ARG&&x9r_completed[0].used);
 assert(repeat(0,r)&&x9r_completed[0].replay&&!x9q_clients[0].system);
}
static void admitted_new_txn_retires_cache(){
 baseline();request a(1),b(2);assert(!x9r_begin(0,a.p,a.n));x9r_poll();assert(x9r_completed[0].used);
 assert(!x9r_begin(0,b.p,b.n)&&!x9r_completed[0].used&&pending()==1);
}
static void delete_pending_and_completed(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));x9r_del(0);x9q_clients[0].generation++;
 x9r_poll();assert(!receipts&&!pending()&&!x9q_clients[0].system);
 assert(!x9r_begin(0,r.p,r.n));x9r_poll();assert(x9r_completed[0].used);x9r_del(0);assert(!x9r_completed[0].used);
}
static void generation_counters_have_different_origins(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));
 assert(x9r_pending[0].generation==0&&x9r_pending[0].bridge_generation==1);x9r_poll();assert(receipts==1);
}
static void real_generation_change_retires(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));bridge_generation[0]++;x9r_poll();
 assert(!pending()&&!receipts&&!x9q_clients[0].system);
}
static void local_generation_change_retires(){
 baseline();request r(1);r.tag(0x19,1);assert(!x9r_begin(0,r.p,r.n));x9q_clients[0].generation++;x9r_poll();
 assert(!pending()&&!receipts&&!x9q_clients[0].signal);
}
static void release_pending_retires(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));live[0]=false;x9r_poll();
 assert(!pending()&&!receipts&&!x9q_clients[0].system);
}
static void allocation_crosses_release(){live[0]=false;}
static void release_between_identity_and_receipt(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n));during_allocation=allocation_crosses_release;x9r_poll();
 assert(!receipts&&!x9q_clients[0].system&&allocations==frees);during_allocation=nullptr;x9r_poll();assert(!pending());
}
static void allocation_crosses_local_generation(){x9q_clients[0].generation++;}
static void local_generation_between_identity_and_receipt(){
 baseline();request r(1);r.tag(0x19,1);assert(!x9r_begin(0,r.p,r.n));during_allocation=allocation_crosses_local_generation;x9r_poll();
 assert(!receipts&&!attempts&&!x9q_clients[0].signal&&allocations==frees);
}
static void bind_pending_busy_completed_retired(){
 baseline();request r(1);r.tag(0x18,1);assert(!x9r_begin(0,r.p,r.n)&&x9r_binding_busy(0));
 x9r_poll();assert(!x9r_binding_busy(0)&&x9r_completed[0].used);x9r_rebound(0);x9q_clients[0].sim_slot=1;
 assert(!x9r_completed[0].used&&!repeat(0,r)&&x9q_clients[0].system);
}
static void disable_controls_old_indication_only_after_receipt(){
 baseline();x9q_clients[0].serving=x9q_clients[0].system=x9q_clients[0].signal=true;
 request r(1);r.tag(0x13,0);r.tag(0x18,0);r.tag(0x19,0);assert(!x9r_begin(0,r.p,r.n));queue_accept=false;x9r_poll();
 for(unsigned id:{0x24u,0x4du,0x4fu})assert(x9r_indication_allowed(CBQ_PORT_BASE,0,0,(uint16_t)id));
 queue_accept=true;x9r_poll();for(unsigned id:{0x24u,0x4du,0x4fu})assert(!x9r_indication_allowed(CBQ_PORT_BASE,0,0,(uint16_t)id));
}
static void indication_fences_generation_sim_and_flag(){
 baseline();x9q_clients[0].signal=true;assert(x9r_indication_allowed(CBQ_PORT_BASE,0,0,0x4f));
 assert(!x9r_indication_allowed(CBQ_PORT_BASE,1,0,0x4f)&&!x9r_indication_allowed(CBQ_PORT_BASE,0,1,0x4f));
 assert(!x9r_indication_allowed(CBQ_PORT_BASE,0,0,0x4d)&&!x9r_indication_allowed(CBQ_PORT_BASE-1,0,0,0x4f));
}
static void exhausted_order_preserves_completed_cache(){
 baseline();request a(1),b(2);assert(!x9r_begin(0,a.p,a.n));x9r_poll();x9q_clients[0].next=789;
 x9r_order=UINT64_MAX;b.tag(0x18,1);assert(x9r_begin(0,b.p,b.n)==X9Q_BUSY);
 assert(x9r_order==UINT64_MAX&&!pending()&&x9r_completed[0].used&&x9q_clients[0].next==789&&!x9q_clients[0].system);
 assert(repeat(0,a));x9r_poll();assert(receipts==2&&x9q_clients[0].next==789);
}
static void last_order_is_valid_never_wraps_fifo(){
 baseline();x9r_order=UINT64_MAX-1;request a(1),b(2);a.tag(0x19,1);b.tag(0x19,0);
 assert(!x9r_begin(0,a.p,a.n)&&x9r_pending[0].order==UINT64_MAX);
 assert(x9r_begin(0,b.p,b.n)==X9Q_BUSY&&pending()==1&&x9r_order==UINT64_MAX);
 x9r_poll();assert(receipts==1&&x9q_clients[0].signal&&!pending());
}
// Addresses keep compile-only vectors in the object; there is no main/runner.
void (*nas_registration_compile_only_vectors[])()={
 atomic_multi_tag_receipt,invalid_tail_is_atomic,widths_zero_two_reject,unknown_accepted_unserved,empty_patch_receipt,
 oom_preserves_owner,queue_full_preserves_result,pending_duplicate_reuses_owner,changed_duplicate_preserves_first,
 malformed_duplicate_scoped_before_validator,same_client_fifo_patch_not_snapshot,independent_clients_do_not_block,
 completed_duplicate_reply_only,completed_changed_is_not_replayed,completed_oom_queue_retry,completed_caches_do_not_exhaust_pool,
 busy_and_bad_new_txn_preserve_cache,admitted_new_txn_retires_cache,delete_pending_and_completed,generation_counters_have_different_origins,
 real_generation_change_retires,local_generation_change_retires,release_pending_retires,release_between_identity_and_receipt,
 local_generation_between_identity_and_receipt,bind_pending_busy_completed_retired,disable_controls_old_indication_only_after_receipt,
 indication_fences_generation_sim_and_flag,exhausted_order_preserves_completed_cache,last_order_is_valid_never_wraps_fifo
};
