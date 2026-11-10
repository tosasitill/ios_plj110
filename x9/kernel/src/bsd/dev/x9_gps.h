/* MT6993 GNSS ingress / iOS PDS adapter boundary. No invented receiver data. */
#ifndef X9_GPS_H
#define X9_GPS_H
#include <stdint.h>
#include "x9_gps_handoff.h"
#define X9_GPS_NMEA_MAX 200u
#define X9_GPS_RING 64u
#define X9_GPS_MAX_AGE_NS 2000000000ULL
#define X9_GPS_PDS_POSITION_BYTES 105u
#define X9_GPS_POSITION_RING 16u
struct x9_gps_status {
 uint64_t generation, sequence, dropped;
 uint32_t ready, enabled, tracking, interval_ms, fault;
 int32_t last_errno;
 uint64_t position_sequence;
 uint32_t position_supported;
 uint32_t layout_valid;
 /* Bring-up stage/error, independent of receiver-ready and output leases. */
 uint32_t physical_stage;
 int32_t physical_errno;
 /* One opt-in AON/PMIC observation, never a power or PDS lease.
  * Valid only at power_sample_time; does not qualify CONN register access. */
 uint32_t power_layout_valid, power_sample_valid;
 int32_t power_errno;
 uint64_t power_sample_time;
 uint32_t power_uv, power_control, power_da, power_hw_en, power_rc_mux;
 uint32_t power_ap_vote, power_global_vote, power_busy, power_done, power_ack;
 /* Raw native v5 AFE SMC journal, independent of receiver/PDS readiness.
  * started without returned may represent a partially accepted secure call. */
 uint32_t afe_smc_started, afe_smc_returned, afe_smc_qualified;
 uint64_t afe_smc_result[4];
 /* Native event2 parser journal, never availability or a FIX lease.
  * Counts only reports accepted by the physical application's handler. */
 uint64_t pvt_received, pvt_no_fix, pvt_rejected, pvt_delivered;
 uint32_t pvt_valid_mask;
 int32_t pvt_errno;
 /* Native clock transaction under a supplier lease, not a GPS FIX.
  * set_started remains latched even if confirmation fails. No auto-unvote. */
 uint32_t clock_started, clock_set_started, clock_complete, clock_borrowed;
 uint32_t clock_prior, clock_selector, clock_ap, clock_global;
 uint32_t clock_busy, clock_done, clock_ack;
 int32_t clock_errno;
 /* Native power SUBlease journal, not the aggregate connv2/PDS lease.
  * phase is a latched history: mapped1/rail2/selector4/HWV8/complete16.
  * No power sublease grants access to BGF DMA/SYSRAM or secure EMI. */
 uint32_t native_power_phase, native_power_vsel_write;
 uint32_t native_power_control_write, native_power_selector_write;
 uint32_t native_power_rc, native_power_clock;
 uint32_t native_power_vsel_prior, native_power_control_prior;
 uint32_t native_power_selector_prior, native_power_ap_prior;
 int32_t native_power_errno;
 /* Transport/power lifetime, independent of the application/output epoch
  * above. Successful OPEN/START/STOP/cadence transitions do not rebind DMA
  * or revoke an owned power request. Any uncertain transition faults BOTH
  * epochs; retained descriptors/resources must not be reused afterwards. */
 uint64_t hardware_generation;
 uint64_t pvt_retired;
 /* Native bus qualification AFTER aggregate acquisition, BEFORE BGF/POS
  * mapping. Journal is not DMA/SYSRAM/secure EMI or receiver readiness. */
 uint32_t bus_started, bus_complete, bus_wake_started, bus_wake_borrowed;
 uint32_t bus_wake_prior, bus_wake, bus_detect_attempts, bus_detect;
 uint32_t bus_version, bus_irq, bus_restore_required, bus_restore;
 int32_t bus_errno;
 /* Supplementary live NS CPU SR/AID policy check under an already held
  * secure/EMI supplier lease. Not BGF permission, DMA or receiver READY. */
 uint32_t emi_cpu_started, emi_cpu_valid, emi_cpu_region, emi_cpu_aid;
 uint32_t emi_cpu_intersections;
 uint64_t emi_cpu_checks;
 int32_t emi_cpu_errno;
 /* Native slot/byte-flow journal, never MNL/PDS readiness. Published is
  * successful HIF POST; host-ACK completion requires real MCU TX done. */
 uint32_t flow_initialized, flow_ack_queued, flow_ack_inflight;
 uint32_t flow_queued[2], flow_queued_bytes[2];
 uint64_t flow_published, flow_remote_received, flow_processed, flow_discarded;
 uint64_t flow_ack_total, flow_ack_completions, flow_reset_serial, flow_completed_reset;
 int32_t flow_errno;
 /* Selected secure ABI/remap observation under the real retained lease.
  * Not permission to call an SMC, BGF authority, ROM or GPS readiness. */
 uint32_t emi_cpu_abi, secure_remap_started, secure_remap_valid;
 uint32_t secure_remap_address;
 uint64_t secure_remap_checks;
 int32_t secure_remap_errno;
 /* Last refused v5/v6 registration, not the current provider's state.
  * May coexist with an already held v1..v4 lease; never overwrite its
  * physical stage/fault/epoch or imply that power callbacks were called. */
 uint32_t connv2_rejected_version, connv2_rejected_profile;
 int32_t connv2_rejected_errno;
 /* Real NORMAL/a0 MCUSYS receive observations; initialized is only the
  * AP parser. CTLR_CREATED/SCIF are not MNL START, PDS READY or an NV lease.
  * Error latches before the existing HIF/worker quarantine path runs. */
 uint32_t mcusys_initialized,mcusys_state,mcusys_seen_events;
 uint32_t mcusys_last_type,mcusys_last_id,mcusys_last_event,mcusys_scif_tick;
 uint64_t mcusys_frames,mcusys_transitions,mcusys_scif_events;
 int32_t mcusys_errno;
 /* Native NV initialized only by an explicit cold-authority provider.
  * No lease/ACK/readiness: dirty/claimed survive uncertain DMA failures. */
 uint32_t nv_started,nv_prepared,nv_dirty,nv_headers,nv_claimed,nv_remote_mask;
 uint32_t nv_last_id,nv_last_version,nv_last_size;
 uint64_t nv_checks,nv_reads,nv_writes,nv_invalidations,nv_contentions,nv_remote_events;
 int32_t nv_errno;
};
struct x9_gps_nmea {
 uint64_t sequence, generation, host_time;
 uint32_t length;
 uint8_t data[X9_GPS_NMEA_MAX];
};
struct x9_gps_backend {
 uint32_t version;
 /* Worker context, bounded synchronous hardware ACK. An error must never
  * leave a partially activated receiver: fault the generation if uncertain.
  * Call deliver_nmea only from a deferred worker, after set_state returns.
  * interval_ms is requested output cadence, not an RF clock/register value. */
 int (*set_state)(int enable, int tracking, uint32_t interval_ms, uint64_t generation);
 /* version 1: NMEA only. version 2: explicit native-position capability.
  * Advertising this requires measured receiver fixes and wire translation;
  * arrival of arbitrary MCU/NMEA traffic is not a parsed-position fix. */
 uint32_t position_supported;
};
struct x9_gps_position {
 uint64_t sequence, generation, host_time;
 uint8_t data[X9_GPS_PDS_POSITION_BYTES];
};
#ifdef __cplusplus
extern "C" {
#endif
void x9_gps_init(void);
int x9_gps_register_backend(const struct x9_gps_backend *backend);
int x9_gps_set_state(int enable, int tracking, uint32_t interval_ms);
void x9_gps_copy_status(struct x9_gps_status *status);
int x9_gps_copy_handoff(struct x9_gps_handoff *);
int x9_gps_deliver_nmea(uint64_t generation, const void *sentence,
    unsigned length, uint64_t host_time);
/* One cursor per PDS client; cursors are not shared or consumed by others. */
int x9_gps_next_nmea(uint64_t *cursor, struct x9_gps_nmea *sentence);
/* Native QMI PDS TLV 0x13. Input must be translated measured receiver data,
 * not raw MNL/MCUSYS bytes. 20H392 decoder 0x1004f09ec requires length0x69,
 * then reads LE32 mask, calendar, leap, UTC, uncertainty, LE doubles/floats.
 * The backend must provide valid_mask/calendar/time/uncertainty semantics;
 * this boundary bounds numeric wire values, but does not complete or infer
 * validity bits, UTC units or session-success status from NMEA traffic.
 * No coordinates are logged. Only an explicitly capable backend may submit. */
int x9_gps_deliver_position(uint64_t generation, const void *wire,
    unsigned length, uint64_t host_time);
int x9_gps_next_position(uint64_t *cursor, struct x9_gps_position *position);
void x9_gps_fault(uint64_t generation, int error);
/* Physical/HIF fault, matched against hardware_generation rather than the
 * current application's epoch. An old transport cannot fault a new owner. */
void x9_gps_hardware_fault(uint64_t hardware_generation, int error);
#ifdef __cplusplus
}
#endif
#endif
