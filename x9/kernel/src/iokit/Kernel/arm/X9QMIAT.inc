/* QMI SDU <-> real CCCI AT. Included inside X9Baseband.cpp's owner namespace.
 * Wire layouts: libqmi UIM/NAS/DMS message dictionaries; no packed host structs.
 * All requests, subscriptions and SIM reads belong to this single QMI owner.
 * No APDU writes, PIN guesses, synthetic AID/IMEI or IPA endpoints.
 * Native WDS uses a persistent MIPC owner for activation/cleanup. */
constexpr unsigned X9Q_JOBS = 32, X9Q_REQ = 1024;
constexpr uint16_t X9Q_BAD = 1, X9Q_INTERNAL = 3, X9Q_BUSY = 2,
                   X9Q_NOT_READY = 16, X9Q_ARG = 48, X9Q_UNSUPPORTED = 94;
static uint16_t x9q_u16(const uint8_t *p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t x9q_u32(const uint8_t *p){return x9q_u16(p)|(uint32_t)x9q_u16(p+2)<<16;}
static uint64_t x9q_ms(){uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
struct x9q_view {const uint8_t *p;unsigned n;};
static bool x9q_valid(const uint8_t *p,unsigned n){
 if(n<7||p[0]!=0||x9q_u16(p+5)!=n-7)return false;
 bool seen[256]={};
 for(unsigned i=7;i<n;){
  if(n-i<3)return false;
  unsigned k=x9q_u16(p+i+1);if(k>n-i-3||seen[p[i]])return false;
  seen[p[i]]=true;i+=3+k;
 }
 return true;
}
static x9q_view x9q_tlv(const uint8_t *p,unsigned n,uint8_t tag){
 for(unsigned i=7;i+3<=n;){unsigned k=x9q_u16(p+i+1);if(k>n-i-3)break;
  if(p[i]==tag)return {p+i+3,k};i+=3+k;}
 return {nullptr,0};
}
struct x9q_packet {
 uint8_t p[1536];unsigned n;bool good;
 x9q_packet(const uint8_t *req,uint16_t error=0):n(7),good(true){
  bzero(p,sizeof(p));p[0]=2;memcpy(p+1,req+1,4);
  uint8_t r[4]={(uint8_t)(error!=0),0,(uint8_t)error,(uint8_t)(error>>8)};tlv(2,r,4);
 }
 void tlv(uint8_t tag,const void *v,unsigned len){
  if(!good||len>sizeof(p)-n-3){good=false;return;}
  p[n]=tag;p[n+1]=(uint8_t)len;p[n+2]=(uint8_t)(len>>8);
  if(len)memcpy(p+n+3,v,len);n+=len+3;
 }
 void send(uint32_t local,uint32_t port,bool indication=false,uint16_t id=0){
  if(!good)return;
  if(indication){ /* Indications contain no operation-result TLV. */
   memmove(p+7,p+14,n-14);n-=7;p[0]=4;p[1]=p[2]=0;p[3]=(uint8_t)id;p[4]=(uint8_t)(id>>8);
  }
  p[5]=(uint8_t)(n-7);p[6]=(uint8_t)((n-7)>>8);
  coachz_baseband_qrtr(local,X9BB_NODE,port,p,n);
 }
};
struct x9q_client {uint64_t generation,next;uint32_t uim_mask;uint8_t sim_slot;bool serving,system,signal,dsd_system;};
static x9q_client x9q_clients[CBQ_MAX_CLIENTS];
static void x9ds_apps(bool ready);
static void x9ds_cards(const uint8_t *first,const uint8_t *second,bool ready);
static bool x9ds_data_ready(unsigned slot);
struct x9q_job {
 bool used,background,probe,mipc,ctx;uint32_t local,port,len;uint64_t generation,order,deadline,token,token_deadline;
 uint8_t req[X9Q_REQ];uint16_t id;unsigned stage;
 char command[X9_AT_COMMAND_MAX];
 int sim; /* 0 unknown, 1 ready, 2 PIN, 3 PUK */
 uint16_t file,offset,amount,record_size,record_count;uint8_t structure;
 char path[33];uint8_t raw[512];unsigned raw_len,record;
 uint8_t aid[4][32],aid_len[4],aid_type[4];unsigned apps;
 uint8_t sim_slot,cards[512],slots[41],iccid[2][10];unsigned card_bytes,slot_bytes;
 bool iccid_valid[2],card_ready[2];
 int reg[3],attached,act,csq;uint16_t mcc,mnc;unsigned mnc_digits;
};
static x9q_job x9q_jobs[X9Q_JOBS];static int x9q_active=-1;static uint64_t x9q_order;
static void x9q_cancel(x9q_job &j){
 if(j.token){if(j.mipc)x9_md_sim_at_cancel(j.token);else x9_md_at_cancel(j.token);j.token=0;}
}
/* Exact complete AT lines only; prefix comparisons never cross into another line. */
static bool x9q_line(const char *s,const char *prefix,char *out,unsigned cap){
 unsigned pl=(unsigned)strlen(prefix);bool found=false;
 while(*s){const char *end=s;while(*end&&*end!='\r'&&*end!='\n')end++;
  unsigned n=(unsigned)(end-s);
  if(n>=pl&&!memcmp(s,prefix,pl)){
   if(found)return false; /* Duplicate tagged query replies are ambiguous. */
   const char *v=s+pl;while(v<end&&*v==' ')v++;
   unsigned k=(unsigned)(end-v);if(k>=cap)return false;
   memcpy(out,v,k);out[k]=0;found=true;
  }
  s=end;while(*s=='\r'||*s=='\n')s++;
 }
 return found;
}
static bool x9q_number(const char *&p,unsigned &n){
 while(*p==' ')p++;if(*p<'0'||*p>'9')return false;n=0;
 do{if(n>65535)return false;n=n*10+(*p++-'0');}while(*p>='0'&&*p<='9');
 while(*p==' ')p++;return true;
}
static int x9q_hex(char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
static bool x9q_crsm(const char *s,uint8_t *out,unsigned cap,unsigned &len,uint8_t &sw1,uint8_t &sw2){
 char line[1200];if(!x9q_line(s,"+CRSM:",line,sizeof(line)))return false;
 const char *p=line;unsigned a,b;
 if(!x9q_number(p,a)||*p++!=','||!x9q_number(p,b)||a>255||b>255)return false;
 sw1=(uint8_t)a;sw2=(uint8_t)b;len=0;
 if(!*p)return true;
 if(*p++!=',')return false;while(*p==' ')p++;if(*p++!='"')return false;
 while(*p&&*p!='"'){
  if(len>=cap||!p[1])return false;int hi=x9q_hex(p[0]),lo=x9q_hex(p[1]);
  if(hi<0||lo<0)return false;out[len++]=(uint8_t)(hi*16+lo);p+=2;
 }
 return *p++=='"'&&!*p;
}
static bool x9q_sw(uint8_t a,uint8_t b){return (a==0x90&&b==0)||a==0x91;}
/* Bounded BER TLV (FCP / EF_DIR), including long-form lengths; reject indefinite. */
static bool x9q_ber(const uint8_t *p,unsigned n,unsigned &pos,uint8_t &tag,x9q_view &v){
 if(pos+2>n)return false;tag=p[pos++];unsigned k=p[pos++];
 if(k&128){unsigned bytes=k&127;if(!bytes||bytes>2||bytes>n-pos)return false;
  k=0;while(bytes--)k=(k<<8)|p[pos++];}
 if(k>n-pos)return false;v={p+pos,k};pos+=k;return true;
}
static bool x9q_fcp(x9q_job &j,const uint8_t *p,unsigned n,uint16_t expected){
 j.record_size=j.record_count=0;j.structure=0;unsigned pos=0;uint8_t tag;x9q_view v;
 if(!x9q_ber(p,n,pos,tag,v)||tag!=0x62||pos!=n)return false;
 bool desc=false,size=false,id=false;unsigned i=0;
 while(i<v.n){x9q_view f;if(!x9q_ber(v.p,v.n,i,tag,f))return false;
  if(tag==0x83){if(f.n!=2||((unsigned)f.p[0]<<8|f.p[1])!=expected)return false;id=true;}
  else if(tag==0x80){if(f.n<1||f.n>2)return false;unsigned bytes=f.p[0];if(f.n==2)bytes=(bytes<<8)|f.p[1];j.raw_len=bytes;size=true;}
  else if(tag==0x82){if(f.n<2)return false;unsigned type=f.p[0]&7;
   if(type==1)j.structure=1;
   else if((type==2||type==6)&&f.n==5){j.structure=(uint8_t)type;j.record_size=(uint16_t)((f.p[2]<<8)|f.p[3]);j.record_count=f.p[4];}
   else return false;desc=true;}
 }
 return id&&size&&desc&&(!j.record_count||j.record_size*j.record_count==j.raw_len);
}
static bool x9q_dir(x9q_job &j,const uint8_t *p,unsigned n){
 unsigned i=0;uint8_t tag;x9q_view v;
 while(i<n&&p[i]!=0xff){
  if(!x9q_ber(p,n,i,tag,v)||tag!=0x61)return false;
  unsigned k=0;while(k<v.n){x9q_view a;if(!x9q_ber(v.p,v.n,k,tag,a))return false;
   if(tag==0x4f){
    if(!a.n||a.n>32||j.apps==4)return false;
    for(unsigned t=0;t<j.apps;t++)if(a.n==j.aid_len[t]&&!memcmp(a.p,j.aid[t],a.n))return false;
    unsigned type=0;const uint8_t rid[]={0xa0,0x00,0x00,0x00,0x87};
    if(a.n>=7&&!memcmp(a.p,rid,5)&&a.p[5]==0x10)type=a.p[6]==2?2:a.p[6]==4?5:0;
    memcpy(j.aid[j.apps],a.p,a.n);j.aid_len[j.apps]=(uint8_t)a.n;j.aid_type[j.apps++]=(uint8_t)type;
   }
  }
 }
 while(i<n)if(p[i++]!=0xff)return false;return true;
}
static void x9q_error(x9q_job &j,uint16_t err){
 if(x9_md_sim_count()==2&&j.port==X9BB_PORT_BASE+0x0b&&(j.id==0x2f||j.id==0x47)){
  /* A failed snapshot is not two physical removals. 182's SIM2 CME14
   * (SIM busy) used to clear both identities. Preserve the last observed
   * cards and DDS identity epoch, but block provisioning/data until a fresh
   * complete CardStatus verifies the applications again. CME10 still goes
   * through x9q_card(false)/x9q_slot(NULL) and publishes real absence. */
  x9ds_apps(false);
 }
 if(!j.background&&!j.probe){x9q_packet p(j.req,err);p.send(j.local,j.port);}
 if(!j.background||j.probe)printf("X9 QMI AT: response service 0x%x msg 0x%04x txn %u error %u SIM%u probe %u stage %u\n",
  j.port-X9BB_PORT_BASE,j.id,x9q_u16(j.req+1),err,j.sim_slot+1,j.probe,j.stage);
 x9q_cancel(j);bzero(&j,sizeof(j));x9q_active=-1;
}
static void x9q_finish(x9q_job &j,x9q_packet &p,uint16_t ind=0){
 if(!j.probe&&(!j.background||!x9q_u16(p.p+10)))p.send(j.local,j.port,j.background,ind);
 if(j.probe)printf("X9 DUALSIM: read-only probe service %u msg 0x%04x completed; SIM%u, no synthetic identities\n",j.port-X9BB_PORT_BASE,j.id,j.sim_slot+1);
 if(!j.background)printf("X9 QMI AT: response service 0x%x msg 0x%04x txn %u result %u error %u (real AT)\n",
   j.port-X9BB_PORT_BASE,j.id,x9q_u16(j.req+1),x9q_u16(p.p+10),x9q_u16(p.p+12));
 bzero(&j,sizeof(j));x9q_active=-1;
}
static void x9q_command(x9q_job &j,const char *s,unsigned stage){
 strlcpy(j.command,s,sizeof(j.command));j.stage=stage;
}
static void x9q_sim_command(x9q_job &j,unsigned op,unsigned file,unsigned p1,unsigned p2,unsigned p3,const char *path,unsigned stage){
 /* path is constructed from validated QMI uint16-LE pairs, never AT text. */
 /* 159.txt: the "" data field got +CME ERROR 0 for every read. MTK's own RIL
  * leaves it empty (AT+ECRLA=...,%d,,"%s" in libmtk-ril), as 27.007 allows. */
 snprintf(j.command,sizeof(j.command),"AT+CRSM=%u,%u,%u,%u,%u,,\"%s\"\r",op,file,p1,p2,p3,path);j.stage=stage;
}
/* 160.txt: every CommCenter ReadTransparent (UIM 0x20) failed here with 48 and
 * no reason. Return the failing check; the caller logs it with the file id. */
static const char *x9q_read_check(x9q_job &j){
 x9q_view session=x9q_tlv(j.req,j.len,1),file=x9q_tlv(j.req,j.len,2),read=x9q_tlv(j.req,j.len,3);
 if(!session.p||session.n!=2)return "session TLV";
 if(session.p[1]!=0)return "non-empty AID";
 if(!file.p||file.n<3||file.n!=3u+file.p[2]||file.p[2]>16||(file.p[2]&1))return "file TLV";
 /* 0x24 GetFileAttributes carries session+file only (162.txt: 01/2 02/7). */
 if(j.id==0x24){if(read.p)return "read TLV on GetFileAttributes";j.file=x9q_u16(file.p);j.offset=j.amount=0;}
 else{if(!read.p||read.n!=4)return "read TLV";
 j.file=x9q_u16(file.p);j.offset=x9q_u16(read.p);j.amount=x9q_u16(read.p+2);}
 /* QMI length 0 = whole file / whole record; resolved from the FCP. */
 if(j.amount>255)return "length over one CRSM read";
 unsigned st=session.p[0];
 /* Fixed primary/secondary GW and card1/card2 routes. No arbitrary AID or
  * logical-channel selection; SIM2 never falls back to SIM1's ttyC0. */
 if(st!=0&&st!=6&&st!=2&&st!=7)return "session type";
 j.sim_slot=(st==2||st==7)?1:0;
 if(j.sim_slot>=x9_md_sim_count())return "second slot disabled";
 const char hex[]="0123456789ABCDEF";
 for(unsigned i=0;i<file.p[2];i+=2){uint16_t f=x9q_u16(file.p+3+i);unsigned o=i*2;
  j.path[o]=hex[f>>12];j.path[o+1]=hex[(f>>8)&15];j.path[o+2]=hex[(f>>4)&15];j.path[o+3]=hex[f&15];}
 j.path[file.p[2]*2]=0;
 bool mf=!strcmp(j.path,"3F00")&&(j.file==0x2fe2||j.file==0x2f00);
 /* 161.txt: CommCenter also reads EFs in USIM sub-DFs (3F007FFF7F665F30/4F34).
  * Read-only CRSM on the routed USIM; no arbitrary-AID selection. */
 bool usim=(st==0||st==2)&&!strncmp(j.path,"3F007FFF",8)&&((j.file>=0x6f00&&j.file<=0x6fff)||(j.file>=0x4f00&&j.file<=0x4fff));
 /* 163.txt: EF 6FE5 under DF_TELECOM (3F007F10) is read every cycle. Read-only. */
 bool telecom=!strcmp(j.path,"3F007F10")&&((j.file>=0x6f00&&j.file<=0x6fff)||(j.file>=0x4f00&&j.file<=0x4fff));
 if(!mf&&!usim&&!telecom)return "path/file outside MF ICCID/DIR, DF_TELECOM and USIM 4Fxx/6Fxx";
 if(j.id==0x21&&(!j.offset||j.offset>255))return "record number";
 /* Reject indication tokens, encryption, multi-record reads and unknown TLVs. */
 for(unsigned i=7;i<j.len;i+=3+x9q_u16(j.req+i+1))if(j.req[i]<1||j.req[i]>3)return "extra TLV";
 return nullptr;
}
static bool x9q_read_args(x9q_job &j){
 const char *why=x9q_read_check(j);if(!why)return true;
 static unsigned logged;
 if(logged<64){logged++;x9q_view se=x9q_tlv(j.req,j.len,1);char t[64];unsigned w=0;t[0]=0;
  for(unsigned i=7;i+3<=j.len&&w+12<sizeof(t);){unsigned k=x9q_u16(j.req+i+1);if(k>j.len-i-3)break;
   w+=(unsigned)snprintf(t+w,sizeof(t)-w," %02x/%u",j.req[i],k);i+=3+k;}
  printf("X9 QMI AT: UIM 0x%04x read rejected (%s): session %d aid %d file 0x%04x path %s offset %u length %u tlvs%s\n",
   j.id,why,se.p&&se.n>=1?se.p[0]:-1,se.p&&se.n>=2?se.p[1]:-1,j.file,j.path[0]?j.path:"-",j.offset,j.amount,t);}
 return false;
}
static void x9q_uim_next(x9q_job &j){
 if(j.sim_slot+1<x9_md_sim_count()){
  j.sim_slot++;j.sim=0;j.apps=0;j.record=0;j.raw_len=0;
  bzero(j.aid,sizeof(j.aid));bzero(j.aid_len,sizeof(j.aid_len));bzero(j.aid_type,sizeof(j.aid_type));
  x9q_command(j,"AT+CPIN?\r",1);return;
 }
 if(x9_md_sim_count()==2&&j.iccid_valid[0]&&j.iccid_valid[1]){
  bool distinct=memcmp(j.iccid[0],j.iccid[1],10)!=0;
  static unsigned logged;if(logged<8){logged++;printf("X9 DUALSIM: SIM1/SIM2 ICCID read via masks 1/2, distinct %u (values private)\n",distinct);}
  if(!distinct){x9q_error(j,X9Q_INTERNAL);return;} // never publish SIM1 twice
 }
 if(x9_md_sim_count()==2)bb_dual_card_status(j.iccid_valid[0]?j.iccid[0]:nullptr,j.iccid_valid[1]?j.iccid[1]:nullptr);
 if(x9_md_sim_count()==2)x9ds_cards(j.iccid_valid[0]?j.iccid[0]:nullptr,j.iccid_valid[1]?j.iccid[1]:nullptr,j.card_ready[0]&&j.card_ready[1]);
 if(x9_md_sim_count()==2&&j.id==0x2f)x9ds_apps(j.card_ready[0]&&j.card_ready[1]&&
  j.cards[0]!=0xff&&j.cards[1]==0&&j.cards[4]!=0xff&&j.cards[5]==1);
 x9q_packet p(j.req);p.tlv(0x10,j.id==0x2f?j.cards:j.slots,j.id==0x2f?j.card_bytes:j.slot_bytes);
 x9q_finish(j,p,j.id==0x2f?0x32:0x48);
}
static void x9q_slot(x9q_job &j,const uint8_t *iccid){
 j.card_ready[j.sim_slot]=iccid&&j.sim==1;
 unsigned n=j.slot_bytes;uint8_t *s=j.slots;
 if(n+20>sizeof(j.slots)){x9q_error(j,X9Q_INTERNAL);return;}
 bzero(s+n,10);s[n]=iccid?2:1;s[n+4]=1;s[n+8]=j.sim_slot+1;s[n+9]=iccid?10:0;n+=10;
 if(iccid){memcpy(s+n,iccid,10);n+=10;memcpy(j.iccid[j.sim_slot],iccid,10);j.iccid_valid[j.sim_slot]=true;}
 j.slot_bytes=n;x9q_uim_next(j);
}
static void x9q_card(x9q_job &j,bool present=true){
 j.card_ready[j.sim_slot]=present&&j.sim==1&&j.apps!=0;
 uint8_t *s=j.cards;unsigned n=j.card_bytes;
 if(n+6+j.apps*46>sizeof(j.cards)){x9q_error(j,X9Q_INTERNAL);return;}
 /* CPIN does not distinguish PIN verified from disabled or expose retries.
  * Unknown PIN state (0) makes its required retry bytes unavailable, not a
  * claimed retry budget. Personalization remains unknown, never invented. */
 s[n++]=present?1:0;s[n++]=0;s[n++]=0;s[n++]=0;s[n++]=0;s[n++]=(uint8_t)j.apps;
 for(unsigned i=0;i<j.apps;i++){
  unsigned gw=j.sim_slot?4:0;
  if(j.aid_type[i]==2&&s[gw]==0xff){s[gw]=(uint8_t)i;s[gw+1]=j.sim_slot;}
  s[n++]=j.aid_type[i];s[n++]=j.aid_type[i]==2&&j.sim==1?7:1;
  s[n++]=0;s[n++]=0xff;s[n++]=0;s[n++]=0;s[n++]=j.aid_len[i];
  memcpy(s+n,j.aid[i],j.aid_len[i]);n+=j.aid_len[i];
  memset(s+n,0,7);n+=7;
 }
 static unsigned logged;if(x9_md_sim_count()==2&&logged<16){logged++;
  printf("X9 DUALSIM: UIM SIM%u present %u CPIN class %d EF_DIR apps %u via MIPC mask %u\n",j.sim_slot+1,present,j.sim,j.apps,1u<<j.sim_slot);}
 j.card_bytes=n;x9q_uim_next(j);
}
static int x9q_registration(const char *s,const char *tag){
 char line[256];if(!x9q_line(s,tag,line,sizeof(line)))return -1;
 const char *p=line;unsigned mode,status;
 if(!x9q_number(p,mode)||*p++!=','||!x9q_number(p,status)||mode>5||status>5||(*p&&*p!=','))return -1;
 return (int)status; /* Only <n>,<stat> query form, not a <stat> URC. */
}
static bool x9q_registered(int r){return r==1||r==5;}
static uint8_t x9q_radio(int act){
 return act==0||act==1||act==3?4:act==2||act==4||act==5||act==6?5:
        act==7||act==9?8:act==11||act==12||act==13?12:0;
}
// WDS may describe RAT only from a fresh, real serving-system snapshot.
// Registration is not an IP route; WDS additionally checks its owned bearer.
struct x9q_data_radio { uint64_t sampled; uint32_t rat; };
static x9q_data_radio x9q_data_radios[2];
static void x9q_nas(x9q_job &j){
 x9q_data_radio &data=x9q_data_radios[j.sim_slot];data.sampled=x9q_ms();data.rat=0;
 if(j.attached==1){
  if(x9q_registered(j.reg[2])&&j.act==11)data.rat=1u<<10; // libqmi 3GPP_5GNR
  else if(x9q_registered(j.reg[1])&&(j.act==7||j.act==9||j.act==13))data.rat=1u<<5; // LTE
 }
 /* 166.txt: SysInfo never carried LTE info; log the real registration inputs
  * whenever they change (stat values / CGATT / COPS AcT only, no identities). */
 {static int last[2][5]={{-9,-9,-9,-9,-9},{-9,-9,-9,-9,-9}};int now[5]={j.reg[0],j.reg[1],j.reg[2],j.attached,j.act};
  if(memcmp(last[j.sim_slot],now,sizeof(now))){memcpy(last[j.sim_slot],now,sizeof(now));
   printf("X9 QMI AT: NAS SIM%u registration CREG %d CEREG %d C5GREG %d CGATT %d COPS-AcT %d\n",j.sim_slot+1,now[0],now[1],now[2],now[3],now[4]);}}
 if(j.reg[0]<0&&j.reg[1]<0&&j.reg[2]<0){x9q_error(j,X9Q_NOT_READY);return;}
 int best=x9q_registered(j.reg[2])?j.reg[2]:x9q_registered(j.reg[1])?j.reg[1]:j.reg[0]>=0?j.reg[0]:j.reg[1];
 uint8_t radio=x9q_radio(j.act);
 if(x9q_registered(j.reg[2])&&j.act==11)radio=12;else if(x9q_registered(j.reg[1]))radio=8;
 x9q_packet p(j.req);
 if(j.port==X9BB_PORT_BASE+0x2a){
  // DSD available systems, not a PDP connection or a UI/SIB2 policy claim.
  // Apple DataSystemStatus: count u8 + {technology u32, RAT u32, SOMask u64}.
  unsigned rat=0;uint64_t so=0;
  if(j.attached==1){
   if(x9q_registered(j.reg[2])&&j.act==11){rat=6;so=1ull<<44;}
   else if(x9q_registered(j.reg[1])&&(j.act==7||j.act==9||j.act==13)){
    rat=3;if(j.act==13)so=1ull<<43; // real E-UTRA-NR dual connectivity, not SA
   }
  }
  uint8_t systems[17]={};systems[0]=(uint8_t)(rat!=0);systems[5]=(uint8_t)rat;
  for(unsigned k=0;k<8;k++)systems[9+k]=(uint8_t)(so>>(8*k));
  p.tlv(0x10,systems,rat?17:1);x9q_finish(j,p,0x24);return;
 }
 if(j.id==0x20){
  if(j.csq<0||j.csq>31||!radio){x9q_error(j,X9Q_NOT_READY);return;}
  uint8_t strength[2]={(uint8_t)(int8_t)(-113+2*j.csq),radio};p.tlv(1,strength,2);
 }else if(j.id==0x24){
  uint8_t s[6]={(uint8_t)(best<0?4:best==5?1:best),
   (uint8_t)(j.reg[0]<0?0:x9q_registered(j.reg[0])?1:2),
   (uint8_t)(j.attached<0?0:j.attached==1?1:2),2,(uint8_t)(radio!=0),radio};
  p.tlv(1,s,radio?6:5);
  if(x9q_registered(best)){uint8_t roam=(uint8_t)(best==5?0:1);p.tlv(0x10,&roam,1);}
  if(j.mnc_digits){uint8_t plmn[5]={(uint8_t)j.mcc,(uint8_t)(j.mcc>>8),(uint8_t)j.mnc,(uint8_t)(j.mnc>>8),0};
   p.tlv(0x12,plmn,sizeof(plmn));plmn[4]=(uint8_t)(j.mnc_digits==3);p.tlv(0x27,plmn,sizeof(plmn));}
 }else{ /* NAS GetSysInfo response / SysInfo indication from real AT state. */
  if(j.reg[1]>=0){uint8_t lte[3]={(uint8_t)(x9q_registered(j.reg[1])?2:0),(uint8_t)(x9q_registered(j.reg[1])?2:0),0};p.tlv(0x14,lte,3);}
  /* 165.txt + 163 logd: CommCenter kept cellularRegistrationStatus=unknown /
   * kNotRegistered with status-only SysInfo, so it never started data. LTE System
   * Info (0x19, libqmi layout, 29 bytes): service domain from real CGATT/CREG,
   * roaming from CEREG stat 5, PLMN from COPS numeric. LAC/cell id/TAC and reject
   * info are marked invalid, not invented. Sent only when CEREG is registered. */
  if(x9q_registered(j.reg[1])){
   uint8_t li[29]={};unsigned w=0;
   uint8_t domain=(uint8_t)(j.attached==1?(x9q_registered(j.reg[0])?3:2):(x9q_registered(j.reg[0])?1:0));
   li[w++]=1;li[w++]=domain;li[w++]=1;li[w++]=domain;          /* domain, capability */
   li[w++]=1;li[w++]=(uint8_t)(j.reg[1]==5);                    /* roaming */
   li[w++]=1;li[w++]=0;                                         /* not forbidden */
   li[w++]=0;w+=2;li[w++]=0;w+=4;                               /* lac, cell id invalid */
   li[w++]=0;w+=2;                                              /* no reject info */
   if(j.mnc_digits){li[w++]=1;
    li[w++]=(uint8_t)('0'+j.mcc/100);li[w++]=(uint8_t)('0'+j.mcc/10%10);li[w++]=(uint8_t)('0'+j.mcc%10);
    if(j.mnc_digits==3){li[w++]=(uint8_t)('0'+j.mnc/100);li[w++]=(uint8_t)('0'+j.mnc/10%10);li[w++]=(uint8_t)('0'+j.mnc%10);}
    else{li[w++]=(uint8_t)('0'+j.mnc/10);li[w++]=(uint8_t)('0'+j.mnc%10);li[w++]=0xff;}
   }else{li[w++]=0;w+=6;}
   li[w++]=0;w+=2;                                              /* tac invalid */
   p.tlv(0x19,li,w);
   static unsigned logged;
   if(logged<4){logged++;printf("X9 QMI AT: NAS SysInfo LTE registered domain %u roam %u plmn %s (lac/cell/tac not reported)\n",
     domain,(unsigned)(j.reg[1]==5),j.mnc_digits?"from COPS":"unknown");}
  }
  /* 20H392 has different NR TLV IDs for response vs indication. Its 0x4e
   * parser expects 0x4c/0x4d, NOT the response's 0x4a/0x4b. LTE IDs are shared.
   * This poll spans multiple AT queries; require COPS AcT=11 (NR connected to
   * 5GC) as well as C5GREG before reporting SA. Do not turn LTE+NR/NSA or a
   * RAT transition into an invented SA serving system. */
  const bool nr_registered=x9q_registered(j.reg[2])&&j.act==11;
  if(j.reg[2]>=0){uint8_t nr[3]={(uint8_t)(nr_registered?2:0),(uint8_t)(nr_registered?2:0),0};
   p.tlv(j.background?0x4c:0x4a,nr,sizeof(nr));}
  if(nr_registered){
   /* NR5GSysInfo's base wire sequence is also 29 bytes (libqmi + Apple
    * tlv::parseV<NR5GSysInfo>). The separate 24-bit NR TAC is not available.
    * NR is PS-only: never copy CREG's CS domain into it. Missing CGATT stays
    * invalid; COPS supplies PLMN, C5GREG supplies registration/roaming. */
   uint8_t ni[29]={};unsigned w=0;
   uint8_t domain=(uint8_t)(j.attached==1?2:0);
   ni[w++]=(uint8_t)(j.attached>=0);ni[w++]=domain;
   ni[w++]=(uint8_t)(j.attached>=0);ni[w++]=domain;
   ni[w++]=1;ni[w++]=(uint8_t)(j.reg[2]==5);                   /* roaming */
   ni[w++]=1;ni[w++]=0;                                      /* registered, not forbidden */
   ni[w++]=0;w+=2;ni[w++]=0;w+=4;                            /* LAC/CID invalid */
   ni[w++]=0;w+=2;                                          /* reject info invalid */
   if(j.mnc_digits){ni[w++]=1;
    ni[w++]=(uint8_t)('0'+j.mcc/100);ni[w++]=(uint8_t)('0'+j.mcc/10%10);ni[w++]=(uint8_t)('0'+j.mcc%10);
    if(j.mnc_digits==3){ni[w++]=(uint8_t)('0'+j.mnc/100);ni[w++]=(uint8_t)('0'+j.mnc/10%10);ni[w++]=(uint8_t)('0'+j.mnc%10);}
    else{ni[w++]=(uint8_t)('0'+j.mnc/10);ni[w++]=(uint8_t)('0'+j.mnc%10);ni[w++]=0xff;}
   }else{ni[w++]=0;w+=6;}
   ni[w++]=0;w+=2;                                          /* base TAC invalid */
   uint8_t tag=j.background?0x4d:0x4b;p.tlv(tag,ni,w);
   static unsigned logged;
   if(logged<8){logged++;printf("X9 QMI AT: NAS SysInfo NR SA %s tlv 0x%02x domain %u valid %u roam %u plmn %s (lac/cell/tac not reported)\n",
    j.background?"indication":"response",tag,domain,(unsigned)(j.attached>=0),(unsigned)(j.reg[2]==5),j.mnc_digits?"from COPS":"unknown");}
  }
 }
 x9q_finish(j,p,j.id==0x4d?0x4e:0x24);
}
static bool x9q_int(const char *&p,int &v){
 while(*p==' ')p++;bool neg=*p=='-';if(neg)p++;unsigned n;
 if(!x9q_number(p,n))return false;v=neg?-(int)n:(int)n;return true;
}
/* NAS 0x4f Get Sig Info / 0x51 indication from MTK AT+ECSQ (169.txt: CommCenter
 * asked 0x4f right after LTE registration; rejected, signal bars stayed empty).
 * Field layout from libmtk-ril RmcNetworkHandler::getMdSignalStrengthByAT
 * (0x39cf90): sig1,sig2,rssi_qdbm,rscp_qdbm,ecn0_qdbm,rsrq_qdbm,rsrp_qdbm,Act,
 * sig3,serv_band[,ss_rsrq,ss_rsrp,ss_sinr when Act 0x8000]. updateSignalStrength
 * (0x39d1a0) LTE path (Act 0x1000/0x2000): valid only when sig1<=34 and sig2<98
 * (CESQ-style indices); rsrp/rsrq/rssi = qdbm/4; rssnr(0.1 dB) = sig3*5/2, sig3
 * 0x7fff = invalid. LTE TLV 0x14 (libqmi): rssi i8 dBm, rsrq i8 dB, rsrp i16
 * dBm, snr i16 0.1 dB. NR (0x17/0x18) below; nothing measured stays NOT_READY. */
static void x9q_siginfo(x9q_job &j,const char *r){
 /* 170.txt: "fields 0" for every reply (AT OK) - no single parseable +ECSQ
  * line. Parse the first 24 numeric fields, ignore anything after them, and
  * log the raw reply (numbers/commas only; no identities in +ECSQ). */
 /* 171.txt raw: the MD answers one +ECSQ line per RAT, e.g.
  * "+ECSQ: 25,52,-325,1,1,-30,-355,4096,55,3" (LTE) and
  * "+ECSQ: 87,126,-325,1,1,0,0,32768,0,0,0,0,0" (NR, unmeasured); x9q_line
  * rejects duplicate tags; parse every line and pick by Act.
  * User 2026-11-10 "接上 5g 的信号显示": also report NR. CommCenter 20H392
  * (libCommCenterMCommandDrivers 0x1f1ae9b40 response / 0x1f1aea150 indication)
  * parses 0x14 LteSigInfo{i8 rssi,i8 rsrq,i16 rsrp,i16 snr*10}, 0x17 NrSigInfo
  * {i16 rsrp,i16 snr*10} (parser 0x1f1952660) and 0x18 NrSigInfoExt{i16 rsrq}
  * (0x1f19526cc). libmtk-ril updateSignalStrength NR path (Act 0x8000): fields
  * 11/10/12 = ss_rsrp/ss_rsrq/ss_sinr and 6/5/8 = rsrp/rsrq/sinr, all quarter dB,
  * 0x7fff invalid; the unmeasured NR line is all zeros (171). SS first. */
 int L[24],N[24];unsigned ln=0,nn=0;
 for(const char *s=r;*s;){const char *e=s;while(*e&&*e!='\r'&&*e!='\n')e++;
  if(e-s>7&&!memcmp(s,"+ECSQ:",6)){char line[256];unsigned k=(unsigned)(e-s-6);
   if(k<sizeof(line)){memcpy(line,s+6,k);line[k]=0;const char *p=line;unsigned m=0;int w[24];
    while(m<24&&x9q_int(p,w[m])){m++;while(*p==' ')p++;if(*p!=',')break;p++;}
    if(m>=10&&(w[7]==0x1000||w[7]==0x2000)&&!ln){memcpy(L,w,sizeof(w));ln=m;}
    else if(m>=10&&w[7]==0x8000&&!nn){memcpy(N,w,sizeof(w));nn=m;}}}
  s=e;while(*s=='\r'||*s=='\n')s++;}
 bool lte_ok=ln>=10&&L[0]>=0&&L[0]<=34&&L[1]>=0&&L[1]<98&&L[6]>=-140*4&&L[6]<=-44*4&&
  L[5]>=-34*4&&L[5]<=3*4&&L[2]>=-128*4&&L[2]<=0&&L[8]!=0x7fff&&L[8]*5/2>=-200&&L[8]*5/2<=300;
 /* NR: one consistent set (SS, else the second set); each value range-checked. */
 int nr_rsrp=0,nr_rsrq=0,nr_sinr=0;bool nr_ok=false,nr_rsrq_ok=false;
 for(int pass=0;pass<2&&!nr_ok&&nn>=10;pass++){
  int ip=pass?6:11,iq=pass?5:10,is=pass?8:12;
  if((unsigned)ip>=nn||(unsigned)is>=nn)continue;
  int pr=N[ip],ps=N[is];
  if(pr==0x7fff||ps==0x7fff||pr<-156*4||pr>-31*4||ps<-23*4||ps>40*4)continue;
  nr_rsrp=pr/4;nr_sinr=ps*5/2;nr_ok=true;
  if((unsigned)iq<nn&&N[iq]!=0x7fff&&N[iq]>=-43*4&&N[iq]<=20*4&&N[iq]!=0){nr_rsrq=N[iq]/4;nr_rsrq_ok=true;}
 }
 static unsigned logged[2];
 if(j.sim_slot<2&&logged[j.sim_slot]<8){logged[j.sim_slot]++;char raw[220];unsigned w=0;
  for(const char *q=r;*q&&w+2<sizeof(raw);q++)raw[w++]=*q=='\r'?'|':*q=='\n'?'/':((unsigned char)*q<32||(unsigned char)*q>126)?'?':*q;
  raw[w]=0;
  printf("X9 QMI AT: ECSQ SIM%u client 0x%x generation %llu background %u lte %d (rsrp_q %d rsrq_q %d sinr_q %d) nr %d (rsrp %d rsrq %d%s snr*10 %d) raw \"%s\"\n",
   j.sim_slot+1,j.local,j.generation,j.background,lte_ok,ln>6?L[6]:0,ln>5?L[5]:0,ln>8?L[8]:0,nr_ok,nr_rsrp,nr_rsrq,nr_rsrq_ok?"":"(n/a)",nr_sinr,raw);}
 if(!lte_ok&&!nr_ok){x9q_error(j,X9Q_NOT_READY);return;}
 x9q_packet p(j.req);
 if(lte_ok){int snr=L[8]*5/2,rsrp=L[6]/4;uint8_t lte[6]={(uint8_t)(int8_t)(L[2]/4),(uint8_t)(int8_t)(L[5]/4),
  (uint8_t)rsrp,(uint8_t)(rsrp>>8),(uint8_t)snr,(uint8_t)(snr>>8)};p.tlv(0x14,lte,sizeof(lte));}
 if(nr_ok){uint8_t nr[4]={(uint8_t)nr_rsrp,(uint8_t)(nr_rsrp>>8),(uint8_t)nr_sinr,(uint8_t)(nr_sinr>>8)};p.tlv(0x17,nr,sizeof(nr));
  if(nr_rsrq_ok){uint8_t q[2]={(uint8_t)nr_rsrq,(uint8_t)(nr_rsrq>>8)};p.tlv(0x18,q,sizeof(q));}}
 // Foreground ECSQ queries can keep the global AT queue busy. Publish that
 // SAME actual sample to live registered consumers on the SAME physical slot;
 // never wait for the entire foreground queue to empty to update their bars.
 if(!j.probe)for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){
  const x9q_client &c=x9q_clients[i];
  if(!c.signal||c.sim_slot!=j.sim_slot||(j.background&&CBQ_PORT_BASE+i==j.local))continue;
  x9q_packet ind=p;ind.send(CBQ_PORT_BASE+i,X9BB_PORT_BASE+3,true,0x51);
 }
 x9q_finish(j,p,0x51);
}
/* CELL225: after ACT the MD has addresses but no user plane in either
 * direction. Read the MD's own PDP/PDU session, QoS flow and TFT state with
 * read-only 27.007 queries so a missing QFI/5QI (no network UP resources) can
 * be told apart from an MD-internal netif<->session routing failure.
 * Raw text is printed in bounded chunks; nothing is parsed or acted on. */
static const char *const x9ctx_cmds[]={"AT+CGACT?\r","AT+CGDCONT?\r","AT+CGPADDR\r","AT+CGCONTRDP\r",
 "AT+C5GQOSRDP\r","AT+CGEQOSRDP\r","AT+CGTFTRDP\r","AT+CSCON?\r"};
#define X9CTX_STAGE 80u
#define X9CTX_COUNT (sizeof(x9ctx_cmds)/sizeof(x9ctx_cmds[0]))
static void x9ctx_result(x9q_job &j,const char *r,int result){
 unsigned k=j.stage-X9CTX_STAGE;
 char cmd[20];strlcpy(cmd,k<X9CTX_COUNT?x9ctx_cmds[k]:"?",sizeof(cmd));
 size_t cl=strlen(cmd);if(cl&&cmd[cl-1]=='\r')cmd[cl-1]=0;
 static char flat[X9_AT_RESPONSE_MAX];unsigned n=0;
 for(const char *p=r?r:"";*p&&n+1<sizeof(flat);p++){
  char c=*p;if(c=='\r')continue;if(c=='\n'){if(n&&flat[n-1]!='|')flat[n++]='|';continue;}
  flat[n++]=(c<32||c>126)?'.':c;
 }
 while(n&&flat[n-1]=='|')n--;flat[n]=0;
 enum{CH=80};unsigned parts=n?(n+CH-1)/CH:1;
 for(unsigned i=0;i<parts&&i<16;i++){
  char chunk[CH+1];unsigned o=i*CH,m=n-o<CH?n-o:CH;if(!n)m=0;
  memcpy(chunk,flat+o,m);chunk[m]=0;
  printf("X9 CELL ATCTX: SIM%u round %u %s result %d part %u/%u \"%s\"\n",
   j.sim_slot+1,j.record,cmd,result,i+1,parts,chunk);
 }
 bzero(flat,sizeof(flat));
 if(k+1<X9CTX_COUNT){x9q_command(j,x9ctx_cmds[k+1],X9CTX_STAGE+k+1);return;}
 printf("X9 CELL ATCTX: SIM%u round %u done (%u read-only queries)\n",j.sim_slot+1,j.record,(unsigned)X9CTX_COUNT);
 bzero(&j,sizeof(j));x9q_active=-1;
}
static void x9q_start(x9q_job &j){
 if(j.ctx){x9q_command(j,x9ctx_cmds[0],X9CTX_STAGE);return;}
 unsigned service=j.port-X9BB_PORT_BASE;
 if(service==1){x9q_error(j,X9Q_UNSUPPORTED);
 }else if(service==0x0b||(service==2&&j.id==0x3c)){
  if(service==0x0b&&(j.id==0x20||j.id==0x21||j.id==0x24)&&!x9q_read_args(j)){x9q_error(j,X9Q_ARG);return;}
  if(service==0x0b&&(j.id==0x2f||j.id==0x47)){
   j.sim_slot=0;j.card_bytes=9;j.slot_bytes=1;memset(j.cards,0xff,8);
   j.cards[8]=j.slots[0]=(uint8_t)x9_md_sim_count();
  }
  x9q_command(j,"AT+CPIN?\r",1);
 }else if(service==3&&j.id==0x4f){
  x9q_command(j,"AT+ECSQ\r",40);
 }else if(service==3||(service==0x2a&&j.id==0x24)){
  for(int &r:j.reg)r=-1;j.attached=j.act=j.csq=-1;x9q_command(j,"AT+CREG?\r",30);
 }else if(service==2&&j.id==0x2e){
  /* DMS Set Operating Mode. 164.txt: CommCenter sends ONLINE (0) right before the
   * attach-APN push; it was rejected, then it asked RESET (4) and power-cycled
   * the board, in every boot since 161. ONLINE / LOW_POWER / PERSISTENT_LOW_POWER
   * map to real CFUN=1 / CFUN=4. RESET, OFFLINE, SHUTDOWN, FTM stay unsupported. */
  x9q_view m=x9q_tlv(j.req,j.len,1);unsigned mode=m.p&&m.n==1?m.p[0]:255;
  printf("X9 QMI AT: DMS 0x002e set operating mode %u\n",mode);
  /* Do not undo the original RIL's two-slot EFUN enable when CommCenter
   * subsequently requests global ONLINE. LOW_POWER remains global flight. */
  if(mode==0)x9q_command(j,x9_md_sim_count()==2?"AT+EFUN=3\r":"AT+CFUN=1\r",21);
  else if(mode==1||mode==6)x9q_command(j,"AT+CFUN=4\r",21);
  else x9q_error(j,X9Q_UNSUPPORTED);
 }else if(service==2){
  const char *cmd=j.id==0x21?"AT+CGMI\r":j.id==0x22?"AT+CGMM\r":j.id==0x23?"AT+CGMR\r":
   j.id==0x25?"AT+CGSN\r":j.id==0x2d?"AT+CFUN?\r":"AT+CIMI\r";
  x9q_command(j,cmd,20);
 }else x9q_error(j,X9Q_UNSUPPORTED);
}
static bool x9q_plain(const char *s,char *out,unsigned cap,bool digits){
 bool found=false;while(*s){const char *e=s;while(*e&&*e!='\r'&&*e!='\n')e++;
  unsigned n=(unsigned)(e-s);if(n&&!(n==2&&!memcmp(s,"OK",2))&&s[0]!='+'&&!(n>=2&&s[0]=='A'&&s[1]=='T')){
   if(found||n>=cap)return false;
   for(unsigned i=0;i<n;i++)if((unsigned char)s[i]<32||(unsigned char)s[i]>126||(digits&&(s[i]<'0'||s[i]>'9')))return false;
   memcpy(out,s,n);out[n]=0;found=true;
  }s=e;while(*s=='\r'||*s=='\n')s++;
 }return found;
}
static void x9q_result(x9q_job &j,const char *r,int result){
 if(j.ctx){x9ctx_result(j,r,result);return;}
 unsigned service=j.port-X9BB_PORT_BASE;bool ok=result==X9_AT_OK;
 if(result==X9_AT_TIMEOUT||result==X9_AT_OFFLINE){x9q_error(j,X9Q_NOT_READY);return;}
 if(result==X9_AT_OVERFLOW){x9q_error(j,X9Q_INTERNAL);return;}
 if(service==1){x9q_error(j,X9Q_UNSUPPORTED);return;}
 if(j.stage==40&&ok){x9q_siginfo(j,r);return;}
 if(j.stage>=30&&j.stage<=35){
  if(j.stage<33){const char *tags[]={"+CREG:","+CEREG:","+C5GREG:"};if(ok)j.reg[j.stage-30]=x9q_registration(r,tags[j.stage-30]);}
  else if(j.stage==33&&ok){char line[80];const char *p=line;unsigned n;
   if(x9q_line(r,"+CGATT:",line,sizeof(line))&&x9q_number(p,n)&&!*p&&n<=1)j.attached=(int)n;
  }else if(j.stage==34&&ok){char line[256];
   if(x9q_line(r,"+COPS:",line,sizeof(line))){const char *p=line;unsigned mode,fmt,act;
    if(x9q_number(p,mode)&&*p++==','&&x9q_number(p,fmt)&&*p++==','){
     while(*p==' ')p++;if(*p=='"'){p++;const char *e=p;while(*e&&*e!='"')e++;
      unsigned n=(unsigned)(e-p);bool numeric=fmt==2&&(n==5||n==6);
      for(unsigned i=0;i<n&&numeric;i++)if(p[i]<'0'||p[i]>'9')numeric=false;
      if(numeric){j.mcc=(uint16_t)((p[0]-'0')*100+(p[1]-'0')*10+p[2]-'0');j.mnc=0;
       for(unsigned i=3;i<n;i++)j.mnc=j.mnc*10+p[i]-'0';j.mnc_digits=n-3;}
      if(*e=='"'&&e[1]==','){p=e+2;if(x9q_number(p,act)&&!*p)j.act=(int)act;}
     }
    }
   }
  }else if(j.stage==35&&ok){char line[80];const char *p=line;unsigned n,ber;
   if(x9q_line(r,"+CSQ:",line,sizeof(line))&&x9q_number(p,n)&&*p++==','&&x9q_number(p,ber)&&!*p&&n<=31)j.csq=(int)n;
  }
  if(j.stage==35||(j.stage==34&&j.id!=0x20)){x9q_nas(j);return;}
  const char *cmds[]={"AT+CEREG?\r","AT+C5GREG?\r","AT+CGATT?\r","AT+COPS?\r","AT+CSQ\r"};
  x9q_command(j,cmds[j.stage-30],j.stage+1);return;
 }
 if(!ok){
  char line[80];const char *v=line;unsigned cme;
  if(j.stage==1&&service==0x0b&&(j.id==0x2f||j.id==0x47)&&
     x9q_line(r,"+CME ERROR:",line,sizeof(line))&&x9q_number(v,cme)&&!*v&&cme==10){
   if(j.id==0x2f)x9q_card(j,false);else x9q_slot(j,nullptr);return;
  }
  {char line2[80];const char *v2=line2;unsigned cme2=0;
   bool has=x9q_line(r,"+CME ERROR:",line2,sizeof(line2))&&x9q_number(v2,cme2)&&!*v2;
   printf("X9 QMI AT: service 0x%x msg 0x%04x stage %u AT result %d cme %d\n",service,j.id,j.stage,result,has?(int)cme2:-1);}
  x9q_error(j,X9Q_NOT_READY);return;
 }
 if(j.stage==1){char state[80];if(!x9q_line(r,"+CPIN:",state,sizeof(state))){
   printf("X9 QMI AT: service 0x%x msg 0x%04x CPIN reply without +CPIN line\n",service,j.id);x9q_error(j,X9Q_INTERNAL);return;}
  j.sim=!strcmp(state,"READY")?1:!strcmp(state,"SIM PIN")?2:!strcmp(state,"SIM PUK")?3:0;
  if(j.sim!=1){printf("X9 QMI AT: service 0x%x msg 0x%04x CPIN class %d (1 READY 2 PIN 3 PUK 0 other)\n",service,j.id,j.sim);
   x9q_error(j,X9Q_NOT_READY);return;}
  if(service==2||j.id==0x47||(j.id==0x2f&&x9_md_sim_count()==2))x9q_sim_command(j,176,0x2fe2,0,0,10,"3F00",2);
  else if(j.id==0x2f)x9q_sim_command(j,192,0x2f00,0,0,0,"3F00",3);
  else x9q_sim_command(j,192,j.file,0,0,0,j.path,5);
  return;
 }
 if(j.stage>=2&&j.stage<=6){uint8_t data[512],a,b;unsigned n=0;
  if(!x9q_crsm(r,data,sizeof(data),n,a,b)){x9q_error(j,X9Q_INTERNAL);return;}
  if(!x9q_sw(a,b)){ /* Report actual SW1/SW2, no manufactured file bytes. */
   x9q_packet p(j.req,X9Q_NOT_READY);uint8_t sw[]={a,b};p.tlv(0x10,sw,2);x9q_finish(j,p);return;
  }
  if(j.stage==2){
   if(n!=10){x9q_error(j,X9Q_INTERNAL);return;}x9q_packet p(j.req);
   char digits[21];unsigned k=0;
   for(unsigned i=0;i<n;i++)for(unsigned half=0;half<2;half++){
    unsigned d=(data[i]>>(half*4))&15;if(d==15&&i==9&&half==1)continue;
    if(d>9){x9q_error(j,X9Q_INTERNAL);return;}digits[k++]=(char)('0'+d);}
   if(service==2){p.tlv(1,digits,k);
   }else if(j.id==0x2f){
    /* Verify two distinct physical ICCIDs before publishing CardStatus too,
     * not only if the client happens to ask for GetSlotStatus afterwards. */
    memcpy(j.iccid[j.sim_slot],data,10);j.iccid_valid[j.sim_slot]=true;
    x9q_sim_command(j,192,0x2f00,0,0,0,"3F00",3);return;
   }else{x9q_slot(j,data);return;}
   x9q_finish(j,p,0x48);return;
  }
  if(j.stage==3){
   if(!x9q_fcp(j,data,n,0x2f00)||j.structure!=2||!j.record_count||j.record_count>16||!j.record_size||j.record_size>255){x9q_error(j,X9Q_UNSUPPORTED);return;}
   j.record=1;x9q_sim_command(j,178,0x2f00,j.record,4,j.record_size,"3F00",4);return;
  }
  if(j.stage==4){
   if(n!=j.record_size||!x9q_dir(j,data,n)){x9q_error(j,X9Q_INTERNAL);return;}
   if(++j.record<=j.record_count)x9q_sim_command(j,178,0x2f00,j.record,4,j.record_size,"3F00",4);
   else x9q_card(j);return;
  }
  if(j.stage==5){
   if(!x9q_fcp(j,data,n,j.file)){x9q_error(j,X9Q_UNSUPPORTED);return;}
   if(j.id==0x24){
    /* File Attributes (libqmi order): size, id, type (0 transparent, 1 cyclic,
     * 2 linear fixed), record size, record count, then read/write/increase/
     * deactivate/activate as {logic u8, mask u16}, then raw FCP. Security is what
     * this bridge permits, not the card's ACL: read ALWAYS(0), others NEVER(1). */
    uint8_t at[16+512];unsigned w=0;
    auto u16=[&](unsigned v){at[w++]=(uint8_t)v;at[w++]=(uint8_t)(v>>8);};
    u16(j.raw_len);u16(j.file);at[w++]=(uint8_t)(j.structure==1?0:j.structure==6?1:2);
    u16(j.record_size);u16(j.record_count);
    for(unsigned k=0;k<5;k++){at[w++]=(uint8_t)(k?1:0);u16(0);}
    if(n>sizeof(at)-w-2){x9q_error(j,X9Q_INTERNAL);return;}
    u16(n);memcpy(at+w,data,n);w+=n;
    x9q_packet p(j.req);uint8_t sw[]={a,b};p.tlv(0x10,sw,2);p.tlv(0x11,at,w);x9q_finish(j,p);return;
   }
   if(j.id==0x20){
    if(j.structure==1&&!j.amount&&j.offset<j.raw_len)j.amount=(uint16_t)(j.raw_len-j.offset); /* FCP size is 2 bytes */
    if(j.structure!=1||!j.amount||j.amount>255||j.offset>j.raw_len||j.amount>j.raw_len-j.offset){
     printf("X9 QMI AT: UIM read 0x%04x FCP mismatch: structure %u size %u offset %u length %u\n",j.file,j.structure,j.raw_len,j.offset,j.amount);
     x9q_error(j,X9Q_ARG);return;}
    x9q_sim_command(j,176,j.file,j.offset>>8,j.offset&255,j.amount,j.path,6);
   }else{if(!j.amount)j.amount=j.record_size;
    if((j.structure!=2&&j.structure!=6)||j.offset>j.record_count||j.amount!=j.record_size){
     printf("X9 QMI AT: UIM record 0x%04x FCP mismatch: structure %u records %u size %u record %u length %u\n",j.file,j.structure,j.record_count,j.record_size,j.offset,j.amount);
     x9q_error(j,X9Q_ARG);return;}
    x9q_sim_command(j,178,j.file,j.offset,4,j.amount,j.path,6);}
   return;
  }
  if(n!=j.amount){x9q_error(j,X9Q_INTERNAL);return;}
  x9q_packet p(j.req);uint8_t sw[]={a,b},bytes[514];bytes[0]=(uint8_t)n;bytes[1]=(uint8_t)(n>>8);memcpy(bytes+2,data,n);
  p.tlv(0x10,sw,2);p.tlv(0x11,bytes,n+2);x9q_finish(j,p);return;
 }
 if(j.stage==21){x9q_packet p(j.req);x9q_finish(j,p);return;} /* CFUN final OK (errors returned above) */
 if(j.stage==20){x9q_packet p(j.req);char text[256];
  if(j.id==0x2d){const char *v=text;unsigned mode;
   if(!x9q_line(r,"+CFUN:",text,sizeof(text))||!x9q_number(v,mode)||*v||(mode!=0&&mode!=1&&mode!=4)){x9q_error(j,X9Q_NOT_READY);return;}
   /* CFUN 0/4 is minimum/RF-disabled, not QMI RESET or persistent NV. */
   uint8_t qmi=(uint8_t)(mode==1?0:1);p.tlv(1,&qmi,1);
  }else{
   const char *tag=j.id==0x21?"+CGMI:":j.id==0x22?"+CGMM:":j.id==0x23?"+CGMR:":nullptr;
   bool got=tag&&x9q_line(r,tag,text,sizeof(text));
   if(got){unsigned n=(unsigned)strlen(text);
    if(n>=2&&text[0]=='"'&&text[n-1]=='"'){memmove(text,text+1,n-2);text[n-2]=0;}
    for(const char *c=text;*c&&got;c++)if((unsigned char)*c<32||(unsigned char)*c>126)got=false;
    if(!text[0])got=false;}
   if(!got&&!x9q_plain(r,text,sizeof(text),j.id==0x25||j.id==0x43)){
    printf("X9 QMI AT: DMS 0x%04x identity text not parsed (neither %s line nor single plain line)\n",j.id,tag?tag:"tagged");
    x9q_error(j,X9Q_INTERNAL);return;}
   unsigned n=(unsigned)strlen(text);
   if((j.id==0x25&&n!=15)||(j.id==0x43&&(n<5||n>15))){x9q_error(j,X9Q_INTERNAL);return;}
   p.tlv(j.id==0x25?0x11:1,text,n);
  }
  x9q_finish(j,p);return;
 }
 x9q_error(j,X9Q_UNSUPPORTED);
}
static bool x9q_supported(unsigned service,unsigned id){
 return (service==0x0b&&(id==0x20||id==0x21||id==0x24||id==0x2f||id==0x47))||
  (service==2&&(id==0x21||id==0x22||id==0x23||id==0x25||id==0x2d||id==0x2e||id==0x3c||id==0x43))||
  (service==3&&(id==0x20||id==0x24||id==0x4d||id==0x4f));
}
static bool x9q_enqueue(uint32_t local,uint32_t port,const uint8_t *req,unsigned len,bool background=false){
 unsigned slot=local-CBQ_PORT_BASE;if(local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS)return false;
 for(x9q_job &j:x9q_jobs)if(!j.used){
  bzero(&j,sizeof(j));j.used=true;j.background=background;j.local=local;j.port=port;j.len=len;
  j.generation=x9q_clients[slot].generation;j.sim_slot=x9q_clients[slot].sim_slot;j.order=++x9q_order;j.deadline=x9q_ms()+180000;
  memcpy(j.req,req,len);j.id=x9q_u16(req+3);return true;
 }return false;
}
#include "X9WDS.inc"
#include "X9NetworkName.inc"
#include "X9Messages.inc"
static void x9q_reject(uint32_t,uint32_t,const uint8_t *,unsigned,uint16_t);
#include "X9DSDS.inc"
#include "X9GPSPDS.inc"
/* Requests answered without an AT job used to be silent (158.txt: 178 requests,
 * 14 logged answers). Log tags/lengths only; values only for 1-byte flags. */
static void x9q_reject(uint32_t local,uint32_t port,const uint8_t *req,unsigned len,uint16_t err){
 static unsigned logged;
 if(logged<256&&len>=7){logged++;char t[96];unsigned w=0;t[0]=0;
  for(unsigned i=7;i+3<=len&&w+16<sizeof(t);){unsigned k=x9q_u16(req+i+1);if(k>len-i-3)break;
   w+=(unsigned)snprintf(t+w,sizeof(t)-w,k==1?" %02x/1=%02x":" %02x/%u",req[i],k==1?req[i+3]:k);i+=3+k;}
  printf("X9 QMI AT: reject service 0x%x msg 0x%04x txn %u error %u tlvs%s\n",
   port-X9BB_PORT_BASE,x9q_u16(req+3),x9q_u16(req+1),err,w?t:" none");}
 x9bb_reply_error(local,port,req,len,err);
}
static void x9q_send(uint32_t local,uint32_t node,uint32_t port,const uint8_t *req,unsigned len){
 if(len<7)return;
 unsigned service=port-X9BB_PORT_BASE,slot=local-CBQ_PORT_BASE,id=x9q_u16(req+3);
 if(node!=X9BB_NODE||local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS||len>X9Q_REQ||!x9q_valid(req,len)){
  x9q_reject(local,port,req,len,X9Q_BAD);return;
 }
 x9q_client &c=x9q_clients[slot];
 if(x9n_request(local,port,req,len))return;
 if(x9g_send(local,port,req,len))return;
 if(x9m_request(local,port,req,len))return;
 if(service==1&&x9w_request(local,port,req,len))return;
 /* Per-client NAS/DMS/DSD binding, not a default-data mutation.
  * Jobs snapshot their route; changing binding cannot retarget old replies. */
 if((service==3&&id==0x45)||(service==2&&id==0x54)||(service==0x2a&&id==0x27)){
  // 20H392: NAS u8 0/1, DMS/DSD u32 1/2. They are NOT one wire enum.
  x9q_view sub=x9q_tlv(req,len,1);bool nas=service==3;
  unsigned value=sub.p&&sub.n==(nas?1u:4u)?(nas?sub.p[0]:x9q_u32(sub.p)):0xffffffffu;
  unsigned route=nas?value:value-1;
  if(!sub.p||sub.n!=(nas?1u:4u)||len!=(nas?11u:14u)||route>=x9_md_sim_count()){
   x9q_reject(local,port,req,len,X9Q_ARG);return;}
  if(c.sim_slot!=route&&x9n_binding_busy(slot)){x9q_reject(local,port,req,len,X9Q_BUSY);return;}
  if(c.sim_slot!=route)for(const x9q_job &j:x9q_jobs)if(j.used&&!j.probe&&j.local==local){x9q_reject(local,port,req,len,X9Q_BUSY);return;}
  c.sim_slot=(uint8_t)route;c.next=0;x9q_packet p(req);p.send(local,port);
  printf("X9 DUALSIM: service %u client 0x%x bound to SIM%u (queries route, not DDS)\n",service,local,c.sim_slot+1);return;
 }
 if(service==0x2a){x9ds_request(local,port,req,len);return;}
 if(service==0x0b&&id==0x6e){x9ds_map(local,port,req,len);return;}
 if(service==0x0b&&id==0x2e){
  x9q_view m=x9q_tlv(req,len,1);
  if(!m.p||m.n!=4||len!=14||x9q_u32(m.p)&~0x11u){x9q_reject(local,port,req,len,X9Q_UNSUPPORTED);return;}
  c.uim_mask=x9q_u32(m.p);c.next=0;x9q_packet p(req);p.tlv(0x10,m.p,4);p.send(local,port);return;
 }
 /* NAS Indication Register. 161.txt: CommCenter always asks for many tags
  * (0x10 0x17 0x19 0x1e 0x23 0x25 0x26 0x29 0x2b 0xa0 0xc0 0xd0 0xd1 ...); rejecting
  * the whole request meant it never got ServingSystem/SysInfo indications and
  * never polled 0x24 itself, so it never learned the registration state. Now
  * 0x13/0x18/0x19 are served from real AT snapshots; the rest are accepted, and logged
  * once, but no indication is ever sent for them. */
 if(service==3&&id==3){
  bool serving=c.serving,system=c.system,signal=c.signal;char unserved[96];unsigned w=0;unserved[0]=0;
  for(unsigned i=7;i<len;i+=3+x9q_u16(req+i+1)){
   unsigned k=x9q_u16(req+i+1);uint8_t tag=req[i];
   if(tag==0x13||tag==0x18||tag==0x19){
    if(k!=1||req[i+3]>1){x9q_reject(local,port,req,len,X9Q_ARG);return;}
    if(tag==0x13)serving=req[i+3]!=0;else if(tag==0x18)system=req[i+3]!=0;else signal=req[i+3]!=0;
   }else if(w+8<sizeof(unserved))w+=(unsigned)snprintf(unserved+w,sizeof(unserved)-w," %02x",tag);
  }
  static unsigned logged;
  if(w&&logged<8){logged++;printf("X9 QMI AT: NAS 0x0003 accepted; serving %d system %d signal %d; never indicated:%s\n",serving,system,signal,unserved);}
  // Mav20 registerEvents_sync (20H392 1f1ab620c) uses SigInfoEvents
  // tag19/u8, separately from ConfigSigInfo2 thresholds (6c). Subscribe at
  // bootstrap, including SA before any LTE threshold request. Unsubscribe0
  // stops this client's background samples; foreground delivery checks signal.
  if(signal!=c.signal)printf("X9 DUALSIM: NAS client 0x%x SIM%u signal events %u (tag19, real ECSQ)\n",local,c.sim_slot+1,signal);
  c.serving=serving;c.system=system;c.signal=signal;c.next=0;x9q_packet p(req);p.send(local,port);return;
 }
 /* WDS Set Event Report (0x01) / Indication Register (0x03) and DMS Set Event
  * Report (0x01): acknowledged, no indication is generated (no event source). */
 /* WDS 00c8 (including empty APN) is handled by the typed MIPC owner above. */
 /* UIM Refresh Register (0x2a): acknowledged; this bridge never sends refresh events. */
 /* NAS 0x6c Config Signal Info v2 (167.txt, right after LTE SysInfo): thresholds
  * are not applied; the client gets a 0x51 Sig Info indication from real
  * AT+ECSQ every subscription cycle instead. */
 if(service==3&&id==0x6c){c.signal=true;c.next=0;}
 if((service==1&&(id==1||id==3))||(service==2&&id==1)||(service==0x0b&&id==0x2a)||(service==3&&id==0x6c)){
  static unsigned logged;
  if(logged<8){logged++;printf("X9 QMI AT: service 0x%x msg 0x%04x registration acknowledged, %s\n",service,id,service==3&&id==0x6c?"0x51 from AT+ECSQ every 10 s":"no indications generated");}
  x9q_packet p(req);p.send(local,port);return;
 }
 /* WDA/DPM must not open a fabricated IPA endpoint. Unsupported WDS operations
  * must not promote CGATT to an IP route. */
 if(!x9q_supported(service,id)){x9q_reject(local,port,req,len,X9Q_UNSUPPORTED);return;}
 if(service==3&&id==0x20&&len!=7){
  x9q_view mask=x9q_tlv(req,len,0x10);
  if(!mask.p||mask.n!=2||len!=12||x9q_u16(mask.p)){x9q_reject(local,port,req,len,X9Q_UNSUPPORTED);return;}
 }
 /* UIM GetCardStatus: CommCenter sends one optional 1-byte TLV 0x10 (158.txt,
  * len 11). It was rejected with 48; answered with the legacy 0x10 card status. */
 bool card_status_flag=false;
 if(service==0x0b&&id==0x2f&&len==11){x9q_view f=x9q_tlv(req,len,0x10);
  if(f.p&&f.n==1){card_status_flag=true;static unsigned once;
   if(!once++)printf("X9 QMI AT: UIM 0x2f optional TLV 0x10 value %u accepted, legacy status TLV 0x10 answered\n",f.p[0]);}}
 bool opmode=service==2&&id==0x2e&&len==11&&x9q_tlv(req,len,1).n==1;
 if(!((service==0x0b&&(id==0x20||id==0x21||id==0x24))||(service==3&&id==0x20))&&len!=7&&!card_status_flag&&!opmode){x9q_reject(local,port,req,len,X9Q_ARG);return;}
 if(!x9q_enqueue(local,port,req,len))x9q_reject(local,port,req,len,X9Q_BUSY);
}
static void x9q_del(uint32_t local){
 unsigned slot=local-CBQ_PORT_BASE;if(local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS)return;
 x9g_del(slot);
 x9n_del(slot);
 x9w_del(slot);
 x9m_del(slot);
 x9ds_del(slot);
 uint64_t generation=x9q_clients[slot].generation+1;bzero(&x9q_clients[slot],sizeof(x9q_clients[slot]));x9q_clients[slot].generation=generation;
 for(unsigned i=0;i<X9Q_JOBS;i++)if(x9q_jobs[i].used&&!x9q_jobs[i].probe&&x9q_jobs[i].local==local){
  x9q_cancel(x9q_jobs[i]);
  bzero(&x9q_jobs[i],sizeof(x9q_jobs[i]));if(x9q_active==(int)i)x9q_active=-1;
 }
}
static void x9q_poll(){
 x9g_poll();
 x9w_poll();
 x9n_poll();
 x9m_poll();
 x9ds_poll();
 /* One bounded read-only pass even when D22 never creates a SIM2 client.
  * It does not send QMI responses, change radio policy or manufacture a SIM. */
 static unsigned probed;
 if(probed<4&&x9_md_sim_count()==2&&x9w_control_ready){
  for(x9q_job &j:x9q_jobs)if(!j.used&&probed<4){
   bzero(&j,sizeof(j));j.used=j.probe=j.background=true;j.local=CBQ_PORT_BASE;
   j.port=X9BB_PORT_BASE+(probed<2?0x0b:3);j.id=probed==0?0x2f:probed==1?0x47:0x4d;
   j.sim_slot=probed==3?1:0;probed++;j.req[3]=(uint8_t)j.id;
   j.len=7;j.order=++x9q_order;j.deadline=x9q_ms()+180000;
   printf("X9 DUALSIM: uim3 queued one-shot service %u msg 0x%04x SIM%u probe; D22 gate unchanged\n",j.port-X9BB_PORT_BASE,j.id,j.sim_slot+1);
  }
 }
 uint64_t now=x9q_ms();
 /* CELL225 user-plane probe: rounds at +1/+6/+15/+40 s after each newly
  * active bearer generation, at most 24 rounds per boot, one queued at a time. */
 {static uint64_t seen_gen[CBP_INTERFACES],t0[CBP_INTERFACES];static unsigned round[CBP_INTERFACES],total;
  static const uint64_t at_ms[]={1000,6000,15000,40000};
  bool queued=false;for(const x9q_job &q:x9q_jobs)if(q.used&&q.ctx)queued=true;
  for(unsigned i=0;i<CBP_INTERFACES&&!queued&&total<24;i++){
   const x9w_bearer &b=x9w_bearers[i];
   if(!b.used||!b.active)continue;
   if(b.generation!=seen_gen[i]){seen_gen[i]=b.generation;t0[i]=now;round[i]=0;}
   if(round[i]>=4||now<t0[i]+at_ms[round[i]])continue;
   for(x9q_job &j:x9q_jobs)if(!j.used){
    bzero(&j,sizeof(j));j.used=j.probe=j.background=j.ctx=true;j.local=CBQ_PORT_BASE;
    j.port=X9BB_PORT_BASE+0x01;j.sim_slot=b.sim_slot;j.record=++total;j.len=7;
    j.order=++x9q_order;j.deadline=now+90000;
    printf("X9 CELL ATCTX: SIM%u round %u queued mux %u gen %llu +%llu ms cid %u channel %u\n",
     b.sim_slot+1,j.record,i+1,b.generation,now-t0[i],b.cid,b.channel);
    round[i]++;queued=true;break;
   }
  }
 }
 for(unsigned i=0;i<X9Q_JOBS;i++)if(x9q_jobs[i].used&&now>=x9q_jobs[i].deadline){
  int active=x9q_active;x9q_error(x9q_jobs[i],X9Q_NOT_READY);if(active!=(int)i)x9q_active=active;
 }
 if(x9q_active<0){
  if(!x9_md_at_online())return;
  if(x9_md_sim_count()==2&&!x9w_control_ready)return;
  for(unsigned i=0;i<X9Q_JOBS;i++)if(x9q_jobs[i].used&&(x9q_active<0||x9q_jobs[i].order<x9q_jobs[x9q_active].order))x9q_active=(int)i;
  if(x9q_active<0)return;x9q_start(x9q_jobs[x9q_active]);if(x9q_active<0)return;
 }
 x9q_job &j=x9q_jobs[x9q_active];
 if(!j.probe&&j.generation!=x9q_clients[j.local-CBQ_PORT_BASE].generation){x9q_del(j.local);return;}
 if(j.token){
  static char response[X9_AT_RESPONSE_MAX];int result;
  int ready=j.mipc?x9_md_sim_at_take(j.token,response,sizeof(response),&result):x9_md_at_take(j.token,response,sizeof(response),&result);
  if(ready==0){if(j.mipc&&now>=j.token_deadline)x9q_error(j,X9Q_NOT_READY);return;}
  j.token=0;if(ready<0){x9q_error(j,X9Q_INTERNAL);return;}
  x9q_result(j,response,result);bzero(response,sizeof(response));return;
 }
 if(!x9_md_at_online()){x9q_error(j,X9Q_NOT_READY);return;}
 /* CFUN mode changes (stage 21) get the init script's 60 s; a timeout would
  * quarantine ttyC0 until boot. */
 j.mipc=x9_md_sim_count()==2&&j.stage!=21;
 j.token=j.mipc?x9_md_sim_at_submit(j.sim_slot,j.command):x9_md_at_submit(j.command,j.stage==21?60000:10000);
 j.token_deadline=now+10000;
 if(!j.token)x9q_error(j,X9Q_BUSY);
}
static void x9q_subscriptions(){
 /* At most six background reads per ten seconds per client, one active
  * cycle globally. Foreground jobs always precede the next background cycle. */
 for(const x9q_job &j:x9q_jobs)if(j.used)return;
 if(!x9_md_at_online())return;uint64_t now=x9q_ms();
 static unsigned cursor;
 for(unsigned step=0;step<CBQ_MAX_CLIENTS;step++){
  unsigned i=(cursor+step)%CBQ_MAX_CLIENTS;
  x9q_client &c=x9q_clients[i];if(now<c.next||(!c.uim_mask&&!c.serving&&!c.system&&!c.signal&&!c.dsd_system))continue;
  c.next=now+10000;uint8_t req[7]={};unsigned local=CBQ_PORT_BASE+i;
  if(c.uim_mask&1){req[3]=0x2f;x9q_enqueue(local,X9BB_PORT_BASE+0x0b,req,7,true);}
  if(c.uim_mask&16){req[3]=0x47;x9q_enqueue(local,X9BB_PORT_BASE+0x0b,req,7,true);}
  if(c.serving){req[3]=0x24;x9q_enqueue(local,X9BB_PORT_BASE+3,req,7,true);}
  if(c.system){req[3]=0x4d;x9q_enqueue(local,X9BB_PORT_BASE+3,req,7,true);}
  if(c.signal){req[3]=0x4f;x9q_enqueue(local,X9BB_PORT_BASE+3,req,7,true);}
  if(c.dsd_system){req[3]=0x24;x9q_enqueue(local,X9BB_PORT_BASE+0x2a,req,7,true);}
  cursor=(i+1)%CBQ_MAX_CLIENTS;break;
 }
}
