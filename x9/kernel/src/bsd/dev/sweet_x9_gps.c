/* OPPO MT6993 uses gps_mcudl + MNL firmware, NOT serial.c/mtk.c.
 * This boundary accepts complete receiver NMEA AFTER MCU transport decoding.
 * The physical worker requires a real connv2 resource/image/protocol supplier;
 * no default power ownership or receiver readiness. Never feed an MCU payload
 * directly as a position fix. */
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <pexpert/device_tree.h>
#include "x9_gps.h"
#include "x9_gps_runtime.h"
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
static struct x9_gps_handoff gps_layout;
static void gps_runtime_start(void);
static void gps_copy_layout(void)
{
 DTEntry chosen; void *p = NULL; unsigned int size = 0;
 struct x9_gps_handoff h;
 if (SecureDTLookupEntry(NULL,"/chosen",&chosen) != kSuccess ||
     SecureDTGetProperty(chosen,"x9-gps-handoff",&p,&size) != kSuccess ||
     !p || size != sizeof(h)) return;
 memcpy(&h,p,sizeof(h));
 if (h.version != 1 || !(h.flags & X9_GPS_LAYOUT_VERIFIED) ||
     (h.flags & ~(X9_GPS_LAYOUT_VERIFIED|X9_GPS_POWER_LAYOUT_VERIFIED|
         X9_GPS_POS_LAYOUT_VERIFIED|X9_GPS_RF_LAYOUT_VERIFIED|
         X9_GPS_LP_LAYOUT_VERIFIED|X9_GPS_CLOCK_LAYOUT_VERIFIED|
         X9_GPS_SMPU_LAYOUT_VERIFIED)) || h.reserved ||
     h.reserve_base < 0x80000000 || h.reserve_base > 0xf0000000 ||
     (h.reserve_base & 0xffff) || h.reserve_size != 0xa00000 ||
     h.reserve_size > 0xf0000000-h.reserve_base || h.layout_size != 0x630000 ||
     h.legacy_offset != 0x4f0000 || h.legacy_size != 0x78000 ||
     h.ap2mcu_offset != 0x628000 || h.mcu2ap_offset != 0x62c000 ||
     h.region_size != 0x4000 || h.data_offset != 0x70 || h.data_size != 0x1000 ||
     h.mcu_bus_base != 0x70000000 || h.ccif_base != 0x4003d000 || h.ccif_size != 0x100) return;
 gps_layout=h; st.layout_valid=1;
 st.power_layout_valid=!!(h.flags & X9_GPS_POWER_LAYOUT_VERIFIED);
}
int x9_gps_copy_handoff(struct x9_gps_handoff *out)
{
 if (!out) return EINVAL;
 lck_mtx_lock(&gps_lock);
 int error=!active ? ENXIO : !st.layout_valid ? EAGAIN : 0;
 if (!error) *out=gps_layout;
 lck_mtx_unlock(&gps_lock); return error;
}

