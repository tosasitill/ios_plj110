#ifndef X9_WDS_RESULT_INC
#define X9_WDS_RESULT_INC
/* Bounded value-only facts. Included after the MIPC wire/TLV helpers.
 * GET identity is frozen by the owner when submitting GET205, and supplied
 * ONLY after consuming that exact token. CNF byte8 is not SIM provenance.
 * epoch is the existing card-pair epoch (x9a_epoch), not a CID ownership proof.
 * family0/effective3 describes a physical dual-capable call, not its
 * initiating QMI client. MD may negotiate PDP1/2/3. Each host family is
 * qualified independently; no IPv4 owner is required for an IPv6 lease.
 * EIF4302 facts carry the dispatch SIM, card epoch and monotonic event serial.
 * No helper binds DPMAIF, changes an ACT lease, sends QMI, or claims host-ready.
 */
struct x9wr_identity {
 uint64_t generation,epoch,if_floor;
 uint8_t sim,cid,family,effective_pdp_type;
};
struct x9wr_family {
 bool address_present,mtu_present;
 uint32_t mtu;
 uint8_t address[16],dns[3][16],dns_count;
};
struct x9wr_get {
 x9wr_identity identity;
 uint32_t channel,trans_id;
 uint8_t modem_pdp;
 x9wr_family v4,v6;
};
struct x9wr_ifup {
 uint64_t epoch,serial;
 uint32_t channel,trans_id;
 uint8_t sim;
 x9wr_family v4,v6;
};
struct x9wr_settings {
 x9wr_identity identity;
 uint64_t if_serial;
 uint32_t channel,trans_id;
 uint8_t modem_pdp;
 bool matched;
 x9wr_family v4,v6;
};
static inline bool x9wr_same_identity(const x9wr_identity &a,const x9wr_identity &b){
 return a.generation==b.generation&&a.epoch==b.epoch&&a.if_floor==b.if_floor&&
  a.sim==b.sim&&a.cid==b.cid&&a.family==b.family&&a.effective_pdp_type==b.effective_pdp_type;
}
static inline bool x9wr_address(const uint8_t *p,unsigned bytes){
 uint8_t any=0;for(unsigned i=0;i<bytes;i++)any|=p[i];
 if(!any)return false;
 return bytes==4?(p[0]!=0&&p[0]!=127&&p[0]<224):p[0]!=0xff;
}
static inline bool x9wr_optional_byte(const uint8_t *p,unsigned n,unsigned tag,unsigned &value){
 x9q_view t=x9w_tlv(p,n,tag);value=0;
 if(!t.p)return true;if(t.n!=1)return false;value=t.p[0];return true;
}
static inline bool x9wr_get_family(const uint8_t *p,unsigned n,unsigned bytes,
 unsigned count_tag,unsigned address_tag,unsigned dns_count_tag,unsigned dns_tag,
 unsigned mtu_tag,bool requested,x9wr_family &out){
 unsigned count=0,dns=0;
 if(!x9wr_optional_byte(p,n,count_tag,count)||count>1||
    !x9wr_optional_byte(p,n,dns_count_tag,dns)||dns>4)return false; // MD caps at 4 (913275ec)
 if(requested&&count!=1)return false;
 if(!requested&&(count||dns))return false;
 x9q_view address=x9w_tlv(p,n,address_tag);
 if(count){
  if(!address.p||address.n!=bytes||!x9wr_address(address.p,bytes))return false;
  out.address_present=true;memcpy(out.address,address.p,bytes);
 }else if(address.p)return false;
 for(unsigned i=0;i<4;i++){
  x9q_view d=x9w_tlv(p,n,dns_tag+i);
  if(i<dns){if(!d.p||d.n!=bytes)return false;if(i<3)memcpy(out.dns[i],d.p,bytes);}
  else if(d.p)return false;
 }
 out.dns_count=(uint8_t)(dns>3?3:dns); // a 4th server is not used
 x9q_view mtu=x9w_tlv(p,n,mtu_tag);
 if(mtu.p){if(mtu.n!=4)return false;out.mtu_present=true;out.mtu=x9q_u32(mtu.p);}
 /* Zero remains unspecified: no invented 1500/1280 or cross-family copy. */
 return true;
}
static inline bool x9wr_decode_get(const uint8_t *p,unsigned n,
 const x9wr_identity &query,x9wr_get &output){
 bzero(&output,sizeof(output));x9wr_get out={};unsigned result,cid,type,channel,trans;
 if(query.sim>=2||(query.family!=0&&query.family!=4&&query.family!=6)||
    (query.effective_pdp_type!=0&&query.effective_pdp_type!=1&&query.effective_pdp_type!=3)||
    !p||!x9w_wire(p,n)||x9q_u16(p+10)!=0x206||
    !x9w_word(p,n,0,result)||result||!x9w_byte(p,n,0x100,cid)||cid!=query.cid||
    !x9w_byte(p,n,0x102,type)||!x9w_word(p,n,0x125,channel)||channel>=20||
    !x9w_word(p,n,0x129,trans)||trans%100!=channel)return false;
 // Physical call always requests PDP3, but each host family has its own lease.
 // Accept the MD's negotiated PDP1/2/3; family qualification happens separately.
 if(type<1||type>3)return false;
 out.identity=query;out.channel=channel;out.trans_id=trans;out.modem_pdp=(uint8_t)type;
 if(!x9wr_get_family(p,n,4,0x103,0x8104,0x10d,0x810e,0x123,type!=2,out.v4)||
    !x9wr_get_family(p,n,16,0x108,0x8109,0x112,0x8113,0x124,type!=1,out.v6))return false;
 output=out;return true;
}
static inline bool x9wr_eif_family(const uint8_t *p,unsigned n,unsigned count_tag,
 unsigned address_tag,unsigned bytes,x9wr_family &out){
 unsigned count=0;if(!x9wr_optional_byte(p,n,count_tag,count)||count>1)return false;
 x9q_view full=x9w_tlv(p,n,address_tag);
 if(!count)return !full.p;
 /* Current RIL copies MTU at +0, address at +4; +bytes+4 is not a proved
  * prefix, gateway or subnet. IFUP103 is NOT a fallback IPv6 MTU. */
 if(!full.p||full.n!=bytes+8||!x9wr_address(full.p+4,bytes))return false;
 out.address_present=out.mtu_present=true;out.mtu=x9q_u32(full.p);
 memcpy(out.address,full.p+4,bytes);return true;
}
static inline bool x9wr_decode_ifup(const uint8_t *p,unsigned n,unsigned sim,
 uint64_t epoch,uint64_t serial,x9wr_ifup &output){
 bzero(&output,sizeof(output));x9wr_ifup out={};unsigned trans,cmd;
 if(sim>=2||!serial||!p||!x9w_wire(p,n)||x9q_u16(p+10)!=0x4302||!(p[8]&(1u<<sim))||
    !x9w_word(p,n,0x100,trans)||trans%100>=20||
    !x9w_byte(p,n,0x101,cmd)||cmd!=0)return false;
 out.epoch=epoch;out.serial=serial;out.sim=(uint8_t)sim;
 out.trans_id=trans;out.channel=trans%100;
 if(!x9wr_eif_family(p,n,0x104,0x8105,4,out.v4)||
    !x9wr_eif_family(p,n,0x106,0x8107,16,out.v6)||
    (!out.v4.address_present&&!out.v6.address_present))return false;
 output=out;return true;
}
static inline bool x9wr_match_family(const x9wr_family &get,const x9wr_family &eif,
 unsigned bytes,x9wr_family &out){
 if(!get.address_present)return !eif.address_present;
 if(!eif.address_present||memcmp(get.address,eif.address,bytes))return false;
 // The network may give no MTU (MD then omits ACT/GET 0x123/0x124 and the
 // EIF MTU word is 0). Use whichever valid value exists (the smaller if both),
 // else 1500, the Android default for a PDN without a network MTU.
 const uint32_t low=bytes==4?68u:1280u;
 // An explicit invalid cap is not an absent cap; never enlarge it to 1500.
 if((eif.mtu_present&&eif.mtu&&(eif.mtu<low||eif.mtu>X9D_TX_SIZE))||
    (get.mtu_present&&get.mtu&&(get.mtu<low||get.mtu>X9D_TX_SIZE)))return false;
 bool e=eif.mtu_present&&eif.mtu>=low&&eif.mtu<=X9D_TX_SIZE,g=get.mtu_present&&get.mtu>=low&&get.mtu<=X9D_TX_SIZE;
 uint32_t mtu=e&&g?(eif.mtu<get.mtu?eif.mtu:get.mtu):e?eif.mtu:g?get.mtu:1500u;
 out=get;out.mtu=mtu;out.mtu_present=true;return true;
}
static inline bool x9wr_match_settings(const x9wr_get &get,const x9wr_ifup &eif,
 const x9wr_identity &current,x9wr_settings &output){
 bzero(&output,sizeof(output));x9wr_settings out={};
 if(!x9wr_same_identity(get.identity,current)||eif.epoch!=current.epoch||
    eif.sim!=current.sim||eif.serial<=current.if_floor||
    eif.trans_id!=get.trans_id||eif.channel!=get.channel)return false;
 out.identity=current;out.if_serial=eif.serial;out.channel=get.channel;
 out.trans_id=get.trans_id;out.modem_pdp=get.modem_pdp;
 if(!x9wr_match_family(get.v4,eif.v4,4,out.v4))bzero(&out.v4,sizeof(out.v4));
 if(!x9wr_match_family(get.v6,eif.v6,16,out.v6))bzero(&out.v6,sizeof(out.v6));
 if(!out.v4.address_present&&!out.v6.address_present)return false;
 out.matched=true;output=out;return true;
}
static inline bool x9wr_ipv4_facts(const x9wr_settings &settings){
 return settings.matched&&(settings.modem_pdp==1||settings.modem_pdp==3)&&
  settings.channel<20&&settings.trans_id%100==settings.channel&&
  settings.if_serial>settings.identity.if_floor&&settings.v4.address_present&&
  x9wr_address(settings.v4.address,4)&&settings.v4.mtu_present&&
  settings.v4.mtu>=68&&settings.v4.mtu<=X9D_TX_SIZE&&settings.v4.dns_count<=3;
}
static inline void x9wr_ipv4_tlv(x9q_packet &p,unsigned tag,const uint8_t *ip){
 uint8_t le[4]={ip[3],ip[2],ip[1],ip[0]};p.tlv((uint8_t)tag,le,4);
}
static inline bool x9wr_serialize_ipv4(const x9wr_settings &settings,x9q_packet &p){
 if(!x9wr_ipv4_facts(settings))return false;
 x9wr_ipv4_tlv(p,0x1e,settings.v4.address);
 for(unsigned i=0;i<settings.v4.dns_count&&i<2;i++)x9wr_ipv4_tlv(p,0x15+i,settings.v4.dns[i]);
 uint32_t mtu=settings.v4.mtu;
 uint8_t le[4]={(uint8_t)mtu,(uint8_t)(mtu>>8),(uint8_t)(mtu>>16),(uint8_t)(mtu>>24)},family=4;
 p.tlv(0x29,le,4);p.tlv(0x2b,&family,1);
 return p.good; // No gateway/netmask or host-ready/route/DNS acceptance claim.
}
/* v6 half of a PDP2/3 call, verified by matched GET205+EIF4302.
 * iOS treats the modem address as the interface identifier and configures the
 * global address itself from the network RA (pdp_ip SLAAC), like a Qualcomm
 * modem's runtime settings. No prefix is invented here. */
