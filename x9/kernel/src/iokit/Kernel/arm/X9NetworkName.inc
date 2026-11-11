/* NAS GetPlmnName (44) -> MTK NW_GET_PROVIDER_NAME (0429/042a).
 * Included after X9WDS for its checked MIPC frame helpers. Single QMI owner.
 * PLJ110 MD: 91345dbc/91345e9e, 913c0f02. Mask selects the physical SIM;
 * LAC=0 asks MD for that SIM's current area (913b8df6), not a made-up LAC.
 * No COPS format changes, DDS mutation, carrier.plist lookup or literal names.
 * Returned UTF-8 is converted to the actual 20H392 PlmnName wire encoding:
 * short/long encoding=1, UTF-16LE. 1f1b0aea4 -> decodeOperatorName(DCS3)
 * -> swapAndDecodeString -> OctetToUcs2Transform (big-endian after swap).
 * The generic NetworkDescriptionEncoding enum is NOT this field's enum.
 */
constexpr unsigned X9N_JOBS=8, X9N_UTF8=127, X9N_UTF16=254;
struct x9n_job {
 bool used,answered; unsigned client,sim,len,rat; uint64_t generation,token,deadline,order;
 char plmn[7]; uint8_t req[X9Q_REQ];
};
static x9n_job x9n_jobs[X9N_JOBS];
static uint64_t x9n_order;
static bool x9n_live(const x9n_job &j){
 return j.client<CBQ_MAX_CLIENTS&&x9q_clients[j.client].generation==j.generation&&
        x9q_clients[j.client].sim_slot==j.sim;
}
static void x9n_error(x9n_job &j,uint16_t error){
 if(!j.answered&&x9n_live(j)){x9q_packet p(j.req,error);p.send(CBQ_PORT_BASE+j.client,X9BB_PORT_BASE+3);}
 j.answered=true;
}
static bool x9n_binding_busy(unsigned client){
 for(const x9n_job &j:x9n_jobs)if(j.used&&!j.answered&&j.client==client)return true;
 return false;
}
static void x9n_del(unsigned client){
 for(x9n_job &j:x9n_jobs)if(j.used&&j.client==client){
  // Sent tokens outlive the host; only the original response can retire them.
  if(j.token)j.answered=true;else bzero(&j,sizeof(j));
 }
}
static bool x9n_string(x9q_view v,unsigned limit,unsigned &length){
 if(!v.p||!v.n||v.n>limit+1)return false;
 length=0;while(length<v.n&&v.p[length])length++;
 if(length==v.n)return false;
 // Fixed-width PLMN/name-pair fields may have zero tail, never hidden text.
 for(unsigned i=length;i<v.n;i++)if(v.p[i])return false;
 return true;
}
static bool x9n_utf16(x9q_view v,uint8_t *out,unsigned &length){
 unsigned size=0;length=0;if(!x9n_string(v,X9N_UTF8,size))return false;
 for(unsigned i=0;i<size;){
  uint32_t c=v.p[i++],min=0;unsigned tail=0;
  if(c<0x80){}
  else if(c>=0xc2&&c<=0xdf){c&=0x1f;tail=1;min=0x80;}
  else if(c>=0xe0&&c<=0xef){c&=0x0f;tail=2;min=0x800;}
  else if(c>=0xf0&&c<=0xf4){c&=7;tail=3;min=0x10000;}
  else return false;
  if(tail>size-i)return false;
  for(unsigned k=0;k<tail;k++){unsigned b=v.p[i++];if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&0x3f);}
  if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff)||c<0x20||(c>=0x7f&&c<=0x9f))return false;
  if(length+(c>=0x10000?4u:2u)>X9N_UTF16)return false; // no truncation
  if(c>=0x10000){c-=0x10000;unsigned hi=0xd800|(c>>10);out[length++]=(uint8_t)hi;out[length++]=(uint8_t)(hi>>8);c=0xdc00|(c&0x3ff);}
  out[length++]=(uint8_t)c;out[length++]=(uint8_t)(c>>8);
 }
 return true;
}
static bool x9n_same_name(x9q_view a,x9q_view b){
 unsigned na=0,nb=0;return x9n_string(a,X9N_UTF8,na)&&x9n_string(b,X9N_UTF8,nb)&&na==nb&&!memcmp(a.p,b.p,na);
}
static void x9n_result(x9n_job &j,const uint8_t *p,unsigned n){
 unsigned result=~0u;
 if(!x9w_wire(p,n)||x9q_u16(p+10)!=0x42a||x9q_u16(p+12)!=(uint16_t)j.token||!x9w_word(p,n,0,result)){
  x9n_error(j,X9Q_INTERNAL);return;
 }
 if(result){printf("X9 NAME: SIM%u txn %u MD result %u (no name update)\n",j.sim+1,x9q_u16(j.req+1),result);x9n_error(j,X9Q_NOT_READY);return;}
 x9q_view plmn=x9w_tlv(p,n,0x8100);unsigned digits=0;
 if(!x9n_string(plmn,6,digits)||digits!=strlen(j.plmn)||memcmp(plmn.p,j.plmn,digits)){
  printf("X9 NAME: SIM%u txn %u mismatched/missing PLMN, discarded\n",j.sim+1,x9q_u16(j.req+1));x9n_error(j,X9Q_INTERNAL);return;
 }
 // New MD provides 127-byte UTF-8 names (810c/d); old fields are 59-byte.
 // Never silently fall back from an explicitly malformed newer field.
 x9q_view short_name=x9w_tlv(p,n,0x810c),long_name=x9w_tlv(p,n,0x810d);
 if(!short_name.p)short_name=x9w_tlv(p,n,0x8102);
 if(!long_name.p)long_name=x9w_tlv(p,n,0x8103);
 uint8_t empty=0,short16[X9N_UTF16],long16[X9N_UTF16];unsigned ns=0,nl=0;
 if(!short_name.p)short_name={&empty,1};if(!long_name.p)long_name={&empty,1};
 if(!x9n_utf16(short_name,short16,ns)||!x9n_utf16(long_name,long16,nl)){
  x9n_error(j,X9Q_INTERNAL);return;
 }
 if(!ns&&!nl){x9n_error(j,X9Q_NOT_READY);return;}
 // Infer a source only when the selected pair exactly matches MD's own
 // separate EONS/NITZ pair; TS25 is NOT QMI's SE13 table. Otherwise UNKNOWN.
 unsigned source=0;
 for(unsigned tag=0x8104;tag<=0x8105&&!source;tag++){
  x9q_view pair=x9w_tlv(p,n,tag);
  if(pair.p&&pair.n==120&&x9n_same_name(short_name,{pair.p,60})&&x9n_same_name(long_name,{pair.p+60,60}))source=tag==0x8104?1:3;
 }
 unsigned ns8=0,nl8=0;x9n_string(short_name,X9N_UTF8,ns8);x9n_string(long_name,X9N_UTF8,nl8);
 if(!source&&ns8==digits&&nl8==digits&&!memcmp(short_name.p,j.plmn,digits)&&!memcmp(long_name.p,j.plmn,digits))source=5;
 uint8_t value[2+4+X9N_UTF16+4+X9N_UTF16];unsigned w=0;
 value[w++]=0;value[w++]=0; // SPN absent: don't label the SIM's provider as registered PLMN.
 value[w++]=1;value[w++]=0;value[w++]=0;value[w++]=(uint8_t)ns;
 memcpy(value+w,short16,ns);w+=ns;
 value[w++]=1;value[w++]=0;value[w++]=0;value[w++]=(uint8_t)nl;
 memcpy(value+w,long16,nl);w+=nl;
 x9q_packet reply(j.req);reply.tlv(0x10,value,w);
 uint8_t src[4]={(uint8_t)source,0,0,0};reply.tlv(0x15,src,4);
 if(!reply.good){x9n_error(j,X9Q_INTERNAL);return;}
 reply.send(CBQ_PORT_BASE+j.client,X9BB_PORT_BASE+3);j.answered=true;
 printf("X9 NAME: SIM%u client 0x%x txn %u PLMN %s RAT %u short/long UTF16 bytes %u/%u source %u result 0 (real MD)\n",
        j.sim+1,CBQ_PORT_BASE+j.client,x9q_u16(j.req+1),j.plmn,j.rat,ns,nl,source);
}
static bool x9n_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned len){
 if(port!=X9BB_PORT_BASE+3||x9q_u16(req+3)!=0x44)return false;
 unsigned client=local-CBQ_PORT_BASE,sim=x9q_clients[client].sim_slot;
 uint16_t error=0;unsigned mcc=0,mnc=0,three=0,rat=0;
 x9q_view plmn=x9q_tlv(req,len,1);
 if(!plmn.p||plmn.n!=4)error=X9Q_ARG;
 else {mcc=x9q_u16(plmn.p);mnc=x9q_u16(plmn.p+2);if(!mcc||mcc>999||mnc>999)error=X9Q_ARG;}
 for(unsigned at=7;at<len&&!error;at+=3+x9q_u16(req+at+1)){
  unsigned tag=req[at],n=x9q_u16(req+at+1);const uint8_t *v=req+at+3;
  if(tag==1)continue;
  if(tag==0x15){if(n!=1)error=X9Q_ARG;else {rat=v[0];if(rat!=4&&rat!=5&&rat!=8&&rat!=12)error=X9Q_UNSUPPORTED;}continue;}
  if(tag!=0x10&&tag!=0x11&&tag!=0x12&&tag!=0x13&&tag!=0x16){error=X9Q_UNSUPPORTED;continue;}
  if(n!=1||v[0]>1){error=X9Q_ARG;continue;}
  if(tag==0x11)three=v[0];
  // No fake SIM-error suppression, static-only table or all-info contract.
  if((tag==0x10||tag==0x13||tag==0x16)&&v[0])error=X9Q_UNSUPPORTED;
 }
 if(!error&&mnc>99&&!three)error=X9Q_ARG;
 if(!error&&sim>=x9_md_sim_count())error=X9Q_ARG;
 if(error){x9q_packet reply(req,error);reply.send(local,port);return true;}
 for(const x9n_job &j:x9n_jobs)if(j.used&&!j.answered&&j.client==client&&
     j.generation==x9q_clients[client].generation&&x9q_u16(j.req+1)==x9q_u16(req+1)){
  if(j.len!=len||memcmp(j.req,req,len)){x9q_packet reply(req,X9Q_ARG);reply.send(local,port);}
  return true; // identical retransmission shares the original completion
 }
 unsigned count=0;for(const x9n_job &j:x9n_jobs)if(j.used&&j.sim==sim)count++;
 x9n_job *job=nullptr;if(count<X9N_JOBS/2)for(x9n_job &j:x9n_jobs)if(!j.used){job=&j;break;}
 if(!job){x9q_packet reply(req,X9Q_BUSY);reply.send(local,port);return true;}
 x9n_job &j=*job;bzero(&j,sizeof(j));j.used=true;j.client=client;j.sim=sim;j.len=len;j.rat=rat;
 j.generation=x9q_clients[client].generation;j.deadline=x9q_ms()+30000;j.order=++x9n_order;memcpy(j.req,req,len);
 snprintf(j.plmn,sizeof(j.plmn),three?"%03u%03u":"%03u%02u",mcc,mnc);
 printf("X9 NAME: queued SIM%u client 0x%x txn %u PLMN %s RAT %u\n",sim+1,local,x9q_u16(req+1),j.plmn,rat);
 return true;
}
static void x9n_poll(){
 uint64_t now=x9q_ms();static uint8_t response[X9_MIPC_FRAME_MAX];
 for(x9n_job &j:x9n_jobs)if(j.used){
  if(!x9n_live(j))j.answered=true;
  if(!j.answered&&now>=j.deadline){x9n_error(j,X9Q_NOT_READY);
   printf("X9 NAME: SIM%u txn %u timeout; token %llu retained until CNF\n",j.sim+1,x9q_u16(j.req+1),j.token);}
  if(!j.token){if(j.answered)bzero(&j,sizeof(j));continue;}
  unsigned n=0;int rc=x9_md_mipc_take(j.token,response,sizeof(response),&n);
  if(rc==0)continue;
  if(!j.answered){
   if(rc!=1||!x9w_control_ready||!x9_md_mipc_online())x9n_error(j,X9Q_NOT_READY);
   else x9n_result(j,response,n);
  }
  bzero(response,sizeof(response));bzero(&j,sizeof(j));
 }
 if(!x9w_control_ready||!x9_md_mipc_online())return;
 // One outstanding wire request per SIM, FIFO within SIM. A late SIM1
 // response cannot block SIM2 or deliver into a reused client generation.
 for(unsigned sim=0;sim<x9_md_sim_count();sim++){
  bool busy=false;x9n_job *next=nullptr;
  for(x9n_job &j:x9n_jobs)if(j.used&&j.sim==sim){
   if(j.token)busy=true;
   else if(!j.answered&&(!next||j.order<next->order))next=&j;
  }
  if(busy||!next)continue;
  x9w_frame f(0x429,1u<<sim);f.tlv(0x100,next->plmn,(unsigned)strlen(next->plmn)+1);
  f.word(0x101,0);f.byte(0x102,1); // current real area + separate name-source pairs
  next->token=f.submit(); // no slot yet: bounded retry until this job's deadline
 }
}
