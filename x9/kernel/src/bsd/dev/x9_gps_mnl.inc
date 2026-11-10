/* No power/firmware/EMI access or invented position/enable acknowledgements. */
void
x9_gps_mnl_discard_partial(struct x9_gps_mnl_stream *s)
{
 if (!s) return;
 s->used = 0; s->total = s->next = 0; s->first_time = 0;
}

void
x9_gps_mnl_stream_init(struct x9_gps_mnl_stream *s, uint64_t generation,
    int (*complete)(void *, const uint8_t *, unsigned, uint64_t), void *context)
{
 if (!s) return;
 bzero(s, sizeof(*s)); s->generation = generation;
 s->complete = complete; s->context = context;
}

int
x9_gps_mnl_receive(struct x9_gps_mnl_stream *s, const void *packet,
    unsigned length, uint64_t generation, uint64_t host_time)
{
 const uint8_t *p = packet;
 if (!s) return EINVAL;
 int error = 0;
 if (!generation || generation != s->generation) error = ESTALE;
 else if (!p || length <= 4 || length > X9_MNL_PACKET_MAX || !host_time ||
     host_time > mach_absolute_time() || host_time < s->last_time) error = EINVAL;
 else if (((unsigned)p[0] | (unsigned)p[1]<<8) != length ||
     !p[2] || !p[3] || p[3] > p[2]) error = EBADMSG;
 if (error) { x9_gps_mnl_discard_partial(s); s->bad_fragments++; return error; }
 s->last_time = host_time;
 if (s->first_time) {
  uint64_t age; absolutetime_to_nanoseconds(host_time-s->first_time, &age);
  if (age > X9_GPS_MAX_AGE_NS) {
   x9_gps_mnl_discard_partial(s); s->bad_fragments++;
  }
 }
 if (p[3] == 1) {
  if (s->total) s->bad_fragments++;
  x9_gps_mnl_discard_partial(s);
  s->total = p[2]; s->next = 1; s->first_time = host_time;
 }
 if (!s->total || p[2] != s->total || p[3] != s->next) error = EBADMSG;
 else if (length-4 > sizeof(s->message)-s->used) error = EOVERFLOW;
 if (error) { x9_gps_mnl_discard_partial(s); s->bad_fragments++; return error; }
 memcpy(s->message+s->used, p+4, length-4); s->used += length-4;
 if (p[3] != s->total) { s->next++; return 0; }
 /* Minimum is MCU event/length, not an AP LE32 command. Leave the event's
  * payload schema to its translator; never pass these bytes to PDS as-is. */
 if (s->used < 4) error = EBADMSG;
 else if (!s->complete) error = ENOTSUP;
 else error = s->complete(s->context, s->message, s->used, s->first_time);
 if (!error) s->messages++;
 x9_gps_mnl_discard_partial(s);
 return error;
}

int
x9_gps_mcudl_make_mnl_control(void *frame, unsigned capacity, uint8_t slot_seq,
    uint32_t command, const void *payload, unsigned length,
    unsigned *frame_bytes, unsigned *channel)
{
 if (!frame_bytes || !channel) return EINVAL;
 *frame_bytes = 0;
 if (!frame || (length && !payload) || length > X9_MNL_PACKET_MAX-12) return EINVAL;
 /* The vendor command bitmap is 0x000de3bf; command6 is routed separately.
  * Unknown commands are unsupported, not converted into a startup success. */
 if (command > 19 || (command != 6 && !(0x000de3bfu & (1u<<command)))) return ENOTSUP;
 unsigned inner = length ? 12+length : 8;
 if (capacity < inner+7) return EMSGSIZE;
 uint8_t *p = frame, *m = p+6;
 /* memmove first permits an input payload inside the output allocation. */
 if (length) memmove(m+12, payload, length);
 m[0] = (uint8_t)inner; m[1] = (uint8_t)(inner>>8); m[2] = m[3] = 1;
 for (unsigned i = 0; i < 4; i++) m[4+i] = (uint8_t)(command>>(8*i));
 if (length) for (unsigned i = 0; i < 4; i++) m[8+i] = (uint8_t)(length>>(8*i));
 p[0] = 0xa5; p[1] = slot_seq; p[2] = command == 6 ? 0xa3 : 0xa2;
 p[3] = (uint8_t)(inner>>8); p[4] = (uint8_t)inner;
 p[5] = (uint8_t)(p[1]+p[2]+p[3]+p[4]); p[6+inner] = 0xa3;
 *frame_bytes = inner+7; *channel = command == 6 ? 1 : 0;
 return 0;
}
