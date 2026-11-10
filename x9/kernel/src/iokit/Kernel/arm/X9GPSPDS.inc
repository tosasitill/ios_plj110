/* Native locationd QMI PDS, not CLLocation simulation/injection.
 * 20H392 CLQmiPdsManager::sendSetGpsServiceState builds message0x21 at
 * 0x1004dd638; request state@TLV1 and optional uint64 mask@TLV0x10.
 * Standard PDS NMEA layouts: libqmi qmi-service-pds.json. Native parsed
 * position: 20H392 locationd 0x1004f0a24 requires 105 bytes, not the obsolete
 * libqmi comment's one-byte validity mask. Assistance/SUPL remain unsupported.
 * All client state is owned by X9Baseband's sole QRTR worker. */
#include "../../../bsd/dev/x9_gps.h"
struct x9g_client {
 bool initialized, enabled, automatic, nmea, extended, parsed;
 uint8_t timeout;
 uint32_t interval, accuracy;
 uint64_t cursor, position_cursor;
};
static x9g_client x9g_clients[CBQ_MAX_CLIENTS];
static bool x9g_enabled;
static x9g_client &x9g_client_for(unsigned slot) {
 x9g_client &c = x9g_clients[slot];
 if (!c.initialized) {
  c.initialized = true; c.automatic = true;
  c.timeout = 30; c.interval = 1; c.accuracy = 50;
  x9_gps_status s; x9_gps_copy_status(&s);
  c.cursor = s.sequence; c.position_cursor = s.position_sequence;
 }
 return c;
}
static uint16_t x9g_error(int error) {
 return error == ENXIO || error == EAGAIN ? X9Q_NOT_READY :
        error == EBUSY ? X9Q_BUSY : error == EINVAL ? X9Q_ARG : X9Q_INTERNAL;
}
/* Transactional union of all clients. A client cannot stop another's lease
 * or make the shared engine claim success when physical activation failed. */
