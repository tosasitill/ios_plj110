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
static bool requested, powered, isolated, pins_prepared, config_dirty, ap_saved;
static uint32_t con2_saved, feedback_saved, fifo_saved[4], default_saved;
static const unsigned ap_offsets[]={0x0c,0x10,0x24,0x28,0x2c,0x40,0x44,0x54,0x58};
static uint32_t ap_registers[sizeof(ap_offsets)/sizeof(ap_offsets[0])];
static uint32_t ap_feature, ap_divisor[3], ap_fifo;
enum { DEV0=0, DEV0_SET=4, DEV0_CLR=8, CON1=0xc4, CON2=0xc8,
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
static int shared_idle(void)
{
    int error=apb(); if (error) return error;
    /* Ready alone isn't busy; SW request/RX-data/busy/BT/host-awake are. */
    if ((rd(hub, 0x40)|rd(hub, 0x80))&0x1d0f) return EBUSY;
    return 0;
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
        (rd(hub,DEV0)&3)!=3 || (rd(peri,0x18)&(7u<<18)) ||
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
static int endpoints_idle(void)
{
    int error=x9_bt_hardware_check(); if (error) return error;
    error=shared_idle(); if (error) return error;
    for (unsigned i=0; i<4; i++) {
        if (!(rd(ep[i],0x14)&0x40) || (rd(ep[i],0x14)&1) ||
            (rd(ep[i],0x64)&0x1f)) return EBUSY;
        /* Android reset_flow_control is a no-op when no endpoint detected
         * XOFF. Do not forcibly loopback/reset live MD/ADSP to clear XOFF. */
        if (rd(ep[i],0x80)&8) return EBUSY;
    }
    return 0;
}
static int pre_on(void *context)
{
    (void)context;
    int error=x9_bt_hardware_map(); if (error) return error;
    error=shared_idle(); if (error) return error;
    if (rd(hub,DEV0)&0x1d0f) return EBUSY;
    con2_saved=rd(hub,CON2); feedback_saved=rd(hub,STA0)&(1u<<12);
    default_saved=rd(sram,SSPM_DEFAULT);
    /* SSPM owns its reset/power/clock lifecycle. This is Android's shared
     * SRAM config, not evidence that the coprocessor has been booted. */
    wr(sram,SSPM_DEFAULT,0x10001); config_dirty=true;
    error=x9_conn_bt_pins(X9_CONN_BT_PRE); if (error) return error;
    pins_prepared=true;
    field(hub,STA0,1u<<10,1u<<10); /* BT off during preparation, not assert */
    field(hub,CON2,4,0); /* multi-host so dev0 request reaches SSPM */
    wr(hub,DEV0_SET,1); requested=true; wr(hub,DEV0_SET,2);
    for (unsigned ms=0; ms<50; ms++) {
        if (rd(hub,DEV0)&(1u<<9)) break;
        sleep_ms(1);
    }
    error=x9_bt_hardware_check();
    if (error) {
        printf("X9 BT: SSPM request-ready failed dev0=%08x cg2=%08x default=%08x\n",
            rd(hub,DEV0),rd(peri,0x18),rd(sram,SSPM_DEFAULT));
        return error;
    }
    field(ep[0],0xb0,1,0); /* is_ready's USB_RX_SEL, NOT AP UART3 */
    if (rd(ap,4) || rd(ap,0x4c) || !(rd(ap,0x14)&0x40) || (rd(ap,0x14)&1)) return EBUSY;
    error=endpoints_idle(); if (error) return error;
    for (unsigned i=0;i<sizeof(ap_offsets)/sizeof(ap_offsets[0]);i++)
        ap_registers[i]=rd(ap,ap_offsets[i]);
    ap_feature=rd(ap,0x9c); ap_fifo=rd(ap,0x5c);
    wr(ap,0x9c,1);
    ap_divisor[0]=rd(ap,0x90); ap_divisor[1]=rd(ap,0x94); ap_divisor[2]=rd(ap,0x98);
    wr(ap,0x9c,ap_feature); ap_saved=true;
    for (unsigned i=0;i<4;i++) fifo_saved[i]=rd(ep[i],0x5c);
    /* Android reset_to_ap_enable_only(1), after proving ALL hosts idle. */
    isolated=true; field(hub,STA0,1u<<12,0);
    for (unsigned i=0;i<4;i++) wr(ep[i],8,0xc0);
    field(hub,CON4,16,16);
    wr(ep[3],8,0xc1); wr(ep[0],8,0xc1);
    error=shared_idle(); if (error) return error;
    field(hub,CON2,4,4);
    uint32_t count=rd(hub,0x24)&0xff;
    field(hub,0x24,0xff,(count+1)&0xff);
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
static int host_uart(void *context, uint32_t baud, uint8_t flow)
{
    (void)context;
    /* Production BT config's SW FC uses hardware XON/XOFF; implementing it
     * in an interrupt/polling backend is separate from Linux APDMA. */
    if (flow!=0 || (baud!=115200 && baud!=24000000)) return ENOTSUP;
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
    wr(ap,0x98,0x10); wr(ap,0x9c,0);
    /* 8250_mtk mtk8250_set_flow_ctrl(NONE), offsets are register index*4.
     * Escape character is still programmed even with escape disabled. */
    wr(ap,0x40,0xdb); wr(ap,0x44,0);
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
static int post_download(void *context, bool inband)
{
    (void)context;
    if (inband) return ENOTSUP;
    int error=endpoints_idle(); if (error) return error;
    /* generic 24M config is TODO under SSPM. Do not copy AP's termios into
     * CMM. No proven SSPM CMM24M handoff exists yet, so refuse multi-host
     * instead of treating default=1 or AP baud as proof of CMM baud. */
    printf("X9 BT: post-download SSPM endpoint handoff required (CMM untouched)\n");
    return ENOTSUP;
}
static int verify_ownership(void *context)
{
    (void)context;
    int error=x9_bt_hardware_check(); if (error) return error;
    if (rd(hub,CON2)&4) return ENXIO;
    if (rd(hub,STA0)&4) {
        /* State bit11, SET bit2, CLR bit4; they are NOT interchangeable. */
        wr(hub,IRQ_CLR,8); wr(hub,DEV0_SET,4);
        for (unsigned n=0;n<20;n++) {
            uint32_t dev=rd(hub,DEV0), sta=rd(hub,STA0);
            if ((dev&(1u<<11)) && (sta&(1u<<7)) && (sta&8) &&
                (dev&(1u<<12)) && (rd(hub,IRQ_STA)&8))
                return x9_conn_power_done(X9_CONN_BT);
            delay(50);
        }
        return ETIMEDOUT;
    }
    return x9_conn_power_done(X9_CONN_BT);
}
static int controller_config(void *context)
{
    (void)context;
    /* btmtk_fops_open -> btmtk_send_init_cmds -> phase1 WMT/vendor config.
     * No captured production selected bt_mt6653_*.cfg / bt.cfg exists in
     * the current inputs. A checked-in example is not the active board's
     * config; absence of that input does not prove empty command arrays. */
    printf("X9 BT: selected production phase1/vendor configuration required\n");
    return ENOENT;
}
int x9_bt_hardware_stop(void)
{
    /* Worker disables its UART IRQ/TX first. Never restore full shared
     * snapshots, clear MD requests, or power down another lease holder. */
    int error=pins_prepared?x9_conn_bt_pins(X9_CONN_BT_RESET_OFF):0;
    if (requested && !apb()) {
        field(hub,STA0,1u<<10,1u<<10);
        if (ap_saved && !x9_bt_hardware_check()) {
            wr(ap,4,0);
            for (unsigned i=0;i<sizeof(ap_offsets)/sizeof(ap_offsets[0]);i++)
                wr(ap,ap_offsets[i],ap_registers[i]);
            wr(ap,0x9c,1); wr(ap,0x90,ap_divisor[0]); wr(ap,0x94,ap_divisor[1]);
            wr(ap,0x98,ap_divisor[2]); wr(ap,0x9c,ap_feature);
            wr(ap,8,ap_fifo&~6u); ap_saved=false;
        }
        if (!shared_idle() && isolated && !x9_bt_hardware_check()) {
            for (unsigned i=0;i<4;i++) wr(ep[i],8,fifo_saved[i]&~6u);
            field(hub,STA0,1u<<12,feedback_saved);
        }
        /* Never replay a shared routing snapshot over a newly busy host. */
        if (!shared_idle()) field(hub,CON2,4,con2_saved);
        else if (!error) error=EBUSY;
        wr(hub,DEV0_CLR,0x1f); /* only DEV0: host-awake/RX-data/hwRX/SWTX/SWRX */
        requested=false;
    }
    if (config_dirty) { wr(sram,SSPM_DEFAULT,default_saved); config_dirty=false; }
    if (powered) { int e=x9_conn_power_release(X9_CONN_BT); if (!error) error=e; powered=false; }
    return error;
}
void x9_bt_hardware_ops(struct x9_bt_session_ops *o)
{
    o->pre_on=pre_on; o->power_on=power_on; o->rx_and_32k=rx_and_32k;
    o->host_uart=host_uart; o->hub_features=hub_features;
    o->post_download=post_download; o->verify_ownership=verify_ownership;
    o->controller_config=controller_config;
}
#endif
