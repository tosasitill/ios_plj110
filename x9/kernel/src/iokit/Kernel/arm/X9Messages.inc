/* Unmodified iOS 20H392 QMISMSCommandDriver -> native MTK MIPC.
 * Apple wire contracts, checked in the actual driver: WMS47 indication
 * registration (13=service-ready), 4c BindSubscription (01/u8, 0/1),
 * 5c ready response (11/u8: 1=3GPP, NOT 2), 5d ready IND (10/u8),
 * RawSend20/SendAck37, ListMessages31/RawRead22/ModifyTag23 (MT read only).
 * Standard WMS transfer-route TLVs are byte encoded. RawRead preserves tags;
 * ModifyTag pre-reads, marks one MT message read, then verifies by readback.
 * List31: storage01, RequestedTag10, Message_Mode11 (not tag11/tag12).
 * MTK: GSM send605/606, SMS host-ready601/602, CMD8601/8603 with
 * ORIGINAL txid ACK8602/8604. Independent from the PDP/DPMAIF state.
 * IMSA exposes only proven registration a04/a05 and 4a01 snapshots.
 * No guessed IMS service capabilities/configuration or CS fallback for an
 * IMS-only send. SIP, non-CS type4/8105 and VoLTE are NOT implemented here.
 * MD maps type4 to C2K; the RIL event consumer is not yet reconciled.
 */
#include "../../../bsd/dev/x9_sms_storage.h"
constexpr unsigned X9M_JOBS=32, X9M_INCOMING=16;
enum { X9M_SEND=1, X9M_ACK, X9M_REG, X9M_SMSC, X9M_LIST, X9M_READ, X9M_TAG, X9M_DIAG };
struct x9m_client {
 bool events,mwi_events,ready_ind,ims_ind,routes,status_reports;
 uint8_t route[6],route_mask; uint64_t reg_next;
};
static x9m_client x9m_clients[CBQ_MAX_CLIENTS];
struct x9m_job {
 unsigned op,slot,sim,len; uint64_t generation,token,deadline;
 bool answered,background,hold,follow; uint8_t req[X9Q_REQ];
 unsigned step,stored_len; uint64_t store_order,reg_epoch; uint8_t stored_pdu[200];
 bool ack_done,ack_reply_pending;uint16_t ack_error;uint64_t bridge_generation;
 bool receipt_pending; unsigned receipt_len; uint8_t receipt[1536];
};
static x9m_job x9m_jobs[X9M_JOBS];
static uint64_t x9m_store_order;
static bool x9m_storage(unsigned op){return op==X9M_LIST||op==X9M_READ||op==X9M_TAG;}
static bool x9m_has_receipt(unsigned op){return op==X9M_SEND||x9m_storage(op);}
/* A pending RawSend may have already sent 0605. Retransmissions, including
 * malformed reuse of the same transaction, cannot send again or race its
 * terminal result with BUSY/ARG. New transactions still validate normally. */
static bool x9m_send_repeated_wire(uint32_t local,uint32_t node,uint32_t port,const uint8_t *req,unsigned len){
 const unsigned slot=local-CBQ_PORT_BASE;
 if(node!=X9BB_NODE||local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS||port!=X9BB_PORT_BASE+5||
  !req||len<7||req[0]!=0||x9q_u16(req+3)!=0x20)return false;
 for(const x9m_job &j:x9m_jobs)if(j.op==X9M_SEND&&j.slot==slot&&
  j.generation==x9q_clients[slot].generation&&x9q_u16(j.req+1)==x9q_u16(req+1))return true;
 return false;
}
/* An active WMS storage transaction owns its QMI transaction number until
 * the native work is retired. A repeated or changed request cannot enqueue
 * another 0607 or produce a competing terminal response. */
static bool x9m_storage_repeated(unsigned slot,uint64_t generation,const uint8_t *req,unsigned len){
 if(!req||len<7)return false;
 for(const x9m_job &j:x9m_jobs)if(x9m_storage(j.op)&&j.slot==slot&&
  j.generation==generation&&x9q_u16(j.req+1)==x9q_u16(req+1)){
  if(j.len!=len||memcmp(j.req,req,len))
   printf("X9 SMS: storage txn %u changed reuse rejected; original owner retained\n",x9q_u16(req+1));
  return true;
 }
 return false;
}
/* Check an existing owner before generic TLV rejection, as for WMS37.
 * A malformed retransmission must not race the original terminal result. */
static bool x9m_storage_repeated_wire(uint32_t local,uint32_t node,uint32_t port,const uint8_t *req,unsigned len){
 unsigned slot=local-CBQ_PORT_BASE;
 if(node!=X9BB_NODE||local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS||port!=X9BB_PORT_BASE+5||
  !req||len<7||req[0]!=0)return false;
 unsigned id=x9q_u16(req+3);
 if(id!=0x31&&id!=0x22&&id!=0x23)return false;
 return x9m_storage_repeated(slot,x9q_clients[slot].generation,req,len);
}
struct x9m_incoming {
 uint32_t token; unsigned sim,len,slot; uint64_t generation,next_delivery;
 bool delivered,replying,held; uint64_t ack_wait; uint8_t pdu[255];
};
static x9m_incoming x9m_incoming_sms[X9M_INCOMING];
enum { X9M_CFG_IND=0, X9M_READY_QUERY, X9M_WAIT_READY, X9M_HOST_READY, X9M_FACTORY_CFG, X9M_INIT_DONE };
struct x9m_sim {
 bool wanted,ready,reg_valid,md_known,md_ready,init_hold,init_timed_out;
 unsigned reg,init,diag_stage,ims_stage; uint64_t sampled,init_token,init_deadline,ready_epoch,init_epoch;
 uint64_t reg_epoch,ims_epoch,ims_token,ims_deadline,ims_admit_deadline; bool ims_hold;
 unsigned fx_stage; uint64_t fx_token,fx_deadline; bool fx_hold;
 bool mem_sent,mem_hold; uint64_t mem_token,mem_deadline;
};
static x9m_sim x9m_sims[2];
static unsigned x9m_init_stage;
static uint64_t x9m_init_token,x9m_init_deadline;
static bool x9m_init_hold;
static bool x9m_live(unsigned slot,uint64_t generation){
 return slot<CBQ_MAX_CLIENTS&&x9q_clients[slot].generation==generation;
}
static bool x9m_ack_repeated(uint32_t local,uint32_t node,uint32_t port,const uint8_t *req,unsigned len){
 const unsigned slot=local-CBQ_PORT_BASE;
 if(node!=X9BB_NODE||local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS||port!=X9BB_PORT_BASE+5||
  !req||len<7||req[0]!=0||x9q_u16(req+3)!=0x37)return false;
 // A submitted ACK owns this QMI transaction until its physical result
 // and actual host response admission retire it. Even a malformed reuse
 // must not get a second BAD/ARG before the original terminal response.
 for(const x9m_job &j:x9m_jobs)if(j.op==X9M_ACK&&j.slot==slot&&
  j.generation==x9q_clients[slot].generation&&x9q_u16(j.req+1)==x9q_u16(req+1)){
  if(j.len!=len||memcmp(j.req,req,len))
   printf("X9 SMS: SIM%u WMS37 txn %u changed reuse rejected; original ACK owner/result retained\n",j.sim+1,x9q_u16(req+1));
  return true; // same-size comparison is bounded by the original validated request
 }
 return false;
}
static void x9m_number(x9q_packet &p,uint8_t tag,uint32_t value,unsigned bytes=4){
 uint8_t v[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
 p.tlv(tag,v,bytes);
}
static void x9m_reply(unsigned slot,unsigned service,const uint8_t *req,uint16_t err){
 x9q_packet p(req,err);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+service);
}
static unsigned x9m_service(const x9m_job &j){return j.op==X9M_REG?0x21:5;}
static bool x9m_ack_response(const x9m_job &j){
 x9q_packet p(j.req,j.ack_error);p.p[5]=(uint8_t)(p.n-7);p.p[6]=(uint8_t)((p.n-7)>>8);
 const uint32_t room=CBQ_HDR+p.n;
 uint8_t *owned=p.good?static_cast<uint8_t *>(IOMallocData(room)):nullptr;
 bool admitted=owned&&bb_x9_sms_ack_result(CBQ_PORT_BASE+j.slot,X9BB_NODE,j.bridge_generation,
  p.p,p.n,owned,room);
 if(!admitted&&owned){bzero(owned,room);IOFreeData(owned,room);}
 bzero(p.p,sizeof(p.p));return admitted;
}
/* First terminal SDU is immutable until admitted or original client loss. */
static void x9m_receipt_retry(x9m_job &j){
 if(!j.receipt_pending)return;
 bool retired=!x9m_live(j.slot,j.generation);
 if(!retired){
  const unsigned room=CBQ_HDR+j.receipt_len;
  uint8_t *owned=static_cast<uint8_t *>(IOMallocData(room));
  const unsigned port=CBQ_PORT_BASE+j.slot;
  const bool admitted=owned&&bb_x9_sms_terminal_result(port,X9BB_NODE,j.bridge_generation,
    j.receipt,j.receipt_len,owned,room);
  if(!admitted&&owned){bzero(owned,room);IOFreeData(owned,room);}
  if(admitted){
   if(j.op==X9M_SEND)
    printf("X9 SMS: SIM%u RawSend txn %u terminal host queue receipt; not delivery\n",j.sim+1,x9q_u16(j.req+1));
   retired=true;
  }
  else{
   // Allocation changed before a local DEL was observed. A new positive
   // identity is decisive; zero can also mean temporarily unavailable.
   const uint64_t current=bb_x9_sms_ack_identity(port,X9BB_NODE);
   retired=current&&current!=j.bridge_generation;
  }
 }
 if(retired){j.receipt_pending=false;bzero(j.receipt,sizeof(j.receipt));j.receipt_len=0;}
}
static void x9m_terminal(x9m_job &j,x9q_packet &p){
 if(j.answered)return;
 j.answered=true;
 if(!x9m_live(j.slot,j.generation)||!p.good)return;
 p.p[5]=(uint8_t)(p.n-7);p.p[6]=(uint8_t)((p.n-7)>>8);
 memcpy(j.receipt,p.p,p.n);j.receipt_len=p.n;j.receipt_pending=true;
 x9m_receipt_retry(j);
}
static void x9m_error(x9m_job &j,uint16_t err){
 if(j.op==X9M_DIAG){
  j.answered=true;if(j.sim<2)x9m_sims[j.sim].diag_stage=2;return;
 }
 if(x9m_has_receipt(j.op)){
  x9q_packet p(j.req,err);x9m_terminal(j,p);bzero(p.p,sizeof(p.p));return;
 }
 if(j.op==X9M_ACK){
  if(j.answered)return;
  if(!x9m_live(j.slot,j.generation)){j.answered=true;return;}
  // First terminal result wins, including a timeout while physical TX is
  // still pending. A queue retry cannot convert it to a late success.
  if(!j.ack_reply_pending){j.ack_reply_pending=true;j.ack_error=err;}
  if(x9m_ack_response(j)){
   j.answered=true;j.ack_reply_pending=false;
   printf("X9 SMS: SIM%u command token %u WMS37 txn %u result %u host queue receipt; not durable storage proof\n",
    j.sim+1,(unsigned)j.token,x9q_u16(j.req+1),j.ack_error);
  }
  return;
 }
 if(!j.answered&&!j.background&&x9m_live(j.slot,j.generation))x9m_reply(j.slot,x9m_service(j),j.req,err);
 j.answered=true;
}
static void x9m_ready_ind(unsigned sim){
 uint8_t req[7]={};
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)if(x9m_clients[i].ready_ind&&x9q_clients[i].sim_slot==sim){
  x9q_packet p(req);x9m_number(p,0x10,x9m_sims[sim].ready?1:0,1);
  p.send(CBQ_PORT_BASE+i,X9BB_PORT_BASE+5,true,0x5d);
 }
}
static void x9m_md_ready(unsigned sim,bool ready){
 x9m_sim &s=x9m_sims[sim];
 if(!s.md_known||s.md_ready!=ready){
  s.ready_epoch++;s.md_known=true;s.md_ready=ready;
  printf("X9 SMS: SIM%u modem SMS readiness %u; not host-ready yet\n",sim+1,ready?1:0);
 }
 if(!ready){
  if(s.ready){s.ready=false;x9m_ready_ind(sim);}
  /* Do not re-use a configured session across a modem/SIM readiness loss.
   * In-flight send/ACK tokens still own their late results. */
  if(s.init>=X9M_HOST_READY)s.init_hold=true;
 }
}
static void x9m_reg(unsigned sim,unsigned reg){
 if(sim>=x9_md_sim_count()||reg>2)return;
 x9m_sim &s=x9m_sims[sim];bool changed=!s.reg_valid||s.reg!=reg;
 s.reg_valid=true;s.reg=reg;s.sampled=x9q_ms();
 if(!changed)return;
 uint8_t req[7]={};
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)if(x9m_clients[i].ims_ind&&x9q_clients[i].sim_slot==sim){
  x9q_packet p(req);x9m_number(p,0x11,reg);
  p.send(CBQ_PORT_BASE+i,X9BB_PORT_BASE+0x21,true,0x23);
 }
 printf("X9 IMS: SIM%u real registration snapshot %u; service capabilities unknown\n",sim+1,reg);
}
static x9m_job *x9m_new(unsigned op,unsigned slot,unsigned sim,const uint8_t *req,unsigned len,bool background=false){
 for(x9m_job &j:x9m_jobs)if(!j.op){
  bzero(&j,sizeof(j));j.op=op;j.slot=slot;j.sim=sim;j.len=len;j.background=background;
  j.generation=x9q_clients[slot].generation;memcpy(j.req,req,len);j.deadline=x9q_ms()+60000;
  if(op==X9M_ACK||x9m_has_receipt(op))j.bridge_generation=bb_x9_sms_ack_identity(CBQ_PORT_BASE+slot,X9BB_NODE);
  return &j;
 }
 return nullptr;
}
static bool x9m_pdu(const uint8_t *p,unsigned n,unsigned mti){
 return p&&n>=3&&n<=255&&p[0]<=12&&p[0]+2u<n&&(p[p[0]+1]&3)==mti;
}
/* MD 91367a2a: 0608/103 is the count, 8104 is count * 264 bytes.
 * Each record is native index/u16, TPDU length/u8, AT storage tag/u8,
 * class/u8, reserved[3], SMSC+TPDU. Never publish a partial/truncated list.
 * AT tags 0/1/2/3 = unread/read/unsent/sent; WMS = read/unread/sent/unsent.
 * Indices are opaque native storage IDs, not assumed zero-based offsets. */
