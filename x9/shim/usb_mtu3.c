/* Experimental MT6993 USB2 EP0 PIO receiver.
 * MMIO addresses: captured X9 iomem/FDT; register semantics: Linux v6.1
 * drivers/usb/mtu3/{mtu3_hw_regs.h,mtu3_core.c,mtu3_gadget_ep0.c}.
 * Restore the X9 vendor XS-PHY core power-on state; preserve inherited clocks.
 * No Qualcomm DWC3 or MT6789 power sequence.
 * This first transport uses control transfers, not an unverified QMU DMA ring.
 */
#include "hw.h"
#include "plog.h"
#include "ramrx.h"
#include "resident.h"
#include "loader.h"
#include "fdt.h"
#include "usb_phy_lk.h"
#include "usb_repeater_probe.h"

#define MAC 0x16751000UL
#define IPPC 0x16753e00UL
#define BIT(n) (1U<<(n))
#define EP0 0x100
#define FIFO 0x300
#define EPISR 0x80
#define POWER 0x2404
#define COMMON 0x241c
#define CONFIG 0x800
#define SETUP BIT(17)
#define RX BIT(16)
#define TX BIT(18)
#define DATAEND BIT(19)
#define DPHTX BIT(20)
#define STALL BIT(25)
#define SENTSTALL BIT(22)
#define W1C (RX|SETUP|SENTSTALL)

