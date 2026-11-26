/* iOS20H392 VOICE bridge staging. Wire translation lives in the shared codec
 * and is tested against the original formatters / MD consumers. No call
 * state is invented here. X9Baseband announces service9; publication does
 * not establish typed call routing, voice audio, or feature acceptance.
 * MD-layer receive now runs at boot, mirroring Android RIL init: once the
 * CALL transport is ready, the Android-derived registration subset (global
 * CMD8901, per-SIM IND4901/4902, owned GET_STATUS baseline) starts without
 * waiting for VOICE03, and a complete CMD8901 in the reconciled SIM2 mode20
 * subset is auto-approved with the live-captured 8902 shape (approve=1, no
 * cause, RIL-converted RSP mask, original txid, echoed id/seq). Approval is
 * the RIL indication acknowledgement, NOT answer; the MD layer never sends
 * ANSWER on its own; this does not establish iOS call UI acceptance.
 * Outside that subset (SIM1, other modes, multi-call, quarantine) approval
 * is withheld fail-closed. Registration/approval are terminal for this boot
 * on timeout or transport loss; no retry.
 * x9cw_dial still translates ordinary VOICE20 digits to the captured SIM2
 * profile for a bound, registered owner. VOICE09 and CallList are published,
 * but each indication still requires live allocation identity, a measured RAT
 * query and owned call state. No audio/network readiness is inferred.
 * Other VOICE requests get unsupported, never a success bootstrap/readiness stub.
 * Explicit VOICE44 routing, guarded VOICE21 end-current-call (MO epoch or
 * owned MT CONVERSATION identity) and guarded VOICE22 answer (owned MT
 * INCOMING identity, captured empty 0907 -> 0908/result0) keep production
 * dispatch. Bound VOICE03 explicit disables reuse the same
 * Android-derived registration and owned initial snapshot; handover14
 * enables actual, ordered Android SRVCC -> Apple54 events; extra
 * CallEventsNotify13=1 remains unsupported. None of this reports
 * network/call/audio readiness. */
#include "../../../bsd/dev/x9_call_state.h"
#include "../../../bsd/dev/x9_call_init.h"
#include "../../../bsd/dev/x9_call_handover.h"
struct x9c_dial_job {
 bool used,answered,cnf,held;unsigned slot,sim,len;uint32_t transaction;
 uint64_t generation,deadline,token,reply,binding;struct x9cw_dial_join join;
 uint8_t req[64];
};
static x9c_dial_job x9c_dial_job;
static const struct x9cw_dial_profile x9c_measured_dial_profile={1,2,1,0,0,0};
static struct x9cw_outbound_state x9c_outbound;
static struct x9cw_inbound_state x9c_inbound;
static bool x9c_observed;
static bool x9c_incoming_observed;
/* Boot registration mirrors Android RIL init (RmmCallControlUrcHandler
 * 37c42c..470 registers CMD8901; rodata133a44's first two IND are 4901/4902),
 * not an iOS VOICE03 trigger. Starts once when the CALL transport is ready;
 * timeout/loss stays terminal for this boot, like the VOICE03 owner. */
static bool x9c_boot_started;
/* Live incoming evidence (android-incoming-20261115-192820): Android sent
 * 8902 87ms after CMD8901, before any user/IMS-framework action: approve
 * 100/u8=1, no cause, RSP mask02 from CMD22 via the proved RIL slot
 * conversion, original txid, id102/seq103 echoed. requestSetCallIndication
 * 382458..382548 builds that RSP from CMD fields only. This owner tracks the
 * delivered CMD identity until the physical RSP result; the CMD bytes (with
 * the number) stay in the MD inbox, never copied here. No retry on failure. */
struct x9c_approve_owner {
 bool used,replied;uint32_t token;unsigned sim;uint32_t id,sequence;uint16_t txid;uint8_t mask;
};
#define X9_C_APPROVE_OWNERS 8u
static struct x9c_approve_owner x9c_approve_owners[X9_C_APPROVE_OWNERS];
/* VOICE binding is independent of NAS/WMS clients and has NO SIM default. */
struct x9c_client {bool bound,registered,handover;unsigned sim;uint32_t handover_floor;uint64_t generation,bridge_generation,binding;
 struct x9cw_handover_queue pending;
 bool calllist_valid;uint64_t calllist_epoch,calllist_revision;unsigned calllist_direction,calllist_id;};
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
 bool used,answered;unsigned slot,sim,stage,id,direction;
 uint16_t transaction;
 uint64_t generation,bridge_generation,deadline,token,epoch,reply,binding;
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
/* VOICE22 answer job: one guarded 0907 attempt per owned MT epoch. The MD
 * answer lease survives this job; only real END + owned empty retires it.
 * CNF success means ANSWER completed, NOT CONVERSATION (ECPI132/6 owns that). */