static bool x9m_stored(const uint8_t *p,unsigned n,x9q_view &records,unsigned &count){
 x9q_view c=x9w_tlv(p,n,0x103);records=x9w_tlv(p,n,0x8104);
 if(!c.p||c.n!=2)return false;
 count=x9q_u16(c.p);
 if(!count)return !records.p||records.n==0;
 if(!records.p||count>X9_MIPC_FRAME_MAX/264||records.n!=count*264)return false;
 for(unsigned i=0;i<count;i++){
  const uint8_t *r=records.p+264*i;unsigned index=x9q_u16(r);
  if(!index||r[3]>3||r[8]>12)return false;
  unsigned full=1u+r[8]+r[2],mti=r[9+r[8]]&3;
  if(full>200||!x9m_pdu(r+8,full,mti)||(r[3]<2?(mti!=0&&mti!=2):mti!=1))return false;
  for(unsigned prev=0;prev<i;prev++)if(x9q_u16(records.p+264*prev)==index)return false;
 }
 return true;
}
/* One native 0607 transaction per SIM, including the entire tag mutation.
 * Apple fans out RawRead for a list: accept bounded queued work instead of
 * returning BUSY for every message after the first. Never replay a sent token.
 * An earlier completed job retaining only its host receipt no longer owns
 * native storage; queue backpressure must not stall later native requests. */
static bool x9m_store_blocked(const x9m_job &j){
 for(const x9m_job &other:x9m_jobs)if(&other!=&j&&other.sim==j.sim&&
   ((other.op==X9M_DIAG&&other.token)||(x9m_storage(other.op)&&
    (other.hold||other.token||(other.follow&&other.store_order<j.store_order)))))return true;
 return false;
}
static uint64_t x9m_store_submit(const x9m_job &j){
 x9q_view storage=x9q_tlv(j.req,j.len,1);unsigned flag=5;
 if(j.op==X9M_LIST){x9q_view tag=x9q_tlv(j.req,j.len,0x10);
  const uint8_t flags[4]={2,1,3,4};flag=tag.p?flags[tag.p[0]]:0;}
 x9w_frame f(0x607,1u<<j.sim);f.byte(0x100,3);f.byte(0x101,(uint8_t)flag);
 if(j.op!=X9M_LIST)f.tlv(0x102,storage.p+1,2);
 /* 103=0 is permitted ONLY for a single indexed CMGR in ModifyTag step1;
  * never mark an entire CMGL enumeration read. CPMS is always explicit. */
 f.byte(0x103,j.op==X9M_TAG&&j.step==1?0:1);f.byte(0x104,storage.p[0]);
 /* A CMGL failure conversion can produce 0x210000 from zero, but its
  * provenance in a particular 0608 is unproved; it is not an empty store.
  * Log request/result shape only;
  * never guess an empty list from a nonzero result or expose PDU/number bytes. */
 static unsigned diag_submits;
 if(diag_submits<8){
  diag_submits++;
  struct x9ms_storage_request sr={};unsigned sok=x9ms_storage_decode(j.req,j.len,&sr);
  printf("X9 SMS DIAG: 0607 SIM%u op %u step %u dec %u storage %u tag %u/%u idx %u flag %u 103=%u\n",
   j.sim+1,j.op,j.step,sok,sok==X9MS_OK?sr.storage:0xff,sok==X9MS_OK?sr.has_tag:0xff,
   sok==X9MS_OK?sr.tag:0xff,sok==X9MS_OK?sr.index:0xffffffffu,flag,j.op==X9M_TAG&&j.step==1?0:1);
 }
 if(j.op==X9M_TAG&&j.step==1){
  /* Serialize exactly the same frame, but carry this mutation's original
   * owner and absolute host guard to the native allocator/unique writer. */
  f.p[14]=(uint8_t)(f.n-16);f.p[15]=(uint8_t)((f.n-16)>>8);
  return f.good?x9_md_mipc_message_mark_read_submit(f.p,f.n,j.slot,j.generation,j.deadline):0;
 }
 return f.submit();
}
static bool x9m_factory_enabled(){
 /* Default on since 2026-11-27 (owner decision): the factory provisioning
  * is part of normal init; x9_factory_init=0 opts out for A/B. */
 static int enabled=-1;if(enabled<0)enabled=sweet_x9_arg("x9_factory_init",1)!=0;
 return enabled!=0;
}

/* Factory SMS memory-status report (0601 tag 0x108; the MD routes it to
 * AT+EMEMS=1). Android's RIL sends it from handleReportSmsMemStatus when
 * the HOST message store becomes available, as its own transaction -- it
 * is not a step of the config sequence (radio-baseline.txt: the host-ready
 * bundle is :1055, the memory-status report is the separate :2341 frame,
 * after whose success the same SIM later receives 8601). The bridge
 * equivalent of "host store available" is the host-ready declaration
 * itself: config confirmed, modem ready, GSM handlers installed. The
 * modem CFG IND (md_ready) is the opposite direction and precedes host
 * config, so it cannot be the trigger. The init sequence also carries
 * the report as X9M_FACTORY_CFG; mem_sent dedupes the two sites so each
 * SIM reports exactly once per boot. A failure here never blocks or
 * revokes host-ready, and is never retried. */
