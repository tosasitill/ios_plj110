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
 * This is deliberately a PARTIAL native PDS translation: only measured
 * latitude/longitude. Unproven accuracy/confidence/time/altitude semantics
 * remain INVALID, not zero-valued valid measurements. Physical MNL session
 * qualification and an explicitly capable backend are still required.
 * No floating-point/SIMD, no coordinates in logs, no vendor code execution. */
#define GPS_PVT_BYTES 3168u
#define GPS_PVT_PREFIX 6u
#define GPS_PVT_LAT_LON_MASK 0x30u
static unsigned gps_pvt_u16(const uint8_t *p)
{ return (unsigned)p[0] | (unsigned)p[1]<<8; }
static uint32_t gps_pvt_u32(const uint8_t *p)
{ return GPS_LE32(p); }

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
 result[0]=GPS_PVT_LAT_LON_MASK;
 memcpy(result+27,p+0x30,8); memcpy(result+35,p+0x38,8);
 memcpy(wire,result,sizeof(result));
 return 0;
}
#undef GPS_PVT_BYTES
#undef GPS_PVT_PREFIX
#undef GPS_PVT_LAT_LON_MASK
