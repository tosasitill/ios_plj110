/* Vendor libmnl inner fragmentation, not the A5 MCUDL envelope or PDS wire.
 * A stream belongs to one deferred transport worker; allocate it off stack.
 * Recovered from libmnl BuildID 0ebb7a655d4a8e040cdd60b0fffa2efd,
 * mtk_gps_ofl_ap_pack_data (0x707a00) and ap2mcu_ofl_sender (0x70661c). */
#ifndef X9_GPS_MNL_H
#define X9_GPS_MNL_H
#include <stdint.h>
#define X9_MNL_PACKET_MAX 1536u
#define X9_MNL_FRAGMENT_MAX (X9_MNL_PACKET_MAX - 4u)
/* The vendor dispatch rejects a negative signed16 assembled length. */
#define X9_MNL_MESSAGE_MAX 32767u
struct x9_gps_mnl_stream {
 uint64_t generation, first_time, last_time;
 uint64_t messages, bad_fragments;
 unsigned used;
 uint8_t total, next;
 uint8_t message[X9_MNL_MESSAGE_MAX];
 /* Complete MCU response BODY, without the four-byte fragment header.
  * NOT the AP command format: MCU event/length are LE16 at body+0/+2.
  * Vendor's synthetic 4B reassembly header is NOT part of this body.
  * Event2 PVT begins at body+6 (vendor buffer+10), with 3168B payload.
  * This callback must
  * translate measured fields before deliver_position; never infer READY.
  * Body lifetime is only the call; callback must not reenter this stream. */
 int (*complete)(void *, const uint8_t *, unsigned, uint64_t);
 void *context;
};
#ifdef __cplusplus
extern "C" {
#endif
void x9_gps_mnl_stream_init(struct x9_gps_mnl_stream *, uint64_t,
    int (*)(void *, const uint8_t *, unsigned, uint64_t), void *);
void x9_gps_mnl_discard_partial(struct x9_gps_mnl_stream *);
/* One complete inner packet per call, after outer MCUDL validation. */
int x9_gps_mnl_receive(struct x9_gps_mnl_stream *, const void *, unsigned,
    uint64_t generation, uint64_t host_time);
/* Strict event2 BODY -> 105B PDS partial translation, no publication/READY.
 * Only measured degree latitude/longitude are currently qualified. All
 * time/accuracy/confidence/altitude/DOP/mode validity bits remain clear.
 * ENOENT is a valid no-fix grade0/1; ENOTSUP is another event/unknown grade.
 * Error leaves output untouched. Receiver/session/age must be qualified
 * separately before submitting to an explicitly capable backend. */
int x9_gps_mnl_position_wire(const void *body, unsigned body_bytes,
    void *wire, unsigned capacity);
#ifdef __cplusplus
}
#endif
#endif
