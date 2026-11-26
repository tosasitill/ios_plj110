/* MT6993/MT6653 board worker. Complete session only; no ROM probe, no raw
 * user-client injection competing with native H4. SSPM post-download handoff
 * requires current hardware evidence, never a fabricated ready result. */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <arm64/sweet_early.h>
#include "x9_bluetooth.h"
#include "x9_bt_serial.h"
#include "x9_bt_hardware.h"
#include "../../../generated/x9_bt_firmware.inc"
extern int sweet_x9_arg(const char *, int);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static LCK_GRP_DECLARE(bt_group,"x9_bluetooth");
static LCK_MTX_DECLARE(bt_lock,&bt_group);
static LCK_SPIN_DECLARE(bt_rx_lock,&bt_group);
static struct x9_bt_status bs={.version=X9_BT_ABI_VERSION};
static struct x9_bt_session_ops ops;
static struct x9_bt_session_result session;
static int started, session_stage=X9_BT_SESSION_INPUT;
/* One synchronous control request; only the UART worker ever executes it.
 * 0 awake, 1 asleep, 2 initializing/transition/unavailable. Not system suspend:
 * there is no FW wake callback, so only a CLOSED native transport may sleep. */
static struct {
    bool live,busy,pending,done;
    int state,target,error;
} power={.state=2,.error=ENXIO};
SYSCTL_INT(_kern, OID_AUTO, x9_bt_stage, CTLFLAG_RD|CTLFLAG_LOCKED,
           &session_stage, 0, "Last MT6653 initialization stage (not ready status)");
static volatile uint32_t *uart, *gic;
#define RX_RING 32768u
/* IRQ copies bytes only; parsing, serial locks and WMT run on worker. */
static struct {
    uint8_t bytes[RX_RING];
    uint32_t head,len;
    int error;
    bool owned;
    uint64_t received,errors,interrupts;
} rx;
static uint64_t route_saved;
static uint64_t route_owned;
static uint8_t priority_saved;
static bool irq_captured;
/* production DT UART3 SPI0x148, level-high. Wake SPI0x246 is NOT enabled:
 * no actual resume/wakeup callback, so session never enables FW wake-enhance. */
enum { UART_INTID=0x148+32, IRQ_BIT=1u<<(UART_INTID%32), BANK=UART_INTID/32,
    G_GROUP=0x80+4*BANK, G_ENABLE=0x100+4*BANK, G_DISABLE=0x180+4*BANK,
    G_PENDING=0x200+4*BANK, G_ACTIVE=0x300+4*BANK,
    G_CONFIG=0xc00+4*(UART_INTID/16), G_PRIORITY=0x400+UART_INTID,
    G_ROUTE=0x6000+8*UART_INTID };
/* Runtime ACL capacity reported by the actual controller is4096 payload
 * bytes, which needs4101 on H4; the old bootstrap4096 cap cuts off its tail.
 * Keep the metadata ABI constant but size worker framing independently.
 * This also covers the actual1504 B ISO payload without fabricating MTUs. */
#define BT_WIRE_FRAME_MAX 8192u
struct bt_frame { uint32_t length; uint8_t data[BT_WIRE_FRAME_MAX]; };
static struct bt_frame assembling, pending[8];
/* Sole worker owns these; no 8KiB packet is allocated on the kernel stack. */
static struct bt_frame incoming;
/* Bounded native HCI trace (log bt4: 203b Command Complete never seen by
 * bluetoothd). x9_bt_hci_trace=N packets, default 400, 0 disables. */
static int hci_trace_left=-1;
static void hci_trace(const char *dir,const uint8_t *p,uint32_t n)
{
    if(hci_trace_left<0) {
        int v=400; if(!PE_parse_boot_argn("x9_bt_hci_trace",&v,sizeof(v))) v=400;
        hci_trace_left=v<0?0:v;
    }
    if(!hci_trace_left) return;
    hci_trace_left--;
    char hex[16*3+1]; uint32_t m=n<16?n:16; hex[0]=0;
    for(uint32_t i=0;i<m;i++) snprintf(hex+3*i,4,"%02x ",p[i]);
    printf("X9 HCI %s len=%u %s\n",dir,n,hex);
}
static uint8_t native_tx[BT_WIRE_FRAME_MAX];
/* Stall probe state (log bt5): last native HCI command and whether any
 * controller frame has arrived since it left the UART. */
