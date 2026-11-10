/* Current PLJ110 libmnl BuildID 0ebb7a655d4a8e040cdd60b0fffa2efd.
 * 707b10 reassembly copies fragment bodies AFTER a synthetic 4B header;
 * 7055d4 checks that synthetic length, 7055e0 reads event/length at +4,
 * and event2 at 7057bc copies 3168B from +10. Our assembler deliberately
 * does NOT add that synthetic header: event=+0, length=+2, PVT=+6.
 * Never interpret +10 relative to our callback's body.
 *
 * get_position 521c7c writes size at +2 and measured degree doubles at
 * +30/+38 (522188); debug consumer 529500 treats +19 >1 as a fix.
 * Only the documented 2D/3D grades 2/3 are accepted here. Cache-present,
 * management ACK and a finite coordinate alone are NOT fix qualification.
 *
 * get_debug529500 maps this PVT into its packed position debug structure;
 * mnld6fc40 prints that SAME structure with named units: +18 speed m/s,
 * +1c bearing degrees, +20 horizontal accuracy metres, +28 vertical
 * accuracy metres and +30 speed accuracy m/s. This proves PVT offsets
 * +50/+58/+84/+88/+8c, NOT +50/+54/+58 as an accuracy triplet. PDS
 * 1004f09ec/1004f0fc8 consumes these at +51/+59/+63/+79/+83.
 *
 * NMEA548e10 calls the original get_position into PVT at sp+3ec0;
 * GGA5496e8 prints +48 as sea-level height in metres and +40-minus-+48
 * as geoid separation (format17cc2). GSA549a7c/25f22 prints +5c/+60/+64
 * as PDOP/HDOP/VDOP; 6cae68/2357b independently names +60 as HDOP.
 * PDS1004f0ef4/1016f7b67 names +47 as alt(sl), and +92/+96/+100 as
 * PDOP/HDOP/VDOP. Do NOT equate MSL with WGS84: the MNL LLH path can
 * apply a configurable ellipsoid transform at 56fce4. +40 is withheld.
 * Heading522420 copies a cached course below 0.15m/s, not a new heading.
 *
 * This is still a PARTIAL native PDS translation. UTC/leap authority,
 * confidence, WGS84 datum and ellipses are not proved here. Only
 * independently qualified GPS calendar and named measured quantities are
 * included; uncertain/zero precision slots stay INVALID, not fabricated
 * measurements. Physical MNL session qualification and an explicitly
 * capable backend are still required. No confidence value is supplied:
 * Apple568810 uses circular uncertainty directly when confidence !=95,
 * rather than treating our value as a measured 95-percent ellipse.
 * No floating-point/SIMD, no coordinates in logs, no vendor code execution. */
#define GPS_PVT_BYTES 3168u
#define GPS_PVT_PREFIX 6u
#define GPS_PVT_LAT_LON_MASK 0x30u
static unsigned gps_pvt_u16(const uint8_t *p)
{ return (unsigned)p[0] | (unsigned)p[1]<<8; }
static uint32_t gps_pvt_u32(const uint8_t *p)
{ return GPS_LE32(p); }
#include "x9_gps_pvt_calendar.inc"
#include "x9_gps_pvt_scalar.inc"

/* Producer clears speed/precision in several unavailable-mode paths.
 * Keep zero inactive: this also conservatively omits stationary speed,
 * rather than pretending an unavailable measurement is a measured zero. */
static int gps_pvt_copy_positive(const uint8_t *p, unsigned src,
    uint8_t *result, unsigned dst)
{
 uint32_t v=gps_pvt_u32(p+src);
 if (!v || (v&0x80000000u) || (v&0x7f800000u)==0x7f800000u) return 0;
 memcpy(result+dst,p+src,4);
 return 1;
}

int x9_gps_mnl_position_wire(const void *body, unsigned length, void *wire,
    unsigned capacity)
{
 if (!body || !wire || capacity < X9_GPS_PDS_POSITION_BYTES) return EINVAL;
 const uint8_t *p=body;
 if (length < 4) return EBADMSG;
 if (gps_pvt_u16(p)!=2) return ENOTSUP;
 if (gps_pvt_u16(p+2)!=GPS_PVT_BYTES ||
     length!=GPS_PVT_PREFIX+GPS_PVT_BYTES) return EBADMSG;
 p+=GPS_PVT_PREFIX;
 if (gps_pvt_u16(p+2)!=GPS_PVT_BYTES) return EBADMSG;
 /* Grade0/1 is a valid no-fix report, not a failed MCU transaction. */
 if (p[0x19]<=1) return ENOENT;
 if (p[0x19]>3) return ENOTSUP;
 uint64_t lat=gps_le64(p+0x30)&0x7fffffffffffffffULL;
 uint64_t lon=gps_le64(p+0x38)&0x7fffffffffffffffULL;
 if (lat>0x4056800000000000ULL || lon>0x4066800000000000ULL) return ERANGE;
 /* Stage a complete result before modifying caller output. Input/output
  * may overlap; an error leaves the output untouched. Inactive slots are
  * zero solely for deterministic serialization, with validity bits clear. */
 uint8_t result[X9_GPS_PDS_POSITION_BYTES]={0};
 uint32_t mask=GPS_PVT_LAT_LON_MASK;
 memcpy(result+27,p+0x30,8); memcpy(result+35,p+0x38,8);
 if (gps_pvt_gps_calendar(p,result+4)) mask|=1u;
 if (gps_pvt_copy_positive(p,0x84,result,63)) mask|=1u<<11;
 if (gps_pvt_copy_positive(p,0x5c,result,92)) mask|=1u<<19;
 if (gps_pvt_copy_positive(p,0x60,result,96)) mask|=1u<<20;
 /* A 2D fix does not qualify vertical precision. A heading is only
  * supplied above the original producer's cached-course threshold. */
 if (p[0x19]==3) {
  if (gps_pvt_copy_positive(p,0x64,result,100)) mask|=1u<<21;
  if (gps_pvt_copy_positive(p,0x88,result,79)) {
   mask|=1u<<15;
   /* Vertical precision plus 3D grade qualifies measured MSL height,
    * including zero/negative heights. Failed conversion only omits height;
    * no guessed geoid, WGS84 height or fabricated uncertainty is supplied. */
   if (gps_pvt_copy_height(p+0x48,result+47)) mask|=1u<<7;
  }
 }
 if (gps_pvt_copy_positive(p,0x50,result,51)) {
  mask|=1u<<8;
  uint32_t heading=gps_pvt_u32(p+0x58);
  if (gps_pvt_u32(p+0x50)>=0x3e19999au && /* vendor's 0.15f m/s */
      (!(heading&0x80000000u) || !(heading&0x7fffffffu)) &&
      (heading&0x7fffffffu)<0x43b40000u) {
   memcpy(result+59,p+0x58,4); mask|=1u<<10;
  }
  if (gps_pvt_copy_positive(p,0x8c,result,83)) mask|=1u<<16;
 }
 for (unsigned i=0;i<4;i++) result[i]=(uint8_t)(mask>>(8*i));
 memcpy(wire,result,sizeof(result));
 return 0;
}
#undef GPS_PVT_BYTES
#undef GPS_PVT_PREFIX
#undef GPS_PVT_LAT_LON_MASK
