/* Experimental MT6993 USB2 EP0 PIO receiver.
 * MMIO addresses: captured X9 iomem/FDT; register semantics: Linux v6.1
 * drivers/usb/mtu3/{mtu3_hw_regs.h,mtu3_core.c,mtu3_gadget_ep0.c}.
 * Restore the X9 vendor XS-PHY core power-on state; preserve inherited clocks.
 * No Qualcomm DWC3 or MT6789 power sequence.
 * This first transport uses control transfers, not an unverified QMU DMA ring.
 */
#include "hw.h"
#include "plog.h"
#include "usb_log_protocol.h"
#include "usb_phy_lk.h"

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

static uint8_t control[X9LOG_BATCH_FRAME];
static unsigned transferred,wanted,in_length,configured;
static enum { IDLE, SEND, SEND_END } phase;
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
static uint64_t usb_log_pos(void *ctx){(void)ctx;return plog_position();}
static uint32_t usb_log_copy(void *ctx,uint64_t *cursor,char *out,uint32_t max)
{(void)ctx;return plog_copy(cursor,out,max);}
static const struct x9log_reader usb_log_reader={0,usb_log_pos,usb_log_copy};
static unsigned string_descriptor(unsigned index)
{
    static const char strings[][24]={"","HelioDarwin","X9 shim log","X9SHIMLOG1"};
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
    phase=IDLE;transferred=0;wanted=length;
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
        configured=value;
    } else if (type==0x80 && request==8 && length==1 && !value && !index) {
        control[0]=configured;count=1;
    } else if ((type==0x80 || type==0x81) && request==0 && length==2 && !value && !index) {
        control[0]=control[1]=0;count=2;
    } else if (type==0x81 && request==10 && length==1 && !value && !index) {
        control[0]=0;count=1;
    } else if (type==1 && request==11 && !length && !value && !index) {
        /* One interface, alternate setting zero. */
    } else if (type==0xc0 && request==X9LOG_BATCH_REQ && configured && length==X9LOG_BATCH_FRAME) {
        int n=x9log_read_capacity(&usb_log_reader,(uint32_t)value|((uint32_t)index<<16),control,sizeof(control),1);
        if(n<0){stall(0);return;}
        count=(unsigned)n;
    } else if (type==0xc0 && request==X9LOG_REQ && configured && length==64) {
        int n=x9log_read(&usb_log_reader,(uint32_t)value|((uint32_t)index<<16),control,1);
        if(n<0){stall(0);return;}
        count=(unsigned)n;
    } else { stall(length?0:SETUP);return; }
    if (!length) { wr(EP0,epcsr()|SETUP|DATAEND);return; }
    if (type&0x80) {
        in_length=count<length?count:length;
        wr(EP0,epcsr()|SETUP|DPHTX);phase=SEND;transmit();
    } else stall(0);
}
static void poll_ep0(void)
{
    uint32_t interrupt=rr(EPISR);
    if (!(interrupt&(BIT(0)|BIT(16)))) return;
    wr(EPISR,interrupt&(BIT(0)|BIT(16)));
    uint32_t csr=rr(EP0);
    if (interrupt&BIT(16)) { phase=IDLE;transferred=0; }
    if (csr&SENTSTALL) {
        wr(EP0,(csr&~(W1C|STALL))|SENTSTALL);
        phase=IDLE;
        csr=rr(EP0); /* Process a new SETUP using the post-ack state. */
    }
    if (csr&SETUP) {
        if (rr(0x108)!=8) {stall(SETUP);return;}
        setup();return;
    }
    if ((phase==SEND || phase==SEND_END) && !(csr&TX)) {
        if (phase==SEND_END) { wr(EP0,epcsr()|DATAEND);phase=IDLE; }
        else transmit();
    }
}

static int ready;static uint64_t last_poll;
void x9_shim_usb_poll(void)
{
 if(!ready)return;uint64_t now=rsr("cntpct_el0");uint64_t interval=counter_frequency()/5000;
 if(now-last_poll<interval)return;last_poll=now;
 uint32_t common=rr(COMMON);if(common)wr(COMMON,common);
 if(common&(BIT(2)|BIT(5))){phase=IDLE;configured=0;transferred=0;wr(CONFIG,rr(CONFIG)&~(0x7fU<<24));wr(EP0,64);}
 poll_ep0();
}
void x9_shim_usb_start(void)
{
 wr32(IPPC+8,rd32(IPPC+8)&~BIT(0));
 wr32(IPPC+0x50,(rd32(IPPC+0x50)&~(BIT(0)|BIT(1)|BIT(2)|BIT(9)))|BIT(7)|BIT(11)|BIT(12));
 wr32(IPPC+0x50,rd32(IPPC+0x50)&~BIT(7));__asm__ volatile("dsb sy":::"memory");
 spin_ms(1);
 if((rd32(IPPC+0x10)&(BIT(0)|BIT(8)))!=(BIT(0)|BIT(8))||!(rd32(IPPC+0x14)&BIT(0))){plog_str("USB shim clock unavailable\n");return;}
 x9_usb_phy_power_on(0,phy_read,phy_write,phy_delay);x9_usb_device_reset(0,phy_read,phy_write,phy_delay);
 uint32_t info=rr(0xc10);if(!(info&31)||!((info>>8)&31)){plog_str("USB shim MAC unavailable\n");return;}
 wr(POWER,rr(POWER)&~BIT(6));spin_ms(30);wr(0x141c,rr(0x141c)&~BIT(0));wr(0xc,0xffffffff);wr(0x8c,0xffffffff);wr(CONFIG,rr(CONFIG)&~(0x7fU<<24));wr(EP0,64);wr(EPISR,0x10001);wr(COMMON,0xffffffff);wr(0x88,0x10001);wr(0x2418,BIT(2)|BIT(5));wr(POWER,rr(POWER)|BIT(6)|BIT(5)|BIT(0));phase=IDLE;configured=0;ready=1;plog_str("USB shim log ready\n");
}

/* End shim ownership before XNU can touch the controller. No external reset. */
void x9_shim_usb_stop(void)
{
 if(!ready)return;
 ready=0; /* wdt_kick must not enter poll while teardown is in progress */
 wr(POWER,rr(POWER)&~BIT(6)); /* detach before resetting any software state */
 wr(0x0c,0xffffffffu);       /* mask level-1 interrupt sources */
 wr(0x8c,0xffffffffu);       /* mask endpoint interrupt sources */
 wr(0x2418,0);               /* mask USB2 common interrupt sources */
 wr(EPISR,0xffffffffu);      /* W1C outstanding endpoint events */
 wr(COMMON,0xffffffffu);     /* W1C outstanding USB2 events */
 phase=IDLE;configured=0;transferred=0;
 spin_ms(30);                /* allow the host to observe the disconnect */
}
