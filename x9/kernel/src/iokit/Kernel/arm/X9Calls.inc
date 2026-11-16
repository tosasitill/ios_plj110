/* iOS20H392 VOICE bridge staging. Wire translation lives in the shared codec
 * and is tested against the original formatters / MD consumers. No call
 * state is invented here. Do not add service9 to X9Baseband's announcement
 * until Android initialization, dial/status/SIM routing and voice audio are
 * reconciled. There is deliberately NO bootarg to override this boundary.
 * x9cw_dial now translates ordinary VOICE20 digits to the captured SIM2
 * profile only when a caller explicitly supplies that verified context.
 * A verified single-call mapping is required before x9cw_action can be
 * submitted. A real native state producer and asynchronous DIAL join now
 * exist; initialization/profile/audio admission remains unconnected. No
 * MIPC call request/registration/approval is started at boot. Other
 * VOICE requests get unsupported, never a success bootstrap/readiness stub.
 * Explicit VOICE44 routing and guarded VOICE21 end-current-call now have
 * production dispatch. Bound VOICE03 explicit disables now cause actual
 * Android-derived CMD/IND registration and an owned initial snapshot;
 * handover14 now enables actual, ordered Android SRVCC -> Apple54 events;
 * extra CallEventsNotify13=1 remains unsupported. This does not publish VOICE,
 * admit a DIAL profile, or report network/call/audio readiness. */
#include "../../../bsd/dev/x9_call_state.h"
#include "../../../bsd/dev/x9_call_init.h"
#include "../../../bsd/dev/x9_call_handover.h"
struct x9c_dial_job {
 bool used,answered,cnf,held;unsigned slot,sim,len;
 uint64_t generation,deadline,token,reply,binding;struct x9cw_dial_join join;
 uint8_t req[64];
};
static x9c_dial_job x9c_dial_job;
static struct x9cw_outbound_state x9c_outbound;
static struct x9cw_inbound_state x9c_inbound;
static bool x9c_observed;
static bool x9c_incoming_observed;
/* VOICE binding is independent of NAS/WMS clients and has NO SIM default. */
struct x9c_client {bool bound,registered,handover;unsigned sim;uint32_t handover_floor;uint64_t generation,bridge_generation,binding;
 struct x9cw_handover_queue pending;};
static x9c_client x9c_clients[CBQ_MAX_CLIENTS];
static uint64_t x9c_binding_next;
static bool x9c_bound_live(unsigned slot,unsigned sim,uint64_t generation,uint64_t binding){
 if(slot>=CBQ_MAX_CLIENTS||!binding)return false;
 const struct x9c_client &c=x9c_clients[slot];
 return c.bound&&c.sim==sim&&c.binding==binding&&c.generation==generation&&
  x9q_clients[slot].generation==generation&&c.bridge_generation&&
  bb_x9_call_identity(CBQ_PORT_BASE+slot,X9BB_NODE)==c.bridge_generation;
}
/* Reserve a terminal-reply owner BEFORE accepting a request which may touch
 * MD. Jobs may drain/retire independently after deciding their result, but
 * local queue/OOM loss cannot erase that result or repeat the MD command.
 * Lease IDs never alias a reused pool entry; no digits are retained here. */
#define X9_C_RESPONSES 32u
struct x9c_response {
 bool used,ready,bind,change,was_bound;unsigned slot,sim,len,route;
 uint64_t lease,generation,bridge_generation,deadline,binding;uint8_t sdu[18];
 // Bind44 owns the OLD binding snapshot until queue receipt, including an
 // initial unbound client. Its future lease is reserved, never made active.
 uint64_t baseline_generation,baseline_bridge,next_binding;uint8_t req[11];
};
static struct x9c_response x9c_responses[X9_C_RESPONSES];
static struct {bool replay;struct x9c_response result;} x9c_bind_completed[CBQ_MAX_CLIENTS];
static uint64_t x9c_response_next;
static struct x9c_response *x9c_response_find(uint64_t lease){
 if(lease)for(struct x9c_response &r:x9c_responses)if(r.used&&r.lease==lease)return &r;
 return nullptr;
}
static bool x9c_response_live(const struct x9c_response &r){
 if(r.bind){
  if(r.slot>=CBQ_MAX_CLIENTS||x9q_clients[r.slot].generation!=r.generation||!r.bridge_generation||
     bb_x9_call_identity(CBQ_PORT_BASE+r.slot,X9BB_NODE)!=r.bridge_generation)return false;
  const struct x9c_client &c=x9c_clients[r.slot];
  return c.bound==r.was_bound&&c.sim==r.sim&&c.binding==r.binding&&
   c.generation==r.baseline_generation&&c.bridge_generation==r.baseline_bridge;
 }
 return x9c_bound_live(r.slot,r.sim,r.generation,r.binding)&&
  x9c_clients[r.slot].bridge_generation==r.bridge_generation;
}
static bool x9c_bind_pending(unsigned slot,bool changing_only=false){
 for(const struct x9c_response &r:x9c_responses)if(r.used&&r.bind&&r.slot==slot&&
    (!changing_only||(r.change&&!x9cw_u16(r.sdu+10)))&&x9c_response_live(r))return true;
 return false;
}
/* Only an existing44 owner/cache may bypass generic validation for a repeat.
 * A changed/malformed request under its txn cannot replace its first result.
 * Message identity is explicit; a03/20/21 txn never consumes a44 response. */
