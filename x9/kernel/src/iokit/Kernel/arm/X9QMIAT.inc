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
struct x9q_client {uint64_t generation,next;uint32_t uim_mask;bool serving,system;};
static x9q_client x9q_clients[CBQ_MAX_CLIENTS];
struct x9q_job {
 bool used,background;uint32_t local,port,len;uint64_t generation,order,deadline,token;
 uint8_t req[X9Q_REQ];uint16_t id;unsigned stage;
 char command[X9_AT_COMMAND_MAX];
 int sim; /* 0 unknown, 1 ready, 2 PIN, 3 PUK */
 uint16_t file,offset,amount,record_size,record_count;uint8_t structure;
 char path[33];uint8_t raw[512];unsigned raw_len,record;
 uint8_t aid[4][32],aid_len[4],aid_type[4];unsigned apps;
 int reg[3],attached,act,csq;uint16_t mcc,mnc;unsigned mnc_digits;
};
static x9q_job x9q_jobs[X9Q_JOBS];static int x9q_active=-1;static uint64_t x9q_order;
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
 if(!j.background){x9q_packet p(j.req,err);p.send(j.local,j.port);
  printf("X9 QMI AT: response service 0x%x msg 0x%04x txn %u error %u\n",j.port-X9BB_PORT_BASE,j.id,x9q_u16(j.req+1),err);}
 if(j.token)x9_md_at_cancel(j.token);bzero(&j,sizeof(j));x9q_active=-1;
}
static void x9q_finish(x9q_job &j,x9q_packet &p,uint16_t ind=0){
 if(!j.background||!x9q_u16(p.p+10))p.send(j.local,j.port,j.background,ind);
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
 if(!read.p||read.n!=4)return "read TLV";
 j.file=x9q_u16(file.p);j.offset=x9q_u16(read.p);j.amount=x9q_u16(read.p+2);
 /* QMI length 0 = whole file / whole record; resolved from the FCP. */
 if(j.amount>255)return "length over one CRSM read";
 unsigned st=session.p[0];
 /* CRSM has no proven arbitrary-AID/slot routing on this MD. Only primary GW
  * (empty AID, current USIM) or card-1 MF files; never map slot-2 to slot-1. */
 if(st!=0&&st!=6)return "session type";
 const char hex[]="0123456789ABCDEF";
 for(unsigned i=0;i<file.p[2];i+=2){uint16_t f=x9q_u16(file.p+3+i);unsigned o=i*2;
  j.path[o]=hex[f>>12];j.path[o+1]=hex[(f>>8)&15];j.path[o+2]=hex[(f>>4)&15];j.path[o+3]=hex[f&15];}
 j.path[file.p[2]*2]=0;
 bool mf=!strcmp(j.path,"3F00")&&(j.file==0x2fe2||j.file==0x2f00);
 bool usim=st==0&&!strcmp(j.path,"3F007FFF")&&j.file>=0x6f00&&j.file<=0x6fff;
 if(!mf&&!usim)return "path/file outside MF ICCID/DIR and USIM 6Fxx";
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
static void x9q_card(x9q_job &j){
 uint8_t s[256];unsigned n=0;memset(s,0xff,8);n=8;s[n++]=1;
 /* CPIN does not distinguish PIN verified from disabled or expose retries.
  * Unknown PIN state (0) makes its required retry bytes unavailable, not a
  * claimed retry budget. Personalization remains unknown, never invented. */
 s[n++]=1;s[n++]=0;s[n++]=0;s[n++]=0;s[n++]=0;s[n++]=(uint8_t)j.apps;
 for(unsigned i=0;i<j.apps;i++){
  if(j.aid_type[i]==2&&s[0]==0xff){s[0]=(uint8_t)i;s[1]=0;}
  s[n++]=j.aid_type[i];s[n++]=j.aid_type[i]==2&&j.sim==1?7:1;
  s[n++]=0;s[n++]=0xff;s[n++]=0;s[n++]=0;s[n++]=j.aid_len[i];
  memcpy(s+n,j.aid[i],j.aid_len[i]);n+=j.aid_len[i];
  memset(s+n,0,7);n+=7;
 }
 x9q_packet p(j.req);p.tlv(0x10,s,n);x9q_finish(j,p,0x32);
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
static void x9q_nas(x9q_job &j){
 if(j.reg[0]<0&&j.reg[1]<0&&j.reg[2]<0){x9q_error(j,X9Q_NOT_READY);return;}
 int best=x9q_registered(j.reg[2])?j.reg[2]:x9q_registered(j.reg[1])?j.reg[1]:j.reg[0]>=0?j.reg[0]:j.reg[1];
 uint8_t radio=x9q_radio(j.act);
 if(x9q_registered(j.reg[2]))radio=12;else if(x9q_registered(j.reg[1]))radio=8;
 x9q_packet p(j.req);
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
 }else{ /* Validity-free service status only: no invented LTE/NR cell/RSRP. */
  if(j.reg[1]>=0){uint8_t lte[3]={(uint8_t)(x9q_registered(j.reg[1])?2:0),(uint8_t)(x9q_registered(j.reg[1])?2:0),0};p.tlv(0x14,lte,3);}
  if(j.reg[2]>=0){uint8_t nr[3]={(uint8_t)(x9q_registered(j.reg[2])?2:0),(uint8_t)(x9q_registered(j.reg[2])?2:0),0};p.tlv(0x4a,nr,3);}
 }
 x9q_finish(j,p,j.id==0x4d?0x4e:0x24);
}
static void x9q_start(x9q_job &j){
 unsigned service=j.port-X9BB_PORT_BASE;
 if(service==1){x9q_error(j,X9Q_UNSUPPORTED);
 }else if(service==0x0b||(service==2&&j.id==0x3c)){
  if(service==0x0b&&(j.id==0x20||j.id==0x21)&&!x9q_read_args(j)){x9q_error(j,X9Q_ARG);return;}
  x9q_command(j,"AT+CPIN?\r",1);
 }else if(service==3){
  for(int &r:j.reg)r=-1;j.attached=j.act=j.csq=-1;x9q_command(j,"AT+CREG?\r",30);
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
 unsigned service=j.port-X9BB_PORT_BASE;bool ok=result==X9_AT_OK;
 if(result==X9_AT_TIMEOUT||result==X9_AT_OFFLINE){x9q_error(j,X9Q_NOT_READY);return;}
 if(result==X9_AT_OVERFLOW){x9q_error(j,X9Q_INTERNAL);return;}
 if(service==1){x9q_error(j,X9Q_UNSUPPORTED);return;}
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
   x9q_packet p(j.req);
   if(j.id==0x2f){uint8_t absent[15]={};memset(absent,0xff,8);absent[8]=1;p.tlv(0x10,absent,sizeof(absent));}
   else{uint8_t absent[11]={1,1,0,0,0,1,0,0,0,1,0};p.tlv(0x10,absent,sizeof(absent));}
   x9q_finish(j,p,j.id==0x2f?0x32:0x48);return;
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
  if(service==2||j.id==0x47)x9q_sim_command(j,176,0x2fe2,0,0,10,"3F00",2);
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
   if(service==2){char digits[21];unsigned k=0;
    for(unsigned i=0;i<n;i++)for(unsigned half=0;half<2;half++){
     unsigned d=(data[i]>>(half*4))&15;if(d==15&&i==9&&half==1)continue;
     if(d>9){x9q_error(j,X9Q_INTERNAL);return;}digits[k++]=(char)('0'+d);}
    p.tlv(1,digits,k);
   }else{uint8_t slot[21]={1,2,0,0,0,1,0,0,0,1,10};memcpy(slot+11,data,10);p.tlv(0x10,slot,sizeof(slot));}
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
 return (service==0x0b&&(id==0x20||id==0x21||id==0x2f||id==0x47))||
  (service==2&&(id==0x21||id==0x22||id==0x23||id==0x25||id==0x2d||id==0x3c||id==0x43))||
  (service==3&&(id==0x20||id==0x24||id==0x4d));
}
static bool x9q_enqueue(uint32_t local,uint32_t port,const uint8_t *req,unsigned len,bool background=false){
 unsigned slot=local-CBQ_PORT_BASE;if(local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS)return false;
 for(x9q_job &j:x9q_jobs)if(!j.used){
  bzero(&j,sizeof(j));j.used=true;j.background=background;j.local=local;j.port=port;j.len=len;
  j.generation=x9q_clients[slot].generation;j.order=++x9q_order;j.deadline=x9q_ms()+180000;
  memcpy(j.req,req,len);j.id=x9q_u16(req+3);return true;
 }return false;
}
#include "X9WDS.inc"
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
 if(service==1&&x9w_request(local,port,req,len))return;
 if(service==0x0b&&id==0x2e){
  x9q_view m=x9q_tlv(req,len,1);
  if(!m.p||m.n!=4||len!=14||x9q_u32(m.p)&~0x11u){x9q_reject(local,port,req,len,X9Q_UNSUPPORTED);return;}
  c.uim_mask=x9q_u32(m.p);c.next=0;x9q_packet p(req);p.tlv(0x10,m.p,4);p.send(local,port);return;
 }
 if(service==3&&id==3){
  bool serving=c.serving,system=c.system;
  for(unsigned i=7;i<len;i+=3+x9q_u16(req+i+1)){
   unsigned k=x9q_u16(req+i+1);uint8_t tag=req[i];
   if(k!=1||req[i+3]>1||(tag!=0x13&&tag!=0x18&&req[i+3])){x9q_reject(local,port,req,len,X9Q_UNSUPPORTED);return;}
   if(tag==0x13)serving=req[i+3]!=0;else if(tag==0x18)system=req[i+3]!=0;
  }
  c.serving=serving;c.system=system;c.next=0;x9q_packet p(req);p.send(local,port);return;
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
 if(!((service==0x0b&&(id==0x20||id==0x21))||(service==3&&id==0x20))&&len!=7&&!card_status_flag){x9q_reject(local,port,req,len,X9Q_ARG);return;}
 if(!x9q_enqueue(local,port,req,len))x9q_reject(local,port,req,len,X9Q_BUSY);
}
static void x9q_del(uint32_t local){
 unsigned slot=local-CBQ_PORT_BASE;if(local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS)return;
 x9w_del(slot);
 uint64_t generation=x9q_clients[slot].generation+1;bzero(&x9q_clients[slot],sizeof(x9q_clients[slot]));x9q_clients[slot].generation=generation;
 for(unsigned i=0;i<X9Q_JOBS;i++)if(x9q_jobs[i].used&&x9q_jobs[i].local==local){
  if(x9q_jobs[i].token)x9_md_at_cancel(x9q_jobs[i].token);
  bzero(&x9q_jobs[i],sizeof(x9q_jobs[i]));if(x9q_active==(int)i)x9q_active=-1;
 }
}
static void x9q_poll(){
 x9w_poll();
 uint64_t now=x9q_ms();
 for(unsigned i=0;i<X9Q_JOBS;i++)if(x9q_jobs[i].used&&now>=x9q_jobs[i].deadline){
  int active=x9q_active;x9q_error(x9q_jobs[i],X9Q_NOT_READY);if(active!=(int)i)x9q_active=active;
 }
 if(x9q_active<0){
  if(!x9_md_at_online())return;
  for(unsigned i=0;i<X9Q_JOBS;i++)if(x9q_jobs[i].used&&(x9q_active<0||x9q_jobs[i].order<x9q_jobs[x9q_active].order))x9q_active=(int)i;
  if(x9q_active<0)return;x9q_start(x9q_jobs[x9q_active]);if(x9q_active<0)return;
 }
 x9q_job &j=x9q_jobs[x9q_active];
 if(j.generation!=x9q_clients[j.local-CBQ_PORT_BASE].generation){x9q_del(j.local);return;}
 if(j.token){
  static char response[X9_AT_RESPONSE_MAX];int result;
  int ready=x9_md_at_take(j.token,response,sizeof(response),&result);
  if(ready==0)return;
  j.token=0;if(ready<0){x9q_error(j,X9Q_INTERNAL);return;}
  x9q_result(j,response,result);bzero(response,sizeof(response));return;
 }
 if(!x9_md_at_online()){x9q_error(j,X9Q_NOT_READY);return;}
 j.token=x9_md_at_submit(j.command,10000);
 if(!j.token)x9q_error(j,X9Q_BUSY);
}
static void x9q_subscriptions(){
 /* At most four background reads per ten seconds per client, one active
  * cycle globally. Foreground jobs always precede the next background cycle. */
 for(const x9q_job &j:x9q_jobs)if(j.used)return;
 if(!x9_md_at_online())return;uint64_t now=x9q_ms();
 for(unsigned i=0;i<CBQ_MAX_CLIENTS;i++){
  x9q_client &c=x9q_clients[i];if(now<c.next||(!c.uim_mask&&!c.serving&&!c.system))continue;
  c.next=now+10000;uint8_t req[7]={};unsigned local=CBQ_PORT_BASE+i;
  if(c.uim_mask&1){req[3]=0x2f;x9q_enqueue(local,X9BB_PORT_BASE+0x0b,req,7,true);}
  if(c.uim_mask&16){req[3]=0x47;x9q_enqueue(local,X9BB_PORT_BASE+0x0b,req,7,true);}
  if(c.serving){req[3]=0x24;x9q_enqueue(local,X9BB_PORT_BASE+3,req,7,true);}
  if(c.system){req[3]=0x4d;x9q_enqueue(local,X9BB_PORT_BASE+3,req,7,true);}
  break;
 }
}
