/* Single MD owner, independent MIPC mailbox. The actual RIL uses ttyCMIPC1,
 * not libmipc's default ttyCMIPC0. Wire contract was checked against this
 * device's libmipc serializer/init and libmtkmipc-ril, not just old headers.
 * Header: LE magic24541984, padding4, SIM mask8, flags9, id10, txid12, len14.
 * TLVs: LE type16/len16, payload, eight-byte padded. Never log payloads.
 * Bounded frames/slots; oversized frames or event overflow quarantine until boot. Sent mutations
 * remain owned indefinitely, including after host timeout. No txid wrap/reuse.
 */
#define X9_MI_SLOTS 8u
#define X9_MI_EVENTS 8u
static LCK_GRP_DECLARE(x9_mi_grp,"x9_mipc_bridge");
static LCK_MTX_DECLARE(x9_mi_lock,&x9_mi_grp);
struct x9_mi_slot {
 uint64_t token; unsigned state,len; uint16_t id,txid; uint8_t mask,cancelled;
 uint8_t frame[X9_MIPC_FRAME_MAX];
};
static struct x9_mi_slot x9_mi_slots[X9_MI_SLOTS];
static struct { unsigned len; uint8_t frame[X9_MIPC_FRAME_MAX]; } x9_mi_events[X9_MI_EVENTS];
static unsigned x9_mi_head,x9_mi_count,x9_mi_next,x9_mi_online,x9_mi_poisoned;
static uint8_t x9_mi_stream[X9_MIPC_FRAME_MAX];
static unsigned x9_mi_bytes;
static unsigned x9mi_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static int x9mi_enabled(void){return x9_at_enabled()&&sweet_x9_arg("x9_mipc",1);}
unsigned x9_md_sim_count(void){return sweet_x9_arg("x9_dual_sim",0)==1?2:1;}
/* libmtk-ril RfxSender::atSendCommandFullNolockAck, 0x23dcf4..0x23dd58:
 * mipc_msg_init(3, 1<<slot), add_tlv(8100, strlen(command)+1), sync, get 8100.
 * Slot table 0x150d1c = {2,4,8}; Android slot 0 defaults to mask 1.
 * No SIM mapping/capability/PIN/NV mutation is allowed on this entrypoint. */
