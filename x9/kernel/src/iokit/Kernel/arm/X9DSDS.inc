/* Default data subscription, same QMI owner as UIM/WDS; no fake readback.
 * 20H392 QMIDataSubscriptionCommandDriver: 38/18:u8 register, 4e/1:u32
 * target + 10:u32 switch type, 4f/10:u32 current, 50/1:u32 current IND,
 * 4e/1:u8 result IND. Current IND (0x1f1a01048) completes the operation;
 * a successful response alone does NOT. PLJ110 RIL 0x2cb1a0 sends MIPC
 * 020d, mask 1<<slot, no TLVs; 020e TLV0:u32 result=0 is real completion.
 * No native DDS read command has been established. On boot, explicitly
 * SELECT the host policy (x9_dds_boot_slot=1/2, default 1, 0 disables), then
 * cache only a successful CNF. This is initialization, NOT an MD query.
 * Initialization belongs to the real card-pair/MIPC lifecycle, not to a
 * future DDS client registration. 181 had a READY pair and online MIPC but
 * no DDS38/18 or 4f; waiting for those left current permanently unknown and
 * every WDS start ineligible. Two READY, distinct physical ICCIDs are required. Sent mutation tokens
 * survive client death/timeouts; never retry an operation of unknown outcome.
 */
struct x9ds_client {
 bool subscribed,query;uint8_t req[7];uint64_t deadline;
};
static x9ds_client x9ds_clients[CBQ_MAX_CLIENTS];
struct x9ds_operation {
 bool used,host,reply,timed_out,hold;unsigned slot,target;
 uint8_t req[21];uint64_t generation,epoch,token,deadline;
};
static x9ds_operation x9ds_op;
static bool x9ds_mutating(){return x9ds_op.used;}
static uint8_t x9ds_iccid[2][10];
static bool x9ds_mapped_apps;
static void x9ds_apps(bool ready){x9ds_mapped_apps=ready;}
static bool x9ds_pair,x9ds_known,x9ds_boot_attempted;
static unsigned x9ds_current;
static uint64_t x9ds_epoch;
static bool x9ds_live(){return !x9ds_op.host&&x9ds_op.slot<CBQ_MAX_CLIENTS&&x9q_clients[x9ds_op.slot].generation==x9ds_op.generation;}
static bool x9ds_data_ready(unsigned slot){
 return x9_md_sim_count()!=2||(x9ds_pair&&x9ds_mapped_apps&&x9ds_known&&!x9ds_op.used&&slot==x9ds_current);
}
static void x9ds_cards(const uint8_t *first,const uint8_t *second,bool ready){
 bool pair=ready&&first&&second&&memcmp(first,second,10)!=0;
 if(pair==x9ds_pair&&(!pair||(!memcmp(x9ds_iccid[0],first,10)&&!memcmp(x9ds_iccid[1],second,10))))return;
 x9ds_epoch++;x9a_cards_changed();x9ds_mapped_apps=false;x9ds_known=false;x9ds_pair=pair;bzero(x9ds_iccid,sizeof(x9ds_iccid));
 if(pair){memcpy(x9ds_iccid[0],first,10);memcpy(x9ds_iccid[1],second,10);}
 printf("X9 DDS: real READY card pair %u epoch %llu; current subscription invalidated\n",pair,x9ds_epoch);
}
static void x9ds_current_ind(unsigned slot){
 uint8_t req[7]={};x9q_packet p(req);uint8_t sub[4]={(uint8_t)(x9ds_current+1),0,0,0};
 p.tlv(1,sub,4);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+0x2a,true,0x50);
}
static void x9ds_result(unsigned error){
 if(!x9ds_op.reply||!x9ds_live()){x9ds_op.reply=false;return;}
 x9q_packet p(x9ds_op.req,(uint16_t)error);p.send(CBQ_PORT_BASE+x9ds_op.slot,X9BB_PORT_BASE+0x2a);
 if(!error){x9q_packet ind(x9ds_op.req);uint8_t ok=0;ind.tlv(1,&ok,1);ind.send(CBQ_PORT_BASE+x9ds_op.slot,X9BB_PORT_BASE+0x2a,true,0x4e);}
 x9ds_op.reply=false;
}
static void x9ds_del(unsigned slot){
 bzero(&x9ds_clients[slot],sizeof(x9ds_clients[slot]));
 if(x9ds_op.used&&!x9ds_op.host&&x9ds_op.slot==slot)x9ds_op.reply=false;
 // In particular, do not cancel an already queued/sent 020d.
}
static void x9ds_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned n){
 unsigned slot=local-CBQ_PORT_BASE,id=x9q_u16(req+3);x9ds_client &c=x9ds_clients[slot];
 unsigned error=0;
 if(x9_md_sim_count()!=2)error=X9Q_UNSUPPORTED;
 else if(id==0x25){
  x9q_view flag=x9q_tlv(req,n,0x11);
  if(n!=11||!flag.p||flag.n!=1||flag.p[0]>1)error=X9Q_ARG;
  else{x9q_clients[slot].dsd_system=flag.p[0]!=0;x9q_clients[slot].next=0;}
 }else if(id==0x24){
  if(n!=7)error=X9Q_ARG;
  else if(x9q_enqueue(local,port,req,n))return;
  else error=X9Q_BUSY;
 }else if(id==0x38){
  x9q_view flag=x9q_tlv(req,n,0x18);
  if(!flag.p)error=X9Q_UNSUPPORTED; // e.g. 176: a2 dynamic-RRC registration, NOT DDS18
  else if(n!=11||flag.n!=1||flag.p[0]>1)error=X9Q_ARG;
  else{c.subscribed=flag.p[0]!=0;x9q_packet p(req);p.send(local,port);
   if(c.subscribed&&x9ds_known&&x9ds_pair&&!x9ds_op.used)x9ds_current_ind(slot);return;}
 }else if(id==0x4f){
  if(n!=7)error=X9Q_ARG;
  else if(c.query)error=X9Q_BUSY;
  else{c.query=true;memcpy(c.req,req,7);c.deadline=x9q_ms()+180000;return;}
 }else if(id==0x4e){
  x9q_view sub=x9q_tlv(req,n,1),type=x9q_tlv(req,n,0x10);
  if(n!=21||!sub.p||sub.n!=4||!type.p||type.n!=4||x9q_u32(sub.p)<1||x9q_u32(sub.p)>2||x9q_u32(type.p)>1)error=X9Q_ARG;
  else if(x9ds_op.used)error=X9Q_BUSY;
 else if(!x9ds_pair||!x9ds_mapped_apps||!x9w_session_ready||!x9_md_mipc_online())error=X9Q_NOT_READY;
  else{
   bzero(&x9ds_op,sizeof(x9ds_op));x9ds_op.used=x9ds_op.reply=true;x9ds_op.slot=slot;
   x9ds_op.target=x9q_u32(sub.p)-1;x9ds_op.generation=x9q_clients[slot].generation;x9ds_op.epoch=x9ds_epoch;
   x9ds_op.deadline=x9q_ms()+60000;memcpy(x9ds_op.req,req,n);
   printf("X9 DDS: switch request SIM%u -> SIM%u type %u; drain owned WDS before 020d\n",x9ds_known?x9ds_current+1:0,x9ds_op.target+1,x9q_u32(type.p));return;
  }
 }else error=X9Q_UNSUPPORTED;
 if(error){x9q_reject(local,port,req,n,(uint16_t)error);return;}
 x9q_packet p(req);p.send(local,port);
}
static bool x9ds_drain(){
 if(x9a_busy())return false; // wait for actual attach completion; never cancel/replay it
 bool empty=true;
 for(unsigned i=0;i<CBP_INTERFACES;i++){
  x9w_bearer &b=x9w_bearers[i];if(!b.used)continue;empty=false;
  // Reuse WDS's actual ACT/DEACT ownership and late-CNF cleanup. Never
  // send a default-SIM mutation while a previous call is still owned.
  b.abandoned=true;x9w_disconnect(b,i+1);x9w_error(b,X9Q_NOT_READY);
 }
 // AP final TX accounting is an additional prerequisite only, not a proof
 // that the MD user slot was released or RX/channel reuse is ordered.
 return empty&&!x9w_accounting_pending();
}
static void x9ds_expire(uint64_t now){
 if(!x9ds_op.used||x9ds_op.timed_out||now<x9ds_op.deadline)return;
 // Retire the original host reply before take or submission. A late valid
 // 020e may still record actual current DDS, but never revive this reply.
 x9ds_op.timed_out=true;x9ds_result(X9Q_NOT_READY);
 if(!x9ds_op.token&&!x9ds_op.hold)bzero(&x9ds_op,sizeof(x9ds_op));
 else printf("X9 DDS: timeout; retain mutation owner, no repeated 020d\n");
}
static void x9ds_poll(){
 uint64_t now=x9q_ms();bool online=x9_md_mipc_online();
 if(!online)x9ds_known=false;
 if(!x9ds_op.used&&!x9ds_boot_attempted&&x9ds_pair&&x9w_session_ready&&online){
  int initial=1;PE_parse_boot_argn("x9_dds_boot_slot",&initial,sizeof(initial));x9ds_boot_attempted=true;
  if(initial==1||initial==2){
   bzero(&x9ds_op,sizeof(x9ds_op));x9ds_op.used=x9ds_op.host=true;x9ds_op.target=(unsigned)initial-1;
   x9ds_op.epoch=x9ds_epoch;x9ds_op.deadline=now+60000;
   printf("X9 DDS: boot policy explicitly selects SIM%d via MD 020d (not native readback)\n",initial);
  }else printf("X9 DDS: boot selection disabled; current unknown until an explicit switch succeeds\n");
 }
 x9ds_expire(x9q_ms()); // ready1 first observed after deadline is still late
 if(x9ds_op.used){
  if(x9ds_op.token){
   uint8_t response[X9_MIPC_FRAME_MAX];unsigned n=0,result=2;
   x9ds_expire(x9q_ms()); // latch before consuming the original token
   int done=x9_md_mipc_take(x9ds_op.token,response,sizeof(response),&n);
   if(done==1){
    bool wire=x9w_wire(response,n)&&x9q_u16(response+10)==0x20e&&x9w_word(response,n,0,result);
    bzero(response,sizeof(response));
    if(!wire){x9ds_known=false;x9ds_op.hold=true;x9ds_result(X9Q_INTERNAL);
     printf("X9 DDS: malformed 020e; outcome unknown, no retry until boot\n");}
    else{
     bool same=x9ds_pair&&x9ds_op.epoch==x9ds_epoch&&online;
     if(result==0&&same){
      x9ds_current=x9ds_op.target;x9ds_known=true;x9ds_result(0);
      for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)if(x9ds_clients[i].subscribed||(x9ds_live()&&i==x9ds_op.slot))x9ds_current_ind(i);
     }else x9ds_result(result==0?X9Q_NOT_READY:X9Q_INTERNAL);
     printf("X9 DDS: 020e result %u target SIM%u same-card-epoch %u current-known %u late %u\n",result,x9ds_op.target+1,same,x9ds_known,x9ds_op.timed_out);
     bzero(&x9ds_op,sizeof(x9ds_op));
    }
   }
  }else if(!x9ds_op.hold){
   if(!x9ds_pair||x9ds_epoch!=x9ds_op.epoch||!online||(!x9ds_op.host&&!x9ds_live())){
    x9ds_result(X9Q_NOT_READY);bzero(&x9ds_op,sizeof(x9ds_op));
   }else if(x9ds_drain()){
    x9ds_expire(x9q_ms()); // AP drain cannot extend an already expired wait
    if(x9ds_op.used&&!x9ds_op.timed_out){
     x9w_frame f(0x20d,1u<<x9ds_op.target);x9ds_op.token=f.submit();
     if(x9ds_op.token){x9ds_op.deadline=x9q_ms()+60000;x9ds_known=false;x9ds_boot_attempted=true;
      printf("X9 DDS: 020d queued mask %u; await transaction-matched 020e\n",1u<<x9ds_op.target);}
    }
   }
  }
 }
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){
  x9ds_client &c=x9ds_clients[i];if(!c.query)continue;
  bool ready=x9ds_known&&x9ds_pair&&!x9ds_op.used;
  if(!ready&&now<c.deadline)continue;
  x9q_packet p(c.req,ready?0:X9Q_NOT_READY);
  if(ready){uint8_t sub[4]={(uint8_t)(x9ds_current+1),0,0,0};p.tlv(0x10,sub,4);}
  p.send(CBQ_PORT_BASE+i,X9BB_PORT_BASE+0x2a);c.query=false;
 }
}

