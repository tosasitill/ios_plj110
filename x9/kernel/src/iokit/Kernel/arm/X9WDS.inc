/* Native WDS dialler, single QMI/DPMAIF owner (NOT the AT job queue).
 * Runtime: libmtkmipc-ril + libmipc, ttyCMIPC1/RX216/TX217.
 * OPEN301/register307 -> GET_PDP_CID219 -> CONFIG211/CNF -> RETRY21b/CNF
 * -> no-auth ACT201 (PDP3, REUSE=0, fallback=0x7f, captured URSP descriptor)
 * -> GET_CALL205 + actual EIF4302/IFUP -> bind.
 * Native decodeDataCallProfile reads ID100 separately from IF125;
 * ccci_dpmaif_com writes channel_id=ccmni_idx. Never use CID-1 as a route.
 * Explicit, RAM-profile or network-default APN, no-auth only. The MD call is
 * requested as IPv4v6; when it returns PDP3 the v6 client's Start attaches
 * to that same CID/channel (x9w_v6_*).
 * Old MIPC headers differ from this RIL at tags128..12b: TRANS_ID is NOT a
 * subnet mask. Missing gateway/netmask is omitted, not invented. APN/IP stays
 * in RAM, never diagnostics. Sent activation survives host timeout/DEL_CLIENT
 * until its late CNF and cleanup. The ACT reuse flag does not prove a fresh CID:
 * saved P77 can reuse same-APN calls, and normal DEACT clears the MD user's
 * entire reference slot. User/CID lifetime still needs qualification before
 * treating this dialler as ready for a new boot candidate. No cancel/reset.
 */
struct x9w_client {
 uint8_t mux, family, sim_slot, stats_interval; bool starting,bearer_report,last_call_bytes_valid;
 // IPv6 attachment to the dual-stack bearer on this mux (no MD call of its own).
 bool v6_pending,v6_active;uint32_t v6_handle,v6_len;uint64_t v6_lifetime,v6_deadline;uint8_t v6_req[64];
 uint32_t stats_mask,last_rat;uint64_t report_next;
 x9d_stats baseline,last_call;
};
static x9w_client x9w_clients[CBQ_MAX_CLIENTS];
enum { X9W_DISC=1, X9W_ACT, X9W_CHECK, X9W_DEACT, X9W_IFWAIT, X9W_CONFIG, X9W_RETRY_RESET,
 X9W_RETRY_QUERY, X9W_RETRY_WAIT };
// AP accounting outlives the control bearer and client.204/4202 may consume
// control state, but cannot erase a published payload's original ledger.
struct x9w_retirement { uint64_t lifetime,generation; unsigned slot; bool pending; };
static x9w_retirement x9w_retirements[CBP_INTERFACES];
static bool x9w_retiring_client(unsigned slot,uint64_t generation){
 for(const x9w_retirement &r:x9w_retirements)if(r.pending&&r.slot==slot&&r.generation==generation)return true;
 return false;
}
static bool x9w_accounting_pending(){
 for(const x9w_retirement &r:x9w_retirements)if(r.pending)return true;
 return x9d_retiring(); // AP-only; never proves MD release or RX drain
}
static void x9w_retire_poll(){
 for(unsigned i=0;i<CBP_INTERFACES;i++){x9w_retirement &r=x9w_retirements[i];if(!r.pending)continue;
  x9d_stats stats={};if(!x9d_retire_finish(i+1,r.lifetime,stats))continue;
  if(r.slot<CBQ_MAX_CLIENTS&&x9q_clients[r.slot].generation==r.generation){
   x9w_clients[r.slot].last_call=stats;x9w_clients[r.slot].last_call_bytes_valid=true;
  }
  bzero(&r,sizeof(r));
 }
}
static uint64_t x9w_if_serial,x9w_init_token,x9w_init_deadline;
static unsigned x9w_init_stage;
static bool x9w_control_ready,x9w_session_ready,x9w_init_hold;
// app3 belongs to Android's ImsRtpRedunDataController, NOT the internet PDN.
// Optional diagnostic only; never a WDS readiness condition.
static bool x9w_packet_route_enabled,x9w_packet_route_pending,x9w_ifup_pending[2];
struct x9w_frame {
 uint8_t p[1024];unsigned n;bool good; // ACT carries a 672-byte URSP traffic descriptor
 x9w_frame(unsigned id,unsigned mask=1):n(16),good(true){
  bzero(p,sizeof(p));p[0]=0x84;p[1]=0x19;p[2]=0x54;p[3]=0x24;p[8]=(uint8_t)mask;
  p[10]=(uint8_t)id;p[11]=(uint8_t)(id>>8);
 }
 ~x9w_frame(){bzero(p,sizeof(p));}
 void tlv(unsigned tag,const void *v,unsigned len){
  unsigned size=(len+11)&~7u;
  if(!len||size>sizeof(p)-n){good=false;return;}
  p[n]=(uint8_t)tag;p[n+1]=(uint8_t)(tag>>8);p[n+2]=(uint8_t)len;p[n+3]=(uint8_t)(len>>8);
  memcpy(p+n+4,v,len);n+=size;
 }
 void byte(unsigned tag,uint8_t v){tlv(tag,&v,1);}
 void word(unsigned tag,uint32_t v){uint8_t a[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};tlv(tag,a,4);}
 uint64_t submit(){p[14]=(uint8_t)(n-16);p[15]=(uint8_t)((n-16)>>8);return good?x9_md_mipc_submit(p,n):0;}
};
static bool x9w_wire(const uint8_t *p,unsigned n,unsigned mask=1){
 /* 159.txt: OPEN_CNF arrives with flag byte 9 = 0x02. The device's libmipc
  * RX path ignores byte 9 and the SIM mask of CNFs (mipc_init+0x4c0). INDs
  * are routed separately by mask; never bind SIM2 by a reused SIM1 call ID. */
 (void)mask;
 if(n<16||n>X9_MIPC_FRAME_MAX||x9q_u32(p)!=0x24541984||x9q_u16(p+14)!=n-16)return false;
 unsigned at=16;
 while(at<n){
  if(n-at<4)return false;
  unsigned tag=x9q_u16(p+at)&0x7fff,k=x9q_u16(p+at+2),size=k?((k+11)&~7u):16;
  if(size>n-at)return false;
  for(unsigned prev=16;prev<at;){
   if((x9q_u16(p+prev)&0x7fff)==tag)return false;
   unsigned old=x9q_u16(p+prev+2);prev+=old?((old+11)&~7u):16;
  }
  at+=size;
 }
 return at==n;
}
static x9q_view x9w_tlv(const uint8_t *p,unsigned n,unsigned tag){
 for(unsigned at=16;at<n;){unsigned k=x9q_u16(p+at+2);
  if((x9q_u16(p+at)&0x7fff)==(tag&0x7fff))return {p+at+4,k};at+=k?((k+11)&~7u):16;
 }return {nullptr,0};
}
static bool x9w_byte(const uint8_t *p,unsigned n,unsigned tag,unsigned &v){
 x9q_view t=x9w_tlv(p,n,tag);if(!t.p||t.n!=1)return false;v=t.p[0];return true;
}
static bool x9w_word(const uint8_t *p,unsigned n,unsigned tag,unsigned &v){
 x9q_view t=x9w_tlv(p,n,tag);if(!t.p||t.n!=4)return false;v=x9q_u32(t.p);return true;
}
#include "X9WDSResult.inc"
struct x9w_bearer {
 bool used,owned,active,abandoned,pending,hold,gone;
 uint16_t end_type,end_verbose; // first known disconnect cause; 0 = local failure
 // MD already took this route down, or the latest GET205 could not confirm
 // the CID still carries our channel/trans_id. Saved P77 L5 DEACT handler
 // 913d8396->913d842a sends B238 to D2CM even when the CID has no L5 node,
 // so a stale 203 could end a foreign PDN (IMS/attach) now owning that CID.
 bool md_down;
 unsigned slot,op,min_cid,max_cid,channel,mtu,dns_count;
 uint64_t generation,token,deadline,reply_deadline,next,if_floor,result_epoch,route_lifetime;
 // MTU from ACT 0x123; 0 = network gave none (MD adds 0x123 only when its
 // +0x2c8 flag is set, 913277822). mtu is the effective route MTU.
 unsigned md_mtu;
 uint32_t handle,len,trans_id,failure_result;
 uint8_t cid,sim_slot,req[X9Q_REQ],ip[4],dns[2][4];
 char apn[101];
 // Raw Start selection (APN14 bytes, profile31) for matching the v6 Start.
 uint8_t start_apn[101],start_apn_n;int start_profile;
 x9wr_identity query; x9wr_get get_result; x9wr_settings runtime;
};
static x9w_bearer x9w_bearers[CBP_INTERFACES];
static char x9w_last_dnn[2][101]; // APN the MD reported in the last ACT CNF (RAM only)
// Retain the latest internet APN's advice plus a per-SIM local churn floor.
// Every later Start still queries the MD, including a changed/uncached APN;
// a cached network timer is never applied to a different APN. Epoch tags
// prevent either a delay or a reset grant leaking across cards.
struct x9w_retry_policy { uint64_t epoch,after,floor; bool valid; char apn[101]; };
static x9w_retry_policy x9w_retries[2];
static uint64_t x9w_reset_epoch[2];
static bool x9w_retry_same(const x9w_bearer &b){
 const x9w_retry_policy &r=x9w_retries[b.sim_slot];
 return r.valid&&r.epoch==b.result_epoch&&!strcasecmp(r.apn,b.apn);
}
static bool x9w_netagent_req; // boot-arg x9_eif_req=1: send EIF ifst/ipupdate + statistics (not done by Android)
static uint32_t x9w_next_handle;
// Latest MD interface state, not Android's local netdev state. The floor and
// never-reused DPMAIF channel prevent accepting a pre-ACT/previous bearer event.
struct x9w_interface { uint64_t serial; uint32_t trans_id,mtu; unsigned cmd; bool valid; uint8_t ip[4]; x9wr_ifup facts; };
static x9w_interface x9w_interfaces[2][20];
static void x9w_init_poll(uint8_t *response,unsigned room){
 bool transport_ok=x9_md_mipc_online();
 if(!transport_ok){
  x9w_control_ready=x9w_session_ready=false;
  // Initial offline wait may precede OPEN. Once this handshake has started,
  // transport loss can only drain its original token, never restart/advance.
  if(x9w_init_stage||x9w_init_token)x9w_init_hold=true;
 }
 if(x9w_init_token){
  // The original stage/SIM mask stays fixed while its token exists. Detect
  // timeout before take: even an already-arrived late CNF cannot grant ready.
  unsigned stage=x9w_init_stage;
  if(!x9w_init_hold&&x9q_ms()>=x9w_init_deadline){
   x9w_init_hold=true;printf("X9 MIPC: handshake stage %u timeout; drain original token, no retry until boot\n",stage);
  }
  unsigned n=0;int ready=x9_md_mipc_take(x9w_init_token,response,room,&n);
  if(ready!=1)return; // keep token on pending/error; no cancel or replacement
  x9w_init_token=0;unsigned result=2,version=0,mask=stage==1?0xff:1u<<((stage-2)/3);
  bool good=x9w_wire(response,n,mask)&&x9q_u16(response+10)==(stage==1?0x302:0x308);
  if(stage==1){
   // libmipc mipc_init (0x133a0) only LOGS VERSION100 (default -1) and
   // TIMEOUT101 (default 300000) and succeeds once the CNF arrives. 160.txt:
   // this MD answers result 0, VERSION 0, TIMEOUT 300000. Keep a non-zero
   // result as a failure; the version is recorded, not enforced.
   x9q_view r=good?x9w_tlv(response,n,0):x9q_view{nullptr,0};
   if(good&&x9w_word(response,n,0x100,version))printf("X9 MIPC: OPEN_CNF version %u (libmipc does not check it)\n",version);
   good=good&&(!r.p||(r.n==4&&x9q_u32(r.p)==0));
  }else good=good&&x9w_word(response,n,0,result)&&result==0;
  if(!good){
   /* Handshake CNFs carry only version/result/timeout words: log tags and
    * 4-byte values so a version mismatch is visible. */
   char t[160];unsigned w=0;t[0]=0;
   for(unsigned at=16;at+4<=n&&w+24<sizeof(t);){unsigned tag=x9q_u16(response+at),k=x9q_u16(response+at+2);
    w+=(unsigned)snprintf(t+w,sizeof(t)-w,k==4&&at+8<=n?" %04x=%u":" %04x/%u",tag,k==4&&at+8<=n?x9q_u32(response+at+4):k);
    at+=k?((k+11)&~7u):16;}
   printf("X9 MIPC: handshake stage %u reply id 0x%04x len %u tlvs%s\n",stage,n>=12?x9q_u16(response+10):0,n,w?t:" none");
  }
  bzero(response,room);
  if(x9w_init_hold||!transport_ok){
   printf("X9 MIPC: held handshake stage %u original CNF drained (%s); no stage advance or ready grant\n",stage,good?"valid":"rejected/malformed");return;
  }
  if(!good){x9w_init_hold=true;printf("X9 MIPC: handshake stage %u rejected/malformed; no dial\n",stage);return;}
  printf("X9 MIPC: handshake stage %u confirmed\n",stage);
  if(stage==1)x9w_control_ready=true; // AT queries need OPEN, not PDP event registration
  if(stage==1+3*x9_md_sim_count()){
   {int v=0;PE_parse_boot_argn("x9_eif_req",&v,sizeof(v));x9w_netagent_req=v!=0;}
   {int v=0;PE_parse_boot_argn("x9_ims_proute",&v,sizeof(v));x9w_packet_route_enabled=v==1;}
   x9w_packet_route_pending=x9w_packet_route_enabled;
   x9w_session_ready=true;printf("X9 MIPC: v2 OPEN + DEACT/EIF subscriptions confirmed for %u SIM(s); CID ownership unqualified\n",x9_md_sim_count());return;}
 }
 if(!transport_ok||x9w_init_hold||x9w_session_ready)return;
 unsigned stage=x9w_init_stage+1;
 x9w_frame f(stage==1?0x301:0x307,stage==1?0xff:1u<<((stage-2)/3));
 if(stage==1){static const char name[]="X9WDS";f.word(0x100,2);f.tlv(0x101,name,sizeof(name));f.byte(0x102,1);}
 // Per SIM: DEACT 4202, EIF 4302 and DATA_IFUP_PACKET_ROUTE_IND 4220 (the
 // MD asks the AP for the packet route; subsys_daemon registers and answers).
 else{static const unsigned ids[3]={0x4202,0x4302,0x4220};unsigned id=ids[(stage-2)%3];
  uint8_t bytes[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,bytes,2);}
 uint64_t token=f.submit();if(token){x9w_init_token=token;x9w_init_stage=stage;x9w_init_deadline=x9q_ms()+30000;}
}
static bool x9w_apn(const uint8_t *p,unsigned n){
 if(!n)return true; // Empty APN is a real network-default request, not a guessed APN.
 if(!p||n>100)return false;unsigned label=0;
 for(unsigned i=0;i<n;i++){uint8_t c=p[i];
  if(c=='.'){if(!label||p[i-1]=='-')return false;label=0;continue;}
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;
  if((!label&&c=='-')||++label>63)return false;
 }return label&&p[n-1]!='-';
}
#include "X9AttachAPN.inc"
#include "X9WDSProfiles.inc"
static unsigned x9w_args(const uint8_t *p,unsigned n,unsigned family){
 if(family!=4)return X9Q_UNSUPPORTED; // native IPv6 uses netagent/SLAAC, no guessed prefix
 x9q_view apn=x9q_tlv(p,n,0x14);if(!x9w_apn(apn.p,apn.n))return X9Q_ARG;
 for(unsigned at=7;at<n;at+=3+x9q_u16(p+at+1)){
  unsigned k=x9q_u16(p+at+1);const uint8_t *v=p+at+3;
  switch(p[at]){
  case 0x14:break;
  case 0x19:if(k!=1||v[0]!=family)return X9Q_ARG;break;
  case 0x30:if(k!=1||(v[0]!=0&&v[0]!=1))return X9Q_UNSUPPORTED;break; // automatic or 3GPP
  case 0x31:case 0x32:if(k!=1||!v[0])return X9Q_ARG;break;
  // 20H392 setDataExtensionParameters always adds CallType35/u8:
  // 1 = Embedded NAI, 0 = Tethering NAI. This is NOT an IMS/emergency
  // selector or an MTK APN type. Embedded Internet uses the existing
  // explicit APN/no-auth ACT201 builder (MTK ApnType102/u32=1).
  case 0x35:if(k!=1)return X9Q_ARG;if(v[0]!=1)return X9Q_UNSUPPORTED;break;
  case 0x3e:if(k!=8||x9q_u32(v)!=1||x9q_u32(v+4))return X9Q_UNSUPPORTED;break; // Internet APN type
  case 0x16:case 0x33:if(k!=1||v[0])return X9Q_UNSUPPORTED;break;
  default:
   printf("X9 WDS: Start unsupported request tag %02x length %u; no dial\n",p[at],k);
   return X9Q_UNSUPPORTED;
  }
 }return 0;
}
static bool x9w_live(const x9w_bearer &b){return b.slot<CBQ_MAX_CLIENTS&&x9q_clients[b.slot].generation==b.generation;}
// Requests can run before the next bearer poll observes a changed card pair.
// A retained handle or accounting ledger never grants current-call success.
static bool x9w_data_ready(const x9w_bearer &b){
 return b.used&&b.active&&!b.abandoned&&!b.hold&&b.result_epoch==x9a_epoch&&
  x9w_live(b)&&x9d_ready()&&x9_md_mipc_online();
}
// QMI WDS call end reasons (libqmi qmi-enums-wds.h). The 20H392 driver
// names the internal block 201.. as INTERNAL_ERROR, CALL_ENDED, ..., 206
// NW_INITIATED_TERMINATION, 208 IPV4_CALL_DISALLOWED, 210 IPV6_DISALLOWED and
// logs Start failures as reason/verboseReasonType/verboseReason. A bare
// error gives it no reason to tell a v4-only PDN apart from a failed call.
enum : uint16_t { X9W_END_UNSPECIFIED=1,X9W_END_CLIENT=2 };
enum : uint16_t { X9W_VERBOSE_INTERNAL=2,X9W_VERBOSE_CM=3 };
enum : uint16_t { X9W_INTERNAL_ERROR=201,X9W_INTERNAL_NW_TERMINATION=206,
 X9W_INTERNAL_IPV6_DISALLOWED=210,X9W_CM_CLIENT_END=2000 };
