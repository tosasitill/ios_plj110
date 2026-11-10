/* OPPO MT6993 uses gps_mcudl + MNL firmware, NOT serial.c/mtk.c.
 * This boundary accepts complete receiver NMEA AFTER MCU transport decoding.
 * gps_mcudl / conninfra power, firmware, EMI, mailbox and MNL framing are not
 * implemented here. Never feed an MCU payload directly as a position fix. */
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include "x9_gps.h"
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
static LCK_GRP_DECLARE(gps_group, "x9-gps");
static LCK_MTX_DECLARE(gps_lock, &gps_group);
static LCK_MTX_DECLARE(gps_control, &gps_group);
static struct x9_gps_status st;
static struct x9_gps_backend hw;
static struct x9_gps_nmea ring[X9_GPS_RING];
static unsigned head, count;
static struct x9_gps_position positions[X9_GPS_POSITION_RING];
static unsigned position_head, position_count;
static int active;

void x9_gps_init(void)
{
 lck_mtx_lock(&gps_control); lck_mtx_lock(&gps_lock);
 if (!active && sweet_early_is_x9() && sweet_x9_arg("x9_gps", (sweet_x9_arg("x9s", 0)&4) ? 1 : 0) == 1) {
  active = 1; st.generation = 1; st.interval_ms = 1000;
 }
 lck_mtx_unlock(&gps_lock); lck_mtx_unlock(&gps_control);
}
int x9_gps_register_backend(const struct x9_gps_backend *p)
{
 if (!p || (p->version != 1 && p->version != 2) || !p->set_state ||
     (p->version == 2 && p->position_supported > 1)) return EINVAL;
 lck_mtx_lock(&gps_control); lck_mtx_lock(&gps_lock);
 int e = !active ? ENXIO : hw.version ? EBUSY : 0;
 if (!e) {
  /* Do not read the new tail of a version-1 descriptor. */
  hw.version = p->version; hw.set_state = p->set_state;
  hw.position_supported = p->version == 2 ? p->position_supported : 0;
  st.position_supported = hw.position_supported;
  st.ready = 1; st.fault = 0; st.last_errno = 0;
 }
 lck_mtx_unlock(&gps_lock); lck_mtx_unlock(&gps_control);
 return e;
}
void x9_gps_copy_status(struct x9_gps_status *out)
{
 if (!out) return;
 lck_mtx_lock(&gps_lock); *out = st; lck_mtx_unlock(&gps_lock);
}
void x9_gps_fault(uint64_t generation, int error)
{
 lck_mtx_lock(&gps_lock);
 if (generation == st.generation) {
  st.fault = 1; st.ready = st.enabled = st.tracking = 0; st.last_errno = error ? error : EIO;
  ++st.generation; head = count = 0;
  position_head = position_count = 0;
 }
 lck_mtx_unlock(&gps_lock);
}
int x9_gps_set_state(int enable, int tracking, uint32_t interval_ms)
{
 if ((enable != 0 && enable != 1) || (tracking != 0 && tracking != 1) || tracking > enable || interval_ms < 1000 || interval_ms > 3600000)
  return EINVAL;
 lck_mtx_lock(&gps_control); lck_mtx_lock(&gps_lock);
 int e = !active ? ENXIO : st.fault ? EIO : 0;
 if (!e && st.enabled == (uint32_t)enable && st.tracking == (uint32_t)tracking && (!tracking || st.interval_ms == interval_ms))
  goto done;
 if (!e && !st.ready) e = ENXIO;
 if (e) goto done;
 uint64_t generation = ++st.generation;
 st.enabled = st.tracking = 0; head = count = 0;
 position_head = position_count = 0;
 lck_mtx_unlock(&gps_lock);
 e = hw.set_state(enable, tracking, interval_ms, generation);
 lck_mtx_lock(&gps_lock);
 if (st.generation != generation || st.fault) e = EIO;
 if (!e) { st.enabled = (uint32_t)enable; st.tracking = (uint32_t)tracking; st.interval_ms = interval_ms; }
 else {
  /* Uncertain transition quarantines output; no success ACK to iOS. */
  st.ready = st.enabled = st.tracking = 0; st.fault = 1; ++st.generation;
 }
 st.last_errno = e;
done:
 lck_mtx_unlock(&gps_lock); lck_mtx_unlock(&gps_control);
 return e;
}
static int hex(uint8_t c)
{
 return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
     c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
int x9_gps_deliver_nmea(uint64_t generation, const void *sentence,
    unsigned length, uint64_t host_time)
{
 const uint8_t *p = sentence;
 if (!p || length < 10 || length > X9_GPS_NMEA_MAX || !host_time ||
     host_time > mach_absolute_time()) return EINVAL;
 /* Checksum must terminate the sentence (optional CRLF only). Proprietary
  * PMTK/P commands, concatenated sentences and non-ASCII are not position data. */
 unsigned n = length;
 if (n >= 2 && p[n-2] == '\r' && p[n-1] == '\n') n -= 2;
 if (n < 10 || p[0] != '$' || p[6] != ',' || p[n-3] != '*') return EINVAL;
 const char *talkers[] = {"GP", "GN", "GL", "GA", "GB", "BD", "GQ", "GI"};
 const char *types[] = {"RMC", "GGA", "GSA", "GSV", "GST", "VTG", "ZDA", "GNS"};
 int talker = 0, type = 0;
 for (unsigned i = 0; i < sizeof(talkers)/sizeof(talkers[0]); i++)
  if (!memcmp(p+1, talkers[i], 2)) talker = 1;
 for (unsigned i = 0; i < sizeof(types)/sizeof(types[0]); i++)
  if (!memcmp(p+3, types[i], 3)) type = 1;
 if (!talker || !type) return ENOTSUP;
 uint8_t checksum = 0;
 for (unsigned i = 1; i < n-3; i++) {
  if (p[i] < 0x20 || p[i] > 0x7e || p[i] == '*' || p[i] == '$') return EINVAL;
  checksum ^= p[i];
 }
 int hi = hex(p[n-2]), lo = hex(p[n-1]);
 if (hi < 0 || lo < 0 || checksum != (uint8_t)(hi*16+lo)) return EBADMSG;
 lck_mtx_lock(&gps_lock);
 int e = !st.ready || !st.tracking ? ENXIO : generation != st.generation ? ESTALE : 0;
 if (!e && count && host_time < ring[(head+count-1)%X9_GPS_RING].host_time) e = EINVAL;
 if (!e) {
  if (count == X9_GPS_RING) { head = (head+1)%X9_GPS_RING; count--; st.dropped++; }
  struct x9_gps_nmea *r = &ring[(head+count)%X9_GPS_RING];
  bzero(r, sizeof(*r)); r->sequence = ++st.sequence; r->generation = generation;
  r->host_time = host_time; r->length = length; memcpy(r->data, p, length); count++;
 }
 lck_mtx_unlock(&gps_lock);
 return e;
}
int x9_gps_next_nmea(uint64_t *cursor, struct x9_gps_nmea *out)
{
 if (!cursor || !out) return EINVAL;
 int e = ENOENT;
 lck_mtx_lock(&gps_lock);
 if (!st.ready || !st.tracking) e = ENXIO;
 else for (unsigned i = 0; i < count; i++) {
  struct x9_gps_nmea *r = &ring[(head+i)%X9_GPS_RING];
  if (r->sequence <= *cursor) continue;
  *cursor = r->sequence;
  uint64_t age;
  absolutetime_to_nanoseconds(mach_absolute_time()-r->host_time, &age);
  if (r->generation != st.generation || age > X9_GPS_MAX_AGE_NS) continue;
  *out = *r; e = 0; break;
 }
 lck_mtx_unlock(&gps_lock);
 return e;
}

#define GPS_LE32(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1]<<8 | (uint32_t)(p)[2]<<16 | (uint32_t)(p)[3]<<24)
static uint64_t gps_le64(const uint8_t *p)
{
 return GPS_LE32(p) | (uint64_t)GPS_LE32(p+4)<<32;
}
/* Validate IEEE wire bits without enabling FP/SIMD in kernel context. This
 * function does not invent uncertainty, infer a fix from GGA or convert units.
 * Inactive fields may be zero; all numeric slots must still be finite. */
static int gps_position_valid(const uint8_t *p, unsigned n)
{
 if (!p || n != X9_GPS_PDS_POSITION_BYTES) return EINVAL;
 uint64_t lat = gps_le64(p+27) & 0x7fffffffffffffffULL;
 uint64_t lon = gps_le64(p+35) & 0x7fffffffffffffffULL;
 /* IEEE754 magnitudes of 90.0 and 180.0 degrees. */
 if (lat > 0x4056800000000000ULL || lon > 0x4066800000000000ULL) return ERANGE;
 for (unsigned i = 43; i <= 87; i += 4)
  if ((GPS_LE32(p+i) & 0x7f800000u) == 0x7f800000u) return ERANGE;
 for (unsigned i = 92; i <= 100; i += 4)
  if ((GPS_LE32(p+i) & 0x7f800000u) == 0x7f800000u) return ERANGE;
 /* Confidence is a percentage; operation mode 0 is standalone. Assistance
  * remains unsupported rather than silently labelling a fix MS-based. */
 if (p[91] > 100 || p[104] != 0) return ENOTSUP;
 return 0;
}
int x9_gps_deliver_position(uint64_t generation, const void *wire,
    unsigned length, uint64_t host_time)
{
 int error = gps_position_valid(wire, length);
 if (error) return error;
 if (!host_time || host_time > mach_absolute_time()) return EINVAL;
 lck_mtx_lock(&gps_lock);
 error = !st.ready || !st.tracking ? ENXIO : generation != st.generation ? ESTALE :
     !st.position_supported ? ENOTSUP : 0;
 if (!error && position_count && host_time <
     positions[(position_head+position_count-1)%X9_GPS_POSITION_RING].host_time) error = EINVAL;
 if (!error) {
  if (position_count == X9_GPS_POSITION_RING) {
   position_head = (position_head+1)%X9_GPS_POSITION_RING; position_count--; st.dropped++;
  }
  struct x9_gps_position *r = &positions[(position_head+position_count)%X9_GPS_POSITION_RING];
  r->sequence = ++st.position_sequence; r->generation = generation; r->host_time = host_time;
  memcpy(r->data, wire, sizeof(r->data)); position_count++;
 }
 lck_mtx_unlock(&gps_lock); return error;
}
int x9_gps_next_position(uint64_t *cursor, struct x9_gps_position *out)
{
 if (!cursor || !out) return EINVAL;
 int error = ENOENT;
 lck_mtx_lock(&gps_lock);
 if (!st.ready || !st.tracking) error = ENXIO;
 else for (unsigned i = 0; i < position_count; i++) {
  struct x9_gps_position *r = &positions[(position_head+i)%X9_GPS_POSITION_RING];
  if (r->sequence <= *cursor) continue;
  *cursor = r->sequence;
  uint64_t age; absolutetime_to_nanoseconds(mach_absolute_time()-r->host_time, &age);
  if (r->generation != st.generation || age > X9_GPS_MAX_AGE_NS) continue;
  *out = *r; error = 0; break;
 }
 lck_mtx_unlock(&gps_lock); return error;
}
#undef GPS_LE32

#include "x9_gps_mcudl.inc"
