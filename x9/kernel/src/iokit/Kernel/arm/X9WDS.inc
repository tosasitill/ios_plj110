/* Native WDS owner: OPEN/subscriptions -> DISC/CONFIG/retry -> ACT(PDP3)
 * -> token-scoped GET + EIF -> physical DPMAIF route -> independent leases.
 * IPv4/IPv6 Starts may arrive in either order. Only the last host reference
 * releases the physical call; client deletion never cancels a wire token.
 * Updated settings require current CID/channel/trans + corroborating MD
 * facts. Native WDS22 requests IP reconfiguration; WDS8c is IMS-only on
 * 20H392. ACT/bind/DRB completion do not establish end-to-end connectivity.
 */
struct x9w_client {
 uint8_t mux, family, sim_slot, stats_interval; bool starting,bearer_report,last_call_bytes_valid;
 // Symmetric host leases; physical control generation is independent of QMI slot reuse.
 bool lease_pending,lease_active;uint32_t lease_handle,lease_len;uint64_t lease_generation,lease_deadline;uint64_t client_generation;uint8_t lease_req[X9Q_REQ];bool reconfigure;uint64_t reconfigure_deadline;
 uint32_t stats_mask,last_rat;uint64_t report_next;
 x9d_stats baseline,last_call;
};
static x9w_client x9w_clients[CBQ_MAX_CLIENTS];
enum { X9W_DISC=1, X9W_ACT, X9W_CHECK, X9W_DEACT, X9W_IFWAIT, X9W_CONFIG, X9W_RETRY_RESET,
 X9W_RETRY_QUERY, X9W_RETRY_WAIT };
