/* Independent call inbox / command owner. RX/tick helpers hold x9_mi_lock.
 * No registrations, call actions or approval policy are started here.
 * Raw CMD masks are preserved for ownership; RSP uses the proved RIL slot
 * conversion, not the raw CMD mask. No route is selected from high bits.
 * Successful and failed CMD identities are retained until boot: retirement
 * across call-end / sequence reuse is not yet proven in Android's RIL.
 * Overflow quarantines ONLY calls. No payload/number/sequence diagnostics. */
#include "x9_call_state.h"
#include "x9_call_handover.h"
#define X9_MC_SLOTS 8u
struct x9_mc_slot {
 uint32_t token;unsigned state,len;int result;
 uint16_t id,txid;uint8_t mask;
 uint8_t frame[X9_MIPC_FRAME_MAX];
};
static struct x9_mc_slot x9_mc_slots[X9_MC_SLOTS];
static uint32_t x9_mc_next;
static unsigned x9_mc_poisoned;
static struct x9cw_outbound_state x9_mc_outbound;
static struct x9cw_inbound_state x9_mc_inbound;
static struct x9cw_current x9_mc_current;
/* A guarded DIAL stays leased across CNF/client loss. Only real END + an
 * owned empty current snapshot with no remaining mutation can retire it. */
static uint64_t x9_mc_dial_token;
/* One end attempt for an epoch; a missing/failed CNF never releases it.
 * Retirement requires real END + a fresh owned empty snapshot and all
 * CALL mutations drained, not a timer or hangup result0. direction records
 * which reducer's epoch the leased end belongs to (0 MO, 1 MT). */
static uint64_t x9_mc_end_token;
/* One answer attempt per MT epoch, same conservative lease: a missing or
 * failed0908 never releases it; only real MT END + owned empty retires it.
 * END stays admissible while this lease is held, mirroring the dial lease. */
static uint64_t x9_mc_answer_token;
static uint64_t x9_mc_dial_floor,x9_mc_end_epoch,x9_mc_answer_epoch;
static unsigned x9_mc_end_direction;
static int x9mi_call_busy(void);
/* This callback receives only an OWNED GET_STATUS confirmation, never an
 * unsolicited090a or a logger-derived reconstruction. No state/ID adoption
 * from CS RIL states: the MO epoch still comes from full ECPI130. */
