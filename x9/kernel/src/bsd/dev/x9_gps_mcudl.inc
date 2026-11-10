/* Reimplemented from the documented OPPO data_intf packet format. Bounded
 * incremental decoding; no Linux dependency or copied parser/global buffer.
 * HEADER: A5, slot/seq, type, length BE16, sum(header[1..4]) modulo 256.
 * TAIL: A3. There is no payload checksum in this envelope; NMEA must pass its
 * own checksum as well. Header seq includes slot bits: NOT a monotonic ID. */
#include "x9_gps_mcudl.h"
#include "x9_gps_mnl.inc"

void
x9_gps_mcudl_stream_init(struct x9_gps_mcudl_stream *s, uint64_t generation,
    int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t), void *context)
{
 if (!s) return;
 bzero(s, sizeof(*s)); s->generation = generation;
 s->control = control; s->context = context;
}

void
x9_gps_mcudl_stream_init_mnl(struct x9_gps_mcudl_stream *s, uint64_t generation,
    int (*control)(void *, uint8_t, const uint8_t *, unsigned, uint64_t),
    int (*mnl)(void *, const uint8_t *, unsigned, uint64_t), void *context)
{
 if (!s) return;
 x9_gps_mcudl_stream_init(s, generation, control, context);
 x9_gps_mnl_stream_init(&s->mnl, generation, mnl, context);
}

static void
mcudl_discard(struct x9_gps_mcudl_stream *s, unsigned n)
{
 s->used -= n;
 if (s->used) memmove(s->frame, s->frame+n, s->used);
}

static void
mcudl_clear_partial(struct x9_gps_mcudl_stream *s)
{
 s->discarded_bytes += s->used;
 s->used = s->sentence_used = 0;
 s->sentence_time = s->fragment_time = 0;
 s->frame_application_generation = s->sentence_application_generation = 0;
 s->mnl_application_generation = 0;
 x9_gps_mnl_discard_partial(&s->mnl);
}

static int
mcudl_nmea(struct x9_gps_mcudl_stream *s, const uint8_t *p, unsigned n,
    uint64_t host_time)
{
 if(s->sentence_used && s->sentence_application_generation !=
     s->frame_application_generation) {
  s->sentence_used = 0; s->sentence_time = 0;
  s->sentence_application_generation = 0;
 }
 for (unsigned i = 0; i < n; i++) {
  uint8_t ch = p[i];
  if (ch == '$') {
   if (s->sentence_used) s->bad_sentences++;
   s->sentence_used = 0; s->sentence_time = host_time;
   s->sentence_application_generation = s->frame_application_generation;
  } else if (!s->sentence_used) continue;
  if (ch == '\n') {
   unsigned len = s->sentence_used;
   /* Normalize receiver LF/CRLF to a complete sentence without terminator;
    * the existing ingress verifies talker/type/ASCII and its exact checksum. */
   if (len && s->sentence[len-1] == '\r') len--;
   int error = x9_gps_deliver_nmea(s->sentence_application_generation,
       s->sentence, len, s->sentence_time);
   if (!error) s->sentences++; else s->bad_sentences++;
   s->sentence_used = 0; s->sentence_time = 0;
   s->sentence_application_generation = 0;
   /* A concurrent application STOP/START retires this sentence, not the
    * still-owned transport. A hardware fault must still stop the stream. */
   if (error == ESTALE || error == ENXIO) {
    struct x9_gps_status status;x9_gps_copy_status(&status);
    if(status.hardware_generation != s->generation || status.fault)
     return status.fault ? ENXIO : ESTALE;
   }
   continue;
  }
  if ((ch < 0x20 && ch != '\r') || ch > 0x7e ||
      s->sentence_used == sizeof(s->sentence)) {
   s->bad_sentences++; s->sentence_used = 0; s->sentence_time = 0;
   continue;
  }
  s->sentence[s->sentence_used++] = ch;
 }
 return 0;
}

/* Returns only control-handler failures, not a checksum error that has been
 * resynchronized. A false frame must invalidate any half NMEA it straddles. */
