/* Native NORMAL byte-credit and local slot reservation, derived from the
 * actual PLJ110 v062 module. Not an application ACK or firmware emulator.
 * Included after gps_runtime_owned; every call is on the sole HIF worker.
 * No reset/rebind operation exists here. Initialize only after held-reset
 * HIF prepare. v2..v4 observe dormant counters until successful native INIT
 * explicitly opts in, then the existing worker pumps its ordered configs.
 * Unknown MCUSYS/NV/MNL transactions still need the supplier. */
static uint64_t gps_flow_le64(const uint8_t *p)
{
 uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;
}
static void gps_flow_put(uint8_t *p,uint64_t v,unsigned n)
{for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static int gps_flow_guard(void)
{
 struct gps_native_flow *f=&gps_rt.flow;
 if(current_thread()!=gps_rt.worker || gps_rt.ops.version<2 ||
     gps_rt.ops.version>6 || !gps_rt.lease)return EPERM;
 if(f->error)return f->error;
 if(!f->initialized || !gps_rt.hif.prepared)return ENXIO;
 struct x9_gps_status s;x9_gps_copy_status(&s);
 if(f->generation!=gps_rt.generation || f->generation!=gps_rt.hif.generation ||
     f->generation!=s.hardware_generation || s.fault)return ESTALE;
 return gps_rt.hif.fault ? (int)gps_rt.hif.fault : 0;
}
static void gps_flow_journal(int error)
{
 struct gps_native_flow *f=&gps_rt.flow;
 if(error && f->initialized && !f->error)f->error=error;
 lck_mtx_lock(&gps_lock);
 st.flow_initialized=f->initialized;st.flow_errno=f->error;
 st.mnl_bootstrap_native=f->native;st.mnl_native_candidate_errno=f->candidate_error;
 st.flow_published=f->published;st.flow_remote_received=f->remote_received;
 st.flow_processed=gps_rt.hif.stream[0].processed_bytes;
 st.flow_discarded=gps_rt.hif.stream[0].discarded_bytes;
 st.flow_ack_total=f->completed_total;st.flow_ack_completions=f->ack_completions;
 st.flow_reset_serial=f->reset_serial;st.flow_completed_reset=f->completed_reset;
 st.flow_ack_queued=f->ack_queued;st.flow_ack_inflight=f->ack_inflight;
 for(unsigned i=0;i<2;i++) {
  st.flow_queued[i]=f->slots[i].count;st.flow_queued_bytes[i]=f->slots[i].used;
 }
 lck_mtx_unlock(&gps_lock);
}
static int gps_flow_init(void)
{
 if(gps_rt.flow.initialized)return EBUSY;
 /* HIF prepare has just verified the physical reset/lease. Do not use
  * type36 or a management reply to zero software/hardware counters. */
 struct x9_gps_status s;x9_gps_copy_status(&s);
 if(!gps_rt.hif.prepared || gps_rt.hif.started || gps_rt.hif.pending ||
     !gps_rt.held_reset || gps_rt.booted || s.fault ||
     s.hardware_generation!=gps_rt.hif.generation ||
     s.hardware_generation!=gps_rt.generation)return ESTALE;
 bzero(&gps_rt.flow,sizeof(gps_rt.flow));
 gps_rt.flow.generation=gps_rt.generation;gps_rt.flow.initialized=1;
 gps_rt.flow.native=gps_rt.ops.version>=5;
 gps_flow_journal(0);return 0;
}
static int gps_flow_control(uint8_t type,const uint8_t *p,unsigned n)
{
 int e=gps_flow_guard();if(e)return e;
 struct gps_native_flow *f=&gps_rt.flow;
 if(type==0x35) {
  /* v062 status is 32B. Padding and debug low32 counters confer no
   * permissions. Never refund credit from a truncated/retrograde/future
   * count, and never interpret this packet as MNL START/receiver READY. */
  if(!p || n!=32)return EBADMSG;
  uint64_t received=gps_flow_le64(p);
  if(received<f->remote_received || received>f->published)return EPROTO;
  f->remote_received=received;return 0; /* duplicate has no credit effect */
 }
 if(type!=0x36)return ENOTSUP;
 /* Actual type36 ignores its payload and resets a host-status VIEW, not
  * parser cumulatives. Request a fresh snapshot, preserving an in-flight
  * older ACK and any subsequent reset request until its own completion. */
 if(f->reset_serial==UINT64_MAX)return EOVERFLOW;
 f->reset_serial++;return 0;
}
static int gps_flow_packet(unsigned c,const uint8_t *p,unsigned n,unsigned host_ack)
{
 if(c>=2 || !p || n<7 || n>X9_MCUDL_FRAME_MAX)return EINVAL;
 unsigned payload=((unsigned)p[3]<<8)|p[4];
 uint8_t sum=(uint8_t)(p[1]+p[2]+p[3]+p[4]),type=p[2];
 if(p[0]!=0xa5 || p[1]!=(uint8_t)(c<<5) || p[5]!=sum ||
     payload>X9_MCUDL_PAYLOAD_MAX || payload+7!=n || p[n-1]!=0xa3)return EBADMSG;
 if(host_ack) {
  if(c || type!=0x7e || n!=GPS_FLOW_ACK_BYTES)return EINVAL;
 } else {
  /* Only known xlink types. Native flow owns 7e; disallow provider-made
   * statistics, MCU-only 35/36 and 76 (unknown counter reset/rebind). */
  if(type<0xa0 || type>0xaf)return ENOTSUP;
  if(c!=(type==0xa3 ? 1u : 0u))return EINVAL;
 }
 return 0;
}
static int gps_flow_queue(unsigned c,const uint8_t *p,unsigned n,unsigned host_ack)
{
 int e=gps_flow_guard();if(e)return e;
 e=gps_flow_packet(c,p,n,host_ack);if(e)return e;
 if(host_ack && (gps_rt.flow.ack_queued || gps_rt.flow.ack_inflight))return EINVAL;
 uint8_t type=p[2];
 struct gps_flow_slot *s=&gps_rt.flow.slots[c];
 /* v062 a0..aa are noncritical; ab..af use the default critical class.
  * All xlink a0..af still obey the independent near-full admission gate. */
 unsigned reserve=(type>=0xa0 && type<=0xaa) ? 256u : 0;
 if(!host_ack && (s->count+10>=GPS_FLOW_ENTRIES ||
     s->used+2048>=GPS_FLOW_RING_BYTES))return ENOBUFS;
 if(s->count==GPS_FLOW_ENTRIES || n+reserve>GPS_FLOW_RING_BYTES-s->used)return ENOBUFS;
 struct gps_flow_entry *entry=&s->entries[(s->head+s->count)%GPS_FLOW_ENTRIES];
 entry->offset=s->write;entry->length=n;entry->host_ack=host_ack;
 entry->bootstrap_step=entry->session_command=0;
 entry->time=mach_absolute_time();
 /* Local ring may wrap a packet; gather it contiguously before HIF POST.
  * Its layout is not the firmware's ring and imposes no target ABI. */
 for(unsigned i=0;i<n;i++)s->bytes[(s->write+i)%GPS_FLOW_RING_BYTES]=p[i];
 s->write=(s->write+n)%GPS_FLOW_RING_BYTES;s->used+=n;s->count++;
 return 0; /* only local reservation, not even HIF submission */
}
static int gps_flow_consumed(uint64_t *consumed)
{
 const struct x9_gps_mcudl_stream *s=&gps_rt.hif.stream[0];
 if(s->processed_bytes>s->received_bytes ||
     s->discarded_bytes>s->received_bytes-s->processed_bytes)return EOVERFLOW;
 *consumed=s->processed_bytes+s->discarded_bytes;
 /* Called after poll/dispatch has returned, never from a parser callback. */
 return s->received_bytes-*consumed==s->used ? 0 : EPROTO;
}
static int gps_flow_ack_complete(void)
{
 struct gps_native_flow *f=&gps_rt.flow;
 if(!f->ack_inflight)return 0;
 /* No later NORMAL TX may replace this descriptor before this function
  * runs. hif.poll validated real MCU id/length/flags and cleared pending. */
 if(gps_rt.hif.id[2]!=f->ack_id)return ESTALE;
 if(gps_rt.hif.pending&(1u<<2))return 0;
 if(gps_rt.hif.tx_time[X9_GPS_HIF_NORMAL])return EPROTO;
 if(f->ack_completions==UINT64_MAX)return EOVERFLOW;
 f->completed_total=f->ack_total;f->completed_reset=f->ack_reset;
 f->ack_completions++;f->ack_inflight=0;return 0;
}
static int gps_flow_ack_stage(void)
{
 struct gps_native_flow *f=&gps_rt.flow;
 if(f->ack_queued || f->ack_inflight)return 0;
 uint64_t consumed;int e=gps_flow_consumed(&consumed);if(e)return e;
 if(consumed<f->completed_total)return EPROTO;
 if(f->reset_serial==f->completed_reset &&
     consumed-f->completed_total<GPS_FLOW_ACK_THRESHOLD)return 0;
 const struct x9_gps_mcudl_stream *s=&gps_rt.hif.stream[0];
 uint8_t frame[GPS_FLOW_ACK_BYTES];bzero(frame,sizeof(frame));
 frame[0]=0xa5;frame[2]=0x7e;frame[4]=32;frame[5]=0x9e;frame[38]=0xa3;
 gps_flow_put(frame+6,consumed,8);
 gps_flow_put(frame+14,(uint32_t)s->processed_bytes,4);
 gps_flow_put(frame+18,(uint32_t)s->discarded_bytes,4);
 /* route-drop is zero just like the running v062 host implementation. */
 gps_flow_put(frame+26,(uint32_t)s->frames,4);frame[30]=32;
 e=gps_flow_queue(0,frame,sizeof(frame),1);
 if(e==ENOBUFS)return 0; /* retain request; existing queue has a deadline */
 if(e)return e;
 f->ack_total=consumed;f->ack_reset=f->reset_serial;f->ack_queued=1;return 0;
}
static unsigned gps_flow_window(void)
{
 uint64_t used=gps_rt.flow.published-gps_rt.flow.remote_received;
 return used>=GPS_FLOW_WINDOW ? 0 : GPS_FLOW_WINDOW-(unsigned)used;
}
static int gps_flow_flush(unsigned c,uint64_t now)
{
 struct gps_native_flow *f=&gps_rt.flow;
 struct gps_flow_slot *s=&f->slots[c];
 if(!s->count || (gps_rt.hif.pending&(1u<<(2*(c+1)))))return 0;
 /* Actual slot_flush_inner uses NORMAL's window for BOTH slots. URGENT
  * has no independent credit accounting and no critical bypass. */
 unsigned window=gps_flow_window();
 /* flush_inner caps at1900 BEFORE best_fit subtracts10: max1890 wire
  * bytes, not1900 with a margin applied only to the credit window. */
 unsigned limit=window>GPS_FLOW_BATCH ? GPS_FLOW_BATCH : window;
 if(limit<=GPS_FLOW_MARGIN)return 0;
 limit-=GPS_FLOW_MARGIN;
 unsigned bytes=0,packets=0,ack=0,bootstrap=0,session=0;
 while(packets<s->count) {
  struct gps_flow_entry *entry=&s->entries[(s->head+packets)%GPS_FLOW_ENTRIES];
  if(entry->bootstrap_step) {
   /* One bootstrap command gets one real descriptor. Flush preceding
    * non-bootstrap traffic first; never coalesce it with a host-status ACK
    * or post the next step before the prior descriptor has retired. */
   if(packets)break;
   if(c || !gps_rt.bootstrap.active ||
       gps_rt.bootstrap.generation!=gps_rt.generation ||
       entry->bootstrap_step!=gps_rt.bootstrap.posted+1)return EPROTO;
   if(gps_rt.bootstrap.inflight ||
       gps_rt.bootstrap.posted!=gps_rt.bootstrap.completed)return 0;
   bootstrap=entry->bootstrap_step;
  }
  if(entry->session_command) {
   /* OPEN/STOP get their own descriptor too. Never retire their TX from
    * completion of a coalesced status ACK or unrelated application bytes. */
   if(packets)break;
   if(c || bootstrap)return EPROTO;
   int e=gps_session_can_post(entry->session_command);
   if(e==EAGAIN)return 0;if(e)return e;
   session=entry->session_command;
  }
  if(entry->length>limit-bytes)break; /* preserve same-slot FIFO */
  for(unsigned i=0;i<entry->length;i++)
   f->batch[bytes+i]=s->bytes[(entry->offset+i)%GPS_FLOW_RING_BYTES];
  bytes+=entry->length;ack+=entry->host_ack;packets++;
  if(bootstrap || session)break;
 }
 if(!bytes)return 0;
 if(ack>1 || (ack && (!f->ack_queued || f->ack_inflight)))return EPROTO;
 if(!c && bytes>UINT64_MAX-f->published)return EOVERFLOW;
 int e=x9_gps_hif_send(&gps_rt.hif,c+1,f->batch,bytes,now);
 if(e==EAGAIN || e==EBUSY)return 0; /* keep all entries/snapshot */
 if(e)return e;
 /* Published interface bytes, not requested bytes or MCU application ACK.
  * HIF owns its own copy; local entries retire only after successful POST. */
 if(!c)f->published+=bytes;
 if(session)gps_session_posted(session,now);
 if(bootstrap) {
  gps_rt.bootstrap.posted=bootstrap;gps_rt.bootstrap.inflight=bootstrap;
  gps_rt.bootstrap.id=gps_rt.hif.id[2];gps_runtime_bootstrap_journal(0);
 }
 if(ack) {
  f->ack_id=gps_rt.hif.id[2];f->ack_queued=0;f->ack_inflight=1;
 }
 s->head=(s->head+packets)%GPS_FLOW_ENTRIES;s->count-=packets;s->used-=bytes;
 return 0;
}
static int gps_flow_drain(int management,uint64_t now)
{
 int e=gps_flow_guard();if(e)return e;
 if(gps_rt.flow.remote_received>gps_rt.flow.published)return EPROTO;
 e=gps_flow_ack_complete();if(e)return e;
 e=gps_flow_ack_stage();if(e)return e;
 for(unsigned c=0;c<2;c++) {
  struct gps_flow_slot *s=&gps_rt.flow.slots[c];
  if(s->count) {
   uint64_t then=s->entries[s->head].time,ns;
   if(now<then) {
    /* ACK staging above may be a few ticks newer than the drain input. */
    now=mach_absolute_time();if(now<then)return EINVAL;
   }
   absolutetime_to_nanoseconds(now-then,&ns);
   if(ns>1500000000ULL)return ETIMEDOUT;
  }
  if(!management) {e=gps_flow_flush(c,now);if(e)return e;}
 }
 return 0;
}
