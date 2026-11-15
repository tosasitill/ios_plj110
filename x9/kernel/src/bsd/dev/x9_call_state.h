/* PLJ110 outbound state provider. Full MD frames only, never logger prefixes.
 * RIL 37ca80 -> 268db0: 109 is ECPI, NOT RIL_CallState. Apple 1939f8c
 * names the corresponding CallEventStatus values. No approval/audio policy.
 * State is scoped to the observed low-bit SIM2 IND route, not CMD masks.
 * Kept freestanding so the transport and iOS owner share one implementation. */
#ifndef X9_CALL_STATE_H
#define X9_CALL_STATE_H
#include "x9_call_wire.h"

struct x9cw_progress {
 uint32_t id,direction,mode,toa,flags,aux,event,cause;
 unsigned has_cause;uint8_t mask;
};
static inline int x9cw_cstring(const uint8_t *v,unsigned k,unsigned bound){
 if(!v||!k||k>bound||v[k-1])return 0;
 for(unsigned i=0;i+1<k;i++)if(!v[i])return 0;
 return 1;
}
/* MD 913257b6: scalar 100/101/102/103/105/107/109, strings
 * 8104 <=128 and 810b <=512, opaque 8106 exactly408, optional10a/u32.
 * 8106 carries binary RIL information despite its USIR/string bit.
 * Validate the WHOLE frame before committing any ID or state. */
static inline int x9cw_progress(const uint8_t *p,unsigned n,struct x9cw_progress *out){
 struct x9cw_progress c;const uint8_t *v;unsigned k;
 if(!out||!x9cw_frame(p,n,0x4901)||!p[8]||(p[8]&(p[8]-1)))return 0;
 if(!x9cw_word(p,n,0x100,&c.id)||!c.id||
    !x9cw_word(p,n,0x101,&c.direction)||c.direction>255||
    !x9cw_word(p,n,0x102,&c.mode)||c.mode>255||
    !x9cw_word(p,n,0x103,&c.toa)||c.toa>255||
    !x9cw_word(p,n,0x105,&c.flags)||!x9cw_word(p,n,0x107,&c.aux)||
    !x9cw_word(p,n,0x109,&c.event)||c.event>255||
    !x9cw_tlv(p,n,0x8104,&v,&k)||!x9cw_cstring(v,k,128)||
    !x9cw_tlv(p,n,0x8106,&v,&k)||!v||k!=408||
    !x9cw_tlv(p,n,0x810b,&v,&k)||!x9cw_cstring(v,k,512))return 0;
 c.has_cause=0;c.cause=0;c.mask=p[8];
 if(!x9cw_tlv(p,n,0x10a,&v,&k))return 0;
 if(v){if(k!=4)return 0;c.has_cause=1;c.cause=x9cw_u32(v);if(c.cause>65535)return 0;}
 for(unsigned at=16;at<n;){
  unsigned tag=x9cw_u16(p+at);
  if(tag!=0x100&&tag!=0x101&&tag!=0x102&&tag!=0x103&&tag!=0x8104&&
     tag!=0x105&&tag!=0x8106&&tag!=0x107&&tag!=0x109&&tag!=0x10a&&tag!=0x810b)return 0;
  at+=(x9cw_u16(p+at+2)+11)&~7u;
 }
 *out=c;return 1;
}

/* Unknown idle != known empty. A status stream cannot prove there are no
 * other calls. count_known comes ONLY from a complete GET_STATUS snapshot.
 * An ended identity is retained until a NEW130, never revived by late6.
 * No u32->u8 truncation, no artificial ID allocator or CNF-derived ID. */
