/* Native cold INIT's OPEN/STOP transport lifecycle on the existing worker.
 * Neither generic event0 nor a fresh event128 is an application transaction
 * ID. Thus this fence does NOT install a backend or make set_state succeed.
 * No second OPEN/STOP in this physical epoch, no reset/retry/free operation.
 * The supplier-owned mode never enters this state or changes behavior. */
static void gps_session_journal(int error)
{
 if(error && !gps_rt.session.error)gps_rt.session.error=error;
 lck_mtx_lock(&gps_lock);
 st.mnl_session_initialized=gps_rt.session.initialized;
 st.mnl_session_hardware=gps_rt.session.generation;
 st.mnl_session_open_queued=gps_rt.session.open_queued;
 st.mnl_session_open_posted=gps_rt.session.open_posted;
 st.mnl_session_open_completed=gps_rt.session.open_completed;
 st.mnl_session_stop_queued=gps_rt.session.stop_queued;
 st.mnl_session_stop_posted=gps_rt.session.stop_posted;
 st.mnl_session_stop_completed=gps_rt.session.stop_completed;
 st.mnl_session_stop_signal=gps_rt.session.stop_signal;
 st.mnl_session_stop_time=gps_rt.session.stop_time;
 st.mnl_session_inflight=gps_rt.session.inflight;
 st.mnl_session_id=gps_rt.session.id;
 st.mnl_session_errno=gps_rt.session.error;
 if(gps_native_output_inhibited())
  head=count=position_head=position_count=0; /* preserve client sequences */
 lck_mtx_unlock(&gps_lock);
}
static int gps_session_admit(uint32_t command,unsigned *tag)
{
 *tag=0;
 if(!gps_rt.session.initialized)return ENXIO;
 if(gps_rt.session.error)return gps_rt.session.error;
 if(gps_rt.session.generation!=gps_rt.generation)return ESTALE;
 /* Responses have no proved reusable identity. Even a fresh stop signal
  * cannot authorize a second native session/config/agent transaction. */
 if(gps_rt.session.stop_queued)return command==X9_GPS_MNL_STOP ? EALREADY : ENOTSUP;
 if(command==X9_GPS_MNL_OPEN) {
  if(gps_rt.session.open_queued)return EALREADY;
  if(gps_rt.bootstrap.completed!=4)return EAGAIN;
  *tag=1;
 } else if(command==X9_GPS_MNL_STOP) {
  if(!gps_rt.session.open_queued)return EINVAL;
  if(!gps_rt.session.open_completed)return EAGAIN;
  /* Do not leave a previously reserved URGENT agent/config command to be
   * posted after STOP. Await BOTH real TX descriptors and local FIFOs;
   * never delete/refund their uncertain traffic to make room for STOP. */
  if(gps_rt.flow.slots[0].count || gps_rt.flow.slots[1].count ||
      (gps_rt.hif.pending&((1u<<2)|(1u<<4))))return EAGAIN;
  *tag=2;
 }
 return 0;
}
static int gps_session_can_post(unsigned tag)
{
 if(tag!=1 && tag!=2)return EINVAL;
 if(!gps_rt.flow.native || !gps_rt.session.initialized)return ENXIO;
 if(gps_rt.session.generation!=gps_rt.generation)return ESTALE;
 if(gps_rt.session.error)return gps_rt.session.error;
 if(gps_rt.session.inflight)return EAGAIN;
 if(tag==1)return gps_rt.session.open_queued && !gps_rt.session.open_posted ? 0 : EPROTO;
 return gps_rt.session.stop_queued && gps_rt.session.open_completed &&
     !gps_rt.session.stop_posted ? 0 : EPROTO;
}
static void gps_session_posted(unsigned tag,uint64_t now)
{
 /* Called only after successful HIF POST. A caller cannot update history
  * by enqueue, management ACK, byte-credit update or generic notification. */
 gps_rt.session.inflight=tag;gps_rt.session.id=gps_rt.hif.id[2];
 if(tag==1)gps_rt.session.open_posted=1;
 else {gps_rt.session.stop_posted=1;gps_rt.session.stop_time=now;}
 gps_session_journal(0);
}
static int gps_session_complete(void)
{
 if(!gps_rt.session.initialized)return 0;
 if(gps_rt.session.error)return gps_rt.session.error;
 if(gps_rt.session.generation!=gps_rt.generation)return ESTALE;
 unsigned tag=gps_rt.session.inflight;
 if(tag) {
  if(tag>2 || gps_rt.hif.id[2]!=gps_rt.session.id)return EPROTO;
  if(!(gps_rt.hif.pending&(1u<<2))) {
   if(gps_rt.hif.tx_time[X9_GPS_HIF_NORMAL])return EPROTO;
   if(tag==1)gps_rt.session.open_completed=1;
   else gps_rt.session.stop_completed=1;
   gps_rt.session.inflight=0;gps_session_journal(0);
  }
 }
 if(gps_rt.session.stop_posted && !gps_rt.session.stop_signal) {
  uint64_t now=mach_absolute_time(),ns;
  if(now<gps_rt.session.stop_time)return EINVAL;
  absolutetime_to_nanoseconds(now-gps_rt.session.stop_time,&ns);
  /* Local six-second observation budget, not a guessed firmware ACK ABI.
   * Missing stop notification is uncertainty: quarantine, retain all DMA
   * and power leases. The HIF's shorter TX timeout remains unchanged. */
  if(ns>6000000000ULL)return ETIMEDOUT;
 }
 return 0;
}