struct x9c_answer_job {
 bool used,answered;unsigned slot,sim,stage,id;
 uint16_t transaction;
 uint64_t generation,bridge_generation,deadline,token,epoch,reply,binding;
 uint8_t req[11];
};
static x9c_answer_job x9c_answer_job;
static bool x9c_answer_live(const struct x9c_answer_job &j){
 return x9c_bound_live(j.slot,j.sim,j.generation,j.binding);
}
static void x9c_answer_answer(struct x9c_answer_job &j,uint16_t err){
 if(!j.answered){
  if(x9c_answer_live(j))x9c_response_decide(j.reply,err);
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
     (x9c_answer_job.used&&x9c_answer_job.slot==slot&&x9c_answer_job.generation==generation)||
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
/* Voice RAT cache fed by the read-only MIPC 042f query
 * (requestVoiceRegistrationState 0x30f8f0 -> 0430, TLV8100/10B,
 * networkType u16LE@+1). Empty until a response lands; the CallList
 * technology gate stays fail-closed while invalid. One query per SIM per
 * registration epoch, issued only after CS registration is confirmed. */
static struct {unsigned networkType;bool valid;} x9c_voice_rat[2];
static struct {uint64_t token,deadline;bool done,held;} x9c_rat_query[2];
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
   if(!c.registered)c.calllist_valid=false;
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
/* Header-only 042f voice-registration query (libmipc addMipcReq, no addMipcVal);
 * the 0430 CNF is matched generically by id+1/txid and parsed host-side. */
static uint64_t x9c_voice_rat_submit(unsigned sim){
 uint8_t query[16];x9cw_header(query,sizeof(query),0x42f,1u<<sim,0);
 uint64_t token=x9_md_mipc_submit(query,sizeof(query));bzero(query,sizeof(query));return token;
}
static void x9c_init_poll(uint8_t *frame){
 bool wanted[2]={false,false};uint64_t now=x9q_ms();
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)for(unsigned index=0;index<X9_C_REG_WAITS;index++){
  struct x9c_reg_wait &w=x9c_reg_wait[i][index];if(!w.used||w.answered)continue;
  if(!x9c_reg_live(i,w)||now>=w.deadline)x9c_reg_answer(i,index,X9Q_NOT_READY);
  else wanted[w.sim]=true;
 }
 bool online=x9c_event_transport_ready();
 if(online&&!x9c_boot_started){
  x9c_boot_started=true;
  printf("X9 CALL: boot registration start (Android RIL init subset; not voice readiness)\n");
 }
 if(x9c_boot_started){
  if(!x9c_cmd_init.confirmed&&!x9c_cmd_init.held){wanted[0]=true;wanted[1]=true;}
  for(unsigned sim=0;sim<2;sim++)
   if(!x9c_init[sim].held&&x9c_init[sim].stage!=X9_CI_DONE)wanted[sim]=true;
 }
 if(!online){
  for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){x9c_clients[i].registered=false;x9c_clients[i].handover=false;
   x9c_clients[i].calllist_valid=false;bzero(&x9c_clients[i].pending,sizeof(x9c_clients[i].pending));}
  for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)for(unsigned index=0;index<X9_C_REG_WAITS;index++){
   struct x9c_reg_wait &w=x9c_reg_wait[i][index];
   if(w.used&&!w.answered)x9c_reg_answer(i,index,X9Q_NOT_READY);
  }
  if(x9c_cmd_init.token||x9c_cmd_init.confirmed)x9c_cmd_init.held=true;
  for(unsigned sim=0;sim<2;sim++)if(x9c_init[sim].stage||x9c_init[sim].token)x9c_init[sim].held=1;
  for(unsigned sim=0;sim<2;sim++){
   if(x9c_rat_query[sim].token||x9c_rat_query[sim].done)x9c_rat_query[sim].held=true;
   x9c_rat_query[sim].token=0;x9c_voice_rat[sim].valid=false;
  }
 }
 if(x9c_cmd_init.token){
  if(now>=x9c_cmd_init.deadline)x9c_cmd_init.held=true;
  uint32_t n=0;int rc=x9_md_mipc_take(x9c_cmd_init.token,frame,X9_MIPC_FRAME_MAX,&n);
  if(rc==1){
   uint32_t result=1;bool good=x9cw_call_register_cnf(frame,n,0x30b,x9c_cmd_init.token,&result);
   x9c_cmd_init.token=0;bzero(frame,X9_MIPC_FRAME_MAX);
   if(!good||result)x9c_cmd_init.held=true;
   if(!x9c_cmd_init.held){x9c_cmd_init.confirmed=true;
    printf("X9 CALL: Android CMD8901 registration confirmed; SIM2 incoming auto-approve armed\n");}
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
 /* Read-only voice RAT query, one per SIM after CS registration is confirmed.
  * The 0430 CNF carries TLV8100/10B; only networkType u16LE@+1 is consumed
  * (x9cw_voice_rat). Fail-closed: a malformed/absent response leaves the cache
  * empty, so the CallList technology gate stays closed. */
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  auto &q=x9c_rat_query[sim];
  if(q.token){
   if(now>=q.deadline)q.held=true;
   uint32_t n=0;int rc=x9_md_mipc_take(q.token,frame,X9_MIPC_FRAME_MAX,&n);
   if(rc==1){
    unsigned networkType=0;
    bool good=n>=16&&x9cw_u16(frame+12)==q.token&&x9cw_voice_rat(frame,n,&networkType);
    q.token=0;
    if(!q.held&&good){x9c_voice_rat[sim].networkType=networkType;x9c_voice_rat[sim].valid=true;q.done=true;
     printf("X9 CALL: SIM%u voice RAT networkType %u cached (042f)\n",sim+1,networkType);}
    else if(!q.held){
     /* One 042f per SIM. A delivered but unusable 0430 is terminal for this
      * boot; resubmitting on the same gate spins 042f/0430 without a log and
      * consumes the shared 16-bit MIPC txid space. */
     q.held=true;
     printf("X9 CALL: SIM%u 042f response unusable; voice RAT cache stays empty\n",sim+1);
    }
    bzero(frame,X9_MIPC_FRAME_MAX);
   }else if(rc<0)q.held=true;
  }
  if(!online||q.held||q.done||q.token||!x9c_cmd_init.confirmed||
     x9c_init[sim].held||x9c_init[sim].stage!=X9_CI_DONE)continue;
  q.token=x9c_voice_rat_submit(sim);
  if(q.token)q.deadline=now+25000;else q.held=true;
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
  else if(index==X9_C_REG_WAITS&&(!w.used||!x9c_response_find(w.reply)))index=i;
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
 * No implicit ID/route adoption from a CNF, log prefix or another client.
 * MO targets the observed dial epoch; MT targets the owned CMD identity
 * after ECPI132/6 (CONVERSATION). Ending an still-INCOMING MT call is a
 * different, unobserved Android action and is not admitted here. */
static uint16_t x9c_end_begin(unsigned slot,const uint8_t *req,unsigned len){
 unsigned op,id;
 if(!x9cw_qmi_action(req,len,&op,&id)||op!=0x21||!id)return X9Q_ARG;
 const struct x9c_client &c=x9c_clients[slot];
 if(!c.bound||c.generation!=x9q_clients[slot].generation)return X9Q_NOT_READY;
 if(c.sim!=1)return X9Q_UNSUPPORTED; /* only the measured SIM2 mode20 subset */
 if(x9c_end_job.used||x9c_answer_job.used)return X9Q_BUSY;
 struct x9cw_outbound_state state;struct x9cw_inbound_state in;
 unsigned native_id,direction;uint64_t epoch;
 if(x9_md_mipc_call_snapshot(&state)==1&&x9cw_outbound_id(&state,&native_id)&&
    native_id==id&&state.mode==20){direction=0;epoch=state.epoch;}
 else if(x9_md_mipc_call_incoming_snapshot(&in)==1&&x9cw_inbound_id(&in,&native_id)&&
    native_id==id&&in.ios_state==3&&in.mode==20){direction=1;epoch=in.epoch;}
 else return X9Q_NOT_READY;
 const uint64_t now=x9q_ms();if(now>UINT64_MAX-25000)return X9Q_BUSY;
 const uint64_t deadline=now+25000;
 uint64_t reply=x9c_response_reserve(slot,req,deadline);if(!reply)return X9Q_BUSY;
 uint64_t token=x9c_current_submit(c.sim);if(!token){x9c_response_release(reply);return X9Q_BUSY;}
 struct x9c_end_job &j=x9c_end_job;bzero(&j,sizeof(j));j.used=true;j.slot=slot;j.sim=c.sim;
 j.generation=c.generation;j.bridge_generation=c.bridge_generation;
 j.binding=c.binding;j.epoch=epoch;j.id=id;j.direction=direction;j.stage=1;j.token=token;j.reply=reply;
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
    x9_md_mipc_call_end_snapshot(&target)==1&&target.epoch==j.epoch&&target.native_id==j.id&&
    target.sim==j.sim&&target.direction==j.direction;
   bzero(frame,X9_MIPC_FRAME_MAX);
   /* take/snapshot may wait for the MD mutex while the actual QMUX client
    * is released, before this thread can dispatch DEL_CLIENT. Revalidate
    * the read-only stage's owner/deadline before enqueueing a new mutation.
    * Native admission/selection also carries the original actual QMUX
    * allocation and deadline under MD -> bb_lock, not this local counter. */
   if(j.answered||!valid||!x9c_end_live(j)||x9q_ms()>=j.deadline){
    x9c_end_answer(j,X9Q_NOT_READY);bzero(&j,sizeof(j));return;
   }
   uint8_t action[32];unsigned k=x9cw_action(j.req,sizeof(j.req),action,sizeof(action),j.id,target.native_id,j.sim,1);
   const struct x9_mipc_call_owner owner={CBQ_PORT_BASE+j.slot,X9BB_NODE,j.bridge_generation,j.deadline};
   j.token=k?x9_md_mipc_call_end_submit(action,k,&target,&owner):0;bzero(action,sizeof(action));
   if(!j.token){
    x9c_end_answer(j,!x9c_end_live(j)||x9q_ms()>=j.deadline?X9Q_NOT_READY:X9Q_BUSY);
    bzero(&j,sizeof(j));return;
   }
   j.stage=2;return;
  }
  if(j.stage==2){
   uint32_t result;bool valid=x9cw_end_cnf(frame,n,token,&result);bzero(frame,X9_MIPC_FRAME_MAX);
   x9c_end_answer(j,valid&&!result?0:X9Q_INTERNAL);j.stage=3;
   /* Do NOT force END, free the epoch, or send CallList from CNF result. */
  }else if(j.stage==4){
   struct x9cw_current current;
   bool retired=x9cw_u16(frame+12)==token&&x9cw_current(frame,n,&current)&&!current.result&&!current.count;
   if(retired&&j.direction){
    struct x9cw_inbound_state in;
    retired=x9_md_mipc_call_incoming_snapshot(&in)==1&&!in.blocked&&in.epoch==j.epoch&&in.ended;
   }else if(retired){
    struct x9cw_outbound_state state;
    retired=x9_md_mipc_call_snapshot(&state)==1&&!state.blocked&&state.epoch==j.epoch&&state.ended&&
     state.count_known&&!state.native_count;
   }
   bzero(frame,X9_MIPC_FRAME_MAX);
   if(retired){bzero(&j,sizeof(j));return;}
   /* Single cleanup query; a stale/error snapshot holds the owner. No
    * repeated query loop, action retry or inferred idle after END. */
   j.stage=5;return;
  }
 }
 if(j.stage==3){
  bool ended;
  if(j.direction){
   struct x9cw_inbound_state in;
   ended=x9_md_mipc_call_incoming_snapshot(&in)==1&&!in.blocked&&in.epoch==j.epoch&&in.ended;
  }else{
   struct x9cw_outbound_state state;
   ended=x9_md_mipc_call_snapshot(&state)==1&&!state.blocked&&state.epoch==j.epoch&&state.ended;
  }
  if(ended){j.token=x9c_current_submit(j.sim);if(j.token)j.stage=4;}
 }
}
/* Bound VOICE22 only. Targets the owned MT identity while INCOMING2 (ECPI0
 * observed). Android capture: ECPI0 -> 0907{} -> 0908/result0; the empty
 * ANSWER cannot target a multi-call set, so the owned snapshot must hold
 * exactly this one call. Apple createAnswerIncomingCallReq 1f1a05da8 stores
 * an explicit 25000ms request guard; mirrored here, not an MD/network timer. */
