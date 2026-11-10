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
/* Actual MCU event IDs in libmnl's 7055a0 dispatch table. Event0 calls
 * the generic notification callback, NOT an OPEN/START result callback.
 * Event128 wakes the STOP waiter; event5 only reports MNL termination.
 * Neither carries a proved application transaction ID. */
enum x9_gps_mnl_control_event {
 X9_GPS_MNL_NOTIFICATION=0,
 X9_GPS_MNL_TERMINATED=5,
 X9_GPS_MNL_STOP_DONE=128
};
struct x9_gps_mnl_control_message {
 uint32_t event, declared_bytes, body_bytes;
 int32_t notification; /* signed16 only for event0; otherwise uninterpreted */
};
struct x9_gps_mnl_stream {
 uint64_t generation, first_time, last_time; /* physical/hardware epoch */
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
/* Metadata-only decoder of the three control events above. Event0 needs
 * declared2 and at least its two real bytes at BODY+4, NOT PVT's BODY+6.
 * Trailing bytes, and event5/128 payloads, are deliberately uninterpreted:
 * their schema is not established by vendor's unchecked dispatch handlers.
 * Success proves only that this notification was received. It NEVER proves
 * START/STOP, quiesced DMA, a matching request or receiver readiness.
 * Unknown events return ENOTSUP; every error leaves output untouched. */
int x9_gps_mnl_control_decode(const void *body, unsigned body_bytes,
    struct x9_gps_mnl_control_message *);
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