static int x9mi_at_read(const char *cmd){
 static const char *const reads[]={"AT+CPIN?","AT+CREG?","AT+CEREG?","AT+C5GREG?",
  "AT+CGATT?","AT+COPS?","AT+CSQ","AT+ECSQ","AT+CIMI","AT+CGSN",
  "AT+CGMI","AT+CGMM","AT+CGMR","AT+CFUN?"};
 for(unsigned i=0;i<sizeof(reads)/sizeof(reads[0]);i++)if(!strcmp(cmd,reads[i]))return 1;
 /* CRSM 176 READ BINARY, 178 READ RECORD, 192 GET RESPONSE. Only printable
  * decimal/hex/path punctuation, one command, no data field or AT chaining. */
 if(strncmp(cmd,"AT+CRSM=176,",12)&&strncmp(cmd,"AT+CRSM=178,",12)&&strncmp(cmd,"AT+CRSM=192,",12))return 0;
 const char *p=cmd+12;
 for(unsigned field=0;field<4;field++){
  unsigned digits=0,value=0;
  while(*p>='0'&&*p<='9'){if(++digits>5)return 0;value=value*10+(*p++-'0');}
  if(!digits||value>(field?255:65535)||*p++!=',')return 0;
 }
 if(*p++!=','||*p++!='"')return 0;
 unsigned n=0;while((*p>='0'&&*p<='9')||(*p>='A'&&*p<='F')){p++;if(++n>32)return 0;}
 return n&&!(n%4)&&*p++=='"'&&!*p;
}
int x9_md_mipc_online(void){
 lck_mtx_lock(&x9_mi_lock);int on=x9_mi_online&&!x9_mi_poisoned;
 lck_mtx_unlock(&x9_mi_lock);return on&&x9_md_at_online();
}
uint64_t x9_md_mipc_submit(const uint8_t *p,uint32_t n){
 if(!p||n<16||n>X9_MIPC_FRAME_MAX||x9mi_u16(p+14)!=n-16||
   p[0]!=0x84||p[1]!=0x19||p[2]!=0x54||p[3]!=0x24||
   p[4]||p[5]||p[6]||p[7]||p[9])return 0;
 unsigned id=x9mi_u16(p+10);
 /* OPEN/version handshake and two IND registrations are required by libmipc.
  * Still no arbitrary MIPC, SIM/NV writes or speculative EIF/EIPPORT commands. */
 if(id==0x301){if(p[8]!=0xff)return 0;}
 else if(id==3){
  unsigned k=n>=20?x9mi_u16(p+18):0;
  if(x9_md_sim_count()!=2||(p[8]!=1&&p[8]!=2)||n<24||x9mi_u16(p+16)!=0x8100||
     k<3||k>X9_AT_COMMAND_MAX||n!=16+((k+11)&~7u)||p[20+k-1]||
     strnlen((const char *)p+20,k)!=k-1||!x9mi_at_read((const char *)p+20))return 0;
 }
 else if((p[8]!=1&&(p[8]!=2||x9_md_sim_count()!=2))||
    (id!=0x201&&id!=0x203&&id!=0x205&&id!=0x219&&id!=0x307))return 0;
 uint64_t token=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mi_online&&!x9_mi_poisoned&&x9_mi_next<65534)for(unsigned i=0;i<X9_MI_SLOTS;i++)if(!x9_mi_slots[i].state){
  struct x9_mi_slot *s=&x9_mi_slots[i];bzero(s,sizeof(*s));
  token=++x9_mi_next;s->token=token;s->txid=(uint16_t)token;s->id=(uint16_t)id;s->mask=p[8];s->len=n;
  memcpy(s->frame,p,n);s->frame[12]=(uint8_t)token;s->frame[13]=(uint8_t)(token>>8);s->state=1;break;
 }
 lck_mtx_unlock(&x9_mi_lock);return token;
}
uint64_t x9_md_sim_at_submit(unsigned sim,const char *cmd){
 if(!cmd||sim>=x9_md_sim_count()||x9_md_sim_count()!=2)return 0;
 size_t n=strnlen(cmd,X9_AT_COMMAND_MAX);
 if(n<4||n>=X9_AT_COMMAND_MAX||cmd[n-1]!='\r')return 0;
 uint8_t frame[16+((X9_AT_COMMAND_MAX+11)&~7u)]={0x84,0x19,0x54,0x24};
 frame[8]=(uint8_t)(1u<<sim);frame[10]=3;frame[16]=0;frame[17]=0x81;
 /* RfxSender sends a NUL-terminated string, NOT the tty trailing CR. */
 frame[18]=(uint8_t)n;frame[19]=(uint8_t)(n>>8);memcpy(frame+20,cmd,n-1);
 unsigned len=(unsigned)((n+11)&~7u);frame[14]=(uint8_t)len;frame[15]=(uint8_t)(len>>8);
 uint64_t token=x9_md_mipc_submit(frame,16+len);bzero(frame,sizeof(frame));return token;
}
void x9_md_sim_at_cancel(uint64_t token){
 lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MI_SLOTS;i++){
  struct x9_mi_slot *s=&x9_mi_slots[i];if(!s->state||s->token!=token||s->id!=3)continue;
  if(s->state==2)s->cancelled=1;else bzero(s,sizeof(*s));break;
 }
 lck_mtx_unlock(&x9_mi_lock);
}
int x9_md_mipc_take(uint64_t token,uint8_t *out,uint32_t room,uint32_t *len){
 if(!token||!out||!len)return -1;
 int found=-1;*len=0;lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MI_SLOTS;i++)if(x9_mi_slots[i].state&&x9_mi_slots[i].token==token){
  struct x9_mi_slot *s=&x9_mi_slots[i];found=0;
  if(s->state==3){
   if(s->len<=room){memcpy(out,s->frame,s->len);*len=s->len;found=1;bzero(s,sizeof(*s));}
   else found=-1;
  }break;
 }
 lck_mtx_unlock(&x9_mi_lock);return found;
}
/* Single QMI owner calls this. No payload (ICCID/IMSI/EF bytes) in diagnostics.
 * A CNF is a complete response, not a successful AT result: require exactly
 * one final OK/ERROR line. Reject truncated, duplicate or nonterminal results. */
