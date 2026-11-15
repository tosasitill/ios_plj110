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
 uint64_t generation,deadline,token;struct x9cw_dial_join join;
 uint8_t req[64];
};
static x9c_dial_job x9c_dial_job;
static struct x9cw_outbound_state x9c_outbound;
static struct x9cw_inbound_state x9c_inbound;
static bool x9c_observed;
static bool x9c_incoming_observed;
/* VOICE binding is independent of NAS/WMS clients and has NO SIM default. */
struct x9c_client {bool bound,registered,handover;unsigned sim;uint32_t handover_floor;uint64_t generation;};
static x9c_client x9c_clients[CBQ_MAX_CLIENTS];
struct x9c_end_job {
 bool used,answered;unsigned slot,sim,stage,id;
 uint16_t transaction;
 uint64_t generation,deadline,token,epoch;
 uint8_t req[11];
};
static x9c_end_job x9c_end_job;
static bool x9c_end_live(const struct x9c_end_job &j){
 return j.slot<CBQ_MAX_CLIENTS&&x9q_clients[j.slot].generation==j.generation&&
  x9c_clients[j.slot].bound&&x9c_clients[j.slot].generation==j.generation&&x9c_clients[j.slot].sim==j.sim;
}
static void x9c_end_answer(struct x9c_end_job &j,uint16_t err){
 if(!j.answered&&x9c_end_live(j)){x9q_packet p(j.req,err);p.send(CBQ_PORT_BASE+j.slot,X9BB_PORT_BASE+9);}
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
 uint64_t generation,deadline;uint8_t req[15];
 struct x9cw_voice_register options;uint32_t floor;
 unsigned event_count;uint8_t events[8];
};
/* Apple sends13=0 and14=1 as independent transactions. Do not turn a
 * valid concurrent bootstrap into BUSY; overlapping options are BUSY. */
#define X9_C_REG_WAITS 2u
static struct x9c_reg_wait x9c_reg_wait[CBQ_MAX_CLIENTS][X9_C_REG_WAITS];
static struct x9cw_call_init x9c_init[2];
static struct {uint64_t token,deadline;bool confirmed,held;} x9c_cmd_init;
static bool x9c_reg_live(unsigned slot,const struct x9c_reg_wait &w){
 return slot<CBQ_MAX_CLIENTS&&x9q_clients[slot].generation==w.generation&&
  x9c_clients[slot].bound&&x9c_clients[slot].generation==w.generation&&x9c_clients[slot].sim==w.sim;
}
static bool x9c_event_transport_ready(){
 uint32_t cursor;
 /* A live generic MIPC transport is insufficient when the independent
  * CALL inbox is quarantined. Do not grant/flush a dead subscription. */
 return x9w_control_ready&&x9_md_mipc_online()&&x9_md_mipc_call_cursor(&cursor)==1;
}
static void x9c_handover_send(unsigned slot,unsigned state){
 uint8_t indication[11];unsigned n=x9cw_handover_indication(indication,sizeof(indication),state);
 if(n)coachz_baseband_qrtr(CBQ_PORT_BASE+slot,X9BB_NODE,X9BB_PORT_BASE+9,indication,n);
 bzero(indication,sizeof(indication));
}
struct x9c_reg_publication {unsigned slot,count;const struct x9c_reg_wait *wait;bb_x9_call_frame frames[9];};
static int x9c_reg_publish_callback(void *context){
 struct x9c_reg_publication &p=*static_cast<x9c_reg_publication *>(context);
 if(!x9w_control_ready||!x9c_reg_live(p.slot,*p.wait)||x9q_ms()>=p.wait->deadline)return 0;
 return bb_x9_call_registration(CBQ_PORT_BASE+p.slot,X9BB_NODE,p.frames,p.count,p.wait->deadline)?1:0;
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
 if(!w.answered&&x9c_reg_live(slot,w)){
  if(!error){
   int rc=x9c_reg_publish(slot,w);
   if(!rc)return; // retain request/events until admission or original deadline
   if(rc<0)error=X9Q_NOT_READY;
  }
  if(error){x9q_packet p(w.req,error);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+9);}
  struct x9c_client &c=x9c_clients[slot];
  if(!error){
   c.registered=true;
   if(w.options.present&2){
    bool enabled=c.handover;c.handover=w.options.handover!=0;
    if(c.handover&&!enabled){
     c.handover_floor=w.floor;
     /* Result and retained START/COMPLETE are already in one admitted batch. */
    }
   }
  }
 }
 w.result=error;w.answered=true;bzero(w.req,sizeof(w.req));
 bzero(w.events,sizeof(w.events));w.event_count=0;
}
static void x9c_handover_event(const uint8_t *frame,unsigned n,uint32_t sequence){
 if(!x9c_event_transport_ready())return;
 struct x9cw_handover e;if(x9cw_handover(frame,n,&e)!=1)return;
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){
  struct x9c_client &c=x9c_clients[slot];
  if(!c.bound||c.generation!=x9q_clients[slot].generation||c.sim!=e.sim)continue;
  if(c.registered&&c.handover&&sequence>c.handover_floor)x9c_handover_send(slot,e.ios_state);
  else for(unsigned index=0;index<X9_C_REG_WAITS;index++){
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
  for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){x9c_clients[i].registered=false;x9c_clients[i].handover=false;}
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
 bzero(&w,sizeof(w));w.used=true;w.sim=c.sim;w.len=len;
 w.generation=c.generation;w.transaction=(uint16_t)x9cw_u16(req+1);
 w.options=r;w.floor=floor;
 w.deadline=x9q_ms()+25000;memcpy(w.req,req,len);return 0;
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
 uint64_t token=x9c_current_submit(c.sim);if(!token)return X9Q_BUSY;
 struct x9c_end_job &j=x9c_end_job;bzero(&j,sizeof(j));j.used=true;j.slot=slot;j.sim=c.sim;
 j.generation=c.generation;j.epoch=state.epoch;j.id=id;j.stage=1;j.token=token;
 /* Apple createEndThisCallReq19a00d4: explicit25000ms request guard. */
 j.deadline=x9q_ms()+25000;j.transaction=(uint16_t)x9cw_u16(req+1);
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
   if(j.answered||!valid){x9c_end_answer(j,X9Q_NOT_READY);bzero(&j,sizeof(j));return;}
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
 return j.slot<CBQ_MAX_CLIENTS&&x9q_clients[j.slot].generation==j.generation&&x9q_clients[j.slot].sim_slot==j.sim;
}
static void x9c_error(struct x9c_dial_job &j,uint16_t err){
 if(!j.answered&&x9c_live(j)){
  x9q_packet p(j.req,err);p.send(CBQ_PORT_BASE+j.slot,X9BB_PORT_BASE+9);
 }
 j.answered=true;bzero(j.req,sizeof(j.req));
}
/* Admission is deliberately NOT called by x9c_request until actual CALL
 * initialization, VOICE SIM binding and runtime profile policy are proven.
 * A future caller must supply that context; even then, require a complete
 * owned GET_STATUS empty baseline and no outstanding dial owner. */