static uint8_t stall_cmd[264];
static uint32_t stall_cmd_len;
static uint64_t stall_cmd_ms;
static bool stall_outstanding, stall_probed;
static unsigned stall_probes;
static unsigned pending_count;
static uint64_t frame_start;
static uint32_t rd(volatile uint32_t *p,unsigned off) { return p[off/4]; }
static void wr(volatile uint32_t *p,unsigned off,uint32_t v)
{ p[off/4]=v; __asm__ volatile("dsb sy" ::: "memory"); }
static boolean_t rx_lock(void)
{ boolean_t old=ml_set_interrupts_enabled(FALSE); lck_spin_lock(&bt_rx_lock); return old; }
static void rx_unlock(boolean_t old)
{ lck_spin_unlock(&bt_rx_lock); ml_set_interrupts_enabled(old); }
static uint64_t now_ms(void *context)
{
    (void)context; uint64_t ticks,ns; clock_get_uptime(&ticks);
    absolutetime_to_nanoseconds(ticks,&ns); return ns/1000000;
}
static void sleep_ms(void *context,uint32_t ms)
{ (void)context; delay_for_interval(ms,1000*NSEC_PER_USEC); }
static void state(enum x9_bt_state s,int error)
{
    lck_mtx_lock(&bt_lock); bs.state=s; bs.error=error;
    if(error) bs.firmware_ready=0;
    lck_mtx_unlock(&bt_lock);
}
/* A real IAR is the only invocation; no timer produces synthetic deliveries. */
bool x9_bluetooth_irq_filter(uint32_t intid)
{
    if(intid!=UART_INTID) return false;
    boolean_t old=rx_lock();
    if(!rx.owned) {
        /* A captured SPI can have an in-flight IAR after masking. Consume
         * our interrupt, but never touch the gated UART while asleep. */
        bool captured=irq_captured; rx_unlock(old); return captured;
    }
    rx.interrupts++;
    for(unsigned n=0;n<512;n++) {
        uint32_t lsr=rd(uart,0x14);
        if(lsr&0x1e) { rx.errors++; rx.error=EIO; break; }
        if(!(lsr&1)) break;
        uint8_t byte=(uint8_t)rd(uart,0);
        if(rx.len==RX_RING) { rx.error=EOVERFLOW; break; }
        rx.bytes[(rx.head+rx.len)%RX_RING]=byte; rx.len++; rx.received++;
    }
    if(rx.error) { wr(uart,4,0); wr(gic,G_DISABLE,IRQ_BIT); }
    rx_unlock(old); thread_wakeup((event_t)&rx.len); return true;
}
static int irq_bind(void)
{
    int error=x9_bt_hardware_uart(&uart); if(error) return error;
    DTEntry entry; const void *p; unsigned n;
    if(SecureDTLookupEntry(NULL,"/arm-io/gic",&entry)!=kSuccess ||
       SecureDTGetProperty(entry,"reg",&p,&n)!=kSuccess || !p || n!=32) return ENXIO;
    uint64_t reg[4]; memcpy(reg,p,sizeof(reg));
    if(reg[0]!=0x06800000-0x400000ULL || reg[1]!=0x40000 ||
       reg[2]!=0x06880000-0x400000ULL || reg[3]!=0x200000) return ENXIO;
    gic=(volatile uint32_t *)ml_io_map(0x06800000,0x40000); if(!gic) return ENOMEM;
    /* Non-secure view, two security states: CTLR bit4 ARE_NS + bit1
     * EnableGrp1A; bit0 is RES0 (pe_fiq sets it, reads back 0x12, log 324).
     * IGROUPR is not checked: this GIC reads group bits as zero from NS even
     * for delivered Group 1 NS interrupts (pe_fiq.c, GICR_IGROUPR0); Android
     * 8250_mtk takes this SPI328 as a normal NS IRQ. Route/priority/enable
     * readbacks below still refuse a line NS cannot actually program. */
    if((rd(gic,0)&0x12)!=0x12 || (rd(gic,0)&(1u<<31)) ||
       (((rd(gic,4)&31)+1)*32)<=UART_INTID ||
       (rd(gic,G_CONFIG)&(3u<<((UART_INTID%16)*2))) ||
       ((rd(gic,G_ENABLE)|rd(gic,G_ACTIVE)|rd(gic,G_PENDING))&IRQ_BIT)) {
        printf("X9 BT: irq_bind GIC refused ctlr=%x typer=%x group=%x config=%x enable=%x active=%x pending=%x bit=%x\n",
               rd(gic,0),rd(gic,4),rd(gic,G_GROUP),rd(gic,G_CONFIG),rd(gic,G_ENABLE),
               rd(gic,G_ACTIVE),rd(gic,G_PENDING),IRQ_BIT);
        return EBUSY;
    }
    uint32_t ier=rd(uart,4), dma=rd(uart,0x4c), lsr=rd(uart,0x14);
    if(ier || dma) {
        printf("X9 BT: irq_bind AP UART3 in use ier=%x dma_en=%x lsr=%x\n",ier,dma,lsr);
        return EBUSY;
    }
    /* First bind runs before this session's first TX, so nothing in the RX
     * FIFO can be a reply. Power-on/pinmux (RX pulled down until the chip
     * drives it) can latch a break or junk byte; Android's tty path also
     * tolerates pre-command dirty data. Discard a bounded amount, report it,
     * and still refuse a line that keeps producing data or never idles. */
    uint32_t lsr_seen=lsr; unsigned dropped=0; uint8_t first[8]={0};
    while((lsr&1) && dropped<64) {
        uint8_t b=(uint8_t)rd(uart,0); if(dropped<8) first[dropped]=b;
        dropped++; lsr=rd(uart,0x14); lsr_seen|=lsr;
    }
    if(dropped || (lsr_seen&0x1e))
        printf("X9 BT: irq_bind discarded %u pre-command RX bytes lsr_seen=%x first=%02x %02x %02x %02x %02x %02x %02x %02x\n",
               dropped,lsr_seen,first[0],first[1],first[2],first[3],first[4],first[5],first[6],first[7]);
    if((lsr&1) || !(lsr&0x40)) {
        printf("X9 BT: irq_bind AP UART3 not idle lsr=%x dropped=%u\n",lsr,dropped);
        return EBUSY;
    }
    route_saved=*(volatile uint64_t *)((uintptr_t)gic+G_ROUTE);
    priority_saved=*(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY); irq_captured=true;
    uint64_t mpidr=__builtin_arm_rsr64("MPIDR_EL1")&0xff00ffffffULL;
    route_owned=mpidr;
    *(volatile uint64_t *)((uintptr_t)gic+G_ROUTE)=mpidr;
    *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY)=0xa0;
    __asm__ volatile("dsb sy" ::: "memory");
    if(*(volatile uint64_t *)((uintptr_t)gic+G_ROUTE)!=mpidr ||
       *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY)!=0xa0) {
        printf("X9 BT: irq_bind GIC route/priority not NS-writable route=%llx/%llx prio=%x saved=%llx/%x\n",
               *(volatile uint64_t *)((uintptr_t)gic+G_ROUTE),mpidr,
               *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY),route_saved,priority_saved);
        return EIO;
    }
    /* 8250 UART64 RX trigger16, RDI + line-status. Not Linux APDMA. The
     * hardware FIFO is empty, drained above; no flush after a command. */
    wr(uart,8,0x41);
    boolean_t old=rx_lock(); rx.owned=true; wr(uart,4,5); wr(gic,G_ENABLE,IRQ_BIT);
    rx_unlock(old);
    for(unsigned i=0;i<500 && (rd(gic,0)&(1u<<31));i++) delay(2);
    if((rd(gic,0)&(1u<<31)) || !(rd(gic,G_ENABLE)&IRQ_BIT)) {
        printf("X9 BT: irq_bind GIC enable not latched ctlr=%x enable=%x pending=%x\n",
               rd(gic,0),rd(gic,G_ENABLE),rd(gic,G_PENDING));
        return EIO;
    }
    printf("X9 BT: irq_bind SPI%u route=%llx prio=a0 enabled\n",UART_INTID-32,route_owned);
    return 0;
}
static void irq_stop(void)
{
    boolean_t old=rx_lock();
    if(rx.owned) { wr(uart,4,0); wr(gic,G_DISABLE,IRQ_BIT); rx.owned=false; }
    rx_unlock(old);
    if(irq_captured) {
        for(unsigned i=0;i<500 && (rd(gic,0)&(1u<<31));i++) delay(2);
        /* Once masked and the filter spin lock is released, no RX handler
         * still uses the route; never clear another SPI's active/pending. */
        *(volatile uint64_t *)((uintptr_t)gic+G_ROUTE)=route_saved;
        *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY)=priority_saved;
        __asm__ volatile("dsb sy" ::: "memory");
        old=rx_lock(); irq_captured=false; rx_unlock(old);
    }
}
/* Preserve our SPI route and ALL received bytes across sleep. No FIFO flush,
 * pending-bit clear, first-bind admission, or writes to SSPM UART banks. */
