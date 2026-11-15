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
 * MIPC call request/registration/approval is started automatically. VOICE
 * requests get a real unsupported result, never a success bootstrap stub.
 * Guarded staging is limited to one attempted dial per boot; there is no
 * admitted action/release path yet. This is NOT the production capacity. */
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
 struct x9c_dial_job &j=x9c_dial_job;if(!j.used)return;
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
 x9q_packet p(req,X9Q_UNSUPPORTED);p.send(local,port);(void)len;return true;
}
