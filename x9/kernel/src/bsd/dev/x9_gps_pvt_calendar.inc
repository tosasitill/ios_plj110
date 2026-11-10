/* PLJ110 get_position521e34 -> gps_to_unix6d8484 -> gmtime56b704.
 * PVT+4 is tm_year (since1900), +6 is tm_mon (zero-based), +7..a
 * are day/hour/minute/second; +c is fractional milliseconds. These are
 * UTC, NOT the GPS calendar which PDS bit0 describes (20H3921004f0fc8).
 * Build GPS calendar from the receiver's positive signed week+12 and
 * integral seconds-of-week+14, retaining its measured fraction. A second
 * calendar, derived using the RECEIVED offset+10, must match the original
 * UTC tm fields. No host wall/arrival time, timezone or compiled leap table.
 *
 * This consistency check does NOT prove that the MCU's leap/UTC authority
 * is current: 5e2bcc can return a host/config-derived offset. Consequently
 * do NOT grant PDS UTC/leap bits1/2, or trigger Apple's system-time update
 * (10056c8e4 requires bits0..2). The application provider must still prove
 * session and measurement age before any of this data is published. */
static int gps_pvt_leap_year(unsigned year)
{ return !(year%4) && ((year%100) || !(year%400)); }

static unsigned gps_pvt_month_days(unsigned year, unsigned month)
{
 static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
 if (!month || month>12) return 0;
 return days[month-1]+(month==2 && gps_pvt_leap_year(year));
}

/* Internal bounded calendar, not a general wall-clock API. The current
 * vendor converter rejects UTC beyond UINT32_MAX seconds; stay within
 * that proven range rather than accepting its signed-week/w32 wrap. */
static int gps_pvt_calendar(uint64_t seconds, unsigned millis, uint8_t *out)
{
 if (seconds<315964800ULL || seconds>0xffffffffULL || millis>=1000) return 0;
 uint32_t days=(uint32_t)(seconds/86400);
 unsigned wday=(days+4)%7, year=1970, month=1;
 while (days>=365u+(unsigned)gps_pvt_leap_year(year)) {
  days-=365u+(unsigned)gps_pvt_leap_year(year); year++;
 }
 while (days>=gps_pvt_month_days(year,month)) {
  days-=gps_pvt_month_days(year,month); month++;
 }
 unsigned tod=(unsigned)(seconds%86400);
 out[0]=(uint8_t)year; out[1]=(uint8_t)(year>>8);
 out[2]=(uint8_t)month; out[3]=(uint8_t)wday; out[4]=(uint8_t)(days+1);
 out[5]=(uint8_t)(tod/3600); out[6]=(uint8_t)(tod/60%60);
 out[7]=(uint8_t)(tod%60); out[8]=(uint8_t)millis;
 out[9]=(uint8_t)(millis>>8);
 return 1;
}

static int gps_pvt_gps_calendar(const uint8_t *p, uint8_t *out)
{
 unsigned week=gps_pvt_u16(p+0x12), offset=gps_pvt_u16(p+0x10);
 unsigned millis=gps_pvt_u16(p+0xc); uint32_t tow=gps_pvt_u32(p+0x14);
 /* Nonpositive signed week is the vendor's unavailable-clock case.
  * Fractional or offset underflow must not become a huge unsigned value. */
 if (!week || week>=0x8000 || tow>=604800 || millis>=1000 || offset>127) return 0;
 uint64_t gps=315964800ULL+(uint64_t)week*604800+tow;
 uint8_t utc[10], result[10];
 if (!gps_pvt_calendar(gps-offset,millis,utc) ||
     !gps_pvt_calendar(gps,millis,result)) return 0;
 if (gps_pvt_u16(p+4)!=gps_pvt_u16(utc)-1900 || p[6]!=utc[2]-1 ||
     p[7]!=utc[4] || p[8]!=utc[5] || p[9]!=utc[6] || p[0xa]!=utc[7]) return 0;
 memcpy(out,result,sizeof(result));
 return 1;
}

static int gps_pds_calendar_valid(const uint8_t *p)
{
 unsigned year=gps_pvt_u16(p), month=p[2], day=p[4];
 if (!year || year>9999 || !month || month>12 || p[3]>6 || !day ||
     day>gps_pvt_month_days(year,month) || p[5]>23 || p[6]>59 ||
     p[7]>59 || gps_pvt_u16(p+8)>=1000) return 0;
 return 1;
}