static int
mcudl_parse(struct x9_gps_mcudl_stream *s, uint64_t host_time)
{
 while (s->used) {
  if (s->frame[0] != 0xa5) {
   s->bad_frames++; s->sentence_used = 0;
   x9_gps_mnl_discard_partial(&s->mnl);
   s->discarded_bytes++; mcudl_discard(s, 1); continue;
  }
  if (s->used < 6) break;
  unsigned payload = ((unsigned)s->frame[3] << 8) | s->frame[4];
  uint8_t checksum = (uint8_t)(s->frame[1] + s->frame[2] + s->frame[3] + s->frame[4]);
  if (checksum != s->frame[5] || payload > X9_MCUDL_PAYLOAD_MAX) {
   s->bad_frames++; s->sentence_used = 0;
   x9_gps_mnl_discard_partial(&s->mnl);
   s->discarded_bytes++; mcudl_discard(s, 1); continue;
  }
  unsigned frame_bytes = 6 + payload + 1;
  if (s->used < frame_bytes) break;
  if (s->frame[frame_bytes-1] != 0xa3) {
   s->bad_frames++; s->sentence_used = 0;
   x9_gps_mnl_discard_partial(&s->mnl);
   s->discarded_bytes++; mcudl_discard(s, 1); continue;
  }
  /* Application ACKs may change tracking without changing the physical
   * owner. Hardware faults still revoke controls within this receive. */
  struct x9_gps_status status; x9_gps_copy_status(&status);
  if (status.hardware_generation != s->generation || status.fault) {
   mcudl_clear_partial(s); return status.fault ? ENXIO : ESTALE;
  }
  s->frames++;
  /* Actual v062 parser updates proc bytes/pkt count BEFORE dispatch. An
   * unsupported application handler does not turn a valid envelope into
   * parse-drop bytes; retire that frame before clearing remaining partials. */
  s->processed_bytes += frame_bytes;
  if (s->frame[2] == X9_MCUDL_NMEA) {
   if (status.ready && status.tracking && s->frame_application_generation &&
       status.generation == s->frame_application_generation) {
    int error = mcudl_nmea(s, s->frame+6, payload, host_time);
    if (error) {
     mcudl_discard(s, frame_bytes); mcudl_clear_partial(s); return error;
    }
   } else {
    s->sentence_used = 0; s->sentence_time = 0;
    s->sentence_application_generation = 0;
   }
  }
  else {
   s->non_nmea++;
   /* Never silently discard firmware/system/flow-control transactions. */
   int error;
   if (s->frame[2] == 0xa2 && s->mnl.complete) {
    /* Preserve the first INNER fragment's epoch through its callback.
     * Mixed epochs cannot produce a current output, but system/application
     * replies still reach their protocol handler (never invent an ACK). */
    if(payload>4 && s->frame[9]==1)
     s->mnl_application_generation=s->frame_application_generation;
    else if(s->mnl_application_generation!=s->frame_application_generation)
     s->mnl_application_generation=0;
    error = x9_gps_mnl_receive(&s->mnl, s->frame+6, payload, s->generation, host_time);
    if(!s->mnl.total)s->mnl_application_generation=0;
   }
   else if (s->control)
    error = s->control(s->context, s->frame[2], s->frame+6, payload, host_time);
   else error = ENOTSUP;
   if (error) {
    mcudl_discard(s, frame_bytes); mcudl_clear_partial(s); return error;
   }
   x9_gps_copy_status(&status);
   if (status.hardware_generation != s->generation || status.fault) {
    mcudl_discard(s, frame_bytes); mcudl_clear_partial(s);
    return status.fault ? ENXIO : ESTALE;
   }
  }
  mcudl_discard(s, frame_bytes);
 }
 return 0;
}

int
x9_gps_mcudl_receive(struct x9_gps_mcudl_stream *s, const void *bytes,
    unsigned length, uint64_t host_time)
{
 struct x9_gps_status status;x9_gps_copy_status(&status);
 return x9_gps_mcudl_receive_epoch(s,bytes,length,host_time,
     status.ready && status.tracking ? status.generation : 0);
}

int
x9_gps_mcudl_receive_epoch(struct x9_gps_mcudl_stream *s, const void *bytes,
    unsigned length, uint64_t host_time, uint64_t application_generation)
{
 const uint8_t *p = bytes;
 if (!s) return EINVAL;
 if (!p || !length || length > 65536 || !host_time ||
     host_time > mach_absolute_time() || host_time < s->last_host_time) {
  mcudl_clear_partial(s); return EINVAL;
 }
 struct x9_gps_status status; x9_gps_copy_status(&status);
 if (!s->generation || s->generation != status.hardware_generation || status.fault) {
  mcudl_clear_partial(s); return status.fault ? ENXIO : ESTALE;
 }
 /* Firmware/system replies are needed to establish READY in the first
  * place. Do not gate the control plane on the location tracking lease. */
 s->last_host_time = host_time;
 uint64_t age;
 if (s->fragment_time) {
  absolutetime_to_nanoseconds(host_time-s->fragment_time, &age);
  if (age > X9_GPS_MAX_AGE_NS) { mcudl_clear_partial(s); s->bad_frames++; }
 }
 if (s->sentence_used) {
  absolutetime_to_nanoseconds(host_time-s->sentence_time, &age);
  if (age > X9_GPS_MAX_AGE_NS) { s->sentence_used = 0; s->bad_sentences++; }
 }
 for (unsigned i = 0; i < length; i++) {
  /* One byte at a time bounds capacity even for coalesced/wrapped rings;
   * parse consumes or waits for at most FRAME_MAX bytes. */
  if (!s->used) {
   s->fragment_time = host_time;
   s->frame_application_generation = application_generation;
  } else if(s->frame_application_generation != application_generation)
   s->frame_application_generation = 0; /* old/mixed descriptor bytes */
  if (s->used >= sizeof(s->frame) || s->received_bytes==UINT64_MAX) {
   mcudl_clear_partial(s); return EOVERFLOW;
  }
  s->frame[s->used++] = p[i]; s->received_bytes++;
  int error = mcudl_parse(s, s->fragment_time);
  if (error) return error;
 }
 if (!s->used) s->fragment_time = 0;
 return 0;
}
