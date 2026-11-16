/* MT6993 SSPM_DRIVER_EN=1. Based on OPPO uarthub/platform/6993/mt6993.c,
 * 8250_mtk.c, btmtk_proj_sp.c and the production live.dts. APB access permits
 * requests even with PLL gated; ready + resources must precede UART MMIO.
 * No manual PLL/CMM baud writes from the non-SSPM path, no default chip ID.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include "x9_bt_hardware.h"
#include "x9_conn_power.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static volatile uint32_t *peri, *top, *hub, *sram, *pll, *spm, *ack, *ap;
static volatile uint32_t *ep[4]; /* dev0, MD, ADSP, CMM; not AP UART3 */
static bool requested, powered, isolated, pins_prepared, config_dirty, ap_saved, bypass_set, hub_saved;
enum { OWN_UNKNOWN, OWN_DRIVER, OWN_SLEEPING, OWN_WAKING };
static unsigned ownership=OWN_UNKNOWN;
/* Once downloaded firmware has acknowledged ownership, a later init/vendor
 * or resume failure does not authorize reset/power-off. Keep its shared
 * supply lease until an explicit, acknowledged function-off exists. */
static bool firmware_live;
static uint8_t host_flow=0xff;
static uint32_t con2_saved, sta0_saved, irq_mask_saved, fifo_saved[4], default_saved;
static const unsigned ap_offsets[]={0x0c,0x10,0x24,0x28,0x2c,0x40,0x44,0x54,0x58};
static uint32_t ap_registers[sizeof(ap_offsets)/sizeof(ap_offsets[0])];
static uint32_t ap_feature, ap_fifo;
/* FEATURE_SEL=1 bank, including the software FC characters. Preserve these
 * too: restoring only DLL/DLH/EFR leaks the new XON/XOFF configuration. */
static const unsigned ap_bank_offsets[]={0x90,0x94,0x98,0xa0,0xa4,0xa8,0xac,0xc0};
static uint32_t ap_bank[sizeof(ap_bank_offsets)/sizeof(ap_bank_offsets[0])];
enum { DEV0=0, DEV0_SET=4, DEV0_CLR=8, DEV0_RESERVE=0x24, DEV0_IRQ_CLR=0x34,
       DEV0_IRQ_MASK=0x38, DEV1=0x40, DEV2=0x80, CON1=0xc4, CON2=0xc8,
       IRQ_STA=0xd0, IRQ_CLR=0xd4, STA0=0xe0, CON4=0xf0,
       /* 60+4 +400+4 +4 +8 +4 = 484 (0x1e4). */
       SSPM_DEFAULT=(12*5+4)+(20*20+4)+4+8+4 };
_Static_assert(SSPM_DEFAULT==0x1e4, "MT6993 SSPM SRAM ABI");
static uint32_t rd(volatile uint32_t *p, unsigned off) { return p[off/4]; }
static void wr(volatile uint32_t *p, unsigned off, uint32_t v)
{ p[off/4]=v; __asm__ volatile("dsb sy" ::: "memory"); }
static void field(volatile uint32_t *p, unsigned off, uint32_t m, uint32_t v)
{ wr(p, off, (rd(p, off)&~m)|(v&m)); }
static void sleep_ms(unsigned ms) { delay_for_interval(ms, 1000*NSEC_PER_USEC); }
static int apb(void)
{
    if (!peri || !hub || (rd(peri, 0x18)&(3u<<19))) return ENXIO;
    /* No INTFHUB read before PCLK/HCLK are available. */
    return (rd(hub, CON1)&0xffff0000u)==0x85810000u ? 0 : ENXIO;
}
int x9_bt_hardware_map(void)
{
#define MAP(dst, pa, sz) do { if (!(dst)) (dst)=(volatile uint32_t *)ml_io_map(pa,sz); \
    if (!(dst)) return ENOMEM; } while(0)
    MAP(peri,0x16640000,0x1000); MAP(top,0x1c210000,0x1000);
    MAP(hub,0x160a0400,0x200); MAP(sram,0x00113c00,0x200);
    MAP(pll,0x1c213100,0x100); MAP(spm,0x1c004000,0x1000);
    MAP(ack,0x1c00d000,0x1000); MAP(ap,0x16030000,0x1000);
    MAP(ep[0],0x16070100,0x100); MAP(ep[1],0x16080200,0x100);
    MAP(ep[2],0x16090300,0x100); MAP(ep[3],0x16060000,0x100);