static bool x9c_bind_repeated(uint32_t local,uint32_t node,uint32_t port,const uint8_t *req,unsigned len){
 const unsigned slot=local-CBQ_PORT_BASE;
 if(node!=X9BB_NODE||port!=X9BB_PORT_BASE+9||local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS||
    !req||len<7||req[0]!=0||x9cw_u16(req+3)!=0x44)return false;
 for(const struct x9c_response &r:x9c_responses)if(r.used&&r.bind&&r.slot==slot&&
    x9c_response_live(r)&&x9cw_u16(r.req+1)==x9cw_u16(req+1))return true;
 auto &cache=x9c_bind_completed[slot];const struct x9c_response &r=cache.result;
 if(!r.used||!x9c_response_live(r)||x9cw_u16(r.req+1)!=x9cw_u16(req+1))return false;
 if(len==sizeof(r.req)&&!memcmp(r.req,req,sizeof(r.req)))cache.replay=true;
 return true; // changed completed reuse retains the first result, no mutation
}
static uint64_t x9c_response_reserve(unsigned slot,const uint8_t *req,uint64_t deadline){
 if(slot>=CBQ_MAX_CLIENTS||!req||!deadline||x9c_response_next==UINT64_MAX)return 0;
 const struct x9c_client &c=x9c_clients[slot];
 if(!x9c_bound_live(slot,c.sim,c.generation,c.binding))return 0;
 for(struct x9c_response &r:x9c_responses)if(!r.used){
  bzero(&r,sizeof(r));r.used=true;r.slot=slot;r.sim=c.sim;r.generation=c.generation;
  r.bridge_generation=c.bridge_generation;r.binding=c.binding;r.deadline=deadline;r.lease=++x9c_response_next;
  r.sdu[0]=2;memcpy(r.sdu+1,req+1,4);return r.lease;
 }
 return 0;
}
static void x9c_response_release(uint64_t lease){
 struct x9c_response *r=x9c_response_find(lease);if(r)bzero(r,sizeof(*r));
}
static bool x9c_response_pending(unsigned slot,const uint8_t *req){
 for(const struct x9c_response &r:x9c_responses)if(r.used&&r.ready&&r.slot==slot&&
    x9c_response_live(r)&&!memcmp(r.sdu+1,req+1,4))
  return true; // original txn/result retained, including changed request reuse
 return false;
}
static void x9c_response_error(struct x9c_response &r,uint16_t error){
 bzero(r.sdu+5,sizeof(r.sdu)-5);r.len=14;r.sdu[5]=7;r.sdu[7]=2;r.sdu[8]=4;
 r.sdu[10]=(uint8_t)(error!=0);x9cw_put16(r.sdu+12,error);
}
static void x9c_response_decide(uint64_t lease,uint16_t error){
 struct x9c_response *r=x9c_response_find(lease);if(!r||r->ready)return;
 x9c_response_error(*r,error);r->ready=true;
}
static void x9c_response_dial(uint64_t lease,const uint8_t *sdu,unsigned n){
 struct x9c_response *r=x9c_response_find(lease);if(!r||r->ready)return;
 if(!sdu||n!=18||memcmp(r->sdu+1,sdu+1,4)){x9c_response_decide(lease,X9Q_INTERNAL);return;}
 memcpy(r->sdu,sdu,n);r->len=n;r->ready=true;
}
static void x9c_bind_commit(struct x9c_response &r);
static bool x9c_response_transfer(const struct x9c_response &r,uint64_t deadline){
 const unsigned room=CBQ_HDR+r.len;
 uint8_t *owned=static_cast<uint8_t *>(IOMallocData(room));
 const bool admitted=owned&&x9c_response_live(r)&&bb_x9_call_result(CBQ_PORT_BASE+r.slot,X9BB_NODE,
  r.bridge_generation,r.sdu,r.len,owned,room,deadline);
 if(!admitted&&owned){bzero(owned,room);IOFreeData(owned,room);}
 return admitted;
}
static void x9c_response_poll(){
 for(auto &cache:x9c_bind_completed){
  if(cache.result.used&&!x9c_response_live(cache.result)){bzero(&cache,sizeof(cache));continue;}
  // This result ALREADY received a queue receipt. Replay is not a fresh
  // admission grant: never rebind or turn its committed success into timeout.
  // Only this completed44 replay uses the unbounded transfer deadline.
  if(cache.result.used&&cache.replay&&x9c_response_transfer(cache.result,UINT64_MAX))cache.replay=false;
 }
 for(struct x9c_response &r:x9c_responses)if(r.used){
  if(!x9c_response_live(r)){bzero(&r,sizeof(r));continue;}
  if(!r.ready)continue;
  // Use the owned admission guard, including time waiting for a queue slot:
  // original03/20/21 guard, local policy for44 (see bind_begin).
  // An error already decided is immutable; no late success resurrection.
  if(!x9cw_u16(r.sdu+10)&&x9q_ms()>=r.deadline)x9c_response_error(r,X9Q_NOT_READY);
  if(!x9c_response_transfer(r,r.deadline))continue;
  if(r.bind){
   if(!x9cw_u16(r.sdu+10))x9c_bind_commit(r);
   auto &cache=x9c_bind_completed[r.slot];cache.result=r;cache.replay=false;
  }
  bzero(&r,sizeof(r));
 }
}
struct x9c_end_job {
 bool used,answered;unsigned slot,sim,stage,id;
 uint16_t transaction;
 uint64_t generation,deadline,token,epoch,reply,binding;
 uint8_t req[11];
};
static x9c_end_job x9c_end_job;
static bool x9c_end_live(const struct x9c_end_job &j){
 return x9c_bound_live(j.slot,j.sim,j.generation,j.binding);
}
static void x9c_end_answer(struct x9c_end_job &j,uint16_t err){
 if(!j.answered){
  if(x9c_end_live(j))x9c_response_decide(j.reply,err);
  else x9c_response_release(j.reply);
 }
 j.answered=true;bzero(j.req,sizeof(j.req));
}
static uint64_t x9c_current_submit(unsigned sim){
 uint8_t query[16];x9cw_header(query,sizeof(query),0x909,1u<<sim,0);
 uint64_t token=x9_md_mipc_submit(query,sizeof(query));bzero(query,sizeof(query));return token;
}
/* VOICE03 is a real trigger; Bind44 alone never sends a registration.
 * MD subscriptions survive QRTR deletion; host waiters do not. Every SIM
 * owns its stage/token. The Android CMD registry itself is global (ff). */
