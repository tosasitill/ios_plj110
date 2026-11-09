/* Single MD owner, independent MIPC mailbox. The actual RIL uses ttyCMIPC1,
 * not libmipc's default ttyCMIPC0. Wire contract was checked against this
 * device's libmipc serializer/init and libmtkmipc-ril, not just old headers.
 * Header: LE magic24541984, padding4, SIM mask8, flags9, id10, txid12, len14.
 * TLVs: LE type16/len16, payload, eight-byte padded. Never log payloads.
 * Bounded frames/slots; unknown framing quarantines until boot. Sent mutations
 * remain owned indefinitely, including after host timeout. No txid wrap/reuse.
 */
#define X9_MI_SLOTS 8u
#define X9_MI_EVENTS 8u
static LCK_GRP_DECLARE(x9_mi_grp,"x9_mipc_bridge");
static LCK_MTX_DECLARE(x9_mi_lock,&x9_mi_grp);
struct x9_mi_slot {
 uint64_t token; unsigned state,len; uint16_t id,txid; uint8_t mask;
 uint8_t frame[X9_MIPC_FRAME_MAX];
};
static struct x9_mi_slot x9_mi_slots[X9_MI_SLOTS];
static struct { unsigned len; uint8_t frame[X9_MIPC_FRAME_MAX]; } x9_mi_events[X9_MI_EVENTS];
static unsigned x9_mi_head,x9_mi_count,x9_mi_next,x9_mi_online,x9_mi_poisoned;
static uint8_t x9_mi_stream[X9_MIPC_FRAME_MAX];
static unsigned x9_mi_bytes;
static unsigned x9mi_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static int x9mi_enabled(void){return x9_at_enabled()&&sweet_x9_arg("x9_mipc",1);}
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
 else if(p[8]!=1||(id!=0x201&&id!=0x203&&id!=0x205&&id!=0x219&&id!=0x307))return 0;
 uint64_t token=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mi_online&&!x9_mi_poisoned&&x9_mi_next<65534)for(unsigned i=0;i<X9_MI_SLOTS;i++)if(!x9_mi_slots[i].state){
  struct x9_mi_slot *s=&x9_mi_slots[i];bzero(s,sizeof(*s));
  token=++x9_mi_next;s->token=token;s->txid=(uint16_t)token;s->id=(uint16_t)id;s->mask=p[8];s->len=n;
  memcpy(s->frame,p,n);s->frame[12]=(uint8_t)token;s->frame[13]=(uint8_t)(token>>8);s->state=1;break;
 }
 lck_mtx_unlock(&x9_mi_lock);return token;
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
static void x9mi_rx(const uint8_t *p,unsigned n){
 lck_mtx_lock(&x9_mi_lock);
 if(!x9mi_enabled()||x9_mi_poisoned){lck_mtx_unlock(&x9_mi_lock);return;}
 while(n&&!x9_mi_poisoned){
  unsigned need=x9_mi_bytes<16?16:16+x9mi_u16(x9_mi_stream+14);
  if(need>X9_MIPC_FRAME_MAX){x9_mi_poisoned=1;break;}
  unsigned k=need-x9_mi_bytes;if(k>n)k=n;
  memcpy(x9_mi_stream+x9_mi_bytes,p,k);x9_mi_bytes+=k;p+=k;n-=k;
  if(x9_mi_bytes<16)continue;
  if(x9_mi_stream[0]!=0x84||x9_mi_stream[1]!=0x19||x9_mi_stream[2]!=0x54||x9_mi_stream[3]!=0x24){x9_mi_poisoned=1;break;}
  need=16+x9mi_u16(x9_mi_stream+14);
  if(need>X9_MIPC_FRAME_MAX){x9_mi_poisoned=1;break;}
  if(x9_mi_bytes<need)continue;
  unsigned id=x9mi_u16(x9_mi_stream+10),tx=x9mi_u16(x9_mi_stream+12);
  if(x9_mi_stream[8]&1){
   if(x9_mi_stream[9]){x9_mi_poisoned=1;break;} /* no guessed MORE assembly */
   if(id==0x4202||id==0x4302){
    if(x9_mi_count==X9_MI_EVENTS){x9_mi_poisoned=1;break;}
    unsigned at=(x9_mi_head+x9_mi_count++)%X9_MI_EVENTS;
    x9_mi_events[at].len=need;memcpy(x9_mi_events[at].frame,x9_mi_stream,need);
   }else for(unsigned i=0;i<X9_MI_SLOTS;i++){
    struct x9_mi_slot *s=&x9_mi_slots[i];
    if(s->state==2&&s->txid==tx&&id==(unsigned)s->id+1&&x9_mi_stream[8]==s->mask){
     s->len=need;memcpy(s->frame,x9_mi_stream,need);s->state=3;break;
    }
   }
  }
  bzero(x9_mi_stream,sizeof(x9_mi_stream));x9_mi_bytes=0;
 }
 if(x9_mi_poisoned)printf("X9 MIPC: framing/event overflow, transport quarantined; no bearer reuse\n");
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
