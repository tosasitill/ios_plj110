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
 * MIPC call request/registration/approval is started automatically. Other
 * VOICE requests get unsupported, never a success bootstrap/readiness stub.
 * Explicit VOICE44 routing and guarded VOICE21 end-current-call now have
 * production dispatch. This does not publish VOICE, register the MD, admit
 * a DIAL profile, or report call/audio readiness. */
#include "../../../bsd/dev/x9_call_state.h"
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
struct x9c_client {bool bound;unsigned sim;uint64_t generation;};
static x9c_client x9c_clients[CBQ_MAX_CLIENTS];
struct x9c_end_job {
 bool used,answered;unsigned slot,sim,stage,id;
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
 j.deadline=x9q_ms()+25000;memcpy(j.req,req,sizeof(j.req));return 0;
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
 uint8_t frame[X9_MIPC_FRAME_MAX];uint32_t n,token;
 for(unsigned i=0;i<8;i++){
  int rc=x9_md_mipc_call_event(frame,sizeof(frame),&n,&token);
  if(rc!=1)break;
  bzero(frame,sizeof(frame));
 }
 x9c_observed=x9_md_mipc_call_snapshot(&x9c_outbound)==1;
 x9c_incoming_observed=x9_md_mipc_call_incoming_snapshot(&x9c_inbound)==1;
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
    x9c_clients[slot].bound=true;x9c_clients[slot].sim=sim;
    x9c_clients[slot].generation=x9q_clients[slot].generation;err=0;
   }
  }else if(x9cw_u16(req+3)==0x21){
   err=x9c_end_begin(slot,req,len);if(!err)return true; /* asynchronous owned reply */
  }
 }
 x9q_packet p(req,err);p.send(local,port);return true;
}