#undef MAP
    return apb();
}
int x9_bt_hardware_check(void)
{
    int error=apb(); if (error) return error;
    if (!requested || !(rd(hub,DEV0)&(1u<<9)) ||
        (rd(hub,DEV0)&3)!=(ownership==OWN_WAKING?2u:3u) || (rd(peri,0x18)&(7u<<18)) ||
        !(rd(pll,0x34)&16) || (rd(spm,0x8a4)&(0x1fu<<20))!=(0x1fu<<20) ||
        (rd(ack,0xa4c)&0x1f)!=0x1f) return ENXIO;
    /* AP UART3 (CG0 b3/reset22), unlike the dev0 hub endpoint. */
    if ((rd(peri,0x10)&8) || (rd(peri,0)&(1u<<22)) ||
        !(rd(peri,0x20)&8) || (rd(top,0x100)&0x80800000u)) return ENXIO;
    return 0;
}
int x9_bt_hardware_uart(volatile uint32_t **out)
{
    if (!out) return EINVAL;
    int error=x9_bt_hardware_check(); if (!error) *out=ap;
    return error;
}
/* INTFHUB registers are APB-only and readable once apb() passes; endpoint
 * registers need ready + resources (x9_bt_hardware_check) first. */
static void hub_dump(const char *why)
{
    if (!peri || !hub || apb()) {
        printf("X9 BT: %s; hub APB unavailable cg2=%08x\n", why, peri?rd(peri,0x18):0);
        return;
    }
    printf("X9 BT: %s dev0=%08x dev1=%08x dev2=%08x con2=%08x sta0=%08x cg2=%08x\n",
        why, rd(hub,DEV0), rd(hub,DEV1), rd(hub,DEV2), rd(hub,CON2), rd(hub,STA0), rd(peri,0x18));
    if (x9_bt_hardware_check()) return;
    printf("X9 BT: %s fcr_rd=%08x/%08x/%08x/%08x dbg8=%08x/%08x/%08x/%08x ap ier=%08x lsr=%08x\n",
        why, rd(ep[0],0x5c), rd(ep[1],0x5c), rd(ep[2],0x5c), rd(ep[3],0x5c),
        rd(ep[0],0x80), rd(ep[1],0x80), rd(ep[2],0x80), rd(ep[3],0x80), rd(ap,4), rd(ap,0x14));
}
/* btmtk_pre_power_on_handler (btmtk_proj_sp.c) on mt6993.c, in Android order:
 * PRE_ON pins -> bypass(0) -> dev0 RX request -> wakeup (TX request, poll
 * ready) -> reset_flow_ctrl -> fifo_ctrl(1) = feedback(0) +
 * reset_to_ap_enable_only(1) -> bypass(1) -> bt_on_count_inc -> TX pins.
 * Android does NOT require MD/ADSP to be idle (MD_CHANNEL_EN=1): fifo_ctrl(1)
 * is what stops them during download. Every refusal logs the hub state. */
