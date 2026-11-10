/* Vendor libmnl inner fragmentation, not the A5 MCUDL envelope or PDS wire.
 * A stream belongs to one deferred transport worker; allocate it off stack.
 * Recovered from libmnl BuildID 0ebb7a655d4a8e040cdd60b0fffa2efd,
 * mtk_gps_ofl_ap_pack_data (0x707a00) and ap2mcu_ofl_sender (0x70661c). */
#ifndef X9_GPS_MNL_H
#define X9_GPS_MNL_H
#include <stdint.h>
#define X9_MNL_PACKET_MAX 1536u
#define X9_MNL_FRAGMENT_MAX (X9_MNL_PACKET_MAX - 4u)
/* AP sender commands, distinct from the MCU event IDs below. The original
 * init call copies a 1256B host driver object, but sends ONLY its 216B
 * prefix. Neither a host object nor MNL_Config.xml is an MCU wire payload. */
enum x9_gps_mnl_command {
 X9_GPS_MNL_OPEN=0, X9_GPS_MNL_STOP=1,
 X9_GPS_MNL_INIT_CONFIG=2, X9_GPS_MNL_DRIVER_CONFIG=3,
 X9_GPS_MNL_AGENT=6
};
#define X9_MNL_INIT_CONFIG_BYTES 136u
#define X9_MNL_DRIVER_CONFIG_BYTES 216u
/* Three DIFFERENT representations of the same qualified clock schematic.
 * driver_flag is the Linux IOCTL11 value, NOT the MNL init+0x13 byte.
 * This describes configuration only, never a running clock/power lease. */
struct x9_gps_mnl_clock {
 uint32_t nominal_hz, driver_flag, receiver_source;
};
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
/* Validate the AP command's known wire shape before serialization/reservation.
 * OPEN/STOP have no payload. INIT_CONFIG requires the original 136B object
 * with offload enabled at +0x68. DRIVER_CONFIG requires the original 216B
 * prefix and its observed +0xd0..d7 extension metadata (210/6/aa55/0102).
 * Other supported sender commands retain their bounded opaque payloads.
 * This does NOT validate the remaining board configuration, calibration,
 * file descriptors, pointers, NV authority, session or receiver readiness.
 * A caller must supply wire bytes from a qualified native producer, NEVER
 * copy an Android process object, synthesize metadata or send an XML file. */
int x9_gps_mnl_command_validate(uint32_t command, const void *payload,
    unsigned payload_bytes);
/* Original gps_drv_dl_v062.ko62b0 maps schematic0/1/2 to driver flags
 * 21/51/00; mnld6db10..6de34 maps those to MNL sourcefe/fe/ff and
 * nominal26M/52M/26M. This is NOT a calibrated input frequency: mnld
 * imports AP NV+0 into init+18 before applying this source, and ONLY
 * driver51 subsequently overwrites init+18 with52M. The26M paths retain
 * their existing frequency. Schematic3 (52M EXTCXO) is deliberately ENOTSUP:
 * the original driver's default maps it to26M CoTMS, not its real clock.
 * No enum-as-Hz/flag-as-source conversion, and no unknown fallback.
 * Errors leave output untouched. This alone cannot qualify board inputs. */
int x9_gps_mnl_clock_profile(unsigned schematic, struct x9_gps_mnl_clock *);
/* Strict event2 BODY -> 105B PDS partial translation, no publication/READY.
 * Qualified fields include degree lat/lon, independent GPS calendar,
 * circular horizontal/vertical uncertainty, speed/speed uncertainty,
 * non-cached course, MSL height and DOP. Vertical fields require grade3.
 * UTC/leap authority, WGS84 height, confidence/ellipse, vertical speed and
 * mode remain invalid; fields without their own evidence are omitted.
 * ENOENT is a valid no-fix grade0/1; ENOTSUP is another event/unknown grade.
 * Error leaves output untouched. Receiver/session/age must be qualified
 * separately before submitting to an explicitly capable backend. */
int x9_gps_mnl_position_wire(const void *body, unsigned body_bytes,
    void *wire, unsigned capacity);
#ifdef __cplusplus
}
#endif
#endif