/* 20H392 1f19febb8 builds UIM6e: 10 primary/11 secondary, each independently
 * optional, u8 physical slot. A zero HardwareSimSlot omits that TLV; omission
 * alone is not proof of a supported dynamic map. 182 sends 11-byte requests six times, while
 * the old mandatory-two-TLV parser returned ARG every time. The native
 * 1f1a01938 callback passes that failure into configureSubscription.
 * Current UIM CardStatus already maps primary USIM -> slot1, secondary USIM
 * -> slot2 from real EF_DIR. The evaluator's +1f8 dynamic mode instead asks
 * for primary->slot2 in 183; CCGATE state5 selects its fixed-slot branch.
 * Accept only supplied entries of the verified existing map;
 * no APDU, physical switching, arbitrary remap or fake provisioning success. */
static void x9ds_map(uint32_t local,uint32_t port,const uint8_t *req,unsigned n){
 x9q_view primary=x9q_tlv(req,n,0x10),secondary=x9q_tlv(req,n,0x11);
 unsigned error=0;
 if((!primary.p&&!secondary.p)||(primary.p&&primary.n!=1)||(secondary.p&&secondary.n!=1)||
    n!=7u+(primary.p?4u:0u)+(secondary.p?4u:0u))error=X9Q_ARG;
 else if((primary.p&&primary.p[0]!=1)||(secondary.p&&secondary.p[0]!=2))error=X9Q_UNSUPPORTED;
 else if(x9_md_sim_count()!=2||!x9ds_pair||!x9ds_mapped_apps)error=X9Q_NOT_READY;
 x9q_packet p(req,(uint16_t)error);p.send(local,port);
 printf("X9 DDS: UIM6e bytes %u primary %d secondary %d pair %u apps %u existing-map result %u (omitted=-1, no SIM mutation)\n",
  n,primary.p&&primary.n==1?(int)primary.p[0]:-1,secondary.p&&secondary.n==1?(int)secondary.p[0]:-1,
  x9ds_pair,x9ds_mapped_apps,error);
}