struct x9cw_outbound_state {
 uint64_t epoch,revision;
 uint32_t native_id,event,flags,mode,cause;
 unsigned ios_state,has_cause,seen,ended,blocked,count_known,native_count;
};
static inline void x9cw_outbound_invalidate(struct x9cw_outbound_state *s){
 s->count_known=0;
 if(s->revision==UINT64_MAX)s->blocked=1;else s->revision++;
}
static inline int x9cw_outbound_apply(struct x9cw_outbound_state *s,
                                     const struct x9cw_progress *c){
 if(!s||!c||!c->id||s->blocked)return -1;
 if(c->mask!=2)return 0; /* live AT capture proves IND2, not CMD20 */
 if(c->event==130){
  if(c->direction||c->mode!=0){s->blocked=1;return -1;}
  if(s->seen&&!s->ended){
   if(s->native_id==c->id&&s->event==130)return 0;
   s->blocked=1;return -1;
  }
  if(s->epoch==UINT64_MAX||s->revision==UINT64_MAX){s->blocked=1;return -1;}
  s->epoch++;s->native_id=c->id;s->seen=1;s->ended=0;s->ios_state=1;
 }else{
  /* A foreign/unsupported event invalidates completeness but cannot adopt
   * an arbitrary ID as our MO, or complete a pending dial with it. */
  if(!s->seen||s->native_id!=c->id){s->count_known=0;s->blocked=1;return -1;}
  if(c->direction||c->mode!=20){s->blocked=1;return -1;}
  if(s->ended){
   if(c->event==2||c->event==132||c->event==6||c->event==133)return 0;
   s->count_known=0;s->blocked=1;return -1;
  }
  if(c->event==2){if(s->ios_state!=1&&s->ios_state!=5){s->blocked=1;return -1;}s->ios_state=5;}
  else if(c->event==132||c->event==6)s->ios_state=3;
  else if(c->event==133){s->ios_state=9;s->ended=1;}
  else{s->blocked=1;return -1;}
 }
 if(s->revision==UINT64_MAX){s->blocked=1;return -1;}
 s->revision++;s->event=c->event;s->flags=c->flags;s->mode=c->mode;
 s->has_cause=c->has_cause;s->cause=c->cause;
 /* Even END133 does not prove global emptiness. Query again for that. */
 s->count_known=0;return 1;
}
static inline int x9cw_outbound_id(const struct x9cw_outbound_state *s,unsigned *id){
 if(!s||!id||s->blocked||!s->seen||s->ended||!s->native_id||s->native_id>255)return 0;
 *id=(unsigned)s->native_id;return 1;
}

/* Incoming identity is provided by the complete CMD, not by ANSWER_CNF.
 * Actual SIM2 capture: CMD22/id1/seq100/mode20 -> ECPI0/direction1 ->
 * 132/6 ->133. RIL2686ec..268784 adopts the CMD's real id for IMS;
 * 269910..269918 sets ACTIVE3, 2695e0..2695f8 sets END8/removes.
 * Apple1939f8c names INCOMING2/CONVERSATION3/END9. CMD alone is a
 * pending identity, NOT an iOS incoming state, an approval, or an answer.
 * Keep this independent of the MO join so an MT can never complete DIAL.
 * No global call count or permission to target empty ANSWER is inferred. */
struct x9cw_inbound_state {
 uint64_t epoch,revision;
 uint32_t native_id,sequence,event,flags,mode,cause;
 unsigned pending,seen,ended,blocked,ios_state,has_cause;
 uint16_t txid;uint8_t mask,response_mask,sim;
};
static inline int x9cw_inbound_begin(struct x9cw_inbound_state *s,
                                    const struct x9cw_incoming *c){
 if(!s||!c||s->blocked||!c->call_id)return -1;
 /* Only the observed ordinary SIM2 IMS mode20 subset; route codec can
  * decode SIM1 without asserting its state/voice profile here. */
 if(c->sim!=1)return 0;
 if(c->mode!=20||c->evoltesi_flow||s->pending||(s->seen&&!s->ended)||
    (s->seen&&s->sequence==c->sequence)||s->revision==UINT64_MAX){s->blocked=1;return -1;}
 s->native_id=c->call_id;s->sequence=c->sequence;s->txid=c->txid;
 s->mask=c->mask;s->response_mask=c->response_mask;s->sim=c->sim;
 s->pending=1;s->seen=s->ended=s->ios_state=s->has_cause=0;
 s->event=s->flags=s->cause=0;s->mode=c->mode;s->revision++;return 1;
}
static inline int x9cw_inbound_apply(struct x9cw_inbound_state *s,
                                    const struct x9cw_progress *c){
 if(!s||!c||s->blocked||!c->id)return -1;
 if(c->mask!=2||c->direction!=1)return 0;
 if(c->mode!=20||s->native_id!=c->id||(!s->pending&&!s->seen)){s->blocked=1;return -1;}
 if(s->ended){
  /* Late progress never creates a new epoch or resurrects an ended id. */
  if(c->event==0||c->event==132||c->event==6||c->event==133)return 0;
  s->blocked=1;return -1;
 }
 if(c->event==0){
  if(s->seen){
   if(s->ios_state==2)return 0;
   s->blocked=1;return -1; /* do not regress ACTIVE to INCOMING */
  }
  if(!s->pending||s->epoch==UINT64_MAX){s->blocked=1;return -1;}
  s->epoch++;s->pending=0;s->seen=1;s->ios_state=2;
 }else if(c->event==133){
  if(!s->seen){s->blocked=1;return -1;} /* unobserved pre-ECPI0 cancellation */
  s->pending=0;s->seen=1;s->ended=1;s->ios_state=9;
 }else if(c->event==132||c->event==6){
  if(!s->seen){s->blocked=1;return -1;}
  s->ios_state=3;
 }else{s->blocked=1;return -1;}
 if(s->revision==UINT64_MAX){s->blocked=1;return -1;}
 s->revision++;s->event=c->event;s->flags=c->flags;
 s->has_cause=c->has_cause;s->cause=c->cause;return 1;
}
static inline int x9cw_inbound_id(const struct x9cw_inbound_state *s,unsigned *id){
 if(!s||!id||s->blocked||s->pending||!s->seen||s->ended||!s->native_id||s->native_id>255)return 0;
 *id=(unsigned)s->native_id;return 1;
}