static uint16_t x9c_answer_begin(unsigned slot,const uint8_t *req,unsigned len){
 unsigned op,id;
 if(!x9cw_qmi_action(req,len,&op,&id)||op!=0x22||!id)return X9Q_ARG;
 const struct x9c_client &c=x9c_clients[slot];
 if(!c.bound||c.generation!=x9q_clients[slot].generation)return X9Q_NOT_READY;
 if(c.sim!=1)return X9Q_UNSUPPORTED; /* only the measured SIM2 mode20 subset */
 if(x9c_answer_job.used||x9c_end_job.used||x9c_dial_job.used)return X9Q_BUSY;
 struct x9cw_inbound_state in;unsigned native_id;
 if(x9_md_mipc_call_incoming_snapshot(&in)!=1||!x9cw_inbound_id(&in,&native_id)||
    native_id!=id||in.ios_state!=2||in.mode!=20)return X9Q_NOT_READY;
 const uint64_t now=x9q_ms();if(now>UINT64_MAX-25000)return X9Q_BUSY;
 const uint64_t deadline=now+25000;
 uint64_t reply=x9c_response_reserve(slot,req,deadline);if(!reply)return X9Q_BUSY;
 uint64_t token=x9c_current_submit(c.sim);if(!token){x9c_response_release(reply);return X9Q_BUSY;}
 struct x9c_answer_job &j=x9c_answer_job;bzero(&j,sizeof(j));j.used=true;j.slot=slot;j.sim=c.sim;
 j.generation=c.generation;j.bridge_generation=c.bridge_generation;
 j.binding=c.binding;j.epoch=in.epoch;j.id=id;j.stage=1;j.token=token;j.reply=reply;
 j.deadline=deadline;j.transaction=(uint16_t)x9cw_u16(req+1);
 memcpy(j.req,req,sizeof(j.req));return 0;
}
static void x9c_answer_poll(uint8_t *frame){
 struct x9c_answer_job &j=x9c_answer_job;if(!j.used)return;
 if(!x9c_answer_live(j)||(!j.answered&&x9q_ms()>=j.deadline)){
  if(j.stage==2)x9_md_mipc_call_answer_abandon(j.token);
  x9c_answer_answer(j,X9Q_NOT_READY);
 }
 if(j.token){
  uint32_t n=0;int rc=x9_md_mipc_take(j.token,frame,X9_MIPC_FRAME_MAX,&n);
  if(!rc)return;
  if(rc<0){
   x9c_answer_answer(j,X9Q_NOT_READY);
   if(rc==-3){bzero(&j,sizeof(j));return;} /* proven pre-selection rejection */
   return; /* uncertain physical owner retained, no retry */
  }
  uint64_t token=j.token;j.token=0;
  if(j.stage==1){
   struct x9cw_current current;struct x9cw_end_target target;
   bool valid=x9cw_u16(frame+12)==token&&x9cw_current(frame,n,&current)&&!current.result&&
    x9_md_mipc_call_answer_snapshot(&target)==1&&target.epoch==j.epoch&&target.native_id==j.id&&
    target.sim==j.sim&&target.direction==1;
   bzero(frame,X9_MIPC_FRAME_MAX);
   if(j.answered||!valid||!x9c_answer_live(j)||x9q_ms()>=j.deadline){
    x9c_answer_answer(j,X9Q_NOT_READY);bzero(&j,sizeof(j));return;
   }
   uint8_t action[16];unsigned k=x9cw_action(j.req,sizeof(j.req),action,sizeof(action),j.id,target.native_id,j.sim,1);
   const struct x9_mipc_call_owner owner={CBQ_PORT_BASE+j.slot,X9BB_NODE,j.bridge_generation,j.deadline};
   j.token=k?x9_md_mipc_call_answer_submit(action,k,&target,&owner):0;bzero(action,sizeof(action));
   if(!j.token){
    x9c_answer_answer(j,!x9c_answer_live(j)||x9q_ms()>=j.deadline?X9Q_NOT_READY:X9Q_BUSY);
    bzero(&j,sizeof(j));return;
   }
   j.stage=2;return;
  }
  if(j.stage==2){
   uint32_t result;bool valid=x9cw_answer_cnf(frame,n,token,&result);bzero(frame,X9_MIPC_FRAME_MAX);
   x9c_answer_answer(j,valid&&!result?0:X9Q_INTERNAL);
   /* No cleanup query: ANSWER does not end the call. The MD answer lease
    * retires only through real END + owned empty + no busy mutation. */
   bzero(&j,sizeof(j));
  }
 }
}
static bool x9c_live(const struct x9c_dial_job &j){
 return x9c_bound_live(j.slot,j.sim,j.generation,j.binding);
}
/* Called with the owned CMD copy from the MD inbox drain. Only the measured
 * SIM2 ordinary IMS mode20 subset is tracked; SIM1/other shapes keep their
 * MD tombstone without approval (no captured profile). The number is never
 * copied out of the MD owner. */