static bool x9c_dial_submit(unsigned slot,const uint8_t *req,unsigned len,
                            const struct x9cw_dial_profile &profile){
 if(slot>=CBQ_MAX_CLIENTS||x9q_clients[slot].sim_slot!=profile.sim||
    x9c_dial_job.used||len>sizeof(x9c_dial_job.req))return false;
 struct x9cw_outbound_state state;struct x9cw_dial_join join={};
 if(x9_md_mipc_call_snapshot(&state)!=1||!x9cw_dial_join_begin(&join,&state,&profile))return false;
 uint8_t frame[104];unsigned n=x9cw_dial(req,len,frame,sizeof(frame),&profile,state.native_count);
 if(!n)return false;
 uint64_t token=x9_md_mipc_call_dial_submit(frame,n,&state);bzero(frame,sizeof(frame));
 if(!token)return false;
 /* Accepted mutations retain ownership across client deletion/timeouts.
  * No cancellation, automatic hangup, retry, or request-token reuse. */
 struct x9c_dial_job &j=x9c_dial_job;bzero(&j,sizeof(j));j.used=true;
 j.slot=slot;j.sim=profile.sim;j.generation=x9q_clients[slot].generation;j.len=len;memcpy(j.req,req,len);
 j.join=join;j.token=token;j.deadline=x9q_ms()+60000;
 if(!x9cw_dial_join_sent(&j.join,token)){j.held=true;x9c_error(j,X9Q_INTERNAL);}
 return true;
}
static void x9c_del(unsigned slot){
 if(slot<CBQ_MAX_CLIENTS)bzero(x9c_reg_wait[slot],sizeof(x9c_reg_wait[slot]));
 if(x9c_end_job.used&&x9c_end_job.slot==slot){
  if(x9c_end_job.stage==2)x9_md_mipc_call_end_abandon(x9c_end_job.token);
  x9c_end_job.answered=true;bzero(x9c_end_job.req,sizeof(x9c_end_job.req));
 }
 if(slot<CBQ_MAX_CLIENTS)bzero(&x9c_clients[slot],sizeof(x9c_clients[slot]));
 if(x9c_dial_job.used&&x9c_dial_job.slot==slot){x9c_dial_job.answered=true;bzero(x9c_dial_job.req,sizeof(x9c_dial_job.req));}
 /* Keep token/epoch even if this QRTR client slot is reused. */
}
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
 x9c_end_poll(frame);
 struct x9c_dial_job &j=x9c_dial_job;if(!j.used)return;
 if(j.answered&&j.cnf&&!j.token&&!j.join.failed&&x9c_observed&&!x9c_outbound.blocked&&
    j.join.epoch_floor!=UINT64_MAX&&x9c_outbound.epoch==j.join.epoch_floor+1&&
    x9c_outbound.ended&&x9c_outbound.count_known&&!x9c_outbound.native_count){
  bzero(&j,sizeof(j));return; /* real END + owned empty, not dial/hangup CNF */
 }
 if(!x9c_live(j)){j.answered=true;bzero(j.req,sizeof(j.req));}
 if(j.token){
  int rc=x9_md_mipc_take(j.token,frame,sizeof(frame),&n);
  if(rc==1){
   j.token=0;j.cnf=true;
   if(!x9cw_dial_join_cnf(&j.join,frame,n)){j.held=true;x9c_error(j,X9Q_INTERNAL);}
   bzero(frame,sizeof(frame));
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
  coachz_baseband_qrtr(CBQ_PORT_BASE+j.slot,X9BB_NODE,X9BB_PORT_BASE+9,response,size);
 }
 j.answered=true;j.held=true;bzero(j.req,sizeof(j.req));
}
static bool x9c_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned len){
 if(port!=X9BB_PORT_BASE+9)return false;
 if(!req||len<7)return true;
 unsigned slot=local-CBQ_PORT_BASE;
 uint16_t err=X9Q_UNSUPPORTED;
 if(local>=CBQ_PORT_BASE&&slot<CBQ_MAX_CLIENTS&&req[0]==0){
  if(x9cw_u16(req+3)==0x44){
   unsigned sim;
   if(!x9cw_qmi_bind(req,len,&sim)||sim>=x9_md_sim_count())err=X9Q_ARG;
   else if(x9c_end_job.used&&x9c_end_job.slot==slot&&
           x9c_end_job.generation==x9q_clients[slot].generation&&x9c_end_job.sim!=sim)err=X9Q_BUSY;
   else{
    bool busy=false;
    for(unsigned index=0;index<X9_C_REG_WAITS;index++){
     const struct x9c_reg_wait &w=x9c_reg_wait[slot][index];
     if(w.used&&!w.answered&&w.generation==x9q_clients[slot].generation&&w.sim!=sim)busy=true;
    }
    if(busy){x9q_packet p(req,X9Q_BUSY);p.send(local,port);return true;}
    if(!x9c_clients[slot].bound||x9c_clients[slot].sim!=sim||
       x9c_clients[slot].generation!=x9q_clients[slot].generation){
     bzero(&x9c_clients[slot],sizeof(x9c_clients[slot]));
     bzero(x9c_reg_wait[slot],sizeof(x9c_reg_wait[slot]));
    }
    x9c_clients[slot].bound=true;x9c_clients[slot].sim=sim;
    x9c_clients[slot].generation=x9q_clients[slot].generation;err=0;
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
     if(!w.answered)x9c_reg_answer(slot,index,X9Q_ARG);
     else {x9q_packet p(req,X9Q_ARG);p.send(local,port);}
     return true;
    }
   }
   err=x9c_reg_begin(slot,req,len);if(!err)return true;
  }else if(x9cw_u16(req+3)==0x21){
   unsigned op,id;
   /* Same validated host transaction while its epoch is still owned is
    * not a second request or a reason to send BUSY and later success for
    * the same transaction. Keep scalar identity after req is erased. */
   if(x9cw_qmi_action(req,len,&op,&id)&&x9c_end_job.used&&
      x9c_end_job.slot==slot&&x9c_end_job.generation==x9q_clients[slot].generation&&
      x9c_end_job.transaction==x9cw_u16(req+1)&&x9c_end_job.id==id)return true;
   err=x9c_end_begin(slot,req,len);if(!err)return true; /* asynchronous owned reply */
  }
 }
 x9q_packet p(req,err);p.send(local,port);return true;
}
