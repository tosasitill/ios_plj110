/* 20H392 Mav20 WDS SetAttachProfileInfo3GPP (00c8), not WDS Start.
 * PLJ110 RmmDcEventHandler::requestSetInitialAttachApn (2c9cc4..2c9d88):
 * APN_SET_IA_REQ 0101 -> APN_SET_IA_CNF 0102. Unlike that RIL's unconditional
 * RIL_Errno0, require a unique result0/u32 == 0 on a complete matched CNF.
 * Internet/no-auth/single final replace only. No IMS, profile-index/PCO conversion, AT
 * writes, borrowed bearer, attach-status fiction or automatic dial/reset.
 * Final Action2 alone requests PS detach/reattach, each with a real CNF.
 * Serialize mutations against DDS and every owned WDS bearer. Route/host
 * generation/card epoch are snapshotted; queued/sent owners survive timeout,
 * DEL_CLIENT and card changes. APN is bounded RAM-only and never logged.
 */
static bool x9ds_mutating();
enum { X9A_CONFIG=1, X9A_DETACH, X9A_REATTACH };
struct x9a_operation {
 bool used,hold,timed_out;
 unsigned slot,sim_slot,len,action,phase;
 uint64_t generation,epoch,token,deadline;
 uint8_t req[X9Q_REQ];
 struct waiter {bool used,reply;unsigned slot;uint64_t generation;uint8_t req[7];} waiters[8];
};
static x9a_operation x9a_op;
static uint64_t x9a_epoch;
static bool x9a_busy(){return x9a_op.used;}
static bool x9a_live(const x9a_operation::waiter &w){return w.slot<CBQ_MAX_CLIENTS&&x9q_clients[w.slot].generation==w.generation;}
static bool x9a_client_busy(unsigned slot){
 if(x9a_op.used)for(const auto &w:x9a_op.waiters)if(w.used&&w.slot==slot&&x9a_live(w))return true;
 return false;
}
static void x9a_reply(unsigned error){
 for(auto &w:x9a_op.waiters){
  if(w.used&&w.reply&&x9a_live(w)){
   x9q_packet p(w.req,(uint16_t)error);p.send(CBQ_PORT_BASE+w.slot,X9BB_PORT_BASE+1);
  }w.reply=false;
 }
}
static void x9a_del(unsigned slot){
 for(auto &w:x9a_op.waiters)if(w.used&&w.slot==slot&&x9a_live(w))w.reply=false;
 // never cancel the mutation, even if the last waiter is gone
}
static bool x9a_waiter(unsigned slot,const uint8_t *req){
 for(auto &w:x9a_op.waiters)if(!w.used){
  w.used=w.reply=true;w.slot=slot;w.generation=x9q_clients[slot].generation;memcpy(w.req,req,7);return true;
 }return false;
}
static void x9a_cards_changed(){
 ++x9a_epoch;
 if(x9a_op.used)x9a_reply(X9Q_NOT_READY); // late old-card CNF cannot ACK a new card
}
static unsigned x9a_args(const uint8_t *req,unsigned n,x9w_frame &f,const char *&why){
 x9q_view home=x9q_tlv(req,n,0x11),apn=x9q_tlv(req,n,0x14),pco=x9q_tlv(req,n,0x1f);
 x9q_view append=x9q_tlv(req,n,0x90),action=x9q_tlv(req,n,0x91),types=x9q_tlv(req,n,0xdd);
 why="shape";
 if(!home.p||home.n!=1||!apn.p||!pco.p||pco.n!=1||!append.p||append.n!=1||
    !action.p||action.n!=4||!types.p||types.n!=8||!x9w_apn(apn.p,apn.n))return X9Q_ARG;
 // These are distinct QMI enums! Primary 0/2/3 -> MIPC IP1/IPv6 2/dual3;
 // LTE/UMTS roaming 0/1/2 -> MIPC 1/2/3 (AllowedProtocolMask minus one).
 unsigned protocol=home.p[0]==0?1:home.p[0]==2?2:home.p[0]==3?3:0;
 why="primary";if(!protocol)return X9Q_UNSUPPORTED;
 // 20H392 1f1a02c20..30 OVERWRITES the last entry's temporary Action0:
 // final Action1 (NO_DETACH) / Action2 (DETACH). Qualcomm's matching
 // ps_sys_attach_profile_action_enum_type confirms 0 is an intermediate
 // profile, not commit. Never commit it early or pretend append is replace.
 why="intermediate-or-unknown-action";
 if(x9q_u32(action.p)!=1&&x9q_u32(action.p)!=2)return X9Q_UNSUPPORTED;
 why="append";if(append.p[0])return X9Q_UNSUPPORTED;
 // 0x1f is ProxyAddrUsingPco, NOT auth. AuthPref is 0x1d.
 why="pco";if(pco.p[0])return X9Q_UNSUPPORTED;
 why="apn-type-mask";if(x9q_u32(types.p)!=1||x9q_u32(types.p+4))return X9Q_UNSUPPORTED;
 unsigned roaming=protocol,explicit_roaming=0;
 for(unsigned at=7;at<n;at+=3+x9q_u16(req+at+1)){
  unsigned tag=req[at],k=x9q_u16(req+at+1);const uint8_t *v=req+at+3;
  switch(tag){
  case 0x11:case 0x14:case 0x1f:case 0x90:case 0x91:case 0xdd:break;
  case 0x41:case 0x42:{
   why="roaming-shape";
   if(k!=4)return X9Q_ARG;
   why="roaming-enum";
   unsigned value=x9q_u32(v);if(value>2)return X9Q_UNSUPPORTED;
   // MIPC has ONE roaming setting. No independently supplied UMTS/LTE
   // policy is discarded: if both are supplied they must agree. Omitted
   // overrides supply no separate policy; absent both use the home type.
   why="conflicting-roaming";if(explicit_roaming&&roaming!=value+1)return X9Q_UNSUPPORTED;
   roaming=value+1;explicit_roaming++;break;
  }
  case 0x1d:why="auth";if(k!=4)return X9Q_ARG;if(x9q_u32(v))return X9Q_UNSUPPORTED;break;
  case 0x1b:case 0x1c:why="credentials";if(k)return X9Q_UNSUPPORTED;break;
  default:why="unmapped-tlv";return X9Q_UNSUPPORTED;
  }
 }
 char name[101]={};if(apn.n)memcpy(name,apn.p,apn.n);
 f.tlv(0x101,name,apn.n+1);bzero(name,sizeof(name));
 f.byte(0x102,(uint8_t)protocol);f.byte(0x103,(uint8_t)roaming);f.byte(0x104,0);
 static const char empty[1]={};f.tlv(0x8105,empty,1);f.tlv(0x8106,empty,1);
 why=f.good?"accepted":"frame";return f.good?0:X9Q_ARG;
}
static unsigned x9a_scalar(const uint8_t *req,unsigned n,uint8_t tag,unsigned size){
 x9q_view v=x9q_tlv(req,n,tag);
 return !v.p||v.n!=size?~0u:size==1?v.p[0]:x9q_u32(v.p);
}
static void x9a_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned n){
 unsigned slot=local-CBQ_PORT_BASE,sim=x9w_clients[slot].sim_slot,error=0;
 const char *why="owner-or-readiness";
 x9w_frame f(0x101,1u<<sim);
 if(x9a_op.used){
  bool identical=n==x9a_op.len&&!memcmp(req+7,x9a_op.req+7,n-7)&&sim==x9a_op.sim_slot;
  for(auto &w:x9a_op.waiters)if(w.used&&w.slot==slot&&x9a_live(w)&&x9q_u16(req+1)==x9q_u16(w.req+1)){
   if(!identical){w.reply=false;error=X9Q_ARG;} // never later ACK a changed reused txn
   else if(w.reply)return; // pending retransmit, only one real command
   else error=X9Q_NOT_READY;
   break;
  }
  if(!error){
   // 186 sends txn2/3 to the SAME SIM1 within 26ms. Join only byte-identical
   // payloads on this route/epoch. Every waiter gets the real 0102 outcome,
   // not BUSY-induced reset and not a premature duplicate-success ACK.
   if(identical&&!x9a_op.hold&&!x9a_op.timed_out&&x9q_ms()<x9a_op.deadline&&x9a_op.epoch==x9a_epoch&&x9_md_mipc_online()&&x9a_waiter(slot,req)){
    printf("X9 ATTACH: coalesced SIM%u txn %u on token %llu; await same real CNF\n",sim+1,x9q_u16(req+1),x9a_op.token);return;
   }error=X9Q_BUSY;
  }
 }
 if(!error)error=x9a_args(req,n,f,why);
 if(!error)why="owner-or-readiness";
 if(!error&&x9ds_mutating())error=X9Q_BUSY;
 if(!error)for(const x9w_bearer &b:x9w_bearers)if(b.used){error=X9Q_BUSY;break;}
 if(!error&&(sim>=x9_md_sim_count()||!x9w_control_ready||!x9_md_mipc_online()))error=X9Q_NOT_READY;
 if(!error){
  uint64_t token=f.submit();
  if(!token)error=X9Q_BUSY;
  else{
   bzero(&x9a_op,sizeof(x9a_op));x9a_op.used=true;x9a_waiter(slot,req);
   x9a_op.slot=slot;x9a_op.sim_slot=sim;x9a_op.generation=x9q_clients[slot].generation;
   x9a_op.epoch=x9a_epoch;x9a_op.token=token;x9a_op.deadline=x9q_ms()+300000;
   x9a_op.action=x9a_scalar(req,n,0x91,4);x9a_op.phase=X9A_CONFIG;
   x9a_op.len=n;memcpy(x9a_op.req,req,n);
   printf("X9 ATTACH: WDS c8 SIM%u client 0x%x txn %u action %u -> typed 0101 token %llu; await real 0102 result (payload private)\n",
    sim+1,local,x9q_u16(req+1),x9a_op.action,token);return;
  }
 }
 printf("X9 ATTACH: WDS c8 SIM%u txn %u refused error %u reason %s; primary %u roaming41 %u roaming42 %u action %u append %u mask %u/%u; no mutation (payload private)\n",
  sim+1,x9q_u16(req+1),error,why,x9a_scalar(req,n,0x11,1),x9a_scalar(req,n,0x41,4),x9a_scalar(req,n,0x42,4),
  x9a_scalar(req,n,0x91,4),x9a_scalar(req,n,0x90,1),x9a_scalar(req,n,0xdd,8),
  x9q_tlv(req,n,0xdd).n==8?x9q_u32(x9q_tlv(req,n,0xdd).p+4):~0u);
 x9bb_reply_error(local,port,req,n,(uint16_t)error);
}
static void x9a_next(){
 if(x9a_op.hold||x9a_op.token||!x9_md_mipc_online())return;
 if(x9a_op.epoch!=x9a_epoch){x9a_op.hold=true;x9a_reply(X9Q_NOT_READY);return;}
 if(x9a_op.phase==X9A_DETACH&&(x9a_op.timed_out||x9q_ms()>=x9a_op.deadline)){
  // CONFIG was confirmed, but detach has never been queued. Do not begin
  // a new disruptive phase after timeout. There is no sent owner to retain.
  x9a_reply(X9Q_NOT_READY);bzero(&x9a_op,sizeof(x9a_op));return;
 }
 // Live PLJ110 RmmDcEventHandler 2d19d8..2d1a34: PS-only 040b,
 // 0101/u8=1 and 0100/u8=(request==ATTACH). Its RFX registration at
 // 211cb4/211ce4 and dispatcher 2a7a28/2a787c prove attach1/detach0.
 // Action2 is CONFIG -> explicit detach -> explicit reattach, not RESET4.
 x9w_frame f(0x40b,1u<<x9a_op.sim_slot);
 f.byte(0x101,1);f.byte(0x100,x9a_op.phase==X9A_REATTACH?1:0);
 x9a_op.token=f.submit(); // only retry enqueue failure; NEVER retry a sent command
 if(x9a_op.token)printf("X9 ATTACH: SIM%u action2 %s -> typed 040b token %llu; await real 040c\n",
  x9a_op.sim_slot+1,x9a_op.phase==X9A_REATTACH?"reattach":"detach",x9a_op.token);
}
static void x9a_poll(uint8_t *response,unsigned room){
 if(!x9a_op.used)return;
 // Expire the host BEFORE consuming a ready CNF. A delayed poll must not
 // ACK a deadline-expired transaction just because its CNF is ready now.
 if(!x9a_op.hold&&!x9a_op.timed_out&&x9q_ms()>=x9a_op.deadline){
  x9a_op.timed_out=true;x9a_reply(X9Q_NOT_READY);
  printf("X9 ATTACH: SIM%u phase %u token %llu timeout; retain owner for late CNF, no sent-command retry/reset\n",x9a_op.sim_slot+1,x9a_op.phase,x9a_op.token);
 }
 bool online=x9_md_mipc_online();
 if(!online||x9a_op.epoch!=x9a_epoch)x9a_reply(X9Q_NOT_READY);
 unsigned n=0,result=2;
 unsigned expected=x9a_op.phase==X9A_CONFIG?0x102:0x40c;
 int ready=x9a_op.token?x9_md_mipc_take(x9a_op.token,response,room,&n):0;
 if(ready==1){
  bool good=x9w_wire(response,n)&&x9q_u16(response+10)==expected&&x9w_word(response,n,0,result);
  bzero(response,room);
  if(!good){
   x9a_reply(X9Q_INTERNAL);x9a_op.hold=true;
   printf("X9 ATTACH: malformed %04x SIM%u token %llu; outcome unknown, reserved until boot\n",expected,x9a_op.sim_slot+1,x9a_op.token);
   return;
  }
  bool same=online&&x9a_op.epoch==x9a_epoch;
  printf("X9 ATTACH: %04x SIM%u phase %u token %llu result %u same-card-epoch %u late %u; no PDP claim\n",
   expected,x9a_op.sim_slot+1,x9a_op.phase,x9a_op.token,result,same,x9a_op.timed_out);
  if(result==0&&same&&x9a_op.action==2&&x9a_op.phase!=X9A_REATTACH&&
     (x9a_op.phase==X9A_DETACH||!x9a_op.timed_out)){
   // Even after a host timeout/DEL_CLIENT, restore PS after our confirmed
   // detach, but never send a continuation across a card epoch change.
   x9a_op.phase++;x9a_op.token=0;x9a_next();return;
  }
  x9a_reply(result==0&&same?0:result==1?X9Q_BUSY:X9Q_NOT_READY);
  if(x9a_op.phase>=X9A_DETACH&&(result||!same)){
   x9a_op.hold=true;printf("X9 ATTACH: PS restore not confirmed on original card; retain barrier until boot\n");return;
  }
  bzero(&x9a_op,sizeof(x9a_op));return;
 }
 x9a_next();
}