static int pre_on(void *context)
{
    (void)context;
    int error=x9_bt_hardware_map();
    if (error) { hub_dump("pre_on map/APB failed"); return error; }
    /* Boot state: a failed session restores exactly this (the state every
     * passive boot ran with), independent of what other hosts are doing. */
    con2_saved=rd(hub,CON2); sta0_saved=rd(hub,STA0); irq_mask_saved=rd(hub,DEV0_IRQ_MASK);
    default_saved=rd(sram,SSPM_DEFAULT); hub_saved=true;
    /* SSPM owns its reset/power/clock lifecycle. This is Android's shared
     * SRAM config, not evidence that the coprocessor has been booted. */
    wr(sram,SSPM_DEFAULT,0x10001); config_dirty=true;
    error=x9_conn_bt_pins(X9_CONN_BT_PRE); if (error) return error;
    pins_prepared=true;
    /* bypass(0): multi-host, clear dev0 IRQs. Android also unmasks them for
     * its hub IRQ handler; XNU has none, so the boot mask is kept here. */
    field(hub,CON2,4,0); wr(hub,DEV0_IRQ_CLR,0xffffffffu);
    /* dev0_set_rx_request, then btmtk_wakeup_uarthub: TX request, 50 x 1 ms. */
    wr(hub,DEV0_SET,1); requested=true; wr(hub,DEV0_SET,2);
    for (unsigned ms=0; ms<50; ms++) {
        if (rd(hub,DEV0)&(1u<<9)) break;
        sleep_ms(1);
    }
    error=x9_bt_hardware_check();
    if (error) { hub_dump("pre_on SSPM request-ready failed"); return error; }
    field(ep[0],0xb0,1,0); /* is_ready's USB_RX_SEL, NOT AP UART3 */
    /* reset_flow_control is a no-op unless an endpoint detected XOFF. Its
     * loopback recovery is not implemented: refuse instead of guessing. */
    for (unsigned i=0;i<4;i++)
        if (rd(ep[i],0x80)&8) { hub_dump("pre_on XOFF detected, flow reset not implemented"); return EBUSY; }
    /* AP UART3 is our own endpoint. An enabled IER means another user. */
    if (rd(ap,4)) { hub_dump("pre_on AP UART3 interrupts already enabled"); return EBUSY; }
    for (unsigned i=0;i<sizeof(ap_offsets)/sizeof(ap_offsets[0]);i++)
        ap_registers[i]=rd(ap,ap_offsets[i]);
    ap_feature=rd(ap,0x9c); ap_fifo=rd(ap,0x5c);
    wr(ap,0x9c,1);
    for (unsigned i=0;i<sizeof(ap_bank_offsets)/sizeof(ap_bank_offsets[0]);i++)
        ap_bank[i]=rd(ap,ap_bank_offsets[i]);
    wr(ap,0x9c,ap_feature); ap_saved=true;
    /* mtk8250_uart_hub_set_request -> mtk8250_clear_fifo: a byte latched
     * while the hub woke (frame error during CKOFF) is discarded, not fatal. */
    wr(ap,8,(ap_fifo&~6u)|6u);
    for (unsigned i=0;i<4;i++) fifo_saved[i]=rd(ep[i],0x5c);
    /* fifo_ctrl(1): feedback_tx_host_awake_sta_en(0), reset_to_ap_enable_only(1). */
    field(hub,STA0,1u<<12,0);
    uint32_t d0=rd(hub,DEV0), d1=rd(hub,DEV1), d2=rd(hub,DEV2);
    if (d0==d1 && d1==d2 && (d0==0x300 || d0==0)) { hub_dump("pre_on all host status equal"); return EIO; }
    uint32_t trx=d0&3;
    wr(hub,DEV0_SET,3);
    isolated=true;
    for (unsigned i=0;i<4;i++) wr(ep[i],8,0xc0);
    field(hub,CON4,16,16); /* sw4_rst */
    wr(ep[3],8,0xc1); wr(ep[0],8,0xc1); /* ap_only=1: CMM + dev0 only */
    wr(hub,DEV0_SET,trx);
    /* bypass(1): mask + clear dev0 IRQs, then bypass. */
    wr(hub,DEV0_IRQ_MASK,0xffffffffu); wr(hub,DEV0_IRQ_CLR,0xffffffffu);
    field(hub,CON2,4,4); bypass_set=true;
    uint32_t count=rd(hub,DEV0_RESERVE)&0xff; /* bt_on_count_inc: 0xff wraps to 0 */
    field(hub,DEV0_RESERVE,0xff,count==0xff?0:count+1);
    return x9_conn_bt_pins(X9_CONN_BT_TX);
}
static int power_on(void *context)
{
    (void)context;
    int error=x9_conn_power_acquire(X9_CONN_BT);
    if (!error) powered=true;
    return error;
}
static int rx_and_32k(void *context)
{
    (void)context;
    /* mt6993's ext_32k callback is absent: the manager's actual no-op. */
    return powered ? x9_conn_bt_pins(X9_CONN_BT_RX) : ENXIO;
}
int x9_bt_hardware_config_uart(uint32_t baud, uint8_t flow)
{
    /* Caller holds the transport RX/IRQ lock. Android UART_FC=1 is the
     * active uart_launcher -f 1 selection, WMT byte 0x80, not IXON/IXOFF.
     * Hardware inserts/removes escape sequences and consumes XON/XOFF;
     * encoding them again in the H4 path would corrupt literal HCI bytes. */
    if ((flow!=0 && flow!=0x80) || (baud!=115200 && baud!=24000000)) return ENOTSUP;
    int error=x9_bt_hardware_check(); if (error) return error;
    uint32_t mux=rd(top,0x100);
    static const unsigned rates[]={26000000,52000000,104000000,208000000};
    unsigned clock=rates[(mux>>24)&3];
    if (baud==24000000 && clock!=208000000) return ENOTSUP;
    unsigned divisor=(unsigned)(((uint64_t)clock+256ULL*baud-1)/(256ULL*baud));
    unsigned sample=clock/(baud*divisor)-1;
    static const uint8_t fl[]={0,1,5,0x15,0x55,0x57,0x57,0x77,0x7f,0xff,0xff};
    static const uint8_t fm[]={0,0,0,0,0,0,1,1,1,1,3};
    unsigned fraction=(unsigned)(((uint64_t)clock*100/baud/divisor)%100+5)/10;
    if (!divisor || divisor>0xffff || fraction>=sizeof(fl) || sample<2) return EINVAL;
    /* Leave the FIFO intact across baud changes: a flush could discard an
     * already received WMT event. IRQ enable is transport-owned. */
    wr(ap,0x0c,3); wr(ap,0x24,3);
    wr(ap,0x28,baud==24000000?7:sample);
    wr(ap,0x2c,baud==24000000?3:(sample>>1)-1);
    wr(ap,0x54,baud==24000000?0xdb:fl[fraction]);
    wr(ap,0x58,baud==24000000?1:fm[fraction]);
    wr(ap,0x9c,1); wr(ap,0x90,divisor&0xff); wr(ap,0x94,divisor>>8);
    /* Actual 8250_mtk mtk8250_set_flow_ctrl: ECB, then SW low bits 0xa.
     * START/STOP=0x11/0x13 read from this active Android tty (2026-11-15),
     * independently also present in the SSPM endpoint setup. */
    wr(ap,0x98,flow==0x80?0x1a:0x10);
    if (flow==0x80) {
        wr(ap,0xa0,0x11); wr(ap,0xa4,0x11);
        wr(ap,0xa8,0x13); wr(ap,0xac,0x13); wr(ap,0xc0,0x13);
    }
    wr(ap,0x9c,0);
    wr(ap,0x40,0xdb); wr(ap,0x44,flow==0x80?1:0);
    /* PIO transport owns RDI/RLSI only. No RTS/CTS or XOFF interrupt has a
     * handler. SW gating/escape is automatic in UART hardware, independent
     * of IER_XOFFI; the Android APDMA path also masks those interrupts. Keep
     * our existing RX bits intact across the baud/flow transition. */
    field(ap,4,0xe0,0);
    host_flow=flow;
    return 0;
}
static int hub_features(void *context, bool *hardware_sleep, bool *inband)
{
    (void)context;
    int error=x9_bt_hardware_check(); if (error) return error;
    *hardware_sleep=(rd(hub,STA0)&4)!=0;
    /* Actual MT6993 platform callback returns 0, not register INB_EN. */
    *inband=false; return 0;
}
static int observed_profile(void)
{
    if (host_flow!=0x80) return ENOTSUP;
    int error=x9_bt_hardware_check(); if (error) return error;
    /* Captured Android boot, 2026-11-15, HUB_DBG at 11.067400..469 and
     * 15.504009..124: measured hub clock208M/mux2, dev0 + CMM have THIS
     * profile. Generic 24M config does nothing; SSPM already configures
     * the endpoints. The same sample7 is labelled 12M in the 104M source
     * branch, so sample alone is insufficient. Never copy AP point3 or
     * reprogram CMM. Require the current running SSPM/request resources,
     * current 208M mux AND every relevant endpoint register on this boot. */
    uint32_t mux=rd(top,0x100);
    if (((mux>>16)&3)!=2 || ((mux>>24)&3)!=3 || !(rd(hub,CON2)&2)) return ENOTSUP;
    static const unsigned offsets[]={0x9c,0x24,0x28,0x2c,0x90,0x94,
                                     0x54,0x58,0x98,0xa0,0xa4,0xa8,0xac,0x40,0x44,0x4c};
    static const uint32_t profile[]={1,3,7,4,1,0,0xdb,1,0xa,0x11,0x11,0x13,0x13,0xdb,1,3};
    for (unsigned index=0;index<2;index++) {
        volatile uint32_t *endpoint=ep[index?3:0];
        for (unsigned k=0;k<sizeof(offsets)/sizeof(offsets[0]);k++) {
            uint32_t value=rd(endpoint,offsets[k]);
            if (value!=profile[k]) {
                printf("X9 BT: SSPM %s profile mismatch off=%x got=%x expected=%x\n",
                       index?"CMM":"dev0",offsets[k],value,profile[k]);
                return ENOTSUP;
            }
        }
    }
    /* AP UART3's running FEATURE_SEL=0 view is safe to read without bank
     * switches or losing wake data. AP sample point is3, not SSPM's4; AP
     * DMA remains off for our PIO/IRQ path. Preserve both IER0 (paused) and
     * IER5 (owned). Nothing here changes UART/FIFO/routing/SSPM state.
     * DLL/DLH/EFR/XON/INB retention is NOT proven by these reads: Android's
     * dump switches FEATURE_SEL=1 before accessing them. We deliberately
     * do not make that write in wake, or read the hidden bank as a guess. */
    static const unsigned ap_normal_offsets[]={0x9c,0x0c,0x24,0x28,0x2c,0x54,0x58,0x40,0x44,0x4c};
    static const uint32_t ap_normal_profile[]={0,3,3,7,3,0xdb,1,0xdb,1,0};
    for (unsigned k=0;k<sizeof(ap_normal_offsets)/sizeof(ap_normal_offsets[0]);k++) {
        uint32_t value=rd(ap,ap_normal_offsets[k]);
        if (value!=ap_normal_profile[k]) {
            printf("X9 BT: AP retained profile mismatch off=%x got=%x expected=%x\n",
                   ap_normal_offsets[k],value,ap_normal_profile[k]);
            return ENOTSUP;
        }
    }
    if (rd(ap,4)&~5u) return EBUSY;
    return 0;
}
static int post_download(void *context, bool inband)
{
    (void)context;
    if (inband) return ENOTSUP;
    int error=x9_bt_hardware_check();
    if (error) { hub_dump("post_download hub not ready"); return error; }
    error=observed_profile(); if (error) return error;
    /* Android post-fwdl bypass(0), then power-on-done fifo_ctrl(0): MD and
     * ADSP FIFOs re-enabled unconditionally, feedback on. No idle check:
     * Android has none. Our driver polls; the boot dev0 IRQ mask returns. */
    field(hub,CON2,4,0); bypass_set=false;
    wr(hub,DEV0_IRQ_CLR,0xffffffffu); wr(hub,DEV0_IRQ_MASK,irq_mask_saved);
    wr(ep[1],8,0xc1); wr(ep[2],8,0xc1); isolated=false;
    field(hub,STA0,1u<<12,1u<<12);
    if ((rd(hub,CON2)&4) || !(rd(hub,STA0)&(1u<<12)) ||
        !(rd(ep[1],0x5c)&1) || !(rd(ep[2],0x5c)&1)) return EIO;
    return x9_bt_hardware_check();
}
static int verify_ownership(void *context, bool *bt_awake)
{
    (void)context;
    if (!bt_awake) return EINVAL;
    *bt_awake=false;
    int error=x9_bt_hardware_check(); if (error) return error;
    if (rd(hub,CON2)&4) return ENXIO;
    /* btmtk_uart_open_done order: connv3_pwr_on_done first. */
    firmware_live=true; /* A later supply-accounting error is not function-off. */
    error=x9_conn_power_done(X9_CONN_BT); if (error) return error;
    if (rd(hub,STA0)&4) {
        /* uarthub_host_awake_sta_ctrl_mt6993(0,1): clear feedback IRQ, read
         * pre-state, DEV0_STA_SET bit2. Poll (11 us + 20 x 50-60 us) only if
         * CMM_BT_AWAKE(STA0 bit3)=1 and DEV0 host-awake(bit11) was 0; else
         * CANCEL. check_irq when sent state STA0 bit7 already equalled 1.
         * Android returns 0 on FAIL and CANCEL alike: logged, never fatal. */
        wr(hub,IRQ_CLR,8);
        uint32_t dev_before=rd(hub,DEV0), sta_before=rd(hub,STA0);
        bool cmm=(sta_before&8)!=0, was_set=(dev_before&(1u<<11))!=0;
        bool check_irq=(sta_before&(1u<<7))!=0;
        wr(hub,DEV0_SET,4);
        const char *result="CANCEL";
        if (cmm && !was_set) {
            result="FAIL"; delay(11);
            for (unsigned n=0;n<20;n++) {
                if ((!check_irq || (rd(hub,IRQ_STA)&8)) && (rd(hub,STA0)&(1u<<7)))
                    { result="OK"; break; }
                delay(55);
            }
        }
        uint32_t dev_after=rd(hub,DEV0), sta_after=rd(hub,STA0);
        /* mtk8250_uart_hub_get_bt_awake_sta = STA0 bit6 (bt_awake_sta). */
        *bt_awake=(sta_after&(1u<<6))!=0;
        printf("X9 BT: SET_HOST_AWK %s dev0=%x/%x sta0=%x/%x irq=%x cmm=%d bt_awake=%d\n",
               result,dev_before,dev_after,sta_before,sta_after,rd(hub,IRQ_STA),
               cmm,*bt_awake);
    } else *bt_awake=true; /* no hw sleep mechanism: Android skips 27/28 */
    /* btmtktty power-on done: mtk8250_uart_hub_assert_bit_ctrl(0). */
    ownership=OWN_DRIVER; field(hub,STA0,1u<<10,0);
    return 0;
}