static void x9w_end_reason(x9q_packet &p,uint16_t reason,uint16_t type,uint16_t verbose){
 uint8_t r[2]={(uint8_t)reason,(uint8_t)(reason>>8)};p.tlv(0x10,r,2);
 uint8_t v[4]={(uint8_t)type,(uint8_t)(type>>8),(uint8_t)verbose,(uint8_t)(verbose>>8)};p.tlv(0x11,v,4);
}
static void x9w_set_end(x9w_bearer &b,uint16_t type,uint16_t verbose){
 if(!b.end_type){b.end_type=type;b.end_verbose=verbose;} // first cause wins
}
static void x9w_integer(x9q_packet &p,unsigned tag,uint64_t value,unsigned bytes){
 uint8_t le[8];for(unsigned i=0;i<bytes;i++){le[i]=(uint8_t)value;value>>=8;}p.tlv((uint8_t)tag,le,bytes);
}
static void x9w_statistics(x9q_packet &p,const x9d_stats &s,const x9d_stats &base,unsigned mask){
 // qmi-service-wds.json: packet counters u32; byte totals u64, LE.
 const uint64_t values[]={s.tx_packets,s.rx_packets,s.tx_errors,s.rx_errors,s.tx_overflows,s.rx_overflows,s.tx_bytes,s.rx_bytes,s.tx_dropped,s.rx_dropped};
 const uint64_t start[]={base.tx_packets,base.rx_packets,base.tx_errors,base.rx_errors,base.tx_overflows,base.rx_overflows,base.tx_bytes,base.rx_bytes,base.tx_dropped,base.rx_dropped};
 const uint8_t tags[]={0x10,0x11,0x12,0x13,0x14,0x15,0x19,0x1a,0x1d,0x1e};
 for(unsigned i=0;i<10;i++)if(mask&(1u<<i))x9w_integer(p,tags[i],values[i]>=start[i]?values[i]-start[i]:0,i==6||i==7?8:4);
}
static unsigned x9w_rat(const x9w_client &c){
 const x9q_data_radio &r=x9q_data_radios[c.sim_slot];
 return r.sampled&&x9q_ms()-r.sampled<=30000?r.rat:0;
}
static void x9w_rat_tlv(x9q_packet &p,unsigned tag,unsigned rat){
 uint8_t value[9]={2,(uint8_t)rat,(uint8_t)(rat>>8),(uint8_t)(rat>>16),(uint8_t)(rat>>24),0,0,0,0};
 p.tlv((uint8_t)tag,value,sizeof(value)); // 3GPP, real LTE/NR RAT mask, SO unknown
}
static void x9w_pending_done(x9w_bearer &b){
 if(b.pending&&x9w_live(b))x9w_clients[b.slot].starting=false;b.pending=false;b.reply_deadline=0;b.len=0;bzero(b.req,sizeof(b.req));
}
static void x9w_v6_fail(unsigned slot,uint16_t verbose);
static void x9w_error(x9w_bearer &b,unsigned error){
 if(b.pending&&x9w_live(b)){
  x9q_packet p(b.req,(uint16_t)error);
  if(error==14&&x9q_u16(b.req+3)==0x20){
   // A proprietary MIPC error is not a QMI/3GPP cause. Report unspecified
   // internal failure, not a fabricated network cause or a bare CALL_FAILED.
   x9w_end_reason(p,X9W_END_UNSPECIFIED,X9W_VERBOSE_INTERNAL,X9W_INTERNAL_ERROR);
   for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){
    const x9w_client &c=x9w_clients[i];
    if(c.v6_pending&&c.sim_slot==b.sim_slot&&c.mux==x9w_clients[b.slot].mux)
     x9w_v6_fail(i,X9W_INTERNAL_ERROR);
   }
  }
  p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);
  printf("X9 WDS: response msg 0x%04x error %u; ownership retained %u op %u\n",x9q_u16(b.req+3),error,b.owned,b.op);
 }x9w_pending_done(b);
}
static void x9w_disconnect(x9w_bearer &b,unsigned mux){
 bool was=b.active;
 if(b.route_lifetime&&cbp_mux_ok(mux)){
  x9w_retirement &r=x9w_retirements[mux-1];
  if(!r.pending){r.lifetime=b.route_lifetime;r.generation=b.generation;r.slot=b.slot;r.pending=true;}
  else if(r.lifetime!=b.route_lifetime)x9d_fail("WDS retirement ledger collision");
  b.route_lifetime=0; // ownership transferred once, including after DEL_CLIENT
 }
 b.active=false;bzero(&b.runtime,sizeof(b.runtime));x9d_unbind(mux);
 if(was&&x9w_live(b)){
  uint8_t req[7]={0,0,0,0x22,0,0,0};x9q_packet p(req);uint8_t status[2]={1,0},family=4;
  p.tlv(1,status,2);
  if(b.end_type==X9W_VERBOSE_CM)x9w_end_reason(p,X9W_END_CLIENT,X9W_VERBOSE_CM,b.end_verbose);
  else x9w_end_reason(p,X9W_END_UNSPECIFIED,X9W_VERBOSE_INTERNAL,b.end_type?b.end_verbose:X9W_INTERNAL_ERROR);
  p.tlv(0x12,&family,1);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,0x22);
  printf("X9 WDS: disconnected indication mux %u verbose type %u reason %u\n",mux,b.end_type?b.end_type:X9W_VERBOSE_INTERNAL,b.end_type?b.end_verbose:X9W_INTERNAL_ERROR);
 }
}
static void x9w_host_expire(x9w_bearer &b,unsigned mux,uint64_t now){
 if(!b.pending||!b.reply_deadline||now<b.reply_deadline)return;
 // Host reply lifetime is independent of any wire token or release gate.
 // Close admission once; pending_done only retires the request bytes, not
 // the bearer, original token, CID reservation or in-flight DPMAIF records.
 b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);
 printf("X9 WDS: host reply deadline mux %u; original control/data retirement retained\n",mux);
}
static void x9w_clear(x9w_bearer &b,unsigned mux){x9w_disconnect(b,mux);x9w_pending_done(b);bzero(&b,sizeof(b));}
static bool x9w_send(x9w_bearer &b,x9w_frame &f,unsigned op,unsigned timeout){
 b.token=f.submit();if(!b.token)return false;b.op=op;b.deadline=x9q_ms()+timeout;return true;
}
static bool x9w_deactivate(x9w_bearer &b,unsigned mux){
 x9w_disconnect(b,mux);
 // A token/old CID is not authority to mutate the replacement card epoch.
 if(b.result_epoch!=x9a_epoch){b.hold=true;return false;}
 if(b.md_down&&b.owned&&!b.token&&!b.hold){
  // No positive evidence the CID is still ours: release locally. At worst
  // one usr1 ref byte stays; a later same-APN ACT reuses it and any 203 on
  // that PDN zeroes the whole byte (913d839e), so nothing leaks for good.
  if(b.pending&&x9q_u16(b.req+3)==0x21&&x9w_live(b)){x9q_packet r(b.req);r.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
  else x9w_error(b,X9Q_NOT_READY);
  printf("X9 WDS: call %u mux %u already down at MD or unconfirmed; local release, no 203\n",b.cid,mux);
  x9w_clear(b,mux);return true;
 }
 if(!b.owned||b.token||b.hold||!x9_md_mipc_online())return false;
 x9w_frame f(0x203,1u<<b.sim_slot);f.byte(0x101,b.cid);f.byte(0x102,1); // normal, never FORCE_LOCAL
 if(!x9w_send(b,f,X9W_DEACT,60000))return false;
 printf("X9 WDS: deactivate owned call %u mux %u (await real CNF)\n",b.cid,mux);return true;
}
static bool x9w_check(x9w_bearer &b){
 // Frozen once per actual GET205 token. Neither a CNF header mask nor a
 // later client incarnation can rewrite the token's originating identity.
 if(b.result_epoch!=x9a_epoch){b.hold=true;return false;}
 if(!x9w_live(b))return false;
 x9wr_identity query={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,4,0};
 x9w_frame f(0x205,1u<<b.sim_slot);f.byte(0x101,b.cid);
 if(!x9w_send(b,f,X9W_CHECK,30000))return false;
 b.query=query;return true;
}
static bool x9w_get_settings(x9w_bearer &b,const uint8_t *p,unsigned n){
 x9wr_identity current={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,4,0};
 x9wr_get get={};unsigned dns=0;
 if(!x9w_byte(p,n,0x10d,dns)||dns>4||b.result_epoch!=x9a_epoch||!x9w_live(b)||!x9wr_same_identity(b.query,current)){
  printf("X9 WDS: GET205 identity/dns gate failed (dns %u)\n",dns);return false;}
 if(!x9wr_decode_get(p,n,b.query,get)){printf("X9 WDS: GET205 decode failed\n");return false;}
 if(get.channel!=b.channel||get.trans_id!=b.trans_id||
    (get.v4.mtu_present&&get.v4.mtu&&b.md_mtu&&get.v4.mtu!=b.md_mtu)||memcmp(get.v4.address,b.ip,4)){
  printf("X9 WDS: GET205 differs from ACT (channel %u/%u trans %u/%u mtu %u/%u addr-same %u)\n",get.channel,b.channel,
   get.trans_id%100,b.trans_id%100,get.v4.mtu_present?get.v4.mtu:0,b.md_mtu,!memcmp(get.v4.address,b.ip,4));return false;}
 // Still corroborate the earlier ACT facts; do not retarget a live route.
 b.get_result=get;return true;
}
// One non-private line naming the ACT CNF fields our gate saw.
static void x9w_act_diag(const x9w_bearer &b,const uint8_t *p,unsigned n){
 unsigned cid=999,type=999,channel=999,trans=999,mtu=0,count=999,dns=999,v6=0;
 x9w_byte(p,n,0x100,cid);x9w_byte(p,n,0x102,type);x9w_word(p,n,0x125,channel);x9w_word(p,n,0x12a,trans);
 bool has_mtu=x9w_word(p,n,0x123,mtu);x9w_byte(p,n,0x103,count);x9w_byte(p,n,0x10d,dns);x9w_byte(p,n,0x108,v6);
 x9q_view ip=x9w_tlv(p,n,0x8104);
 printf("X9 WDS: ACT CNF rejected: cid %u/%u-%u type %u count %u v6count %u dns %u channel %u trans%%100 %u mtu %s%u ip-len %u gone %u abandoned %u dpmaif %u\n",
  cid,b.min_cid,b.max_cid,type,count,v6,dns,channel,trans==999?999:trans%100,has_mtu?"":"absent ",mtu,ip.p?ip.n:0,b.gone,b.abandoned,x9d_ready());
}
static bool x9w_settings(x9w_bearer &b,const uint8_t *p,unsigned n){
 unsigned cid,type,channel,mtu=0,count,dns;
 if(!x9w_byte(p,n,0x100,cid)||cid!=b.cid||!x9w_byte(p,n,0x102,type)||(type!=1&&type!=3)||
  !x9w_word(p,n,0x125,channel)||channel>=20||
  !x9w_byte(p,n,0x103,count)||count!=1||!x9w_byte(p,n,0x10d,dns)||dns>4)return false;
 x9q_view m=x9w_tlv(p,n,0x123);
 if(m.p&&(m.n!=4||((mtu=x9q_u32(m.p))!=0&&(mtu<68||mtu>X9D_TX_SIZE))))return false; // absent/0 = unspecified
 x9q_view ip=x9w_tlv(p,n,0x8104);
 if(!ip.p||ip.n!=4||!ip.p[0]||ip.p[0]>=224||ip.p[0]==127)return false;
 uint8_t addresses[2][4]={};
 for(unsigned i=0;i<dns;i++){x9q_view d=x9w_tlv(p,n,0x810e + i);if(!d.p||d.n!=4)return false;if(i<2)memcpy(addresses[i],d.p,4);}
 if(b.ip[0]&&(channel!=b.channel||mtu!=b.md_mtu||memcmp(b.ip,ip.p,4)))return false; // no live retargeting
 b.channel=channel;b.md_mtu=mtu;memcpy(b.ip,ip.p,4);b.dns_count=dns>2?2:dns;memcpy(b.dns,addresses,sizeof(addresses));
 return true; // no gateway/netmask derived from stale versioned tags
}
static void x9w_handle_reply(x9w_bearer &b){
 uint32_t h=b.handle;uint8_t bytes[4]={(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24)};
 x9q_packet p(b.req);p.tlv(1,bytes,4);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);x9w_pending_done(b);
}
// ---- AP interface state to the MD (netagent role) ---------------------
// mipc_msg_tlv_const.h: INTERNAL_EIF_REQ = 0x313 / CNF 0x314; TRANSID100
// u32, CMD101 u8 (0 IFUP, 1 IFDOWN, 2 IPUPDATE...), ADDRESS_TYPE104 u8
// (1 v4, 2 v6, 3 v4v6). MD mipc_internal_eif_req (91335d2a) turns IFUP into
// AT+EIF=<id>,"ifst","up",<type>. On Android the netagent sends it after it
// has configured the ccmni for the MD's EIF IFUP; 193/194.txt never sent it
// and the UL engine stopped after 2 packets. CNFs are drained by their own
// token table so no MIPC slot is leaked when a bearer is cleared.
struct x9w_side { uint64_t token,deadline; uint8_t cmd,sim; uint32_t trans; uint16_t cnf; const char *what; };
static x9w_side x9w_sides[8];
static const char *x9w_eif_name(unsigned cmd){return cmd==0?"ifst up":cmd==1?"ifst down":cmd==2?"ipupdate v4":"?";}
static void x9w_ifst(unsigned sim,uint32_t trans,unsigned cmd,unsigned type,const uint8_t *v4=nullptr){
 x9w_side *slot=nullptr;for(x9w_side &e:x9w_sides)if(!e.token){slot=&e;break;}
 if(!slot||!x9_md_mipc_online()){printf("X9 WDS: EIF %s ccmni%u not sent (no slot/MIPC)\n",x9w_eif_name(cmd),trans%100);return;}
 x9w_frame f(0x313,1u<<sim);f.word(0x100,trans);f.byte(0x101,(uint8_t)cmd);
 if(!cmd)f.byte(0x104,(uint8_t)type);
 if(cmd==2){ // NEW_ADDR 0x8102: MD reads exactly 0x20 B, u32 len @0, addr @4
  uint8_t a[32]={4,0,0,0};if(v4)memcpy(a+4,v4,4);f.tlv(0x8102,a,sizeof(a));
 }
 uint64_t token=f.submit();
 if(!token){printf("X9 WDS: EIF %s ccmni%u submit failed\n",x9w_eif_name(cmd),trans%100);return;}
 *slot={token,x9q_ms()+30000,(uint8_t)cmd,(uint8_t)sim,trans,0,nullptr};
 printf("X9 WDS: SIM%u EIF %s ccmni%u type %u sent to MD\n",sim+1,x9w_eif_name(cmd),trans%100,type);
}
// SYS_CONNECTIVITY_STATISTICS_REQ 0x004b (vendor header): START101 resets
// and starts the MD counters, READ_FLAG100 returns them in the CNF.
static uint64_t x9w_stats_next;
static void x9w_stats(bool start){
 x9w_side *slot=nullptr;for(x9w_side &e:x9w_sides)if(!e.token){slot=&e;break;}
 if(!slot||!x9_md_mipc_online())return;
 x9w_frame f(0x4b,1);f.byte(start?0x101:0x100,1);
 uint64_t token=f.submit();if(!token){printf("X9 WDS: MD statistics submit failed\n");return;}
 *slot={token,x9q_ms()+30000,(uint8_t)(start?0x80:0x81),0,0,0,nullptr};
 x9w_stats_next=x9q_ms()+10000;
}
// Mirror of the Android RIL requests around a data call (datacycle.log):
// After the ACT CNF: DATA_GET_5GQOS_REQ {cid},
// DATA_GET_URSP_ROUTE_PROFILE_REQ {u32 cid}, DATA_GET_MD_DATA_CALL_LIST_REQ {}.
// These are diagnostic queries only; CONFIG/RETRY_RESET belong to the bearer.
static bool x9w_side_send(x9w_frame &f,unsigned sim,uint16_t cnf,const char *what,bool quiet=false){
 x9w_side *slot=nullptr;for(x9w_side &e:x9w_sides)if(!e.token){slot=&e;break;}
 if(!slot||!x9_md_mipc_online()){if(!quiet)printf("X9 WDS: %s not sent (no slot/MIPC)\n",what);return false;}
 uint64_t token=f.submit();if(!token){if(!quiet)printf("X9 WDS: %s submit failed\n",what);return false;}
 *slot={token,x9q_ms()+30000,0xa0,(uint8_t)sim,0,cnf,what};
 return true;
}
static void x9w_ril_post_act(unsigned sim,unsigned cid){
 {x9w_frame f(0x22d,1u<<sim);f.byte(0x100,(uint8_t)cid);x9w_side_send(f,sim,0x22e,"DATA_GET_5GQOS");}
 {x9w_frame f(0x233,1u<<sim);f.word(0x100,cid);x9w_side_send(f,sim,0x234,"DATA_GET_URSP_ROUTE_PROFILE");}
 {x9w_frame f(0x20f,1u<<sim);x9w_side_send(f,sim,0x210,"DATA_GET_MD_DATA_CALL_LIST");}
}
// Android boot capture (subsys_daemon, libmtk-radio-service): before the
// internet ACT it sends DATA_ACT_PACKET_ROUTE_REQ 0x25f {0x100 u8 3} and, on
// the MD's DATA_IFUP_PACKET_ROUTE_IND 0x4220 {app_id 3,...}, DATA_IFUP_
// PACKET_ROUTE_REQ 0x261 {0x100 u8 3}. The surrounding runtime log identifies
// ImsRtpRedunDataController -> syncImsRtpRedunDataPathConfig. Temporal order
// does NOT make this an internet-PDN dependency; disabled unless requested.
static void x9w_side_poll(){
 if(x9w_stats_next&&x9q_ms()>=x9w_stats_next)x9w_stats(false);
 static uint8_t response[X9_MIPC_FRAME_MAX];
 for(x9w_side &e:x9w_sides){if(!e.token)continue;
  unsigned n=0;int ready=x9_md_mipc_take(e.token,response,sizeof(response),&n);
  if(ready==1&&e.cmd==0xa0){
   unsigned result=~0u;bool good=x9w_wire(response,n)&&x9q_u16(response+10)==e.cnf&&x9w_word(response,n,0,result);
   if(good&&(e.cnf==0x260||e.cnf==0x262)){unsigned app=0;good=x9w_byte(response,n,0x100,app)&&app==3;}
   printf("X9 WDS: SIM%u %s CNF %s result %u (%u B)\n",e.sim+1,e.what,good?"valid":"malformed",result,n);
   bzero(response,sizeof(response));bzero(&e,sizeof(e));continue;
  }
  if(ready==1&&e.cmd>=0x80){
   // SYS_CONNECTIVITY_STATISTICS_CNF 0x004c: MD-side air-interface totals.
   unsigned result=~0u,kb_tx=0,kb_rx=0;bool good=x9w_wire(response,n)&&x9q_u16(response+10)==0x4c&&x9w_word(response,n,0,result);
   x9q_view tx=x9w_tlv(response,n,0x107),rx=x9w_tlv(response,n,0x108);
   x9w_word(response,n,0x102,kb_tx);x9w_word(response,n,0x103,kb_rx);
   uint64_t btx=0,brx=0;
   if(tx.p&&tx.n==8)for(unsigned i=0;i<8;i++)btx|=(uint64_t)tx.p[i]<<(8*i);
   if(rx.p&&rx.n==8)for(unsigned i=0;i<8;i++)brx|=(uint64_t)rx.p[i]<<(8*i);
   printf("X9 WDS: MD statistics %s CNF %s result %u: air TX %llu B (%u KB) RX %llu B (%u KB)\n",
    e.cmd==0x80?"start":"read",good?"valid":"malformed",result,btx,kb_tx,brx,kb_rx);
   bzero(response,sizeof(response));bzero(&e,sizeof(e));continue;
  }
  if(ready==1){
   unsigned result=~0u;bool good=x9w_wire(response,n)&&x9q_u16(response+10)==0x314&&x9w_word(response,n,0,result);
   printf("X9 WDS: SIM%u EIF %s ccmni%u CNF %s result %u\n",e.sim+1,x9w_eif_name(e.cmd),e.trans%100,good?"valid":"malformed",result);
   bzero(response,sizeof(response));bzero(&e,sizeof(e));continue;
  }
  if(e.deadline&&x9q_ms()>=e.deadline){printf("X9 WDS: %s CNF late; still draining\n",e.what?e.what:"EIF/statistics");e.deadline=0;}
 }
 // Drain first; retry only enqueue failures, never a sent request. 210 lost
 // 16 IFUP submissions when the shared MIPC mailbox was full.
 if(!x9w_packet_route_enabled||!x9w_session_ready||!x9_md_mipc_online())return;
 if(x9w_packet_route_pending){
  x9w_frame f(0x25f,1);f.byte(0x100,3);
  if(x9w_side_send(f,0,0x260,"DATA_ACT_PACKET_ROUTE",true)){
   x9w_packet_route_pending=false;
   printf("X9 WDS: IMS diagnostic ACT packet route app3 queued once; not an internet route\n");
  }
 }
 for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(x9w_ifup_pending[sim]){
  x9w_frame f(0x261,1u<<sim);f.byte(0x100,3);
  if(x9w_side_send(f,sim,0x262,"DATA_IFUP_PACKET_ROUTE",true))x9w_ifup_pending[sim]=false;
 }
}
static void x9w_publish(x9w_bearer &b,unsigned mux){
 x9w_host_expire(b,mux,x9q_ms()); // every publication entry owns its deadline gate
 if(b.abandoned||b.hold||!b.pending)return;
 // The MD may deliver EIF IFUP before the ACT CNF (191.txt) in modem
 // interface form; adopt it for the host IF_ID once trans_id is known.
 if(!x9w_interfaces[b.sim_slot][b.channel].valid)
  for(unsigned i=0;i<20;i++){x9w_interface &m=x9w_interfaces[b.sim_slot][i];
   if(i!=b.channel&&m.valid&&m.cmd==0&&m.serial>b.if_floor&&m.trans_id/100==b.trans_id/100){
    x9w_interface &h=x9w_interfaces[b.sim_slot][b.channel];h=m;h.trans_id=b.trans_id;
    h.facts.trans_id=b.trans_id;h.facts.channel=b.channel;bzero(&m,sizeof(m));
    printf("X9 WDS: early EIF IFUP on modem interface %u adopted for host interface %u\n",i,b.channel);break;
   }
  }
 const x9w_interface &in=x9w_interfaces[b.sim_slot][b.channel];
 if(!in.valid||in.serial<=b.if_floor||in.trans_id!=b.trans_id||in.cmd!=0)return;
 x9wr_identity current={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,4,0};
 x9wr_settings runtime={};
 if(b.result_epoch!=x9a_epoch||!x9wr_match_settings(b.get_result,in.facts,current,runtime)||
    !x9wr_ipv4_facts(runtime)||memcmp(in.ip,b.ip,4)||!b.pending||!x9w_live(b)||
    x9w_next_handle>=0xfffffffeu||x9w_retirements[mux-1].pending||
    !x9d_bind(mux,b.channel,x9wr_ipv6_attach_ready(runtime)?3:1,runtime.v4.mtu,b.route_lifetime)){
  printf("X9 WDS: publish failed mux %u (match %u v4 %u addr-same %u retire %u)\n",mux,runtime.matched,x9wr_ipv4_facts(runtime),
   !memcmp(in.ip,b.ip,4),x9w_retirements[mux-1].pending);
  b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
 }
 b.mtu=runtime.v4.mtu;b.runtime=runtime;b.handle=++x9w_next_handle;b.active=true;b.op=0;b.next=x9q_ms()+10000;
 x9w_client &c=x9w_clients[b.slot];bzero(&c.baseline,sizeof(c.baseline));c.report_next=0;c.last_rat=0;
 c.last_call_bytes_valid=false;bzero(&c.last_call,sizeof(c.last_call));
 printf("X9 WDS: SIM%u PDP activated call %u -> ccmni%u -> mux %u IPv4 MTU %u PDP%u v6-ready %u, ACT+GET+EIF/IFUP verified, DPMAIF bound\n",
  b.sim_slot+1,b.cid,b.channel,mux,b.mtu,runtime.modem_pdp,x9wr_ipv6_attach_ready(runtime));
 x9w_handle_reply(b);
 // Connection notification is emitted only AFTER real bind; not CGATT alone.
 uint8_t req[7]={};x9q_packet connected(req);uint8_t status[2]={2,0},family=4;
 connected.tlv(1,status,2);connected.tlv(0x12,&family,1);
 connected.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,0x22);
 // DPMAIF route is bound and the host has the address: report the AP
 // interface up, typed like the verified PDN (EIF_ADDRESS_TYPE 1 or 3).
 x9w_ril_post_act(b.sim_slot,b.cid);
 printf("X9 WDS: call %u address %u.%u.%u.%u dns0 %u.%u.%u.%u dns_count %u (UL source must match)\n",b.cid,
  b.ip[0],b.ip[1],b.ip[2],b.ip[3],b.dns[0][0],b.dns[0][1],b.dns[0][2],b.dns[0][3],b.dns_count);
 // Android capture: the RIL/NetAgentService never sends INTERNAL_EIF_REQ
 // (no 0x313 in the MIPC log); it only consumes EIF indications. The
 // ifst/ipupdate/statistics requests stay available but are off by default.
 if(x9w_netagent_req)x9w_ifst(b.sim_slot,b.trans_id,0,runtime.modem_pdp==3?3:1);
 // netagent then reports the configured address (AT+EIF=<id>,"ipupdate",1,a.b.c.d).
 if(x9w_netagent_req)x9w_ifst(b.sim_slot,b.trans_id,2,0,b.ip);
 if(x9w_netagent_req)x9w_stats(true);
}
// Android datacycle: 0211 -> successful 0212 -> 021b -> successful 021c
// -> 0201. These are bearer-owned prerequisites, not best-effort side queries.
// mode1 is requestResetMdDataRetryCount (RIL 2ca5cc..2ca610), NOT the
// mode0 getModemSuggestedRetryTime query (2b6790). Its response consumer
// checks result only (2d1d34); CNF tag101=255 is not a delay to sleep.
// A full mailbox leaves an UNSENT stage pending under the original host
// deadline. Once submitted, its sole token must be drained before advancing.
static void x9w_start_next(x9w_bearer &b,unsigned mux){
 if(b.token||b.owned||b.hold||b.abandoned||!x9w_live(b)||b.result_epoch!=x9a_epoch||
    !x9d_ready()||!x9_md_mipc_online())return;
 x9w_host_expire(b,mux,x9q_ms());if(b.abandoned)return;
 if(b.op==X9W_RETRY_WAIT){
  if(x9q_ms()<b.next)return;
  b.op=X9W_RETRY_QUERY; // read back the MD timer, never reset it on a retry
 }
 if(b.op==X9W_RETRY_QUERY){
  // Android getModemSuggestedRetryTime (2b6790): mode0 + requested APN.
  // The empty/network-default APN is kept empty; do not guess a carrier DNN.
  x9w_frame f(0x21b,1u<<b.sim_slot);f.byte(0x100,0);
  f.tlv(0x101,b.apn,(unsigned)strlen(b.apn)+1);
  if(x9w_send(b,f,X9W_RETRY_QUERY,30000))printf("X9 WDS: SIM%u retry advice mode0 queued (%s); no counter reset\n",b.sim_slot+1,b.failure_result?"ACT failed":"retry admission");
  return;
 }
 if(b.op==X9W_DISC){
  x9w_frame f(0x219,1u<<b.sim_slot);x9w_send(b,f,X9W_DISC,10000);return;
 }
 if(b.op==X9W_CONFIG){
  x9w_frame f(0x211,1u<<b.sim_slot);f.byte(0x100,1);f.byte(0x101,1);f.byte(0x103,0);
  if(x9w_send(b,f,X9W_CONFIG,30000))printf("X9 WDS: SIM%u DATA_SET_CONFIG queued; await 0212 before RETRY_TIMER/ACT\n",b.sim_slot+1);
  return;
 }
 if(b.op==X9W_RETRY_RESET){
  const char *apn=b.apn[0]?b.apn:x9w_last_dnn[b.sim_slot];
  x9w_frame f(0x21b,1u<<b.sim_slot);f.byte(0x100,1);f.tlv(0x101,apn,(unsigned)strnlen(apn,100)+1);
  if(x9w_send(b,f,X9W_RETRY_RESET,30000))printf("X9 WDS: SIM%u DATA_RETRY_TIMER reset mode1 queued; await 021c before ACT\n",b.sim_slot+1);
  return;
 }
 if(b.op!=X9W_ACT)return;
 x9w_frame f(0x201,1u<<b.sim_slot);
 // Field-for-field the Android RIL DATA_ACT_CALL_REQ (datacycle.log
 // 14:02:21): USERID/PASSWORD empty, 0x11e {0f,0..}, URSP_EVAL_FLAG 0,
 // fallback 0x7f, APN, BEARER_BITMASK 0x7ffdffff, APN_TYPE 1, REUSE 0,
 // PDP 3 (v4v6), APN_INDEX 0, ROAMING 3, URSP_TRAFFIC_DESC (672 B), no
 // AUTH_TYPE and no IF_ID. On 5G SA the UE routes traffic to a PDU session
 // through URSP; matching the captured request is not proof of data delivery.
 static const char no_credentials[]="";
 static const uint8_t r11e[8]={0x0f,0,0,0,0,0,0,0};
 f.tlv(0x8106,no_credentials,sizeof(no_credentials));
 f.tlv(0x11e,r11e,sizeof(r11e));f.word(0x10f,0);
 f.tlv(0x8107,no_credentials,sizeof(no_credentials));
 f.byte(0x108,0x7f);
 f.tlv(0x101,b.apn,(unsigned)strlen(b.apn)+1);
 f.word(0x109,0x7ffdffff);f.word(0x102,1);f.byte(0x10a,0);f.byte(0x103,3);f.word(0x10c,0);f.byte(0x104,3);
 { // mipc_ursp_traffic_desc_struct_struct4: is_match_all@0 dnn_len@1 dnn@4
   // ... prot_id_next_hdr@414 (0xff = unspecified) dst_fqdn_len@415.
   static uint8_t td[672];bzero(td,sizeof(td));
   const char *dnn=b.apn[0]?b.apn:x9w_last_dnn[b.sim_slot];unsigned dl=(unsigned)strnlen(dnn,100);
   if(dl){td[0]=0;td[1]=(uint8_t)dl;memcpy(td+4,dnn,dl);}else td[0]=1; // no DNN known: match all
   td[414]=0xff;f.tlv(0x10d,td,sizeof(td));
 }
 b.if_floor=x9w_if_serial;
 if(x9w_send(b,f,X9W_ACT,120000))printf("X9 WDS: requesting v4v6 call mux %u (RIL-shaped ACT: pdp 3, reuse 0, fallback 0x7f, URSP desc), frame %u B\n",mux,f.n);
}
static void x9w_result(x9w_bearer &b,unsigned mux,const uint8_t *p,unsigned n){
 x9w_host_expire(b,mux,x9q_ms()); // late ready1 cannot restore host success
 unsigned op=b.op;b.op=0;unsigned result;
 unsigned expected=op==X9W_DISC?0x21a:op==X9W_CONFIG?0x212:(op==X9W_RETRY_RESET||op==X9W_RETRY_QUERY)?0x21c:
  op==X9W_ACT?0x202:op==X9W_CHECK?0x206:0x204;
 if(!x9w_wire(p,n)||x9q_u16(p+10)!=expected||!x9w_word(p,n,0,result)){
  b.hold=true;b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_INTERNAL);
  printf("X9 WDS: malformed CNF op %u; quarantined until boot\n",op);return;
 }
 // The actual take consumed this token, but a CNF from the retired card
 // epoch cannot authorize an old ACT continuation, cleanup203 or success.
 if(b.result_epoch!=x9a_epoch){
  b.hold=true;b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);return;
 }
 b.hold=false; // late valid CNF restores cleanup ability, NEVER host success
 printf("X9 WDS: MIPC CNF msg 0x%04x result %u mux %u\n",expected,result,mux);
 if(op==X9W_RETRY_QUERY){
  unsigned mode=~0u,seconds=0;
  bool good=!result&&x9w_byte(p,n,0x100,mode)&&mode<=2&&
   (mode!=2||(x9w_word(p,n,0x101,seconds)&&seconds<=0x7fffffffu));
  uint64_t now=x9q_ms(),after=now+30000;
  // Local 30s floor bounds host churn; it is NOT a claimed network timer.
  // MD seconds are widened before multiplication, and are never shortened.
  if(good&&mode!=1){after=mode==2?now+uint64_t(seconds)*1000:now;}
  x9w_retry_policy &policy=x9w_retries[b.sim_slot];
  if(!policy.valid||policy.epoch!=b.result_epoch)bzero(&policy,sizeof(policy));
  if(!good||mode==1)policy.floor=now+30000;
  if(policy.floor>after)after=policy.floor;
  if(x9w_retry_same(b)&&policy.after>after)after=policy.after;
  policy.valid=true;policy.epoch=b.result_epoch;policy.after=after;memcpy(policy.apn,b.apn,sizeof(policy.apn));
  printf("X9 WDS: SIM%u retry advice valid %u mode %u seconds %u wait-ms %llu; raw ACT result 0x%x\n",
   b.sim_slot+1,good,mode,seconds,after-now,b.failure_result);
  if(b.failure_result||b.abandoned||!good||mode==1){
   x9w_error(b,14);x9w_clear(b,mux);return;
  }
  if(after>now){b.next=after;b.op=X9W_RETRY_WAIT;return;}
  b.op=X9W_DISC;x9w_start_next(b,mux);return;
 }
 if(op==X9W_DISC){
  if(result||b.abandoned||!x9w_byte(p,n,0x100,b.min_cid)||!x9w_byte(p,n,0x101,b.max_cid)||b.min_cid>b.max_cid||b.max_cid>=255){
   x9w_error(b,result==1?X9Q_BUSY:X9Q_NOT_READY);x9w_clear(b,mux);return;
  }
  if(!x9d_ready()||!x9_md_mipc_online()){x9w_error(b,X9Q_NOT_READY);x9w_clear(b,mux);return;}
  b.op=X9W_CONFIG;x9w_start_next(b,mux);return;
 }
 if(op==X9W_CONFIG||op==X9W_RETRY_RESET){
  if(op==X9W_RETRY_RESET&&!result)x9w_reset_epoch[b.sim_slot]=b.result_epoch;
  if(result||b.abandoned||!x9d_ready()||!x9_md_mipc_online()){
   x9w_error(b,result==1?X9Q_BUSY:X9Q_NOT_READY);x9w_clear(b,mux);return;
  }
  b.op=op==X9W_CONFIG&&x9w_reset_epoch[b.sim_slot]!=b.result_epoch?X9W_RETRY_RESET:X9W_ACT;
  x9w_start_next(b,mux);return;
 }
 if(op==X9W_ACT){
  unsigned cid;
  if(result&&result!=0x150202&&result!=0x150203&&result!=0x150204){
   // P77 mipc_data_act_call_cnf 913274ea..91327500 also emits tag100
   // on ordinary FAILURE whenever its input CID is nonnegative. It is a
   // diagnostic hint, NOT an owned call or a reason to reserve the host mux
   // forever (211: 510 BUSY replies). Never bind or issue 203 for this hint.
   bool hint=x9w_byte(p,n,0x100,cid);
   printf("X9 WDS: ACT rejected 0x%x mux %u CID-hint %u; no ownership/203, query retry policy\n",result,mux,hint);
   x9w_retry_policy &r=x9w_retries[b.sim_slot];
   r.valid=true;r.epoch=b.result_epoch;r.floor=x9q_ms()+30000;r.after=r.floor;
   memcpy(r.apn,b.apn,sizeof(r.apn));
   b.failure_result=result;
   if(b.abandoned){x9w_error(b,14);x9w_clear(b,mux);return;}
   b.op=X9W_RETRY_QUERY;x9w_start_next(b,mux);return;
  }
  if(!x9w_byte(p,n,0x100,cid)||cid<b.min_cid||cid>b.max_cid){b.hold=true;b.abandoned=true;x9w_error(b,X9Q_INTERNAL);return;}
  for(const x9w_bearer &other:x9w_bearers)if(&other!=&b&&other.used&&other.owned&&other.sim_slot==b.sim_slot&&other.cid==cid){
   b.hold=true;b.abandoned=true;x9w_error(b,X9Q_INTERNAL);return; // don't deactivate sibling
  }
  b.cid=(uint8_t)cid;b.owned=true;
  { x9q_view a=x9w_tlv(p,n,0x101); // CNF APN actually used: the URSP DNN for later ACTs
    if(!result&&a.p&&a.n>1&&a.n<=101){unsigned l=(unsigned)strnlen((const char *)a.p,a.n);
     if(l+1==a.n&&x9w_apn(a.p,l)){memcpy(x9w_last_dnn[b.sim_slot],a.p,l);x9w_last_dnn[b.sim_slot][l]=0;}}}
  // Actual RIL treats 150202/150204 as partial success and decodes 150203's
  // partial profile. This IPv4-only dialler cleans up instead of falling back.
  if(result||b.gone||b.abandoned||!x9w_settings(b,p,n)||
     !x9w_word(p,n,0x12a,b.trans_id)||b.trans_id%100!=b.channel||!x9d_ready()){
   x9w_act_diag(b,p,n);b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  if(!x9w_check(b)){b.abandoned=true;x9w_error(b,X9Q_BUSY);x9w_deactivate(b,mux);}return;
 }
 if(op==X9W_CHECK){
  if(b.gone){x9w_clear(b,mux);return;}
  if(b.abandoned){x9w_deactivate(b,mux);return;}
  if(result||!x9w_get_settings(b,p,n)||!x9d_ready()){
   printf("X9 WDS: GET205 result %u rejected mux %u\n",result,mux);
   // Keep 203 only if this reply proves the CID still maps to our route.
   x9wr_get still={};
   if(result||!x9wr_decode_get(p,n,b.query,still)||still.channel!=b.channel||still.trans_id!=b.trans_id)b.md_down=true;
   b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  if(!b.active){
   b.op=X9W_IFWAIT;b.deadline=x9q_ms()+30000;x9w_publish(b,mux);return;
  }
  const x9w_interface &in=x9w_interfaces[b.sim_slot][b.channel];x9wr_settings runtime={};
  if(!in.valid||in.cmd!=0||!x9wr_match_settings(b.get_result,in.facts,b.query,runtime)||
     !x9wr_ipv4_facts(runtime)){
   b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  b.runtime=runtime;b.next=x9q_ms()+10000;return;
 }
 unsigned cid;
 if(result||!x9w_byte(p,n,0x100,cid)||cid!=b.cid){
  b.hold=true;x9w_error(b,X9Q_NOT_READY);printf("X9 WDS: deactivation unconfirmed, reserved mux %u until boot\n",mux);return;
 }
 if(b.pending&&x9w_live(b)){x9q_packet reply(b.req);reply.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
 printf("X9 WDS: owned call %u deactivation confirmed mux %u\n",b.cid,mux);x9w_clear(b,mux);
}
static void x9w_del(unsigned slot){
 x9a_del(slot);
 for(unsigned i=0;i<CBP_INTERFACES;i++){x9w_bearer &b=x9w_bearers[i];
  if(b.used&&b.slot==slot&&b.generation==x9q_clients[slot].generation){b.abandoned=true;x9w_pending_done(b);x9w_disconnect(b,i+1);}
 }bzero(&x9w_clients[slot],sizeof(x9w_clients[slot]));
}
static void x9w_bad_event(){
 x9w_session_ready=false;x9w_init_hold=true;
 for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];if(!b.used)continue;
  b.hold=true;b.abandoned=true;x9w_disconnect(b,m+1);x9w_error(b,X9Q_NOT_READY);
 }printf("X9 WDS: invalid MD interface/deactivation event; no connected state, quarantined until boot\n");
}
static bool x9w_interface_event(const uint8_t *p,unsigned n,unsigned sim){
 unsigned trans,cmd;
 if(!x9w_word(p,n,0x100,trans)||!x9w_byte(p,n,0x101,cmd))return false;
 if(trans%100>=20)return true; // not a supported/owned ccmni route
 // Host IF_ID calls: mipc_internal_eif_ind (913368e8..) and the ACT CNF both
 // build host_trans = modem_trans - modem_trans%100 + host_if. A client the
 // MD does not treat as sAP still receives the modem_trans form, so map an
 // event whose hundreds match an owned call but whose interface differs.
 bool remapped=false;
 for(const x9w_bearer &b:x9w_bearers)
  if(b.used&&b.owned&&b.result_epoch==x9a_epoch&&b.sim_slot==sim&&b.trans_id&&b.trans_id!=trans&&b.trans_id/100==trans/100){
   printf("X9 WDS: EIF modem interface %u mapped to host interface %u (call %u)\n",trans%100,b.trans_id%100,b.cid);
   trans=b.trans_id;remapped=true;break;
  }
 // Only IFUP(0)/IFDOWN(1) change interface state. 193.txt: command 5 came
 // 9.6 s after IFUP and the real IFDOWN(1) only 42 s later, yet the old code
 // tore the call down on it. Other commands (change/RA/state updates) are
 // logged; an address change is still caught by the periodic GET205 check.
 if(cmd!=0&&cmd!=1){
  printf("X9 WDS: MD SIM%u EIF ccmni%u command %u (not IFUP/IFDOWN; state kept)\n",sim+1,trans%100,cmd);return true;
 }
 x9w_interface &in=x9w_interfaces[sim][trans%100];bzero(&in,sizeof(in));
 in.serial=++x9w_if_serial;in.trans_id=trans;in.cmd=cmd;
 if(cmd==0){
  // Typed EIF facts including the capture card epoch. A dual-stack IFUP
  // carries both halves; the v4 half is required for our route, the v6 half
  // is only used after it matches GET205. No tag103 fallback MTU.
  if(x9wr_decode_ifup(p,n,sim,x9a_epoch,in.serial,in.facts)&&in.facts.v4.address_present){
   if(remapped){in.facts.trans_id=trans;in.facts.channel=trans%100;}
   in.mtu=in.facts.v4.mtu;memcpy(in.ip,in.facts.v4.address,4);
   in.valid=true; // MTU may be 0/unspecified; x9wr_match_family picks it
  }
  // Foreign IMS/IPv6 events do not poison the IPv4 dialler. An owned route
  // with an unsupported IFUP remains disconnected and is cleaned up.
 }else in.valid=true;
 printf("X9 WDS: MD SIM%u EIF/IFST ccmni%u command %u (payload private)\n",sim+1,trans%100,cmd);
 for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
  if(b.used&&b.owned&&b.result_epoch==x9a_epoch&&b.sim_slot==sim&&b.trans_id==trans&&in.serial>b.if_floor&&
     (cmd!=0||!in.valid||(b.active&&memcmp(in.ip,b.ip,4)))){
   if(cmd!=0){b.md_down=true;if(x9w_netagent_req)x9w_ifst(sim,trans,1,0);} // MD ifdown precedes its logical CID removal
   x9w_host_expire(b,m+1,x9q_ms());
   // 212: expected IFDOWN arrived just before our 204/result0 and incorrectly
   // consumed the pending Stop with error16. Revoke data admission now, but
   // leave the original 203 token and host deadline intact. IFDOWN is NOT a
   // successful DEACT CNF; a timeout/bad CNF must still fail the Stop.
   if(cmd==1&&b.op==X9W_DEACT&&b.token&&!b.hold&&b.pending&&
      x9q_u16(b.req+3)==0x21&&x9w_live(b)){
    x9w_disconnect(b,m+1);
    printf("X9 WDS: expected IFDOWN during Stop mux %u; await original DEACT CNF, deadline unchanged\n",m+1);
    continue;
   }
   b.abandoned=true;x9w_disconnect(b,m+1);x9w_error(b,X9Q_NOT_READY);
  }
 }return true;
}
// ---- IPv6 half of a dual-stack call -------------------------------------
// iOS opens separate v4 and v6 WDS clients on the same mux (191.txt: both
// Starts within 1 ms). The MD call is the v4 client's; the v6 client attaches
// to it when the verified call is PDP3 and its Start asks for the same
// APN/profile. Stop/teardown of the v6 half never touches the MD call.
static void x9w_selection(const uint8_t *req,unsigned n,uint8_t *apn,uint8_t &apn_n,int &profile){
 x9q_view a=x9q_tlv(req,n,0x14),pr=x9q_tlv(req,n,0x31);
 apn_n=(uint8_t)(a.p&&a.n<=100?a.n:0);if(apn_n)memcpy(apn,a.p,apn_n);
 profile=pr.p&&pr.n==1?pr.p[0]:-1;
}
static x9w_bearer *x9w_v6_bearer(const x9w_client &c){
 if(!cbp_mux_ok(c.mux))return nullptr;x9w_bearer &b=x9w_bearers[c.mux-1];
 return b.used&&b.sim_slot==c.sim_slot?&b:nullptr;
}
static bool x9w_v6_valid(unsigned slot){
 const x9w_client &c=x9w_clients[slot];const x9w_bearer *b=x9w_v6_bearer(c);
 return c.v6_active&&b&&x9w_data_ready(*b)&&b->route_lifetime==c.v6_lifetime&&
  x9wr_ipv6_attach_ready(b->runtime);
}
static void x9w_v6_status(unsigned slot,bool connected,uint16_t type,uint16_t verbose){
 uint8_t req[7]={};x9q_packet p(req);uint8_t status[2]={(uint8_t)(connected?2:1),0},family=6;
 p.tlv(1,status,2);
 if(!connected)x9w_end_reason(p,type==X9W_VERBOSE_CM?X9W_END_CLIENT:X9W_END_UNSPECIFIED,type,verbose);
 p.tlv(0x12,&family,1);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1,true,0x22);
}
static void x9w_v6_fail(unsigned slot,uint16_t verbose){
 x9w_client &c=x9w_clients[slot];
 if(c.v6_pending&&c.v6_len){x9q_packet p(c.v6_req,14);x9w_end_reason(p,X9W_END_UNSPECIFIED,X9W_VERBOSE_INTERNAL,verbose);
  p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1);}
 printf("X9 WDS: v6 Start client %u mux %u failed: internal %u\n",slot,c.mux,verbose);
 c.v6_pending=false;c.v6_len=0;bzero(c.v6_req,sizeof(c.v6_req));
}
// Returns true once the pending Start has been answered (success or failure).
static bool x9w_v6_try(unsigned slot){
 x9w_client &c=x9w_clients[slot];x9w_bearer *b=x9w_v6_bearer(c);
 if(!b||!x9w_data_ready(*b))return false;
 uint8_t apn[101];uint8_t apn_n=0;int profile=-1;x9w_selection(c.v6_req,c.v6_len,apn,apn_n,profile);
 if(apn_n!=b->start_apn_n||memcmp(apn,b->start_apn,apn_n)||profile!=b->start_profile){
  x9w_v6_fail(slot,X9W_INTERNAL_IPV6_DISALLOWED);printf("X9 WDS: v6 Start selection differs from the live call; not attached\n");return true;
 }
 if(!x9wr_ipv6_attach_ready(b->runtime)||x9w_next_handle>=0xfffffffeu){
  x9w_v6_fail(slot,X9W_INTERNAL_IPV6_DISALLOWED); // v4-only PDN: host keeps its v4 call
  return true;
 }
 c.v6_handle=++x9w_next_handle;c.v6_lifetime=b->route_lifetime;c.v6_active=true;c.v6_pending=false;
 uint32_t h=c.v6_handle;uint8_t bytes[4]={(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24)};
 x9q_packet p(c.v6_req);p.tlv(1,bytes,4);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1);
 c.v6_len=0;bzero(c.v6_req,sizeof(c.v6_req));
 x9w_v6_status(slot,true,0,0);
 printf("X9 WDS: SIM%u v6 client %u attached to call %u mux %u (PDP3, MTU %u, DNS %u, MD address %s)\n",
  b->sim_slot+1,slot,b->cid,c.mux,b->runtime.v6.mtu,b->runtime.v6.dns_count,
  b->runtime.v6.address[0]==0xfe&&(b->runtime.v6.address[1]&0xc0)==0x80?"link-local":"global");
 return true;
}
static void x9w_v6_poll(){
 uint64_t now=x9q_ms();
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){x9w_client &c=x9w_clients[slot];
  if(c.v6_active&&!x9w_v6_valid(slot)){
   const x9w_bearer *b=x9w_v6_bearer(c);
   uint16_t type=b&&b->end_type?b->end_type:X9W_VERBOSE_INTERNAL,verbose=b&&b->end_type?b->end_verbose:X9W_INTERNAL_ERROR;
   c.v6_active=false;c.v6_handle=0;c.v6_lifetime=0;x9w_v6_status(slot,false,type,verbose);
   printf("X9 WDS: v6 client %u mux %u disconnected with its call (type %u reason %u)\n",slot,c.mux,type,verbose);
  }
  if(!c.v6_pending)continue;
  if(x9w_v6_try(slot))continue;
  const x9w_bearer *b=x9w_v6_bearer(c);
  if(b&&(b->abandoned||b->hold))x9w_v6_fail(slot,X9W_INTERNAL_ERROR); // v4 call failed
  else if(b)c.v6_deadline=now+30000; // v4 call still being set up
  else if(now>=c.v6_deadline)x9w_v6_fail(slot,X9W_INTERNAL_ERROR);   // no v4 call on this mux
 }
}
static void x9w_poll(){
 x9w_retire_poll();
 static uint8_t response[X9_MIPC_FRAME_MAX];unsigned n=0;
 x9w_init_poll(response,sizeof(response));
 x9a_poll(response,sizeof(response));
 for(unsigned i=0;i<8;i++){
  int event=x9_md_mipc_event(response,sizeof(response),&n);if(event<=0)break;unsigned cid;
  if(!x9w_wire(response,n)){x9w_bad_event();}
  else if(x9q_u16(response+10)==0x4302){
   for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(response[8]&(1u<<sim))
    if(!x9w_interface_event(response,n,sim))x9w_bad_event();
  }
  else if(x9q_u16(response+10)==0x4220){
   unsigned app=0,a1=0,a2=0,a3=0;
   bool valid=x9w_byte(response,n,0x100,app)&&app==3&&x9w_byte(response,n,0x101,a1)&&
    x9w_byte(response,n,0x102,a2)&&x9w_byte(response,n,0x103,a3);
   if(x9w_packet_route_enabled&&valid&&(response[8]==1||(response[8]==2&&x9_md_sim_count()==2))){
    unsigned sim=response[8]==1?0:1;
    printf("X9 WDS: SIM%u IMS DATA_IFUP_PACKET_ROUTE_IND app %u (%u %u %u); queue IFUP only\n",sim+1,app,a1,a2,a3);
    // 210.txt: re-sending ACT_PACKET_ROUTE on every IND made the MD raise
    // the IND again (1016 round trips). subsys_daemon answers an IND with
    // DATA_IFUP_PACKET_ROUTE only; ACT_PACKET_ROUTE is sent once at start.
    x9w_ifup_pending[sim]=true;
   }
  }
  else if(x9q_u16(response+10)==0x4202&&x9w_byte(response,n,0x100,cid)){
   for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
    // Event delivery precedes the bearer epoch sweep below. A replacement
    // card's same-CID event is not proof that this original call was released.
    if(b.used&&b.owned&&b.result_epoch==x9a_epoch&&b.cid==cid&&(response[8]&(1u<<b.sim_slot))){
     x9w_set_end(b,X9W_VERBOSE_INTERNAL,X9W_INTERNAL_NW_TERMINATION); // MD-side end, cause not exported
     x9w_host_expire(b,m+1,x9q_ms()); // bound unsolicited Stop completion too
     b.gone=true;b.owned=false;b.abandoned=true;x9w_disconnect(b,m+1);
     if(b.pending&&x9q_u16(b.req+3)==0x21&&x9w_live(b)){x9q_packet p(b.req);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
     else x9w_error(b,X9Q_NOT_READY);
     x9w_pending_done(b);if(!b.token)x9w_clear(b,m+1);
     printf("X9 WDS: MD deactivation indication for owned call %u mux %u\n",cid,m+1);
    }
   }
  }else x9w_bad_event();
  bzero(response,sizeof(response));
 }
 uint64_t now=x9q_ms();bool online=x9_md_mipc_online();
 x9w_v6_poll();
 for(unsigned i=0;i<CBP_INTERFACES;i++){x9w_bearer &b=x9w_bearers[i];if(!b.used)continue;
  x9w_host_expire(b,i+1,now);
  if(b.result_epoch!=x9a_epoch)b.hold=true; // retain/drain token, never old-epoch203
  if(!x9w_live(b)||!online||!x9d_ready()||b.result_epoch!=x9a_epoch){b.abandoned=true;x9w_disconnect(b,i+1);x9w_error(b,X9Q_NOT_READY);}
  if(b.token){
   // Deadline precedes take, including a ready1 first observed after expiry.
   // Keep the original token on pending/error and drain it even while held.
   if(!b.hold&&x9q_ms()>=b.deadline){
    b.abandoned=true;x9w_disconnect(b,i+1);x9w_error(b,X9Q_NOT_READY);b.hold=true;
    printf("X9 WDS: op %u timed out mux %u; retain token for late CNF, no retry\n",b.op,i+1);
   }
   int ready=x9_md_mipc_take(b.token,response,sizeof(response),&n);
   if(ready==1){b.token=0;x9w_result(b,i+1,response,n);bzero(response,sizeof(response));continue;}
   continue;
  }
  if(b.gone){x9w_clear(b,i+1);continue;}
  if(b.abandoned){if(b.owned&&!b.hold)x9w_deactivate(b,i+1);else if(!b.owned&&!b.hold)x9w_clear(b,i+1);continue;}
  if(!b.hold&&(b.op==X9W_DISC||b.op==X9W_CONFIG||b.op==X9W_RETRY_RESET||b.op==X9W_ACT||
              b.op==X9W_RETRY_QUERY||b.op==X9W_RETRY_WAIT)){x9w_start_next(b,i+1);continue;}
  if(b.op==X9W_IFWAIT){
   x9w_publish(b,i+1);
   if(!b.active&&!b.abandoned&&now>=b.deadline){b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,i+1);printf("X9 WDS: owned call has no matching EIF/IFUP; no host success\n");}
   continue;
  }
  if(b.active&&!b.hold){
   x9w_client &c=x9w_clients[b.slot];unsigned rat=x9w_rat(c);
   bool stats_due=c.stats_interval&&c.stats_mask&&now>=c.report_next;
   bool rat_changed=c.bearer_report&&rat&&rat!=c.last_rat;
   if(stats_due||rat_changed){
    uint8_t req[7]={};x9q_packet report(req);x9d_stats stats={};
    if(stats_due&&x9d_account(i+1,b.route_lifetime,stats))x9w_statistics(report,stats,c.baseline,c.stats_mask);
    if(rat_changed){x9w_rat_tlv(report,0x1d,rat);c.last_rat=rat;}
    report.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,1);
    c.report_next=now+uint64_t(c.stats_interval)*1000;
   }
   if(now>=b.next&& !x9w_check(b))b.next=now+10000;
  }
 }
 x9w_v6_poll();
 x9w_side_poll();
 x9w_retire_poll();
}
static bool x9w_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned n){
 unsigned slot=local-CBQ_PORT_BASE,id=x9q_u16(req+3);x9w_client &c=x9w_clients[slot];
 if(x9p_request(local,port,req,n,c.sim_slot))return true;
 if(id==0xc8){x9a_request(local,port,req,n);return true;}
 // Native StartNetworkIfaceV2 still constructs message20. At 1f1a8ba3c,
 // d0 is passed to getTLV<HandOverIPv6Prefix>, NOT a request constructor.
 // Never interpret a message with that unrelated ID as a data activation.
 if(id!=1&&id!=0x20&&id!=0x21&&id!=0x22&&id!=0x24&&id!=0x2d&&id!=0x30&&id!=0x37&&id!=0x44&&id!=0x4d&&id!=0x86&&id!=0xa2&&id!=0xaf&&id!=0xb0&&id!=0x5569)return false;
 if(!c.family)c.family=4;x9w_bearer *own=nullptr;
 for(x9w_bearer &b:x9w_bearers)if(b.used&&b.slot==slot&&b.generation==x9q_clients[slot].generation)own=&b;
 // Read-only queries from an attached v6 client see the shared call.
 bool v6=!own&&x9w_v6_valid(slot);x9w_bearer *view=own?own:v6?x9w_v6_bearer(c):nullptr;
 unsigned error=0;
 if(id==1){
  x9q_view stats=x9q_tlv(req,n,0x11),bearer=x9q_tlv(req,n,0x15);
  if(stats.p&&(stats.n!=5||(x9q_u32(stats.p+1)&~0x3ffu)))error=X9Q_ARG;
  if(bearer.p&&(bearer.n!=1||bearer.p[0]>1))error=X9Q_ARG;
  if(!error){
   if(stats.p){c.stats_interval=stats.p[0];c.stats_mask=x9q_u32(stats.p+1);c.report_next=0;}
   if(bearer.p){c.bearer_report=bearer.p[0]!=0;c.last_rat=0;}
  }
 }else if(id==0x30){
  // RRC dormancy is not exported by our current MD event source. The wire
  // enum explicitly supports UNKNOWN=0; do not invent ACTIVE from CGATT or
  // idle packet counters (174.txt requests this during data setup).
  if(n!=7)error=X9Q_ARG;
  else{x9q_packet p(req);uint8_t unknown=0;p.tlv(1,&unknown,1);p.send(local,port);return true;}
 }else if(id==0x24||id==0x86){
  x9q_view mask=x9q_tlv(req,n,1);unsigned bits=mask.p&&mask.n==4?x9q_u32(mask.p):0;
  if((id==0x24&&(!mask.p||mask.n!=4||n!=14||(bits&~0x3ffu)))||(id==0x86&&n!=7))error=X9Q_ARG;
  else if(!view||!x9w_data_ready(*view)){
   x9q_packet p(req,15); // OUT_OF_CALL: only retained last-call totals are valid.
   if(c.last_call_bytes_valid){x9w_integer(p,0x1b,c.last_call.tx_bytes,8);x9w_integer(p,0x1c,c.last_call.rx_bytes,8);}
   p.send(local,port);return true;
  }else{
   x9d_stats s={};if(!x9d_account(c.mux,view->route_lifetime,s)){x9q_packet p(req,X9Q_NOT_READY);p.send(local,port);return true;}
   x9q_packet p(req);
   if(id==0x86)c.baseline=s;else x9w_statistics(p,s,c.baseline,bits);
   p.send(local,port);return true;
  }
 }else if(id==0x44||id==0x37){
  unsigned rat=x9w_rat(c);
  if(n!=7)error=X9Q_ARG;
  else if(!view||!x9w_data_ready(*view))error=15;
  else if(!rat)error=X9Q_NOT_READY;
  else{
   x9q_packet p(req);
   if(id==0x44)x9w_rat_tlv(p,1,rat);
   else{uint8_t legacy=rat==(1u<<5)?10:0xff;p.tlv(1,&legacy,1);} // Legacy enum has no NR; never relabel it LTE.
   p.send(local,port);return true;
  }
 }else if(id==0x5569){
  /* 20H392 updateCurrentDataSIM_sync, 0x1f19d751c: NotifyCurrentDataSub,
   * TLV10/u8 subscriber SimSlot, TLV11/u8 reason (1..3). Its callback
   * only checks QMI result. This is host data routing, not a DDS command:
   * accept only the subscription already confirmed by the real 020e.
   * Never ACK a mismatch or retarget an existing bearer. */
  x9q_view sub=x9q_tlv(req,n,0x10),reason=x9q_tlv(req,n,0x11);
  unsigned value=sub.p&&sub.n==1?sub.p[0]:0;
  if(n!=15||!value||value>x9_md_sim_count()||!reason.p||reason.n!=1||reason.p[0]<1||reason.p[0]>3)error=X9Q_ARG;
  else if(!x9ds_data_ready(value-1))error=X9Q_NOT_READY;
  else if(c.sim_slot!=value-1&&(c.starting||own||c.v6_pending||c.v6_active||x9a_client_busy(slot)))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;
   printf("X9 DDS: WDS 5569 client 0x%x routes confirmed default SIM%u reason %u\n",local,value,reason.p[0]);}
 }else if(id==0xaf){
  /* WDS differs from NAS/DMS: u32 1=primary, 2=secondary. DEFAULT/ANY
   * need DDS semantics, so reject them rather than silently selecting SIM1. */
  x9q_view sub=x9q_tlv(req,n,1);unsigned value=sub.p&&sub.n==4?x9q_u32(sub.p):0;
  if(n!=14||!value||value>x9_md_sim_count())error=X9Q_ARG;
  else if(c.sim_slot!=value-1&&(c.starting||own||c.v6_pending||c.v6_active||x9a_client_busy(slot)))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;printf("X9 DUALSIM: WDS client 0x%x bound to SIM%u (not DDS)\n",local,value);}
 }else if(id==0xb0){
  if(n!=7)error=X9Q_ARG;
  else{x9q_packet p(req);uint8_t sub[4]={(uint8_t)(c.sim_slot+1),0,0,0};p.tlv(0x10,sub,4);p.send(local,port);return true;}
 }else if(id==0xa2||id==0x4d){
  if(c.starting||own||c.v6_pending||c.v6_active)error=X9Q_BUSY;
  else if(id==0x4d){x9q_view f=x9q_tlv(req,n,1);
   if(!f.p||f.n!=1||n!=11||!cbp_family_bit(f.p[0]))error=X9Q_ARG;else c.family=f.p[0];
  }else{
   x9q_view mux=x9q_tlv(req,n,0x11),ep=x9q_tlv(req,n,0x10);
   if(!mux.p||mux.n!=1||!cbp_mux_ok(mux.p[0]))error=X9Q_ARG;
   if(ep.p&&(ep.n!=8||x9q_u32(ep.p)!=3||x9q_u32(ep.p+4)!=4))error=X9Q_UNSUPPORTED;
   for(unsigned at=7;at<n;at+=3+x9q_u16(req+at+1))if(req[at]!=0x10&&req[at]!=0x11)error=X9Q_UNSUPPORTED;
   if(!error)c.mux=mux.p[0];
  }
 }else if(id==0x22){
  if(n!=7)error=X9Q_ARG;
  else{x9q_packet p(req);uint8_t status=view&&x9w_data_ready(*view)?2:1;p.tlv(1,&status,1);p.send(local,port);return true;}
 }else if(id==0x2d){
  x9q_view mask=x9q_tlv(req,n,0x10);
  if(!mask.p||mask.n!=4||n!=14)error=X9Q_ARG;
  else if(v6){
   x9w_bearer &b=*view;x9q_packet p(req);uint8_t type=0;p.tlv(0x11,&type,1);p.tlv(0x14,b.apn,(unsigned)strlen(b.apn));
   if(!x9wr_serialize_ipv6(b.runtime,p))error=X9Q_NOT_READY;else{p.send(local,port);return true;}
  }
  else if(!own||!x9w_data_ready(*own))error=15;
  else{
   x9wr_identity current={own->generation,own->result_epoch,own->if_floor,own->sim_slot,own->cid,4,0};
   x9q_packet p(req);uint8_t type=0;p.tlv(0x11,&type,1);p.tlv(0x14,own->apn,(unsigned)strlen(own->apn));
   if(own->result_epoch!=x9a_epoch||!x9w_live(*own)||
      !x9wr_same_identity(own->runtime.identity,current)||!x9wr_serialize_ipv4(own->runtime,p))error=X9Q_NOT_READY;
   else{p.send(local,port);return true;}
  }
 }else if(id==0x21){
  x9q_view h=x9q_tlv(req,n,1);
  if(!h.p||h.n!=4||n!=14)error=X9Q_ARG;
  else if(!own&&c.v6_active&&x9q_u32(h.p)==c.v6_handle){
   // Detach only: the MD call belongs to the v4 client.
   c.v6_active=false;c.v6_handle=0;c.v6_lifetime=0;
   x9q_packet reply(req);reply.send(local,port);x9w_v6_status(slot,false,X9W_VERBOSE_CM,X9W_CM_CLIENT_END);
   printf("X9 WDS: v6 client %u mux %u detached by Stop\n",slot,c.mux);return true;
  }
  else if(!own||!own->handle||x9q_u32(h.p)!=own->handle)error=9;
  else if(own->pending||own->hold||(own->token&&own->op!=X9W_CHECK))error=X9Q_BUSY;
  else{own->pending=true;own->reply_deadline=x9q_ms()+60000;own->len=n;memcpy(own->req,req,n);own->abandoned=true;c.starting=true;
   x9w_set_end(*own,X9W_VERBOSE_CM,X9W_CM_CLIENT_END);
   // Established before GET drain/DEACT submission: no-token release denial
   // cannot leave an accepted Stop waiting forever or extend its host budget.
   if(own->token){x9w_disconnect(*own,c.mux);return true;} // drain periodic GET, then real DEACT
   if(!x9w_deactivate(*own,c.mux))x9w_error(*own,X9Q_NOT_READY);return true;
  }
 }else{
  x9q_view f=x9q_tlv(req,n,0x19);unsigned family=f.p&&f.n==1?f.p[0]:c.family;
  char selected_apn[101]={};
  x9q_view start_apn=x9q_tlv(req,n,0x14);
  printf("X9 WDS: Start msg 0x%04x SIM%u mux %u family %u APN length %u native-ready %u session-ready %u\n",
   id,c.sim_slot+1,c.mux,family,start_apn.n,x9d_ready(),x9w_session_ready);
  if(!cbp_mux_ok(c.mux))error=X9Q_ARG;
  else if(family==6){
   // Attach to the dual-stack call on this mux, now or once the v4 client's
   // call is verified. A v4-only PDN answers CALL_FAILED 210 IPV6_DISALLOWED
   // so the host keeps its v4 call. No MD request is sent for the v6 half.
   if(c.starting||own||c.v6_pending||c.v6_active)error=X9Q_BUSY;
   else if(n>sizeof(c.v6_req))error=X9Q_ARG;
   else{
    c.v6_pending=true;c.v6_len=n;memcpy(c.v6_req,req,n);c.v6_deadline=x9q_ms()+30000;
    if(!x9w_v6_try(slot))printf("X9 WDS: v6 Start client %u mux %u waiting for the v4 call\n",slot,c.mux);
    return true;
   }
  }
  else if(!x9ds_data_ready(c.sim_slot))error=X9Q_NOT_READY;
  else if(c.starting||own||x9w_bearers[c.mux-1].used||x9w_retirements[c.mux-1].pending||
          x9w_retiring_client(slot,x9q_clients[slot].generation)||x9a_busy())error=X9Q_BUSY;
  else if((error=x9w_args(req,n,family))!=0){}
  else if((error=x9p_start(req,n,c.sim_slot,family,selected_apn))!=0){}
  else if(!x9d_ready()||!x9_md_mipc_online()||!x9w_session_ready)error=X9Q_NOT_READY;
  else{
   x9w_bearer &b=x9w_bearers[c.mux-1];bzero(&b,sizeof(b));b.used=b.pending=true;
   b.slot=slot;b.sim_slot=c.sim_slot;b.generation=x9q_clients[slot].generation;b.result_epoch=x9a_epoch;b.len=n;memcpy(b.req,req,n);
   b.reply_deadline=x9q_ms()+200000; // total host bound includes CONFIG/RETRY; stages never extend it
   x9w_selection(req,n,b.start_apn,b.start_apn_n,b.start_profile);
   x9q_view apn=x9q_tlv(req,n,0x14);
   if(x9q_tlv(req,n,0x31).p)memcpy(b.apn,selected_apn,sizeof(b.apn));
   else if(apn.n)memcpy(b.apn,apn.p,apn.n);c.starting=true;
   bzero(selected_apn,sizeof(selected_apn));
   if(x9w_reset_epoch[b.sim_slot]==b.result_epoch||
      (x9w_retries[b.sim_slot].valid&&x9w_retries[b.sim_slot].epoch==b.result_epoch)){
    const x9w_retry_policy &r=x9w_retries[b.sim_slot];
    b.next=r.valid&&r.epoch==b.result_epoch?(x9w_retry_same(b)?r.after:r.floor):0;
    b.op=X9W_RETRY_WAIT;
    printf("X9 WDS: SIM%u retry Start retained under original host deadline; no immediate ACT/BUSY loop\n",b.sim_slot+1);
   }else b.op=X9W_DISC;
   x9w_start_next(b,c.mux);return true;
  }
  bzero(selected_apn,sizeof(selected_apn));
 }
 if(error){
  if(id==0x20||id==0x21)printf("X9 WDS: request msg 0x%04x SIM%u mux %u error %u\n",id,c.sim_slot+1,c.mux,error);
  x9bb_reply_error(local,port,req,n,(uint16_t)error);return true;
 }
 x9q_packet reply(req);reply.send(local,port);return true;
}
