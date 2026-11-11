/* Native IPv4 WDS dialler, single QMI/DPMAIF owner (NOT the AT job queue).
 * Runtime: libmtkmipc-ril + libmipc, ttyCMIPC1/RX216/TX217.
 * OPEN301/register307 -> GET_PDP_CID219 -> explicit no-auth ACT201 (REUSE=2, fallback=0)
 * -> GET_CALL205 + actual EIF4302/IFUP -> bind.
 * Native decodeDataCallProfile reads ID100 separately from IF125;
 * ccci_dpmaif_com writes channel_id=ccmni_idx. Never use CID-1 as a route.
 * Explicit, RAM-profile or network-default APN, IPv4, no-auth only; no IMS/IPv6 SLAAC translation.
 * Old MIPC headers differ from this RIL at tags128..12b: TRANS_ID is NOT a
 * subnet mask. Missing gateway/netmask is omitted, not invented. APN/IP stays
 * in RAM, never diagnostics. Sent activation survives host timeout/DEL_CLIENT
 * until its late CNF and cleanup. REUSE=2 does not prove a globally fresh CID:
 * saved P77 can reuse same-APN calls, and normal DEACT clears the MD user's
 * entire reference slot. User/CID lifetime still needs qualification before
 * treating this dialler as ready for a new boot candidate. No cancel/reset.
 */