static void x9c_incoming_deliver(const uint8_t *frame,unsigned n,uint32_t token){
 struct x9cw_incoming c;
 if(!x9cw_incoming(frame,n,&c))return;
 if(c.sim!=1||c.mode!=20||c.evoltesi_flow){
  printf("X9 CALL: incoming CMD id %u outside measured SIM2 mode20 subset; retained, not approved\n",c.call_id);
  return;
 }
 for(struct x9c_approve_owner &o:x9c_approve_owners)if(o.used&&o.token==token)return;
 for(struct x9c_approve_owner &o:x9c_approve_owners)if(!o.used){
  o.used=true;o.replied=false;o.token=token;o.sim=c.sim;o.id=c.call_id;
  o.sequence=c.sequence;o.txid=c.txid;o.mask=c.mask;
  printf("X9 CALL: SIM2 incoming CMD id %u seq %u owned; auto-approve pending (Android RSP shape)\n",c.call_id,c.sequence);
  return;
 }
 printf("X9 CALL: approve owner pool full; incoming CMD id %u retained, not approved\n",c.call_id);
}
/* Approval gates: live CALL transport, confirmed CMD8901 registration, and
 * the MD-owned inbound identity still matching this CMD (pending or already
 * ECPI0-seen, never ended/blocked). ECPI0 may outrun the lazy boot IND
 * registration; the RSP is built from CMD fields only, so seen is admitted.
 * A lost/ended/blocked identity can never grant approval. rc==0 keeps the
 * owner for the next poll's re-gate; physical failure is terminal, no retry. */
