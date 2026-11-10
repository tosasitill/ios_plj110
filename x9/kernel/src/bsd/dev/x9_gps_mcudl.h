/* MT6993 gps_mcudl data_intf wire boundary, not a UART or a receiver emulator.
 * Reference: OPPO gps_mcudl/data_intf/common/{inc/gps_mcudl_data_pkt.h,
 * gps_mcudl_data_pkt_parser.c} and host/gps_mcudl_data_pkt_host_impl.c.
 * A single deferred transport worker owns each stream. */
#ifndef X9_GPS_MCUDL_H
#define X9_GPS_MCUDL_H
#include "x9_gps.h"
#include "x9_gps_mnl.h"
#define X9_MCUDL_PAYLOAD_MAX 1800u
#define X9_MCUDL_FRAME_MAX (6u + X9_MCUDL_PAYLOAD_MAX + 1u)
#define X9_MCUDL_NMEA 0xa4u
struct x9_gps_mcudl_stream {
 uint64_t generation, sentence_time, fragment_time, last_host_time;
 /* generation is the hardware epoch. Preserve output epochs from the
  * first byte/fragment/sentence; never rebind old fragments after START. */
 uint64_t frame_application_generation, sentence_application_generation;
 uint64_t mnl_application_generation;
 uint64_t received_bytes, frames, bad_frames, non_nmea, sentences, bad_sentences;
 /* Raw arrival above includes partial frames. Native host status instead
  * acknowledges only bytes actually consumed or retired by the parser. */
 uint64_t processed_bytes, discarded_bytes;
 unsigned used, sentence_used;
 uint8_t frame[X9_MCUDL_FRAME_MAX], sentence[X9_GPS_NMEA_MAX];
 /* Control/flow-control packets must be consumed by the physical transport.
  * In particular, 0x35/0x36 are not NMEA and are not a ready/fix signal.
  * Callback must not re-enter receive on this same stream. */
 int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t);
 void *context;
 /* Optional MNL assembler. Legacy init preserves raw control dispatch. */
 struct x9_gps_mnl_stream mnl;
};
#ifdef __cplusplus
extern "C" {
#endif
void x9_gps_mcudl_stream_init(struct x9_gps_mcudl_stream *, uint64_t generation,
    int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t), void *);
/* Routes type0xa2 through the vendor inner assembler. Other non-NMEA types
 * still require the physical control handler, including urgent agent0xa3.
 * A NORMAL stream and an URGENT stream must never share partial state. */
void x9_gps_mcudl_stream_init_mnl(struct x9_gps_mcudl_stream *, uint64_t,
    int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t),
    int (*mnl)(void *, const uint8_t *, unsigned, uint64_t), void *);
/* Pure AP command+MCUDL serialization, not a send or a hardware ACK.
 * channel0=NORMAL/MNL, channel1=URGENT/agent. slot_seq is owned by the
 * physical slot/flow-control implementation, NOT an incrementing packet ID. */
int x9_gps_mcudl_make_mnl_control(void *, unsigned, uint8_t slot_seq,
    uint32_t command, const void *payload, unsigned payload_bytes,
    unsigned *frame_bytes, unsigned *channel);
/* Feed bytes from the MCU2AP NORMAL data ring, after CCIF ownership/cache
 * synchronization. This does not map EMI, acknowledge CCIF or boot firmware.
 * Other packet types go to control even before tracking/receiver readiness;
 * only type 0xa4 can deliver NMEA to PDS, and only while tracking is active.
 * Input and callbacks run in thread context. A physical reset needs a new
 * hardware generation; output transitions retain the old transport owner.
 * Nonzero errors mean the transport must stop using the partial stream. */
int x9_gps_mcudl_receive(struct x9_gps_mcudl_stream *, const void *, unsigned,
    uint64_t host_time);
/* HIF passes the epoch captured when the RX descriptor was POSTED, not at
 * completion. Controls still dispatch across application transitions;
 * only their proved session handler may accept OPEN/START/STOP responses. */
int x9_gps_mcudl_receive_epoch(struct x9_gps_mcudl_stream *, const void *, unsigned,
    uint64_t host_time, uint64_t application_generation);
#ifdef __cplusplus
}
#endif
#endif
