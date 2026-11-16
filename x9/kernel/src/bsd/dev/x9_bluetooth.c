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
static uint8_t native_tx[BT_WIRE_FRAME_MAX];
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
    uint32_t flow;
    /* uart_launcher -f and HCIUARTSETBAUD supply the production selection;
     * its CLI default is not proof of this device's effective config. This
     * value is the Android UART_FC enum, not the WMT flow-control byte.
     * Never silently choose NONE, or start power/download for an AP flow
     * mode whose IRQ/escape implementation is not available yet. */
    if(!PE_parse_boot_argn("x9_bt_fc",&flow,sizeof(flow))) {
        printf("X9 BT: effective Android UART flow-control selection required\n");
        return ENOENT;
    }
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
            }
        }
        /* Retain the WHOLE frame under temporary daemon backpressure.
         * Closed-daemon packets are discarded by serial_rx, as before.
         * The bounded IRQ ring still makes a genuinely stalled consumer
         * fail explicitly, never truncate its H4 stream. */
        if(have_rx && x9_bt_serial_rx(incoming.data,incoming.length)==incoming.length)
            have_rx=false;
        error=x9_bt_serial_tx_claim(native_tx,sizeof(native_tx),&n,&generation);
        if(error) break;
        if(n) {
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
            lck_mtx_lock(&bt_lock); bs.tx_frames++; lck_mtx_unlock(&bt_lock);
        }
        sleep_ms(NULL,1);
    }
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
    /* Stock boot-device stays passive. Only an explicit complete candidate
     * enables initialization; runtime ACK/profile gates remain mandatory. */
    if(sweet_x9_arg("x9_bt_init",0)!=1) return;
    thread_t thread;
    if(kernel_thread_start(worker,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
    else { state(X9_BT_FAILED,ENOMEM); x9_bt_serial_result(ENOMEM,0); }
}
#endif
