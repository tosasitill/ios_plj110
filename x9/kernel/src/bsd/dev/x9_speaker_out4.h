/* Disabled OUT4 register profile shared by the engine and cold board cycle.
 * OPPO mt6993-dai-i2s.c: S16 valid=15, 32-bit slot=31, FS=5/relatch=10.
 * This is register configuration, NOT proof of clocks, pads or PA readiness.
 * Initializers match {off,mask,value,saved,expected}. No OUT4 EN bit is set. */
#ifndef X9_SPEAKER_OUT4_H
#define X9_SPEAKER_OUT4_H
#define X9_SPEAKER_OUT4_DISABLED_CONFIG \
    {0x1580,1u|2u|8u|32u|(7u<<6)|(1u<<10)|(31u<<11)|(31u<<16)|(31u<<23)|(7u<<28), \
        (15u<<11)|(31u<<16)|(1u<<23)|(1u<<28),0,0}, \
    {0x1584,31u|(31u<<5)|(1u<<15),5u|(6u<<5)|(1u<<15),0,0}, \
    {0x1588,31u,6u,0,0}, \
    {0x1590,31u|(7u<<6)|(31u<<24),5u|(1u<<6)|(10u<<24),0,0}
#endif