static void x9mc_current(const uint8_t *p,unsigned n,uint64_t revision){
 if(revision!=x9_mc_outbound.revision)return; /* in-flight status crossed an event/mutation */
 struct x9cw_current c;
 if(!x9cw_current(p,n,&c)){x9_mc_outbound.blocked=1;return;}
 x9cw_outbound_invalidate(&x9_mc_outbound);
 if(c.result)return;
 x9_mc_outbound.count_known=1;x9_mc_outbound.native_count=c.count;
 x9_mc_current=c;
 /* A complete snapshot conflicting with an active observed call is not a
  * licence to erase it or reuse an ID. Preserve the identity and fail closed. */
 if(x9_mc_outbound.seen&&!x9_mc_outbound.ended){
  unsigned found=0;for(unsigned i=0;i<c.count;i++)if(c.calls[i].id==x9_mc_outbound.native_id)found++;
  if(!found)x9_mc_outbound.blocked=1;
 }
 if(!x9_mc_outbound.blocked&&!c.count&&x9_mc_outbound.seen&&x9_mc_outbound.ended&&
    !x9mi_call_busy()){
  if(x9_mc_dial_token&&x9_mc_dial_floor!=UINT64_MAX&&x9_mc_outbound.epoch==x9_mc_dial_floor+1)
   x9_mc_dial_token=0;
  if(x9_mc_end_token&&!x9_mc_end_direction&&x9_mc_outbound.epoch==x9_mc_end_epoch)x9_mc_end_token=0;
 }
 /* MT retirement is independent of the MO reducer: after8901 the outbound
  * is permanently blocked, so its seen/ended can never prove MT emptiness.
  * Only the CMD-owned inbound END plus the same fresh owned empty snapshot. */
 if(!c.count&&x9_mc_inbound.seen&&x9_mc_inbound.ended&&!x9_mc_inbound.blocked&&
    !x9mi_call_busy()){
  if(x9_mc_end_token&&x9_mc_end_direction==1&&x9_mc_inbound.epoch==x9_mc_end_epoch)x9_mc_end_token=0;
  if(x9_mc_answer_token&&x9_mc_inbound.epoch==x9_mc_answer_epoch)x9_mc_answer_token=0;
 }
}
int x9_md_mipc_call_end_snapshot(struct x9cw_end_target *out){
 if(!out)return -1;
 bzero(out,sizeof(*out));int rc=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mc_poisoned||x9_mi_poisoned||!x9_mi_online)rc=-1;
 else if(!x9_mc_end_token){
  if(x9cw_end_target(&x9_mc_outbound,&x9_mc_current,out))rc=1;
  else if(x9cw_end_target_inbound(&x9_mc_inbound,&x9_mc_current,out))rc=1;
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_answer_snapshot(struct x9cw_end_target *out){
 if(!out)return -1;
 bzero(out,sizeof(*out));int rc=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mc_poisoned||x9_mi_poisoned||!x9_mi_online)rc=-1;
 else if(!x9_mc_answer_token&&!x9_mc_end_token&&
         x9cw_answer_target(&x9_mc_inbound,&x9_mc_current,out))rc=1;
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_snapshot(struct x9cw_outbound_state *out){
 if(!out)return -1;
 bzero(out,sizeof(*out));int rc=-1;lck_mtx_lock(&x9_mi_lock);
 if(!x9_mc_poisoned&&!x9_mi_poisoned&&x9_mi_online){*out=x9_mc_outbound;rc=1;}
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_incoming_snapshot(struct x9cw_inbound_state *out){
 if(!out)return -1;
 bzero(out,sizeof(*out));int rc=-1;lck_mtx_lock(&x9_mi_lock);
 if(!x9_mc_poisoned&&!x9_mi_poisoned&&x9_mi_online){*out=x9_mc_inbound;rc=1;}
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
static int x9mc_id(unsigned id){return id==0x8901||id==0x4901||id==0x4902||id==0x4904;}
static void x9mc_rx(const uint8_t *p,unsigned n,unsigned id,unsigned tx){
 if(!x9mc_id(id)||x9_mc_poisoned)return;
 if(!x9cw_frame(p,n,id)){x9_mc_poisoned=1;return;}
 if(id==0x4901){
  struct x9cw_progress c;
  if(!x9cw_progress(p,n,&c)){x9_mc_outbound.blocked=1;x9_mc_poisoned=1;return;}
  if(c.mask==2&&c.direction==1){
   /* MT is not the next MO epoch; it must never complete an owned DIAL.
    * Its real identity must already exist from the complete incoming CMD. */
   x9_mc_outbound.count_known=0;x9_mc_outbound.blocked=1;
   (void)x9cw_inbound_apply(&x9_mc_inbound,&c);
  }else (void)x9cw_outbound_apply(&x9_mc_outbound,&c);
 }
 if(id==0x4902){
  struct x9cw_handover c;int rc=x9cw_handover(p,n,&c);
  if(rc){
   /* A handover crosses the single IMS-only mutation policy. Invalidate
    * at RX, BEFORE a queued DIAL/END can be writer-selected. Forwarding
    * its real event is not permission to reuse pre-handover call IDs. */
   unsigned low=p[8]&15;
   if(low==2||rc<0){
    x9cw_outbound_invalidate(&x9_mc_outbound);x9_mc_outbound.blocked=1;
    x9_mc_inbound.blocked=1;
   }
   if(rc<0){x9_mc_poisoned=1;return;}
  }
 }
 if(id==0x8901){
  /* Outside this MO-only policy. Never preserve a usable empty baseline
   * across an incoming approval command. Parsing the proved route is NOT
   * permission to approve, answer or publish this call. */
  x9_mc_outbound.count_known=0;x9_mc_outbound.blocked=1;
  struct x9cw_incoming c;
  if(!x9cw_incoming(p,n,&c)){x9_mc_poisoned=1;return;}
  for(unsigned i=0;i<X9_MC_SLOTS;i++){
   struct x9_mc_slot *s=&x9_mc_slots[i];
   if(s->state&&s->id==id&&s->txid==tx&&s->mask==p[8]){
    if(s->state>2||s->len!=n||memcmp(s->frame,p,n))x9_mc_poisoned=1;
    return; /* same outstanding CMD is not a second incoming call */
   }
  }
  /* Capture the independent native MT lifecycle only after duplicate
   * ownership checks. No approval or telephony publication occurs here. */
  (void)x9cw_inbound_begin(&x9_mc_inbound,&c);
 }
 for(unsigned i=0;i<X9_MC_SLOTS;i++)if(!x9_mc_slots[i].state){
  if(x9_mc_next==0xffffffffu){x9_mc_poisoned=1;return;}
  struct x9_mc_slot *s=&x9_mc_slots[i];bzero(s,sizeof(*s));
  s->token=++x9_mc_next;s->id=(uint16_t)id;s->txid=(uint16_t)tx;s->mask=p[8];
  s->len=n;memcpy(s->frame,p,n);s->state=1;return;
 }
 x9_mc_poisoned=1;printf("X9 CALL: inbox bounded; calls quarantined, SMS/WDS unchanged\n");
}
/* Subscription fences use the RX sequence, not the MD transaction or an
 * observer's latest-state snapshot. A reused QRTR slot cannot see events
 * that were already queued when it requested enable. */
int x9_md_mipc_call_cursor(uint32_t *sequence){
 if(!sequence)return -1;
 *sequence=0;int rc=-1;lck_mtx_lock(&x9_mi_lock);
 if(!x9_mc_poisoned&&!x9_mi_poisoned&&x9_mi_online){*sequence=x9_mc_next;rc=1;}
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_publish(uint32_t floor,x9_md_mipc_call_publish_cb callback,void *context){
 if(!callback)return -1;
 int rc=-1;lck_mtx_lock(&x9_mi_lock);
 /* A getter followed by an unlocked queue write can grant a subscription
  * after changed-CMD poison. Cover the actual publication with this lock. */
 if(!x9_mc_poisoned&&!x9_mi_poisoned&&x9_mi_online&&floor<=x9_mc_next)
  rc=callback(context)?1:0;
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_publish_state(uint32_t floor,x9_md_mipc_call_publish_state_cb callback,void *context){
 if(!callback)return -1;
 int rc=-1;lck_mtx_lock(&x9_mi_lock);
 if(!x9_mc_poisoned&&!x9_mi_poisoned&&x9_mi_online&&floor<=x9_mc_next)
  rc=callback(context,&x9_mc_outbound,&x9_mc_inbound)?1:0;
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_event_ordered(uint8_t *out,uint32_t room,uint32_t *len,uint32_t *token,uint32_t *sequence){
 if(!out||!len||!token||!sequence)return -1;
 *len=*token=*sequence=0;int rc=0;lck_mtx_lock(&x9_mi_lock);
 if(x9_mc_poisoned||x9_mi_poisoned||!x9_mi_online)rc=-1;
 else{
  struct x9_mc_slot *first=NULL;
  for(unsigned i=0;i<X9_MC_SLOTS;i++)if(x9_mc_slots[i].state==1&&
     (!first||x9_mc_slots[i].token<first->token))first=&x9_mc_slots[i];
  if(first){
   if(first->len>room)rc=-1;
   else{
    *len=first->len;*sequence=first->token;memcpy(out,first->frame,*len);rc=1;
    if(first->id==0x8901){*token=first->token;first->state=2;}
    else bzero(first,sizeof(*first));
   }
  }
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_event(uint8_t *out,uint32_t room,uint32_t *len,uint32_t *token){
 uint32_t sequence;return x9_md_mipc_call_event_ordered(out,room,len,token,&sequence);
}
int x9_md_mipc_call_reply(uint32_t token,unsigned approve,unsigned include_cause,uint32_t cause){
 if(!token||approve>1||include_cause>1)return -1;
 int rc=-1;lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MC_SLOTS;i++){
  struct x9_mc_slot *s=&x9_mc_slots[i];if(!s->state||s->token!=token)continue;
  if(s->state!=2||s->id!=0x8901)break;
  if(x9_mc_poisoned||x9_mi_poisoned||!x9_mi_online){rc=0;break;}
  uint8_t response[48];unsigned n=x9cw_approve(s->frame,s->len,response,sizeof(response),approve,include_cause,cause);
  if(!n)break;
  bzero(s->frame,sizeof(s->frame));memcpy(s->frame,response,n);bzero(response,sizeof(response));
  s->len=n;s->state=3;rc=1;break;
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
int x9_md_mipc_call_reply_take(uint32_t token,int *result){
 if(!token||!result)return -1;
 int rc=-1;lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MC_SLOTS;i++){
  struct x9_mc_slot *s=&x9_mc_slots[i];if(!s->state||s->token!=token)continue;
  rc=0;if(s->state==5){*result=s->result;s->state=6;rc=1;}
  break; /* identity tombstone remains even for physical TX success */
 }
 lck_mtx_unlock(&x9_mi_lock);return rc;
}
static uint32_t x9mc_tx(uint8_t *out,unsigned *len){
 if(x9_mc_poisoned)return 0;
 struct x9_mc_slot *first=NULL;
 for(unsigned i=0;i<X9_MC_SLOTS;i++)if(x9_mc_slots[i].state==3&&
    (!first||x9_mc_slots[i].token<first->token))first=&x9_mc_slots[i];
 if(!first)return 0;
 first->state=4;*len=first->len;memcpy(out,first->frame,*len);return first->token;
}
static void x9mc_tx_done(uint32_t token,int rc){
 lck_mtx_lock(&x9_mi_lock);
 for(unsigned i=0;i<X9_MC_SLOTS;i++)if(x9_mc_slots[i].state==4&&x9_mc_slots[i].token==token){
  struct x9_mc_slot *s=&x9_mc_slots[i];s->result=rc<0?X9_AT_OFFLINE:X9_AT_OK;
  bzero(s->frame,sizeof(s->frame));s->len=0;s->state=5;break;
 }
 lck_mtx_unlock(&x9_mi_lock);
}
