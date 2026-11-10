/* Complete BODY notifications on the sole HIF worker, not MNL commands.
 * Original libmnl705664 event0 -> generic function pointer766218, written
 * by542f68 from registration member0. Original mnld6ccc4 registers table
 * a0d50; its RELATIVE relocation selects generic callback6a730. Notification
 * 0/3/7/13/16 there are not OPEN/START transaction results.
 * Event128 ->705cc0 ->51b990 -> level event10, matching cmd1 STOP waiter
 * 541330. Event5 ->705d24 only logs terminaled. Neither has a proved wire
 * transaction ID, freshness barrier or DMA retirement proof. No matching
 * transaction/default ACK/READY is manufactured from any of these events. */
static void gps_mnl_control_journal_locked(void)
{
 st.mnl_control_initialized=gps_rt.mnl_control.initialized;
 st.mnl_control_event=gps_rt.mnl_control.event;
 st.mnl_control_declared=gps_rt.mnl_control.declared;
 st.mnl_control_body=gps_rt.mnl_control.body;
 st.mnl_notification=gps_rt.mnl_control.notification;
 st.mnl_control_frames=gps_rt.mnl_control.frames;
 st.mnl_notifications=gps_rt.mnl_control.notifications;
 st.mnl_stop_done=gps_rt.mnl_control.stop_done;
 st.mnl_terminations=gps_rt.mnl_control.terminations;
 st.mnl_control_application=gps_rt.mnl_control.application;
 st.mnl_control_time=gps_rt.mnl_control.time;
 st.mnl_control_errno=gps_rt.mnl_control.error;
}
static int gps_mnl_control_init(void)
{
 if(current_thread()!=gps_rt.worker || !gps_rt.lease ||
     !gps_rt.hif.prepared || gps_rt.hif.generation!=gps_rt.generation)return EPERM;
 if(gps_rt.mnl_control.initialized)return EBUSY;
 /* Software monitor only, before reset release. No MCU command is sent. */
 gps_rt.mnl_control.event=UINT32_MAX;
 gps_rt.mnl_control.initialized=1;
 lck_mtx_lock(&gps_lock);gps_mnl_control_journal_locked();
 lck_mtx_unlock(&gps_lock);return 0;
}
static int gps_mnl_control_receive(const uint8_t *p,unsigned length,
    uint64_t application,uint64_t first_time)
{
 if(current_thread()!=gps_rt.worker || !gps_rt.lease ||
     !gps_rt.mnl_control.initialized)return EPERM;
 if(gps_rt.mnl_control.error)return gps_rt.mnl_control.error;
 struct x9_gps_mnl_control_message message;
 int error=x9_gps_mnl_control_decode(p,length,&message);
 lck_mtx_lock(&gps_lock);
 if(st.hardware_generation!=gps_rt.generation || st.fault) {
  lck_mtx_unlock(&gps_lock);return ESTALE;
 }
 if(error==ENOTSUP) {
  /* PVT/text/other events still require their actual provider. No blanket
   * ACK, extra payload parser or changed translation semantics here. */
  lck_mtx_unlock(&gps_lock);return 0;
 }
 if(!error && (!first_time || first_time>mach_absolute_time()))error=EINVAL;
 if(!error && (gps_rt.mnl_control.frames==UINT64_MAX ||
     gps_rt.mnl_control.notifications==UINT64_MAX ||
     gps_rt.mnl_control.stop_done==UINT64_MAX ||
     gps_rt.mnl_control.terminations==UINT64_MAX))error=EOVERFLOW;
 if(!error) {
  gps_rt.mnl_control.frames++;
  gps_rt.mnl_control.event=message.event;
  gps_rt.mnl_control.declared=message.declared_bytes;
  gps_rt.mnl_control.body=message.body_bytes;
  gps_rt.mnl_control.notification=message.notification;
  gps_rt.mnl_control.application=application;
  gps_rt.mnl_control.time=first_time;
  switch(message.event) {
  case X9_GPS_MNL_NOTIFICATION:gps_rt.mnl_control.notifications++;break;
  case X9_GPS_MNL_STOP_DONE:gps_rt.mnl_control.stop_done++;break;
  case X9_GPS_MNL_TERMINATED:
   gps_rt.mnl_control.terminations++;error=ECONNABORTED;break;
  }
 }
 /* Even a terminal event from an old APPLICATION epoch ends this same
  * physical MCU runtime. Do not let cached fixes survive it. A malformed
  * known event likewise cannot authorize any subsequent provider traffic.
  * Status and epoch/output retirement are one atomic mutation. */
 if(error) {
  gps_rt.mnl_control.error=error;
  gps_fault_locked(error);
 }
 gps_mnl_control_journal_locked();lck_mtx_unlock(&gps_lock);
 return error;
}