static void x9c_approve_poll(){
 const bool ready=x9c_event_transport_ready();
 struct x9cw_inbound_state in;
 const bool observed=ready&&x9_md_mipc_call_incoming_snapshot(&in)==1;
 for(struct x9c_approve_owner &o:x9c_approve_owners){
  if(!o.used)continue;
  if(!ready){bzero(&o,sizeof(o));continue;}
  if(o.replied){
   int result;int rc=x9_md_mipc_call_reply_take(o.token,&result);
   if(rc==1){
    printf("X9 CALL: SIM2 incoming id %u approve RSP physical TX %s\n",o.id,result==X9_AT_OK?"ok":"failed");
    bzero(&o,sizeof(o));
   }else if(rc<0)bzero(&o,sizeof(o));
   continue;
  }
  if(!x9c_cmd_init.confirmed)continue;
  if(!observed||in.blocked||in.ended||(!in.pending&&!in.seen)||
     in.native_id!=o.id||in.sequence!=o.sequence||in.txid!=o.txid||in.mask!=o.mask||in.sim!=o.sim){
   printf("X9 CALL: incoming id %u approve withheld; identity no longer pending\n",o.id);
   bzero(&o,sizeof(o));continue;
  }
  int rc=x9_md_mipc_call_reply(o.token,1,0,0);
  if(rc==1){
   o.replied=true;
   printf("X9 CALL: SIM2 incoming id %u approve=1 queued (no cause, original txid %u)\n",o.id,(unsigned)o.txid);
  }else if(rc<0)bzero(&o,sizeof(o));
 }
}
/* Observation only: the MD reducer already owns MT state. This log is not
 * a CallList receipt and contains no number. Logged on change so a boot log can verify
 * the incoming signalling chain without a probe image. */