static int x9g_apply(unsigned slot, const x9g_client &proposal) {
 bool enabled = false, tracking = false; uint32_t interval = 3600;
 for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++) {
  const x9g_client &c = i == slot ? proposal : x9g_clients[i];
  if (!c.initialized || !c.enabled) continue;
  enabled = true;
  if (c.automatic) { tracking = true; if (c.interval < interval) interval = c.interval; }
 }
 return x9_gps_set_state(enabled, tracking, tracking ? interval*1000 : 1000);
}
static bool x9g_send(uint32_t local, uint32_t port, const uint8_t *req, unsigned len) {
 if (port != X9BB_PORT_BASE + 6) return false;
 unsigned slot = local-CBQ_PORT_BASE;
 if (!x9g_enabled || slot >= CBQ_MAX_CLIENTS) {
  x9q_packet p(req, X9Q_UNSUPPORTED); p.send(local, port); return true;
 }
 x9g_client &c = x9g_client_for(slot), next = c;
 uint16_t id = x9q_u16(req+3), error = 0; bool mutation = false;
 x9q_view v = x9q_tlv(req, len, 1);
 switch (id) {
 case 0: /* Reset this client's settings/lease only. */
  if (len != 7) { error = X9Q_ARG; break; }
  next.enabled = next.nmea = next.extended = next.parsed = false;
  next.automatic = true; next.interval = 1; next.timeout = 30; next.accuracy = 50;
  mutation = true; break;
 case 1: /* Only an explicitly capable physical backend can subscribe fixes. */
  for (unsigned i = 7; i < len; i += 3+x9q_u16(req+i+1)) {
   uint8_t tag = req[i]; unsigned n = x9q_u16(req+i+1);
   if (n != 1 || req[i+3] > 1) { error = X9Q_ARG; break; }
   if (tag == 0x10) next.nmea = req[i+3];
   else if (tag == 0x11) next.extended = req[i+3];
   else if (tag == 0x12) {
    x9_gps_status s; x9_gps_copy_status(&s);
    if (req[i+3] && !s.position_supported) { error = X9Q_UNSUPPORTED; break; }
    next.parsed = req[i+3];
   }
   else if (tag < 0x12 || tag > 0x23 || req[i+3]) { error = X9Q_UNSUPPORTED; break; }
  }
  if (!error) {
   x9_gps_status s; x9_gps_copy_status(&s);
   next.cursor = s.sequence; next.position_cursor = s.position_sequence; c = next;
  }
  break;
 case 0x20: /* Get GPS service state */
  if (len != 7) error = X9Q_ARG;
  else {
   x9_gps_status s; x9_gps_copy_status(&s);
   uint8_t state[2] = { (uint8_t)(c.enabled && s.enabled && s.ready),
       (uint8_t)(c.enabled && c.automatic && s.tracking ? 2 : 1) };
   x9q_packet p(req); p.tlv(1, state, sizeof(state)); p.send(local, port); return true;
  } break;
 case 0x21: /* iOS may supply an 8-byte reason/enable mask. */
  if (!v.p || v.n != 1 || v.p[0] > 1) { error = X9Q_ARG; break; }
  for (unsigned i = 7; i < len; i += 3+x9q_u16(req+i+1))
   if (req[i] != 1 && (req[i] != 0x10 || x9q_u16(req+i+1) != 8)) error = X9Q_UNSUPPORTED;
  next.enabled = v.p[0]; mutation = !error; break;
 case 0x29: { /* Standalone default tracking configuration. */
  if (len != 7) { error = X9Q_ARG; break; }
  uint8_t info[10] = {0, c.timeout};
  for (unsigned i = 0; i < 4; i++) {
   info[2+i] = (uint8_t)(c.interval >> (8*i));
   info[6+i] = (uint8_t)(c.accuracy >> (8*i));
  }
  x9q_packet p(req); p.tlv(1, info, sizeof(info)); p.send(local, port); return true;
 }
 case 0x2a:
  if (len != 20 || !v.p || v.n != 10 || !v.p[1] || !x9q_u32(v.p+2) ||
      x9q_u32(v.p+2) > 3600 || !x9q_u32(v.p+6)) { error = X9Q_ARG; break; }
  if (v.p[0]) { error = X9Q_UNSUPPORTED; break; } /* no claimed MS-based assistance */
  next.timeout = v.p[1]; next.interval = x9q_u32(v.p+2); next.accuracy = x9q_u32(v.p+6);
  mutation = true; break;
 case 0x30: { /* Get Auto Tracking State */
  if (len != 7) { error = X9Q_ARG; break; }
  uint8_t automatic = c.automatic;
  x9q_packet p(req); p.tlv(1, &automatic, 1); p.send(local, port); return true;
 }
 case 0x31:
  if (len != 11 || !v.p || v.n != 1 || v.p[0] > 1) error = X9Q_ARG;
  else { next.automatic = v.p[0]; mutation = true; } break;
 default: error = X9Q_UNSUPPORTED; break;
 }
 if (mutation) {
  int e = x9g_apply(slot, next);
  if (e) {
   error = x9g_error(e);
   x9_gps_status s; x9_gps_copy_status(&s);
   /* The native client must distinguish a qualified LK layout from an
    * activated receiver. Never use layout_valid to grant a PDS lease. */
   printf("X9 GPS PDS: hardware transition errno %d layout %u ready %u fault %u physical %u/%d app/hw epoch %llu/%llu\n",
       e, s.layout_valid, s.ready, s.fault, s.physical_stage, s.physical_errno,
       s.generation,s.hardware_generation);
   if(s.connv2_rejected_errno)
    printf("X9 GPS PDS: last connv2 registration refused v%u secure profile %u errno %d before acquire (not current provider state or receiver readiness)\n",
        s.connv2_rejected_version,s.connv2_rejected_profile,
        s.connv2_rejected_errno);
   if(s.mcusys_initialized)
    printf("X9 GPS PDS: MCUSYS RX frames %llu state %u events %x transitions %llu last type/id/event %u/%u/%u SCIF %llu tick %u errno %d (not NV/app ACK or receiver readiness)\n",
        s.mcusys_frames,s.mcusys_state,s.mcusys_seen_events,s.mcusys_transitions,
        s.mcusys_last_type,s.mcusys_last_id,s.mcusys_last_event,
        s.mcusys_scif_events,s.mcusys_scif_tick,s.mcusys_errno);
   if(s.nv_started)
    printf("X9 GPS PDS: native NV prepared %u dirty %u headers %x retained-claim %x notifications %llu/%x last id/version/size %u/%u/%u checks %llu read/write/delete %llu/%llu/%llu contention %llu errno %d (not persistent storage, app ACK or receiver readiness)\n",
        s.nv_prepared,s.nv_dirty,s.nv_headers,s.nv_claimed,
        s.nv_remote_events,s.nv_remote_mask,s.nv_last_id,s.nv_last_version,
        s.nv_last_size,s.nv_checks,s.nv_reads,s.nv_writes,s.nv_invalidations,
        s.nv_contentions,s.nv_errno);
   if(s.mnl_control_initialized)
    printf("X9 GPS PDS: MNL control frames %llu last event %u declared/body %u/%u notification %d generic/stop-done/terminated %llu/%llu/%llu RX app/time %llu/%llu errno %d (observations, not matched START/STOP ACK or receiver readiness)\n",
        s.mnl_control_frames,s.mnl_control_event,s.mnl_control_declared,
        s.mnl_control_body,s.mnl_notification,s.mnl_notifications,
        s.mnl_stop_done,s.mnl_terminations,s.mnl_control_application,
        s.mnl_control_time,s.mnl_control_errno);
   if(s.pvt_received)
    printf("X9 GPS PDS: native PVT reports %llu no-fix %llu rejected %llu delivered %llu retired %llu mask %x errno %d (not a readiness lease)\n",
        s.pvt_received,s.pvt_no_fix,s.pvt_rejected,s.pvt_delivered,s.pvt_retired,
        s.pvt_valid_mask,s.pvt_errno);
   if(s.bus_started)
    printf("X9 GPS PDS: native bus complete %u wake %u/%u detect %x attempts %u IP %x IRQ %x restore %u/%x errno %d (not DMA/EMI/receiver readiness)\n",
        s.bus_complete,s.bus_wake_started,s.bus_wake_borrowed,s.bus_detect,
        s.bus_detect_attempts,s.bus_version,s.bus_irq,s.bus_restore_required,
        s.bus_restore,s.bus_errno);
   if(s.clock_started)
    printf("X9 GPS PDS: clock started %u set %u complete %u borrowed %u selector %x ap %x global %x busy %x done %x ack %x errno %d (not receiver readiness)\n",
        s.clock_started,s.clock_set_started,s.clock_complete,s.clock_borrowed,
        s.clock_selector,s.clock_ap,s.clock_global,s.clock_busy,s.clock_done,
        s.clock_ack,s.clock_errno);
   if(s.emi_cpu_started)
    printf("X9 GPS PDS: HIF CPU SMPU ABI %u policy valid %u SR %u AID %u intersections %u checks %llu errno %d (not BGF/DMA/receiver authority)\n",
        s.emi_cpu_abi,s.emi_cpu_valid,s.emi_cpu_region,s.emi_cpu_aid,
        s.emi_cpu_intersections,s.emi_cpu_checks,s.emi_cpu_errno);
   if(s.secure_remap_started)
    printf("X9 GPS PDS: secure remap readback valid %u CSR %x checks %llu errno %d (not secure ABI/BGF/receiver authority)\n",
        s.secure_remap_valid,s.secure_remap_address,s.secure_remap_checks,
        s.secure_remap_errno);
   if(s.flow_initialized)
    printf("X9 GPS PDS: native flow published %llu remote %llu parse %llu/%llu host-ACK %llu completions %llu reset %llu/%llu staged %u inflight %u slots %u/%u bytes %u/%u errno %d (not MNL/PDS readiness)\n",
        s.flow_published,s.flow_remote_received,s.flow_processed,s.flow_discarded,
        s.flow_ack_total,s.flow_ack_completions,s.flow_reset_serial,s.flow_completed_reset,
        s.flow_ack_queued,s.flow_ack_inflight,s.flow_queued[0],s.flow_queued[1],
        s.flow_queued_bytes[0],s.flow_queued_bytes[1],s.flow_errno);
   if(s.native_power_phase || s.native_power_errno)
    printf("X9 GPS PDS: native power sublease phase %x writes %u/%u/%u RC %u clock %u errno %d (not connv2/DMA/receiver qualification)\n",
        s.native_power_phase,s.native_power_vsel_write,
        s.native_power_control_write,s.native_power_selector_write,
        s.native_power_rc,s.native_power_clock,s.native_power_errno);
   /* A supply readback is a timestamped observation, not permission to
    * activate receiver services or infer availability from Android. */
   if(s.afe_smc_started)
    printf("X9 GPS PDS: AFE SMC started %u returned %u qualified %u raw %llx/%llx/%llx/%llx (not receiver readiness)\n",
        s.afe_smc_started,s.afe_smc_returned,s.afe_smc_qualified,
        s.afe_smc_result[0],s.afe_smc_result[1],s.afe_smc_result[2],s.afe_smc_result[3]);
   if(s.power_sample_time || s.power_errno)
    printf("X9 GPS PDS: power observation topology %u valid %u errno %d uv %u ap %x global %x busy %x (not a lease)\n",
        s.power_layout_valid,s.power_sample_valid,s.power_errno,s.power_uv,
        s.power_ap_vote,s.power_global_vote,s.power_busy);
  }
  else {
   x9_gps_status s; x9_gps_copy_status(&s);
   next.cursor = s.sequence; next.position_cursor = s.position_sequence; c = next;
  }
 }
 x9q_packet p(req, error); p.send(local, port);
 printf("X9 GPS PDS: msg 0x%04x txn %u error %u (no coordinates logged)\n", id, x9q_u16(req+1), error);
 return true;
}
static void x9g_del(unsigned slot) {
 if (!x9g_enabled || slot >= CBQ_MAX_CLIENTS || !x9g_clients[slot].initialized) return;
 x9g_client empty = {};
 int e = x9g_apply(slot, empty);
 x9g_clients[slot] = empty;
 if (e) { x9_gps_status s; x9_gps_copy_status(&s); x9_gps_fault(s.generation, e); }
}
static void x9g_poll(void) {
 if (!x9g_enabled) return;
 for (unsigned slot = 0; slot < CBQ_MAX_CLIENTS; slot++) {
  x9g_client &c = x9g_clients[slot];
  if (!c.enabled || !c.automatic) continue;
  if (c.parsed) for (unsigned i = 0; i < 4; i++) {
   x9_gps_position position;
   if (x9_gps_next_position(&c.position_cursor, &position)) break;
   uint8_t req[7] = {}; x9q_packet p(req);
   p.tlv(0x13, position.data, sizeof(position.data));
   p.send(CBQ_PORT_BASE+slot, X9BB_PORT_BASE+6, true, 1);
  }
  if (!c.nmea && !c.extended) continue;
  /* Bound work per bridge iteration; no starvation of WDS/CCCI/SIM. */
  for (unsigned i = 0; i < 4; i++) {
   x9_gps_nmea n;
   if (x9_gps_next_nmea(&c.cursor, &n)) break;
   uint8_t req[7] = {}; x9q_packet p(req);
   if (c.nmea) p.tlv(0x10, n.data, n.length);
   if (c.extended) {
    uint8_t data[3+X9_GPS_NMEA_MAX] = {0, (uint8_t)n.length, (uint8_t)(n.length>>8)};
    memcpy(data+3, n.data, n.length); p.tlv(0x11, data, n.length+3);
   }
   /* Never derive a FIX/connected status from receipt of NMEA: RMC V/GGA 0
    * are legitimate no-fix sentences, and must stay no-fix at the consumer. */
   p.send(CBQ_PORT_BASE+slot, X9BB_PORT_BASE+6, true, 1);
  }
 }
}
