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
#ifdef __cplusplus
}
#endif
#endif
