/* MT6993 GNSS ingress / iOS PDS adapter boundary. No invented receiver data. */
#ifndef X9_GPS_H
#define X9_GPS_H
#include <stdint.h>
#define X9_GPS_NMEA_MAX 200u
#define X9_GPS_RING 64u
#define X9_GPS_MAX_AGE_NS 2000000000ULL
struct x9_gps_status {
 uint64_t generation, sequence, dropped;
 uint32_t ready, enabled, tracking, interval_ms, fault;
 int32_t last_errno;
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
};
#ifdef __cplusplus
extern "C" {
#endif
void x9_gps_init(void);
int x9_gps_register_backend(const struct x9_gps_backend *backend);
int x9_gps_set_state(int enable, int tracking, uint32_t interval_ms);
void x9_gps_copy_status(struct x9_gps_status *status);
int x9_gps_deliver_nmea(uint64_t generation, const void *sentence,
    unsigned length, uint64_t host_time);
/* One cursor per PDS client; cursors are not shared or consumed by others. */
int x9_gps_next_nmea(uint64_t *cursor, struct x9_gps_nmea *sentence);
void x9_gps_fault(uint64_t generation, int error);
#ifdef __cplusplus
}
#endif
#endif