/* mt6993.c:uarthub_host_awake_sta_ctrl. Only our DEV0 command and shared
 * feedback IRQ bit3 are touched. When CMM was awake and the host state
 * changes, wait for the transmitted host-awake state; if it was already the
 * desired sent state, require a fresh feedback completion as Android does.
 * A sleeping CMM cannot send feedback: the later BT_AWAKE poll owns that
 * proof on resume. Never require feedback from a gated UART. */
static int host_awake(bool set)
{
    int error=x9_bt_hardware_check(); if (error) return error;
    uint32_t before_dev=rd(hub,DEV0), before_sta=rd(hub,STA0);
    bool wanted=set;
    bool changed=((before_dev&(1u<<11))!=0)!=wanted;
    bool check_irq=(((before_sta&(1u<<7))!=0)==wanted);
    wr(hub,IRQ_CLR,8);
    wr(hub,set?DEV0_SET:DEV0_CLR,set?4:16);
    if (((rd(hub,DEV0)&(1u<<11))!=0)!=wanted) return EIO;
    if (!(before_sta&8) || !changed) return 0;
    delay(11);
    for (unsigned n=0;n<20;n++) {
        bool sent=(rd(hub,STA0)&(1u<<7))!=0;
        if (sent==wanted && (!check_irq || (rd(hub,IRQ_STA)&8))) return 0;
        delay(50);
    }
    return ETIMEDOUT;
}