/* MIPC arrays are descriptors + individually tagged elements, NOT count*u32.
 * libmipc add_array12310 / add_idx12420; MD9141b68a/9141b77c.
 * Descriptor: len0, reserved[8]=0, base/u16, count/u16. */
struct x9cw_array {unsigned tag,base,count;};
static inline int x9cw_array(const uint8_t *p,unsigned n,unsigned tag,unsigned count,
                            struct x9cw_array *out){
 const uint8_t *v;unsigned k;
 if(!out||!x9cw_tlv(p,n,tag,&v,&k))return 0;
 if(!count)return !v; /* add_array(size0) produces no descriptor */
 if(!v||k||x9cw_u32(v)||x9cw_u32(v+4)||x9cw_u16(v+10)!=count)return 0;
 unsigned base=x9cw_u16(v+8),low=base&0x7fff;
 if((base&0x8000)!=(tag&0x8000)||!low||count>0x8000-low)return 0;
 out->tag=tag;out->base=base;out->count=count;return 1;
}
struct x9cw_current_call {
 uint32_t id,direction,mode,state,multiparty,toa,priority,flags,aux;
};
#define X9_CW_CURRENT_MAX 7u
struct x9cw_current {uint32_t result,count;struct x9cw_current_call calls[X9_CW_CURRENT_MAX];};
/* MD9131d574..d86c, RIL385180. MD includes IMS-mode records; the RIL's
 * later filtering of modes20..39 is a CS consumer policy, NOT missing data.
 * Result failure does not contain a usable snapshot or invent count0. */
static inline int x9cw_current(const uint8_t *p,unsigned n,struct x9cw_current *out){
 struct x9cw_current c;struct x9cw_array a[11];const uint8_t *v;unsigned k;
 const unsigned tags[11]={0x10b,0x10c,0x10d,0x10e,0x10f,0x110,0x8111,0x112,0x113,0x8114,0x115};
 x9cw_zero((uint8_t *)&c,sizeof(c));
 if(!out||!x9cw_frame(p,n,0x90a)||!x9cw_word(p,n,0,&c.result))return 0;
 /* The MD producer unconditionally writes result AND count, also on error.
  * Validate its arrays but never use a failed result as an idle snapshot. */
 if(!x9cw_word(p,n,0x100,&c.count)||c.count>X9_CW_CURRENT_MAX)return 0;
 for(unsigned j=0;j<11;j++)if(!x9cw_array(p,n,tags[j],c.count,&a[j]))return 0;
 for(unsigned i=0;i<c.count;i++){
  uint32_t words[11]={0};
  for(unsigned j=0;j<11;j++){
   if(!x9cw_tlv(p,n,a[j].base+i,&v,&k)||!v)return 0;
   if(j==6){if(!x9cw_cstring(v,k,128))return 0;}
   else if(j==9){if(k!=408)return 0;}
   else{if(k!=4)return 0;words[j]=x9cw_u32(v);}
  }
  struct x9cw_current_call *r=&c.calls[i];
  r->id=words[0];r->direction=words[1];r->mode=words[2];r->state=words[3];
  r->multiparty=words[4];r->toa=words[5];r->priority=words[7];r->flags=words[8];r->aux=words[10];
  if(!r->id||r->direction>255||r->mode>255||r->state>5||r->multiparty>255||r->toa>255||r->priority>255)return 0;
  for(unsigned prev=0;prev<i;prev++)if(c.calls[prev].id==r->id)return 0;
 }
 /* Every tag is a scalar, known descriptor or a referenced element.
  * Reject aliases/overlapping ranges, orphan elements and extra fields. */
 for(unsigned at=16;at<n;){
  unsigned tag=x9cw_u16(p+at),hits=tag==0||tag==0x100;
  for(unsigned j=0;j<11;j++){
   if(tag==tags[j])hits++;
   if(c.count&&tag>=a[j].base&&tag-a[j].base<c.count)hits++;
  }
  if(hits!=1)return 0;
  unsigned z=x9cw_u16(p+at+2);at+=z?((z+11)&~7u):16;
 }
 *out=c;return 1;
}
/* A hangup targets a real observed MO epoch AND a complete owned current
 * snapshot. RIL386180 accepts current state0..5: ACTIVE0/ALERTING3
 * match our observed ordinary SIM2 mode20 call; initial ECPI130 is mode0
 * and is deliberately not admitted as a mode20 end target.
 * are admitted. No wildcard, inferred single count, default id or MT here.
 * The snapshot revision is copied again and checked at writer selection. */
