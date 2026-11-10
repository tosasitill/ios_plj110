/* Unmodified iOS 20H392 QMISMSCommandDriver -> native MTK MIPC.
 * Apple wire contracts, checked in the actual driver: WMS47 indication
 * registration (13=service-ready), 4c BindSubscription (01/u8, 0/1),
 * 5c ready response (11/u8: 1=3GPP, NOT 2), 5d ready IND (10/u8),
 * RawSend20/SendAck37. Standard WMS transfer-route TLVs are byte encoded.
 * MTK: GSM send605/606, SMS host-ready601/602, CMD8601/8603 with
 * ORIGINAL txid ACK8602/8604. Independent from the PDP/DPMAIF state.
 * IMSA exposes only proven registration a04/a05 and 4a01 snapshots.
 * No guessed IMS service capabilities/configuration or CS fallback for an
 * IMS-only send. SIP, IMS PDU8105 and VoLTE are NOT implemented here.
 */
constexpr unsigned X9M_JOBS=8, X9M_INCOMING=16;
enum { X9M_SEND=1, X9M_ACK, X9M_REG, X9M_SMSC };
struct x9m_client {
 bool events,ready_ind,ims_ind,routes,status_reports;
 uint8_t route[6],route_mask; uint64_t reg_next;
};
static x9m_client x9m_clients[CBQ_MAX_CLIENTS];
struct x9m_job {
 unsigned op,slot,sim,len; uint64_t generation,token,deadline;
 bool answered,background,hold; uint8_t req[X9Q_REQ];
};
static x9m_job x9m_jobs[X9M_JOBS];
struct x9m_incoming {
 uint32_t token; unsigned sim,len,slot; uint64_t generation;
 bool delivered,replying; uint8_t pdu[255];
};
static x9m_incoming x9m_incoming_sms[X9M_INCOMING];
struct x9m_sim { bool wanted,ready,reg_valid; unsigned reg; uint64_t sampled; };
static x9m_sim x9m_sims[2];
static unsigned x9m_init_stage;
static uint64_t x9m_init_token,x9m_init_deadline;
static bool x9m_init_hold;
static bool x9m_live(unsigned slot,uint64_t generation){
 return slot<CBQ_MAX_CLIENTS&&x9q_clients[slot].generation==generation;
}
static void x9m_number(x9q_packet &p,uint8_t tag,uint32_t value,unsigned bytes=4){
 uint8_t v[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
 p.tlv(tag,v,bytes);
}
static void x9m_reply(unsigned slot,unsigned service,const uint8_t *req,uint16_t err){
 x9q_packet p(req,err);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+service);
}
static unsigned x9m_service(const x9m_job &j){return j.op==X9M_REG?0x21:5;}
static void x9m_error(x9m_job &j,uint16_t err){
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
  return &j;
 }
 return nullptr;
}
static bool x9m_pdu(const uint8_t *p,unsigned n,unsigned mti){
 return p&&n>=3&&n<=255&&p[0]<=12&&p[0]+2u<n&&(p[p[0]+1]&3)==mti;
}
static unsigned x9m_class(const uint8_t *p,unsigned n){
 unsigned start=p[0]+1;
 if(n-start<3)return 6;
 unsigned addr=p[start+1];
 /* TP-OA alphanumeric length is in semi-octets too (23.040 9.1.2.5). */
 unsigned dcs=start+4+(addr+1)/2;
 if(addr>20||dcs>=n)return 6;
 unsigned v=p[dcs];
 return (((v&0xc0)==0&&(v&0x10))||(v&0xf0)==0xf0)?v&3:4;
}
static void x9m_init_poll(uint8_t *response,unsigned room){
 bool wanted=false;for(unsigned sim=0;sim<x9_md_sim_count();sim++)wanted|=x9m_sims[sim].wanted;
 if(!wanted||!x9w_control_ready||!x9_md_mipc_online())return;
 if(x9m_init_token){
  unsigned n=0;int rc=x9_md_mipc_take(x9m_init_token,response,room,&n);
  if(rc!=1){
   if(!x9m_init_hold&&x9q_ms()>=x9m_init_deadline){x9m_init_hold=true;
    printf("X9 SMS: init stage %u timeout; token retained, no retry\n",x9m_init_stage);}
   return;
  }
  x9m_init_token=0;unsigned result=1;
  unsigned id=x9m_init_stage<=2?0x30c:(x9m_init_stage-3)%3==2?0x602:0x308;
  bool good=x9w_wire(response,n)&&x9q_u16(response+10)==id&&x9w_word(response,n,0,result)&&!result;
  bzero(response,room);
  if(!good){x9m_init_hold=true;printf("X9 SMS: init stage %u rejected; no synthetic ready\n",x9m_init_stage);return;}
  x9m_init_hold=false;
  if(x9m_init_stage>2&&(x9m_init_stage-3)%3==2){
   unsigned sim=(x9m_init_stage-3)/3;x9m_sims[sim].ready=true;x9m_ready_ind(sim);
   printf("X9 SMS: SIM%u real host-ready CNF; GSM command handlers installed\n",sim+1);
  }
 }
 if(x9m_init_hold||x9m_init_stage==2+3*x9_md_sim_count())return;
 unsigned stage=x9m_init_stage+1,sim=stage>2?(stage-3)/3:0,step=stage>2?(stage-3)%3:0;
 x9w_frame f(stage<=2?0x30b:step==2?0x601:0x307,stage<=2?0xff:1u<<sim);
 if(stage<=2||step<2){
  unsigned id=stage<=2?(stage==1?0x8601:0x8603):(step==0?0x4601:0x4a01);
  uint8_t v[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,v,2);
 }else{f.byte(0x100,0);f.byte(0x105,1);f.byte(0x106,2);}
 uint64_t token=f.submit();
 if(token){x9m_init_stage=stage;x9m_init_token=token;x9m_init_deadline=x9q_ms()+300000;}
}
static void x9m_deliver(x9m_incoming &in,unsigned mti){
 unsigned cls=mti==2?4:x9m_class(in.pdu,in.len);
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
  p.send(CBQ_PORT_BASE+i,X9BB_PORT_BASE+5,true,1);bzero(v,sizeof(v));bzero(p.p,sizeof(p.p));
  in.delivered=true;in.slot=i;in.generation=x9q_clients[i].generation;bzero(in.pdu,sizeof(in.pdu));
  printf("X9 SMS: incoming GSM %s SIM%u command token %u; waiting for host ACK\n",mti==2?"status report":"message",in.sim+1,in.token);
  return;
 }
}
static void x9m_event(uint8_t *p,unsigned n,uint32_t token){
 if(!x9w_wire(p,n))return; /* Bad commands stay owned, never success-ACKed. */
 unsigned mask=p[8]&((1u<<x9_md_sim_count())-1),id=x9q_u16(p+10);
 if(id==0x4a01){
  /* URC101 is CIREGU 0/1, NOT the CIREG query's 0..4 enumeration. */
  unsigned state;if(!x9w_byte(p,n,0x101,state)||state>1)return;
  for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(mask&(1u<<sim))x9m_reg(sim,state?2:0);
  return;
 }
 if(!token)return; // 4601 is readiness notification, not proof of 601 success.
 if(mask!=1&&mask!=2)return;
 x9q_view v={nullptr,0};unsigned mti=id==0x8601?0:2;
 if(id==0x8601){
  unsigned type;if(!x9w_byte(p,n,0x100,type)||type!=3){
   static unsigned once;if(!once++)printf("X9 SMS: non-CS command retained; IMS8105 encoding/ACK not yet mapped\n");return;}
  v=x9w_tlv(p,n,0x8104);
  if(!v.p||v.n<11)return;
  unsigned full=1u+v.p[8]+v.p[2];
  if(full>v.n-8||!x9m_pdu(v.p+8,full,0))return;
  v={v.p+8,full};
 }else if(id==0x8603){v=x9w_tlv(p,n,0x8100);if(!x9m_pdu(v.p,v.n,2))return;}
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
  x9q_view v=x9q_tlv(req,len,0x10);
  if(len!=11||!v.p||v.n!=1||v.p[0]>1)err=X9Q_ARG;else c.events=v.p[0];
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
  x9q_packet p(req);x9m_number(p,0x11,x9m_sims[sim].ready&&x9_md_mipc_online()?1:0,1);p.send(local,port);return true;
 }else if(service==5&&id==0x30&&len==7){
  if(!x9m_sims[sim].ready)err=X9Q_NOT_READY;
  else{x9q_packet p(req);x9m_number(p,1,1,1);p.send(local,port);return true;}
 }else if(service==5&&id==0x20){
  x9q_view raw=x9q_tlv(req,len,1),ims=x9q_tlv(req,len,0x13);
  if(!raw.p||raw.n<6||raw.p[0]!=6||x9q_u16(raw.p+1)!=raw.n-3||!x9m_pdu(raw.p+3,raw.n-3,1))err=X9Q_ARG;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1))if(req[at]!=1&&
   !(req[at]==0x13&&x9q_u16(req+at+1)==1&&req[at+3]==0)&&
   !(req[at]==0x14&&x9q_u16(req+at+1)==1&&req[at+3]==0)&&
   /* LinkTimer is a dedicated-channel reuse hint, not the SMS delivery
    * timeout. Leave MTK's native link lifetime unchanged; never resend. */
   !(req[at]==0x12&&x9q_u16(req+at+1)==1&&req[at+3]<=1))err=X9Q_UNSUPPORTED;
  if(ims.p&&(ims.n!=1||ims.p[0]))err=X9Q_UNSUPPORTED;
  if(!err&&(!x9m_sims[sim].ready||!x9_md_mipc_online()))err=X9Q_NOT_READY;
  for(const x9m_job &j:x9m_jobs)if(j.op==X9M_SEND&&j.sim==sim)err=X9Q_BUSY;
  if(!err){
   x9m_job *j=x9m_new(X9M_SEND,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else{
    x9w_frame f(0x605,1u<<sim);f.byte(0x100,0);f.tlv(0x8101,raw.p+3,raw.n-3);
    f.byte(0x102,(uint8_t)(raw.n-4-raw.p[3]));j->token=f.submit();
    if(j->token)return true;bzero(j,sizeof(*j));err=X9Q_BUSY;
   }
  }
 }else if(service==5&&id==0x37){
  x9q_view info=x9q_tlv(req,len,1),failure=x9q_tlv(req,len,0x11),ims=x9q_tlv(req,len,0x12);
  if(!info.p||info.n!=6||info.p[4]!=1||info.p[5]>1)err=X9Q_ARG;
  if(failure.p&&(failure.n!=2||info.n!=6||info.p[5]))err=X9Q_ARG;
  if(ims.p&&(ims.n!=1||ims.p[0]))err=X9Q_UNSUPPORTED;
  for(unsigned at=7;at<len;at+=3+x9q_u16(req+at+1))if(req[at]!=1&&req[at]!=0x11&&req[at]!=0x12)err=X9Q_UNSUPPORTED;
  x9m_incoming *owner=nullptr;
  if(!err)for(x9m_incoming &in:x9m_incoming_sms)if(in.token==x9q_u32(info.p)&&in.delivered&&
   in.sim==sim&&in.slot==slot&&in.generation==x9q_clients[slot].generation)owner=&in;
  if(!err&&(!owner||owner->replying))err=X9Q_ARG;
  if(!err){
   x9m_job *j=x9m_new(X9M_ACK,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else{
    int rc=x9_md_mipc_message_reply(owner->token,info.p[5],failure.p?failure.p[1]:0xff);
    if(rc==1){j->token=owner->token;owner->replying=true;return true;}
    bzero(j,sizeof(*j));err=rc<0?X9Q_ARG:X9Q_NOT_READY;
   }
  }
 }else if((service==0x21&&id==0x20)||(service==5&&id==0x34)){
  if(len!=7)err=X9Q_ARG;
  else if(!x9w_control_ready||!x9_md_mipc_online())err=X9Q_NOT_READY;
  else{
   unsigned op=service==0x21?X9M_REG:X9M_SMSC;
   x9m_job *j=x9m_new(op,slot,sim,req,len);
   if(!j)err=X9Q_BUSY;
   else{x9w_frame f(op==X9M_REG?0xa04:0x601,1u<<sim);f.byte(op==X9M_REG?0x100:0x102,op==X9M_REG?0:1);
    j->token=f.submit();if(j->token)return true;bzero(j,sizeof(*j));err=X9Q_BUSY;}
  }
 }else err=X9Q_UNSUPPORTED;
 x9m_reply(slot,service,req,err);return true;
}
static void x9m_job_done(x9m_job &j,const uint8_t *p,unsigned n){
 unsigned result=1;
 bool known=x9w_wire(p,n)&&x9q_u16(p+10)==(j.op==X9M_SEND?0x606:j.op==X9M_REG?0xa05:0x602)&&
  x9w_word(p,n,0,result);
 if(!known||result){j.hold=j.op==X9M_SEND&&!known;x9m_error(j,X9Q_INTERNAL);return;}
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
  if(state==1){x9m_sims[j.sim].reg_valid=false;x9m_error(j,X9Q_NOT_READY);return;}
  unsigned reg=state==2?2:0;x9m_reg(j.sim,reg);x9m_number(response,0x12,reg);
 }else{
  x9q_view text=x9w_tlv(p,n,0x8101);
  if(!text.p||text.n<2||text.n>23||text.p[text.n-1]||strnlen((const char *)text.p,text.n)!=text.n-1){x9m_error(j,X9Q_INTERNAL);return;}
  bool intl=text.p[0]=='+';unsigned start=intl?1:0,k=text.n-1-start;
  if(!k||k>20){x9m_error(j,X9Q_INTERNAL);return;}
  for(unsigned i=start;i<text.n-1;i++)if(text.p[i]<'0'||text.p[i]>'9'){x9m_error(j,X9Q_INTERNAL);return;}
  uint8_t value[24]={'1',(uint8_t)(intl?'4':'2'),(uint8_t)(intl?'5':'9'),(uint8_t)k};
  memcpy(value+4,text.p+start,k);response.tlv(1,value,k+4);bzero(value,sizeof(value));
 }
 if(!j.answered&&!j.background&&x9m_live(j.slot,j.generation))response.send(CBQ_PORT_BASE+j.slot,X9BB_PORT_BASE+x9m_service(j));
 bzero(response.p,sizeof(response.p));
}
static void x9m_del(unsigned slot){
 bzero(&x9m_clients[slot],sizeof(x9m_clients[slot]));
 /* Sent SMS/ACK owners survive deletion and drain their late result. */
 for(x9m_job &j:x9m_jobs)if(j.op&&j.slot==slot)j.answered=true;
 /* No successful auto-ACK on client loss. Commands remain bound to the old
  * generation, so a reused port/SIM cannot acknowledge someone else's SMS. */
}
static void x9m_poll(){
 static uint8_t p[X9_MIPC_FRAME_MAX];uint64_t now=x9q_ms();
 x9m_init_poll(p,sizeof(p));
 if(!x9_md_mipc_online())for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  if(x9m_sims[sim].ready){x9m_sims[sim].ready=false;x9m_ready_ind(sim);}
  x9m_sims[sim].reg_valid=false;
 }
 for(x9m_job &j:x9m_jobs)if(j.op){
  if(j.hold)continue;
  unsigned n=0;
  if(j.op==X9M_ACK){
   int result=0,rc=x9_md_mipc_message_reply_take((uint32_t)j.token,&result);
   if(rc==1){
    x9m_error(j,result?X9Q_NOT_READY:0);
    for(x9m_incoming &in:x9m_incoming_sms)if(in.token==j.token)bzero(&in,sizeof(in));
    bzero(&j,sizeof(j));continue;
   }
  }else if(x9_md_mipc_take(j.token,p,sizeof(p),&n)==1){
   x9m_job_done(j,p,n);bzero(p,sizeof(p));
   if(j.hold){bzero(j.req+7,sizeof(j.req)-7);
    printf("X9 SMS: SIM%u ambiguous send CNF; no new send until boot, payload erased\n",j.sim+1);}
   else bzero(&j,sizeof(j));continue;
  }
  if(!j.answered&&now>=j.deadline){x9m_error(j,X9Q_NOT_READY);
   printf("X9 SMS/IMS: operation %u SIM%u timed out; sent token retained, no resend\n",j.op,j.sim+1);}
 }
 for(unsigned i=0;i<X9M_INCOMING;i++){
  unsigned n=0;uint32_t token=0;int rc=x9_md_mipc_message_event(p,sizeof(p),&n,&token);
  if(rc!=1)break;x9m_event(p,n,token);bzero(p,sizeof(p));
 }
 for(x9m_incoming &in:x9m_incoming_sms)if(in.token&&!in.delivered){
  if(in.replying){int result=0;if(x9_md_mipc_message_reply_take(in.token,&result)==1)bzero(&in,sizeof(in));}
  else x9m_deliver(in,in.pdu[in.pdu[0]+1]&3);
 }
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(x9m_clients[slot].ims_ind&&now>=x9m_clients[slot].reg_next&&x9w_control_ready&&x9_md_mipc_online()){
  unsigned sim=x9q_clients[slot].sim_slot;bool pending=false;
  for(const x9m_job &j:x9m_jobs)if(j.op==X9M_REG&&j.sim==sim)pending=true;
  x9m_clients[slot].reg_next=now+15000;if(pending)continue;
  uint8_t req[7]={0,0,0,0x20,0,0,0};x9m_job *j=x9m_new(X9M_REG,slot,sim,req,7,true);
  if(j){x9w_frame f(0xa04,1u<<sim);f.byte(0x100,0);j->token=f.submit();if(!j->token)bzero(j,sizeof(*j));}
 }
}