static inline bool x9wr_ipv6_attach_ready(const x9wr_settings &settings){
 return settings.matched&&(settings.modem_pdp==2||settings.modem_pdp==3)&&
  settings.channel<20&&settings.trans_id%100==settings.channel&&
  settings.if_serial>settings.identity.if_floor&&settings.v6.address_present&&
  x9wr_address(settings.v6.address,16)&&settings.v6.mtu_present&&
  settings.v6.mtu>=1280&&settings.v6.mtu<=X9D_TX_SIZE&&settings.v6.dns_count<=3;
}
static inline bool x9wr_serialize_ipv6(const x9wr_settings &settings,x9q_packet &p){
 if(!x9wr_ipv6_attach_ready(settings))return false;
 uint8_t address[17];memcpy(address,settings.v6.address,16);address[16]=64; // 3GPP /64 per PDN
 p.tlv(0x25,address,17);
 for(unsigned i=0;i<settings.v6.dns_count&&i<2;i++)p.tlv((uint8_t)(0x27+i),settings.v6.dns[i],16);
 uint32_t mtu=settings.v6.mtu;
 uint8_t le[4]={(uint8_t)mtu,(uint8_t)(mtu>>8),(uint8_t)(mtu>>16),(uint8_t)(mtu>>24)},family=6;
 p.tlv(0x29,le,4);p.tlv(0x2b,&family,1);
 return p.good; // No gateway26: the router comes from the RA.
}
static inline bool x9wr_serialize_ipv6_metadata(const x9wr_settings &settings,x9q_packet &p){
 if(!settings.matched||settings.identity.family!=6||!settings.v6.address_present||!settings.v6.mtu_present||
    settings.v6.mtu<1280||settings.v6.mtu>X9D_TX_SIZE||settings.v6.dns_count>3)return false;
 for(unsigned i=0;i<settings.v6.dns_count&&i<2;i++)p.tlv((uint8_t)(0x27+i),settings.v6.dns[i],16);
 uint32_t mtu=settings.v6.mtu;
 uint8_t le[4]={(uint8_t)mtu,(uint8_t)(mtu>>8),(uint8_t)(mtu>>16),(uint8_t)(mtu>>24)},family=6;
 p.tlv(0x29,le,4);p.tlv(0x2b,&family,1);
 /* NO IPv6/CarrierIPv6 tags25/c0 (both need address16+prefix1), NO gateway26.
  * Caller must never publish connected merely because metadata encoded. */
 return p.good;
}
#endif