static void x9c_mt_observe(const struct x9cw_inbound_state &in){
 static bool valid;static unsigned pending,seen,ended,blocked,state;static uint32_t id;
 if(valid&&pending==in.pending&&seen==in.seen&&ended==in.ended&&blocked==in.blocked&&
    state==in.ios_state&&id==in.native_id)return;
 valid=true;pending=in.pending;seen=in.seen;ended=in.ended;blocked=in.blocked;
 state=in.ios_state;id=in.native_id;
 if(in.blocked){printf("X9 CALL: MT reducer blocked; incoming approval/publication fail-closed\n");return;}
 if(!in.pending&&!in.seen)return;
 const char *s=in.ended?"END":in.ios_state==3?"CONVERSATION":in.ios_state==2?"INCOMING":"pending";
 printf("X9 CALL: SIM%u MT id %u %s (MD observation only; not iOS call UI acceptance)\n",
        in.sim+1,in.native_id,s);
}
/* CallTechnology provenance for the CallList46 item+4 byte. The proven source
 * is the real voice registration RAT: RIL requestVoiceRegistrationState 30f8f0
 * sends a HEADER-ONLY MIPC 042f (timeout 300000ms), response 8100 forced 10B,
 * networkType = u16LE@value+1 (30fa5c), cached per slot in RIL_CS_REG_STATE_CACHE
 * (stride 0x28). x9cw_calltechnology collapses convertCSNetworkType 2fd710 ->
 * voiceRat -> Apple RAT (语义对应, 推断) -> asWirelessTechnology 1f03304ac ->
 * convertWirelessTechnologyToCallTechnology 1f03921e8 (实测 WT4->CT2/WT8->CT1/
 * WT0x20->CT4/else->CT6; NR->CT6 is the real result, not a placeholder).
 * The kernel 042f read-only query/8100 parse that POPULATES this cache is
 * implemented in x9c_init_poll (x9c_voice_rat_submit + x9cw_voice_rat) and
 * runs once per SIM after CS registration. While the cache is empty this
 * provider returns no technology and CallList publication stays fail-closed.
 * docs 495-506 forbid rat2->LTE4 and Unknown placeholders; a real queried
 * networkType mapped by x9cw_calltechnology is the only admitted source. */
static int x9c_call_technology(unsigned sim,unsigned *technology){
 if(sim>=2||!x9c_voice_rat[sim].valid)return 0;
 return x9cw_calltechnology(x9c_voice_rat[sim].networkType,technology);
}
/* CallList46 publication owner. x9c_poll calls this only after VOICE09 is
 * announced by the bridge. Each client must still be bound/registered, own a
 * live QMUX allocation, and have a real 042f-derived CallTechnology. It
 * reconciles ONE owned call from the MD reducers (single-call SIM2 mode20
 * subset), revision-tracked so an unchanged state is not republished, encodes
 * via x9cw_calllist (no RemoteNumberList), and transfers an IOMallocData-owned
 * frame through the MD->bb batch. No number, no audio, no readiness claim. */
static bool x9c_calllist_announced=true;
static bool x9c_calllist_select_state(const struct x9cw_outbound_state &o,
                                     const struct x9cw_inbound_state &in,unsigned sim,
                                     unsigned *id,unsigned *ios_state,unsigned *direction,
                                     struct x9cw_end_reason *reason,uint64_t *epoch,uint64_t *revision){
 if(sim!=1)return false;
 if(in.sim==sim&&in.seen&&!in.blocked&&!in.pending&&in.native_id&&in.native_id<=255){
  *id=in.native_id;*ios_state=in.ios_state;*direction=2;*reason=in.end_reason;
  *epoch=in.epoch;*revision=in.revision;return true;
 }
 if(o.seen&&!o.blocked&&o.native_id&&o.native_id<=255){
  *id=o.native_id;*ios_state=o.ios_state;*direction=1;*reason=o.end_reason;
  *epoch=o.epoch;*revision=o.revision;return true;
 }
 return false;
}
static bool x9c_calllist_select(unsigned sim,unsigned *id,unsigned *ios_state,unsigned *direction,
                               struct x9cw_end_reason *reason,uint64_t *epoch,uint64_t *revision){
 struct x9cw_outbound_state o;struct x9cw_inbound_state in;
 if(x9_md_mipc_call_snapshot(&o)!=1||x9_md_mipc_call_incoming_snapshot(&in)!=1)return false;
 return x9c_calllist_select_state(o,in,sim,id,ios_state,direction,reason,epoch,revision);
}
struct x9c_calllist_publication {
 unsigned slot,sim,id,direction;uint64_t generation,binding,bridge_generation,epoch,revision;
 bb_x9_call_frame frame;
};
static int x9c_calllist_publish_callback(void *context,
                                       const struct x9cw_outbound_state *outbound,
                                       const struct x9cw_inbound_state *inbound){
 struct x9c_calllist_publication &p=*static_cast<x9c_calllist_publication *>(context);
 const struct x9c_client &c=x9c_clients[p.slot];
 if(!x9w_control_ready||!c.registered||c.bridge_generation!=p.bridge_generation||
    !x9c_bound_live(p.slot,p.sim,p.generation,p.binding))return 0;
 unsigned id,ios_state,direction;struct x9cw_end_reason reason;uint64_t epoch,revision;
 if(!x9c_calllist_select_state(*outbound,*inbound,p.sim,&id,&ios_state,&direction,&reason,&epoch,&revision)||
    id!=p.id||direction!=p.direction||epoch!=p.epoch||revision!=p.revision)return 0;
 return bb_x9_call_calllist(CBQ_PORT_BASE+p.slot,X9BB_NODE,c.bridge_generation,&p.frame,1)?1:0;
}
static void x9c_calllist_flush(unsigned slot){
 if(!x9c_calllist_announced||slot>=CBQ_MAX_CLIENTS)return;
 struct x9c_client &c=x9c_clients[slot];
 if(!c.bound||!c.registered||c.generation!=x9q_clients[slot].generation||!x9c_event_transport_ready())return;
 unsigned id,ios_state,direction;struct x9cw_end_reason reason={0,0};uint64_t epoch,revision;
 if(!x9c_calllist_select(c.sim,&id,&ios_state,&direction,&reason,&epoch,&revision))return;
 if(c.calllist_valid&&c.calllist_epoch==epoch&&c.calllist_revision==revision&&
    c.calllist_direction==direction&&c.calllist_id==id)return;
 unsigned technology;
 if(!x9c_call_technology(c.sim,&technology))return; /* fail-closed: no proven RAT */
 uint8_t sdu[25];unsigned size=x9cw_calllist(sdu,sizeof(sdu),id,ios_state,direction,technology,1,&reason);
 if(!size)return;
 uint32_t floor;
 if(x9_md_mipc_call_cursor(&floor)!=1)return;
 x9c_calllist_publication p={};p.slot=slot;p.sim=c.sim;p.id=id;p.direction=direction;
 p.generation=c.generation;p.binding=c.binding;p.bridge_generation=c.bridge_generation;
 p.epoch=epoch;p.revision=revision;
 p.frame.sdu=sdu;p.frame.size=size;
 p.frame.owned=static_cast<uint8_t *>(IOMallocData(CBQ_HDR+size));
 if(!p.frame.owned)return;
 int rc=x9_md_mipc_call_publish_state(floor,x9c_calllist_publish_callback,&p);
 if(rc!=1){bzero(p.frame.owned,CBQ_HDR+size);IOFreeData(p.frame.owned,CBQ_HDR+size);}
 else{c.calllist_valid=true;c.calllist_epoch=epoch;c.calllist_revision=revision;
  c.calllist_direction=direction;c.calllist_id=id;}
 bzero(sdu,sizeof(sdu));
}
static void x9c_error(struct x9c_dial_job &j,uint16_t err){
 if(!j.answered&&x9c_live(j)){
  x9c_response_decide(j.reply,err);
 }
 if(!j.answered&&!x9c_live(j))x9c_response_release(j.reply);
 j.answered=true;bzero(j.req,sizeof(j.req));
}
/* VOICE20 dispatch uses the measured SIM2 ordinary-IMS profile. The caller
 * must already be bound/registered on a live bridge, have no changing bind or
 * outstanding dial owner, and present an owned empty GET_STATUS baseline. */