// AP accounting outlives the control bearer and client.204/4202 may consume
// control state, but cannot erase a published payload's original ledger.
struct x9w_retirement { uint64_t lifetime,control_generation,generations[CBQ_MAX_CLIENTS]; bool members[CBQ_MAX_CLIENTS],pending; };
static x9w_retirement x9w_retirements[CBP_INTERFACES];
static bool x9w_retiring_client(unsigned slot,uint64_t generation){
 for(const x9w_retirement &r:x9w_retirements)if(r.pending&&slot<CBQ_MAX_CLIENTS&&r.members[slot]&&r.generations[slot]==generation)return true;
 return false;
}
static bool x9w_accounting_pending(){
 for(const x9w_retirement &r:x9w_retirements)if(r.pending)return true;
 return x9d_retiring(); // AP-only; never proves MD release or RX drain
}
static void x9w_retire_poll(){
 for(unsigned i=0;i<CBP_INTERFACES;i++){x9w_retirement &r=x9w_retirements[i];if(!r.pending)continue;
  x9d_stats stats={};if(!x9d_retire_finish(i+1,r.lifetime,stats))continue;
  for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(r.members[slot]&&x9q_clients[slot].generation==r.generations[slot]&&x9w_clients[slot].lease_generation==r.control_generation){
   x9w_clients[slot].last_call=stats;x9w_clients[slot].last_call_bytes_valid=true;
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
 uint64_t host_generation,setup_deadline,change_serial,query_serial;
 uint64_t account_generations[CBQ_MAX_CLIENTS];bool account_members[CBQ_MAX_CLIENTS];unsigned reconfigure_mask,refresh_mask;
 // MTU from ACT 0x123; 0 = network gave none (MD adds 0x123 only when its
 // +0x2c8 flag is set, 913277822). mtu is the effective route MTU.
 unsigned md_mtu;
 uint32_t len,trans_id,failure_result;
 uint8_t cid,sim_slot,req[X9Q_REQ],ip[4],dns[2][4];
 char apn[101];
 // Frozen Start selection: sibling leases must match this physical call.
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
static uint64_t x9w_next_generation;
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
  x9w_init_token=0;unsigned result=2,version=0,mask=stage==1?0xff:1u<<((stage-2)/4);
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
  if(stage==1+4*x9_md_sim_count()){
   {int v=0;PE_parse_boot_argn("x9_eif_req",&v,sizeof(v));x9w_netagent_req=v!=0;}
   {int v=0;PE_parse_boot_argn("x9_ims_proute",&v,sizeof(v));x9w_packet_route_enabled=v==1;}
   x9w_packet_route_pending=x9w_packet_route_enabled;
   x9w_session_ready=true;printf("X9 MIPC: v2 OPEN + DEACT/EIF/MOD subscriptions confirmed for %u SIM(s); CID ownership unqualified\n",x9_md_sim_count());return;}
 }
 if(!transport_ok||x9w_init_hold||x9w_session_ready)return;
 unsigned stage=x9w_init_stage+1;
 x9w_frame f(stage==1?0x301:0x307,stage==1?0xff:1u<<((stage-2)/4));
 if(stage==1){static const char name[]="X9WDS";f.word(0x100,2);f.tlv(0x101,name,sizeof(name));f.byte(0x102,1);}
 // Per SIM: DEACT4202, EIF4302, optional packet-route4220, DATA_MOD4203.
 else{static const unsigned ids[4]={0x4202,0x4302,0x4220,0x4203};unsigned id=ids[(stage-2)%4];
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
 if(family!=4&&family!=6)return X9Q_UNSUPPORTED;
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
// Physical control liveness never depends on whichever host client started first.
static bool x9w_live(const x9w_bearer &b){return b.used&&b.generation;}
static bool x9w_reply_live(const x9w_bearer &b){return b.slot<CBQ_MAX_CLIENTS&&x9q_clients[b.slot].generation==b.host_generation;}
static bool x9w_member(unsigned slot,const x9w_bearer &b){
 const x9w_client &c=x9w_clients[slot];
 return b.used&&c.client_generation==x9q_clients[slot].generation&&c.lease_generation==b.generation&&
  cbp_mux_ok(c.mux)&&(&x9w_bearers[c.mux-1]==&b)&&c.sim_slot==b.sim_slot&&(c.lease_pending||c.lease_active);
}
static unsigned x9w_refs(const x9w_bearer &b,bool pending=true){
 unsigned mask=0;for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)
  if(x9w_member(slot,b)&&(x9w_clients[slot].lease_active||pending))mask|=cbp_family_bit(x9w_clients[slot].family);
 return mask;
}
static unsigned x9w_families(const x9wr_settings &r){return (x9wr_ipv4_facts(r)?1u:0u)|(x9wr_ipv6_attach_ready(r)?2u:0u);}
static bool x9w_sync_route(x9w_bearer &b,unsigned mux){
 b.reconfigure_mask &= x9w_refs(b,false);
 return !b.active||x9d_update(mux,b.route_lifetime,x9w_refs(b,false)&x9w_families(b.runtime)&~(b.reconfigure_mask|b.refresh_mask),b.runtime.v4.mtu,b.runtime.v6.mtu);
}
// Requests can run before the next bearer poll observes a changed card pair.
// A retained handle or accounting ledger never grants current-call success.
static bool x9w_data_ready(const x9w_bearer &b){
 return b.used&&b.active&&!b.abandoned&&!b.hold&&b.result_epoch==x9a_epoch&&
  x9w_live(b)&&x9d_ready()&&x9_md_mipc_online();
}
// Owner-only, value-only probes. All outcomes are eligible; first two and
// bounded transitions print immediately, repeated outcomes get a 3s heartbeat.
enum { X9WP_SEND, X9WP_CNF, X9WP_GET, X9WP_PUBLISH, X9WP_STATE, X9WP_TAKE, X9WP_ERROR, X9WP_COUNT };
struct x9w_probe_rate { uint64_t generation,next,seen; unsigned emitted; int value; const char *reason; };
static x9w_probe_rate x9w_probe_rates[CBP_INTERFACES][X9WP_COUNT];
static void x9w_probe(const x9w_bearer &b,unsigned site,const char *reason,int value,const x9wr_settings *candidate=nullptr){
 unsigned mux=(unsigned)(&b-x9w_bearers)+1;if(!cbp_mux_ok(mux)||site>=X9WP_COUNT)return;
 x9w_probe_rate &r=x9w_probe_rates[mux-1][site];uint64_t now=x9q_ms();
 if(r.generation!=b.generation){bzero(&r,sizeof(r));r.generation=b.generation;}
 bool changed=r.reason!=reason||r.value!=value;r.seen++;
 if(r.seen>2&&!(changed&&r.emitted<16)&&now<r.next)return;
 r.reason=reason;r.value=value;r.emitted++;r.next=now+3000;
 const x9w_interface *in=b.sim_slot<2&&b.channel<20?&x9w_interfaces[b.sim_slot][b.channel]:nullptr;
 const x9wr_settings &settings=candidate?*candidate:b.runtime;
 // XNU's kernel os_log buffer is 256B and log_encode.c asserts (instead of
 // dropping) near the limit, so 38 args in one printf panicked (221). Keep
 // each line under ~130B of args (<=17, one string), tagged mux/site/seen.
 printf("X9 CELL WDS: site %u why %s value %d seen %llu SIM%u mux %u gen %llu epoch %llu/%llu op %u token %llu\n",
  site,reason,value,r.seen,b.sim_slot+1,mux,b.generation,b.result_epoch,x9a_epoch,b.op,b.token);
 printf("X9 CELL WDS:  mux %u site %u seen %llu flags %u%u%u%u%u refs %u cid %u channel %u trans %u floor %llu EIF %u/%u/%llu/%u\n",
  mux,site,r.seen,b.used,b.owned,b.active,b.abandoned,b.hold,x9w_refs(b),b.cid,b.channel,b.trans_id,b.if_floor,
  in?in->valid:0,in?in->cmd:0,in?in->serial:0,in?in->trans_id:0);
 printf("X9 CELL WDS:  mux %u site %u seen %llu GET %u/%u/%u candidate %u matched %u families %u route %llu serial %llu/%llu\n",
  mux,site,r.seen,b.get_result.identity.cid,b.get_result.channel,b.get_result.trans_id,candidate!=nullptr,settings.matched,
  x9w_families(settings),b.route_lifetime,b.query_serial,b.change_serial);
 printf("X9 CELL WDS:  mux %u site %u seen %llu retire %u handle-room %u wait-left-ms %llu setup-left-ms %llu MD-session unobserved\n",
  mux,site,r.seen,x9w_retirements[mux-1].pending,x9w_next_handle<0xfffffffeu,
  b.deadline>now?b.deadline-now:0,b.setup_deadline>now?b.setup_deadline-now:0);
}
static void x9w_wire_probe(const x9w_bearer &b,const uint8_t *p,unsigned n,unsigned id){
 // Called only after the actual CNF wire/result gate, never on malformed TLVs.
 unsigned cid=~0u,type=~0u,channel=~0u,trans=~0u,v4=~0u,v6=~0u,mtu4=0,mtu6=0;
 unsigned result=~0u;x9w_word(p,n,0,result);x9w_byte(p,n,0x100,cid);x9w_byte(p,n,0x102,type);
 x9w_word(p,n,0x125,channel);x9w_word(p,n,id==0x202?0x12a:0x129,trans);
 x9w_byte(p,n,0x103,v4);x9w_byte(p,n,0x108,v6);
 bool m4=x9w_word(p,n,0x123,mtu4),m6=x9w_word(p,n,0x124,mtu6);
 printf("X9 CELL CNF: SIM%u mux %u gen %llu id %04x result %u CID %u expected %u range %u-%u PDP %u channel %u/%u trans %u/%u v4/v6-count %u/%u MTU-present %u/%u MTU %u/%u; AP-fields-not-MD-session\n",
  b.sim_slot+1,(unsigned)(&b-x9w_bearers)+1,b.generation,id,result,cid,b.cid,b.min_cid,b.max_cid,
  type,channel,b.channel,trans,b.trans_id,v4,v6,m4,m6,mtu4,mtu6);
}
// QMI WDS call end reasons (libqmi qmi-enums-wds.h). The 20H392 driver
// names the internal block 201.. as INTERNAL_ERROR, CALL_ENDED, ..., 206
// NW_INITIATED_TERMINATION, 208 IPV4_CALL_DISALLOWED, 210 IPV6_DISALLOWED and
// logs Start failures as reason/verboseReasonType/verboseReason. A bare
// error gives it no reason to tell a v4-only PDN apart from a failed call.
enum : uint16_t { X9W_END_UNSPECIFIED=1,X9W_END_CLIENT=2 };
enum : uint16_t { X9W_VERBOSE_INTERNAL=2,X9W_VERBOSE_CM=3 };
enum : uint16_t { X9W_INTERNAL_ERROR=201,X9W_INTERNAL_NW_TERMINATION=206,X9W_INTERNAL_IPV4_DISALLOWED=208,
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
// 238 panicked with a kernel stack overflow on the bridge thread: these logs
// were inlined into the large x9w_request/x9w_poll frames. Keep them
// out-of-line and buffer-free so the hot frames do not grow.
__attribute__((noinline)) static void x9w_log_event_report(unsigned local,const uint8_t *req,unsigned n){
 static unsigned logged;if(logged>=12)return;logged++;
 unsigned tag[4]={0,0,0,0},val[4]={0,0,0,0},count=0;
 for(unsigned at=7;at+3<=n&&count<4;at+=3+x9q_u16(req+at+1)){unsigned k=x9q_u16(req+at+1);
  tag[count]=req[at];val[count]=k&&at+3<n?req[at+3]:0xffu;count++;}
 printf("X9 WDS: client 0x%x SetEventReport tlvs %u: %02x=%02x %02x=%02x %02x=%02x %02x=%02x\n",
  local,count,tag[0],val[0],tag[1],val[1],tag[2],val[2],tag[3],val[3]);
}
__attribute__((noinline)) static void x9w_log_bearer_query(unsigned local,unsigned id,unsigned sim,unsigned rat){
 static unsigned logged;if(logged>=16)return;logged++;
 printf("X9 WDS: client 0x%x bearer-tech query 0x%02x SIM%u rat mask 0x%x\n",local,id,sim+1,rat);
}
__attribute__((noinline)) static void x9w_log_bearer_report(unsigned slot,unsigned rat){
 printf("X9 WDS: client slot %u bearer-tech report rat mask 0x%x\n",slot,rat);
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
 if(b.pending&&x9w_reply_live(b))x9w_clients[b.slot].starting=false;b.pending=false;b.reply_deadline=0;b.len=0;bzero(b.req,sizeof(b.req));
}
static void x9w_lease_fail(unsigned slot,uint16_t verbose);
static void x9w_lease_status(unsigned slot,bool connected,uint16_t type,uint16_t verbose,bool reconfigure=false);
static void x9w_error(x9w_bearer &b,unsigned error){
 x9w_probe(b,X9WP_ERROR,"host-error",(int)error);
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++)if(x9w_member(i,b)&&x9w_clients[i].lease_pending)x9w_lease_fail(i,X9W_INTERNAL_ERROR);
 if(b.pending&&x9w_reply_live(b)){
  x9q_packet p(b.req,(uint16_t)error);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);
  printf("X9 WDS: response msg 0x%04x error %u; ownership retained %u op %u\n",x9q_u16(b.req+3),error,b.owned,b.op);
 }x9w_pending_done(b);
}
static void x9w_disconnect(x9w_bearer &b,unsigned mux){
 if(b.route_lifetime&&cbp_mux_ok(mux)){
  x9w_retirement &r=x9w_retirements[mux-1];
  if(!r.pending){r.lifetime=b.route_lifetime;r.control_generation=b.generation;memcpy(r.generations,b.account_generations,sizeof(r.generations));memcpy(r.members,b.account_members,sizeof(r.members));r.pending=true;}
  else if(r.lifetime!=b.route_lifetime)x9d_fail("WDS retirement ledger collision");
  b.route_lifetime=0;
 }
 b.active=false;bzero(&b.runtime,sizeof(b.runtime));x9d_unbind(mux);
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(x9w_member(slot,b)&&x9w_clients[slot].lease_active){
  x9w_client &c=x9w_clients[slot];c.lease_active=false;c.lease_handle=0;c.reconfigure=false;
  x9w_lease_status(slot,false,b.end_type?b.end_type:X9W_VERBOSE_INTERNAL,b.end_type?b.end_verbose:X9W_INTERNAL_ERROR);
 }
}
static void x9w_host_expire(x9w_bearer &b,unsigned mux,uint64_t now){
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(x9w_member(slot,b)){
  x9w_client &c=x9w_clients[slot];
  if(c.lease_pending&&now>=c.lease_deadline)x9w_lease_fail(slot,X9W_INTERNAL_ERROR);
 }
 if(!b.active&&b.setup_deadline&&now>=b.setup_deadline&&!b.abandoned){
  b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);
 }
 if(!x9w_refs(b)&&!b.abandoned){b.abandoned=true;x9w_disconnect(b,mux);}
 if(!b.pending||!b.reply_deadline||now<b.reply_deadline)return;
 b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);
 printf("X9 WDS: host Stop deadline mux %u; original control/data retirement retained\n",mux);
}
static void x9w_clear(x9w_bearer &b,unsigned mux){x9w_disconnect(b,mux);x9w_pending_done(b);bzero(&b,sizeof(b));}
static bool x9w_send(x9w_bearer &b,x9w_frame &f,unsigned op,unsigned timeout){
 b.token=f.submit();if(!b.token){x9w_probe(b,X9WP_SEND,"enqueue-failed",(int)op);return false;}
 b.op=op;b.deadline=x9q_ms()+timeout;x9w_probe(b,X9WP_SEND,"enqueued",(int)op);return true;
}
static bool x9w_deactivate(x9w_bearer &b,unsigned mux){
 x9w_disconnect(b,mux);
 // A token/old CID is not authority to mutate the replacement card epoch.
 if(b.result_epoch!=x9a_epoch){b.hold=true;return false;}
 if(b.md_down&&b.owned&&!b.token&&!b.hold){
  // No positive evidence the CID is still ours: release locally. At worst
  // one usr1 ref byte stays; a later same-APN ACT reuses it and any 203 on
  // that PDN zeroes the whole byte (913d839e), so nothing leaks for good.
  if(b.pending&&x9q_u16(b.req+3)==0x21&&x9w_reply_live(b)){x9q_packet r(b.req);r.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
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
 x9wr_identity query={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,0,3};
 x9w_frame f(0x205,1u<<b.sim_slot);f.byte(0x101,b.cid);
 if(!x9w_send(b,f,X9W_CHECK,30000))return false;
 b.query=query;b.query_serial=b.change_serial;return true;
}
static bool x9w_get_settings(x9w_bearer &b,const uint8_t *p,unsigned n){
 x9wr_identity current={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,0,3};
 x9wr_get get={};unsigned dns=0;
 if(!x9wr_optional_byte(p,n,0x10d,dns)||dns>4||b.result_epoch!=x9a_epoch||!x9w_live(b)||!x9wr_same_identity(b.query,current)){
  x9w_probe(b,X9WP_GET,"identity-or-dns-reject",(int)dns);
  printf("X9 WDS: GET205 identity/dns gate failed (dns %u)\n",dns);return false;}
 if(!x9wr_decode_get(p,n,b.query,get)){x9w_probe(b,X9WP_GET,"decode-reject",0);printf("X9 WDS: GET205 decode failed\n");return false;}
 // CID/channel/trans remain the immutable physical identity. Addresses/DNS/
 // MTU are mutable settings, not proof that a different call owns this CID.
 if(get.channel!=b.channel||get.trans_id!=b.trans_id){x9w_probe(b,X9WP_GET,"channel-or-trans-reject",0);return false;}
 b.get_result=get;x9w_probe(b,X9WP_GET,"identity-pass",1);return true;
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
 unsigned cid,type,channel;
 if(!x9w_byte(p,n,0x100,cid)||cid!=b.cid||!x9w_byte(p,n,0x102,type)||type<1||type>3||
    !x9w_word(p,n,0x125,channel)||channel>=20)return false;
 x9wr_family v4={},v6={};
 if(!x9wr_get_family(p,n,4,0x103,0x8104,0x10d,0x810e,0x123,type!=2,v4)||
    !x9wr_get_family(p,n,16,0x108,0x8109,0x112,0x8113,0x124,type!=1,v6))return false;
 b.channel=channel;b.md_mtu=v4.mtu;memcpy(b.ip,v4.address,4);
 b.dns_count=v4.dns_count>2?2:v4.dns_count;
 for(unsigned i=0;i<b.dns_count;i++)memcpy(b.dns[i],v4.dns[i],4);
 return true;
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
 // quiet used to hide failed RRC probes. Preserve the send decision, not silence.
 (void)quiet;static uint64_t blocked_count[8],blocked_next[8];
 unsigned key=(sim<2?sim:0)*4+(cnf==0x308?0:cnf==0x1152?1:cnf==0x4c8?2:3);
 uint64_t &blocked=blocked_count[key],&next=blocked_next[key];
 if(!slot||!x9_md_mipc_online()){
  blocked++;if(blocked<=2||x9q_ms()>=next){next=x9q_ms()+3000;printf("X9 CELL SIDE: SIM%u %s blocked slot %u online %u seen %llu\n",sim+1,what,slot!=nullptr,x9_md_mipc_online(),blocked);}return false;}
 uint64_t token=f.submit();if(!token){
  blocked++;if(blocked<=2||x9q_ms()>=next){next=x9q_ms()+3000;printf("X9 CELL SIDE: SIM%u %s enqueue-failed seen %llu\n",sim+1,what,blocked);}return false;}
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
// cell161 diagnostic (Android RIL sends all of these): register the RRC
// CSCON 4409 and physical-channel 4419 indications once per SIM, then while a
// call is owned read NW_GET_CSCON_STATE 1151 and NW_GET_PHYSICAL_CHANNEL_
// CONFIGS 04c7 every 3 s. Read-only; they do not change admission.
static void x9w_up_diag(){
 static unsigned reg;static uint64_t next;static unsigned polls;
 if(!x9w_session_ready||!x9_md_mipc_online())return;
 if(reg<2*x9_md_sim_count()){
  unsigned sim=reg/2,id=reg&1?0x4419:0x4409;uint8_t bytes[2]={(uint8_t)id,(uint8_t)(id>>8)};
  x9w_frame f(0x307,1u<<sim);f.tlv(0x100,bytes,2);
  if(x9w_side_send(f,sim,0x308,reg&1?"REGISTER_IND 4419":"REGISTER_IND 4409",true))reg++;
  return;
 }
 if(polls>=80||x9q_ms()<next)return;
 for(const x9w_bearer &b:x9w_bearers)if(b.used&&b.owned&&!b.abandoned){
  next=x9q_ms()+3000;polls++;
  {x9w_frame f(0x1151,1u<<b.sim_slot);x9w_side_send(f,b.sim_slot,0x1152,"NW_GET_CSCON_STATE",true);}
  {x9w_frame f(0x4c7,1u<<b.sim_slot);x9w_side_send(f,b.sim_slot,0x4c8,"NW_GET_PHYSICAL_CHANNEL_CONFIGS",true);}
  break;
 }
}
static void x9w_side_poll(){
 x9w_up_diag();
 if(x9w_stats_next&&x9q_ms()>=x9w_stats_next)x9w_stats(false);
 static uint8_t response[X9_MIPC_FRAME_MAX];
 for(x9w_side &e:x9w_sides){if(!e.token)continue;
  unsigned n=0;int ready=x9_md_mipc_take(e.token,response,sizeof(response),&n);
  if(ready!=1){
   static uint64_t tokens[8],next[8];unsigned index=(unsigned)(&e-x9w_sides);uint64_t now=x9q_ms();
   if(tokens[index]!=e.token||now>=next[index]){
    tokens[index]=e.token;next[index]=now+3000;
    printf("X9 CELL SIDE: SIM%u %s token %llu expected %04x take %d late %u wait-left-ms %llu\n",
     e.sim+1,e.what?e.what:"EIF/statistics",e.token,e.cnf,ready,!e.deadline||now>=e.deadline,e.deadline>now?e.deadline-now:0);
   }
  }
  if(ready==1&&e.cmd==0xa0){
   unsigned result=~0u;bool good=x9w_wire(response,n)&&x9q_u16(response+10)==e.cnf&&x9w_word(response,n,0,result);
   if(good&&(e.cnf==0x260||e.cnf==0x262)){unsigned app=0;good=x9w_byte(response,n,0x100,app)&&app==3;}
   printf("X9 WDS: SIM%u %s CNF %s result %u (%u B)\n",e.sim+1,e.what,good?"valid":"malformed",result,n);
   if(e.cnf==0x1152||e.cnf==0x4c8){ // cell161 diagnostic: raw RRC-connection / physical-channel state (no identities)
    char h[2*112+1];unsigned o=0,m=n<16+112?n:16+112;h[0]=0;
    for(unsigned j=16;j<m;j++)o+=(unsigned)snprintf(h+o,sizeof(h)-o,"%02x",response[j]);
    printf("X9 WDS UP: +%llu ms SIM%u %s payload %s\n",(unsigned long long)x9q_ms(),e.sim+1,e.what,h);
   }
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
 if(b.abandoned||b.hold||!x9w_refs(b)){x9w_probe(b,X9WP_PUBLISH,"host-gate",0);return;}
 if(b.query_serial!=b.change_serial){x9w_probe(b,X9WP_PUBLISH,"newer-event-wait",0);if(!b.token)x9w_check(b);return;}
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
 if(!in.valid||in.serial<=b.if_floor||in.trans_id!=b.trans_id||in.cmd!=0){x9w_probe(b,X9WP_PUBLISH,"EIF-wait-or-mismatch",0);return;}
 x9wr_identity current={b.generation,b.result_epoch,b.if_floor,b.sim_slot,b.cid,0,3};
 x9wr_settings runtime={};
 bool matched=b.result_epoch==x9a_epoch&&x9wr_match_settings(b.get_result,in.facts,current,runtime);
 if(matched){
  for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(x9w_member(slot,b)&&x9w_clients[slot].lease_pending&&
    !(x9w_families(runtime)&cbp_family_bit(x9w_clients[slot].family)))
   x9w_lease_fail(slot,x9w_clients[slot].family==4?X9W_INTERNAL_IPV4_DISALLOWED:X9W_INTERNAL_IPV6_DISALLOWED);
 }
 if(!matched||!(x9w_families(runtime)&x9w_refs(b))||!x9w_live(b)||
    x9w_next_handle>=0xfffffffeu||x9w_retirements[mux-1].pending||
    !x9d_bind(mux,b.channel,x9w_families(runtime)&x9w_refs(b),runtime.v4.mtu,runtime.v6.mtu,b.route_lifetime)){
  x9w_probe(b,X9WP_PUBLISH,!matched?"GET-EIF-mismatch":!(x9w_families(runtime)&x9w_refs(b))?"family-disallowed":
   x9w_retirements[mux-1].pending?"AP-retirement-pending":"route-bind-or-owner-reject",0,&runtime);
  printf("X9 WDS: publish failed mux %u (match %u v4 %u addr-same %u retire %u)\n",mux,runtime.matched,x9wr_ipv4_facts(runtime),
   !memcmp(in.ip,b.ip,4),x9w_retirements[mux-1].pending);
  b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
 }
 b.mtu=runtime.v4.mtu;b.runtime=runtime;b.refresh_mask=0;b.active=true;b.op=0;b.next=x9q_ms()+10000;b.setup_deadline=0;
 x9w_probe(b,X9WP_PUBLISH,"published",1);
 printf("X9 WDS: cell157 SIM%u physical call %u -> ccmni%u -> mux %u PDP%u verified families %u MTU4/6 %u/%u; independent leases\n",
  b.sim_slot+1,b.cid,b.channel,mux,runtime.modem_pdp,x9w_families(runtime),runtime.v4.mtu,runtime.v6.mtu);
 // DPMAIF route is bound and the host has the address: report the AP
 // interface up, typed like the verified PDN (EIF_ADDRESS_TYPE 1 or 3).
 x9w_ril_post_act(b.sim_slot,b.cid);
 printf("X9 WDS: call %u address %u.%u.%u.%u dns0 %u.%u.%u.%u dns_count %u (UL source must match)\n",b.cid,
  b.ip[0],b.ip[1],b.ip[2],b.ip[3],b.dns[0][0],b.dns[0][1],b.dns[0][2],b.dns[0][3],b.dns_count);
 // Android capture: the RIL/NetAgentService never sends INTERNAL_EIF_REQ
 // (no 0x313 in the MIPC log); it only consumes EIF indications. The
 // ifst/ipupdate/statistics requests stay available but are off by default.
 if(x9w_netagent_req)x9w_ifst(b.sim_slot,b.trans_id,0,runtime.modem_pdp);
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
  // Android RIL (boot_logcat 14:30:08.543) sends the data SIM a second
  // DATA_SET_CONFIG {DEFAULT_DATA_SIM_CARD106=1, IMS_TEST_MODE103=0} next to
  // the MOBILE_DATA one; 215 never sent it. Queued first so it precedes ACT.
  static uint64_t dds_cfg_ms[2];
  if(b.sim_slot<2&&x9q_ms()-dds_cfg_ms[b.sim_slot]>=5000)
  {x9w_frame g(0x211,1u<<b.sim_slot);g.byte(0x106,1);g.byte(0x103,0);dds_cfg_ms[b.sim_slot]=x9q_ms();
   if(x9w_side_send(g,b.sim_slot,0x212,"DATA_SET_CONFIG default-data-SIM"))printf("X9 WDS: SIM%u DATA_SET_CONFIG default-data-SIM queued\n",b.sim_slot+1);}
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
// Settings events never replace a pending GET token. The query serial freezes
// the event horizon; a CNF predating a newer event is drained then re-queried.
static void x9w_dirty(x9w_bearer &b,uint64_t serial,unsigned close_mask=0){
 if(b.abandoned||b.hold||b.result_epoch!=x9a_epoch)return;
 if(serial>b.change_serial)b.change_serial=serial;
 b.refresh_mask|=close_mask;
 if(b.active&&!x9w_sync_route(b,(unsigned)(&b-x9w_bearers)+1)){b.abandoned=true;x9w_disconnect(b,(unsigned)(&b-x9w_bearers)+1);}
 b.next=0;
}
// 236: IFUP publishes fe80::IID; after the RA, EIF5/MOD4203/GET report
// prefix::IID for the same interface (SLAAC). The host forms its global
// address from the RA itself, so this is not an address change. Treating it
// as one closed v6 admission and sent WDS22, and CommCenter stopped IPv6.
static bool x9w_v6_slaac(const x9wr_family &published,const x9wr_family &now){
 const uint8_t *o=published.address,*n=now.address;
 return published.address_present&&now.address_present&&o[0]==0xfe&&(o[1]&0xc0)==0x80&&
  !(n[0]==0xfe&&(n[1]&0xc0)==0x80)&&!memcmp(o+8,n+8,8);
}
static bool x9w_family_equal(const x9wr_family &a,const x9wr_family &b,unsigned bytes,bool ignore_mtu=false){
 if(a.address_present!=b.address_present)return false;
 if(!a.address_present)return true;
 if((!ignore_mtu&&a.mtu!=b.mtu)||a.dns_count!=b.dns_count||memcmp(a.address,b.address,bytes))return false;
 for(unsigned i=0;i<a.dns_count;i++)if(memcmp(a.dns[i],b.dns[i],bytes))return false;
 return true;
}
static void x9w_reconfigure(x9w_bearer &b,unsigned mux,unsigned mask){
 mask&=x9w_refs(b,false);b.reconfigure_mask|=mask;
 // Admission closes BEFORE the host notification. The 20H392 WDS22
 // reconfigurationRequired consumer tears IP down and issues family Stop.
 // WDS8c would only refresh PCSCF/PCO, not these address/DNS/MTU settings.
 if(!x9w_sync_route(b,mux)){b.abandoned=true;x9w_disconnect(b,mux);return;}
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++)if(x9w_member(slot,b)){
  x9w_client &c=x9w_clients[slot];
  if(c.lease_active&&!c.reconfigure&&(mask&cbp_family_bit(c.family))){
   c.reconfigure=true;c.reconfigure_deadline=x9q_ms()+60000;
   x9w_lease_status(slot,true,0,0,true);
   printf("X9 WDS: family%u mux %u configuration changed; admission closed, native IP reconfigure requested\n",c.family,mux);
  }
 }
}
static void x9w_refresh(x9w_bearer &b,unsigned mux){
 if(b.query_serial!=b.change_serial){b.next=0;return;}
 const x9w_interface &in=x9w_interfaces[b.sim_slot][b.channel];x9wr_settings runtime={};
 if(in.valid&&in.cmd==0)x9wr_match_settings(b.get_result,in.facts,b.query,runtime);
 if(runtime.matched||!b.runtime.v4.address_present){ // unmatched: only a v6-only call may keep its old facts
  const x9wr_family &now=runtime.v6.address_present?runtime.v6:b.get_result.v6;
  if(x9w_v6_slaac(b.runtime.v6,now)){
   x9wr_family keep=b.runtime.v6;
   if(now.mtu_present&&now.mtu>=1280&&now.mtu<keep.mtu)keep.mtu=now.mtu;
   // Published address and DNS stay as the host has them (v4 DNS carries resolution).
   if(!runtime.matched)runtime=b.runtime;
   runtime.v6=keep;
   static unsigned slaac_logs;if(slaac_logs<8){slaac_logs++;
    printf("X9 WDS: mux %u v6 fe80::IID -> global prefix (same IID, SLAAC); published v6 kept, no host reconfigure\n",mux);}
  }
 }
 unsigned changed=0,mtu_only=0;
 if(!x9w_family_equal(b.runtime.v4,runtime.v4,4,true))changed|=1;
 else if(!x9w_family_equal(b.runtime.v4,runtime.v4,4))mtu_only|=1;
 if(!x9w_family_equal(b.runtime.v6,runtime.v6,16,true))changed|=2;
 else if(!x9w_family_equal(b.runtime.v6,runtime.v6,16))mtu_only|=2;
 // 235/234: EIF5 (RA MTU, 1500->1450) landing after publish went through
 // WDS22 reconfigurationRequired; CommCenter answered with family6 Stop in
 // ~2 ms and the bearer then carried no traffic. The Android RIL maps EIF5
 // to configureIpv6Mtu (netd interface MTU only, no teardown). An MTU-only
 // difference is applied to the route silently; the RA itself reaches the
 // host ND6 path. Address/DNS changes still close admission + reconfigure.
 if(mtu_only&&!changed)printf("X9 WDS: mux %u MTU-only change v4 %u->%u v6 %u->%u applied to route; no host reconfigure (RIL configureIpv6Mtu)\n",
  mux,b.runtime.v4.mtu,runtime.v4.mtu,b.runtime.v6.mtu,runtime.v6.mtu);
 // Only GET + current scoped IFUP/MOD facts publish new settings. An
 // uncorroborated family is closed, never replaced with guessed addresses.
 b.runtime=runtime;b.mtu=runtime.v4.mtu;b.refresh_mask=0;
 if(!changed){if(!x9w_sync_route(b,mux)){b.abandoned=true;x9w_disconnect(b,mux);return;}}
 else x9w_reconfigure(b,mux,changed);
 b.next=x9q_ms()+10000;
}
static void x9w_result(x9w_bearer &b,unsigned mux,const uint8_t *p,unsigned n){
 x9w_host_expire(b,mux,x9q_ms()); // late ready1 cannot restore host success
 unsigned op=b.op;b.op=0;unsigned result;
 unsigned expected=op==X9W_DISC?0x21a:op==X9W_CONFIG?0x212:(op==X9W_RETRY_RESET||op==X9W_RETRY_QUERY)?0x21c:
  op==X9W_ACT?0x202:op==X9W_CHECK?0x206:0x204;
 if(!x9w_wire(p,n)||x9q_u16(p+10)!=expected||!x9w_word(p,n,0,result)){
  x9w_probe(b,X9WP_CNF,"wire-reject",(int)op);
  b.hold=true;b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_INTERNAL);
  printf("X9 WDS: malformed CNF op %u; quarantined until boot\n",op);return;
 }
 // The actual take consumed this token, but a CNF from the retired card
 // epoch cannot authorize an old ACT continuation, cleanup203 or success.
 if(b.result_epoch!=x9a_epoch){
  x9w_probe(b,X9WP_CNF,"epoch-reject",(int)op);
  b.hold=true;b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);return;
 }
 b.hold=false; // late valid CNF restores cleanup ability, NEVER host success
 printf("X9 WDS: MIPC CNF msg 0x%04x result %u mux %u\n",expected,result,mux);
 x9w_probe(b,X9WP_CNF,"wire-result",(int)result);
 if(op==X9W_ACT&&result==0)x9_md_log_mark("ACT");
 if(op==X9W_ACT||op==X9W_CHECK)x9w_wire_probe(b,p,n,expected);
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
  // partial profile. Qualify each negotiated family; never claim the missing one.
  if(b.gone||b.abandoned||!x9w_settings(b,p,n)||
     !x9w_word(p,n,0x12a,b.trans_id)||b.trans_id%100!=b.channel||!x9d_ready()){
   x9w_probe(b,X9WP_CNF,"ACT-settings-reject",0);
   x9w_act_diag(b,p,n);b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  x9w_probe(b,X9WP_CNF,"ACT-settings-pass",1);
  if(!x9w_check(b)){b.abandoned=true;x9w_error(b,X9Q_BUSY);x9w_deactivate(b,mux);}return;
 }
 if(op==X9W_CHECK){
  if(b.gone){x9w_clear(b,mux);return;}
  if(b.abandoned){
   // A Stop can arrive while CHECK is in flight. Drain that reply and still
   // honor evidence that the CID was removed/reassigned before sending 203.
   x9wr_get still={};
   if(result||!x9wr_decode_get(p,n,b.query,still)||still.channel!=b.channel||still.trans_id!=b.trans_id)b.md_down=true;
   x9w_deactivate(b,mux);return;
  }
  if(result||!x9w_get_settings(b,p,n)||!x9d_ready()){
   printf("X9 WDS: GET205 result %u rejected mux %u\n",result,mux);
   // Keep 203 only if this reply proves the CID still maps to our route.
   x9wr_get still={};
   if(result||!x9wr_decode_get(p,n,b.query,still)||still.channel!=b.channel||still.trans_id!=b.trans_id)b.md_down=true;
   b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  if(b.query_serial!=b.change_serial){
   b.next=0;
   if(!b.active){b.op=X9W_IFWAIT;b.deadline=x9q_ms()+30000;}
   return;
  }
  if(!b.active){
   b.op=X9W_IFWAIT;b.deadline=x9q_ms()+30000;x9w_publish(b,mux);return;
  }
  x9w_refresh(b,mux);return;
 }
 unsigned cid;
 if(result||!x9w_byte(p,n,0x100,cid)||cid!=b.cid){
  b.hold=true;x9w_error(b,X9Q_NOT_READY);printf("X9 WDS: deactivation unconfirmed, reserved mux %u until boot\n",mux);return;
 }
 if(b.pending&&x9w_reply_live(b)){x9q_packet reply(b.req);reply.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
 printf("X9 WDS: owned call %u deactivation confirmed mux %u\n",b.cid,mux);x9w_clear(b,mux);
}
static void x9w_del(unsigned slot){
 x9a_del(slot);
 for(unsigned i=0;i<CBP_INTERFACES;i++){x9w_bearer &b=x9w_bearers[i];
  if(!b.used)continue;
  bool member=x9w_member(slot,b);
  if(b.pending&&b.slot==slot&&x9w_reply_live(b))x9w_pending_done(b);
  if(member){
   x9w_clients[slot].lease_active=x9w_clients[slot].lease_pending=false;
   if(!x9w_refs(b)){b.abandoned=true;x9w_disconnect(b,i+1);}
   else x9w_sync_route(b,i+1);
  }
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
 // wire5 maps to configureInterfaceChange/configureIpv6Mtu in the RIL,
 // NOT to a full IFUP or a netlink global-address update. Preserve all old
 // address facts; consume only the proved MTU word from the optional v6 item.
 if(cmd==5){
  uint64_t serial=++x9w_if_serial;unsigned count=0;
  if(!x9wr_optional_byte(p,n,0x106,count)||count>1)return false;
  x9q_view item=x9w_tlv(p,n,0x8107);
  if((count&&(!item.p||item.n!=24))||(!count&&item.p))return false;
  x9w_interface &in=x9w_interfaces[sim][trans%100];
  for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
   if(!b.used||!b.owned||b.result_epoch!=x9a_epoch||b.sim_slot!=sim||b.trans_id!=trans)continue;
   if(in.valid&&in.cmd==0&&in.trans_id==trans&&in.facts.epoch==b.result_epoch&&in.serial>b.if_floor&&count){
    in.facts.v6.mtu_present=true;in.facts.v6.mtu=x9q_u32(item.p);
   }
   // A smaller RA MTU narrows the route in place (RIL configureIpv6Mtu).
   // Closing v6 admission here bumped the route epoch and dropped the RA
   // that followed 4 ms later (235); vendor never gates the PDN on wire5.
   unsigned cap=count?x9q_u32(item.p):0;
   if(cap>=1280&&cap<b.runtime.v6.mtu)b.runtime.v6.mtu=cap;
   x9w_dirty(b,serial,cap&&cap<1280?2u:0u);
  }
  printf("X9 WDS: EIF5 SIM%u ccmni%u incremental v6 MTU present %u; coalesced GET refresh\n",sim+1,trans%100,count);
  return true;
 }
 if(cmd!=0&&cmd!=1){
  printf("X9 WDS: MD SIM%u EIF ccmni%u unhandled command %u; no state mutation\n",sim+1,trans%100,cmd);return true;
 }
 x9w_interface &in=x9w_interfaces[sim][trans%100];bzero(&in,sizeof(in));
 in.serial=++x9w_if_serial;in.trans_id=trans;in.cmd=cmd;
 if(cmd==0){
  // Typed EIF facts including the capture card epoch. A dual-stack IFUP
  // carries either/both halves, qualified independently against GET205.
  // No tag103 fallback MTU.
  if(x9wr_decode_ifup(p,n,sim,x9a_epoch,in.serial,in.facts)){
   if(remapped){in.facts.trans_id=trans;in.facts.channel=trans%100;}
   in.mtu=in.facts.v4.mtu;memcpy(in.ip,in.facts.v4.address,4);
   in.valid=true; // MTU may be 0/unspecified; x9wr_match_family picks it
  }
  // Foreign interface events do not grant a host lease.
 }else in.valid=true;
 printf("X9 WDS: MD SIM%u EIF/IFST ccmni%u command %u (payload private)\n",sim+1,trans%100,cmd);
 for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
  if(b.used&&b.owned&&b.result_epoch==x9a_epoch&&b.sim_slot==sim&&b.trans_id==trans&&in.serial>b.if_floor&&
     (cmd!=0||!in.valid||b.active)){
   if(cmd==0&&in.valid){
    unsigned mask=0;
    if(in.facts.v4.address_present!=b.runtime.v4.address_present||memcmp(in.facts.v4.address,b.runtime.v4.address,4)||
       (in.facts.v4.mtu&&in.facts.v4.mtu<b.runtime.v4.mtu))mask|=1;
    if(((in.facts.v6.address_present!=b.runtime.v6.address_present||memcmp(in.facts.v6.address,b.runtime.v6.address,16))&&
        !x9w_v6_slaac(b.runtime.v6,in.facts.v6))||(in.facts.v6.mtu&&in.facts.v6.mtu<1280))mask|=2;
    x9w_dirty(b,in.serial,mask);continue;
   }
   if(cmd!=0){b.md_down=true;if(x9w_netagent_req)x9w_ifst(sim,trans,1,0);} // MD ifdown precedes its logical CID removal
   x9w_host_expire(b,m+1,x9q_ms());
   // 212: expected IFDOWN arrived just before our 204/result0 and incorrectly
   // consumed the pending Stop with error16. Revoke data admission now, but
   // leave the original 203 token and host deadline intact. IFDOWN is NOT a
   // successful DEACT CNF; a timeout/bad CNF must still fail the Stop.
   if(cmd==1&&b.op==X9W_DEACT&&b.token&&!b.hold&&b.pending&&
      x9q_u16(b.req+3)==0x21&&x9w_reply_live(b)){
    x9w_disconnect(b,m+1);
    printf("X9 WDS: expected IFDOWN during Stop mux %u; await original DEACT CNF, deadline unchanged\n",m+1);
    continue;
   }
   b.abandoned=true;x9w_disconnect(b,m+1);x9w_error(b,X9Q_NOT_READY);
  }
 }return true;
}
static bool x9w_modified_event(const uint8_t *p,unsigned n,unsigned sim){
 unsigned cid,channel,trans,type,result;
 if(!x9w_byte(p,n,0x100,cid))return false;
 bool full_result=x9w_word(p,n,0,result)&&result==0;
 for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
  if(!b.used||!b.owned||b.result_epoch!=x9a_epoch||b.sim_slot!=sim||b.cid!=cid||b.abandoned)continue;
  uint64_t serial=++x9w_if_serial;
  // DATA_MOD (Android capture): IF125/u32, TRANS126/u32. Never use
  // GET129 or ACT12a here; CID-only/partial updates merely schedule GET.
  if(!x9w_word(p,n,0x125,channel)||!x9w_word(p,n,0x126,trans)){x9w_dirty(b,serial);continue;}
  if(channel!=b.channel||trans!=b.trans_id)continue;
  x9w_dirty(b,serial);
  x9wr_family v4={},v6={};
  if(!full_result||!x9w_byte(p,n,0x102,type)||type<1||type>3||
     !x9wr_get_family(p,n,4,0x103,0x8104,0x10d,0x810e,0x123,type!=2,v4)||
     !x9wr_get_family(p,n,16,0x108,0x8109,0x112,0x8113,0x124,type!=1,v6))continue;
  x9w_interface &in=x9w_interfaces[sim][channel];
  // Require the original real IFUP for this physical lifetime first.
  if(!in.valid||in.cmd!=0||in.trans_id!=trans||in.facts.epoch!=b.result_epoch||in.serial<=b.if_floor)continue;
  // MOD is not an EIF cap update. Preserve the independently tracked EIF cap; GET supplies the current MD cap.
  if(in.facts.v4.mtu_present){v4.mtu=in.facts.v4.mtu;v4.mtu_present=true;}
  if(in.facts.v6.mtu_present){v6.mtu=in.facts.v6.mtu;v6.mtu_present=true;}
  in.serial=serial;in.facts.serial=serial;in.facts.v4=v4;in.facts.v6=v6;
  unsigned mask=0;
  if(v4.address_present!=b.runtime.v4.address_present||memcmp(v4.address,b.runtime.v4.address,4))mask|=1;
  if((v6.address_present!=b.runtime.v6.address_present||memcmp(v6.address,b.runtime.v6.address,16))&&!x9w_v6_slaac(b.runtime.v6,v6))mask|=2;
  x9w_dirty(b,serial,mask);
  printf("X9 WDS: DATA_MOD4203 SIM%u call %u ccmni%u scoped facts; GET scheduled, not yet published\n",sim+1,cid,channel);
 }
 return true;
}
// ---- Symmetric family leases on a physical PDN -------------------------
static void x9w_selection(const uint8_t *req,unsigned n,uint8_t *apn,uint8_t &apn_n,int &profile){
 x9q_view a=x9q_tlv(req,n,0x14),pr=x9q_tlv(req,n,0x31);
 apn_n=(uint8_t)(a.p&&a.n<=100?a.n:0);if(apn_n)memcpy(apn,a.p,apn_n);
 profile=pr.p&&pr.n==1?pr.p[0]:-1;
}
static x9w_bearer *x9w_lease_bearer(unsigned slot){
 const x9w_client &c=x9w_clients[slot];if(!cbp_mux_ok(c.mux))return nullptr;
 x9w_bearer &b=x9w_bearers[c.mux-1];return x9w_member(slot,b)?&b:nullptr;
}
static bool x9w_lease_valid(unsigned slot){
 const x9w_client &c=x9w_clients[slot];const x9w_bearer *b=x9w_lease_bearer(slot);
 return c.lease_active&&b&&x9w_data_ready(*b)&&!c.reconfigure&&!(b->refresh_mask&cbp_family_bit(c.family))&&
  (x9w_families(b->runtime)&cbp_family_bit(c.family));
}
static void x9w_lease_status(unsigned slot,bool connected,uint16_t type,uint16_t verbose,bool reconfigure){
 uint8_t req[7]={};x9q_packet p(req);uint8_t status[2]={(uint8_t)(connected?2:1),(uint8_t)reconfigure},family=x9w_clients[slot].family;
 p.tlv(1,status,2);
 if(!connected)x9w_end_reason(p,type==X9W_VERBOSE_CM?X9W_END_CLIENT:X9W_END_UNSPECIFIED,type,verbose);
 p.tlv(0x12,&family,1);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1,true,0x22);
}
static void x9w_lease_fail(unsigned slot,uint16_t verbose){
 x9w_client &c=x9w_clients[slot];
 if(c.lease_pending&&c.lease_len&&c.client_generation==x9q_clients[slot].generation){
  x9q_packet p(c.lease_req,14);x9w_end_reason(p,X9W_END_UNSPECIFIED,X9W_VERBOSE_INTERNAL,verbose);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1);
 }
 printf("X9 WDS: family%u Start client %u mux %u failed: internal %u\n",c.family,slot,c.mux,verbose);
 c.starting=c.lease_pending=false;c.lease_len=0;bzero(c.lease_req,sizeof(c.lease_req));
}
static bool x9w_lease_try(unsigned slot){
 x9w_client &c=x9w_clients[slot];x9w_bearer *b=x9w_lease_bearer(slot);
 if(!b||!x9w_data_ready(*b))return false;
 unsigned bit=cbp_family_bit(c.family);
 if(b->refresh_mask&bit)return false;
 if(!(x9w_families(b->runtime)&bit)||(b->reconfigure_mask&bit)||x9w_next_handle>=0xfffffffeu){
  x9w_lease_fail(slot,c.family==4?X9W_INTERNAL_IPV4_DISALLOWED:X9W_INTERNAL_IPV6_DISALLOWED);return true;
 }
 c.lease_handle=++x9w_next_handle;c.lease_active=true;c.lease_pending=c.starting=false;c.reconfigure=false;
 b->account_generations[slot]=c.client_generation;b->account_members[slot]=true;
 bzero(&c.baseline,sizeof(c.baseline));bzero(&c.last_call,sizeof(c.last_call));c.last_call_bytes_valid=false;c.report_next=0;c.last_rat=0;
 if(!x9w_sync_route(*b,c.mux)){c.lease_active=false;c.lease_pending=true;x9w_lease_fail(slot,X9W_INTERNAL_ERROR);return true;}
 uint32_t h=c.lease_handle;uint8_t bytes[4]={(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24)};
 x9q_packet p(c.lease_req);p.tlv(1,bytes,4);p.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1);
 c.lease_len=0;bzero(c.lease_req,sizeof(c.lease_req));x9w_lease_status(slot,true,0,0);
 printf("X9 WDS: family%u client %u leased call %u mux %u control generation %llu\n",c.family,slot,b->cid,c.mux,b->generation);
 return true;
}
static void x9w_lease_poll(){
 uint64_t now=x9q_ms();
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){
  x9w_client &c=x9w_clients[slot];x9w_bearer *b=x9w_lease_bearer(slot);
  if(b&&c.lease_active&&c.reconfigure&&now>=c.reconfigure_deadline){
   c.lease_active=false;c.lease_handle=0;c.reconfigure=false;
   x9w_lease_status(slot,false,X9W_VERBOSE_INTERNAL,X9W_INTERNAL_ERROR);x9w_sync_route(*b,c.mux);
  }
  if(!c.lease_pending)continue;
  if(now>=c.lease_deadline||!b||b->abandoned||b->hold)x9w_lease_fail(slot,X9W_INTERNAL_ERROR);
  else x9w_lease_try(slot);
 }
}
// cell160 diagnostic only: every EIF(4302)/DATA_MOD(4203) indication with its
// arrival time and TLV shape. Android shows EIF5 with tag106=1 + 4203 about
// 36 ms after ACT (the MD has the network RA); 214 never showed that form.
// Values stay private except the first 8 bytes of the v6 item (MTU word and
// the top of the address: tells link-local from a global prefix).
static void x9w_diag(const uint8_t *p,unsigned n){
 static unsigned shown;if(shown>=200||n<16)return;shown++;
 unsigned id=x9q_u16(p+10),trans=0,cmd=0xff,c4=0xff,c6=0xff;char tl[200];unsigned o=0;tl[0]=0;
 x9w_word(p,n,id==0x4302?0x100:0x126,trans);
 if(id==0x4302){x9w_byte(p,n,0x101,cmd);x9w_byte(p,n,0x104,c4);x9w_byte(p,n,0x106,c6);}
 else{x9w_byte(p,n,0x100,cmd);x9w_byte(p,n,0x103,c4);x9w_byte(p,n,0x108,c6);}
 for(unsigned i=16;i+4<=n&&o+12<sizeof(tl);){unsigned t=x9q_u16(p+i),l=x9q_u16(p+i+2);
  o+=(unsigned)snprintf(tl+o,sizeof(tl)-o," %x/%u",t,l);i+=4+((l+3)&~3u);}
 x9q_view v6=x9w_tlv(p,n,id==0x4302?0x8107:0x8109);
 printf("X9 WDS DIAG: +%llu ms IND %04x simmask %u bytes %u %s %u trans %u v4count %u v6count %u v6head %02x%02x%02x%02x %02x%02x%02x%02x tlvs%s\n",
  (unsigned long long)x9q_ms(),id,p[8],n,id==0x4302?"cmd":"cid",cmd,trans,c4,c6,
  v6.p&&v6.n>=8?v6.p[0]:0,v6.p&&v6.n>=8?v6.p[1]:0,v6.p&&v6.n>=8?v6.p[2]:0,v6.p&&v6.n>=8?v6.p[3]:0,
  v6.p&&v6.n>=8?v6.p[4]:0,v6.p&&v6.n>=8?v6.p[5]:0,v6.p&&v6.n>=8?v6.p[6]:0,v6.p&&v6.n>=8?v6.p[7]:0,tl);
}
static void x9w_poll(){
 x9w_retire_poll();
 static uint8_t response[X9_MIPC_FRAME_MAX];unsigned n=0;
 x9w_init_poll(response,sizeof(response));
 {static uint64_t next;uint64_t now=x9q_ms();if(now>=next){next=now+3000;
  printf("X9 CELL CONTROL: online %u stage %u token %llu hold %u control-ready %u session-ready %u dpmaif-ready %u init-wait-left-ms %llu; MD-internal unobserved\n",
   x9_md_mipc_online(),x9w_init_stage,x9w_init_token,x9w_init_hold,x9w_control_ready,x9w_session_ready,x9d_ready(),
   x9w_init_deadline>now?x9w_init_deadline-now:0);
  x9_md_log_poll();
 }}
 x9a_poll(response,sizeof(response));
 for(unsigned i=0;i<8;i++){
  int event=x9_md_mipc_event(response,sizeof(response),&n);if(event<=0)break;unsigned cid;
  if(!x9w_wire(response,n)){x9w_bad_event();}
  else if(x9w_diag(response,n),false){}
  else if(x9q_u16(response+10)==0x4302){
   for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(response[8]&(1u<<sim))
    if(!x9w_interface_event(response,n,sim))x9w_bad_event();
  }
  else if(x9q_u16(response+10)==0x4203){
   for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(response[8]&(1u<<sim))
    if(!x9w_modified_event(response,n,sim))x9w_bad_event();
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
     if(b.pending&&x9q_u16(b.req+3)==0x21&&x9w_reply_live(b)){x9q_packet p(b.req);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
     else x9w_error(b,X9Q_NOT_READY);
     x9w_pending_done(b);if(!b.token)x9w_clear(b,m+1);
     printf("X9 WDS: MD deactivation indication for owned call %u mux %u\n",cid,m+1);
    }
   }
  }else x9w_bad_event();
  bzero(response,sizeof(response));
 }
 uint64_t now=x9q_ms();bool online=x9_md_mipc_online();
 x9w_lease_poll();
 for(unsigned i=0;i<CBP_INTERFACES;i++){x9w_bearer &b=x9w_bearers[i];if(!b.used)continue;
  x9w_probe(b,X9WP_STATE,"heartbeat",(int)b.op);
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
   x9w_probe(b,X9WP_TAKE,ready==1?"reply-ready":ready==0?"reply-pending":"take-error",ready);
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
   for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){if(!x9w_member(slot,b)||!x9w_lease_valid(slot))continue;
   x9w_client &c=x9w_clients[slot];unsigned rat=x9w_rat(c);
   bool stats_due=c.stats_interval&&c.stats_mask&&now>=c.report_next;
   bool rat_changed=c.bearer_report&&rat&&rat!=c.last_rat;
   if(stats_due||rat_changed){
    uint8_t req[7]={};x9q_packet report(req);x9d_stats stats={};
    if(stats_due&&x9d_account(i+1,b.route_lifetime,stats))x9w_statistics(report,stats,c.baseline,c.stats_mask);
    if(rat_changed){x9w_rat_tlv(report,0x1d,rat);c.last_rat=rat;x9w_log_bearer_report(slot,rat);}
    report.send(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+1,true,1);
    c.report_next=now+uint64_t(c.stats_interval)*1000;
   }
   }
   if(now>=b.next&& !x9w_check(b))b.next=now+10000;
  }
 }
 x9w_lease_poll();
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
 if(!c.family)c.family=4;
 x9w_bearer *own=x9w_lease_bearer(slot);
 x9w_bearer *view=x9w_lease_valid(slot)?own:nullptr;
 unsigned error=0;
 if(id==1){
  x9q_view stats=x9q_tlv(req,n,0x11),bearer=x9q_tlv(req,n,0x15);
  if(stats.p&&(stats.n!=5||(x9q_u32(stats.p+1)&~0x3ffu)))error=X9Q_ARG;
  if(bearer.p&&(bearer.n!=1||bearer.p[0]>1))error=X9Q_ARG;
  x9w_log_event_report(local,req,n);
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
   x9w_log_bearer_query(local,id,c.sim_slot,rat);
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
  else if(c.sim_slot!=value-1&&(c.starting||own||c.lease_pending||c.lease_active||x9a_client_busy(slot)))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;
   printf("X9 DDS: WDS 5569 client 0x%x routes confirmed default SIM%u reason %u\n",local,value,reason.p[0]);}
 }else if(id==0xaf){
  /* WDS differs from NAS/DMS: u32 1=primary, 2=secondary. DEFAULT/ANY
   * need DDS semantics, so reject them rather than silently selecting SIM1. */
  x9q_view sub=x9q_tlv(req,n,1);unsigned value=sub.p&&sub.n==4?x9q_u32(sub.p):0;
  if(n!=14||!value||value>x9_md_sim_count())error=X9Q_ARG;
  else if(c.sim_slot!=value-1&&(c.starting||own||c.lease_pending||c.lease_active||x9a_client_busy(slot)))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;printf("X9 DUALSIM: WDS client 0x%x bound to SIM%u (not DDS)\n",local,value);}
 }else if(id==0xb0){
  if(n!=7)error=X9Q_ARG;
  else{x9q_packet p(req);uint8_t sub[4]={(uint8_t)(c.sim_slot+1),0,0,0};p.tlv(0x10,sub,4);p.send(local,port);return true;}
 }else if(id==0xa2||id==0x4d){
  if(c.starting||own||c.lease_pending||c.lease_active)error=X9Q_BUSY;
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
  else if(!view)error=15;
  else{
   x9q_packet p(req);uint8_t type=0;p.tlv(0x11,&type,1);p.tlv(0x14,view->apn,(unsigned)strlen(view->apn));
   bool encoded=c.family==6?x9wr_serialize_ipv6(view->runtime,p):x9wr_serialize_ipv4(view->runtime,p);
   if(!encoded)error=X9Q_NOT_READY;else{p.send(local,port);return true;}
  }
 }else if(id==0x21){
  x9q_view h=x9q_tlv(req,n,1);
  if(!h.p||h.n!=4||n!=14)error=X9Q_ARG;
  else if(!own||!c.lease_active||!c.lease_handle||x9q_u32(h.p)!=c.lease_handle)error=9;
  else if(own->pending||own->hold||(own->token&&own->op!=X9W_CHECK))error=X9Q_BUSY;
  else{
   c.lease_active=false;c.lease_handle=0;c.reconfigure=false;
   if(x9w_refs(*own)){
    x9w_sync_route(*own,c.mux);x9q_packet reply(req);reply.send(local,port);
    x9w_lease_status(slot,false,X9W_VERBOSE_CM,X9W_CM_CLIENT_END);
    printf("X9 WDS: family%u Stop mux %u retained physical call %u for sibling lease\n",c.family,c.mux,own->cid);return true;
   }
   own->slot=slot;own->host_generation=x9q_clients[slot].generation;
   own->pending=true;own->reply_deadline=x9q_ms()+60000;own->len=n;memcpy(own->req,req,n);own->abandoned=true;c.starting=true;
   x9w_set_end(*own,X9W_VERBOSE_CM,X9W_CM_CLIENT_END);
   x9w_lease_status(slot,false,X9W_VERBOSE_CM,X9W_CM_CLIENT_END);
   if(own->token){x9w_disconnect(*own,c.mux);return true;}
   if(!x9w_deactivate(*own,c.mux))x9w_error(*own,X9Q_NOT_READY);return true;
  }
 }else{
  x9q_view f=x9q_tlv(req,n,0x19);unsigned family=f.p&&f.n==1?f.p[0]:c.family;
  char selected_apn[101]={};
  x9q_view start_apn=x9q_tlv(req,n,0x14);
  printf("X9 WDS: Start msg 0x%04x SIM%u mux %u family %u APN length %u native-ready %u session-ready %u\n",
   id,c.sim_slot+1,c.mux,family,start_apn.n,x9d_ready(),x9w_session_ready);
  if(!cbp_mux_ok(c.mux)||n>sizeof(c.lease_req))error=X9Q_ARG;
  else if(!x9ds_data_ready(c.sim_slot))error=X9Q_NOT_READY;
  else if(c.starting||own||x9w_retirements[c.mux-1].pending||
          x9w_retiring_client(slot,x9q_clients[slot].generation)||x9a_busy())error=X9Q_BUSY;
  else if((error=x9w_args(req,n,family))!=0){}
  else if((error=x9p_start(req,n,c.sim_slot,family,selected_apn))!=0){}
  else if(!x9d_ready()||!x9_md_mipc_online()||!x9w_session_ready)error=X9Q_NOT_READY;
  else{
   x9w_bearer &b=x9w_bearers[c.mux-1];
   if(!x9q_tlv(req,n,0x31).p&&start_apn.n)memcpy(selected_apn,start_apn.p,start_apn.n);
   uint8_t apn[101]={},apn_n=0;int profile=-1;x9w_selection(req,n,apn,apn_n,profile);
   if(b.used&&(b.sim_slot!=c.sim_slot||b.result_epoch!=x9a_epoch||b.abandoned||b.hold||
      apn_n!=b.start_apn_n||memcmp(apn,b.start_apn,apn_n)||profile!=b.start_profile||strcasecmp(selected_apn,b.apn)))error=X9Q_BUSY;
   else if(!b.used&&x9w_next_generation==~0ULL)error=X9Q_INTERNAL;
   else{
    bool fresh=!b.used;
    if(fresh){
     bzero(&b,sizeof(b));b.used=true;b.slot=CBQ_MAX_CLIENTS;b.sim_slot=c.sim_slot;
     b.generation=++x9w_next_generation;b.result_epoch=x9a_epoch;b.setup_deadline=x9q_ms()+200000;
     memcpy(b.apn,selected_apn,sizeof(b.apn));memcpy(b.start_apn,apn,apn_n);b.start_apn_n=apn_n;b.start_profile=profile;
    }
    c.family=(uint8_t)family;c.lease_pending=c.starting=true;c.lease_len=n;memcpy(c.lease_req,req,n);
    c.client_generation=x9q_clients[slot].generation;c.lease_generation=b.generation;
    c.lease_deadline=b.active?x9q_ms()+30000:b.setup_deadline;
    bzero(selected_apn,sizeof(selected_apn));
    if(!fresh){x9w_lease_try(slot);return true;}
    if(x9w_reset_epoch[b.sim_slot]==b.result_epoch||
       (x9w_retries[b.sim_slot].valid&&x9w_retries[b.sim_slot].epoch==b.result_epoch)){
     const x9w_retry_policy &r=x9w_retries[b.sim_slot];
     b.next=r.valid&&r.epoch==b.result_epoch?(x9w_retry_same(b)?r.after:r.floor):0;b.op=X9W_RETRY_WAIT;
     printf("X9 WDS: SIM%u retry Start retained under original host deadline; no immediate ACT/BUSY loop\n",b.sim_slot+1);
    }else b.op=X9W_DISC;
    x9w_start_next(b,c.mux);return true;
   }
  }
  bzero(selected_apn,sizeof(selected_apn));
 }
 if(error){
  if(id==0x20||id==0x21)printf("X9 WDS: request msg 0x%04x SIM%u mux %u error %u\n",id,c.sim_slot+1,c.mux,error);
  x9bb_reply_error(local,port,req,n,(uint16_t)error);return true;
 }
 x9q_packet reply(req);reply.send(local,port);return true;
}