static void x9m_mem_status_poll(uint8_t *p,unsigned room){
 if(!x9m_factory_enabled())return;
 const bool online=x9w_control_ready&&x9_md_mipc_online();
 const uint64_t now=x9q_ms();
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  x9m_sim &s=x9m_sims[sim];
  if(s.mem_sent||s.mem_hold)continue;
  if(s.mem_token){
   if(now>=s.mem_deadline){s.mem_hold=true;
    printf("X9 SMS: SIM%u memory status report timeout; token retained, no retry\n",sim+1);continue;}
   unsigned n=0;int rc=x9_md_mipc_take(s.mem_token,p,room,&n);
   if(!rc)continue;
   if(rc<0){s.mem_hold=true;
    printf("X9 SMS: SIM%u memory status report transport error; held\n",sim+1);continue;}
   s.mem_token=0;
   unsigned result=1;
   bool known=x9w_wire(p,n)&&p[8]==(1u<<sim)&&x9q_u16(p+10)==0x602&&x9w_word(p,n,0,result);
   if(!known||result){s.mem_hold=true;
    printf("X9 SMS: SIM%u memory status report rejected known %u result %u; held\n",sim+1,known?1:0,result);}
   else{s.mem_sent=true;
    printf("X9 SMS: SIM%u memory status reported at host-ready (factory EMEMS point)\n",sim+1);}
   bzero(p,room);continue;
  }
  if(!online||!s.ready)continue;
  x9w_frame f(0x601,1u<<sim);f.byte(0x108,1);
  uint64_t token=f.submit();
  if(token){s.mem_token=token;s.mem_deadline=now+10000;}
  else s.mem_hold=true;
 }
}
static void x9m_init_transport_loss(){
  /* Loss is not registration/config retirement. Stop advancing this boot,
   * but still drain any complete results belonging to the original token. */
  if(x9m_init_stage||x9m_init_token)x9m_init_hold=true;
  for(unsigned sim=0;sim<x9_md_sim_count();sim++){
   x9m_sim &s=x9m_sims[sim];if(s.init||s.init_token)s.init_hold=true;
   if(s.ready){s.ready=false;x9m_ready_ind(sim);}
  }
}
static void x9m_init_poll(uint8_t *response,unsigned room){
 bool wanted=false;for(unsigned sim=0;sim<x9_md_sim_count();sim++)wanted|=x9m_sims[sim].wanted;
 const bool online=x9w_control_ready&&x9_md_mipc_message_online();const uint64_t now=x9q_ms();
 if(!online)x9m_init_transport_loss();
 if(x9m_init_token){
  /* Check BEFORE take: a CNF first polled at/after the guard is still late.
   * A previous timeout is terminal, never cleared by a successful CNF. */
  if(!x9m_init_hold&&now>=x9m_init_deadline){x9m_init_hold=true;
   printf("X9 SMS: init stage %u timeout; token retained, no retry\n",x9m_init_stage);}
  unsigned n=0;int rc=x9_md_mipc_take(x9m_init_token,response,room,&n);
  if(rc==1){
   x9m_init_token=0;unsigned result=1;
   if(!x9w_control_ready||!x9_md_mipc_message_online())x9m_init_hold=true;
   bool known=x9w_wire(response,n)&&x9q_u16(response+10)==0x30c&&x9w_word(response,n,0,result);
   bzero(response,room);
   if(!known||result){x9m_init_hold=true;
    printf("X9 SMS: CMD registration stage %u rejected known %u result %u\n",x9m_init_stage,known?1:0,result);}
  }else if(rc==-3){x9m_init_token=0;x9m_init_hold=true; /* proven pre-TX rejection */
   printf("X9 SMS: CMD registration stage %u locally rejected before TX; no retry\n",x9m_init_stage);
  }else if(rc<0)x9m_init_hold=true; /* uncertain physical token remains */
 }
 if(online&&wanted&&!x9m_init_hold&&!x9m_init_token&&x9m_init_stage<2){
  unsigned stage=x9m_init_stage+1,id=stage==1?0x8601:0x8603;
  x9w_frame f(0x30b,0xff);
  uint8_t v[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,v,2);
  uint64_t token=f.submit();
  if(token){x9m_init_stage=stage;x9m_init_token=token;x9m_init_deadline=x9q_ms()+300000;}
  return;
 }
 /* Android: register SMS_CFG_IND, wait for 102=1, THEN notifySmsReady.
  * One read-only CFG query covers a ready IND sent before registration.
  * Each SIM owns its progress: a locked/absent SIM cannot block the other.
  * No retry of a timed-out config and no timer-generated ready state. */
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  x9m_sim &s=x9m_sims[sim];
  if(x9m_init_hold&&(s.init||s.init_token))s.init_hold=true;
  if(s.init_token){
   if(!s.init_timed_out&&now>=s.init_deadline){s.init_timed_out=true;s.init_hold=true;
    printf("X9 SMS: SIM%u init %u timeout; token retained, no retry\n",sim+1,s.init);}
   unsigned n=0;int rc=x9_md_mipc_take(s.init_token,response,room,&n);
   if(rc!=1){if(rc<0)s.init_hold=true;
    if(rc==-3){s.init_token=0;printf("X9 SMS: SIM%u init %u locally rejected before TX; no retry\n",sim+1,s.init);}
    continue;}
   s.init_token=0;
   if(!x9w_control_ready||!x9_md_mipc_message_online())s.init_hold=true;
   if(s.init_hold){bzero(response,room);continue;} /* drain, never grant/advance */
   unsigned result=1,ready=0;
   unsigned id=s.init==X9M_CFG_IND?0x308:0x602;
   bool known=x9w_wire(response,n)&&x9q_u16(response+10)==id&&
    response[8]==(1u<<sim)&&x9w_word(response,n,0,result);
   bool snapshot=known&&!result&&x9w_byte(response,n,0x102,ready)&&ready<=1;
   if(s.init==X9M_READY_QUERY){
    /* A later IND wins over this query's snapshot. No SMSC bytes logged. */
    if(snapshot&&s.init_epoch==s.ready_epoch)x9m_md_ready(sim,ready!=0);
    s.init=X9M_WAIT_READY;
    if(!snapshot)printf("X9 SMS: SIM%u readiness query unavailable known %u result %u; waiting for CFG IND\n",sim+1,known?1:0,result);
   }else if(!known||result){
    s.init_hold=true;
    printf("X9 SMS: SIM%u init %u rejected known %u result %u; other SIM unaffected\n",sim+1,s.init,known?1:0,result);
   }else if(s.init==X9M_HOST_READY||s.init==X9M_FACTORY_CFG){
    if(s.init==X9M_HOST_READY&&x9m_factory_enabled()){
     /* The factory SMS_CFG sequence carries tag 0x108 after the config
      * bundle; host-ready waits for that confirmation too. */
     s.init=X9M_FACTORY_CFG;
    }else{
    if(s.init==X9M_FACTORY_CFG)s.mem_sent=true; /* same report as the host-ready point */
    if(snapshot&&!ready)x9m_md_ready(sim,false);
    s.init=X9M_INIT_DONE;
    if(!s.init_hold&&s.md_known&&s.md_ready&&s.init_epoch==s.ready_epoch){
     s.ready=true;x9m_ready_ind(sim);
     printf("X9 SMS: SIM%u real host-ready CNF after modem-ready; GSM handlers installed\n",sim+1);
    }else{s.init_hold=true;printf("X9 SMS: SIM%u config confirmed but readiness changed; session held\n",sim+1);}
    }
   }else s.init++;
   bzero(response,room);
  }
  if(!online||!s.wanted||x9m_init_hold||x9m_init_token||x9m_init_stage<2||s.init_hold||s.init==X9M_INIT_DONE)continue;
  unsigned step=s.init;
  if(step==X9M_WAIT_READY){if(!s.md_known||!s.md_ready)continue;step=X9M_HOST_READY;}
  x9w_frame f(step==X9M_CFG_IND?0x307:0x601,1u<<sim);
  if(step==X9M_CFG_IND){
   unsigned id=0x4601;
   uint8_t v[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,v,2);
  }else if(step==X9M_READY_QUERY)f.byte(0x102,1);
  else if(step==X9M_FACTORY_CFG)f.byte(0x108,1);
  else{f.byte(0x100,0);f.byte(0x105,1);f.byte(0x106,2);}
  uint64_t token=f.submit();
  if(token){s.init=step;s.init_token=token;s.init_epoch=s.ready_epoch;s.init_deadline=x9q_ms()+300000;}
 }
}
/* Boot IMS observation is independent of WMS clients and SMS readiness.
 * Register the already-supported 4a01 indication, then take one 0a04
 * snapshot per SIM. A timed-out read relinquishes its owner without retry;
 * subscriptions retain their existing ownership and no state is invented. */
static void x9m_ims_poll(uint8_t *p,unsigned room){
 const bool online=x9w_control_ready&&x9_md_mipc_online();
 const uint64_t now=x9q_ms();
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  x9m_sim &s=x9m_sims[sim];
  if(!online&&s.ims_stage)s.ims_hold=true;
  if(s.ims_token){
   if(now>=s.ims_deadline)s.ims_hold=true;
   if(s.ims_hold&&s.ims_stage==3){
    x9_md_mipc_ims_registration_abandon(s.ims_token);s.ims_token=0;
    continue;
   }
   unsigned n=0;int rc=x9_md_mipc_take(s.ims_token,p,room,&n);
   if(rc==1){
    s.ims_token=0;
    if(!online)s.ims_hold=true;
    unsigned result=1;
    bool known=x9w_wire(p,n)&&p[8]==(1u<<sim)&&
     x9q_u16(p+10)==(s.ims_stage==1?0x308:0xa05)&&x9w_word(p,n,0,result);
    if(!s.ims_hold&&s.ims_stage==1){
     if(!known||result)printf("X9 IMS: SIM%u indication registration unavailable known %u result %u\n",sim+1,known?1:0,result);
     s.ims_stage=2; /* A failed subscription does not prevent a read. */
    }else if(!s.ims_hold&&s.ims_stage==3){
     unsigned type=0,state=0;
     if(known&&!result&&x9w_byte(p,n,0x100,type)&&!type&&
       x9w_byte(p,n,0x101,state)&&state<=4&&s.ims_epoch==s.reg_epoch){
      s.reg_epoch++;
      if(state==1){s.reg_valid=false;printf("X9 IMS: SIM%u registration state 1 unclassified\n",sim+1);}
      else x9m_reg(sim,state==2?2:0);
     }else printf("X9 IMS: SIM%u registration snapshot unavailable or superseded\n",sim+1);
     s.ims_stage=4;
    }
    bzero(p,room);
   }else if(rc==-3){s.ims_token=0;
    if(s.ims_stage==1&&!s.ims_hold&&online)s.ims_stage=2;
    else s.ims_hold=true;
   }
   else if(rc<0)s.ims_hold=true;
  }
  if(!online||s.ims_hold||s.ims_token||s.ims_stage>=3)continue;
  if(!s.ims_admit_deadline)s.ims_admit_deadline=now+60000;
  if(now>=s.ims_admit_deadline){s.ims_hold=true;printf("X9 IMS: SIM%u admission timed out; no retry\n",sim+1);continue;}
  /* The indication inbox is quarantined independently of generic reads. */
  if(!s.ims_stage&&!x9_md_mipc_message_online())s.ims_stage=2;
  if(!s.ims_stage){
   x9w_frame f(0x307,1u<<sim);uint8_t v[2]={0x01,0x4a};f.tlv(0x100,v,2);
   uint64_t token=f.submit();if(token){s.ims_stage=1;s.ims_token=token;s.ims_admit_deadline=0;s.ims_deadline=x9q_ms()+60000;}
  }else if(s.ims_stage==2){
   x9w_frame f(0xa04,1u<<sim);f.byte(0x100,0);
   uint64_t token=f.submit();if(token){s.ims_stage=3;s.ims_token=token;s.ims_epoch=s.reg_epoch;s.ims_admit_deadline=0;s.ims_deadline=x9q_ms()+60000;}
  }
 }
}
static bool x9m_diag_enabled(){
 static int enabled=-1;if(enabled<0)enabled=sweet_x9_arg("x9_sms_diag",0)==1;
 return enabled!=0;
}
/* Observe the baseband feature cache independently of the registration
 * snapshot. Cached features do not authorize a provisioning write. */