static int irq_pause(void)
{
    boolean_t old=rx_lock();
    int error=(!irq_captured || !rx.owned)?ENXIO:rx.error;
    if(!error) {
        wr(uart,4,0); wr(gic,G_DISABLE,IRQ_BIT); rx.owned=false;
        if(rx.len || (rd(uart,0x14)&1)) error=EPROTO;
    }
    rx_unlock(old);
    for(unsigned i=0;i<500 && (rd(gic,0)&(1u<<31));i++) delay(2);
    if(!error && ((rd(gic,0)&(1u<<31)) || (rd(gic,G_ENABLE)&IRQ_BIT))) error=EIO;
    return error;
}
static int irq_resume(void)
{
    int error=x9_bt_hardware_check(); if(error) return error;
    boolean_t old=rx_lock();
    if(!irq_captured || rx.owned || (rd(gic,G_ENABLE)&IRQ_BIT) ||
       (rd(gic,G_CONFIG)&(3u<<((UART_INTID%16)*2))) ||
       *(volatile uint64_t *)((uintptr_t)gic+G_ROUTE)!=route_owned ||
       *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY)!=0xa0) error=EBUSY;
    else if(rx.error || (rd(uart,0x14)&0x1e)) error=rx.error?rx.error:EIO;
    else {
        /* A wake pending bit and a nonempty FIFO belong to us. Restore RX
         * before any raw wake/own command, without dropping wake data. */
        rx.owned=true; wr(uart,4,5); wr(gic,G_ENABLE,IRQ_BIT);
    }
    rx_unlock(old);
    for(unsigned i=0;i<500 && (rd(gic,0)&(1u<<31));i++) delay(2);
    if(!error && ((rd(gic,0)&(1u<<31)) || !(rd(gic,G_ENABLE)&IRQ_BIT))) error=EIO;
    return error;
}
static int rx_take(uint8_t *out,uint32_t capacity,uint32_t *n)
{
    boolean_t old=rx_lock(); *n=MIN(rx.len,capacity);
    for(uint32_t i=0;i<*n;i++) out[i]=rx.bytes[(rx.head+i)%RX_RING];
    rx.head=(rx.head+*n)%RX_RING; rx.len-=*n; int error=rx.error;
    rx_unlock(old); return error;
}
static int raw_write(void *context,const uint8_t *p,size_t n,size_t *sent)
{
    (void)context; if(!p || !sent) return EINVAL; *sent=0;
    if(!rx.owned) {
        if(irq_captured) return EHOSTDOWN; /* Resume must restore owned IRQ. */
        int e=irq_bind(); if(e) return e;
    }
    int error=x9_bt_hardware_check(); if(error) return error;
    uint64_t end=now_ms(NULL)+1000;
    while(*sent<n) {
        boolean_t old=rx_lock(); error=rx.error; rx_unlock(old); if(error) return error;
        if(rd(uart,0x14)&0x20) { wr(uart,0,p[*sent]); (*sent)++; }
        else delay(2);
        if(now_ms(NULL)>=end) return ETIMEDOUT;
    }
    lck_mtx_lock(&bt_lock); bs.tx_bytes+=*sent; lck_mtx_unlock(&bt_lock); return 0;
}
static int drain(void *context,uint32_t timeout)
{
    (void)context;
    if(!rx.owned) return EHOSTDOWN;
    int error=x9_bt_hardware_check(); if(error) return error;
    uint64_t end=now_ms(NULL)+timeout;
    while(!(rd(uart,0x14)&0x40)) {
        boolean_t old=rx_lock(); error=rx.error; rx_unlock(old); if(error) return error;
        if(now_ms(NULL)>=end) return ETIMEDOUT; delay(2);
    }
    return 0;
}
static int host_uart(void *context,uint32_t baud,uint8_t flow)
{
    (void)context;
    /* IRQ can mask IER on RX failure. Never race that failure by replaying
     * an earlier IER snapshot during a baud/FEATURE_SEL bank transition. */
    boolean_t old=rx_lock();
    int error=rx.error;
    if(!error) error=x9_bt_hardware_config_uart(baud,flow);
    rx_unlock(old); return error;
}
/* This parser is UART-WIRE only. Active Android btmtk_uart_unify.ko:
 * txcommon .text1825c/183e8..18438 and recv_sco3854/rxworkc864..c8ac.
 * Every SCO packet (not just VTS) translates as follows:
 * native03 AA BB LL payload <-> wire03 AA (LL+2) (LL+1) BB payload.
 * LL is payload length; preserve all high handle/status bits in BB.
 * Command/ACL/Event/ISO remain standard H4, and UART HW owns SW escaping. */
static int frame_length(const uint8_t *p,uint32_t n)
{
    if(!n) return 0;
    switch(p[0]) {
    case 1: return n<4?0:4+p[3];
    case 2: return n<5?0:5+p[3]+((unsigned)p[4]<<8);
    case 3:
        if(n<4) return 0;
        if(p[2]<2 || p[3]<1 || (unsigned)p[2]!=(unsigned)p[3]+1) return -EPROTO;
        if((unsigned)p[2]-2>X9_BT_SCO_PAYLOAD_MAX) return -EMSGSIZE;
        return n<5?0:5+(unsigned)p[2]-2;
    case 4: return n<3?0:3+p[2];
    case 5: return n<5?0:5+p[3]+((unsigned)(p[4]&0x3f)<<8);
    case 0x40: case 0x41: return n<5?0:5+p[3]+((unsigned)p[4]<<8); /* H4_RECV_RHW_* */
    default: return -EPROTO;
    }
}
static void sco_to_native(struct bt_frame *frame)
{
    /* Called only after strict wire header and whole-frame admission. */
    if(frame->data[0]!=3) return;
    uint8_t high=frame->data[4],length=frame->data[2]-2;
    memmove(frame->data+4,frame->data+5,length);
    frame->data[2]=high; frame->data[3]=length; frame->length--;
}
static int native_to_wire(uint32_t n,uint32_t *wire_length)
{
    if(!n || !wire_length || n>sizeof(native_tx)) return EINVAL;
    *wire_length=n;
    if(native_tx[0]!=3) return 0;
    if(n<4 || n!=4u+native_tx[3]) return EPROTO;
    if(native_tx[3]>X9_BT_SCO_PAYLOAD_MAX || n==sizeof(native_tx)) return EMSGSIZE;
    uint8_t high=native_tx[2],length=native_tx[3];
    memmove(native_tx+5,native_tx+4,length);
    native_tx[2]=length+2; native_tx[3]=length+1; native_tx[4]=high;
    *wire_length=n+1; return 0;
}
static int next_frame(struct bt_frame *out)
{
    for(;;) {
        /* Expire a stalled partial frame even when no more bytes arrive. */
        if(assembling.length && now_ms(NULL)-frame_start>1000) return ETIMEDOUT;
        uint8_t b; uint32_t count; int error=rx_take(&b,1,&count); if(error) return error;
        if(!count) return EAGAIN;
        /* btmtktty raw_filter, production node==1: only boundary0xff
         * (FW wake) and0x00 (UART noise). Never trim either byte from a
         * payload, or use the node==0-only0xf0 filter on this controller.
         * The power queue never calls this parser while UART is gated. */
        if(!assembling.length && (b==0xff || b==0)) continue;
        if(!assembling.length) frame_start=now_ms(NULL);
        if(assembling.length>=sizeof(assembling.data)) return EMSGSIZE;
        assembling.data[assembling.length++]=b;
        int frame_bytes=frame_length(assembling.data,assembling.length);
        if(frame_bytes<0) return -frame_bytes;
        if(frame_bytes>(int)sizeof(assembling.data)) return EMSGSIZE;
        if(frame_bytes && frame_bytes==(int)assembling.length) {
            sco_to_native(&assembling);
            *out=assembling; assembling.length=0;
            lck_mtx_lock(&bt_lock); bs.rx_frames++; lck_mtx_unlock(&bt_lock); return 0;
        }
        if(now_ms(NULL)-frame_start>1000) return ETIMEDOUT;
    }
}
static int exchange_frame(void *context,const uint8_t *tx,size_t txlen,
                         uint8_t *reply,size_t *replylen,uint32_t timeout,bool hci)
{
    (void)context;
    uint16_t opcode;
    if(!tx || !reply || !replylen) return EINVAL;
    if(hci) {
        if(txlen<4 || tx[0]!=1 || txlen!=(size_t)tx[3]+4) return EINVAL;
        opcode=tx[1]|((uint16_t)tx[2]<<8);
    }
    else if(txlen==1 && tx[0]==0xff) opcode=3;
    else if(txlen>=6 && tx[0]==1) opcode=tx[5];
    else if(txlen>=7 && tx[0]==2) opcode=tx[6];
    else return EINVAL;
    size_t sent; int error=raw_write(NULL,tx,txlen,&sent); if(error) return error;
    if(sent!=txlen) return EIO;
    uint64_t end=now_ms(NULL)+timeout;
    for(;;) {
        error=next_frame(&incoming);
        if(error && error!=EAGAIN) return error;
        if(!error) {
            bool matches=false;
            if(incoming.data[0]==4) {
                if(!hci) matches=incoming.length>=5 && incoming.data[1]==0xe4 && incoming.data[4]==opcode;
                else if(incoming.length>=7 && incoming.data[1]==0x0e)
                    matches=(incoming.data[4]|((uint16_t)incoming.data[5]<<8))==opcode;
                else if(incoming.length==7 && incoming.data[1]==0x0f)
                    matches=(incoming.data[5]|((uint16_t)incoming.data[6]<<8))==opcode;
            }
            if(matches) {
                if(*replylen<incoming.length) { *replylen=incoming.length; return EMSGSIZE; }
                memcpy(reply,incoming.data,incoming.length); *replylen=incoming.length; return 0;
            }
            /* Preserve unrelated complete packets; don't let a native/raw
             * client steal the decisive initialization ACK. */
            if(pending_count==8) return ENOBUFS;
            pending[pending_count++]=incoming;
        }
        if(now_ms(NULL)>=end) return ETIMEDOUT;
        if(error==EAGAIN) sleep_ms(NULL,1);
    }
}
static int exchange(void *context,const uint8_t *tx,size_t txlen,
                    uint8_t *reply,size_t *replylen,uint32_t timeout)
{ return exchange_frame(context,tx,txlen,reply,replylen,timeout,false); }
static int hci_exchange(void *context,const uint8_t *tx,size_t txlen,
                        uint8_t *reply,size_t *replylen,uint32_t timeout)
{ return exchange_frame(context,tx,txlen,reply,replylen,timeout,true); }
static int connfem(void *context) { (void)context; return x9_bt_session_connfem(&ops.io); }
void x9_bluetooth_status(struct x9_bt_status *out)
{
    lck_mtx_lock(&bt_lock); *out=bs; lck_mtx_unlock(&bt_lock);
    boolean_t old=rx_lock(); out->rx_bytes=rx.received; out->uart_errors=rx.errors; rx_unlock(old);
}
int x9_bluetooth_section(uint32_t n,struct x9_bt_section *out)
{ if(!out || n>=X9_BT_FW_SECTIONS) return ENOENT; *out=x9_bt_fw_sections[n]; return 0; }
/* Bootstrap IOUserClient remains a metadata interface only. The serial path
 * is exclusive and is the one transport the native daemon may own. */
