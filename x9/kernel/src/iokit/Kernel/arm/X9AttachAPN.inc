/* 20H392 Mav20 WDS SetAttachProfileInfo3GPP (00c8), not WDS Start.
 * PLJ110 RmmDcEventHandler::requestSetInitialAttachApn (2c9cc4..2c9d88):
 * APN_SET_IA_REQ 0101 -> APN_SET_IA_CNF 0102. Unlike that RIL's unconditional
 * RIL_Errno0, require a unique result0/u32 == 0 on a complete matched CNF.
 * Internet/no-auth/replace only. No IMS, profile-index/PCO conversion, AT
 * writes, borrowed bearer, attach-status fiction or automatic dial/reset.
 * Serialize mutations against DDS and every owned WDS bearer. Route/host
 * generation/card epoch are snapshotted; queued/sent owners survive timeout,
 * DEL_CLIENT and card changes. APN is bounded RAM-only and never logged.
 */
static bool x9ds_mutating();
struct x9a_operation {
 bool used,hold,timed_out;
 unsigned slot,sim_slot,len;
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
static unsigned x9a_args(const uint8_t *req,unsigned n,x9w_frame &f){
 x9q_view home=x9q_tlv(req,n,0x11),apn=x9q_tlv(req,n,0x14),auth=x9q_tlv(req,n,0x1f);
 x9q_view append=x9q_tlv(req,n,0x90),action=x9q_tlv(req,n,0x91),types=x9q_tlv(req,n,0xdd);
 if(!home.p||home.n!=1||!apn.p||!auth.p||auth.n!=1||!append.p||append.n!=1||
    !action.p||action.n!=4||!types.p||types.n!=8||!x9w_apn(apn.p,apn.n))return X9Q_ARG;
 // These are distinct QMI enums! Primary 0/2/3 -> MIPC IP1/IPv6 2/dual3;
 // LTE/UMTS roaming 0/1/2 -> MIPC 1/2/3 (AllowedProtocolMask minus one).
 unsigned protocol=home.p[0]==0?1:home.p[0]==2?2:home.p[0]==3?3:0;
 if(!protocol||auth.p[0]||append.p[0]||x9q_u32(action.p)||
    x9q_u32(types.p)!=1||x9q_u32(types.p+4))return X9Q_UNSUPPORTED;
 unsigned roaming=protocol,explicit_roaming=0;
 for(unsigned at=7;at<n;at+=3+x9q_u16(req+at+1)){
  unsigned tag=req[at],k=x9q_u16(req+at+1);const uint8_t *v=req+at+3;
  switch(tag){
  case 0x11:case 0x14:case 0x1f:case 0x90:case 0x91:case 0xdd:break;
  case 0x41:case 0x42:{
   if(k!=4)return X9Q_ARG;
   unsigned value=x9q_u32(v);if(value>2)return X9Q_UNSUPPORTED;
   // MIPC has ONE roaming setting. No independently supplied UMTS/LTE
   // policy is discarded: if both are supplied they must agree. Omitted
   // overrides supply no separate policy; absent both use the home type.
   if(explicit_roaming&&roaming!=value+1)return X9Q_UNSUPPORTED;
   roaming=value+1;explicit_roaming++;break;
  }
  case 0x1b:case 0x1c:if(k)return X9Q_UNSUPPORTED;break; // no-auth, empty credentials only
  default:return X9Q_UNSUPPORTED; // includes IMS/PCO/compression/profile overrides
  }
 }
 char name[101]={};if(apn.n)memcpy(name,apn.p,apn.n);
 f.tlv(0x101,name,apn.n+1);bzero(name,sizeof(name));
 f.byte(0x102,(uint8_t)protocol);f.byte(0x103,(uint8_t)roaming);f.byte(0x104,0);
 static const char empty[1]={};f.tlv(0x8105,empty,1);f.tlv(0x8106,empty,1);
 return f.good?0:X9Q_ARG;
}
static void x9a_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned n){
 unsigned slot=local-CBQ_PORT_BASE,sim=x9w_clients[slot].sim_slot,error=0;
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
   if(identical&&!x9a_op.hold&&!x9a_op.timed_out&&x9a_op.epoch==x9a_epoch&&x9_md_mipc_online()&&x9a_waiter(slot,req)){
    printf("X9 ATTACH: coalesced SIM%u txn %u on token %llu; await same real CNF\n",sim+1,x9q_u16(req+1),x9a_op.token);return;
   }error=X9Q_BUSY;
  }
 }
 if(!error)error=x9a_args(req,n,f);
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
   x9a_op.len=n;memcpy(x9a_op.req,req,n);
   printf("X9 ATTACH: WDS c8 SIM%u client 0x%x txn %u -> typed 0101 token %llu; await real 0102 result (payload private)\n",
    sim+1,local,x9q_u16(req+1),token);return;
  }
 }
 printf("X9 ATTACH: WDS c8 SIM%u txn %u refused error %u; no mutation (payload private)\n",sim+1,x9q_u16(req+1),error);
 x9bb_reply_error(local,port,req,n,(uint16_t)error);
}
static void x9a_poll(uint8_t *response,unsigned room){
 if(!x9a_op.used)return;
 bool online=x9_md_mipc_online();
 if(!online||x9a_op.epoch!=x9a_epoch)x9a_reply(X9Q_NOT_READY);
 unsigned n=0,result=2;
 int ready=x9_md_mipc_take(x9a_op.token,response,room,&n);
 if(ready==1){
  bool good=x9w_wire(response,n)&&x9q_u16(response+10)==0x102&&x9w_word(response,n,0,result);
  bzero(response,room);
  if(!good){
   x9a_reply(X9Q_INTERNAL);x9a_op.hold=true;
   printf("X9 ATTACH: malformed 0102 SIM%u token %llu; outcome unknown, reserved until boot\n",x9a_op.sim_slot+1,x9a_op.token);
   return;
  }
  bool same=online&&x9a_op.epoch==x9a_epoch;
  x9a_reply(result==0&&same?0:result==1?X9Q_BUSY:X9Q_NOT_READY);
  printf("X9 ATTACH: 0102 SIM%u token %llu result %u same-card-epoch %u late %u; attach only, no PDP claim\n",
   x9a_op.sim_slot+1,x9a_op.token,result,same,x9a_op.timed_out);
  bzero(&x9a_op,sizeof(x9a_op));return;
 }
 if(!x9a_op.hold&&!x9a_op.timed_out&&x9q_ms()>=x9a_op.deadline){
  x9a_op.timed_out=true;x9a_reply(X9Q_NOT_READY);
  printf("X9 ATTACH: 0101 SIM%u token %llu timeout; retain owner for late CNF, no retry/reset\n",x9a_op.sim_slot+1,x9a_op.token);
 }
}
