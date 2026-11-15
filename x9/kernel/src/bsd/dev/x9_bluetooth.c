/* MT6993/MT6653 board worker. Complete session only; no ROM probe, no raw
 * user-client injection competing with native H4. SSPM post-download handoff
 * is still a hard gate in x9_bt_hardware.c, never a fabricated ready result. */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
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
static uint8_t priority_saved;
static bool irq_captured;
/* production DT UART3 SPI0x148, level-high. Wake SPI0x246 is NOT enabled:
 * no actual resume/wakeup callback, so session never enables FW wake-enhance. */
enum { UART_INTID=0x148+32, IRQ_BIT=1u<<(UART_INTID%32), BANK=UART_INTID/32,
    G_GROUP=0x80+4*BANK, G_ENABLE=0x100+4*BANK, G_DISABLE=0x180+4*BANK,
    G_PENDING=0x200+4*BANK, G_ACTIVE=0x300+4*BANK,
    G_CONFIG=0xc00+4*(UART_INTID/16), G_PRIORITY=0x400+UART_INTID,
    G_ROUTE=0x6000+8*UART_INTID };
struct bt_frame { uint32_t length; uint8_t data[X9_BT_FRAME_MAX]; };
static struct bt_frame assembling, pending[8];
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
    if(!rx.owned) { rx_unlock(old); return false; }
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
    if((rd(gic,0)&0x11)!=0x11 || (rd(gic,0)&(1u<<31)) ||
       (((rd(gic,4)&31)+1)*32)<=UART_INTID || !(rd(gic,G_GROUP)&IRQ_BIT) ||
       (rd(gic,G_CONFIG)&(3u<<((UART_INTID%16)*2))) ||
       ((rd(gic,G_ENABLE)|rd(gic,G_ACTIVE)|rd(gic,G_PENDING))&IRQ_BIT)) return EBUSY;
    if(rd(uart,4) || rd(uart,0x4c) || !(rd(uart,0x14)&0x40) || (rd(uart,0x14)&1)) return EBUSY;
    route_saved=*(volatile uint64_t *)((uintptr_t)gic+G_ROUTE);
    priority_saved=*(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY); irq_captured=true;
    uint64_t mpidr=__builtin_arm_rsr64("MPIDR_EL1")&0xff00ffffffULL;
    *(volatile uint64_t *)((uintptr_t)gic+G_ROUTE)=mpidr;
    *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY)=0xa0;
    __asm__ volatile("dsb sy" ::: "memory");
    if(*(volatile uint64_t *)((uintptr_t)gic+G_ROUTE)!=mpidr ||
       *(volatile uint8_t *)((uintptr_t)gic+G_PRIORITY)!=0xa0) return EIO;
    /* 8250 UART64 RX trigger16, RDI + line-status. Not Linux APDMA. The
     * hardware FIFO is empty, proved above; no flush after a command. */
    wr(uart,8,0x41);
    boolean_t old=rx_lock(); rx.owned=true; wr(uart,4,5); wr(gic,G_ENABLE,IRQ_BIT);
    rx_unlock(old);
    for(unsigned i=0;i<500 && (rd(gic,0)&(1u<<31));i++) delay(2);
    if((rd(gic,0)&(1u<<31)) || !(rd(gic,G_ENABLE)&IRQ_BIT)) return EIO;
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
        __asm__ volatile("dsb sy" ::: "memory"); irq_captured=false;
    }
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
    if(!rx.owned) { int e=irq_bind(); if(e) return e; }
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
static int frame_length(const uint8_t *p,uint32_t n)
{
    if(!n) return 0;
    switch(p[0]) {
    case 1: return n<4?0:4+p[3];
    case 2: return n<5?0:5+p[3]+((unsigned)p[4]<<8);
    case 3: return n<4?0:4+p[3];
    case 4: return n<3?0:3+p[2];
    case 5: return n<5?0:5+p[3]+((unsigned)(p[4]&0x3f)<<8);
    default: return -1;
    }
}
static int next_frame(struct bt_frame *out)
{
    for(;;) {
        uint8_t b; uint32_t count; int error=rx_take(&b,1,&count); if(error) return error;
        if(!count) return EAGAIN;
        if(!assembling.length) frame_start=now_ms(NULL);
        if(assembling.length>=sizeof(assembling.data)) return EMSGSIZE;
        assembling.data[assembling.length++]=b;
        int frame_bytes=frame_length(assembling.data,assembling.length);
        if(frame_bytes<0) return EPROTO;
        if(frame_bytes>(int)sizeof(assembling.data)) return EMSGSIZE;
        if(frame_bytes && frame_bytes==(int)assembling.length) {
            *out=assembling; assembling.length=0;
            lck_mtx_lock(&bt_lock); bs.rx_frames++; lck_mtx_unlock(&bt_lock); return 0;
        }
        if(now_ms(NULL)-frame_start>1000) return ETIMEDOUT;
    }
}
static int exchange(void *context,const uint8_t *tx,size_t txlen,
                    uint8_t *reply,size_t *replylen,uint32_t timeout)
{
    (void)context;
    uint8_t opcode;
    if(!tx || !reply || !replylen) return EINVAL;
    if(txlen==1 && tx[0]==0xff) opcode=3;
    else if(txlen>=6 && tx[0]==1) opcode=tx[5];
    else if(txlen>=7 && tx[0]==2) opcode=tx[6];
    else return EINVAL;
    size_t sent; int error=raw_write(NULL,tx,txlen,&sent); if(error) return error;
    if(sent!=txlen) return EIO;
    uint64_t end=now_ms(NULL)+timeout;
    for(;;) {
        struct bt_frame f;
        error=next_frame(&f);
        if(error && error!=EAGAIN) return error;
        if(!error) {
            if(f.length>=5 && f.data[0]==4 && f.data[1]==0xe4 && f.data[4]==opcode) {
                if(*replylen<f.length) { *replylen=f.length; return EMSGSIZE; }
                memcpy(reply,f.data,f.length); *replylen=f.length; return 0;
            }
            /* Preserve unrelated complete packets; don't let a native/raw
             * client steal the decisive initialization ACK. */
            if(pending_count==8) return ENOBUFS;
            pending[pending_count++]=f;
        }
        if(now_ms(NULL)>=end) return ETIMEDOUT;
        if(error==EAGAIN) sleep_ms(NULL,1);
    }
}
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
static void worker(void *unused,wait_result_t result)
{
    (void)unused; (void)result;
    struct x9_bt_session_board board={0};
    int error=calibration(&board);
    ops.io=(struct x9_bt_download_ops){.exchange=exchange,.raw_write=raw_write,
        .drain=drain,.sleep_ms=sleep_ms,.now_ms=now_ms,.connfem=connfem};
    ops.host_uart=host_uart;
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
            session_stage,error,stop_error); return;
    }
    /* This branch is currently unreachable while the SSPM CMM gate returns
     * ENOTSUP. Compile/link success must not be reported as reaching it. */
    x9_bt_serial_result(0,session.baud);
    /* Publish READY last. The launch/BlueTool gate also reads serial.boot;
     * exposing READY before clearing its initial ENXIO would reject a real
     * completed session during this otherwise harmless publication window. */
    lck_mtx_lock(&bt_lock); bs.firmware_ready=1; bs.state=X9_BT_READY; lck_mtx_unlock(&bt_lock);
    printf("X9 BT: complete firmware session chip=%04x baud=%u driver-own verified\n",session.chip_id,session.baud);
    uint8_t bytes[1024];
    for(;;) {
        uint32_t n=0, generation;
        error=rx_take(bytes,sizeof(bytes),&n);
        if(!error && n && x9_bt_serial_rx(bytes,n)!=n) error=ENOBUFS;
        if(error) break;
        n=x9_bt_serial_tx_peek(bytes,sizeof(bytes),&generation);
        if(n) {
            size_t sent=0; error=raw_write(NULL,bytes,n,&sent);
            x9_bt_serial_tx_commit((uint32_t)sent,generation); if(error) break;
        }
        sleep_ms(NULL,1);
    }
    x9_bt_serial_result(error,session.baud); irq_stop();
    (void)x9_bt_hardware_stop(); state(X9_BT_FAILED,error);
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
    /* Keep current boot-device passive until the SSPM post-download handoff
     * and native model capability integration are both implemented. No new
     * candidate/probe image or launch staging is produced by this change. */
    if(sweet_x9_arg("x9_bt_init",0)!=1) return;
    thread_t thread;
    if(kernel_thread_start(worker,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
    else { state(X9_BT_FAILED,ENOMEM); x9_bt_serial_result(ENOMEM,0); }
}
#endif