static struct ramrx session;
static uint8_t control[RAMRX_FRAME_MAX];
static unsigned transferred,wanted,in_length,command,configured;
static enum { IDLE, RECEIVE, SEND, SEND_END } phase;
static uint32_t rr(unsigned o) { return rd32(MAC+o); }
static void wr(unsigned o,uint32_t n) { wr32(MAC+o,n); __asm__ volatile("dsb sy" ::: "memory"); }
static uint32_t epcsr(void) { return rr(EP0)&~W1C; }
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1]<<8; }
static void spin_ms(unsigned n)
{
    uint64_t begin=rsr("cntpct_el0"),ticks=counter_frequency()/1000*n;
    while (rsr("cntpct_el0")-begin<ticks) wdt_kick();
}
static uint32_t phy_read(void *context,uintptr_t address)
{
    (void)context;return rd32(address);
}
static void phy_write(void *context,uintptr_t address,uint32_t value)
{
    (void)context;wr32(address,value);
    __asm__ volatile("dsb sy" ::: "memory");
}
static void phy_delay(void *context,unsigned us)
{
    (void)context;
    uint64_t start=rsr("cntpct_el0"),ticks=counter_frequency()*us/1000000;
    while (rsr("cntpct_el0")-start<ticks) wdt_kick();
}
static void repeater_probe(void)
{
    static const uint16_t addresses[]={0,0xd00,0xd03,0xd12,0xd88,0xd89,0xd8b,0xd92};
    plog_str("X9 repeater: configuration reads only; PMIF-P SWINF2 SID14\n");
    for (unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);i++) {
        uint8_t value=0;uint32_t status=0;
        int result=x9_usb_repeater_read(0,addresses[i],&value,&status,phy_read,phy_write,phy_delay);
        plog_hex("RP address",addresses[i]);
        if (result) {
            plog_hex("RP read error",(uint64_t)(int64_t)result);
            plog_hex("RP PMIF status",status);break;
        }
        plog_hex("RP value",value);
    }
    plog_str("X9 repeater probe finished; no PMIC configuration writes\n");
    plog_flush();
}
static void fifo_read(uint8_t *p,unsigned n)
{
    while (n) {
        uint32_t v=rr(FIFO);
        unsigned count=n<4?n:4;
        for (unsigned i=0;i<count;i++) p[i]=(uint8_t)(v>>(8*i));
        p+=count;n-=count;
    }
}
static void fifo_write(const uint8_t *p,unsigned n)
{
    while (n>=4) {
        uint32_t v=p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
        wr(FIFO,v);p+=4;n-=4;
    }
    if (n>=2) { *(volatile uint16_t *)(MAC+FIFO)=p[0]|(uint16_t)p[1]<<8;p+=2;n-=2; }
    if (n) *(volatile uint8_t *)(MAC+FIFO)=*p;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void stall(unsigned ready)
{
    wr(EP0,epcsr()|STALL|ready);phase=IDLE;
}
static void transmit(void)
{
    unsigned n=in_length-transferred;
    if (n>64) n=64;
    fifo_write(control+transferred,n);transferred+=n;
    /* A short response whose length is a multiple of 64 needs a ZLP. */
    phase=(n<64 || (transferred==in_length && in_length==wanted))?SEND_END:SEND;
    wr(EP0,epcsr()|TX);
}
static unsigned string_descriptor(unsigned index)
{
    static const char strings[][24]={"","HelioDarwin","X9 RAM receiver","X9RAMRX1"};
    if (!index) { control[0]=4;control[1]=3;control[2]=9;control[3]=4;return 4; }
    if (index>=4) return 0;
    unsigned n=2;
    for (const char *p=strings[index];*p && n<126;p++) { control[n++]=*p;control[n++]=0; }
    control[0]=n;control[1]=3;return n;
}
static void setup(void)
{
    uint8_t s[8];fifo_read(s,8);
    unsigned type=s[0],request=s[1],value=u16(s+2),index=u16(s+4),length=u16(s+6);
    unsigned count=0;
    phase=IDLE;transferred=0;wanted=length;command=0;
    if (type==0x80 && request==6) {
        static const uint8_t device[]={18,1,0,2,0,0,0,64,9,0x12,1,0,0,1,1,2,3,1};
        static const uint8_t config[]={9,2,18,0,1,1,0,0x80,250,9,4,0,0,0,0xff,0,0,0};
        static const uint8_t qualifier[]={10,6,0,2,0,0,0,64,1,0};
        const uint8_t *source=0;
        if (value==0x100 && !index) {source=device;count=sizeof(device);}
        else if (value==0x200 && !index) {source=config;count=sizeof(config);}
        else if ((value>>8)==3) count=string_descriptor(value&255);
        else if (value==0x600 && !index) {source=qualifier;count=sizeof(qualifier);}
        else if (value==0x700 && !index) {source=config;count=sizeof(config);}
        if (!count || !length) { stall(length?0:SETUP);return; }
        if (source) for (unsigned i=0;i<count;i++) control[i]=source[i];
        if (value==0x700) control[1]=7;
    } else if (type==0 && request==5 && !length && value<128 && !index) {
        wr(CONFIG,(rr(CONFIG)&~(0x7fU<<24)) | value<<24);
    } else if (type==0 && request==9 && !length && value<=1 && !index) {
        configured=value;plog_hex("USB configuration",value);
    } else if (type==0x80 && request==8 && length==1 && !value && !index) {
        control[0]=configured;count=1;
    } else if ((type==0x80 || type==0x81) && request==0 && length==2 && !value && !index) {
        control[0]=control[1]=0;count=2;
    } else if (type==0x81 && request==10 && length==1 && !value && !index) {
        control[0]=0;count=1;
    } else if (type==1 && request==11 && !length && !value && !index) {
        /* One interface, alternate setting zero. */
    } else if (type==0xc0 && request==RAMRX_STATUS && length==48 && !value && !index) {
        ramrx_status(&session,control);count=48;
    } else if (type==0x40 && configured && !value && !index) {
        command=request;
        if (request==RAMRX_BEGIN && length==24) { /* OUT data follows. */ }
        else if (request==RAMRX_DATA && length>=24 && length<=sizeof(control)) { }
        else if ((request==RAMRX_COMMIT || request==RAMRX_BOOT || request==RAMRX_ABORT) && !length) {
            if (ramrx_command(&session,request,control,0)) { stall(SETUP);return; }
        } else { stall(length?0:SETUP);return; }
    } else { stall(length?0:SETUP);return; }
    if (!length) { wr(EP0,epcsr()|SETUP|DATAEND);return; }
    if (type&0x80) {
        in_length=count<length?count:length;
        wr(EP0,epcsr()|SETUP|DPHTX);phase=SEND;transmit();
    } else {
        wr(EP0,(epcsr()|SETUP)&~DPHTX);phase=RECEIVE;
    }
}
static void poll_ep0(void)
{
    uint32_t interrupt=rr(EPISR);
    if (!(interrupt&(BIT(0)|BIT(16)))) return;
    wr(EPISR,interrupt&(BIT(0)|BIT(16)));
    uint32_t csr=rr(EP0);
    if (interrupt&BIT(16)) { phase=IDLE;transferred=0; }
    if (csr&SENTSTALL) { wr(EP0,(csr&~W1C)|SENTSTALL);phase=IDLE; }
    if (csr&SETUP) {
        if (rr(0x108)!=8) {stall(SETUP);return;}
        setup();return;
    }
    if (phase==RECEIVE && (csr&RX)) {
        unsigned n=rr(0x108);
        if (n>64 || n>wanted-transferred) {stall(RX);return;}
        fifo_read(control+transferred,n);transferred+=n;
        if (transferred==wanted || n<64) {
            if (transferred!=wanted || ramrx_command(&session,command,control,transferred)) {
                stall(RX);return;
            }
            wr(EP0,epcsr()|RX|DATAEND);phase=IDLE;
        } else wr(EP0,epcsr()|RX);
    } else if ((phase==SEND || phase==SEND_END) && !(csr&TX)) {
        if (phase==SEND_END) { wr(EP0,epcsr()|DATAEND);phase=IDLE; }
        else transmit();
    }
}
const char *usb_receiver_boot(uintptr_t dtb,const void *payload,size_t size)
{
    uint64_t base,capacity;
    const char *error=resident_layout(payload,size,&base,&capacity);
    if (error) return error;
    const struct payload *p=payload;
    if (!fdt_ram_available(dtb,p->phys_base,p->mem_size))
        return "current LK FDT does not allow the selected RAM window";
    /* Validate the placeholder on a bounded scratch copy before any root write. */
    if (p->adt_size>sizeof(control)) return "receiver ADT too large";
    for (size_t i=0;i<p->adt_size;i++) control[i]=((const uint8_t *)payload)[p->adt_off+i];
    if (resident_patch_adt(control,p->adt_size,base,0)) return "receiver ADT missing RAMDisk placeholder";
    ramrx_init(&session,(void *)(uintptr_t)base,capacity);
    plog_str("X9 USB receiver: stock XS-PHY core power-on; inherited clocks; USB2 EP0 PIO\n");
    plog_hex("root destination",base);plog_hex("root capacity",capacity);
    plog_hex("USB IP HW ID",rd32(IPPC+0xa0));
    plog_hex("USB IP clock status1",rd32(IPPC+0x10));
    plog_hex("USB IP clock status2",rd32(IPPC+0x14));plog_flush();
    /* Only device-port controls; never reset the IP/PHY or guess clock gates. */
    wr32(IPPC+8,rd32(IPPC+8)&~BIT(0));
    /* Linux mtu3_dr.c:ssusb_set_force_mode(MTU3_DR_FORCE_DEVICE).
     * With no resident Type-C/role-switch driver, do not depend on IDDIG
     * left by LK. Both FORCE_IDDIG and RG_IDDIG must select peripheral.
     * mtu3_core.c:mtu3_device_enable also sets U2_PORT_OTG_SEL for OTG.
     * This overrides the digital controller role, not PHY or power rails. */
    plog_hex("USB U2 port before role",rd32(IPPC+0x50));
    wr32(IPPC+0x50,(rd32(IPPC+0x50)&~(BIT(0)|BIT(1)|BIT(2)|BIT(9))) |
                      BIT(7)|BIT(11)|BIT(12));
    __asm__ volatile("dsb sy" ::: "memory");
    plog_hex("USB U2 port forced device",rd32(IPPC+0x50));
    spin_ms(1);
    if ((rd32(IPPC+0x10)&(BIT(0)|BIT(8)))!=(BIT(0)|BIT(8)) ||
        !(rd32(IPPC+0x14)&BIT(0))) return "USB clocks unavailable; PHY initialization required";
    repeater_probe();
    plog_hex("USB PHY 368 before power-on",rd32(0x16760368UL));
    plog_hex("USB PHY 36c before power-on",rd32(0x1676036cUL));
    plog_hex("USB PHY 318 before power-on",rd32(0x16760318UL));
    plog_hex("USB PHY 344 before power-on",rd32(0x16760344UL));
    x9_usb_phy_power_on(0,phy_read,phy_write,phy_delay);
    plog_hex("USB PHY 318 after power-on",rd32(0x16760318UL));
    plog_hex("USB PHY 344 after power-on",rd32(0x16760344UL));
    plog_hex("USB PHY 368 after power-on",rd32(0x16760368UL));
    plog_hex("USB PHY 36c after power-on",rd32(0x1676036cUL));
    plog_hex("USB DEV_RST before pulse",rd32(IPPC+0x98));
    x9_usb_device_reset(0,phy_read,phy_write,phy_delay);
    plog_hex("USB DEV_RST after pulse",rd32(IPPC+0x98));
    plog_hex("USB POWER after device reset",rr(POWER));
    uint32_t info=rr(0xc10);
    plog_hex("USB endpoint capability",info);
    if (!(info&31) || !((info>>8)&31)) return "USB MAC capability unavailable";
    wr(POWER,rr(POWER)&~BIT(6));spin_ms(30);
    wr(0x141c,rr(0x141c)&~BIT(0)); /* disable SuperSpeed, retain PHY setup */
    wr(0xc,0xffffffff);wr(0x8c,0xffffffff); /* poll instead of GIC interrupts */
    wr(CONFIG,rr(CONFIG)&~(0x7fU<<24));
    wr(EP0,64);wr(EPISR,0x10001);wr(COMMON,0xffffffff);
    /* EP0 interrupt latches are needed for the PIO state machine. */
    wr(0x88,0x10001);wr(0x2418,BIT(2)|BIT(5));
    wr(POWER,rr(POWER)|BIT(6)|BIT(5)|BIT(0));
    phase=IDLE;configured=0;
    plog_hex("USB POWER after connect",rr(POWER));
    plog_hex("USB DEVICE_CONF after connect",rr(CONFIG));
    plog_hex("USB MISC VBUS control",rr(0xc84));
    plog_hex("USB OTG status",rd32(IPPC+0x18));
    plog_str("USB waiting: 1209:0001; computer may send root now\n");plog_flush();
    uint64_t progress=0;
    uint64_t next_sample=rsr("cntpct_el0")+counter_frequency()*2;
    unsigned samples=0,setup_events=0,reset_events=0;
    for (;;) {
        wdt_kick();
        uint32_t common=rr(COMMON);
        if (common) wr(COMMON,common);
        if (common&(BIT(2)|BIT(5))) {
            reset_events++;
            phase=IDLE;configured=0;transferred=0;
            ramrx_init(&session,(void *)(uintptr_t)base,capacity);
            wr(CONFIG,rr(CONFIG)&~(0x7fU<<24));wr(EP0,64);
            plog_str("USB bus reset/disconnect: transfer session cleared\n");plog_flush();
        }
        if ((rr(EPISR)&BIT(0)) && (rr(EP0)&SETUP)) setup_events++;
        poll_ep0();
        /* Three observations only; no extra register writes or timed resets. */
        if (samples<3 && rsr("cntpct_el0")>=next_sample) {
            plog_hex("USB link sample",++samples);
            plog_hex("USB U2 port",rd32(IPPC+0x50));
            plog_hex("USB DEVICE_CONF",rr(CONFIG));
            plog_hex("USB DEVICE_CONTROL",rr(0x240c));
            plog_hex("USB USB20_OPSTATE",rr(0x2460));
            plog_hex("USB EP0CSR",rr(EP0));
            plog_hex("USB PHY 368 link",rd32(0x16760368UL));
            plog_hex("USB PHY 318 link",rd32(0x16760318UL));
            plog_hex("USB USB3_CONFIG",rr(0x141c));
            plog_hex("USB reset events",reset_events);
            plog_hex("USB setup events",setup_events);
            plog_flush();
            next_sample=rsr("cntpct_el0")+counter_frequency()*3;
        }
        if (session.received-progress>=0x1000000) {
            progress=session.received;plog_hex("USB received",progress);plog_flush();
        }
        if (session.state==3) {
            /* Let the explicit BOOT control status packet reach the computer. */
            spin_ms(100);
            wr(POWER,rr(POWER)&~BIT(6));
            plog_hex("USB root verified bytes",session.total);plog_flush();
            return loader_boot_resident(dtb,payload,size,base,session.total);
        }
    }
}