struct x9c_reg_wait {
 bool used,answered;unsigned sim,len;uint16_t transaction,result;
 uint64_t generation,deadline,reply,bridge_generation,binding;uint8_t req[15];
 struct x9cw_voice_register options;uint32_t floor;
 unsigned event_count;uint8_t events[8];
};
/* Apple sends13=0 and14=1 as independent transactions. Do not turn a
 * valid concurrent bootstrap into BUSY; overlapping options are BUSY. */
#define X9_C_REG_WAITS 2u
static struct x9c_reg_wait x9c_reg_wait[CBQ_MAX_CLIENTS][X9_C_REG_WAITS];
static void x9c_bind_commit(struct x9c_response &r){
 struct x9c_client &c=x9c_clients[r.slot];
 // send/del/poll run on the sole QMI thread. The bridge checked allocation
 // and deadline under its mutex before receipt. No fallible post-receipt
 // gate may emit success without committing; no MD owner is touched here.
 if(r.change){
  bzero(&c,sizeof(c));bzero(x9c_reg_wait[r.slot],sizeof(x9c_reg_wait[r.slot]));
  c.binding=r.next_binding;
 }
 c.bound=true;c.sim=r.route;c.generation=r.generation;c.bridge_generation=r.bridge_generation;
 // Completed replay must pin the COMMITTED snapshot, not the old lease.
 r.was_bound=c.bound;r.sim=c.sim;r.binding=c.binding;
 r.baseline_generation=c.generation;r.baseline_bridge=c.bridge_generation;
}
static uint16_t x9c_bind_begin(unsigned slot,const uint8_t *req,unsigned sim,uint64_t bridge){
 if(x9c_bind_pending(slot))return X9Q_BUSY;
 const struct x9c_client &c=x9c_clients[slot];const uint64_t generation=x9q_clients[slot].generation;
 const bool change=!c.bound||c.sim!=sim||c.generation!=generation||c.bridge_generation!=bridge;
 if(change){
  // Even an answered job/waiter may still have an undelivered old terminal.
  // Refuse a new changing bind until these owners receive their results;
  // pool index order is not a proof that the old response drained first.
  if((x9c_end_job.used&&x9c_end_job.slot==slot&&x9c_end_job.generation==generation)||
     (x9c_dial_job.used&&x9c_dial_job.slot==slot&&x9c_dial_job.generation==generation))return X9Q_BUSY;
  for(const struct x9c_reg_wait &w:x9c_reg_wait[slot])
   if(w.used&&!w.answered&&w.generation==generation)return X9Q_BUSY;
  for(const struct x9c_response &r:x9c_responses)
   if(r.used&&!r.bind&&r.slot==slot&&x9c_response_live(r))return X9Q_BUSY;
 }
 const uint64_t now=x9q_ms();
 // Local25s admission guard. Existing evidence does NOT establish the
 // original44 SendProxy timeout; do not mislabel this as an Apple timer.
 if(!bridge||now>UINT64_MAX-25000||x9c_response_next==UINT64_MAX||
    (change&&x9c_binding_next==UINT64_MAX))return X9Q_BUSY;
 for(struct x9c_response &r:x9c_responses)if(!r.used){
  bzero(&r,sizeof(r));r.used=r.ready=r.bind=true;r.change=change;r.slot=slot;
  r.sim=c.sim;r.was_bound=c.bound;r.binding=c.binding;r.baseline_generation=c.generation;
  r.baseline_bridge=c.bridge_generation;r.generation=generation;r.bridge_generation=bridge;
  r.route=sim;r.deadline=now+25000;r.lease=++x9c_response_next;
  // Allocate unique leases across clients before admission. Timeout may
  // consume a lease, but retry never advances it or activates the new route.
  r.next_binding=change?++x9c_binding_next:c.binding;
  memcpy(r.req,req,sizeof(r.req));r.sdu[0]=2;memcpy(r.sdu+1,req+1,4);x9c_response_error(r,0);
  bzero(&x9c_bind_completed[slot],sizeof(x9c_bind_completed[slot]));return 0;
 }
 return X9Q_BUSY;
}
static struct x9cw_call_init x9c_init[2];
static struct {uint64_t token,deadline;bool confirmed,held;} x9c_cmd_init;
static bool x9c_reg_live(unsigned slot,const struct x9c_reg_wait &w){
 return x9c_bound_live(slot,w.sim,w.generation,w.binding)&&
  x9c_clients[slot].bridge_generation==w.bridge_generation;
}
static bool x9c_event_transport_ready(){
 uint32_t cursor;
 /* A live generic MIPC transport is insufficient when the independent
  * CALL inbox is quarantined. Do not grant/flush a dead subscription. */
 return x9w_control_ready&&x9_md_mipc_online()&&x9_md_mipc_call_cursor(&cursor)==1;
}
struct x9c_handover_publication {unsigned slot,count,sim;uint64_t generation;bb_x9_call_frame frames[8];};
static int x9c_handover_publish_callback(void *context){
 struct x9c_handover_publication &p=*static_cast<x9c_handover_publication *>(context);
 const struct x9c_client &c=x9c_clients[p.slot];
 if(!x9w_control_ready||!c.registered||!c.handover||c.pending.blocked||
    !x9c_bound_live(p.slot,p.sim,p.generation,c.binding))return 0;
 return bb_x9_call_handover(CBQ_PORT_BASE+p.slot,X9BB_NODE,c.bridge_generation,p.frames,p.count)?1:0;
}
static void x9c_handover_flush(unsigned slot){
 struct x9c_client &c=x9c_clients[slot];
 if(!c.bound||!c.registered||!c.handover||!c.pending.count||c.pending.blocked||
    c.generation!=x9q_clients[slot].generation||!x9c_event_transport_ready())return;
 uint8_t sdu[8][11]={};x9c_handover_publication p={};p.slot=slot;p.sim=c.sim;p.generation=c.generation;
 p.count=c.pending.count;bool allocated=true;
 for(unsigned i=0;i<p.count;i++){
  bb_x9_call_frame &f=p.frames[i];f.sdu=sdu[i];f.size=11;
  if(x9cw_handover_indication(sdu[i],11,c.pending.events[i])!=11){allocated=false;break;}
  f.owned=static_cast<uint8_t *>(IOMallocData(CBQ_HDR+11));
  if(!f.owned){allocated=false;break;}
 }
 int rc=allocated?x9_md_mipc_call_publish(c.handover_floor,x9c_handover_publish_callback,&p):0;
 for(unsigned i=0;i<p.count;i++)if(p.frames[i].owned&&rc!=1){
  bzero(p.frames[i].owned,CBQ_HDR+11);IOFreeData(p.frames[i].owned,CBQ_HDR+11);
 }
 if(rc==1)x9cw_handover_queue_published(&c.pending);
 else if(rc<0){c.registered=false;c.handover=false;bzero(&c.pending,sizeof(c.pending));}
 bzero(sdu,sizeof(sdu));
}
struct x9c_reg_publication {unsigned slot,count;const struct x9c_reg_wait *wait;bb_x9_call_frame frames[9];};
static int x9c_reg_publish_callback(void *context){
 struct x9c_reg_publication &p=*static_cast<x9c_reg_publication *>(context);
 if(!x9w_control_ready||!x9c_reg_live(p.slot,*p.wait)||x9q_ms()>=p.wait->deadline)return 0;
 return bb_x9_call_registration(CBQ_PORT_BASE+p.slot,X9BB_NODE,p.wait->bridge_generation,
  p.frames,p.count,p.wait->deadline)?1:0;
}
/* Retry only local publication on queue/OOM loss. No MD request is repeated.
 * Result and retained events are an all-or-none, allocation-free MD->bb batch. */