int x9_md_sim_at_take(uint64_t token,char *out,uint32_t room,int *result){
 static uint8_t frame[X9_MIPC_FRAME_MAX];uint32_t n=0;
 if(!out||!room||!result)return -1;
 int ready=x9_md_mipc_take(token,frame,sizeof(frame),&n);if(ready!=1)return ready;
 const uint8_t *text=NULL;unsigned len=0,at=16;int good=n>=16&&x9mi_u16(frame+10)==4;
 while(good&&at<n){
  if(n-at<4){good=0;break;}
  unsigned tag=x9mi_u16(frame+at)&0x7fff,k=x9mi_u16(frame+at+2),size=k?((k+11)&~7u):16;
  if(size>n-at){good=0;break;}
  if(tag==0x100){if(text){good=0;break;}text=frame+at+4;len=k;}
  else if(tag==0&&(k!=4||frame[at+4]||frame[at+5]||frame[at+6]||frame[at+7]))good=0;
  at+=size;
 }
 out[0]=0;*result=X9_AT_OVERFLOW;
 if(good&&text&&len&&len<=room&&text[len-1]==0&&strnlen((const char *)text,len)==len-1){
  unsigned pos=0,finals=0;int final_result=X9_AT_MODEM_ERROR;
  while(pos<len-1&&good){
   while(pos<len-1&&(text[pos]=='\r'||text[pos]=='\n'))pos++;
   unsigned start=pos;
   while(pos<len-1&&text[pos]!='\r'&&text[pos]!='\n'){
    if(text[pos]<32||text[pos]>126)good=0;pos++;
   }
   unsigned k=pos-start;if(!k)continue;
   if(finals){good=0;break;}
   if(k==2&&!memcmp(text+start,"OK",2)){finals++;final_result=X9_AT_OK;}
   else if((k==5&&!memcmp(text+start,"ERROR",5))||
      (k>=11&&!memcmp(text+start,"+CME ERROR:",11))||
      (k>=11&&!memcmp(text+start,"+CMS ERROR:",11))){finals++;final_result=X9_AT_MODEM_ERROR;}
  }
  if(good&&finals==1){memcpy(out,text,len);*result=final_result;}
 }
 if(*result==X9_AT_OVERFLOW){static unsigned logged;if(logged<12){logged++;
  printf("X9 MIPC AT: CNF txn %llu rejected: frame bytes %u TLV text bytes %u framing %d (payload private)\n",token,n,len,good);}}
 bzero(frame,sizeof(frame));return 1;
}
int x9_md_mipc_event(uint8_t *out,uint32_t room,uint32_t *len){
 if(!out||!len)return -1;int rc=0;*len=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mi_poisoned)rc=-1;
 else if(x9_mi_count){
  unsigned n=x9_mi_events[x9_mi_head].len;
  if(n>room)rc=-1;
  else{memcpy(out,x9_mi_events[x9_mi_head].frame,n);*len=n;rc=1;
   bzero(&x9_mi_events[x9_mi_head],sizeof(x9_mi_events[0]));x9_mi_head=(x9_mi_head+1)%X9_MI_EVENTS;x9_mi_count--;}
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
static void x9mi_online(int on){
 lck_mtx_lock(&x9_mi_lock);x9_mi_online=on&&x9mi_enabled();
 /* MD loss is not deactivation proof. Do not discard any mutation/token. */
 if(!on){x9_mi_poisoned=1;x9_mi_bytes=0;bzero(x9_mi_stream,sizeof(x9_mi_stream));}
 lck_mtx_unlock(&x9_mi_lock);
}
/* Header-only diagnostics (magic/pad/mask/flag/id/txid/len), never TLV payload. */
static void x9mi_hdr_log(const char *what,const uint8_t *h){
 printf("X9 MIPC: %s hdr %02x%02x%02x%02x %02x%02x%02x%02x mask 0x%02x flag 0x%02x id 0x%04x txn %u len %u\n",what,
  h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8],h[9],x9mi_u16(h+10),x9mi_u16(h+12),x9mi_u16(h+14));
}
static void x9mi_poison(const char *why){
 x9_mi_poisoned=1;printf("X9 MIPC: quarantined (%s); no bearer reuse\n",why);
 if(x9_mi_bytes>=16)x9mi_hdr_log("offending",x9_mi_stream);
}
/* Framing follows the device's libmipc RX loop (mipc_init+0x4c0, 158.txt analysis):
 * READCOM(16), then READCOM(len@14); no check of magic, byte 9 (flag) or the SIM
 * mask on CNFs. Dispatch is msg_id>>14: 0 = CNF matched by txid, 1 = IND fanned
 * out per SIM bit in byte 8, 2 = MD->AP command. hs quarantined on the first
 * OPEN_CNF because it rejected byte 9 != 0 / required mask bit 0. */