int x9_bt_hardware_sleep(void)
{
    if (ownership!=OWN_DRIVER) return ENXIO;
    int error=x9_bt_hardware_check(); if (error) return error;
    if (!(rd(hub,STA0)&4) || (rd(hub,CON2)&4) || !powered) return ENOTSUP;
    /* The sole worker already drained its last FW-own command and masked
     * its UART IRQ. Do not touch AP registers once requests are released. */
    ownership=OWN_UNKNOWN;
    error=host_awake(false); if (error) return error;
    /* btmtk_release_uarthub(true): HOST_AWAKE -> SW_TX -> RX/hwRX/RX_DATA.
     * Clear-TX is a separate write; setting USB_RX_SEL precedes the last
     * RX clear because TX is now zero. No MD/ADSP requests are changed. */
    wr(hub,DEV0_CLR,2);
    field(ep[0],0xb0,1,1);
    wr(hub,DEV0_CLR,1|4|8);
    /* Only APB remains safe now. BT_AWAKE may stay high if another host
     * still needs the controller; that is not failure to release our lease. */
    error=apb(); if (error) return error;
    if (rd(hub,DEV0)&(3u|(1u<<11))) return EIO;
    ownership=OWN_SLEEPING;
    return 0;
}

int x9_bt_hardware_wake(void)
{
    if (ownership!=OWN_SLEEPING || !powered || !requested) return ENXIO;
    int error=apb(); if (error) return error;
    if (!(rd(hub,STA0)&4) || (rd(hub,CON2)&4)) return ENOTSUP;
    /* Android's mtk8250...set_tx_request calls the platform SW_TX setter.
     * It acquires APDMA RX resources but NOT hub SW_RX until own-clear ACK.
     * Our PIO backend has no APDMA to enable. Allow TX-only hardware_check
     * only in this explicit worker transition; keep native admission shut. */
    ownership=OWN_WAKING;
    wr(hub,DEV0_SET,2);
    for (unsigned ms=0;ms<50;ms++) {
        error=x9_bt_hardware_check();
        if (!error) break;
        sleep_ms(1);
    }
    if (error) return error;
    /* A first-boot profile readback does not establish SSPM retention over
     * this sleep cycle. Refuse changed clocks/baud/escape instead of blindly
     * programming CMM or re-running post_download's shared FIFO writes. */
    error=observed_profile(); if (error) return error;
    if (!(rd(hub,STA0)&(1u<<12))) return ENOTSUP;
    /* is_ready resets the DEV0 USB RX pin only AFTER endpoint clocks exist. */
    field(ep[0],0xb0,1,0);
    return host_awake(true);
}