int x9_bluetooth_send(const void *p,uint32_t n) { (void)p; (void)n; return ENOTSUP; }
int x9_bluetooth_receive(void *p,uint32_t *n) { (void)p; (void)n; return ENOTSUP; }
static int calibration(struct x9_bt_session_board *board)
{
    static const char *const names[]={"x9_wifi_xo0","x9_wifi_xo1","x9_wifi_xo2",
        "x9_wifi_xo3","x9_wifi_xo4","x9_wifi_xo5","x9_wifi_xo6"};
    uint32_t flow = sweet_x9_arg("x9_bt_fc", 0);
    /* uart_launcher -f and HCIUARTSETBAUD supply the production selection;
     * its CLI default is not proof of this device's effective config. This
     * value is the Android UART_FC enum, not the WMT flow-control byte.
     * Never silently choose NONE, or start power/download for an AP flow
     * mode whose IRQ/escape implementation is not available yet. */
    if(flow>3) return EINVAL;
    if(flow!=0 && flow!=1) return ENOTSUP; /* Linux=2/HW=3 still lack backend. */
    uint8_t record[28];
    for(unsigned k=0;k<7;k++) {
        uint32_t v;
        if(!PE_parse_boot_argn(names[k],&v,sizeof(v))) return ENOENT;
        for(unsigned j=0;j<4;j++) record[k*4+j]=(uint8_t)(v>>(8*j));
    }
    if(record[0]!=25 || record[1]) return EINVAL;
    memcpy(board->xo,record+2,25); board->calibration_present=true;
    board->platform_config=0x00158000; /* MT6653 dx5_2g2a co-clock0, real board config */
    board->flow_control=flow==1?0x80:0; /* explicit enum -> WMT byte mapping */
    board->wakeup_callback=false; return 0;
}
static void power_publish(bool asleep)
{
    x9_bt_serial_result(asleep?EHOSTDOWN:0,session.baud);
    lck_mtx_lock(&bt_lock);
    bs.state=asleep?X9_BT_ASLEEP:X9_BT_READY;
    bs.error=asleep?EHOSTDOWN:0; bs.firmware_ready=1;
    lck_mtx_unlock(&bt_lock);
}
static int ownership_rx_barrier(bool wake_ack)
{
    /* MT6653 raw FF and WMT own-clear produce the SAME ten-byte ACK.
     * Android's no-event FF send does not suppress its reply. Consume the
     * one FF reply BEFORE sending own-clear, not after it as a false proof
     * of the new transaction. No FF was sent => no old own ACK is allowed.
     * Missing/ambiguous wake replies fail closed; never send another FF. */
    static const uint8_t ack[]={4,0xe4,7,2,3,3,0,0,3,1};
    bool seen=false; uint64_t end=now_ms(NULL)+1000;
    for(;;) {
        int error=next_frame(&incoming);
        if(error && error!=EAGAIN) return error;
        if(!error) {
            bool own=incoming.length>=5 && incoming.data[0]==4 &&
                incoming.data[1]==0xe4 && incoming.data[4]==3;
            if(own) {
                if(!wake_ack || seen || incoming.length!=sizeof(ack) ||
                   memcmp(incoming.data,ack,sizeof(ack))) return EPROTO;
                seen=true;
            } else {
                if(pending_count==8) return ENOBUFS;
                pending[pending_count++]=incoming;
            }
        } else if(!assembling.length && (!wake_ack || seen)) return 0;
        if(now_ms(NULL)>=end) return ETIMEDOUT;
        if(error==EAGAIN) sleep_ms(NULL,1);
    }
}
static int power_execute(int target,bool have_rx,bool *fatal)
{
    *fatal=false;
    if(target==1 && (have_rx || assembling.length || pending_count)) return EBUSY;
    int error=x9_bt_serial_power_begin(target==1);
    if(error) { *fatal=error!=EBUSY; return error; }
    lck_mtx_lock(&bt_lock); bs.state=X9_BT_TRANSITION; bs.error=EBUSY; lck_mtx_unlock(&bt_lock);
    if(target==1) {
        error=x9_bt_session_fw_own(&ops,&session);
        if(error==EBUSY && session.driver_owned) {
            /* Valid FW refusal: no resources/IRQ were released. */
            power_publish(false); return EBUSY;
        }
        if(!error) error=drain(NULL,1000);
        /* exchange uses sole-worker incoming. Never overwrite a retained
         * native frame, or release the bus with new undecoded HCI traffic. */
        if(!error && (pending_count || assembling.length)) error=EPROTO;
        if(!error) error=irq_pause();
        if(!error) error=x9_bt_hardware_sleep();
    } else {
        error=x9_bt_hardware_wake();
        if(!error) error=irq_resume();
        if(!error) error=ownership_rx_barrier(false);
        bool awake=false,need_ff=false;
        if(!error) error=x9_bt_hardware_awake(&awake,&need_ff);
        bool sent_ff=false;
        if(!error && need_ff) {
            const uint8_t ff=0xff; size_t sent=0;
            error=raw_write(NULL,&ff,1,&sent);
            if(!error && sent!=1) error=EIO;
            if(!error) error=drain(NULL,1000);
            sent_ff=!error;
        }
        uint64_t end=now_ms(NULL)+1000;
        while(!error && !awake) {
            error=x9_bt_hardware_awake(&awake,&need_ff);
            if(!error && !awake) {
                if(now_ms(NULL)>=end) { error=ETIMEDOUT; break; }
                sleep_ms(NULL,1);
            }
        }
        if(!error) error=ownership_rx_barrier(sent_ff);
        if(!error) error=x9_bt_session_driver_own(&ops,&session);
        if(!error) error=x9_bt_hardware_ownership_finish();
    }
    if(error) { *fatal=true; return error; }
    power_publish(target==1);
    printf("X9 BT: explicit transport %s; actual ownership and SSPM requests verified\n",
        target==1?"sleep":"resume");
    return 0;
}
static int power_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    lck_mtx_lock(&bt_lock); int value=power.state; lck_mtx_unlock(&bt_lock);
    int error=SYSCTL_OUT(req,&value,sizeof(value));
    if(error || !req->newptr) return error;
    /* launchd's actual bluetoothd/BlueTool identity is mobile. ANYBODY only
     * bypasses generic root-only sysctl admission; keep this interface owned
     * by root/mobile, just like the tty, and expose no arbitrary HCI. */
    uid_t uid=kauth_getuid(); if(uid!=0 && uid!=501) return EPERM;
    error=SYSCTL_IN(req,&value,sizeof(value)); if(error) return error;
    if(value!=0 && value!=1) return EINVAL;
    lck_mtx_lock(&bt_lock);
    if(!power.live) error=power.error?power.error:ENXIO;
    else if(power.busy || power.state==2) error=EBUSY;
    else if(value==power.state) error=0; /* Idempotent; never reboot/download. */
    else {
        int previous=power.state;
        power.busy=true; power.pending=true; power.done=false;
        power.target=value; power.state=2; power.error=0;
        wakeup(&power.pending);
        /* Accepted work cannot be abandoned by a signal/timeout and later
         * mutate hardware behind a caller that thought it was cancelled.
         * Every physical operation on worker is itself finitely bounded. */
        while(!power.done) (void)msleep(&power.done,&bt_lock,PZERO,"x9btpower",NULL);
        error=power.error;
        if(error && power.live) power.state=previous;
        power.busy=false;
    }
    lck_mtx_unlock(&bt_lock); return error;
}
/* launchd jobs' stderr is /dev/null (log 328: bluetoothd exits 1 with no
 * trace). The H4 bridge, vendor adapter and launcher mirror their own
 * messages here; bounded, sanitized, root/mobile only, no HCI access. */