static void x9m_ims_feature_poll(uint8_t *p,unsigned room){
 static struct {uint64_t token,deadline;bool done;} reads[2];
 if(sweet_x9_arg("x9_call_sip_diag",0)!=1)return;
 const bool online=x9w_control_ready&&x9_md_mipc_online();
 const uint64_t now=x9q_ms();
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  auto &query=reads[sim];
  if(query.done)continue;
  if(!query.token){
   if(!online||(!x9m_sims[sim].reg_valid&&x9m_sims[sim].ims_stage!=4&&!x9m_sims[sim].ims_hold))continue;
   if(!query.deadline)query.deadline=now+10000;
   if(now<query.deadline){
    x9w_frame f(0xa02,1u<<sim);f.word(0x100,0);
    query.token=f.submit();
    if(query.token)query.deadline=now+10000;
    continue;
   }
  }
  unsigned n=0,result=0;
  int rc=0;
  if(query.token&&online&&now<query.deadline){
   rc=x9_md_mipc_take(query.token,p,room,&n);
   if(!rc)continue;
  }else if(query.token)x9_md_mipc_ims_feature_abandon(query.token);
  x9q_view data={nullptr,0};
  if(rc==1&&x9w_wire(p,n)&&p[8]==(1u<<sim)&&x9q_u16(p+10)==0xa03&&
     x9w_word(p,n,0,result)&&!result)data=x9w_tlv(p,n,0x100);
  if(data.p&&data.n==6){
   printf("X9 IMS: SIM%u feature cache raw=%02x%02x%02x%02x%02x%02x "
          "volte=%u vonr=%u vilte=%u vinr=%u wfc=%u viwifi=%u sms=%u ims=%u\n",
    sim+1,data.p[0],data.p[1],data.p[2],data.p[3],data.p[4],data.p[5],
    (data.p[0]&1)?1u:0u,(data.p[0]&2)?1u:0u,
    (data.p[1]&1)?1u:0u,(data.p[1]&2)?1u:0u,
    data.p[2]?1u:0u,data.p[3]?1u:0u,data.p[4]?1u:0u,data.p[5]?1u:0u);
  }else printf("X9 IMS: SIM%u feature cache unavailable rc=%d result=0x%x\n",sim+1,rc,result);
  query.done=true;query.token=0;bzero(p,room);
 }
}
static void x9m_rawsend_reject_diag(unsigned sim,const uint8_t *req,unsigned len){
 static unsigned remaining=32;
 if(!remaining||!x9m_diag_enabled()||len<7)return;
 for(unsigned at=7;remaining&&len-at>=3;){
  const unsigned size=x9q_u16(req+at+1);
  if(size>len-at-3)return;
  if(req[at]!=1){
   printf("X9 SMS DIAG: SIM%u RawSend rejected txn %u TLV 0x%02x len %u\n",
          sim+1,x9q_u16(req+1),req[at],size);
   remaining--;
  }
  at+=3+size;
 }
}

/* Factory-init replay (x9_factory_init, default on; =0 opts out). During telephony
 * init Android's RIL provisions each slot (this device's own capture,
 * radio-baseline.txt 2026-11-15): SIM1 first receives one zeroed
 * IMS_CFG_ADDITION_SERVICE 0a1c, then each SIM receives
 * IMS_ENABLE_DATA_CHANNEL 0a18 (tag 0x100 = 0) and IMS_SET_CONFIG 0a00
 * (tag 0x100 = 0, tag 0x102 = the six aggregated feature bytes), followed
 * by the minimal IMS DATA_ACT_CALL (APN_TYPE 2, URSP descriptor DNN
 * "ims") that establishes the factory IMS bearer. Under iOS
 * the MD feature cache reads all zero, so none of this ever happens. The
 * writer mirrors the factory frames and order, then re-reads the cache;
 * a config ACK alone does not prove registration, so registration
 * evidence stays with the existing 4a01/0a04 observers. Any rejection or
 * timeout holds the sequence for the boot without retry. */
