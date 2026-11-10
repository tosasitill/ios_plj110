/* Native MCUSYS parsing/state observation on the sole HIF worker.
 * Real vendor FSM3399c: SYS_ON->UNINIT, INIT_DONE->1, CTLR_CREATED->2,
 * RESET_START->3; RESET_END changes to UNINIT only from3. Its controller
 * event also releases pending kernel xlink opens, NOT AP MNL START/FIX.
 * Preserve these observations, but leave NV/shared RAM/xlink/application
 * handling to the actual provider. No NVLOCK ACK, POST_ON or fake ready.
 * Remote reset/restart is fatal to this worker: retained posted DMA and
 * application epochs cannot be rebound by a software state reset. */
static uint32_t gps_mcusys_u32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int x9_gps_mcusys_decode(const void *payload,unsigned length,
    struct x9_gps_mcusys_message *output)
{
 const uint8_t *p=payload;
 if(!p || !output)return EINVAL;
 if(length<8 || length>X9_GPS_MCUSYS_FRAME_BYTES)return EBADMSG;
 struct x9_gps_mcusys_message message={gps_mcusys_u32(p),UINT32_MAX,0};
 unsigned size=p[4]|(unsigned)p[5]<<8,required;
 switch(message.type) {
 case X9_GPS_MCUSYS_MNLBIN:case X9_GPS_MCUSYS_SCIF:required=4;break;
 case X9_GPS_MCUSYS_NVLOCK:case X9_GPS_MCUSYS_NVDATA:required=8;break;
 default:return ENOTSUP; /* includes AP-only types3/4 and unknown type6 */
 }
 if(size!=required || length<8+required)return EBADMSG;
 if(required==8) {
  message.id=gps_mcusys_u32(p+8);message.event=gps_mcusys_u32(p+12);
  if(message.id>=X9_GPS_MCUSYS_NV_IDS)return EBADMSG;
  if(message.event>(message.type==X9_GPS_MCUSYS_NVLOCK ? 8u : 4u))return EBADMSG;
 } else {
  message.event=gps_mcusys_u32(p+8);
  if(message.type==X9_GPS_MCUSYS_MNLBIN && message.event>4)return EBADMSG;
 }
 *output=message;return 0;
}
static void gps_mcusys_journal(int error)
{
 if(error && !gps_rt.mcusys.error)gps_rt.mcusys.error=error;
 lck_mtx_lock(&gps_lock);
 st.mcusys_initialized=gps_rt.mcusys.initialized;
 st.mcusys_state=gps_rt.mcusys.state;
 st.mcusys_seen_events=gps_rt.mcusys.seen_events;
 st.mcusys_last_type=gps_rt.mcusys.last_type;
 st.mcusys_last_id=gps_rt.mcusys.last_id;
 st.mcusys_last_event=gps_rt.mcusys.last_event;
 st.mcusys_scif_tick=gps_rt.mcusys.scif_tick;
 st.mcusys_frames=gps_rt.mcusys.frames;
 st.mcusys_transitions=gps_rt.mcusys.transitions;
 st.mcusys_scif_events=gps_rt.mcusys.scif_events;
 st.mcusys_errno=gps_rt.mcusys.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_mcusys_init(void)
{
 if(current_thread()!=gps_rt.worker || !gps_rt.lease ||
     !gps_rt.hif.prepared || gps_rt.hif.generation!=gps_rt.generation)return EPERM;
 if(gps_rt.mcusys.initialized)return EBUSY;
 /* Software monitor only, AFTER real HIF prepare and BEFORE reset release.
  * Do not clear target NV headers or infer that POST_ON has been sent. */
 gps_rt.mcusys.state=X9_GPS_MCUSYS_UNINIT;
 gps_rt.mcusys.last_type=gps_rt.mcusys.last_id=UINT32_MAX;
 gps_rt.mcusys.initialized=1;gps_mcusys_journal(0);return 0;
}
static int gps_mcusys_receive(unsigned channel,const uint8_t *p,unsigned length)
{
 if(current_thread()!=gps_rt.worker || !gps_rt.lease ||
     !gps_rt.mcusys.initialized || channel!=X9_GPS_HIF_NORMAL)return EPERM;
 if(gps_rt.mcusys.error)return gps_rt.mcusys.error;
 struct x9_gps_status status;x9_gps_copy_status(&status);
 if(status.hardware_generation!=gps_rt.generation || status.fault)return ESTALE;
 struct x9_gps_mcusys_message message;
 int error=x9_gps_mcusys_decode(p,length,&message);
 if(error){gps_mcusys_journal(error);return error;}
 if(gps_rt.mcusys.frames==UINT64_MAX ||
     gps_rt.mcusys.transitions==UINT64_MAX ||
     gps_rt.mcusys.scif_events==UINT64_MAX) {
  gps_mcusys_journal(EOVERFLOW);return EOVERFLOW;
 }
 gps_rt.mcusys.frames++;
 gps_rt.mcusys.last_type=message.type;gps_rt.mcusys.last_id=message.id;
 gps_rt.mcusys.last_event=message.event;
 if(message.type==X9_GPS_MCUSYS_MNLBIN) {
  uint32_t previous=gps_rt.mcusys.state,next=previous;
  gps_rt.mcusys.seen_events|=1u<<message.event;
  switch(message.event) {
  case X9_GPS_MCUSYS_SYS_ON:
   next=X9_GPS_MCUSYS_UNINIT;
   if(previous!=X9_GPS_MCUSYS_UNINIT)error=ECONNRESET;
   break;
  case X9_GPS_MCUSYS_INIT_DONE:next=X9_GPS_MCUSYS_INITIALIZED;break;
  case X9_GPS_MCUSYS_CTLR_CREATED:next=X9_GPS_MCUSYS_CONTROLLER;break;
  case X9_GPS_MCUSYS_RESET_START:
   next=X9_GPS_MCUSYS_RESETTING;error=ECONNRESET;break;
  case X9_GPS_MCUSYS_RESET_END:
   if(previous==X9_GPS_MCUSYS_RESETTING)next=X9_GPS_MCUSYS_UNINIT;
   /* A missing RESET_START cannot qualify old in-flight descriptors. */
   error=ECONNRESET;break;
  }
  if(next!=previous)gps_rt.mcusys.transitions++;
  gps_rt.mcusys.state=next;
 } else if(message.type==X9_GPS_MCUSYS_SCIF) {
  gps_rt.mcusys.scif_events++;gps_rt.mcusys.scif_tick=message.event;
 }
 /* Record actual RX regardless of whether the provider can process it.
  * These are received facts, NOT successful NV/app acknowledgements. */
 gps_mcusys_journal(error);return error;
}