static SInt32 bt_notes;
static int note_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    if(!req->newptr) return 0;
    uid_t uid=kauth_getuid(); if(uid!=0 && uid!=501) return EPERM;
    char text[192]; size_t n=req->newlen; if(n>=sizeof(text)) n=sizeof(text)-1;
    int error=SYSCTL_IN(req,text,n); if(error) return error;
    text[n]=0;
    for(size_t i=0;i<n && text[i];i++) if(text[i]<32 || text[i]>126) text[i]='?';
    if(OSIncrementAtomic(&bt_notes)>=400) return 0;
    char name[24]; proc_selfname(name,sizeof(name));
    printf("X9 BT USER: %s[%d] %s\n",name,proc_selfpid(),text);
    return 0;
}
SYSCTL_PROC(_kern,OID_AUTO,x9_bt_note,CTLTYPE_STRING|CTLFLAG_WR|CTLFLAG_ANYBODY|CTLFLAG_LOCKED,
    NULL,0,note_sysctl,"A","X9 Bluetooth userland bridge messages (stderr is /dev/null)");
SYSCTL_PROC(_kern,OID_AUTO,x9_bt_power,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_ANYBODY|CTLFLAG_LOCKED,
    NULL,0,power_sysctl,"I","Explicit closed-transport ownership: 0 awake, 1 asleep, 2 unavailable/transition");
/* Android btmtk_uart_driver_own, run once per stalled command: snapshot,
 * DEV0_HOST_AWAKE, 0xFF when DEV0_BT_AWAKE=0, own-clear WMT 6f/03 and its
 * E4 ACK. Frames collected meanwhile stay in pending[] for the daemon. The
 * stalled command is retransmitted once only if the firmware answered. */
/* Android mt6653_rhw_dumpReg (mt6653_bt_dbg_sop.h v20250417): RHW packets
 * 0x40 write / 0x41 read, {type,0,0,8,0,addr32,val32}; reply echoes the
 * first 9 bytes, value at [9]. Select via 0x20023a0c, read 0x20023a10. */
