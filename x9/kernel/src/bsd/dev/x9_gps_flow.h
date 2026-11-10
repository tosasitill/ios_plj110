/* Internal v062 native slot state. Sole physical worker only; no public
 * supplier ABI or receiver-ready authority. Storage is always off-stack. */
#ifndef X9_GPS_FLOW_H
#define X9_GPS_FLOW_H
#define GPS_FLOW_ENTRIES 64u
#define GPS_FLOW_RING_BYTES 10240u
#define GPS_FLOW_WINDOW 3000u
#define GPS_FLOW_BATCH 1900u
#define GPS_FLOW_MARGIN 10u
#define GPS_FLOW_ACK_THRESHOLD 2048u
#define GPS_FLOW_ACK_BYTES 39u /* 6B envelope + 32B status + tail */
struct gps_flow_entry {
 uint64_t time;
 unsigned offset, length, host_ack, bootstrap_step;
};
struct gps_flow_slot {
 struct gps_flow_entry entries[GPS_FLOW_ENTRIES];
 uint8_t bytes[GPS_FLOW_RING_BYTES];
 unsigned head, count, write, used;
};
struct gps_native_flow {
 uint64_t generation, published, remote_received;
 uint64_t reset_serial, completed_reset;
 uint64_t ack_total, ack_reset, completed_total, ack_completions;
 uint32_t ack_id;
 unsigned initialized, native, ack_queued, ack_inflight;
 int error, candidate_error; /* dormant observation cannot fault a legacy supplier */
 struct gps_flow_slot slots[2];
 uint8_t batch[GPS_FLOW_BATCH];
};
#endif