static int x9c_reg_publish(unsigned slot,const struct x9c_reg_wait &w){
 if(w.event_count>8)return -1;
 uint8_t sdu[9][14]={};x9c_reg_publication publication={};publication.slot=slot;publication.wait=&w;
 uint8_t *result=sdu[0];result[0]=2;memcpy(result+1,w.req+1,4);
 result[5]=7;result[7]=2;result[8]=4;publication.count=1;
 const struct x9c_client &c=x9c_clients[slot];
 if((w.options.present&2)&&w.options.handover&&!c.handover){
  for(unsigned i=0;i<w.event_count;i++){
   if(x9cw_handover_indication(sdu[publication.count],14,w.events[i])!=11)return -1;
   publication.count++;
  }
 }
 bool allocated=true;
 for(unsigned i=0;i<publication.count;i++){
  bb_x9_call_frame &f=publication.frames[i];f.sdu=sdu[i];f.size=i?11:14;
  f.owned=static_cast<uint8_t *>(IOMallocData(CBQ_HDR+f.size));
  if(!f.owned){allocated=false;break;}
 }
 int rc=allocated?x9_md_mipc_call_publish(w.floor,x9c_reg_publish_callback,&publication):0;
 for(unsigned i=0;i<publication.count;i++){
  bb_x9_call_frame &f=publication.frames[i];
  if(f.owned&&rc!=1){bzero(f.owned,CBQ_HDR+f.size);IOFreeData(f.owned,CBQ_HDR+f.size);}
 }
 bzero(sdu,sizeof(sdu));return rc;
}
static void x9c_reg_answer(unsigned slot,unsigned index,uint16_t error){
 struct x9c_reg_wait &w=x9c_reg_wait[slot][index];
 if(!error&&!x9c_event_transport_ready())error=X9Q_NOT_READY;
 /* An enable accepted before live queue overflow cannot clear that owner.
  * Only a successfully published explicit disable retires the blocked batch. */
 if(!error&&(w.options.present&2)&&w.options.handover&&x9c_clients[slot].pending.blocked)
  error=X9Q_NOT_READY;
 if(!w.answered&&x9c_reg_live(slot,w)){
  if(!error){
   int rc=x9c_reg_publish(slot,w);
   if(!rc)return; // retain request/events until admission or original deadline
   if(rc<0)error=X9Q_NOT_READY;
  }
  if(error)x9c_response_decide(w.reply,error);
  else x9c_response_release(w.reply); // the successful batch already owns Result03
  struct x9c_client &c=x9c_clients[slot];
  if(!error){
   c.registered=true;
   if(w.options.present&2){
    bool enabled=c.handover;c.handover=w.options.handover!=0;
    if(c.handover&&!enabled){
     bzero(&c.pending,sizeof(c.pending));
     c.handover_floor=w.floor;
     /* Result and retained START/COMPLETE are already in one admitted batch. */
    }else if(!c.handover)bzero(&c.pending,sizeof(c.pending)); // explicit successful disable retires local publication
   }
  }
 }else if(!w.answered)x9c_response_release(w.reply);
 w.result=error;w.answered=true;bzero(w.req,sizeof(w.req));
 bzero(w.events,sizeof(w.events));w.event_count=0;
}
static void x9c_handover_event(const uint8_t *frame,unsigned n,uint32_t sequence){
 if(!x9c_event_transport_ready())return;
 struct x9cw_handover e;if(x9cw_handover(frame,n,&e)!=1)return;
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){
  struct x9c_client &c=x9c_clients[slot];
  if(!c.bound||c.generation!=x9q_clients[slot].generation||c.sim!=e.sim)continue;
  if(c.registered&&c.handover){
   if(x9cw_handover_queue_push(&c.pending,c.handover_floor,sequence,e.ios_state)<0){
    c.handover=false; // lost sequence cannot authorize continued partial publication
    printf("X9 CALL: SIM%u handover publication bounded; explicit disable required\n",c.sim+1);
   }
  }else for(unsigned index=0;index<X9_C_REG_WAITS;index++){
   struct x9c_reg_wait &w=x9c_reg_wait[slot][index];
   if(!w.used||w.answered||!x9c_reg_live(slot,w)||!(w.options.present&2)||
      !w.options.handover||sequence<=w.floor)continue;
   if(x9q_ms()>=w.deadline||w.event_count==sizeof(w.events))x9c_reg_answer(slot,index,X9Q_NOT_READY);
   else w.events[w.event_count++]=(uint8_t)e.ios_state;
  }
 }
}
static uint64_t x9c_register_submit(unsigned id,unsigned sim){
 uint8_t frame[24];unsigned n=x9cw_call_register(frame,sizeof(frame),id,sim,x9_md_sim_count());
 uint64_t token=n?x9_md_mipc_submit(frame,n):0;bzero(frame,sizeof(frame));return token;
}
static void x9c_init_poll(uint8_t *frame){
 bool wanted[2]={false,false};uint64_t now=x9q_ms();
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)for(unsigned index=0;index<X9_C_REG_WAITS;index++){
  struct x9c_reg_wait &w=x9c_reg_wait[i][index];if(!w.used||w.answered)continue;
  if(!x9c_reg_live(i,w)||now>=w.deadline)x9c_reg_answer(i,index,X9Q_NOT_READY);
  else wanted[w.sim]=true;
 }
 bool online=x9c_event_transport_ready();
 if(!online){
  for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){x9c_clients[i].registered=false;x9c_clients[i].handover=false;
   bzero(&x9c_clients[i].pending,sizeof(x9c_clients[i].pending));}
  for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)for(unsigned index=0;index<X9_C_REG_WAITS;index++){
   struct x9c_reg_wait &w=x9c_reg_wait[i][index];
   if(w.used&&!w.answered)x9c_reg_answer(i,index,X9Q_NOT_READY);
  }
  if(x9c_cmd_init.token||x9c_cmd_init.confirmed)x9c_cmd_init.held=true;
  for(unsigned sim=0;sim<2;sim++)if(x9c_init[sim].stage||x9c_init[sim].token)x9c_init[sim].held=1;
 }
 if(x9c_cmd_init.token){
  if(now>=x9c_cmd_init.deadline)x9c_cmd_init.held=true;
  uint32_t n=0;int rc=x9_md_mipc_take(x9c_cmd_init.token,frame,X9_MIPC_FRAME_MAX,&n);
  if(rc==1){
   uint32_t result=1;bool good=x9cw_call_register_cnf(frame,n,0x30b,x9c_cmd_init.token,&result);
   x9c_cmd_init.token=0;bzero(frame,X9_MIPC_FRAME_MAX);
   if(!good||result)x9c_cmd_init.held=true;
   if(!x9c_cmd_init.held){x9c_cmd_init.confirmed=true;
    printf("X9 CALL: Android CMD8901 registration confirmed; no automatic approval\n");}
  }else if(rc<0)x9c_cmd_init.held=true; /* keep uncertain physical token */
 }
 if(online&&!x9c_cmd_init.held&&!x9c_cmd_init.confirmed&&!x9c_cmd_init.token&&(wanted[0]||wanted[1])){
  x9c_cmd_init.token=x9c_register_submit(0x8901,wanted[0]?0:1);
  if(x9c_cmd_init.token)x9c_cmd_init.deadline=now+25000;
 }
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  struct x9cw_call_init &s=x9c_init[sim];
  if(s.token){
   if(now>=s.deadline)s.held=1;
   uint32_t n=0;int rc=x9_md_mipc_take(s.token,frame,X9_MIPC_FRAME_MAX,&n);
   if(rc==1){
    if(s.stage==X9_CI_BASELINE){
     struct x9cw_current current;struct x9cw_outbound_state state;
     bool good=n>=16&&x9cw_u16(frame+12)==s.token&&x9cw_current(frame,n,&current)&&!current.result;
     s.token=0;
     if(s.held||!good)s.held=1;
     else{
      /* Nonempty is not idle. Only SIM2 has the reconciled reducer; its
       * query must also have survived the MD owner's revision guard. */
      s.baseline_known=!current.count&&(sim==0||
       (x9_md_mipc_call_snapshot(&state)==1&&!state.blocked&&state.count_known&&!state.native_count));
      s.stage=X9_CI_DONE;
      printf("X9 CALL: SIM%u registrations confirmed, initial count %u idle-baseline %u; audio/profile not ready\n",
       sim+1,current.count,s.baseline_known);
     }
    }else if(x9cw_call_init_cnf(&s,frame,n,now)==1)
     printf("X9 CALL: SIM%u registration confirmed, next stage %u\n",sim+1,s.stage);
    bzero(frame,X9_MIPC_FRAME_MAX);
   }else if(rc<0)s.held=1;
  }
  if(!online||!wanted[sim]||s.held||x9c_cmd_init.held||!x9c_cmd_init.confirmed||s.token||s.stage==X9_CI_DONE)continue;
  if(s.stage==X9_CI_NEW)s.stage=X9_CI_STATUS;
  s.token=s.stage==X9_CI_BASELINE?x9c_current_submit(sim):
   x9c_register_submit(s.stage==X9_CI_STATUS?0x4901:0x4902,sim);
  if(s.token)s.deadline=now+25000;
 }
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)for(unsigned index=0;index<X9_C_REG_WAITS;index++){
  struct x9c_reg_wait &w=x9c_reg_wait[i][index];if(!w.used||w.answered)continue;
  const struct x9cw_call_init &s=x9c_init[w.sim];
  if(x9c_cmd_init.held||s.held)x9c_reg_answer(i,index,X9Q_NOT_READY);
  else if(s.stage==X9_CI_DONE&&s.registered)x9c_reg_answer(i,index,0);
 }
}
static uint16_t x9c_reg_begin(unsigned slot,const uint8_t *req,unsigned len){
 struct x9cw_voice_register r;
 if(!x9cw_qmi_register(req,len,&r))return X9Q_ARG;
 /* 13=0 and14=1 are the actual independent Apple requests. The latter
  * now has a real4902/SRVCC ->54 consumer, not a readiness success stub.
  * CallEventsNotify13=1 has a different payload contract, still unsupported. */
 if(r.call_events)return X9Q_UNSUPPORTED;
 const struct x9c_client &c=x9c_clients[slot];
 if(!c.bound||c.generation!=x9q_clients[slot].generation)return X9Q_NOT_READY;
 if((r.present&2)&&r.handover&&c.pending.blocked)return X9Q_NOT_READY;
 unsigned index=X9_C_REG_WAITS;
 for(unsigned i=0;i<X9_C_REG_WAITS;i++){
  const struct x9c_reg_wait &w=x9c_reg_wait[slot][i];
  if(w.used&&!w.answered){if(w.options.present&r.present)return X9Q_BUSY;}
  else if(index==X9_C_REG_WAITS)index=i;
 }
 if(index==X9_C_REG_WAITS)return X9Q_BUSY;
 uint32_t floor;
 if(x9_md_mipc_call_cursor(&floor)!=1)return X9Q_NOT_READY;
 struct x9c_reg_wait &w=x9c_reg_wait[slot][index];
 const uint64_t now=x9q_ms();if(now>UINT64_MAX-25000)return X9Q_BUSY;
 const uint64_t deadline=now+25000;
 uint64_t reply=x9c_response_reserve(slot,req,deadline);if(!reply)return X9Q_BUSY;
 bzero(&w,sizeof(w));w.used=true;w.sim=c.sim;w.len=len;w.reply=reply;
 w.bridge_generation=c.bridge_generation;w.binding=c.binding;
 w.generation=c.generation;w.transaction=(uint16_t)x9cw_u16(req+1);
 w.options=r;w.floor=floor;
 w.deadline=deadline;memcpy(w.req,req,len);return 0;
}
/* This read-only query is caused ONLY by an explicit, bound VOICE21.
 * No implicit ID/route adoption from a CNF, log prefix or another client. */