int x9_bt_hardware_awake(bool *awake, bool *need_ff)
{
    if (!awake || !need_ff) return EINVAL;
    *awake=false; *need_ff=false;
    if (ownership!=OWN_WAKING) return ENXIO;
    int error=x9_bt_hardware_check(); if (error) return error;
    uint32_t dev=rd(hub,DEV0), sta=rd(hub,STA0);
    *need_ff=(dev&(1u<<12))==0;
    *awake=(sta&8)!=0 || !*need_ff;
    return 0;
}

int x9_bt_hardware_ownership_finish(void)
{
    if (ownership!=OWN_WAKING) return ENXIO;
    bool awake,need_ff;
    int error=x9_bt_hardware_awake(&awake,&need_ff); if (error) return error;
    if (!awake) return ETIMEDOUT;
    /* Caller obtained the full successful driver-own WMT ACK. Unlike the
     * Android debug failure path, a failed ACK never reaches this setter. */
    wr(hub,DEV0_SET,1);
    ownership=OWN_UNKNOWN; /* hardware_check now requires both SW requests. */
    error=x9_bt_hardware_check(); if (error) return error;
    if (!(rd(hub,DEV0)&(1u<<11))) return EIO;
    ownership=OWN_DRIVER;
    return 0;
}
int x9_bt_hardware_stop(void)
{
    /* Worker disables its UART IRQ/TX first. Never clear MD/ADSP requests
     * or power down another lease holder. */
    if (firmware_live) {
        printf("X9 BT: cleanup retained live firmware lease ownership=%u; no acknowledged function-off\n",
               ownership);
        return EBUSY;
    }
    /* A failed session puts the hub back to its boot state (what every
     * passive boot ran with), whether or not MD/ADSP are busy: Android's
     * fifo_ctrl/bypass/assert calls never wait for other hosts either.
     * INTFHUB registers need APB only; endpoint and AP UART registers are
     * written only while ready + resources still hold. */
    int error=pins_prepared?x9_conn_bt_pins(X9_CONN_BT_RESET_OFF):0;
    pins_prepared=false;
    if (requested || hub_saved) {
        if (apb()) {
            hub_dump("cleanup hub APB unavailable; boot state not restored");
            if (!error) error=ENXIO;
        } else {
            bool ready=requested && !x9_bt_hardware_check();
            if (isolated) {
                if (ready) {
                    /* reset_to_ap_enable_only(0) semantics: FIFOE as found. */
                    for (unsigned i=1;i<3;i++) wr(ep[i],8,0xc0|(fifo_saved[i]&1));
                } else {
                    hub_dump("cleanup cannot reopen MD/ADSP FIFO without resources");
                    if (!error) error=ENXIO;
                }
                isolated=false;
            }
            if (ap_saved && ready) {
                wr(ap,4,0);
                for (unsigned i=0;i<sizeof(ap_offsets)/sizeof(ap_offsets[0]);i++)
                    wr(ap,ap_offsets[i],ap_registers[i]);
                wr(ap,0x9c,1);
                for (unsigned i=0;i<sizeof(ap_bank_offsets)/sizeof(ap_bank_offsets[0]);i++)
                    wr(ap,ap_bank_offsets[i],ap_bank[i]);
                wr(ap,0x9c,ap_feature);
                wr(ap,8,ap_fifo&~6u);
            }
            ap_saved=false;
            if (hub_saved) {
                field(hub,CON2,4,con2_saved);
                field(hub,STA0,(1u<<12)|(1u<<10),sta0_saved);
                wr(hub,DEV0_IRQ_CLR,0xffffffffu); wr(hub,DEV0_IRQ_MASK,irq_mask_saved);
            }
            bypass_set=false;
            if (requested) {
                /* btmtk_release_uarthub(true): HOST_AWAKE -> SW_TX -> USB_RX_SEL -> RX. */
                wr(hub,DEV0_CLR,16); wr(hub,DEV0_CLR,2);
                if (ready) field(ep[0],0xb0,1,1);
                wr(hub,DEV0_CLR,1|4|8);
            }
            printf("X9 BT: cleanup restored hub boot state con2=%08x sta0=%08x dev1=%08x dev2=%08x error=%d\n",
                   rd(hub,CON2),rd(hub,STA0),rd(hub,DEV1),rd(hub,DEV2),error);
        }
        requested=false; hub_saved=false;
    }
    if (config_dirty) { wr(sram,SSPM_DEFAULT,default_saved); config_dirty=false; }
    if (powered) { int e=x9_conn_power_release(X9_CONN_BT); if (!error) error=e; powered=false; }
    ownership=OWN_UNKNOWN;
    return error;
}
void x9_bt_hardware_ops(struct x9_bt_session_ops *o)
{
    o->pre_on=pre_on; o->power_on=power_on; o->rx_and_32k=rx_and_32k;
    /* host_uart is transport-owned: its wrapper serializes FEATURE_SEL and
     * IER changes with the actual IRQ filter. Do not replace it here. */
    o->hub_features=hub_features;
    o->post_download=post_download; o->verify_ownership=verify_ownership;
}
#endif