static void x9mi_rx(const uint8_t *p,unsigned n){
 static unsigned rx_logged;
 lck_mtx_lock(&x9_mi_lock);
 if(!x9mi_enabled()||x9_mi_poisoned){lck_mtx_unlock(&x9_mi_lock);return;}
 while(n&&!x9_mi_poisoned){
  unsigned need=x9_mi_bytes<16?16:16+x9mi_u16(x9_mi_stream+14);
  if(need>X9_MIPC_FRAME_MAX){x9mi_poison("oversized frame");break;}
  unsigned k=need-x9_mi_bytes;if(k>n)k=n;
  memcpy(x9_mi_stream+x9_mi_bytes,p,k);x9_mi_bytes+=k;p+=k;n-=k;
  if(x9_mi_bytes<16)continue;
  need=16+x9mi_u16(x9_mi_stream+14);
  if(need>X9_MIPC_FRAME_MAX){x9mi_poison("oversized frame");break;}
  if(x9_mi_bytes<need)continue;
  unsigned id=x9mi_u16(x9_mi_stream+10),tx=x9mi_u16(x9_mi_stream+12),kind=id>>14;
  if(rx_logged<64){rx_logged++;x9mi_hdr_log("RX",x9_mi_stream);}
  if(x9_mi_stream[0]!=0x84||x9_mi_stream[1]!=0x19||x9_mi_stream[2]!=0x54||x9_mi_stream[3]!=0x24)
   printf("X9 MIPC: RX magic differs (libmipc does not check it), framed by length\n");
  if(kind==0){
   unsigned i=0;
   for(;i<X9_MI_SLOTS;i++){
    struct x9_mi_slot *s=&x9_mi_slots[i];
    if(s->state==2&&s->txid==tx&&id==(unsigned)s->id+1){
     if(s->cancelled)bzero(s,sizeof(*s));
     else{s->len=need;memcpy(s->frame,x9_mi_stream,need);s->state=3;}break;
    }
   }
   if(i==X9_MI_SLOTS)printf("X9 MIPC: CNF id 0x%04x txn %u has no owner, dropped\n",id,tx);
  }else if(kind==1){
   if((id==0x4202||id==0x4302)&&(x9_mi_stream[8]&((1u<<x9_md_sim_count())-1))){
    if(x9_mi_count==X9_MI_EVENTS){x9mi_poison("event queue overflow");break;}
    unsigned at=(x9_mi_head+x9_mi_count++)%X9_MI_EVENTS;
    x9_mi_events[at].len=need;memcpy(x9_mi_events[at].frame,x9_mi_stream,need);
   }
  }else printf("X9 MIPC: MD command id 0x%04x txn %u ignored (no AP handler)\n",id,tx);
  bzero(x9_mi_stream,sizeof(x9_mi_stream));x9_mi_bytes=0;
 }
 lck_mtx_unlock(&x9_mi_lock);
}
static void x9mi_tick(void){
 if(!x9_md_at_online())return;
 static uint8_t frame[X9_MIPC_FRAME_MAX];unsigned len=0,id=0,tx=0;
 lck_mtx_lock(&x9_mi_lock);
 if(x9_mi_online&&!x9_mi_poisoned)for(unsigned i=0;i<X9_MI_SLOTS;i++)if(x9_mi_slots[i].state==1){
  struct x9_mi_slot *s=&x9_mi_slots[i];s->state=2;len=s->len;id=s->id;tx=s->txid;memcpy(frame,s->frame,len);break;
 }
 lck_mtx_unlock(&x9_mi_lock);if(!len)return;
 int rc=x9md_tx(5,217,0,16+len,0,frame,len);bzero(frame,sizeof(frame));
 printf("X9 MIPC: TX msg 0x%04x txn %u bytes %u transport %d\n",id,tx,len,rc);
 if(rc<0)x9mi_online(0); /* possible partial side effect: retain sent owner */
}