static uint16_t x9c_end_begin(unsigned slot,const uint8_t *req,unsigned len){
 unsigned op,id;
 if(!x9cw_qmi_action(req,len,&op,&id)||op!=0x21||!id)return X9Q_ARG;
 const struct x9c_client &c=x9c_clients[slot];
 if(!c.bound||c.generation!=x9q_clients[slot].generation)return X9Q_NOT_READY;
 if(c.sim!=1)return X9Q_UNSUPPORTED; /* only the measured SIM2 MO subset */
 if(x9c_end_job.used)return X9Q_BUSY;
 struct x9cw_outbound_state state;unsigned native_id;
 if(x9_md_mipc_call_snapshot(&state)!=1||!x9cw_outbound_id(&state,&native_id)||
    native_id!=id||state.mode!=20)return X9Q_NOT_READY;
 const uint64_t now=x9q_ms();if(now>UINT64_MAX-25000)return X9Q_BUSY;
 const uint64_t deadline=now+25000;
 uint64_t reply=x9c_response_reserve(slot,req,deadline);if(!reply)return X9Q_BUSY;
 uint64_t token=x9c_current_submit(c.sim);if(!token){x9c_response_release(reply);return X9Q_BUSY;}
 struct x9c_end_job &j=x9c_end_job;bzero(&j,sizeof(j));j.used=true;j.slot=slot;j.sim=c.sim;
 j.generation=c.generation;j.binding=c.binding;j.epoch=state.epoch;j.id=id;j.stage=1;j.token=token;j.reply=reply;
 /* Apple createEndThisCallReq19a00d4: explicit25000ms request guard. */
 j.deadline=deadline;j.transaction=(uint16_t)x9cw_u16(req+1);
 memcpy(j.req,req,sizeof(j.req));return 0;
}
static void x9c_end_poll(uint8_t *frame){
 struct x9c_end_job &j=x9c_end_job;if(!j.used)return;
 if(!x9c_end_live(j)||(!j.answered&&x9q_ms()>=j.deadline)){
  if(j.stage==2)x9_md_mipc_call_end_abandon(j.token);
  x9c_end_answer(j,X9Q_NOT_READY);
 }
 if(j.token){
  uint32_t n=0;int rc=x9_md_mipc_take(j.token,frame,X9_MIPC_FRAME_MAX,&n);
  if(!rc)return;
  if(rc<0){
   x9c_end_answer(j,X9Q_NOT_READY);
   if(rc==-3){bzero(&j,sizeof(j));return;} /* proven pre-selection rejection */
   return; /* uncertain physical owner retained, no retry */
  }
  uint64_t token=j.token;j.token=0;
  if(j.stage==1){
   struct x9cw_current current;struct x9cw_end_target target;
   bool valid=x9cw_u16(frame+12)==token&&x9cw_current(frame,n,&current)&&!current.result&&
    x9_md_mipc_call_end_snapshot(&target)==1&&target.epoch==j.epoch&&target.native_id==j.id&&target.sim==j.sim;
   bzero(frame,X9_MIPC_FRAME_MAX);
   /* take/snapshot may wait for the MD mutex while the actual QMUX client
    * is released, before this thread can dispatch DEL_CLIENT. Revalidate
    * the read-only stage's owner/deadline before enqueueing a new mutation.
    * This is a post-wait fence, not an atomic BB/MD submission lease. */
   if(j.answered||!valid||!x9c_end_live(j)||x9q_ms()>=j.deadline){
    x9c_end_answer(j,X9Q_NOT_READY);bzero(&j,sizeof(j));return;
   }
   uint8_t action[32];unsigned k=x9cw_action(j.req,sizeof(j.req),action,sizeof(action),j.id,target.native_id,j.sim,1);
   j.token=k?x9_md_mipc_call_end_submit(action,k,&target):0;bzero(action,sizeof(action));
   if(!j.token){x9c_end_answer(j,X9Q_BUSY);bzero(&j,sizeof(j));return;}
   j.stage=2;return;
  }
  if(j.stage==2){
   uint32_t result;bool valid=x9cw_end_cnf(frame,n,token,&result);bzero(frame,X9_MIPC_FRAME_MAX);
   x9c_end_answer(j,valid&&!result?0:X9Q_INTERNAL);j.stage=3;
   /* Do NOT force END, free the epoch, or send CallList from CNF result. */
  }else if(j.stage==4){
   struct x9cw_current current;struct x9cw_outbound_state state;
   bool retired=x9cw_u16(frame+12)==token&&x9cw_current(frame,n,&current)&&!current.result&&!current.count&&
    x9_md_mipc_call_snapshot(&state)==1&&!state.blocked&&state.epoch==j.epoch&&state.ended&&
    state.count_known&&!state.native_count;
   bzero(frame,X9_MIPC_FRAME_MAX);
   if(retired){bzero(&j,sizeof(j));return;}
   /* Single cleanup query; a stale/error snapshot holds the owner. No
    * repeated query loop, action retry or inferred idle after END. */
   j.stage=5;return;
  }
 }
 if(j.stage==3){
  struct x9cw_outbound_state state;
  if(x9_md_mipc_call_snapshot(&state)==1&&!state.blocked&&state.epoch==j.epoch&&state.ended){
   j.token=x9c_current_submit(j.sim);if(j.token)j.stage=4;
  }
 }
}
static bool x9c_live(const struct x9c_dial_job &j){
 return x9c_bound_live(j.slot,j.sim,j.generation,j.binding);
}
static void x9c_error(struct x9c_dial_job &j,uint16_t err){
 if(!j.answered&&x9c_live(j)){
  x9c_response_decide(j.reply,err);
 }
 if(!j.answered&&!x9c_live(j))x9c_response_release(j.reply);
 j.answered=true;bzero(j.req,sizeof(j.req));
}
/* Admission is deliberately NOT called by x9c_request until actual CALL
 * initialization, VOICE SIM binding and runtime profile policy are proven.
 * A future caller must supply that context; even then, require a complete
 * owned GET_STATUS empty baseline and no outstanding dial owner. */