static const uint32_t rhw_sel_j[]={0xc0043400,0xc0043000};
static const uint32_t rhw_sel_a[]={0xc0040200,0xc0040201,0xc0040202,0xc0040203,0xc0040204,0xc0040205,0xc0040206,0xc0040207,0xc0040208,0xc0040209,0xc004020a,0xc004020b,0xc004020c,0xc004020d,0xc004020e,0xc004020f,0xc0040210,0xc0040211,0xc0040212,0xc0040213,0xc0040214,0xc0040215,0xc0040216,0xc0040217,0xc0040218,0xc0040219,0xc004021a,0xc004021b,0xc004021c,0xc004021d,0xc004021e,0xc004021f,0xc0040220,0xc0040221,0xc0040222,0xc0040223,0xc0040224,0xc0040225,0xc0040400,0xc0040401,0xc0040402,0xc0040403};
static const uint32_t rhw_sel_d[]={0xc0040e00,0xc0040e01,0xc0040e02,0xc0040e03,0xc0040e04,0xc0040e05,0xc0040e06,0xc0040e07,0xc0040e08,0xc0040e09,0xc0040e0a,0xc0040e0b,0xc0040e0c,0xc0040e0d,0xc0040e0e,0xc0040e0f,0xc0040e10,0xc0040e11,0xc0040e12,0xc0040e13,0xc0040e14,0xc0040e15,0xc0040e16,0xc0040e17,0xc0040e18,0xc0040e19,0xc0040e1a,0xc0040e1b,0xc0040e1c,0xc0040e1d,0xc0040e1e,0xc0040e1f,0xc0040e20,0xc0040e21,0xc0040e22,0xc0040e23,0xc0040e24,0xc0040e25,0xc0040e26,0xc0040e27,0xc0040e28,0xc0040e29,0xc0040e2a,0xc0040e2b,0xc0040e2c,0xc0040e2d,0xc0040e2e,0xc0040e2f,0xc0040e30,0xc0040e31,0xc0040e32,0xc0040e33,0xc0040e34,0xc0040e35,0xc0040e36,0xc0040e37,0xc0040e38,0xc0040e39,0xc0040e3a,0xc0040e3b,0xc0040e3c,0xc0040e3d,0xc0040e3e,0xc0040e3f,0xc0040e40,0xc0040e41,0xc0040e42,0xc0040e43,0xc0040e44,0xc0040e45,0xc0040e46,0xc0040e47,0xc0040e48,0xc0040e49,0xc0040e4a,0xc0040e4b,0xc0040e4c,0xc0040e4d,0xc0040e4e,0xc0040e4f,0xc0040e50,0xc0040e51,0xc0040e52,0xc0040e53,0xc0040e54};
static const uint32_t rhw_sel_i[]={0xc0042600,0xc0042601,0xc0042602,0xc0042603,0xc0042604,0xc0042605,0xc0042606,0xc0042607,0xc0042608,0xc0042609,0xc004260a,0xc0042800,0xc0042a00,0xc0042c00,0xc0042e00,0xc0042e01,0xc0042e02,0xc0042e03,0xc0042e04,0xc0042e05,0xc0042e06,0xc0042e07,0xc0042e08,0xc0042e09,0xc0042e0a,0xc0042e0b,0xc0042e0c,0xc0042e0d,0xc0042e10,0xc0042e11,0xc0042e12,0xc0042e13,0xc0042e14,0xc0042e15,0xc0042e16,0xc0042e17,0xc0042e18,0xc0042e19,0xc0042e1a,0xc0042e1b,0xc0042e1c,0xc0042e1d,0xc0043000,0xc0043001,0xc0043002,0xc0043003,0xc0043004,0xc0043005,0xc0043006,0xc0043007,0xc0043008,0xc0043009,0xc004300a,0xc004300b,0xc004300c,0xc004300d,0xc004300e,0xc004300f,0xc004301e};
static const uint32_t rhw_sel_g[]={0xc0041600,0xc0041601,0xc0041602,0xc0041603};
static unsigned rhw_fail;
static int rhw_op(uint8_t type,uint32_t addr,uint32_t val,uint32_t *out)
{
    uint8_t tx[13]={type,0,0,8,0,(uint8_t)addr,(uint8_t)(addr>>8),(uint8_t)(addr>>16),(uint8_t)(addr>>24),
        (uint8_t)val,(uint8_t)(val>>8),(uint8_t)(val>>16),(uint8_t)(val>>24)};
    if(type==0x41) tx[9]=tx[10]=tx[11]=tx[12]=0;
    size_t sent=0; int error=raw_write(NULL,tx,sizeof(tx),&sent);
    if(error) return error;
    uint64_t end=now_ms(NULL)+100;
    while(now_ms(NULL)<end) {
        int e=next_frame(&incoming);
        if(e==EAGAIN) { sleep_ms(NULL,1); continue; }
        if(e) return e;
        if(incoming.length==13 && !memcmp(incoming.data,tx,9)) {
            if(out) *out=incoming.data[9]|((uint32_t)incoming.data[10]<<8)|
                ((uint32_t)incoming.data[11]<<16)|((uint32_t)incoming.data[12]<<24);
            return 0;
        }
        hci_trace("RX?",incoming.data,incoming.length);
        if(incoming.data[0]!=0x40 && incoming.data[0]!=0x41 && pending_count<8) pending[pending_count++]=incoming;
    }
    return ETIMEDOUT;
}
static void rhw_section(const char *tag,const uint32_t *s,unsigned n)
{
    char line[160]; unsigned off=0,row=0;
    for(unsigned i=0;i<n;i++) {
        uint32_t v=0xdeaddead;
        if(rhw_fail<=4) {
            int e=rhw_op(0x40,0x20023a0c,s[i],NULL);
            if(!e) e=rhw_op(0x41,0x20023a10,0,&v);
            if(e) { rhw_fail++; printf("X9 BT RHW %s sel=%08x errno=%d\n",tag,s[i],e); v=0xdeaddead; }
        }
        if(!off) off=(unsigned)snprintf(line,sizeof(line),"X9 BT RHW %s[%u]",tag,row++);
        off+=(unsigned)snprintf(line+off,sizeof(line)-off," %08x",v);
        if((i&7)==7 || i+1==n) { printf("%s\n",line); off=0; }
    }
}
static void __unused rhw_dump(void)
{
    rhw_fail=0;
    rhw_section("J0",rhw_sel_j,sizeof(rhw_sel_j)/4);
    rhw_section("D0",rhw_sel_d,sizeof(rhw_sel_d)/4);
    rhw_section("J1",rhw_sel_j,sizeof(rhw_sel_j)/4);
    rhw_section("A",rhw_sel_a,sizeof(rhw_sel_a)/4);
    rhw_section("G",rhw_sel_g,sizeof(rhw_sel_g)/4);
    rhw_section("I",rhw_sel_i,sizeof(rhw_sel_i)/4);
    rhw_section("J2",rhw_sel_j,sizeof(rhw_sel_j)/4);
    rhw_section("D1",rhw_sel_d,sizeof(rhw_sel_d)/4);
}
/* btmtk_send_assert_cmd, USE_DEVICE_NODE==1: RHW_ASSERT_CMD writes
 * 0x81028918 (content[9]=0x01 on MT6653). FW then streams its coredump as
 * ACL frames with handle bytes 6f fc (btmtk_fw_log.c), text from byte 4 of
 * the ACL payload, last one ending in "; coredump end". FF/FE 05 frames are
 * FW log. Collect printable runs first; printing to serial would stall RX. */