static void x9m_factory_ims_bytes(uint8_t out[6]){
 /* Aggregation inputs are this device's factory-observed carrier policy
  * for its SIMs: VoLTE/VoNR/ViLTE/ViNR on, WFC/ViWiFi off, IMS SMS on,
  * eims byte on. Bit layout follows the feature-cache decode above. */
 const bool volte=true,vonr=true,vilte=true,vinr=true;
 const bool wfc=false,viwifi=false,sms=true,ims=true;
 out[0]=(uint8_t)((volte?1:0)|(vonr?2:0));
 out[1]=(uint8_t)((vilte?1:0)|(vinr?2:0));
 out[2]=wfc?1:0;out[3]=viwifi?1:0;out[4]=sms?1:0;out[5]=ims?1:0;
}
static void x9m_factory_poll(uint8_t *p,unsigned room){
 if(!x9m_factory_enabled())return;
 const bool online=x9w_control_ready&&x9_md_mipc_online();
 const uint64_t now=x9q_ms();
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  x9m_sim &s=x9m_sims[sim];
  if(s.fx_stage>=6||s.fx_hold)continue;
  if(s.fx_token){
   if(now>=s.fx_deadline){s.fx_hold=true;
    printf("X9 FACTORY: SIM%u stage %u timeout; token retained, no retry\n",sim+1,s.fx_stage);continue;}
   unsigned n=0;int rc=x9_md_mipc_take(s.fx_token,p,room,&n);
   if(!rc)continue;
   if(rc<0){s.fx_hold=true;
    printf("X9 FACTORY: SIM%u stage %u transport error; held\n",sim+1,s.fx_stage);continue;}
   s.fx_token=0;
   unsigned expect=s.fx_stage==1?0xa1d:s.fx_stage==2?0xa19:s.fx_stage==3?0xa01:s.fx_stage==4?0x202:0xa03;
   unsigned result=1;
   bool known=p[8]==(1u<<sim)&&x9q_u16(p+10)==expect&&x9w_word(p,n,0,result)&&
    (s.fx_stage==4||x9w_wire(p,n));
   if(!known||result){s.fx_hold=true;
    printf("X9 FACTORY: SIM%u stage %u rejected known %u result %u; held\n",sim+1,s.fx_stage,known?1:0,result);
    bzero(p,room);continue;}
   if(s.fx_stage==3)printf("X9 FACTORY: SIM%u IMS config accepted by modem\n",sim+1);
   if(s.fx_stage==4)printf("X9 FACTORY: SIM%u IMS data call accepted by modem\n",sim+1);
   if(s.fx_stage==5){
    x9q_view data=x9w_tlv(p,n,0x100);
    if(data.p&&data.n==6)
     printf("X9 FACTORY: SIM%u feature cache after write raw=%02x%02x%02x%02x%02x%02x\n",
      sim+1,data.p[0],data.p[1],data.p[2],data.p[3],data.p[4],data.p[5]);
    else printf("X9 FACTORY: SIM%u post-write feature cache unreadable\n",sim+1);
   }
   s.fx_stage++;bzero(p,room);continue;
  }
  if(!online)continue;
  /* Start only after the boot IMS snapshot concluded for this SIM. */
  if(!s.fx_stage&&!(s.ims_stage>=4||s.ims_hold))continue;
  if(!s.fx_stage)s.fx_stage=sim?2:1; /* 0a1c observed on SIM1 only. */
  uint64_t token=0;
  if(s.fx_stage==1){x9w_frame f(0xa1c,1u<<sim);uint8_t z[64]={};f.tlv(0x100,z,sizeof(z));token=f.submit();}
  else if(s.fx_stage==2){x9w_frame f(0xa18,1u<<sim);f.byte(0x100,0);token=f.submit();}
  else if(s.fx_stage==3){x9w_frame f(0xa00,1u<<sim);f.word(0x100,0);
   uint8_t b[6];x9m_factory_ims_bytes(b);f.tlv(0x102,b,6);token=f.submit();}
  else if(s.fx_stage==4){
   /* Factory IMS bearer (radio-baseline.txt 18:38:15.635): a minimal
    * DATA_ACT_CALL with APN_TYPE 2 and only the URSP traffic descriptor
    * carrying DNN "ims" (is_match_all 0, dnn_len 3 at +1, dnn at +4,
    * rest zero) beside URSP_EVAL_FLAG 0 and tag 0x10a = 3. */
   x9w_frame f(0x201,1u<<sim);f.word(0x10f,0);f.word(0x102,2);f.byte(0x10a,3);
   static uint8_t td[672];bzero(td,sizeof(td));td[1]=3;memcpy(td+4,"ims",3);
   f.tlv(0x10d,td,sizeof(td));token=f.submit();
  }
  else if(s.fx_stage==5){x9w_frame f(0xa02,1u<<sim);f.word(0x100,0);token=f.submit();}
  if(token){s.fx_token=token;s.fx_deadline=now+10000;}
  else s.fx_hold=true;
 }
}
static bool x9m_diag_storage_busy(unsigned sim,const x9m_job *self){
 for(const x9m_job &other:x9m_jobs)if(&other!=self&&other.sim==sim&&x9m_storage(other.op))return true;
 return false;
}
static void x9m_diag_poll(){
 if(!x9m_diag_enabled())return;
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  x9m_sim &s=x9m_sims[sim];
  if(s.diag_stage||!s.ready||!x9w_control_ready||!x9_md_mipc_message_online())continue;
  bool busy=x9m_diag_storage_busy(sim,nullptr);
  for(const x9m_job &j:x9m_jobs)if(j.op==X9M_DIAG&&j.sim==sim){busy=true;s.diag_stage=1;}
  if(busy)continue;
  uint8_t req[7]={};x9m_job *j=x9m_new(X9M_DIAG,0,sim,req,sizeof(req),true);
  if(j){s.diag_stage=1;j->follow=true;
   printf("X9 SMS DIAG: SIM%u storage status probe armed; explicit bootarg only\n",sim+1);}
 }
}
struct x9m_publication {
 uint32_t port,node;const uint8_t *sdu;uint32_t size;
 uint8_t *owned;uint32_t room;
};
static int x9m_publish(void *context){
 /* Trusted closure: host queue only, no allocation or MD API reentry. */
 x9m_publication &p=*static_cast<x9m_publication *>(context);
 return bb_x9_sms_event(p.port,p.node,p.sdu,p.size,p.owned,p.room)?1:0;
}
static void x9m_deliver(x9m_incoming &in,unsigned mti){
 /* The QMI owner alone changes control-ready. Native publish below holds
  * the MD mutex through queue admission, closing event->poison->retry. */
 if(!x9w_control_ready||x9q_ms()<in.next_delivery||!x9_md_mipc_message_online())return;
 unsigned cls=mti==2?4:x9ms_pdu_class(in.pdu,in.len);
 if(cls>5)return;
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){
  x9m_client &c=x9m_clients[i];
  if(!c.events||!c.routes||x9q_clients[i].sim_slot!=in.sim||(mti==2&&!c.status_reports))continue;
  if(!(c.route_mask&(1u<<cls)))continue;
  /* One client owns the command, even when multiple clients subscribe. */
  if(c.route[cls]==0){
   /* Explicit DISCARD policy still needs the original MD command ACK.
    * This is not an automatic successful ACK for a transfer-only route. */
   if(x9_md_mipc_message_reply(in.token,1,0)==1){
    in.replying=true;bzero(in.pdu,sizeof(in.pdu));
   }
   return;
  }
  uint8_t req[7]={},v[8+255]={};v[0]=0;
  v[1]=(uint8_t)in.token;v[2]=(uint8_t)(in.token>>8);v[3]=(uint8_t)(in.token>>16);v[4]=(uint8_t)(in.token>>24);
  v[5]=6;v[6]=(uint8_t)in.len;v[7]=(uint8_t)(in.len>>8);memcpy(v+8,in.pdu,in.len);
  x9q_packet p(req);p.tlv(0x11,v,8+in.len);x9m_number(p,0x12,1,1);x9m_number(p,0x16,0,1);
  /* Serialize this indication without the response result TLV. The legacy
   * QRTR API returns port recognition, not enqueue success. Keep the PDU
   * and retry only admission (never MD ACK) if the actual host queue is full. */
  memmove(p.p+7,p.p+14,p.n-14);p.n-=7;p.p[0]=4;p.p[3]=1;
  p.p[5]=(uint8_t)(p.n-7);p.p[6]=(uint8_t)((p.n-7)>>8);
  uint32_t room=CBQ_HDR+p.n;
  uint8_t *owned=p.good?static_cast<uint8_t *>(IOMallocData(room)):nullptr;
  x9m_publication publication={CBQ_PORT_BASE+i,X9BB_NODE,p.p,p.n,owned,room};
  bool admitted=owned&&x9_md_mipc_message_publish(in.token,in.sim,x9m_publish,&publication)==1;
  if(!admitted&&owned){bzero(owned,room);IOFreeData(owned,room);}
  bzero(v,sizeof(v));bzero(p.p,sizeof(p.p));
  if(!admitted){in.next_delivery=x9q_ms()+100;return;}
  in.delivered=true;in.slot=i;in.generation=x9q_clients[i].generation;in.ack_wait=x9q_ms()+30000;
  bzero(in.pdu,sizeof(in.pdu));
  printf("X9 SMS: incoming GSM %s SIM%u command token %u; waiting for host ACK\n",mti==2?"status report":"message",in.sim+1,in.token);
  return;
 }
}
static void x9m_event(uint8_t *p,unsigned n,uint32_t token){
 if(!x9w_wire(p,n))return; /* Bad commands stay owned, never success-ACKed. */
 unsigned mask=p[8]&((1u<<x9_md_sim_count())-1),id=x9q_u16(p+10);
 if(id==0x4601){
  unsigned result,ready;
  if(!x9w_word(p,n,0,result)||result||!x9w_byte(p,n,0x102,ready)||ready>1)return;
  for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(mask&(1u<<sim))x9m_md_ready(sim,ready!=0);
  return; // Modem-ready is prerequisite, never proof of host config success.
 }
 if(id==0x4a01){
  /* RIL onHandleUrc 290e18..290ecc first dispatches tag100: only type0
   * owns CIREGU101 (0/1, NOT the query's 0..4 enumeration). Type1 is WFC;
   * type2/3 own account/URI/expiry/response, not this registration state.
   * The captured type2 expiry106=255 is NOT a service-capability mask. */
  unsigned type,state;
  if(!x9w_byte(p,n,0x100,type)||type||!x9w_byte(p,n,0x101,state)||state>1)return;
  for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(mask&(1u<<sim)){
   x9m_sims[sim].reg_epoch++;x9m_reg(sim,state?2:0);
  }
  return;
 }
 if(!token)return;
 if(mask!=1&&mask!=2)return;
 x9q_view v={nullptr,0};unsigned mti=id==0x8601?0:2;
 if(id==0x8601){
  unsigned type;if(!x9w_byte(p,n,0x100,type)||type!=3){
   static unsigned once;if(!once++)printf("X9 SMS: non-CS command retained; type4/8105 encoding and ACK not mapped\n");return;}
  v=x9w_tlv(p,n,0x8104);
  if(!v.p||v.n<11)return;
  unsigned full=1u+v.p[8]+v.p[2];
  if(full>v.n-8||!x9m_pdu(v.p+8,full,0))return;
  v={v.p+8,full};
 }else if(id==0x8603){
  v=x9w_tlv(p,n,0x8100);
  bool valid=x9m_pdu(v.p,v.n,2);
  printf("X9 SMS DIAG: 8603 SIM%u token %u status report valid %u\n",mask==2?2u:1u,token,valid?1u:0u);
  if(!valid)return;
 }
 else return;
 for(x9m_incoming &in:x9m_incoming_sms)if(!in.token){
  bzero(&in,sizeof(in));in.token=token;in.sim=mask==2?1:0;in.len=v.n;memcpy(in.pdu,v.p,v.n);
  x9m_deliver(in,mti);return;
 }
 printf("X9 SMS: host inbox full; MD command retained without ACK\n");
}
static bool x9m_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned len){
 unsigned service=port-X9BB_PORT_BASE;if(service!=5&&service!=0x21)return false;
 unsigned slot=local-CBQ_PORT_BASE,id=x9q_u16(req+3),sim=x9q_clients[slot].sim_slot;
 x9m_client &c=x9m_clients[slot];x9m_sims[sim].wanted=true;
 uint16_t err=0;
 if(service==5&&id==0x4c){
  x9q_view v=x9q_tlv(req,len,1);
  if(len!=11||!v.p||v.n!=1||v.p[0]>=x9_md_sim_count())err=X9Q_ARG;
  else{
   for(const x9m_job &j:x9m_jobs)if(j.op&&j.slot==slot)err=X9Q_BUSY;
   for(const x9m_incoming &in:x9m_incoming_sms)if(in.token&&in.delivered&&in.slot==slot)err=X9Q_BUSY;
   if(!err){x9q_clients[slot].sim_slot=v.p[0];c.reg_next=0;x9m_sims[v.p[0]].wanted=true;}
  }
 }else if(service==5&&id==1){
  /* 20H392 emits BOTH ReportMTMessage(10) and ReportMwiMessage(12).
   * Validate the entire patch before changing either subscription. MWI
   * subscription does not synthesize a voicemail state or IMS capability. */
  bool events=c.events,mwi=c.mwi_events;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1)){
   if((req[at]!=0x10&&req[at]!=0x12)||x9q_u16(req+at+1)!=1||req[at+3]>1){err=X9Q_ARG;break;}
   if(req[at]==0x10)events=req[at+3];else mwi=req[at+3];
  }
  if(!err){c.events=events;c.mwi_events=mwi;
   printf("X9 SMS: SIM%u event subscription MT %u MWI %u\n",sim+1,events?1:0,mwi?1:0);}
 }else if(service==5&&id==0x47){
  bool ready=c.ready_ind;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1)){
   if(req[at]<0x10||req[at]>0x18||x9q_u16(req+at+1)!=1||req[at+3]>1){err=X9Q_ARG;break;}
   if(req[at]==0x13)ready=req[at+3];
  }
  /* These are subscriptions, not synthesized transport/MWI/storage state.
   * Only service-ready is currently produced; other events remain absent. */
  if(!err)c.ready_ind=ready;
 }else if(service==0x21&&id==0x22){
  bool reg=c.ims_ind;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1)){
   if(req[at]!=0x10||x9q_u16(req+at+1)!=1||req[at+3]>1){err=X9Q_UNSUPPORTED;break;}
   reg=req[at+3];
  }
  if(!err){c.ims_ind=reg;c.reg_next=0;}
 }else if(service==5&&id==0x32){
  x9q_view v=x9q_tlv(req,len,1),reports=x9q_tlv(req,len,0x10);uint8_t route[6]={};bool seen[6]={};
  if(!v.p||v.n<2||x9q_u16(v.p)>6||v.n!=2+4u*x9q_u16(v.p))err=X9Q_ARG;
  if(!err)for(unsigned at=2;at<v.n;at+=4){
   unsigned cls=v.p[at+1];
   if(v.p[at]||cls>5||seen[cls]||v.p[at+2]!=0xff||(v.p[at+3]!=0&&v.p[at+3]!=2)){err=X9Q_UNSUPPORTED;break;}
   seen[cls]=true;route[cls]=v.p[at+3];
  }
  if(reports.p&&(reports.n!=1||reports.p[0]!=1))err=X9Q_UNSUPPORTED;
  if(len!=7+3+v.n+(reports.p?4:0))err=X9Q_ARG;
  if(!err){memcpy(c.route,route,sizeof(route));c.route_mask=0;
   for(unsigned i=0;i<6;i++)if(seen[i])c.route_mask|=(uint8_t)(1u<<i);
   c.routes=true;c.status_reports=reports.p!=nullptr;}
 }else if(service==5&&id==0x33&&len==7){
  if(!c.routes)err=X9Q_NOT_READY;
  else{
   uint8_t routes[2+4*6]={};unsigned n=2,count=0;
   for(unsigned i=0;i<6;i++)if(c.route_mask&(1u<<i)){
    routes[n++]=0;routes[n++]=(uint8_t)i;routes[n++]=0xff;routes[n++]=c.route[i];count++;
   }
   routes[0]=(uint8_t)count;x9q_packet p(req);p.tlv(1,routes,n);
   if(c.status_reports)x9m_number(p,0x10,1,1);p.send(local,port);return true;
  }
 }else if(service==5&&id==0x5c&&len==7){
  x9q_packet p(req);x9m_number(p,0x11,x9m_sims[sim].ready&&x9w_control_ready&&x9_md_mipc_message_online()?1:0,1);p.send(local,port);return true;
 }else if(service==5&&id==0x30&&len==7){
  if(!x9m_sims[sim].ready||!x9w_control_ready||!x9_md_mipc_message_online())err=X9Q_NOT_READY;
  else{x9q_packet p(req);x9m_number(p,1,1,1);p.send(local,port);return true;}
 }else if(service==5&&(id==0x31||id==0x22||id==0x23)){
  struct x9ms_storage_request storage;unsigned decoded=x9ms_storage_decode(req,len,&storage);
  if(decoded)err=decoded==X9MS_ARG?X9Q_ARG:X9Q_UNSUPPORTED;
  /* Apple sends List31 during startup before the real host-ready CNF.
   * Accept validated bounded work into the existing per-SIM queue; poll
   * still requires THIS SIM's actual ready CNF before any native TX.
   * Do not synthesize an empty list or enqueue with an offline transport. */
  if(!err&&(!x9w_control_ready||!x9_md_mipc_message_online()))err=X9Q_NOT_READY;
  if(x9m_store_order==~0ull)err=X9Q_BUSY;
  if(!err){
   x9m_job *j=x9m_new(storage.list?X9M_LIST:storage.modify?X9M_TAG:X9M_READ,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else if(!j->bridge_generation){bzero(j,sizeof(*j));err=X9Q_NOT_READY;}
   else{
    j->store_order=++x9m_store_order;j->follow=true;
    printf("X9 SMS: SIM%u stored %s queued QMI txn %u; native per-SIM serialization\n",
     sim+1,storage.list?"list":storage.modify?"mark-read":"read",x9q_u16(req+1));return true;
   }
  }
 }else if(service==5&&id==0x20){
  x9q_view raw=x9q_tlv(req,len,1),ims=x9q_tlv(req,len,0x13);
  x9q_view retry=x9q_tlv(req,len,0x14),retry_id=x9q_tlv(req,len,0x15);
  const bool is_retry=retry.p&&retry.n==1&&retry.p[0]==1;
  const bool has_retry_id=retry_id.p&&retry_id.n==4&&x9q_u32(retry_id.p)<=255;
  if(!raw.p||raw.n<6||raw.p[0]!=6||x9q_u16(raw.p+1)!=raw.n-3||!x9m_pdu(raw.p+3,raw.n-3,1))err=X9Q_ARG;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1))if(req[at]!=1&&
   !(req[at]==0x13&&x9q_u16(req+at+1)==1&&req[at+3]==0)&&
   req[at]!=0x14&&req[at]!=0x15&&
   /* LinkTimer is a dedicated-channel reuse hint, not the SMS delivery
    * timeout. Leave MTK's native link lifetime unchanged; never resend. */
   !(req[at]==0x12&&x9q_u16(req+at+1)==1&&req[at+3]<=1))err=X9Q_UNSUPPORTED;
  if(ims.p&&(ims.n!=1||ims.p[0]))err=X9Q_UNSUPPORTED;
  if(!err&&((retry.p&&(retry.n!=1||retry.p[0]>1))||
            (is_retry?!has_retry_id:retry_id.p!=nullptr)))err=X9Q_ARG;
  if(err==X9Q_UNSUPPORTED)x9m_rawsend_reject_diag(sim,req,len);
  if(!err&&(!x9m_sims[sim].ready||!x9w_control_ready||!x9_md_mipc_message_online()))err=X9Q_NOT_READY;
  for(const x9m_job &j:x9m_jobs)if(j.op==X9M_SEND&&j.sim==sim)err=X9Q_BUSY;
  if(!err){
   x9m_job *j=x9m_new(X9M_SEND,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else if(!j->bridge_generation){bzero(j,sizeof(*j));err=X9Q_NOT_READY;}
   else{
    x9w_frame f(0x605,1u<<sim);f.byte(0x100,0);
    const unsigned pdu_offset=f.n+4;
    f.tlv(0x8101,raw.p+3,raw.n-3);
    if(is_retry&&f.good&&!x9ms_submit_retry(f.p+pdu_offset,raw.n-3,x9q_u32(retry_id.p))){
     bzero(j,sizeof(*j));x9q_packet p(req,X9Q_ARG);p.send(local,port);return true;
    }
    const bool report=f.good&&x9ms_submit_report(f.p+pdu_offset,raw.n-3,
                                                sweet_x9_arg("x9_sms_srr",0)==1);
    f.byte(0x102,(uint8_t)(raw.n-4-raw.p[3]));j->token=f.submit();
    if(j->token){printf("X9 SMS: SIM%u send submitted token %llu QMI txn %u status report requested %u retry %u; waiting for modem CNF\n",
      sim+1,(unsigned long long)j->token,x9q_u16(req+1),report?1u:0u,is_retry?1u:0u);return true;}
    bzero(j,sizeof(*j));err=X9Q_BUSY;
   }
  }
 }else if(service==5&&id==0x37){
  if(x9m_ack_repeated(local,X9BB_NODE,port,req,len))return true;
  x9q_view info=x9q_tlv(req,len,1),failure=x9q_tlv(req,len,0x11),ims=x9q_tlv(req,len,0x12);
  if(!info.p||info.n!=6||info.p[4]!=1||info.p[5]>1)err=X9Q_ARG;
  if(failure.p&&(failure.n!=2||info.n!=6||info.p[5]))err=X9Q_ARG;
  uint8_t cause=0xff;
  if(!err&&failure.p){
   /* QMISMS sendAck writes RP memory-capacity=22, TP=0. Android's GSM
    * acknowledge expects a TP failure (23.040): memory-capacity is D3,
    * not 00 and not the RP enumeration. Other RP-only causes are unmapped. */
   if(failure.p[0]==0x16&&failure.p[1]==0)cause=0xd3;
   else if(failure.p[1]>=0x80)cause=failure.p[1];
   else err=X9Q_UNSUPPORTED;
  }
  if(ims.p&&(ims.n!=1||ims.p[0]))err=X9Q_UNSUPPORTED;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1))if(req[at]!=1&&req[at]!=0x11&&req[at]!=0x12)err=X9Q_UNSUPPORTED;
  x9m_incoming *owner=nullptr;
  if(!err)for(x9m_incoming &in:x9m_incoming_sms)if(in.token==x9q_u32(info.p)&&in.delivered&&
   in.sim==sim&&in.slot==slot&&in.generation==x9q_clients[slot].generation)owner=&in;
  if(!err&&(!owner||owner->replying))err=X9Q_ARG;
  printf("X9 SMS: host ACK SIM%u txn %u owner %u validation %u; no payload logged\n",
   sim+1,x9q_u16(req+1),owner?1:0,err);
  if(!err){
   x9m_job *j=x9m_new(X9M_ACK,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else{
    int rc=j->bridge_generation?x9_md_mipc_message_reply(owner->token,info.p[5],cause):0;
    if(rc==1){j->token=owner->token;owner->replying=true;
     printf("X9 SMS: SIM%u command token %u host %s queued; waiting for physical ACK TX\n",
      sim+1,owner->token,info.p[5]?"ACK":"NACK");return true;}
    bzero(j,sizeof(*j));err=rc<0?X9Q_ARG:X9Q_NOT_READY;
   }
  }
 }else if((service==0x21&&id==0x20)||(service==5&&id==0x34)){
  if(len!=7)err=X9Q_ARG;
  else if(!x9w_control_ready||!(service==5?x9_md_mipc_message_online():x9_md_mipc_online()))err=X9Q_NOT_READY;
  else{
   unsigned op=service==0x21?X9M_REG:X9M_SMSC;
   x9m_job *j=x9m_new(op,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else if(op==X9M_SMSC){
    /* iOS asks once during startup. Like Android checkSMSC, serialize the
     * read after this SIM's ready/config handshake rather than racing it. */
    j->deadline=x9q_ms()+20000;return true;
   }else{x9w_frame f(0xa04,1u<<sim);f.byte(0x100,0);
    j->reg_epoch=x9m_sims[sim].reg_epoch;j->token=f.submit();if(j->token)return true;bzero(j,sizeof(*j));err=X9Q_BUSY;}
  }
 }else err=X9Q_UNSUPPORTED;
 x9m_reply(slot,service,req,err);return true;
}
static void x9m_job_done(x9m_job &j,const uint8_t *p,unsigned n){
 if(j.op==X9M_DIAG){
  const unsigned expected=j.step?0x608:0x60c;unsigned result=1;
  bool known=x9w_wire(p,n)&&x9q_u16(p+10)==expected&&p[8]==(1u<<j.sim)&&x9w_word(p,n,0,result);
  if(!known||result){
   printf("X9 SMS DIAG: SIM%u %s known %u result 0x%x id 0x%04x tx %u; no retry\n",
    j.sim+1,j.step?"CMGR":"CPMS",known?1:0,result,known?(unsigned)x9q_u16(p+10):0u,
    known?(unsigned)x9q_u16(p+12):0u);
   x9m_error(j,X9Q_INTERNAL);return;
  }
  if(!j.step){
   const unsigned tags[5]={0x100,0x101,0x102,0x103,0x104},sizes[5]={4,2,2,2,1};
   unsigned values[5]={UINT32_MAX,0,0,0,0};
   for(unsigned i=0;i<5;i++){
    x9q_view v=x9w_tlv(p,n,tags[i]);if(!v.p||v.n!=sizes[i])continue;
    values[i]=0;for(unsigned k=0;k<v.n;k++)values[i]|=(unsigned)v.p[k]<<(8*k);
   }
   printf("X9 SMS DIAG: SIM%u CPMS 100=%u 101=%u 102=%u 103=%u 104=%u\n",
    j.sim+1,values[0],values[1],values[2],values[3],values[4]);
   j.step=1;j.follow=true;j.token=0;return;
  }
  x9q_view records={nullptr,0};unsigned count=0,type=UINT32_MAX,index=0;
  bool valid=x9m_stored(p,n,records,count);
  if(valid&&count){x9w_byte(p,n,0x100,type);index=x9q_u16(records.p);}
  printf("X9 SMS DIAG: SIM%u CMGR index1 valid %u count %u type %u index_match %u; payload not logged\n",
   j.sim+1,valid?1:0,count,type,valid&&count==1&&index==1?1:0);
  j.answered=true;j.follow=false;j.token=0;x9m_sims[j.sim].diag_stage=2;return;
 }
 unsigned result=1;
 bool stored=x9m_storage(j.op);
 bool known=x9w_wire(p,n)&&x9q_u16(p+10)==(stored?0x608:j.op==X9M_SEND?0x606:j.op==X9M_REG?0xa05:0x602)&&
  p[8]==(1u<<j.sim)&&x9w_word(p,n,0,result);
 if(j.op==X9M_SEND){
  /* Correlate the real 0606 with the submitted 0605 and WMS request.
   * Do not print TPDU, address, message reference or infer delivery. */
  printf("X9 SMS: send CNF SIM%u token %llu QMI txn %u known %u result 0x%x; not delivery\n",
   j.sim+1,(unsigned long long)j.token,(unsigned)x9q_u16(j.req+1),known?1:0,result);
 }
 if(!known||result){j.hold=(j.op==X9M_SEND&&!known)||(j.op==X9M_TAG&&j.step!=0);
  static unsigned diag_failures;
  if((stored||j.op==X9M_SEND)&&diag_failures<16){
   diag_failures++;
   bool wire=x9w_wire(p,n);
   printf("X9 SMS DIAG: %s SIM%u op %u known %u result 0x%x id 0x%04x tx %u mask 0x%02x bytes %u tlvs",
    stored?"0608":"0606",j.sim+1,j.op,known?1:0,result,
    n>=16?(unsigned)x9q_u16(p+10):0u,n>=16?(unsigned)x9q_u16(p+12):0u,
    n>=16?(unsigned)p[8]:0u,n);
   /* Print short non-payload scalar TLV values (<0x8000, <=4B) only; never log PDU/address payloads. */
   for(unsigned at=16;wire&&at+4<=n;){
    unsigned raw_tag=x9q_u16(p+at),tag=raw_tag&0x7fffu,k=x9q_u16(p+at+2);
    if(k>n-at-4){printf(" %x/%u!",tag,k);break;}
    if(raw_tag<0x8000u&&k>0&&k<=4&&(j.op!=X9M_SEND||tag==0||tag==0x106)){
     unsigned v=0;for(unsigned i=0;i<k;i++)v|=(unsigned)p[at+4+i]<<(8u*i);
     printf(" %x/%u=0x%x",tag,k,v);
    }else printf(" %x/%u",tag,k);
    at+=k?((k+11)&~7u):16;
   }
   printf("\n");
  }
  printf("X9 SMS/IMS: operation %u SIM%u CNF known %u result %u\n",j.op,j.sim+1,known?1:0,result);
  x9m_error(j,X9Q_INTERNAL);return;}
 x9q_packet response(j.req);
 if(j.op==X9M_SEND){
  unsigned mr;if(!x9w_byte(p,n,0x103,mr)){j.hold=true;x9m_error(j,X9Q_INTERNAL);return;}x9m_number(response,1,mr,2);
  printf("X9 SMS: SIM%u send CNF accepted by modem; NOT a delivery receipt\n",j.sim+1);
 }else if(j.op==X9M_REG){
  unsigned type,state;
  if(!x9w_byte(p,n,0x100,type)||type||!x9w_byte(p,n,0x101,state)||state>4){x9m_error(j,X9Q_INTERNAL);return;}
  /* This RIL explicitly maps 2 -> registered, 3/4 -> not registered.
   * It passes through 0/1 without naming them; do not invent REGISTERING
   * from a numeric similarity to IMSA. State 1 remains unclassified. */
  if(j.reg_epoch!=x9m_sims[j.sim].reg_epoch){x9m_error(j,X9Q_NOT_READY);return;}
  x9m_sims[j.sim].reg_epoch++;
  if(state==1){x9m_sims[j.sim].reg_valid=false;x9m_error(j,X9Q_NOT_READY);return;}
  unsigned reg=state==2?2:0;x9m_reg(j.sim,reg);x9m_number(response,0x12,reg);
 }else if(stored){
  x9q_view records={nullptr,0};unsigned count=0,type=0;
  if(!x9m_stored(p,n,records,count)||
   (count&&(!x9w_byte(p,n,0x100,type)||type!=3))){j.hold=j.op==X9M_TAG&&j.step!=0;x9m_error(j,X9Q_INTERNAL);return;}
  if(j.op==X9M_LIST){
   /* Bounded by the native frame, not a guessed SIM capacity. */
   uint8_t values[4+5*(X9_MIPC_FRAME_MAX/264)]={};values[0]=(uint8_t)count;
   x9q_view tag=x9q_tlv(j.req,j.len,0x10);
   for(unsigned i=0;i<count;i++){
    const uint8_t *r=records.p+264*i;unsigned at=4+5*i;
    if(tag.p&&(r[3]^1u)!=tag.p[0]){bzero(values,sizeof(values));x9m_error(j,X9Q_INTERNAL);return;}
    values[at]=r[0];values[at+1]=r[1];values[at+4]=r[3]^1u;
   }
   response.tlv(1,values,4+5*count);bzero(values,sizeof(values));
   printf("X9 SMS: SIM%u real stored list CNF count %u; no synthetic empty list\n",j.sim+1,count);
  }else{
   x9q_view storage=x9q_tlv(j.req,j.len,1);
   if(count!=1||x9q_u16(records.p)!=x9q_u32(storage.p+1)){
    j.hold=j.op==X9M_TAG&&j.step!=0;x9m_error(j,X9Q_INTERNAL);return;}
   const uint8_t *r=records.p;unsigned full=1u+r[8]+r[2];uint8_t value[4+200]={};
   if(j.op==X9M_TAG){
    if(r[3]>1){j.hold=j.step!=0;x9m_error(j,X9Q_UNSUPPORTED);return;}
    if(j.step==0){
     if(r[3]==0){
      if(j.answered||!x9m_live(j.slot,j.generation)||x9q_ms()>=j.deadline){x9m_error(j,X9Q_NOT_READY);return;}
      j.stored_len=full;memcpy(j.stored_pdu,r+8,full);j.step=1;j.follow=true;j.token=0;return;
     } // Already read: the requested tag is confirmed by a real native read.
    }else{
     if(full!=j.stored_len||memcmp(j.stored_pdu,r+8,full)||(j.step==2&&r[3]!=1)){
      j.hold=true;x9m_error(j,X9Q_INTERNAL);return;
     }
     if(j.step==1){j.step=2;j.follow=true;j.token=0;return;}
    }
    printf("X9 SMS: SIM%u stored MT read tag verified by native readback; not application persistence proof\n",j.sim+1);
   }else{
    value[0]=r[3]^1u;value[1]=6;value[2]=(uint8_t)full;memcpy(value+4,r+8,full);
    response.tlv(1,value,4+full);bzero(value,sizeof(value));
    printf("X9 SMS: SIM%u real stored read CNF; read tag unchanged\n",j.sim+1);
   }
  }
 }else{
  x9q_view text=x9w_tlv(p,n,0x8101);
  if(!text.p||text.n<2||text.n>23||text.p[text.n-1]||strnlen((const char *)text.p,text.n)!=text.n-1){x9m_error(j,X9Q_INTERNAL);return;}
  bool intl=text.p[0]=='+';unsigned start=intl?1:0,k=text.n-1-start;
  if(!k||k>20){x9m_error(j,X9Q_INTERNAL);return;}
  for(unsigned i=start;i<text.n-1;i++)if(text.p[i]<'0'||text.p[i]>'9'){x9m_error(j,X9Q_INTERNAL);return;}
  uint8_t value[24]={'1',(uint8_t)(intl?'4':'2'),(uint8_t)(intl?'5':'9'),(uint8_t)k};
  memcpy(value+4,text.p+start,k);response.tlv(1,value,k+4);bzero(value,sizeof(value));
 }
 if(x9m_has_receipt(j.op))x9m_terminal(j,response);
 else if(!j.answered&&!j.background&&x9m_live(j.slot,j.generation))response.send(CBQ_PORT_BASE+j.slot,X9BB_PORT_BASE+x9m_service(j));
 bzero(response.p,sizeof(response.p));
}
static void x9m_del(unsigned slot){
 bzero(&x9m_clients[slot],sizeof(x9m_clients[slot]));
 /* Sent SMS/ACK owners survive deletion and drain their late result. */
 for(x9m_job &j:x9m_jobs)if(j.op&&j.op!=X9M_DIAG&&j.slot==slot){
  if(j.op==X9M_TAG&&j.step==1&&j.token)
   x9_md_mipc_message_mark_read_abandon(j.token,j.slot,j.generation);
  j.answered=true;j.receipt_pending=false;j.receipt_len=0;bzero(j.receipt,sizeof(j.receipt));
 }
 /* No successful auto-ACK on client loss. Commands remain bound to the old
  * generation, so a reused port/SIM cannot acknowledge someone else's SMS. */
}
static void x9m_poll(){
 static uint8_t p[X9_MIPC_FRAME_MAX];uint64_t now=x9q_ms();
 if(!x9w_control_ready||!x9_md_mipc_message_online())x9m_init_transport_loss();
 if(!x9_md_mipc_online())for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  if(x9m_sims[sim].md_known)x9m_md_ready(sim,false);
  x9m_sims[sim].reg_valid=false;
 }
 for(unsigned i=0;i<X9M_INCOMING;i++){
  unsigned n=0;uint32_t token=0;int rc=x9_md_mipc_message_event(p,sizeof(p),&n,&token);
  if(rc!=1)break;x9m_event(p,n,token);bzero(p,sizeof(p));
 }
 for(x9m_job &j:x9m_jobs)if(j.op){
  if(x9m_has_receipt(j.op)){
   x9m_receipt_retry(j);
   if(j.answered&&!j.token&&!j.follow&&!j.hold&&!j.receipt_pending){bzero(&j,sizeof(j));continue;}
  }
  if(j.hold)continue;
  /* Host guard is not a delivery timeout or cancellation of MD side
   * effects. Reply once before taking a late result, then keep/drain every
   * sent MO/ACK token; no resend or second success for the timed-out txn. */
  if(!j.answered&&now>=j.deadline){x9m_error(j,X9Q_NOT_READY);
   printf("X9 SMS/IMS: operation %u SIM%u timed out; sent token retained, no resend\n",j.op,j.sim+1);}
  if(j.op==X9M_TAG&&j.step==1&&j.token&&
    (j.answered||now>=j.deadline||!x9m_live(j.slot,j.generation)||!x9w_control_ready||!x9m_sims[j.sim].ready))
   x9_md_mipc_message_mark_read_abandon(j.token,j.slot,j.generation);
  if(j.op==X9M_DIAG&&!j.token){
   if(j.answered||now>=j.deadline){x9m_error(j,X9Q_NOT_READY);bzero(&j,sizeof(j));continue;}
   if(!x9m_sims[j.sim].ready||!x9w_control_ready||!x9_md_mipc_message_online())continue;
   /* A queued diagnostic yields to storage work. Only its in-flight
    * token blocks that work until the native confirmation is drained. */
   if(x9m_diag_storage_busy(j.sim,&j))continue;
   x9w_frame f(j.step?0x607:0x60b,1u<<j.sim);
   if(j.step){
    const uint8_t index[2]={1,0};f.byte(0x100,3);f.byte(0x101,5);f.tlv(0x102,index,2);
    /* Diagnostic CMGR observes record shape only; preserve its read tag. */
    f.byte(0x103,1);f.byte(0x104,0);
   }
   j.token=f.submit();if(!j.token)continue;j.follow=false;
   printf("X9 SMS DIAG: SIM%u %s submitted token %llu\n",j.sim+1,j.step?"CMGR":"CPMS",
    (unsigned long long)j.token);
  }
  if(x9m_storage(j.op)&&!j.token){
   if(!j.follow||now>=j.deadline||j.answered||!x9m_live(j.slot,j.generation)){
    x9m_error(j,X9Q_NOT_READY);
    if(j.op==X9M_TAG&&j.step==2){j.hold=true;bzero(j.stored_pdu,sizeof(j.stored_pdu));}
    else {j.follow=false;if(!j.receipt_pending)bzero(&j,sizeof(j));}
    continue; // A lost/expired client may not start a delayed mutation.
   }
   if(x9m_store_blocked(j)||!x9m_sims[j.sim].ready||!x9w_control_ready||!x9_md_mipc_message_online())continue;
   j.token=x9m_store_submit(j);
   if(!j.token)continue;
   j.follow=false;
   printf("X9 SMS: SIM%u stored op %u step %u submitted QMI txn %u\n",j.sim+1,j.op,j.step,x9q_u16(j.req+1));
  }
  if(j.op==X9M_SMSC&&!j.token){
   if(j.answered||now>=j.deadline){x9m_error(j,X9Q_NOT_READY);bzero(&j,sizeof(j));continue;}
   if(x9m_sims[j.sim].ready&&x9w_control_ready&&x9_md_mipc_message_online()){
    x9w_frame f(0x601,1u<<j.sim);f.byte(0x102,1);j.token=f.submit();
   }
   if(!j.token)continue;
  }
  unsigned n=0;
  if(j.op==X9M_ACK){
   if(j.ack_done){
    // Pure host completion only: the native result was already consumed.
    // No MD API, PDU replay or transfer to a replacement client here.
    x9m_error(j,j.ack_error);if(j.answered)bzero(&j,sizeof(j));continue;
   }
   int result=0,rc=x9_md_mipc_message_reply_take((uint32_t)j.token,&result);
   if(rc==1){
    j.ack_done=true;
    // Native take can wait on the sleepable MD lock. Decide an as-yet
    // unanswered result after that wait, not solely at poll entry.
    const uint16_t terminal_error=result||(!j.answered&&!j.ack_reply_pending&&x9q_ms()>=j.deadline)?X9Q_NOT_READY:0;
    x9m_error(j,terminal_error);
    printf("X9 SMS: SIM%u command token %u ACK physical TX result %d; not durable storage proof\n",
     j.sim+1,(unsigned)j.token,result);
    for(x9m_incoming &in:x9m_incoming_sms)if(in.token==j.token){
     if(result)in.held=true;else bzero(&in,sizeof(in));
    }
    if(j.answered)bzero(&j,sizeof(j));continue;
   }
  }else{
   int taken=x9_md_mipc_take(j.token,p,sizeof(p),&n);
   if(taken==-3){
    /* This request never reached writer selection. A TAG step2 still owns
     * the earlier physical mark-read until verification; do not release it. */
    x9m_error(j,X9Q_NOT_READY);j.token=0;
    printf("X9 SMS: SIM%u operation %u step %u locally rejected before TX\n",j.sim+1,j.op,j.step);
    if(j.op==X9M_TAG&&j.step==2){j.hold=true;bzero(j.stored_pdu,sizeof(j.stored_pdu));}
    else if(!j.receipt_pending)bzero(&j,sizeof(j));
    continue;
   }
   if(taken==-2){
    x9m_error(j,X9Q_INTERNAL);
    printf("X9 SMS: SIM%u operation %u oversized CNF fully drained; no partial list\n",j.sim+1,j.op);
    if(j.op==X9M_TAG&&j.step!=0){j.hold=true;bzero(j.stored_pdu,sizeof(j.stored_pdu));}
    else {j.token=0;if(!j.receipt_pending)bzero(&j,sizeof(j));}
    continue;
   }
   if(taken==1){
    /* take may wait for the MD lock: recheck after it returns, before
     * an MO/storage CNF can publish success or start a later TAG stage.
     * Still drain the actual result; a timed-out send is never retried. */
    if((j.op==X9M_SEND||x9m_storage(j.op))&&!j.answered&&x9q_ms()>=j.deadline)
     x9m_error(j,X9Q_NOT_READY);
    x9m_job_done(j,p,n);bzero(p,sizeof(p));
    if(j.hold){bzero(j.req+7,sizeof(j.req)-7);bzero(j.stored_pdu,sizeof(j.stored_pdu));
     printf("X9 SMS: SIM%u ambiguous operation %u CNF; owner retained until boot, payload erased\n",j.sim+1,j.op);}
    else if(j.follow)continue;
    else {j.token=0;if(!j.receipt_pending)bzero(&j,sizeof(j));}continue;
   }
  }
 }
 x9m_ims_poll(p,sizeof(p));
 x9m_ims_feature_poll(p,sizeof(p));
 x9m_factory_poll(p,sizeof(p));
 x9m_init_poll(p,sizeof(p)); // Consume pending readiness INDs before submitting config.
 x9m_mem_status_poll(p,sizeof(p));
 for(x9m_incoming &in:x9m_incoming_sms)if(in.token&&!in.held){
  if(in.delivered){
   if(!in.replying&&in.ack_wait&&now>=in.ack_wait){in.ack_wait=0;
    printf("X9 SMS: SIM%u command token %u host ACK still absent after 30s client-live %u; ownership retained\n",
     in.sim+1,in.token,x9m_live(in.slot,in.generation)?1:0);}
  }else if(in.replying){int result=0;if(x9_md_mipc_message_reply_take(in.token,&result)==1){
   if(result){in.held=true;printf("X9 SMS: SIM%u command token %u discard ACK TX failed; retained without retry\n",in.sim+1,in.token);}
   else bzero(&in,sizeof(in));}}
  else x9m_deliver(in,in.pdu[in.pdu[0]+1]&3);
 }
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(x9m_clients[slot].ims_ind&&now>=x9m_clients[slot].reg_next&&x9w_control_ready&&x9_md_mipc_online()){
  unsigned sim=x9q_clients[slot].sim_slot;bool pending=false;
  for(const x9m_job &j:x9m_jobs)if(j.op==X9M_REG&&j.sim==sim)pending=true;
  x9m_clients[slot].reg_next=now+15000;if(pending)continue;
  uint8_t req[7]={0,0,0,0x20,0,0,0};x9m_job *j=x9m_new(X9M_REG,slot,sim,req,7,true);
  if(j){x9w_frame f(0xa04,1u<<sim);f.byte(0x100,0);j->reg_epoch=x9m_sims[sim].reg_epoch;j->token=f.submit();if(!j->token)bzero(j,sizeof(*j));}
 }
 x9m_diag_poll();
}