struct x9cw_end_target {uint64_t epoch,revision;uint32_t native_id;unsigned sim;};
static inline int x9cw_end_target(const struct x9cw_outbound_state *s,
                                  const struct x9cw_current *c,
                                  struct x9cw_end_target *out){
 unsigned id;
 if(!out||!s||!c||!x9cw_outbound_id(s,&id)||!s->epoch||
    !s->count_known||s->native_count!=1||c->result||c->count!=1||s->mode!=20)return 0;
 const struct x9cw_current_call *r=&c->calls[0];
 if(r->id!=id||r->direction||r->mode!=20||r->multiparty||
    !((s->ios_state==5&&r->state==3)||
      (s->ios_state==3&&r->state==0)))return 0;
 out->epoch=s->epoch;out->revision=s->revision;out->native_id=id;out->sim=1;return 1;
}
static inline int x9cw_end_matches(const struct x9cw_end_target *a,
                                   const struct x9cw_end_target *b){
 return a&&b&&a->epoch==b->epoch&&a->revision==b->revision&&
        a->native_id==b->native_id&&a->sim==b->sim;
}
/* Join a single explicitly admitted DIAL with BOTH its owned0902 and the
 * next observed MO epoch. Capture's0902 has no ID. Caller retains its real
 * transport token independently through timeout/client loss/late CNF.
 * No fallback to ID1, no casting an out-of-range native ID, no retry. */
struct x9cw_dial_join {uint64_t epoch_floor,token;unsigned started,confirmed,failed;};
static inline int x9cw_dial_join_begin(struct x9cw_dial_join *j,
                                      const struct x9cw_outbound_state *s,
                                      const struct x9cw_dial_profile *profile){
 if(!j||!s||j->started||!x9cw_dial_profile_valid(profile)||s->blocked||
    !s->count_known||s->native_count||(s->seen&&!s->ended)||s->epoch==UINT64_MAX)return 0;
 j->epoch_floor=s->epoch;j->token=0;j->started=1;j->confirmed=j->failed=0;return 1;
}
/* Existing MD owner allocates token=txid in1..65534; bind only its accepted
 * token, not a transaction supplied by iOS or parsed from an indication. */
static inline int x9cw_dial_join_sent(struct x9cw_dial_join *j,uint64_t token){
 if(!j||!j->started||j->token||!token||token>65534)return 0;
 j->token=token;return 1;
}
static inline int x9cw_dial_join_cnf(struct x9cw_dial_join *j,const uint8_t *p,unsigned n){
 uint32_t result;
 if(!j||!j->started||!j->token||j->confirmed||j->failed||!p||n<16||
    x9cw_u16(p+10)!=0x902||x9cw_u16(p+12)!=j->token)return 0;
 if(!x9cw_dial_cnf(p,n,&result)){j->failed=1;return 0;}
 j->confirmed=1;j->failed=result!=0;return 1;
}
static inline int x9cw_dial_join_id(const struct x9cw_dial_join *j,
                                   const struct x9cw_outbound_state *s,unsigned *id){
 if(!j||!s||!id||!j->started||j->failed||s->blocked)return -1;
 if(!j->confirmed||s->epoch==j->epoch_floor)return 0;
 if(s->epoch!=j->epoch_floor+1||!x9cw_outbound_id(s,id))return -1;
 return 1;
}
/* Apple handleNormalVoiceDialResp1f1ac2a34 reads CallIdentifier tag10/u8.
 * Retain original QMI txn/message; successful result + ID only after join.
 * This writes a RESPONSE, not the seven-byte CallList indication (other
 * fields of that list still require mode/direction/audio reconciliation). */
static inline unsigned x9cw_dial_join_response(const struct x9cw_dial_join *j,
                                              const struct x9cw_outbound_state *s,
                                              const uint8_t *req,unsigned len,
                                              uint8_t *out,unsigned room){
 struct x9cw_dial_number number;unsigned id;
 if(!out||room<18||!x9cw_qmi_dial(req,len,&number)||x9cw_dial_join_id(j,s,&id)!=1)return 0;
 uint8_t header[4];for(unsigned i=0;i<4;i++)header[i]=req[i+1];
 x9cw_zero(out,18);out[0]=2;for(unsigned i=0;i<4;i++)out[i+1]=header[i];
 x9cw_put16(out+5,11);out[7]=2;x9cw_put16(out+8,4);
 out[14]=0x10;x9cw_put16(out+15,1);out[17]=(uint8_t)id;return 18;
}
#endif
