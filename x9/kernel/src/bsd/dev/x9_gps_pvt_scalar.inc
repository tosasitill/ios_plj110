/* MNL PVT carries binary64 heights; PDS carries binary32 heights. Convert
 * wire bits with round-to-nearest, ties-to-even, without enabling kernel
 * FP/SIMD or depending on a host rounding mode. A nonfinite, overflowing,
 * or nonzero-underflowing input leaves the destination untouched. Signed
 * zero is preserved, but only the caller's measurement qualification may
 * make it valid. This helper does not grant a height datum or fix authority. */
static uint64_t gps_pvt_round_right(uint64_t value, unsigned shift)
{
 /* The only callers use shifts29..53 of a 53-bit significand. */
 uint64_t kept=value>>shift;
 uint64_t lost=value&((1ULL<<shift)-1), half=1ULL<<(shift-1);
 return kept+(lost>half || (lost==half && (kept&1)));
}

static int gps_pvt_copy_height(const uint8_t *src, uint8_t *dst)
{
 uint64_t value=gps_le64(src), magnitude=value&0x7fffffffffffffffULL;
 unsigned exponent=(unsigned)(magnitude>>52);
 uint32_t sign=(uint32_t)(value>>32)&0x80000000u, result;
 if (exponent==0x7ff) return 0;
 if (!magnitude) result=sign;
 else {
  /* Binary64 subnormals are all below the binary32 rounding boundary. */
  if (!exponent) return 0;
  int power=(int)exponent-1023;
  if (power>127 || power<-150) return 0;
  uint64_t significand=(magnitude&0x000fffffffffffffULL)|(1ULL<<52);
  if (power>=-126) {
   uint64_t rounded=gps_pvt_round_right(significand,29);
   if (rounded==(1ULL<<24)) {
    rounded>>=1;
    if (++power>127) return 0;
   }
   result=sign|((uint32_t)(power+127)<<23)|((uint32_t)rounded&0x007fffffu);
  } else {
   /* Rounding in units of 2^-149 can also produce the smallest normal. */
   uint64_t rounded=gps_pvt_round_right(significand,(unsigned)(-power-97));
   if (!rounded) return 0;
   result=sign|(uint32_t)rounded;
  }
 }
 for (unsigned i=0;i<4;i++) dst[i]=(uint8_t)(result>>(8*i));
 return 1;
}
