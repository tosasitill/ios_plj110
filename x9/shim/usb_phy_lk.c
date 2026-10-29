/* MT6993 stock LK USB PHY recovery, not a cross-SoC initialization guess.
 * Source: user-captured x9/partitions/lk_a.img, LK text at file +0x200.
 * RVA 0xc1c30..0xc1d1c: called by UDC start at 0xbfb10.
 * Paired save-current RVA 0xc1d20..0xc1d80: UDC stop call at 0xbffe0.
 * Exact masks/order retained; delays are 2000us as in the stock recovery.
 * Symbolic trace and source disassembly: work/usb-lk-analysis/.
 */
#include "usb_phy_lk.h"

void x9_usb_phy_recover(void *context,
                        uint32_t (*read)(void *, uintptr_t),
                        void (*write)(void *, uintptr_t, uint32_t),
                        void (*delay_us)(void *, unsigned))
{
#define UPDATE(address, mask, bits) \
    write(context, address, (read(context,address)&(uint32_t)(mask))|(uint32_t)(bits))
    UPDATE(0x16760368UL,0xfbffffffU,0);
    UPDATE(0x1676036cUL,0xfffeffffU,0);
    UPDATE(0x16760320UL,0xfffffdffU,0);
    UPDATE(0x16760320UL,0xfffffeffU,0);
    UPDATE(0x16760318UL,0xff7fffffU,0);
    UPDATE(0x16760300UL,0xffffffffU,0x20);
    UPDATE(0x16760314UL,0xfffff7ffU,0);
    UPDATE(0x16760368UL,0xffffffffU,0x8);
    UPDATE(0x16760368UL,0xffffffffU,0x40000);
    delay_us(context,2000);
    UPDATE(0x1676036cUL,0xffffffefU,0);
    UPDATE(0x1676036cUL,0xffffffffU,0x2a);
    UPDATE(0x1676036cUL,0xffffffffU,0x3a00);
    UPDATE(0x16760368UL,0xffffc30bU,0);
    UPDATE(0x16760368UL,0xff41ffffU,0);
    delay_us(context,2000);
#undef UPDATE
}

/* X9 phy-mtk-xsphy.ko .text 0x53a8..0x5554, cross-checked against
 * OPPO MT6993 u2_phy_instance_power_on(). This is the core MMIO sequence;
 * repeater power, efuse/calibration and device tuning are not fabricated. */
void x9_usb_phy_power_on(void *context,
                         uint32_t (*read)(void *, uintptr_t),
                         void (*write)(void *, uintptr_t, uint32_t),
                         void (*delay_us)(void *, unsigned))
{
#define UPDATE(address, mask, bits) \
    write(context, address, (read(context,address)&(uint32_t)(mask))|(uint32_t)(bits))
    UPDATE(0x16760344UL,0xffffffffU,0x20);
    delay_us(context,250);
    UPDATE(0x16760368UL,0xffffffffU,0x40000);
    UPDATE(0x16760368UL,0xfffffff7U,0);
    UPDATE(0x16760368UL,0xffffffffU,0x8);
    delay_us(context,30);
    UPDATE(0x16760368UL,0xfffbffffU,0);
    UPDATE(0x16760368UL,0xfffffff7U,0);
    UPDATE(0x16760368UL,0xfbffffffU,0);
    UPDATE(0x1676036cUL,0xfffeffffU,0);
    UPDATE(0x16760320UL,0xfffffcffU,0);
    UPDATE(0x16760368UL,0xfffbffffU,0);
    UPDATE(0x16760368UL,0xff41c30bU,0);
    UPDATE(0x16760318UL,0xff7fffffU,0);
    UPDATE(0x16760318UL,0xffffffffU,0x100000);
    UPDATE(0x1676036cUL,0xffffffcfU,0x24);
    UPDATE(0x16760300UL,0xf8ffffffU,0);
    UPDATE(0x16760318UL,0x3dffffffU,0);
    UPDATE(0x16760318UL,0xffffffffU,0x40000000);
    delay_us(context,800);
#undef UPDATE
}

/* Stock LK RVA 0xbfb14..0xbfb38 after PHY recovery at 0xbfb10;
 * Linux mtu3_core.c:mtu3_device_reset() uses the identical 1us pulse.
 * This is IPPC DEV_RST_CTRL, not global IP reset or a PHY/power reset. */
void x9_usb_device_reset(void *context,
                          uint32_t (*read)(void *, uintptr_t),
                          void (*write)(void *, uintptr_t, uint32_t),
                          void (*delay_us)(void *, unsigned))
{
    const uintptr_t reset=0x16753e98UL;
    write(context,reset,read(context,reset)|1U);
    delay_us(context,1);
    write(context,reset,read(context,reset)&~1U);
}
