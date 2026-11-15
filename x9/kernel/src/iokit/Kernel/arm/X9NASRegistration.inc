/* NAS03 indication registration and NAS45 host subscription binding are
 * local operations, not network registration/readiness. Retain the original
 * transaction/result until an actual host queue receipt; retry no MD command.
 * NAS45 has the original Apple 25s SendProxy guard; NAS03 has no invented timer. */
constexpr unsigned X9R_PENDING=X9Q_JOBS;
struct x9r_reply {
 bool used,replay,bind;unsigned slot,sim,route,len;uint8_t mask,values;uint16_t error;
 uint64_t generation,bridge_generation,order,deadline;uint8_t req[X9Q_REQ];
};
static x9r_reply x9r_pending[X9R_PENDING],x9r_completed[CBQ_MAX_CLIENTS];
static uint64_t x9r_order;
static bool x9r_local_live(const x9r_reply &w){
 return w.slot<CBQ_MAX_CLIENTS&&x9q_clients[w.slot].generation==w.generation&&
  x9q_clients[w.slot].sim_slot==w.sim;
}
static bool x9r_live(const x9r_reply &w){
 return x9r_local_live(w)&&w.bridge_generation&&
  bb_x9_nas_registration_identity(CBQ_PORT_BASE+w.slot,X9BB_NODE)==w.bridge_generation;
}
static void x9r_del(unsigned slot){
 for(x9r_reply &w:x9r_pending)if(w.used&&w.slot==slot)bzero(&w,sizeof(w));
 if(slot<CBQ_MAX_CLIENTS)bzero(&x9r_completed[slot],sizeof(x9r_completed[slot]));
}
static bool x9r_binding_busy(unsigned slot){
 for(const x9r_reply &w:x9r_pending)if(w.used&&w.slot==slot&&x9r_local_live(w))return true;
 return false;
}
static bool x9r_route_pending(unsigned slot){
 for(const x9r_reply &w:x9r_pending)if(w.used&&w.bind&&!w.error&&w.slot==slot&&
  w.route!=w.sim&&x9r_local_live(w))return true;
 return false;
}
static void x9r_rebound(unsigned slot){
 if(slot<CBQ_MAX_CLIENTS)bzero(&x9r_completed[slot],sizeof(x9r_completed[slot]));
}
static bool x9r_indication_allowed(uint32_t local,unsigned sim,uint64_t generation,uint16_t id){
 const unsigned slot=local-CBQ_PORT_BASE;
 if(local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS)return false;
 const x9q_client &c=x9q_clients[slot];
 return c.generation==generation&&c.sim_slot==sim&&
  (id==0x24?c.serving:id==0x4d?c.system:id==0x4f?c.signal:false);
}
static bool x9r_repeated(uint32_t local,uint32_t node,uint32_t port,const uint8_t *req,unsigned len){
 const unsigned slot=local-CBQ_PORT_BASE;
 if(node!=X9BB_NODE||port!=X9BB_PORT_BASE+3||local<CBQ_PORT_BASE||slot>=CBQ_MAX_CLIENTS||
  !req||len<7||req[0]!=0||(x9q_u16(req+3)!=3&&x9q_u16(req+3)!=0x45))return false;
 const uint64_t bridge=bb_x9_nas_registration_identity(local,node);
 for(x9r_reply &w:x9r_pending)if(w.used&&w.slot==slot&&x9r_local_live(w)&&
  bridge&&w.bridge_generation==bridge&&x9q_u16(w.req+1)==x9q_u16(req+1)){
  if(w.len!=len||memcmp(w.req,req,len))
   printf("X9 NAS: SIM%u msg %04x txn %u changed reuse rejected; original owner/result retained\n",w.sim+1,x9q_u16(w.req+3),x9q_u16(req+1));
  return true; // same-size comparison is bounded by the original validated request
 }
 x9r_reply &w=x9r_completed[slot];
 if(!w.used||!x9r_local_live(w)||!bridge||w.bridge_generation!=bridge||
  x9q_u16(w.req+1)!=x9q_u16(req+1))return false;
 if(w.len!=len||memcmp(w.req,req,len))
  printf("X9 NAS: SIM%u msg %04x txn %u changed completed reuse rejected; first result retained\n",w.sim+1,x9q_u16(w.req+3),x9q_u16(req+1));
 else w.replay=true; // reply only: never rebind, reapply flags or restart samples
 return true;
}
/* Caller has passed the generic framing/unique-TLV validator. Unknown tags
 * retain the existing accepted-but-unserved semantics, with no invented IND. */