static char cd_text[24576];
static void __unused fw_assert_dump(void)
{
    uint32_t echo=0; unsigned frames=0,dump=0,logs=0,other=0,perr=0; uint64_t bytes=0; size_t used=0;
    bool ended=false;
    int e=rhw_op(0x40,0x81028918,0x01,&echo);
    printf("X9 BT ASSERT rhw write errno=%d\n",e);
    uint64_t start=now_ms(NULL),end=start+15000,last=start;
    while(!ended && now_ms(NULL)<end && now_ms(NULL)-last<3000) {
        e=next_frame(&incoming);
        if(e==EAGAIN) { sleep_ms(NULL,1); continue; }
        if(e) { perr++; assembling.length=0; if(perr>50) break; continue; }
        frames++; last=now_ms(NULL); bytes+=incoming.length;
        const uint8_t *p=incoming.data; uint32_t n=incoming.length;
        bool cd=n>5 && p[0]==2 && p[1]==0x6f && p[2]==0xfc;
        bool lg=n>5 && p[0]==2 && (p[1]==0xff || p[1]==0xfe) && p[2]==0x05;
        if(cd) dump++; else if(lg) logs++; else { other++; if(other<=6) hci_trace("RX#",p,n); }
        if(cd || lg) {
            uint32_t run=0;
            for(uint32_t i=5;i<=n;i++) {
                bool pr=i<n && p[i]>=0x20 && p[i]<0x7f;
                if(pr) { run++; continue; }
                if(run>=6 && used+run+2<sizeof(cd_text)) {
                    if(lg) cd_text[used++]='%';
                    memcpy(cd_text+used,p+i-run,run); used+=run; cd_text[used++]='|';
                }
                run=0;
            }
            if(cd && n>=8 && p[n-3]=='e' && p[n-2]=='n' && p[n-1]=='d') ended=true;
        }
    }
    printf("X9 BT ASSERT frames=%u coredump=%u fwlog=%u other=%u parser_err=%u bytes=%llu ms=%llu end=%d text=%lu\n",
        frames,dump,logs,other,perr,(unsigned long long)bytes,(unsigned long long)(now_ms(NULL)-start),ended,(unsigned long)used);
    for(size_t o=0;o<used;o+=150) {
        size_t k=used-o<150?used-o:150; char line[160];
        memcpy(line,cd_text+o,k); line[k]=0;
        printf("X9 BT CD %s\n",line);
    }
}
static void stall_collect(const char *what,const uint8_t *tx,size_t len,uint32_t ms)
{
    size_t sent=0; int error=raw_write(NULL,tx,len,&sent);
    if(!error) error=drain(NULL,100);
    hci_trace(what,tx,(uint32_t)len);
    unsigned got=0; uint64_t end=now_ms(NULL)+ms;
    while(now_ms(NULL)<end) {
        int e=next_frame(&incoming);
        if(!e) {
            got++; hci_trace("RX*",incoming.data,incoming.length);
            if(pending_count<8) pending[pending_count++]=incoming;
        } else if(e!=EAGAIN) { printf("X9 BT STALL %s parser errno=%d\n",what,e); break; }
        else sleep_ms(NULL,1);
    }
    boolean_t old=rx_lock(); uint64_t rcv=rx.received,irq=rx.interrupts,err=rx.errors; rx_unlock(old);
    printf("X9 BT STALL %s tx errno=%d frames=%u rx bytes=%llu irqs=%llu errs=%llu\n",what,error,got,
        (unsigned long long)rcv,(unsigned long long)irq,(unsigned long long)err);
}
static void stall_probe(void)
{
    stall_probes++;
    {
        boolean_t o2=rx_lock(); uint64_t rcv=rx.received,irq=rx.interrupts,err=rx.errors; rx_unlock(o2);
        printf("X9 BT STALL counters rx bytes=%llu irqs=%llu errs=%llu\n",(unsigned long long)rcv,
            (unsigned long long)irq,(unsigned long long)err);
    }
    boolean_t old=rx_lock(); uint32_t rl=rx.len; int re=rx.error; rx_unlock(old);
    printf("X9 BT STALL #%u: cmd %02x%02x len=%u %llums without RX; uart lsr=%x ier=%x rxring=%u rxerr=%d pending=%u asm=%u\n",
        stall_probes,stall_cmd_len>2?stall_cmd[2]:0,stall_cmd_len>1?stall_cmd[1]:0,stall_cmd_len,
        (unsigned long long)(now_ms(NULL)-stall_cmd_ms),rd(uart,0x14),rd(uart,0x04),rl,re,pending_count,assembling.length);
    x9_bt_hardware_snapshot("before");
    bool awake=false; int error=x9_bt_hardware_host_awake_set(&awake);
    printf("X9 BT STALL host_awake errno=%d bt_awake=%d\n",error,awake);
    if(!awake) {
        const uint8_t ff=0xff; size_t sent=0;
        error=raw_write(NULL,&ff,1,&sent);
        if(!error) error=drain(NULL,100);
        for(unsigned i=0;i<50 && x9_bt_hardware_bt_awake()!=1;i++) sleep_ms(NULL,1);
        printf("X9 BT STALL sent 0xFF errno=%d bt_awake=%d\n",error,x9_bt_hardware_bt_awake());
    }
    static const uint8_t own_clear[]={1,0x6f,0xfc,6,1,3,2,0,3,1};
    uint8_t reply[64]; size_t n=sizeof(reply); unsigned before=pending_count;
    error=exchange(NULL,own_clear,sizeof(own_clear),reply,&n,500);
    char hex[16*3+1]; hex[0]=0;
    for(size_t i=0;!error && i<n && i<16;i++) snprintf(hex+3*i,4,"%02x ",reply[i]);
    printf("X9 BT STALL own-clear errno=%d reply=%s queued=%u\n",error,hex,pending_count-before);
    for(unsigned i=before;i<pending_count;i++) hci_trace("RX*",pending[i].data,pending[i].length);
    x9_bt_hardware_snapshot("after");
    /* btmtk_query_fw_schedule_info: FW (not hub) answers 04 ff .. 3a fc a0. */
    static const uint8_t sched[]={1,0x3a,0xfc,2,0xa0,0};
    stall_collect("FC3A",sched,sizeof(sched),300);
    static const uint8_t version[]={1,0x01,0x10,0};
    stall_collect("1001",version,sizeof(version),300);
    bool answered=false;
    for(unsigned i=0;i<pending_count;i++)
        if(pending[i].length>=6 && pending[i].data[0]==4 && pending[i].data[1]==0x0e &&
           stall_cmd_len>=3 && pending[i].data[4]==stall_cmd[1] && pending[i].data[5]==stall_cmd[2]) answered=true;
    if(!error && !answered && stall_cmd_len) {
        size_t sent=0; int e=raw_write(NULL,stall_cmd,stall_cmd_len,&sent);
        if(!e) e=drain(NULL,1000);
        hci_trace("RETX",stall_cmd,stall_cmd_len);
        printf("X9 BT STALL retransmit errno=%d\n",e);
        stall_cmd_ms=now_ms(NULL);
    }
}
/* LE host prelude (log bt6 vs Android btsnoop CsLog_2026_1125_155703):
 * the controller's HCI task wedges when 0x203b is the first LE query after
 * HCI Reset. Android always precedes it with Set Event Mask, Write LE Host
 * Support and LE Set Event Mask (exact parameters below). bluetoothd's own
 * later 0c01/0c6d/2001 overwrite these values. */