void x9_gps_init(void)
{
 int start = 0;
 lck_mtx_lock(&gps_control); lck_mtx_lock(&gps_lock);
 if (!active && sweet_early_is_x9() && sweet_x9_arg("x9_gps", (sweet_x9_arg("x9s", 0)&4) ? 1 : 0) == 1) {
  active = 1; st.generation = st.hardware_generation = 1; st.interval_ms = 1000;
  gps_copy_layout();
  if (st.layout_valid) {
   st.physical_stage = X9_GPS_PHYSICAL_WAIT_CONNV2;
   st.physical_errno = ENXIO; start = 1;
  }
  printf("X9 GPS: consys HIF layout %s; physical receiver remains unavailable\n",
      st.layout_valid ? "qualified" : "pending");
 }
 lck_mtx_unlock(&gps_lock); lck_mtx_unlock(&gps_control);
 if (start) gps_runtime_start();
}
int x9_gps_register_backend(const struct x9_gps_backend *p)
{
 if (!p || (p->version != 1 && p->version != 2) || !p->set_state ||
     (p->version == 2 && p->position_supported > 1)) return EINVAL;
 lck_mtx_lock(&gps_control); lck_mtx_lock(&gps_lock);
 int e = !active ? ENXIO : st.fault ? EIO : hw.version ? EBUSY : 0;
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
static void gps_fault_locked(int error)
{
 /* First fault revokes both epochs once. Preserve the earliest uncertainty;
  * a second HIF/worker/PDS fault cannot hide it or create another owner. */
 if (!st.fault) {
  ++st.generation; ++st.hardware_generation;
  st.last_errno = error ? error : EIO;
 }
 st.fault = 1; st.ready = st.enabled = st.tracking = 0;
 head = count = position_head = position_count = 0;
}
void x9_gps_fault(uint64_t generation, int error)
{
 lck_mtx_lock(&gps_lock);
 if (generation && generation == st.generation) gps_fault_locked(error);
 lck_mtx_unlock(&gps_lock);
}
void x9_gps_hardware_fault(uint64_t generation, int error)
{
 lck_mtx_lock(&gps_lock);
 if (generation && generation == st.hardware_generation) gps_fault_locked(error);
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
  gps_fault_locked(e);
 }
 if (!st.fault) st.last_errno = e;
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
#include "x9_gps_pvt.inc"
/* Validate IEEE wire bits without enabling FP/SIMD in kernel context. This
 * function does not invent uncertainty, infer a fix from GGA or convert units.
 * Inactive fields may be zero; all numeric slots must still be finite. */
static int gps_position_valid(const uint8_t *p, unsigned n)
{
 if (!p || n != X9_GPS_PDS_POSITION_BYTES) return EINVAL;
 uint32_t mask=GPS_LE32(p);
 /* PDS defines bits0..22. A parsed position must actually contain BOTH
  * coordinates; an empty/unknown mask is not a measured receiver fix. */
 if ((mask&~0x007fffffu) || (mask&0x30u)!=0x30u) return EBADMSG;
 uint64_t lat = gps_le64(p+27) & 0x7fffffffffffffffULL;
 uint64_t lon = gps_le64(p+35) & 0x7fffffffffffffffULL;
 /* IEEE754 magnitudes of 90.0 and 180.0 degrees. */
 if (lat > 0x4056800000000000ULL || lon > 0x4066800000000000ULL) return ERANGE;
 for (unsigned i = 43; i <= 87; i += 4)
  if ((GPS_LE32(p+i) & 0x7f800000u) == 0x7f800000u) return ERANGE;
 for (unsigned i = 92; i <= 100; i += 4)
  if ((GPS_LE32(p+i) & 0x7f800000u) == 0x7f800000u) return ERANGE;
 /* Validate active unsigned-magnitude quantities without any kernel FP.
  * Vertical speed and altitudes may be negative; they are not in this set.
  * Inactive zero placeholders MUST NOT cause validity bits to be inferred. */
 const uint8_t nonnegative[]={8,11,12,13,15,16,17};
 for(unsigned j=0;j<sizeof(nonnegative);j++) {
  unsigned bit=nonnegative[j]; uint32_t v=GPS_LE32(p+43+4*(bit-6));
  if((mask&(1u<<bit)) && (v&0x80000000u) && (v&0x7fffffffu))return ERANGE;
 }
 for(unsigned bit=10;bit<=14;bit+=4)if(mask&(1u<<bit)) {
  uint32_t v=GPS_LE32(p+43+4*(bit-6));
  if(((v&0x80000000u) && (v&0x7fffffffu)) ||
      (v&0x7fffffffu)>=0x43b40000u)return ERANGE; /* [0,360) degrees */
 }
 for(unsigned bit=19;bit<=21;bit++)if(mask&(1u<<bit)) {
  uint32_t v=GPS_LE32(p+92+4*(bit-19));
  if((v&0x80000000u) || !v)return ERANGE; /* positive DOP */
 }
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
#include "x9_gps_hif.inc"
#include "x9_gps_runtime.inc"