static uint16_t x9r_begin(unsigned slot,const uint8_t *req,unsigned len){
 uint8_t mask=0,values=0;char unserved[96];unsigned used=0;unserved[0]=0;
 for(unsigned i=7;i<len;i+=3+x9q_u16(req+i+1)){
  const unsigned k=x9q_u16(req+i+1);const uint8_t tag=req[i];
  if(tag==0x13||tag==0x18||tag==0x19){
   if(k!=1||req[i+3]>1)return X9Q_ARG;
   const uint8_t bit=tag==0x13?1:tag==0x18?2:4;mask|=bit;if(req[i+3])values|=bit;
  }else if(used+8<sizeof(unserved))used+=(unsigned)snprintf(unserved+used,sizeof(unserved)-used," %02x",tag);
 }
 // A pending route change must not invalidate a newly accepted NAS03 owner.
 if(x9r_route_pending(slot))return X9Q_BUSY;
 const uint64_t bridge=bb_x9_nas_registration_identity(CBQ_PORT_BASE+slot,X9BB_NODE);
 if(!bridge)return X9Q_NOT_READY;
 if(x9r_order==UINT64_MAX)return X9Q_BUSY; // never wrap FIFO order or retire the cache
 for(x9r_reply &w:x9r_pending)if(!w.used){
  bzero(&w,sizeof(w));w.used=true;w.slot=slot;w.sim=x9q_clients[slot].sim_slot;
  w.generation=x9q_clients[slot].generation;w.bridge_generation=bridge;
  w.len=len;w.mask=mask;w.values=values;w.order=++x9r_order;memcpy(w.req,req,len);
  // Only successful new admission may retire a completed transaction cache.
  // Cached replies never occupy the bounded pending pool.
  x9r_rebound(slot);
  static unsigned logged;if(used&&logged<8){logged++;
   printf("X9 NAS: registration03 txn %u admitted; never indicated:%s (subscription awaits host receipt)\n",x9q_u16(req+1),unserved);}
  return 0;
 }
 // Existing bounded admission boundary: BUSY is not a guaranteed queue receipt.
 return X9Q_BUSY;
}
/* Caller validated the exact NAS45 tag01/u8 wire and checked existing NAS03
 * and NetworkName owners before a route change. Admission preserves the old
 * route; no query/token is cancelled or retargeted while receipt is pending. */
static uint16_t x9r_bind(unsigned slot,const uint8_t *req,unsigned len,unsigned route){
 if(x9r_route_pending(slot))return X9Q_BUSY;
 const uint64_t bridge=bb_x9_nas_registration_identity(CBQ_PORT_BASE+slot,X9BB_NODE);
 if(!bridge)return X9Q_NOT_READY;
 const uint64_t now=x9q_ms();
 if(x9r_order==UINT64_MAX||now>UINT64_MAX-25000)return X9Q_BUSY;
 for(x9r_reply &w:x9r_pending)if(!w.used){
  bzero(&w,sizeof(w));w.used=w.bind=true;w.slot=slot;w.sim=x9q_clients[slot].sim_slot;w.route=route;
  w.generation=x9q_clients[slot].generation;w.bridge_generation=bridge;
  w.len=len;w.order=++x9r_order;w.deadline=now+25000;memcpy(w.req,req,len);
  x9r_rebound(slot);return 0;
 }
 return X9Q_BUSY;
}
static bool x9r_response(const x9r_reply &w){
 x9q_packet p(w.req,w.error);p.p[5]=(uint8_t)(p.n-7);p.p[6]=(uint8_t)((p.n-7)>>8);
 const unsigned room=CBQ_HDR+p.n;
 uint8_t *owned=p.good?static_cast<uint8_t *>(IOMallocData(room)):nullptr;
 const bool admitted=owned&&x9r_local_live(w)&&
  bb_x9_nas_registration_result(CBQ_PORT_BASE+w.slot,X9BB_NODE,w.bridge_generation,p.p,p.n,owned,room,
   w.bind&&!w.error?w.deadline:0);
 if(!admitted&&owned){bzero(owned,room);IOFreeData(owned,room);}
 bzero(p.p,sizeof(p.p));return admitted;
}
static void x9r_poll(){
 for(x9r_reply &w:x9r_pending)if(w.used&&!x9r_live(w))bzero(&w,sizeof(w));
 for(unsigned slot=0;slot<CBQ_MAX_CLIENTS;slot++){
  x9r_reply &cache=x9r_completed[slot];
  if(cache.used&&!x9r_live(cache))bzero(&cache,sizeof(cache));
  if(cache.used&&cache.replay){if(!x9r_response(cache))continue;cache.replay=false;}
  x9r_reply *first=nullptr;
  for(x9r_reply &w:x9r_pending)if(w.used&&w.slot==slot&&(!first||w.order<first->order))first=&w;
  if(!first)continue;
  if(first->bind&&!first->error&&x9q_ms()>=first->deadline)first->error=X9Q_NOT_READY;
  if(!x9r_response(*first))continue;
  x9q_client &c=x9q_clients[slot];
  // The sole QMI thread also runs send/del callbacks. BB notifications only
  // complete host reads; host writes enqueue work for its NEXT poll, not reentry.
  // Only queue receipt grants the atomic patch. Untouched options use CURRENT
  // flags, not a stale admission snapshot from an earlier pending transaction.
  if(!first->error){
   if(first->bind)c.sim_slot=(uint8_t)first->route;
   else{
    if(first->mask&1)c.serving=(first->values&1)!=0;
    if(first->mask&2)c.system=(first->values&2)!=0;
    if(first->mask&4)c.signal=(first->values&4)!=0;
   }
   c.next=0;
  }
  printf("X9 NAS: SIM%u msg %04x txn %u result %u host queue receipt; route SIM%u serving %u system %u signal %u (not network registration/ready)\n",
   first->sim+1,x9q_u16(first->req+3),x9q_u16(first->req+1),first->error,c.sim_slot+1,c.serving,c.system,c.signal);
  cache=*first;cache.sim=c.sim_slot;cache.deadline=0;cache.replay=false;bzero(first,sizeof(*first));
 }
}