static bool le_host_ready;
static void le_host_prelude(void)
{
    static const uint8_t cmds[3][12]={
        {1,0x01,0x0c,8,0xff,0xff,0xff,0xff,0xff,0xff,0xbf,0x3d},
        {1,0x6d,0x0c,2,0x01,0x00},
        {1,0x01,0x20,8,0x7f,0xfe,0x02,0x7f,0x07,0xf8,0x07,0x00},
    };
    for(unsigned i=0;i<3;i++) {
        size_t len=(size_t)cmds[i][3]+4;
        uint8_t reply[64]; size_t n=sizeof(reply);
        hci_trace("TX+",cmds[i],(uint32_t)len);
        int error=hci_exchange(NULL,cmds[i],len,reply,&n,1000);
        printf("X9 BT: LE host prelude %02x%02x errno=%d status=%d\n",cmds[i][2],cmds[i][1],
            error,(!error && n>=7)?reply[6]:-1);
        if(error) break;
    }
    le_host_ready=true;
}
static void worker(void *unused,wait_result_t result)
{
    (void)unused; (void)result;
    struct x9_bt_session_board board={0};
    int error=calibration(&board);
    ops.io=(struct x9_bt_download_ops){.exchange=exchange,.raw_write=raw_write,
        .drain=drain,.sleep_ms=sleep_ms,.now_ms=now_ms,.connfem=connfem};
    ops.host_uart=host_uart;
    ops.hci_exchange=hci_exchange;
    x9_bt_hardware_ops(&ops);
    state(X9_BT_INITIALIZING,0);
    if(!error) error=x9_bt_session_run(&ops,&board,&session);
    session_stage=(int)session.stage;
    lck_mtx_lock(&bt_lock); bs.chip_id=session.chip_id; bs.baud=session.baud; lck_mtx_unlock(&bt_lock);
    if(!error && (!session.driver_owned || !session.function_powered ||
                  session.stage!=X9_BT_SESSION_DONE)) error=ENXIO;
    if(error) {
        irq_stop(); int stop_error=x9_bt_hardware_stop();
        state(X9_BT_FAILED,error); x9_bt_serial_result(error,session.baud);
        printf("X9 BT: initializer failed stage=%d errno=%d cleanup=%d; native remains unavailable\n",
            session_stage,error,stop_error);
        lck_mtx_lock(&bt_lock); power.error=error; lck_mtx_unlock(&bt_lock); return;
    }
    /* Every preceding gate must pass on THIS boot, including current SSPM
     * readbacks and all real vendor completions. Compilation is not proof. */
    x9_bt_serial_result(0,session.baud);
    /* Publish READY last. The launch/BlueTool gate also reads serial.boot;
     * exposing READY before clearing its initial ENXIO would reject a real
     * completed session during this otherwise harmless publication window. */
    lck_mtx_lock(&bt_lock); bs.firmware_ready=1; bs.state=X9_BT_READY;
    power.live=true; power.state=0; power.error=0; lck_mtx_unlock(&bt_lock);
    printf("X9 BT: complete firmware session chip=%04x baud=%u driver-own verified\n",session.chip_id,session.baud);
    bool have_rx=false;
    for(;;) {
        lck_mtx_lock(&bt_lock);
        bool requested=power.pending; int target=power.target;
        if(requested) power.pending=false;
        lck_mtx_unlock(&bt_lock);
        if(requested) {
            bool fatal=false;
            error=power_execute(target,have_rx,&fatal);
            lck_mtx_lock(&bt_lock);
            power.error=error; power.done=true;
            if(fatal) { power.live=false; power.state=2; }
            else if(!error) power.state=target;
            /* On refusal restore current awake/asleep before waking caller,
             * not a transient READY publication paired with state2. */
            else power.state=target==1?0:1;
            wakeup(&power.done); lck_mtx_unlock(&bt_lock);
            if(fatal) break;
        }
        lck_mtx_lock(&bt_lock);
        while(power.state==1 && !power.pending)
            (void)msleep(&power.pending,&bt_lock,PZERO,"x9btsleep",NULL);
        bool asleep=power.state==1 || (power.busy && power.target==0);
        lck_mtx_unlock(&bt_lock);
        if(asleep) continue; /* Absolutely no UART MMIO/parser while gated. */
        /* A full serial FIFO must not hide an IRQ overflow/error forever. */
        boolean_t old=rx_lock(); error=rx.error; rx_unlock(old);
        if(error) break;
        uint32_t n=0,generation;
        if(!have_rx) {
            if(pending_count) {
                incoming=pending[0]; pending_count--;
                memmove(pending,pending+1,pending_count*sizeof(pending[0]));
                have_rx=true;
            } else {
                error=next_frame(&incoming);
                if(error && error!=EAGAIN) break;
                have_rx=error==0;
                if(have_rx) { hci_trace("RX",incoming.data,incoming.length); stall_outstanding=false; }
            }
        }
        /* connac3 FE01: Android drops this command complete in btmtk_rx_work
         * and reloads the ROM-patch section named by data[6] over WMT before
         * the stack sees it. This worker is the only RX owner, so the
         * download's own ACKs are consumed here; Android needs a workqueue
         * only because its rx_work thread cannot wait for them. */
        if(have_rx && incoming.length==7 && incoming.data[0]==4 &&
           incoming.data[1]==0x0e && incoming.data[2]==4 && incoming.data[3]==1 &&
           incoming.data[4]==1 && incoming.data[5]==0xfe) {
            unsigned bin=incoming.data[6];
            static unsigned dynamic_logs;
            bool log=dynamic_logs<8;
            if(log) { dynamic_logs++; printf("X9 BT: dynamic FW section %u requested\n",bin); }
            struct x9_bt_download_result dr;
            int derr=x9_bt_download_dynamic(&ops.io,bin,&dr);
            if(log)
                printf("X9 BT: dynamic FW section %u %s errno=%d bytes=%llu ready=%u\n",
                    bin,derr?"failed":"loaded",derr,(unsigned long long)dr.bytes,
                    dr.already_ready);
            stall_outstanding=false; stall_probed=false;
            have_rx=false;
        }
        /* MTK vendor events (HCI event 0xE0..0xff, observed 0xE3 and 0xE4)
         * belong to the vendor transport and are never delivered to the host
         * stack on Android. Apple's event lookup 0x10014a3bc rejects every
         * code above 0x58, and hcievents.c fatals with reason 607. Consume
         * the complete H4 frame here. */
        if(have_rx && incoming.length>=3 && incoming.data[0]==4 &&
           incoming.data[1]>=0xe0 && incoming.length==(uint32_t)incoming.data[2]+3) {
            static unsigned wmt_logs;
            if(wmt_logs<8) {
                wmt_logs++;
                printf("X9 BT: WMT event %02x len=%u consumed\n",
                       incoming.data[2]?incoming.data[3]:0,incoming.length);
            }
            have_rx=false;
        }
        /* Retain the WHOLE frame under temporary daemon backpressure.
         * Closed-daemon packets are discarded by serial_rx, as before.
         * The bounded IRQ ring still makes a genuinely stalled consumer
         * fail explicitly, never truncate its H4 stream. */
        if(have_rx && x9_bt_serial_rx(incoming.data,incoming.length)==incoming.length)
            have_rx=false;
        error=x9_bt_serial_tx_claim(native_tx,sizeof(native_tx),&n,&generation);
        if(error) break;
        if(n && native_tx[0]==1 && n>=3) {
            uint16_t op=native_tx[1]|((uint16_t)native_tx[2]<<8);
            if(op==0x0c03) {
                le_host_ready=false;
                static bool rhw_baseline;
                if(!rhw_baseline && !have_rx && !pending_count) {
                    /* Healthy-state reference before the daemon's first Reset. */
                    rhw_baseline=true; rhw_fail=0;
                    rhw_section("BJ",rhw_sel_j,sizeof(rhw_sel_j)/4);
                    rhw_section("BA",rhw_sel_a,sizeof(rhw_sel_a)/4);
                    rhw_section("BJ1",rhw_sel_j,sizeof(rhw_sel_j)/4);
                }
            }
            else if(op==0x0c6d) le_host_ready=true;
            else if(op==0x203b && !le_host_ready) {
                /* hci_exchange reuses incoming: keep an undelivered frame. */
                if(have_rx && pending_count<8) {
                    memmove(pending+1,pending,pending_count*sizeof(pending[0]));
                    pending[0]=incoming; pending_count++; have_rx=false;
                }
                if(!have_rx) le_host_prelude();
            }
        }
        if(n) {
            hci_trace("TX",native_tx,n);
            uint32_t wire_length=0;
            size_t sent=0; error=native_to_wire(n,&wire_length);
            if(!error) error=raw_write(NULL,native_tx,wire_length,&sent);
            if(!error && sent!=wire_length) error=EIO;
            if(!error) error=drain(NULL,1000);
            /* A translated SCO wire frame is one byte longer than its
             * native claim. Never retire the claim by wire byte count,
             * or acknowledge a partial write / incomplete UART drain. */
            x9_bt_serial_tx_finish(error?0:n,generation,error);
            if(error) break;
            if(native_tx[0]==1 && n<=sizeof(stall_cmd)) {
                memcpy(stall_cmd,native_tx,n); stall_cmd_len=n;
                stall_cmd_ms=now_ms(NULL); stall_outstanding=true; stall_probed=false;
            }
            lck_mtx_lock(&bt_lock); bs.tx_frames++; lck_mtx_unlock(&bt_lock);
        }
        if(stall_outstanding && !stall_probed && !have_rx && !pending_count &&
           now_ms(NULL)-stall_cmd_ms>300) { stall_probed=true; stall_probe(); }
        sleep_ms(NULL,1);
    }
    printf("X9 BT: native worker loop exit errno=%d\n",error);
    lck_mtx_lock(&bt_lock); power.live=false; power.state=2; power.error=error;
    if(power.busy && !power.done) { power.done=true; wakeup(&power.done); }
    lck_mtx_unlock(&bt_lock);
    x9_bt_serial_result(error,session.baud); irq_stop();
    int stop_error=x9_bt_hardware_stop(); state(X9_BT_FAILED,error);
    printf("X9 BT: worker failed errno=%d cleanup=%d; no clean hardware baseline claimed\n",
        error,stop_error);
}
void x9_bluetooth_start(void)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_bt",0)!=1 || started) return;
    started=1;
    int error=x9_bt_serial_init();
    if(error) { state(X9_BT_FAILED,error); return; }
    lck_mtx_lock(&bt_lock); bs.state=X9_BT_PASSIVE; bs.firmware_bytes=X9_BT_FW_BYTES;
    bs.firmware_sections=X9_BT_FW_SECTIONS; lck_mtx_unlock(&bt_lock);
    printf("X9 BT: complete-session backend; old x9_bt_probe/raw paths removed, firmware-ready=0\n");
    /* X9 defaults enable initialization; x9_bt_init=0 keeps the radio passive.
     * Runtime ACK/profile gates remain mandatory. */
    if(sweet_x9_arg("x9_bt_init",0)!=1) return;
    thread_t thread;
    if(kernel_thread_start(worker,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
    else { state(X9_BT_FAILED,ENOMEM); x9_bt_serial_result(ENOMEM,0); }
}
#endif
