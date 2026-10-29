/* X9 capture: spmi@1c01a000, pmif-p=0x1c018000, spmi-dev-mask=0x55a8
 * selects path P for mt6379@e. MT6993 PMIF uses mt6xxx_regs and soc_chan=2.
 * Verified against captured spmi-mtk-pmif.ko and OPPO spmi-mtk-pmif-core.c.
 * Only issue EXT_READ_LONG and acknowledge its own completed transaction.
 */
#include "usb_repeater_probe.h"

int x9_usb_repeater_read(void *context, uint16_t address, uint8_t *value,
                         uint32_t *last_status,
                         uint32_t (*read)(void *, uintptr_t),
                         void (*write)(void *, uintptr_t, uint32_t),
                         void (*delay_us)(void *, unsigned))
{
    const uintptr_t pmif=0x1c018000UL;
    /* Exact non-destructive device/control register allowlist, not a scan. */
    switch (address) {
    case 0x0000:case 0x0d00:case 0x0d03:case 0x0d12:
    case 0x0d88:case 0x0d89:case 0x0d8b:case 0x0d92:break;
    default:return -1;
    }
    if (!value || !last_status) return -1;
    uint32_t init=read(context,pmif);
    uint32_t status=read(context,pmif+0x8a8);
    *last_status=status;
    if (!(init&1)) return -2;
    /* Refuse someone else's pending transaction; never force-clear a channel. */
    if ((status>>1)&7) return -3;
    /* Opcode3, SID14, bytecount-1=0, WRITE bit29=0. */
    write(context,pmif+0x880,0xce000000U|address);
    for (unsigned i=0;i<1000;i++) {
        status=read(context,pmif+0x8a8);*last_status=status;
        if (((status>>1)&7)==6) {
            uint32_t data=read(context,pmif+0x894);
            write(context,pmif+0x8a4,1);
            if (status&(1U<<18)) return -4;
            *value=(uint8_t)data;
            return 0;
        }
        delay_us(context,10);
    }
    return -5;
}
