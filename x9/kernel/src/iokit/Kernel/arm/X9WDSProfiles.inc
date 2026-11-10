/* 20H392 Mav16: 27 creates a NON-PERSISTENT profile, c7 replaces its
 * settings, 31/32 select the resulting profile in Start20/d0. These are
 * adapter-owned QMI configuration objects, NOT MTK CIDs or connected calls.
 * Storage success means only RAM was changed. Only the IPv4 dialler can
 * create a modem call, and only matched ACT/GET/EIF can publish success.
 * The namespace is shared by WDS clients on one SIM (as on QMI), but never
 * across SIM/card epoch. IDs are not reused before reboot; DEL_CLIENT does
 * not erase another client's profile or an already copied bearer config.
 */
struct x9p_profile {
 bool used,configured;uint8_t sim,type,index;uint64_t epoch;
 unsigned len;uint8_t settings[X9Q_REQ];
};
static x9p_profile x9p_profiles[64];
static unsigned x9p_next[2][2];
static void x9p_expire(){
 for(x9p_profile &p:x9p_profiles)if(p.used&&p.epoch!=x9a_epoch)bzero(&p,sizeof(p));
}
static x9p_profile *x9p_find(unsigned sim,unsigned type,unsigned index){
 for(x9p_profile &p:x9p_profiles)
  if(p.used&&p.epoch==x9a_epoch&&p.sim==sim&&p.type==type&&p.index==index)return &p;
 return nullptr;
}
static bool x9p_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned n,unsigned sim){
 unsigned id=x9q_u16(req+3);
 if(id!=0x27&&id!=0x29&&id!=0x2b&&id!=0xc7)return false;
 x9p_expire();unsigned error=0;x9q_view key=x9q_tlv(req,n,1);x9p_profile *p=nullptr;
 if(sim>=x9_md_sim_count())error=X9Q_ARG;
 else if(id==0x27){
  // Runtime189: exactly ProfileType01/u8 (0 or 1), Persistence8f/u8=0.
  x9q_view persist=x9q_tlv(req,n,0x8f);
  if(!key.p||key.n!=1||key.p[0]>1||!persist.p||persist.n!=1||n!=15)error=X9Q_ARG;
  else if(persist.p[0])error=X9Q_UNSUPPORTED;
  else if(x9p_next[sim][key.p[0]]>=255)error=X9Q_BUSY;
  else{
   for(x9p_profile &candidate:x9p_profiles)if(!candidate.used){p=&candidate;break;}
   if(!p)error=X9Q_BUSY;
   else{
    bzero(p,sizeof(*p));p->used=true;p->sim=(uint8_t)sim;p->type=key.p[0];
    p->index=(uint8_t)++x9p_next[sim][p->type];p->epoch=x9a_epoch;
    uint8_t identifier[2]={p->type,p->index};x9q_packet reply(req);reply.tlv(1,identifier,2);reply.send(local,port);
    printf("X9 PROFILE: create SIM%u type %u index %u epoch %llu RAM-only; no PDP claim\n",sim+1,p->type,p->index,p->epoch);
    return true;
   }
  }
 }else if(!key.p||key.n!=2||key.p[0]>1||!key.p[1])error=X9Q_ARG;
 else if(!(p=x9p_find(sim,key.p[0],key.p[1])))error=10; // INVALID_PROFILE
 else if(id==0xc7){
  x9q_view persist=x9q_tlv(req,n,0x8f);
  if(persist.p&&(persist.n!=1||persist.p[0]))error=X9Q_UNSUPPORTED;
  else{
   // Atomic replacement, not a modem write. Opaque options are retained,
   // not silently executed; Start validates what this dialler can apply.
   bzero(p->settings,sizeof(p->settings));memcpy(p->settings,req,n);p->len=n;p->configured=true;
   printf("X9 PROFILE: configure SIM%u type %u index %u bytes %u RAM-only; no PDP claim\n",sim+1,p->type,p->index,n);
  }
 }else if(n!=12)error=X9Q_ARG;
 else if(id==0x29)bzero(p,sizeof(*p));
 else{
  x9q_packet reply(req);
  if(p->configured)for(unsigned at=7;at<p->len;at+=3+x9q_u16(p->settings+at+1)){
   unsigned tag=p->settings[at],k=x9q_u16(p->settings+at+1);
   if(tag!=1)reply.tlv((uint8_t)tag,p->settings+at+3,k);
  }
  reply.send(local,port);return true;
 }
 if(error)x9bb_reply_error(local,port,req,n,(uint16_t)error);
 else{x9q_packet reply(req);reply.send(local,port);}
 return true;
}
static unsigned x9p_start(const uint8_t *req,unsigned n,unsigned sim,unsigned family,char *apn){
 x9q_view primary=x9q_tlv(req,n,0x31),secondary=x9q_tlv(req,n,0x32);
 if(!primary.p&&!secondary.p)return 0;
 x9p_expire();
 if((primary.p&&primary.n!=1)||(secondary.p&&secondary.n!=1))return X9Q_ARG;
 x9p_profile *p=primary.p?x9p_find(sim,0,primary.p[0]):nullptr;
 if(primary.p&&(!p||!p->configured))return 10;
 // Native creates both types even on a 3GPP-only SIM. Validate the secondary
 // reference but never choose a CDMA profile as the fallback Internet APN.
 if(secondary.p&&!x9p_find(sim,1,secondary.p[0]))return 10;
 if(!p||family!=4)return X9Q_UNSUPPORTED;
 x9q_view pdp=x9q_tlv(p->settings,p->len,0x11),auth=x9q_tlv(p->settings,p->len,0x1d);
 if(pdp.p&&(pdp.n!=1||(pdp.p[0]!=0&&pdp.p[0]!=3)))return X9Q_UNSUPPORTED; // IPv4 or dual-capable
 if(auth.p&&(auth.n!=1||auth.p[0]))return X9Q_UNSUPPORTED;
 for(unsigned tag=0x1b;tag<=0x1c;tag++){
  x9q_view credentials=x9q_tlv(p->settings,p->len,(uint8_t)tag);
  if(credentials.p&&credentials.n)return X9Q_UNSUPPORTED;
 }
 // Do not silently turn an IMS/emergency/proxy profile into Internet.
 // Native profile tags: proxy1f, IMS QoS/SigInd29, emergency36.
 // 23/24 are UMTS QoS settings, not the IMS/emergency flags.
 const unsigned flags[]={0x1f,0x29,0x36};
 for(unsigned tag:flags){
  x9q_view option=x9q_tlv(p->settings,p->len,(uint8_t)tag);
  if(option.p){for(unsigned i=0;i<option.n;i++)if(option.p[i])return X9Q_UNSUPPORTED;}
 }
 x9q_view mask=x9q_tlv(p->settings,p->len,0xdd);
 if(mask.p&&(mask.n!=8||x9q_u32(mask.p)!=1||x9q_u32(mask.p+4)))return X9Q_UNSUPPORTED;
 for(unsigned at=7;at<p->len;at+=3+x9q_u16(p->settings+at+1)){
  unsigned tag=p->settings[at];
  switch(tag){
   case 1:case 0x10:case 0x11:case 0x14:case 0x1b:case 0x1c:case 0x1d:
   case 0x1f:case 0x29:case 0x36:case 0x8f:case 0xdd:break;
   default:
    printf("X9 PROFILE: Start SIM%u type 0 index %u unsupported configured tag %02x length %u; no dial\n",sim+1,p->index,tag,x9q_u16(p->settings+at+1));
    return X9Q_UNSUPPORTED;
  }
 }
 x9q_view configured=x9q_tlv(p->settings,p->len,0x14),explicit_apn=x9q_tlv(req,n,0x14);
 x9q_view chosen=explicit_apn.p?explicit_apn:configured;
 if(!x9w_apn(chosen.p,chosen.n))return X9Q_ARG;
 if(chosen.n)memcpy(apn,chosen.p,chosen.n);apn[chosen.n]=0;
 return 0;
}
