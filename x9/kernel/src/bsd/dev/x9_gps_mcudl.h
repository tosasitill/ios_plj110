/* MT6993 gps_mcudl data_intf wire boundary, not a UART or a receiver emulator.
 * Reference: OPPO gps_mcudl/data_intf/common/{inc/gps_mcudl_data_pkt.h,
 * gps_mcudl_data_pkt_parser.c} and host/gps_mcudl_data_pkt_host_impl.c.
 * A single deferred transport worker owns each stream. */
#ifndef X9_GPS_MCUDL_H
#define X9_GPS_MCUDL_H
#include "x9_gps.h"
#define X9_MCUDL_PAYLOAD_MAX 1800u
#define X9_MCUDL_FRAME_MAX (6u + X9_MCUDL_PAYLOAD_MAX + 1u)
#define X9_MCUDL_NMEA 0xa4u
struct x9_gps_mcudl_stream {
 uint64_t generation, sentence_time, fragment_time, last_host_time;
 uint64_t received_bytes, frames, bad_frames, non_nmea, sentences, bad_sentences;
 unsigned used, sentence_used;
 uint8_t frame[X9_MCUDL_FRAME_MAX], sentence[X9_GPS_NMEA_MAX];
 /* Control/flow-control packets must be consumed by the physical transport.
  * In particular, 0x35/0x36 are not NMEA and are not a ready/fix signal.
  * Callback must not re-enter receive on this same stream. */
 int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t);
 void *context;
};
#ifdef __cplusplus
extern "C" {
#endif
void x9_gps_mcudl_stream_init(struct x9_gps_mcudl_stream *, uint64_t generation,
    int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t), void *);
/* Feed bytes from the MCU2AP NORMAL data ring, after CCIF ownership/cache
 * synchronization. This does not map EMI, acknowledge CCIF or boot firmware.
 * Other packet types go to control even before tracking/receiver readiness;
 * only type 0xa4 can deliver NMEA to PDS, and only while tracking is active.
 * Input and callbacks run in thread context. A reset needs a new generation.
 * Nonzero errors mean the transport must stop using the partial stream. */
int x9_gps_mcudl_receive(struct x9_gps_mcudl_stream *, const void *, unsigned,
    uint64_t host_time);
#ifdef __cplusplus
}
#endif
#endif