static bool x9c_dial_submit(unsigned slot,const uint8_t *req,unsigned len,
                            const struct x9cw_dial_profile &profile){
 if(slot>=CBQ_MAX_CLIENTS||!x9c_clients[slot].bound||x9c_clients[slot].sim!=profile.sim||
    !x9c_clients[slot].registered||x9c_clients[slot].generation!=x9q_clients[slot].generation||
    !x9c_event_transport_ready()||x9c_bind_pending(slot,true)||x9c_dial_job.used||len>sizeof(x9c_dial_job.req))return false;
 struct x9cw_outbound_state state;struct x9cw_dial_join join={};
 if(x9_md_mipc_call_snapshot(&state)!=1||!x9cw_dial_join_begin(&join,&state,&profile))return false;
 uint8_t frame[104];unsigned n=x9cw_dial(req,len,frame,sizeof(frame),&profile,state.native_count);
 if(!n)return false;
 const uint64_t now=x9q_ms();
 if(now>UINT64_MAX-60000){bzero(frame,sizeof(frame));return false;}
 const uint64_t deadline=now+60000;
 uint64_t reply=x9c_response_reserve(slot,req,deadline);
 if(!reply){bzero(frame,sizeof(frame));return false;}
 const struct x9_mipc_call_owner owner={CBQ_PORT_BASE+slot,X9BB_NODE,x9c_clients[slot].bridge_generation,deadline};
 uint64_t token=x9_md_mipc_call_dial_submit(frame,n,&state,&owner);bzero(frame,sizeof(frame));
 if(!token){x9c_response_release(reply);return false;}
 /* Accepted mutations retain ownership across client deletion/timeouts.
  * No cancellation, automatic hangup, retry, or request-token reuse. */
 struct x9c_dial_job &j=x9c_dial_job;bzero(&j,sizeof(j));j.used=true;
 j.slot=slot;j.sim=profile.sim;j.generation=x9q_clients[slot].generation;
 j.binding=x9c_clients[slot].binding;j.len=len;j.transaction=x9cw_u16(req+1);memcpy(j.req,req,len);
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
 if(x9c_answer_job.used&&x9c_answer_job.slot==slot){
  if(x9c_answer_job.stage==2)x9_md_mipc_call_answer_abandon(x9c_answer_job.token);
  x9c_answer_job.answered=true;bzero(x9c_answer_job.req,sizeof(x9c_answer_job.req));
 }
 if(slot<CBQ_MAX_CLIENTS)bzero(&x9c_clients[slot],sizeof(x9c_clients[slot]));
 if(x9c_dial_job.used&&x9c_dial_job.slot==slot){x9c_dial_job.answered=true;bzero(x9c_dial_job.req,sizeof(x9c_dial_job.req));}
 /* Keep token/epoch even if this QRTR client slot is reused. */
}
static void x9c_dial_poll(uint8_t *frame);
static void x9c_poll(){
 /* Drain raw CALL events. CMD8901 is auto-approved in the measured subset
  * (Android RSP shape); ANSWER is never sent. The MD owner has already
  * reduced every complete4901 under its lock. Number buffers are erased
  * immediately; CMD identity tokens remain in MD owner. */
 uint8_t frame[X9_MIPC_FRAME_MAX];uint32_t n,token,sequence;
 for(unsigned i=0;i<8;i++){
  int rc=x9_md_mipc_call_event_ordered(frame,sizeof(frame),&n,&token,&sequence);
  if(rc!=1)break;
  if(x9cw_u16(frame+10)==0x4902)x9c_handover_event(frame,n,sequence);
  else if(x9cw_u16(frame+10)==0x8901)x9c_incoming_deliver(frame,n,token);
  bzero(frame,sizeof(frame));
 }
 x9c_observed=x9_md_mipc_call_snapshot(&x9c_outbound)==1;
 x9c_incoming_observed=x9_md_mipc_call_incoming_snapshot(&x9c_inbound)==1;
 if(x9c_incoming_observed)x9c_mt_observe(x9c_inbound);
 x9c_approve_poll();
 x9c_init_poll(frame);
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)x9c_handover_flush(slot);
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)x9c_calllist_flush(slot);
 x9c_end_poll(frame);
 x9c_answer_poll(frame);
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
   if(rc==-3){
    /* Writer proved no selection/TX; the native lease was retired there.
     * The separate response owner retains any undelivered terminal result. */
    x9c_error(j,X9Q_NOT_READY);bzero(&j,sizeof(j));return;
   }
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
   const bool valid=x9cw_qmi_register(req,len,&options);
   for(unsigned index=0;index<X9_C_REG_WAITS;index++){
    const struct x9c_reg_wait &w=x9c_reg_wait[slot][index];
    if(w.used&&w.generation==x9q_clients[slot].generation&&w.transaction==x9cw_u16(req+1)){
     if(valid&&w.options.present==options.present&&w.options.call_events==options.call_events&&
        w.options.handover==options.handover)return true;
     /* Changed or malformed reuse resolves through the original owner,
      * never through an independent response. */
     if(!w.answered){x9c_reg_answer(slot,index,X9Q_ARG);x9c_response_poll();}
     // An answered owner already has its first terminal (queued or retained).
     // Changed reuse must not append ARG after that transaction's success/error.
     return true;
    }
   }
   if(!valid){x9q_packet p(req,X9Q_ARG);p.send(local,port);return true;}
   err=x9c_bind_pending(slot,true)?X9Q_BUSY:x9c_reg_begin(slot,req,len);if(!err)return true;
  }else if(x9cw_u16(req+3)==0x20){
   /* The accepted owner fences every later request under its transaction,
    * including a changed/malformed retry: one terminal, one MD mutation. */
   if(x9c_dial_job.used&&x9c_dial_job.slot==slot&&
      x9c_dial_job.generation==x9q_clients[slot].generation&&
      x9c_dial_job.transaction==x9cw_u16(req+1))return true;
   struct x9cw_dial_number dial;
   if(!x9cw_qmi_dial(req,len,&dial))err=X9Q_ARG;
   else if(x9c_bind_pending(slot,true))err=X9Q_BUSY;
   else if(!x9c_clients[slot].bound||!x9c_clients[slot].registered||
           x9c_clients[slot].generation!=x9q_clients[slot].generation)err=X9Q_NOT_READY;
   else if(x9c_clients[slot].sim!=1)err=X9Q_UNSUPPORTED;
   else if(x9c_dial_job.used)err=X9Q_BUSY;
   else if(x9c_dial_submit(slot,req,len,x9c_measured_dial_profile)){
    printf("X9 CALL: VOICE20 txn %u admitted on measured SIM2 route (digits %u); no audio readiness implied\n",
     x9cw_u16(req+1),dial.length);
    return true;
   }else err=X9Q_NOT_READY;
  }else if(x9cw_u16(req+3)==0x21){
   /* The live allocation's validated host transaction already owns its
    * target. Even a malformed retry cannot append ARG or a changed ID BUSY
    * before the original success,
    * or retarget its MD command. Keep identity after req is erased; actual
    * QMUX allocation is fenced by end_live, not only the local generation. */
   if(x9c_end_job.used&&
      x9c_end_job.slot==slot&&x9c_end_job.generation==x9q_clients[slot].generation&&
      x9c_end_job.transaction==x9cw_u16(req+1)&&x9c_end_live(x9c_end_job))return true;
   err=x9c_bind_pending(slot,true)?X9Q_BUSY:x9c_end_begin(slot,req,len);if(!err)return true; /* asynchronous owned reply */
  }else if(x9cw_u16(req+3)==0x22){
   /* Same repeat-transaction fence as END: the live allocation owns its
    * target; malformed/changed retries cannot append ARG/BUSY before the
    * original result. */
   if(x9c_answer_job.used&&
      x9c_answer_job.slot==slot&&x9c_answer_job.generation==x9q_clients[slot].generation&&
      x9c_answer_job.transaction==x9cw_u16(req+1)&&x9c_answer_live(x9c_answer_job))return true;
   err=x9c_bind_pending(slot,true)?X9Q_BUSY:x9c_answer_begin(slot,req,len);if(!err)return true;
  }
 }
 x9q_packet p(req,err);p.send(local,port);return true;
}