struct x9w_client {
 uint8_t mux, family, sim_slot, stats_interval; bool starting,bearer_report,last_call_bytes_valid;
 uint32_t stats_mask,last_rat;uint64_t report_next;
 x9d_stats baseline,last_call;
};
static x9w_client x9w_clients[CBQ_MAX_CLIENTS];
enum { X9W_DISC=1, X9W_ACT, X9W_CHECK, X9W_DEACT, X9W_IFWAIT };
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
struct x9w_frame {
 uint8_t p[512];unsigned n;bool good;
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
 unsigned slot,op,min_cid,max_cid,channel,mtu,dns_count;
 uint64_t generation,token,deadline,reply_deadline,next,if_floor,result_epoch,route_lifetime;
 uint32_t handle,len,trans_id;
 uint8_t cid,sim_slot,req[X9Q_REQ],ip[4],dns[2][4];
 char apn[101];
 x9wr_identity query; x9wr_get get_result; x9wr_settings runtime;
};
static x9w_bearer x9w_bearers[CBP_INTERFACES];
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
  x9w_init_token=0;unsigned result=2,version=0,mask=stage==1?0xff:1u<<((stage-2)/2);
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
  if(stage==1+2*x9_md_sim_count()){
   x9w_session_ready=true;printf("X9 MIPC: v2 OPEN + DEACT/EIF subscriptions confirmed for %u SIM(s); CID ownership unqualified\n",x9_md_sim_count());return;}
 }
 if(!transport_ok||x9w_init_hold||x9w_session_ready)return;
 unsigned stage=x9w_init_stage+1;
 x9w_frame f(stage==1?0x301:0x307,stage==1?0xff:1u<<((stage-2)/2));
 if(stage==1){static const char name[]="X9WDS";f.word(0x100,2);f.tlv(0x101,name,sizeof(name));f.byte(0x102,1);}
 else{unsigned id=stage%2==0?0x4202:0x4302;uint8_t bytes[2]={(uint8_t)id,(uint8_t)(id>>8)};f.tlv(0x100,bytes,2);}
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
static void x9w_error(x9w_bearer &b,unsigned error){
 if(b.pending&&x9w_live(b)){
  x9q_packet p(b.req,(uint16_t)error);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);
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
 if(!x9w_byte(p,n,0x10d,dns)||dns>3||b.result_epoch!=x9a_epoch||!x9w_live(b)||!x9wr_same_identity(b.query,current)||
    !x9wr_decode_get(p,n,b.query,get)||get.channel!=b.channel||get.trans_id!=b.trans_id||
    !get.v4.mtu_present||get.v4.mtu!=b.mtu||memcmp(get.v4.address,b.ip,4))return false;
 // Still corroborate the earlier ACT facts; do not retarget a live route.
 b.get_result=get;return true;
}
static bool x9w_settings(x9w_bearer &b,const uint8_t *p,unsigned n){
 unsigned cid,type,channel,mtu,count,dns;
 if(!x9w_byte(p,n,0x100,cid)||cid!=b.cid||!x9w_byte(p,n,0x102,type)||type!=1||
  !x9w_word(p,n,0x125,channel)||channel>=20||!x9w_word(p,n,0x123,mtu)||mtu<68||mtu>X9D_TX_SIZE||
  !x9w_byte(p,n,0x103,count)||count!=1||!x9w_byte(p,n,0x10d,dns)||dns>4)return false;
 x9q_view ip=x9w_tlv(p,n,0x8104);
 if(!ip.p||ip.n!=4||!ip.p[0]||ip.p[0]>=224||ip.p[0]==127)return false;
 uint8_t addresses[2][4]={};
 for(unsigned i=0;i<dns;i++){x9q_view d=x9w_tlv(p,n,0x810e + i);if(!d.p||d.n!=4)return false;if(i<2)memcpy(addresses[i],d.p,4);}
 if(b.mtu&&(channel!=b.channel||mtu!=b.mtu||memcmp(b.ip,ip.p,4)))return false; // no live retargeting
 b.channel=channel;b.mtu=mtu;memcpy(b.ip,ip.p,4);b.dns_count=dns>2?2:dns;memcpy(b.dns,addresses,sizeof(addresses));
 return true; // no gateway/netmask derived from stale versioned tags
}
static void x9w_handle_reply(x9w_bearer &b){
 uint32_t h=b.handle;uint8_t bytes[4]={(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24)};
 x9q_packet p(b.req);p.tlv(1,bytes,4);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);x9w_pending_done(b);
}
static void x9w_publish(x9w_bearer &b,unsigned mux){
 x9w_host_expire(b,mux,x9q_ms()); // every publication entry owns its deadline gate
 if(b.abandoned||b.hold||!b.pending)return;
 const x9w_interface &in=x9w_interfaces[b.sim_slot][b.channel];
 if(!in.valid||in.serial<=b.if_floor||in.trans_id!=b.trans_id||in.cmd!=0)return;
 x9wr_identity current={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,4,0};
 x9wr_settings runtime={};
 if(b.result_epoch!=x9a_epoch||!x9wr_match_settings(b.get_result,in.facts,current,runtime)||
    !x9wr_ipv4_facts(runtime)||in.mtu!=b.mtu||memcmp(in.ip,b.ip,4)||!b.pending||!x9w_live(b)||
    x9w_next_handle>=0xfffffffeu||x9w_retirements[mux-1].pending||!x9d_bind(mux,b.channel,1,b.mtu,b.route_lifetime)){
  b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
 }
 b.runtime=runtime;b.handle=++x9w_next_handle;b.active=true;b.op=0;b.next=x9q_ms()+10000;
 x9w_client &c=x9w_clients[b.slot];bzero(&c.baseline,sizeof(c.baseline));c.report_next=0;c.last_rat=0;
 c.last_call_bytes_valid=false;bzero(&c.last_call,sizeof(c.last_call));
 printf("X9 WDS: SIM%u PDP activated call %u -> ccmni%u -> mux %u IPv4 MTU %u, ACT+GET+EIF/IFUP verified, DPMAIF bound\n",b.sim_slot+1,b.cid,b.channel,mux,b.mtu);
 x9w_handle_reply(b);
 // Connection notification is emitted only AFTER real bind; not CGATT alone.
 uint8_t req[7]={};x9q_packet connected(req);uint8_t status[2]={2,0},family=4;
 connected.tlv(1,status,2);connected.tlv(0x12,&family,1);
 connected.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,0x22);
}
static void x9w_result(x9w_bearer &b,unsigned mux,const uint8_t *p,unsigned n){
 x9w_host_expire(b,mux,x9q_ms()); // late ready1 cannot restore host success
 unsigned op=b.op;b.op=0;unsigned result;
 unsigned expected=op==X9W_DISC?0x21a:op==X9W_ACT?0x202:op==X9W_CHECK?0x206:0x204;
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
 if(op==X9W_DISC){
  if(result||b.abandoned||!x9w_byte(p,n,0x100,b.min_cid)||!x9w_byte(p,n,0x101,b.max_cid)||b.min_cid>b.max_cid||b.max_cid>=255){
   x9w_error(b,result==1?X9Q_BUSY:X9Q_NOT_READY);x9w_clear(b,mux);return;
  }
  if(!x9d_ready()||!x9_md_mipc_online()){x9w_error(b,X9Q_NOT_READY);x9w_clear(b,mux);return;}
  x9w_frame f(0x201,1u<<b.sim_slot);f.tlv(0x101,b.apn,(unsigned)strlen(b.apn)+1);
  f.word(0x102,1);f.byte(0x103,1);f.byte(0x104,1);f.byte(0x105,0);
  // Loaded RIL 2b8740/8750 sends strlen+1 credentials even for no-auth.
  // MD 91326e0e..e44 requires both TLVs PRESENT to set full-parameters +98.
  // Omitting them lets its APN cache replace protocol/auth/fallback. These
  // are explicit empty strings, not zero-length TLVs or invented credentials.
  static const char no_credentials[]="";
  f.tlv(0x8106,no_credentials,sizeof(no_credentials));
  f.tlv(0x8107,no_credentials,sizeof(no_credentials));
  f.byte(0x108,0);f.byte(0x10a,2);
  b.if_floor=x9w_if_serial;
  if(!x9w_send(b,f,X9W_ACT,120000)){x9w_error(b,X9Q_BUSY);x9w_clear(b,mux);}
  else printf("X9 WDS: requesting IPv4 call mux %u, explicit no-auth, reuse flag 2 fallback 0 (not a fresh-CID guarantee)\n",mux);return;
 }
 if(op==X9W_ACT){
  unsigned cid;
  if(result&&result!=0x150202&&result!=0x150203&&result!=0x150204){
   // Ordinary failure has no owned call. An unexpected call ID on that failure
   // is ambiguous: reserve it without deactivating a possibly foreign call.
   if(x9w_byte(p,n,0x100,cid)&&cid>=b.min_cid&&cid<=b.max_cid){b.hold=true;b.abandoned=true;x9w_error(b,14);}
   else{x9w_error(b,result==1?X9Q_BUSY:14);x9w_clear(b,mux);}return;
  }
  if(!x9w_byte(p,n,0x100,cid)||cid<b.min_cid||cid>b.max_cid){b.hold=true;b.abandoned=true;x9w_error(b,X9Q_INTERNAL);return;}
  for(const x9w_bearer &other:x9w_bearers)if(&other!=&b&&other.used&&other.owned&&other.sim_slot==b.sim_slot&&other.cid==cid){
   b.hold=true;b.abandoned=true;x9w_error(b,X9Q_INTERNAL);return; // don't deactivate sibling
  }
  b.cid=(uint8_t)cid;b.owned=true;
  // Actual RIL treats 150202/150204 as partial success and decodes 150203's
  // partial profile. This IPv4-only dialler cleans up instead of falling back.
  if(result||b.gone||b.abandoned||!x9w_settings(b,p,n)||
     !x9w_word(p,n,0x12a,b.trans_id)||b.trans_id%100!=b.channel||!x9d_ready()){
   b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  if(!x9w_check(b)){b.abandoned=true;x9w_error(b,X9Q_BUSY);x9w_deactivate(b,mux);}return;
 }
 if(op==X9W_CHECK){
  if(b.gone){x9w_clear(b,mux);return;}
  if(b.abandoned){x9w_deactivate(b,mux);return;}
  if(result||!x9w_get_settings(b,p,n)||!x9d_ready()){
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
 x9w_interface &in=x9w_interfaces[sim][trans%100];bzero(&in,sizeof(in));
 in.serial=++x9w_if_serial;in.trans_id=trans;in.cmd=cmd;
 if(cmd==0){
  unsigned v6=0;
  // Preserve the old explicit IPv6-count-zero gate, then the typed EIF facts
  // including the capture card epoch. Optional metadata is not new readiness.
  // IPv6 remains unsupported; no fulladdr tail or tag103 fallback MTU.
  if(x9w_byte(p,n,0x106,v6)&&!v6&&x9wr_decode_ifup(p,n,sim,x9a_epoch,in.serial,in.facts)&&
     in.facts.v4.address_present&&!in.facts.v6.address_present){
   in.mtu=in.facts.v4.mtu;memcpy(in.ip,in.facts.v4.address,4);
   in.valid=in.mtu>=68&&in.mtu<=X9D_TX_SIZE;
  }
  // Foreign IMS/IPv6 events do not poison the IPv4 dialler. An owned route
  // with an unsupported IFUP remains disconnected and is cleaned up.
 }else in.valid=true;
 printf("X9 WDS: MD SIM%u EIF/IFST ccmni%u command %u (payload private)\n",sim+1,trans%100,cmd);
 for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
  if(b.used&&b.owned&&b.sim_slot==sim&&b.trans_id==trans&&in.serial>b.if_floor&&
     (cmd!=0||!in.valid||(b.active&&(in.mtu!=b.mtu||memcmp(in.ip,b.ip,4))))){
   b.abandoned=true;x9w_disconnect(b,m+1);x9w_error(b,X9Q_NOT_READY);
  }
 }return true;
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
  else if(!own||!x9w_data_ready(*own)){
   x9q_packet p(req,15); // OUT_OF_CALL: only retained last-call totals are valid.
   if(c.last_call_bytes_valid){x9w_integer(p,0x1b,c.last_call.tx_bytes,8);x9w_integer(p,0x1c,c.last_call.rx_bytes,8);}
   p.send(local,port);return true;
  }else{
   x9d_stats s={};if(!x9d_account(c.mux,own->route_lifetime,s)){x9q_packet p(req,X9Q_NOT_READY);p.send(local,port);return true;}
   x9q_packet p(req);
   if(id==0x86)c.baseline=s;else x9w_statistics(p,s,c.baseline,bits);
   p.send(local,port);return true;
  }
 }else if(id==0x44||id==0x37){
  unsigned rat=x9w_rat(c);
  if(n!=7)error=X9Q_ARG;
  else if(!own||!x9w_data_ready(*own))error=15;
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
  else if(c.sim_slot!=value-1&&(c.starting||own||x9a_client_busy(slot)))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;
   printf("X9 DDS: WDS 5569 client 0x%x routes confirmed default SIM%u reason %u\n",local,value,reason.p[0]);}
 }else if(id==0xaf){
  /* WDS differs from NAS/DMS: u32 1=primary, 2=secondary. DEFAULT/ANY
   * need DDS semantics, so reject them rather than silently selecting SIM1. */
  x9q_view sub=x9q_tlv(req,n,1);unsigned value=sub.p&&sub.n==4?x9q_u32(sub.p):0;
  if(n!=14||!value||value>x9_md_sim_count())error=X9Q_ARG;
  else if(c.sim_slot!=value-1&&(c.starting||own||x9a_client_busy(slot)))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;printf("X9 DUALSIM: WDS client 0x%x bound to SIM%u (not DDS)\n",local,value);}
 }else if(id==0xb0){
  if(n!=7)error=X9Q_ARG;
  else{x9q_packet p(req);uint8_t sub[4]={(uint8_t)(c.sim_slot+1),0,0,0};p.tlv(0x10,sub,4);p.send(local,port);return true;}
 }else if(id==0xa2||id==0x4d){
  if(c.starting||own)error=X9Q_BUSY;
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
  else{x9q_packet p(req);uint8_t status=own&&x9w_data_ready(*own)?2:1;p.tlv(1,&status,1);p.send(local,port);return true;}
 }else if(id==0x2d){
  x9q_view mask=x9q_tlv(req,n,0x10);
  if(!mask.p||mask.n!=4||n!=14)error=X9Q_ARG;
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
   // Only the IPv4 dialler exists (no prefix/RA/DAD path). Answer like a
   // modem that disallows IPv6 on this PDN so the host keeps its IPv4 call
   // instead of seeing an unexplained failure. No MD request is sent.
   x9q_packet p(req,14); // CALL_FAILED
   x9w_end_reason(p,X9W_END_UNSPECIFIED,X9W_VERBOSE_INTERNAL,X9W_INTERNAL_IPV6_DISALLOWED);
   p.send(local,port);
   printf("X9 WDS: Start SIM%u mux %u IPv6 refused: CALL_FAILED internal 210 IPV6_DISALLOWED (IPv4-only dialler)\n",c.sim_slot+1,c.mux);
   return true;
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
   b.reply_deadline=x9q_ms()+200000; // DISC10 + ACT120 + GET30 + IFWAIT30
   x9q_view apn=x9q_tlv(req,n,0x14);
   if(x9q_tlv(req,n,0x31).p)memcpy(b.apn,selected_apn,sizeof(b.apn));
   else if(apn.n)memcpy(b.apn,apn.p,apn.n);c.starting=true;
   bzero(selected_apn,sizeof(selected_apn));
   x9w_frame query(0x219,1u<<b.sim_slot);
   if(!x9w_send(b,query,X9W_DISC,10000)){x9w_error(b,X9Q_BUSY);x9w_clear(b,c.mux);}return true;
  }
  bzero(selected_apn,sizeof(selected_apn));
 }
 if(error){
  if(id==0x20||id==0x21)printf("X9 WDS: request msg 0x%04x SIM%u mux %u error %u\n",id,c.sim_slot+1,c.mux,error);
  x9bb_reply_error(local,port,req,n,(uint16_t)error);return true;
 }
 x9q_packet reply(req);reply.send(local,port);return true;
}
