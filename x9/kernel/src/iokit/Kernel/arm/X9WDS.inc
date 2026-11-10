/* Native IPv4 WDS dialler, single QMI/DPMAIF owner (NOT the AT job queue).
 * Runtime: libmtkmipc-ril + libmipc, ttyCMIPC1/RX216/TX217.
 * OPEN301/register307 -> GET_PDP_CID219 -> ACT201 (REUSE=2, fallback=0)
 * -> GET_CALL205 + actual EIF4302/IFUP -> bind.
 * Native decodeDataCallProfile reads ID100 separately from IF125;
 * ccci_dpmaif_com writes channel_id=ccmni_idx. Never use CID-1 as a route.
 * Explicit or network-default APN, IPv4, no-auth only; no profiles/IMS/IPv6 SLAAC translation.
 * Old MIPC headers differ from this RIL at tags128..12b: TRANS_ID is NOT a
 * subnet mask. Missing gateway/netmask is omitted, not invented. APN/IP stays
 * in RAM, never diagnostics. Sent activation survives host timeout/DEL_CLIENT
 * until its late CNF and real owned-call cleanup. No cancel/reset/borrowed CID.
 */
struct x9w_client {
 uint8_t mux, family, sim_slot, stats_interval; bool starting,bearer_report;
 uint32_t stats_mask,last_rat;uint64_t report_next;
 x9d_stats baseline,last_call;
};
static x9w_client x9w_clients[CBQ_MAX_CLIENTS];
enum { X9W_DISC=1, X9W_ACT, X9W_CHECK, X9W_DEACT, X9W_IFWAIT };
struct x9w_bearer {
 bool used,owned,active,abandoned,pending,hold,gone;
 unsigned slot,op,min_cid,max_cid,channel,mtu,dns_count;
 uint64_t generation,token,deadline,next,if_floor;
 uint32_t handle,len,trans_id;
 uint8_t cid,sim_slot,req[X9Q_REQ],ip[4],dns[2][4];
 char apn[101];
};
static x9w_bearer x9w_bearers[CBP_INTERFACES];
static uint32_t x9w_next_handle;
// Latest MD interface state, not Android's local netdev state. The floor and
// never-reused DPMAIF channel prevent accepting a pre-ACT/previous bearer event.
struct x9w_interface { uint64_t serial; uint32_t trans_id,mtu; unsigned cmd; bool valid; uint8_t ip[4]; };
static x9w_interface x9w_interfaces[2][20];
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
static void x9w_init_poll(uint8_t *response,unsigned room){
 if(!x9_md_mipc_online()){x9w_control_ready=x9w_session_ready=false;return;}
 if(x9w_init_hold||x9w_session_ready)return;
 if(x9w_init_token){
  unsigned n=0;int ready=x9_md_mipc_take(x9w_init_token,response,room,&n);
  if(ready!=1){
   if(x9q_ms()>=x9w_init_deadline){x9w_init_hold=true;printf("X9 MIPC: handshake stage %u timeout; no retry until boot\n",x9w_init_stage);}
   return;
  }
  x9w_init_token=0;unsigned result=2,version=0,mask=x9w_init_stage==1?0xff:1u<<((x9w_init_stage-2)/2);
  bool good=x9w_wire(response,n,mask)&&x9q_u16(response+10)==(x9w_init_stage==1?0x302:0x308);
  if(x9w_init_stage==1){
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
   printf("X9 MIPC: handshake stage %u reply id 0x%04x len %u tlvs%s\n",x9w_init_stage,n>=12?x9q_u16(response+10):0,n,w?t:" none");
  }
  bzero(response,room);
  if(!good){x9w_init_hold=true;printf("X9 MIPC: handshake stage %u rejected/malformed; no dial\n",x9w_init_stage);return;}
  printf("X9 MIPC: handshake stage %u confirmed\n",x9w_init_stage);
  if(x9w_init_stage==1)x9w_control_ready=true; // AT queries need OPEN, not PDP event registration
  if(x9w_init_stage==1+2*x9_md_sim_count()){
   x9w_session_ready=true;printf("X9 MIPC: v2 OPEN + DEACT/EIF subscriptions confirmed for %u SIM(s), IPv4 dialler ready\n",x9_md_sim_count());return;}
 }
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
static unsigned x9w_args(const uint8_t *p,unsigned n,unsigned family){
 if(family!=4)return X9Q_UNSUPPORTED; // native IPv6 uses netagent/SLAAC, no guessed prefix
 x9q_view apn=x9q_tlv(p,n,0x14);if(!x9w_apn(apn.p,apn.n))return X9Q_ARG;
 for(unsigned at=7;at<n;at+=3+x9q_u16(p+at+1)){
  unsigned k=x9q_u16(p+at+1);const uint8_t *v=p+at+3;
  switch(p[at]){
  case 0x14:break;
  case 0x19:if(k!=1||v[0]!=family)return X9Q_ARG;break;
  case 0x30:if(k!=1||(v[0]!=0&&v[0]!=1))return X9Q_UNSUPPORTED;break; // automatic or 3GPP
  case 0x16:case 0x33:if(k!=1||v[0])return X9Q_UNSUPPORTED;break;
  default:return X9Q_UNSUPPORTED;
  }
 }return 0;
}
static bool x9w_live(const x9w_bearer &b){return b.slot<CBQ_MAX_CLIENTS&&x9q_clients[b.slot].generation==b.generation;}
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
 if(b.pending&&x9w_live(b))x9w_clients[b.slot].starting=false;b.pending=false;b.len=0;bzero(b.req,sizeof(b.req));
}
static void x9w_error(x9w_bearer &b,unsigned error){
 if(b.pending&&x9w_live(b)){
  x9q_packet p(b.req,(uint16_t)error);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);
  printf("X9 WDS: response msg 0x%04x error %u; ownership retained %u op %u\n",x9q_u16(b.req+3),error,b.owned,b.op);
 }x9w_pending_done(b);
}
static void x9w_disconnect(x9w_bearer &b,unsigned mux){
 bool was=b.active;
 if(was&&x9w_live(b))x9d_account(mux,x9w_clients[b.slot].last_call);
 b.active=false;x9d_unbind(mux);
 if(was&&x9w_live(b)){
  uint8_t req[7]={0,0,0,0x22,0,0,0};x9q_packet p(req);uint8_t status[2]={1,0},family=4;
  p.tlv(1,status,2);p.tlv(0x12,&family,1);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,0x22);
 }
}
static void x9w_clear(x9w_bearer &b,unsigned mux){x9w_disconnect(b,mux);x9w_pending_done(b);bzero(&b,sizeof(b));}
static bool x9w_send(x9w_bearer &b,x9w_frame &f,unsigned op,unsigned timeout){
 b.token=f.submit();if(!b.token)return false;b.op=op;b.deadline=x9q_ms()+timeout;return true;
}
static bool x9w_deactivate(x9w_bearer &b,unsigned mux){
 x9w_disconnect(b,mux);
 if(!b.owned||b.token||b.hold||!x9_md_mipc_online())return false;
 x9w_frame f(0x203,1u<<b.sim_slot);f.byte(0x101,b.cid);f.byte(0x102,1); // normal, never FORCE_LOCAL
 if(!x9w_send(b,f,X9W_DEACT,60000))return false;
 printf("X9 WDS: deactivate owned call %u mux %u (await real CNF)\n",b.cid,mux);return true;
}
static bool x9w_check(x9w_bearer &b){x9w_frame f(0x205,1u<<b.sim_slot);f.byte(0x101,b.cid);return x9w_send(b,f,X9W_CHECK,30000);}
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
static void x9w_ip_tlv(x9q_packet &p,unsigned tag,const uint8_t *ip){uint8_t le[4]={ip[3],ip[2],ip[1],ip[0]};p.tlv((uint8_t)tag,le,4);}
static void x9w_handle_reply(x9w_bearer &b){
 uint32_t h=b.handle;uint8_t bytes[4]={(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24)};
 x9q_packet p(b.req);p.tlv(1,bytes,4);p.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);x9w_pending_done(b);
}
static void x9w_publish(x9w_bearer &b,unsigned mux){
 const x9w_interface &in=x9w_interfaces[b.sim_slot][b.channel];
 if(!in.valid||in.serial<=b.if_floor||in.trans_id!=b.trans_id||in.cmd!=0)return;
 if(in.mtu!=b.mtu||memcmp(in.ip,b.ip,4)||!b.pending||!x9w_live(b)||
    x9w_next_handle>=0xfffffffeu||!x9d_bind(mux,b.channel,1,b.mtu)){
  b.abandoned=true;x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
 }
 b.handle=++x9w_next_handle;b.active=true;b.op=0;b.next=x9q_ms()+10000;
 x9w_client &c=x9w_clients[b.slot];bzero(&c.baseline,sizeof(c.baseline));c.report_next=0;c.last_rat=0;
 printf("X9 WDS: SIM%u PDP activated call %u -> ccmni%u -> mux %u IPv4 MTU %u, ACT+GET+EIF/IFUP verified, DPMAIF bound\n",b.sim_slot+1,b.cid,b.channel,mux,b.mtu);
 x9w_handle_reply(b);
 // Connection notification is emitted only AFTER real bind; not CGATT alone.
 uint8_t req[7]={};x9q_packet connected(req);uint8_t status[2]={2,0},family=4;
 connected.tlv(1,status,2);connected.tlv(0x12,&family,1);
 connected.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,0x22);
}
static void x9w_result(x9w_bearer &b,unsigned mux,const uint8_t *p,unsigned n){
 unsigned op=b.op;b.op=0;unsigned result;
 unsigned expected=op==X9W_DISC?0x21a:op==X9W_ACT?0x202:op==X9W_CHECK?0x206:0x204;
 if(!x9w_wire(p,n)||x9q_u16(p+10)!=expected||!x9w_word(p,n,0,result)){
  b.hold=true;b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_INTERNAL);
  printf("X9 WDS: malformed CNF op %u; quarantined until boot\n",op);return;
 }
 b.hold=false; // late valid CNF restores cleanup ability, NEVER host success
 printf("X9 WDS: MIPC CNF msg 0x%04x result %u mux %u\n",expected,result,mux);
 if(op==X9W_DISC){
  if(result||b.abandoned||!x9w_byte(p,n,0x100,b.min_cid)||!x9w_byte(p,n,0x101,b.max_cid)||b.min_cid>b.max_cid||b.max_cid>=255){
   x9w_error(b,result==1?X9Q_BUSY:X9Q_NOT_READY);x9w_clear(b,mux);return;
  }
  if(!x9d_ready()||!x9_md_mipc_online()){x9w_error(b,X9Q_NOT_READY);x9w_clear(b,mux);return;}
  x9w_frame f(0x201,1u<<b.sim_slot);f.tlv(0x101,b.apn,(unsigned)strlen(b.apn)+1);
  f.word(0x102,1);f.byte(0x103,1);f.byte(0x104,1);f.byte(0x105,0);f.byte(0x108,0);f.byte(0x10a,2);
  b.if_floor=x9w_if_serial;
  if(!x9w_send(b,f,X9W_ACT,120000)){x9w_error(b,X9Q_BUSY);x9w_clear(b,mux);}
  else printf("X9 WDS: activating new IPv4 call mux %u, no reuse/fallback\n",mux);return;
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
  if(result||!x9w_settings(b,p,n)||!x9d_ready()){
   b.abandoned=true;x9w_disconnect(b,mux);x9w_error(b,X9Q_NOT_READY);x9w_deactivate(b,mux);return;
  }
  if(!b.active){
   b.op=X9W_IFWAIT;b.deadline=x9q_ms()+30000;x9w_publish(b,mux);return;
  }b.next=x9q_ms()+10000;return;
 }
 unsigned cid;
 if(result||!x9w_byte(p,n,0x100,cid)||cid!=b.cid){
  b.hold=true;x9w_error(b,X9Q_NOT_READY);printf("X9 WDS: deactivation unconfirmed, reserved mux %u until boot\n",mux);return;
 }
 if(b.pending&&x9w_live(b)){x9q_packet reply(b.req);reply.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1);}
 printf("X9 WDS: owned call %u deactivation confirmed mux %u\n",b.cid,mux);x9w_clear(b,mux);
}
static void x9w_del(unsigned slot){
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
  unsigned v4,v6;
  x9q_view a=x9w_tlv(p,n,0x8105);
  // Current MipcEventHandler copies v4_full_addr's +4 IPv4 and +0 MTU.
  // No fabricated mask and no unimplemented multi-address/IPv6 conversion.
  if(x9w_byte(p,n,0x104,v4)&&v4==1&&x9w_byte(p,n,0x106,v6)&&!v6&&a.p&&a.n==12){
   in.mtu=x9q_u32(a.p);if(!in.mtu)x9w_word(p,n,0x103,in.mtu);
   memcpy(in.ip,a.p+4,4);in.valid=in.mtu&&in.ip[0]&&in.ip[0]<224&&in.ip[0]!=127;
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
 static uint8_t response[X9_MIPC_FRAME_MAX];unsigned n=0;
 x9w_init_poll(response,sizeof(response));
 for(unsigned i=0;i<8;i++){
  int event=x9_md_mipc_event(response,sizeof(response),&n);if(event<=0)break;unsigned cid;
  if(!x9w_wire(response,n)){x9w_bad_event();}
  else if(x9q_u16(response+10)==0x4302){
   for(unsigned sim=0;sim<x9_md_sim_count();sim++)if(response[8]&(1u<<sim))
    if(!x9w_interface_event(response,n,sim))x9w_bad_event();
  }
  else if(x9q_u16(response+10)==0x4202&&x9w_byte(response,n,0x100,cid)){
   for(unsigned m=0;m<CBP_INTERFACES;m++){x9w_bearer &b=x9w_bearers[m];
    if(b.used&&b.owned&&b.cid==cid&&(response[8]&(1u<<b.sim_slot))){
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
  if(!x9w_live(b)||!online||!x9d_ready()){b.abandoned=true;x9w_disconnect(b,i+1);x9w_error(b,X9Q_NOT_READY);}
  if(b.token){
   int ready=x9_md_mipc_take(b.token,response,sizeof(response),&n);
   if(ready==1){b.token=0;x9w_result(b,i+1,response,n);bzero(response,sizeof(response));continue;}
   if(!b.hold&&now>=b.deadline){
    b.abandoned=true;x9w_disconnect(b,i+1);x9w_error(b,X9Q_NOT_READY);b.hold=true;
    printf("X9 WDS: op %u timed out mux %u; retain token for late CNF, no retry\n",b.op,i+1);
   }continue;
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
    if(stats_due&&x9d_account(i+1,stats))x9w_statistics(report,stats,c.baseline,c.stats_mask);
    if(rat_changed){x9w_rat_tlv(report,0x1d,rat);c.last_rat=rat;}
    report.send(CBQ_PORT_BASE+b.slot,X9BB_PORT_BASE+1,true,1);
    c.report_next=now+uint64_t(c.stats_interval)*1000;
   }
   if(now>=b.next&& !x9w_check(b))b.next=now+10000;
  }
 }
}
static bool x9w_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned n){
 unsigned slot=local-CBQ_PORT_BASE,id=x9q_u16(req+3);x9w_client &c=x9w_clients[slot];
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
  else if(!own||!own->active||!x9d_ready()||!x9_md_mipc_online()){
   x9q_packet p(req,15); // OUT_OF_CALL: only retained last-call totals are valid.
   x9w_integer(p,0x1b,c.last_call.tx_bytes,8);x9w_integer(p,0x1c,c.last_call.rx_bytes,8);p.send(local,port);return true;
  }else{
   x9d_stats s={};x9d_account(c.mux,s);x9q_packet p(req);
   if(id==0x86)c.baseline=s;else x9w_statistics(p,s,c.baseline,bits);
   p.send(local,port);return true;
  }
 }else if(id==0x44||id==0x37){
  unsigned rat=x9w_rat(c);
  if(n!=7)error=X9Q_ARG;
  else if(!own||!own->active||!x9d_ready()||!x9_md_mipc_online())error=15;
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
  else if(c.sim_slot!=value-1&&(c.starting||own))error=X9Q_BUSY;
  else{c.sim_slot=(uint8_t)(value-1);x9q_clients[slot].sim_slot=c.sim_slot;
   printf("X9 DDS: WDS 5569 client 0x%x routes confirmed default SIM%u reason %u\n",local,value,reason.p[0]);}
 }else if(id==0xaf){
  /* WDS differs from NAS/DMS: u32 1=primary, 2=secondary. DEFAULT/ANY
   * need DDS semantics, so reject them rather than silently selecting SIM1. */
  x9q_view sub=x9q_tlv(req,n,1);unsigned value=sub.p&&sub.n==4?x9q_u32(sub.p):0;
  if(n!=14||!value||value>x9_md_sim_count())error=X9Q_ARG;
  else if(c.sim_slot!=value-1&&(c.starting||own))error=X9Q_BUSY;
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
  else{x9q_packet p(req);uint8_t status=own&&own->active&&x9d_ready()&&x9_md_mipc_online()?2:1;p.tlv(1,&status,1);p.send(local,port);return true;}
 }else if(id==0x2d){
  x9q_view mask=x9q_tlv(req,n,0x10);
  if(!mask.p||mask.n!=4||n!=14)error=X9Q_ARG;
  else if(!own||!own->active||!x9d_ready()||!x9_md_mipc_online())error=15;
  else{
   x9q_packet p(req);uint8_t family=4,type=0;p.tlv(0x11,&type,1);p.tlv(0x14,own->apn,(unsigned)strlen(own->apn));
   x9w_ip_tlv(p,0x1e,own->ip);for(unsigned i=0;i<own->dns_count;i++)x9w_ip_tlv(p,0x15+i,own->dns[i]);
   unsigned mtu=own->mtu;uint8_t le[4]={(uint8_t)mtu,(uint8_t)(mtu>>8),(uint8_t)(mtu>>16),(uint8_t)(mtu>>24)};
   p.tlv(0x29,le,4);p.tlv(0x2b,&family,1);p.send(local,port);return true;
  }
 }else if(id==0x21){
  x9q_view h=x9q_tlv(req,n,1);
  if(!h.p||h.n!=4||n!=14)error=X9Q_ARG;
  else if(!own||!own->handle||x9q_u32(h.p)!=own->handle)error=9;
  else if(own->pending||own->hold||(own->token&&own->op!=X9W_CHECK))error=X9Q_BUSY;
  else{own->pending=true;own->len=n;memcpy(own->req,req,n);own->abandoned=true;c.starting=true;
   if(own->token){x9w_disconnect(*own,c.mux);return true;} // drain periodic GET, then real DEACT
   if(!x9w_deactivate(*own,c.mux))x9w_error(*own,X9Q_NOT_READY);return true;
  }
 }else{
  x9q_view f=x9q_tlv(req,n,0x19);unsigned family=f.p&&f.n==1?f.p[0]:c.family;
  x9q_view start_apn=x9q_tlv(req,n,0x14);
  printf("X9 WDS: Start SIM%u mux %u family %u APN length %u native-ready %u session-ready %u\n",
   c.sim_slot+1,c.mux,family,start_apn.n,x9d_ready(),x9w_session_ready);
  if(!cbp_mux_ok(c.mux))error=X9Q_ARG;
  else if(!x9ds_data_ready(c.sim_slot))error=X9Q_NOT_READY;
  else if(c.starting||own||x9w_bearers[c.mux-1].used)error=X9Q_BUSY;
  else if((error=x9w_args(req,n,family))!=0){}
  else if(!x9d_ready()||!x9_md_mipc_online()||!x9w_session_ready)error=X9Q_NOT_READY;
  else{
   x9w_bearer &b=x9w_bearers[c.mux-1];bzero(&b,sizeof(b));b.used=b.pending=true;
   b.slot=slot;b.sim_slot=c.sim_slot;b.generation=x9q_clients[slot].generation;b.len=n;memcpy(b.req,req,n);
   x9q_view apn=x9q_tlv(req,n,0x14);if(apn.n)memcpy(b.apn,apn.p,apn.n);c.starting=true;
   x9w_frame query(0x219,1u<<b.sim_slot);
   if(!x9w_send(b,query,X9W_DISC,10000)){x9w_error(b,X9Q_BUSY);x9w_clear(b,c.mux);}return true;
  }
 }
 if(error){
  if(id==0x20||id==0x21)printf("X9 WDS: request msg 0x%04x SIM%u mux %u error %u\n",id,c.sim_slot+1,c.mux,error);
  x9bb_reply_error(local,port,req,n,(uint16_t)error);return true;
 }
 x9q_packet reply(req);reply.send(local,port);return true;
}
