/* Included with x9_mi_lock held by RX/tick helpers. No second physical MD
 * writer and no WDS inbox sharing. Runtime contracts: libmtkmipc-ril's
 * GSM CMD8601/8603, libmipc::mipc_msg_rsp (flags=0, original id+1/txid).
 * Content stays in bounded RAM and is zeroed on release, never logged.
 * Overflow quarantines ONLY messaging; ambiguous commands are never ACKed. */
#define X9_MM_SLOTS 16u
struct x9_mm_slot {
 uint32_t token; unsigned state,len; int result;
 uint16_t id,txid; uint8_t mask;
 uint8_t frame[X9_MIPC_FRAME_MAX];
};
static struct x9_mm_slot x9_mm_slots[X9_MM_SLOTS];
static uint32_t x9_mm_next;
static unsigned x9_mm_poisoned;
static int x9mm_id(unsigned id){return id==0x8601||id==0x8603||id==0x4601||id==0x4a01;}
/* Exact padded TLVs, unique normalized tags. Enforce this again at the
 * transport boundary, not merely in the QMI adapter. */
static int x9mm_find(const uint8_t *p,unsigned n,unsigned wanted,const uint8_t **out,unsigned *len){
 unsigned at=16;*out=NULL;*len=0;
 while(at<n){
  if(n-at<4)return 0;
  unsigned tag=x9mi_u16(p+at)&0x7fff,k=x9mi_u16(p+at+2),size=k?((k+11)&~7u):16;
  if(size>n-at)return 0;
  for(unsigned prev=16;prev<at;){
   if((x9mi_u16(p+prev)&0x7fff)==tag)return 0;
   unsigned old=x9mi_u16(p+prev+2);prev+=old?((old+11)&~7u):16;
  }
  if(tag==(wanted&0x7fff)){*out=p+at+4;*len=k;}
  at+=size;
 }
 return at==n;
}
static int x9mm_one(const uint8_t *p,unsigned n,unsigned tag,unsigned value){
 const uint8_t *v;unsigned k;
 return x9mm_find(p,n,tag,&v,&k)&&v&&k==1&&v[0]==value;
}
static int x9mm_request(const uint8_t *p,unsigned n,unsigned id){
 const uint8_t *v;unsigned k;
 if(id==0x30b){
  return p[8]==0xff&&n==24&&x9mm_find(p,n,0x100,&v,&k)&&v&&k==2&&
    (x9mi_u16(v)==0x8601||x9mi_u16(v)==0x8603);
 }
 if(p[8]!=1&&(p[8]!=2||x9_md_sim_count()!=2))return 0;
 if(id==0xa04)return n==24&&x9mm_one(p,n,0x100,0);
 if(id==0x601){
  /* Real SMS host-ready settings, or read-only SMSC query. No SMS/NV writes. */
  return (n==40&&x9mm_one(p,n,0x100,0)&&x9mm_one(p,n,0x105,1)&&x9mm_one(p,n,0x106,2))||
   (n==24&&x9mm_one(p,n,0x102,1));
 }
 if(id==0x607){
  /* MD 913677c6 + 91408c60/9140702a/91407060. GSM storage:
   * flag0..4 enumerate, flag5 reads a NONZERO native index; 103=1 preserves
   * tags (CMGL/CMGR ...,1). Only indexed CMGR may use 103=0 to mark MT read.
   * Explicit SIM/ME storage, no current-store guess or whole-store mutation. */
  if(!x9mm_one(p,n,0x100,3)||
   !x9mm_find(p,n,0x104,&v,&k)||!v||k!=1||v[0]>1||
   !x9mm_find(p,n,0x101,&v,&k)||!v||k!=1||v[0]>5)return 0;
  if(v[0]<5)return n==48&&x9mm_one(p,n,0x103,1);
  return n==56&&(x9mm_one(p,n,0x103,1)||x9mm_one(p,n,0x103,0))&&
   x9mm_find(p,n,0x102,&v,&k)&&v&&k==2&&x9mi_u16(v)!=0;
 }
 if(id!=0x605||!x9mm_one(p,n,0x100,0)||!x9mm_find(p,n,0x8101,&v,&k)||!v||k<2||k>255)return 0;
 unsigned full=k,smsc=v[0];
 if(smsc>12||smsc+1>=full)return 0;
 unsigned tpdu=full-smsc-1;
 if((v[smsc+1]&3)!=1||!x9mm_one(p,n,0x102,tpdu))return 0;
 /* Only the three audited GSM send TLVs. No transport/IMS guessing. */
 return n==16+8+((full+11)&~7u)+8;
}
static void x9mm_rx(const uint8_t *p,unsigned n,unsigned id,unsigned tx){
 if(!x9mm_id(id)||x9_mm_poisoned)return;
 unsigned mask=p[8]&((1u<<x9_md_sim_count())-1);
 if(!mask)return;
 /* Multi-SIM command ACK ownership is ambiguous; never pick the first bit. */
 if((id>>14)==2&&(mask&(mask-1))){x9_mm_poisoned=1;return;}
 for(unsigned i=0;i<X9_MM_SLOTS;i++){
  struct x9_mm_slot *s=&x9_mm_slots[i];
  if(s->state&&(id>>14)==2&&s->id==id&&s->txid==tx&&s->mask==p[8]){
   /* A retransmitted outstanding MD command is not a second SMS. Changed
    * content or retransmission after reply is ambiguous: keep ownership. */
   if(s->state>2||s->len!=n||memcmp(s->frame,p,n))x9_mm_poisoned=1;
   return;
  }
 }
 for(unsigned i=0;i<X9_MM_SLOTS;i++)if(!x9_mm_slots[i].state){
  if(x9_mm_next==0xffffffffu){x9_mm_poisoned=1;return;}
  struct x9_mm_slot *s=&x9_mm_slots[i];bzero(s,sizeof(*s));
  s->token=++x9_mm_next;s->id=(uint16_t)id;s->txid=(uint16_t)tx;s->mask=p[8];
  s->len=n;memcpy(s->frame,p,n);s->state=1;return;
 }
 x9_mm_poisoned=1;printf("X9 SMS: inbox overflow; messaging quarantined, WDS unchanged\n");
}
int x9_md_mipc_message_event(uint8_t *out,uint32_t room,uint32_t *len,uint32_t *token){
 if(!out||!len||!token)return -1;
 *len=*token=0;int rc=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mm_poisoned||x9_mi_poisoned||!x9_mi_online)rc=-1;
 else{
  struct x9_mm_slot *first=NULL;
  for(unsigned i=0;i<X9_MM_SLOTS;i++)if(x9_mm_slots[i].state==1&&
   (!first||x9_mm_slots[i].token<first->token))first=&x9_mm_slots[i];
  if(first){
   if(first->len>room)rc=-1;
   else{*len=first->len;memcpy(out,first->frame,*len);rc=1;
    if((first->id>>14)==2){*token=first->token;first->state=2;}
    else bzero(first,sizeof(*first));
   }
  }
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_message_reply(uint32_t token,unsigned success,uint8_t cause){
 if(!token||success>1)return -1;
 int rc=-1;lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MM_SLOTS;i++){
  struct x9_mm_slot *s=&x9_mm_slots[i];if(!s->state||s->token!=token)continue;
  if(s->state!=2)break;
  /* Only the audited CS GSM variants. Non-CS type4/8105 needs its own adapter;
   * do not allow a caller to success-ACK it with a guessed CS response. */
  if(s->id==0x8601&&!x9mm_one(s->frame,s->len,0x100,3))break;
  if(s->id==0x8603){
   const uint8_t *v;unsigned k;
   if(!x9mm_find(s->frame,s->len,0x8100,&v,&k)||!v||k<3||k>255||v[0]>12||v[0]+2u>=k||
    (v[v[0]+1]&3)!=2)break;
  }
  if(x9_mm_poisoned||x9_mi_poisoned||!x9_mi_online){rc=0;break;}
  bzero(s->frame,sizeof(s->frame));uint8_t *p=s->frame;unsigned id=s->id+1;
  p[0]=0x84;p[1]=0x19;p[2]=0x54;p[3]=0x24;p[8]=s->mask;
  p[10]=(uint8_t)id;p[11]=(uint8_t)(id>>8);p[12]=(uint8_t)s->txid;p[13]=(uint8_t)(s->txid>>8);
  p[16]=0;p[17]=1;p[18]=1;p[20]=(uint8_t)!success;s->len=24;
  if(!success){p[24]=1;p[25]=1;p[26]=1;p[28]=cause;s->len=32;}
  p[14]=(uint8_t)(s->len-16);s->state=3;rc=1;break;
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_message_reply_take(uint32_t token,int *result){
 if(!token||!result)return -1;
 int rc=-1;lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MM_SLOTS;i++){
  struct x9_mm_slot *s=&x9_mm_slots[i];if(!s->state||s->token!=token)continue;
  rc=0;if(s->state==5){*result=s->result;rc=1;
   /* TX failure is not retirement. Deliver that result once but keep the
    * original command id/txid/mask owned; never recycle/retry its ACK. */
   if(s->result)s->state=6;else bzero(s,sizeof(*s));
  }break;
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
static uint32_t x9mm_tx(uint8_t *out,unsigned *len){
 if(x9_mm_poisoned)return 0;
 for(unsigned i=0;i<X9_MM_SLOTS;i++)if(x9_mm_slots[i].state==3){
  struct x9_mm_slot *s=&x9_mm_slots[i];s->state=4;*len=s->len;memcpy(out,s->frame,*len);return s->token;
 }
 return 0;
}
static void x9mm_tx_done(uint32_t token,int rc){
 lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MM_SLOTS;i++)if(x9_mm_slots[i].state==4&&x9_mm_slots[i].token==token){
  struct x9_mm_slot *s=&x9_mm_slots[i];s->result=rc<0?X9_AT_OFFLINE:X9_AT_OK;
  bzero(s->frame,sizeof(s->frame));s->len=0;s->state=5;break;
 }
 lck_mtx_unlock(&x9_mi_lock);
}