static bool x9c_dial_submit(unsigned slot,const uint8_t *req,unsigned len,
                            const struct x9cw_dial_profile &profile){
 if(slot>=CBQ_MAX_CLIENTS||!x9c_clients[slot].bound||x9c_clients[slot].sim!=profile.sim||
    !x9c_clients[slot].registered||x9c_clients[slot].generation!=x9q_clients[slot].generation||
    x9c_bind_pending(slot,true)||x9c_dial_job.used||len>sizeof(x9c_dial_job.req))return false;
 struct x9cw_outbound_state state;struct x9cw_dial_join join={};
 if(x9_md_mipc_call_snapshot(&state)!=1||!x9cw_dial_join_begin(&join,&state,&profile))return false;
 uint8_t frame[104];unsigned n=x9cw_dial(req,len,frame,sizeof(frame),&profile,state.native_count);
 if(!n)return false;
 const uint64_t now=x9q_ms();
 if(now>UINT64_MAX-60000){bzero(frame,sizeof(frame));return false;}
 const uint64_t deadline=now+60000;
 uint64_t reply=x9c_response_reserve(slot,req,deadline);
 if(!reply){bzero(frame,sizeof(frame));return false;}
 uint64_t token=x9_md_mipc_call_dial_submit(frame,n,&state);bzero(frame,sizeof(frame));
 if(!token){x9c_response_release(reply);return false;}
 /* Accepted mutations retain ownership across client deletion/timeouts.
  * No cancellation, automatic hangup, retry, or request-token reuse. */
 struct x9c_dial_job &j=x9c_dial_job;bzero(&j,sizeof(j));j.used=true;
 j.slot=slot;j.sim=profile.sim;j.generation=x9q_clients[slot].generation;
 j.binding=x9c_clients[slot].binding;j.len=len;memcpy(j.req,req,len);
 j.join=join;j.token=token;j.deadline=deadline;j.reply=reply;
 if(!x9cw_dial_join_sent(&j.join,token)){j.held=true;x9c_error(j,X9Q_INTERNAL);}
 return true;
}
static void x9c_del(unsigned slot){
 for(struct x9c_response &r:x9c_responses)if(r.used&&r.slot==slot)bzero(&r,sizeof(r));
 if(slot<CBQ_MAX_CLIENTS)bzero(&x9c_bind_completed[slot],sizeof(x9c_bind_completed[slot]));
 if(slot<CBQ_MAX_CLIENTS)bzero(x9c_reg_wait[slot],sizeof(x9c_reg_wait[slot]));
 if(x9c_end_job.used&&x9c_end_job.slot==slot){
  if(x9c_end_job.stage==2)x9_md_mipc_call_end_abandon(x9c_end_job.token);
  x9c_end_job.answered=true;bzero(x9c_end_job.req,sizeof(x9c_end_job.req));
 }
 if(slot<CBQ_MAX_CLIENTS)bzero(&x9c_clients[slot],sizeof(x9c_clients[slot]));
 if(x9c_dial_job.used&&x9c_dial_job.slot==slot){x9c_dial_job.answered=true;bzero(x9c_dial_job.req,sizeof(x9c_dial_job.req));}
 /* Keep token/epoch even if this QRTR client slot is reused. */
}
static void x9c_dial_poll(uint8_t *frame);
static void x9c_poll(){
 /* Drain raw CALL events, but do not approve or answer CMD8901. The MD
  * owner has already reduced every complete4901 under its lock. Number
  * buffers are erased immediately; CMD identity tokens remain in MD owner. */
 uint8_t frame[X9_MIPC_FRAME_MAX];uint32_t n,token,sequence;
 for(unsigned i=0;i<8;i++){
  int rc=x9_md_mipc_call_event_ordered(frame,sizeof(frame),&n,&token,&sequence);
  if(rc!=1)break;
  if(x9cw_u16(frame+10)==0x4902)x9c_handover_event(frame,n,sequence);
  bzero(frame,sizeof(frame));
 }
 x9c_observed=x9_md_mipc_call_snapshot(&x9c_outbound)==1;
 x9c_incoming_observed=x9_md_mipc_call_incoming_snapshot(&x9c_inbound)==1;
 x9c_init_poll(frame);
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)x9c_handover_flush(slot);
 x9c_end_poll(frame);
 x9c_dial_poll(frame);
 x9c_response_poll();
}
static void x9c_dial_poll(uint8_t *frame){
 uint32_t n;
 struct x9c_dial_job &j=x9c_dial_job;if(!j.used)return;
 if(j.answered&&j.cnf&&!j.token&&!j.join.failed&&x9c_observed&&!x9c_outbound.blocked&&
    j.join.epoch_floor!=UINT64_MAX&&x9c_outbound.epoch==j.join.epoch_floor+1&&
    x9c_outbound.ended&&x9c_outbound.count_known&&!x9c_outbound.native_count){
  bzero(&j,sizeof(j));return; /* real END + owned empty, not dial/hangup CNF */
 }
 if(!x9c_live(j)){x9c_response_release(j.reply);j.answered=true;bzero(j.req,sizeof(j.req));}
 if(j.token){
  int rc=x9_md_mipc_take(j.token,frame,X9_MIPC_FRAME_MAX,&n);
  if(rc==1){
   j.token=0;j.cnf=true;
   if(!x9cw_dial_join_cnf(&j.join,frame,n)){j.held=true;x9c_error(j,X9Q_INTERNAL);}
   bzero(frame,X9_MIPC_FRAME_MAX);
  }else if(rc<0){
   if(rc==-3)j.token=0; /* definitive local pre-TX rejection */
   j.held=true;x9c_error(j,X9Q_NOT_READY);
  }
 }
 if(!j.answered&&x9q_ms()>=j.deadline){
  /* Local60s host guard, NOT an inferred MD/network timer. Late results
   * are still drained but can never resurrect the timed-out host request. */
  j.held=true;x9c_error(j,X9Q_NOT_READY);
 }
 if(!x9c_observed||x9c_outbound.blocked){j.held=true;x9c_error(j,X9Q_NOT_READY);}
 if(j.held||!j.cnf)return;
 unsigned id;int ready=x9cw_dial_join_id(&j.join,&x9c_outbound,&id);
 if(!ready)return;
 if(ready<0){j.held=true;x9c_error(j,X9Q_INTERNAL);return;}
 if(!j.answered&&x9c_live(j)){
  uint8_t response[18];unsigned size=x9cw_dial_join_response(&j.join,&x9c_outbound,j.req,j.len,response,sizeof(response));
  if(!size){j.held=true;x9c_error(j,X9Q_INTERNAL);return;}
  x9c_response_dial(j.reply,response,size);bzero(response,sizeof(response));
 }
 j.answered=true;j.held=true;bzero(j.req,sizeof(j.req));
}
static bool x9c_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned len){
 if(port!=X9BB_PORT_BASE+9)return false;
 if(!req||len<7)return true;
 if(x9c_bind_repeated(local,X9BB_NODE,port,req,len))return true;
 unsigned slot=local-CBQ_PORT_BASE;
 uint16_t err=X9Q_UNSUPPORTED;
 if(local>=CBQ_PORT_BASE&&slot<CBQ_MAX_CLIENTS&&req[0]==0){
  if(x9c_response_pending(slot,req))return true;
  if(x9cw_u16(req+3)==0x44){
   unsigned sim;
   const uint64_t bridge=bb_x9_call_identity(local,X9BB_NODE);
   if(!x9cw_qmi_bind(req,len,&sim)||sim>=x9_md_sim_count())err=X9Q_ARG;
   else if(!bridge)err=X9Q_NOT_READY;
   else{
    err=x9c_bind_begin(slot,req,sim,bridge);if(!err)return true;
   }
  }else if(x9cw_u16(req+3)==3){
   struct x9cw_voice_register options;
   if(!x9cw_qmi_register(req,len,&options)){x9q_packet p(req,X9Q_ARG);p.send(local,port);return true;}
   for(unsigned index=0;index<X9_C_REG_WAITS;index++){
    const struct x9c_reg_wait &w=x9c_reg_wait[slot][index];
    if(w.used&&w.generation==x9q_clients[slot].generation&&w.transaction==x9cw_u16(req+1)){
     if(w.options.present==options.present&&w.options.call_events==options.call_events&&
        w.options.handover==options.handover)return true;
     /* Never advance/grant a changed request under an old transaction. */
     if(!w.answered){x9c_reg_answer(slot,index,X9Q_ARG);x9c_response_poll();}
     // An answered owner already has its first terminal (queued or retained).
     // Changed reuse must not append ARG after that transaction's success/error.
     return true;
    }
   }
   err=x9c_bind_pending(slot,true)?X9Q_BUSY:x9c_reg_begin(slot,req,len);if(!err)return true;
  }else if(x9cw_u16(req+3)==0x21){
   unsigned op,id;
   /* The live allocation's validated host transaction already owns its
    * target. Even a changed ID cannot append BUSY then the original success,
    * or retarget its MD command. Keep identity after req is erased; actual
    * QMUX allocation is fenced by end_live, not only the local generation. */
   if(x9cw_qmi_action(req,len,&op,&id)&&x9c_end_job.used&&
      x9c_end_job.slot==slot&&x9c_end_job.generation==x9q_clients[slot].generation&&
      x9c_end_job.transaction==x9cw_u16(req+1)&&x9c_end_live(x9c_end_job))return true;
   err=x9c_bind_pending(slot,true)?X9Q_BUSY:x9c_end_begin(slot,req,len);if(!err)return true; /* asynchronous owned reply */
  }
 }
 x9q_packet p(req,err);p.send(local,port);return true;
}
