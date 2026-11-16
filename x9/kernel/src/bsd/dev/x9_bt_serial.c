/* Native serial frontend; reference contract: UniXNU QcaBluetooth qbt_tty.c
 * (986d6319), not its Qualcomm executor or macOS daemon ABI. All ready state
 * is supplied by the MT6653 board worker, never inferred from service names.
 * This byte BT_SERIAL_FIFO permits partial H4 writes. It is deliberately not the raw
 * bootstrap IOUserClient frame queue, and never carries firmware downloads. */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include "x9_bt_serial.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <miscfs/devfs/devfs.h>

extern kern_return_t thread_terminate(thread_t);
#define BT_SERIAL_FIFO 16384u
/* 64-bit iOS16 ttycom/filio/ioss wire layout, independent of kernel ABI. */
struct bt_termios64 {
    uint64_t iflag,oflag,cflag,lflag;
    uint8_t cc[20]; uint32_t pad;
    uint64_t ispeed,ospeed;
};
_Static_assert(sizeof(struct bt_termios64)==72,"termios64 ioctl ABI");
static LCK_GRP_DECLARE(serial_group,"x9.bt.serial");
static LCK_MTX_DECLARE(serial_lock,&serial_group);
static LCK_MTX_DECLARE(serial_write_lock,&serial_group);
static struct {
    int major,boot;
    void *node;
    bool opened,node_worker,tx_claimed,transition,read_stopping;
    uint32_t baud,generation,rhead,rlen,thead,tlen;
    uint32_t claim_generation,claim_length;
    struct selinfo readers,writers;
    struct bt_termios64 line;
    uint8_t rx[BT_SERIAL_FIFO],tx[BT_SERIAL_FIFO];
} serial={.major=-1,.boot=ENXIO};
static int io_status(void)
{ return !serial.opened?ENXIO:serial.boot; }
static int read_status(void)
{ int error=io_status(); return error?error:serial.read_stopping?ECANCELED:0; }
static void wake_io(void)
{
    wakeup(&serial.rlen); wakeup(&serial.tlen);
    selwakeup(&serial.readers); selwakeup(&serial.writers);
}
static void put(uint8_t *f,uint32_t h,uint32_t *len,const uint8_t *p,uint32_t n)
{
    uint32_t at=(h+*len)%BT_SERIAL_FIFO,first=MIN(n,BT_SERIAL_FIFO-at);
    memcpy(f+at,p,first); memcpy(f,p+first,n-first); *len+=n;
}
static void peek(uint8_t *p,const uint8_t *f,uint32_t h,uint32_t n)
{ uint32_t first=MIN(n,BT_SERIAL_FIFO-h); memcpy(p,f+h,first); memcpy(p+first,f,n-first); }
bool x9_bt_serial_active(void)
{
    lck_mtx_lock(&serial_lock); bool active=serial.opened && !serial.boot;
    lck_mtx_unlock(&serial_lock); return active;
}
int x9_bt_serial_power_begin(bool sleep)
{
    lck_mtx_lock(&serial_lock);
    int error=0;
    if(serial.transition || serial.opened || serial.tx_claimed || serial.tlen || serial.rlen)
        error=EBUSY;
    else if(serial.boot!=(sleep?0:EHOSTDOWN)) error=serial.boot?serial.boot:EINVAL;
    if(!error) {
        serial.transition=true; serial.boot=EBUSY; serial.generation++;
        wake_io();
    }
    lck_mtx_unlock(&serial_lock); return error;
}
uint32_t x9_bt_serial_rx(const uint8_t *p,uint32_t n)
{
    if(!p || !n) return 0;
    lck_mtx_lock(&serial_lock);
    /* No daemon: board may discard stale HCI, never queue pre-open events. */
    if(!serial.opened || serial.boot) { lck_mtx_unlock(&serial_lock); return n; }
    /* Atomic frame admission: truncating H4 to the available FIFO room
     * loses the tail and desynchronizes the daemon's native RxLoop. */
    if(n>BT_SERIAL_FIFO-serial.rlen) n=0;
    if(n) { put(serial.rx,serial.rhead,&serial.rlen,p,n); wake_io(); }
    lck_mtx_unlock(&serial_lock); return n;
}
int x9_bt_serial_tx_claim(uint8_t *p,uint32_t capacity,uint32_t *length,uint32_t *generation)
{
    if(!p || capacity<5 || !length || !generation) return EINVAL;
    *length=0;
    lck_mtx_lock(&serial_lock);
    *generation=serial.generation;
    int error=serial.boot;
    if(!error && serial.tx_claimed) error=EBUSY;
    if(!error && serial.opened && serial.tlen) {
        uint8_t header[5]; uint32_t n=MIN(serial.tlen,sizeof(header)),size=0;
        peek(header,serial.tx,serial.thead,n);
        switch(header[0]) {
        case 1: if(n>=4) size=4u+header[3]; break;
        case 2: if(n>=5) size=5u+header[3]+((uint32_t)header[4]<<8); break;
        case 3:
            if(n>=4) {
                if(header[3]>X9_BT_SCO_PAYLOAD_MAX) error=EMSGSIZE;
                else size=4u+header[3];
            }
            break;
        case 5: if(n>=5) size=5u+header[3]+((uint32_t)(header[4]&0x3f)<<8); break;
        default: error=EPROTO; break; /* Events/raw wake bytes are board-owned. */
        }
        if(!error && size>capacity) error=EMSGSIZE;
        if(!error && size && size<=serial.tlen) {
            peek(p,serial.tx,serial.thead,size); *length=size;
            serial.tx_claimed=true; serial.claim_length=size;
            serial.claim_generation=serial.generation;
        }
    }
    lck_mtx_unlock(&serial_lock); return error;
}
void x9_bt_serial_tx_finish(uint32_t sent,uint32_t generation,int error)
{
    lck_mtx_lock(&serial_lock);
    if(serial.tx_claimed && generation==serial.claim_generation) {
        if(!error && sent!=serial.claim_length) error=EIO;
        serial.tx_claimed=false;
        if(error) {
            serial.boot=error; serial.generation++;
            serial.rhead=serial.rlen=serial.thead=serial.tlen=0;
        } else if(generation==serial.generation && sent<=serial.tlen) {
            serial.thead=(serial.thead+sent)%BT_SERIAL_FIFO; serial.tlen-=sent;
        }
        wake_io();
    }
    lck_mtx_unlock(&serial_lock);
}
void x9_bt_serial_result(int error,uint32_t baud)
{
    lck_mtx_lock(&serial_lock);
    serial.transition=false;
    serial.boot=error?error:baud?0:EINVAL;
    if(!serial.boot) serial.baud=baud;
    else { serial.generation++; serial.rhead=serial.rlen=serial.thead=serial.tlen=0; }
    wake_io(); lck_mtx_unlock(&serial_lock);
}
static int serial_open(dev_t d __unused,int flags __unused,int type __unused,struct proc *p)
{
    lck_mtx_lock(&serial_lock);
    int error=(serial.opened || serial.tx_claimed || serial.transition)?EBUSY:serial.boot;
    if(!error) {
        serial.opened=true; serial.generation++;
        serial.read_stopping=false;
        serial.rhead=serial.rlen=serial.thead=serial.tlen=0;
        memset(&serial.line,0,sizeof(serial.line));
        serial.line.cflag=0x300|0x800|0x8000|0x10000|0x20000;
        serial.line.ispeed=serial.line.ospeed=serial.baud;
        serial.line.cc[16]=1; /* VMIN */
    }
    lck_mtx_unlock(&serial_lock);
    printf("X9 BT: native tty open pid=%d errno=%d\n",proc_pid(p),error);
    return error;
}
static int serial_close(dev_t d __unused,int flags __unused,int type __unused,struct proc *p __unused)
{
    lck_mtx_lock(&serial_lock);
    serial.opened=false; serial.generation++;
    serial.rhead=serial.rlen=serial.thead=serial.tlen=0; wake_io();
    lck_mtx_unlock(&serial_lock); return 0;
}
static int serial_read(dev_t d __unused,struct uio *uio,int flags)
{
    if(!uio_resid(uio)) return 0;
    uint8_t data[512]; int error=0;
    lck_mtx_lock(&serial_lock);
    uint32_t generation=serial.generation;
    while(!serial.rlen && !(error=read_status())) {
        if(flags&IO_NDELAY) { error=EAGAIN; break; }
        error=msleep(&serial.rlen,&serial_lock,PZERO|PCATCH,"x9btread",NULL);
        if(error) break;
        if(generation!=serial.generation) { error=ENXIO; break; }
    }
    if(!error) error=read_status();
    while(!error && serial.rlen && uio_resid(uio)>0) {
        uint32_t n=(uint32_t)MIN(uio_resid(uio),(user_ssize_t)MIN(serial.rlen,sizeof(data)));
        peek(data,serial.rx,serial.rhead,n);
        serial.rhead=(serial.rhead+n)%BT_SERIAL_FIFO; serial.rlen-=n;
        lck_mtx_unlock(&serial_lock); error=uiomove((char *)data,(int)n,uio); lck_mtx_lock(&serial_lock);
        if(generation!=serial.generation) break; /* Never consume a reopened session. */
        if(!error) error=read_status();
    }
    lck_mtx_unlock(&serial_lock); return error;
}
static int serial_write(dev_t d __unused,struct uio *uio,int flags)
{
    uint8_t data[512]; int error=0;
    /* uiomove drops serial_lock. Serialize whole write calls so two native
     * writers cannot interleave their H4 headers/payload512-byte chunks. */
    lck_mtx_lock(&serial_write_lock);
    lck_mtx_lock(&serial_lock); uint32_t generation=serial.generation;
    while(uio_resid(uio)>0 && !error) {
        uint32_t n=(uint32_t)MIN(uio_resid(uio),(user_ssize_t)sizeof(data));
        while(!(error=io_status()) && n>BT_SERIAL_FIFO-serial.tlen) {
            if(flags&IO_NDELAY) { error=EAGAIN; break; }
            error=msleep(&serial.tlen,&serial_lock,PZERO|PCATCH,"x9btwrite",NULL);
            if(error) break;
            if(generation!=serial.generation) { error=ENXIO; break; }
        }
        if(error) break;
        lck_mtx_unlock(&serial_lock); error=uiomove((char *)data,(int)n,uio); lck_mtx_lock(&serial_lock);
        if(!error && generation!=serial.generation) error=ENXIO;
        if(!error) error=io_status();
        if(!error && n>BT_SERIAL_FIFO-serial.tlen) error=EBUSY;
        if(!error) put(serial.tx,serial.thead,&serial.tlen,data,n);
    }
    lck_mtx_unlock(&serial_lock); lck_mtx_unlock(&serial_write_lock); return error;
}
static int serial_select(dev_t d __unused,int which,void *wql,struct proc *p)
{
    if(which!=FREAD && which!=FWRITE) return 0;
    lck_mtx_lock(&serial_lock);
    int ready=(which==FREAD?read_status():io_status())!=0 ||
        (which==FREAD?serial.rlen!=0:serial.tlen<BT_SERIAL_FIFO);
    if(!ready) selrecord(p,which==FREAD?&serial.readers:&serial.writers,wql);
    lck_mtx_unlock(&serial_lock); return ready;
}
static int serial_ioctl(dev_t d __unused,u_long cmd,caddr_t data,int flags __unused,struct proc *p __unused)
{
    int error=0;
    lck_mtx_lock(&serial_lock);
    switch((uint32_t)cmd) {
    case 0x20005801: /* _IO('X',1): bridge cancels read BEFORE BSD close drain. */
        /* close waits on fp_iocount before d_close; a blocked read must be
         * released first. This does not clear TX, close hardware or sleep.
         * The version-locked native close bridge sets its RX stop flag first. */
        serial.read_stopping=true; wake_io(); break;
    case 0x40487413: memcpy(data,&serial.line,sizeof(serial.line)); break; /* GETA */
    case 0x80487414: case 0x80487415: case 0x80487416: /* SETA/W/F */
        memcpy(&serial.line,data,sizeof(serial.line)); break;
    case 0x80085402: { /* IOSSIOSPEED records host request; board owns 24M. */
        uint64_t speed; memcpy(&speed,data,sizeof(speed));
        if(!speed) error=EINVAL;
        else serial.line.ispeed=serial.line.ospeed=speed;
        break;
    }
    case 0x4004667f: { int n=(int)serial.rlen; memcpy(data,&n,sizeof(n)); break; }
    case 0x40047473: { int n=(int)serial.tlen; memcpy(data,&n,sizeof(n)); break; }
    case 0x80047410: { /* Flush is serialized with queued userspace bytes. */
        int what; memcpy(&what,data,sizeof(what));
        if(!what || (what&FREAD)) serial.rhead=serial.rlen=0;
        /* TX already peeked by the board cannot safely be withdrawn. */
        if(!what || (what&FWRITE)) { if(serial.tlen) error=EBUSY; }
        wake_io(); break;
    }
    case 0x2000745e: { /* Never report drain success after a timeout. */
        struct timespec timeout={2,0};
        error=io_status();
        while(serial.tlen && !(error=io_status())) {
            error=msleep(&serial.tlen,&serial_lock,PZERO|PCATCH,"x9btdrain",&timeout);
            if(error) break;
        }
        /* A worker fault clears tlen while waking this waiter; an empty
         * queue by itself is NOT successful physical drain. */
        if(!error) error=io_status();
        break;
    }
    case 0x2000740d: case 0x2000740e: /* Single-open ownership enforced above. */
    case 0x8004667e: break; /* FIONBIO: VFS records nonblocking state. */
    default: error=ENOTTY; break; /* Do not invent modem-line or async status. */
    }
    lck_mtx_unlock(&serial_lock); return error;
}
static const struct cdevsw serial_cdev={
    .d_open=serial_open,.d_close=serial_close,.d_read=serial_read,.d_write=serial_write,.d_ioctl=serial_ioctl,
    .d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=serial_select,.d_mmap=eno_mmap,
    .d_strategy=eno_strat,.d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=0,
};
static bool make_node(void)
{
    lck_mtx_lock(&serial_lock);
    if(!serial.node && serial.major>=0)
        /* bluetoothd runs as mobile in the actual 20H392 launch plist. */
        serial.node=devfs_make_node(makedev(serial.major,0),DEVFS_CHAR,501,501,0600,"cu.bluetooth");
    bool made=serial.node!=NULL; lck_mtx_unlock(&serial_lock); return made;
}
static void node_worker(void *ctx __unused,wait_result_t result __unused)
{
    for(unsigned n=0;n<600 && !make_node();n++) {
        struct timespec timeout={0,100000000};
        (void)msleep(&serial.node_worker,NULL,PZERO,"x9btnode",&timeout);
    }
    printf("X9 BT: native tty node %s; ready follows full board initialization only\n",serial.node?"created":"unavailable");
    thread_terminate(current_thread());
}
int x9_bt_serial_init(void)
{
    lck_mtx_lock(&serial_lock);
    if(serial.major>=0) { lck_mtx_unlock(&serial_lock); return EALREADY; }
    serial.major=cdevsw_add(-1,&serial_cdev);
    int error=serial.major<0?ENXIO:0;
    lck_mtx_unlock(&serial_lock);
    if(!error && !make_node()) {
        thread_t thread;
        if(kernel_thread_start(node_worker,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
        else error=ENOMEM;
    }
    return error;
}
static int boot_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    lck_mtx_lock(&serial_lock); int result=serial.boot; lck_mtx_unlock(&serial_lock);
    return SYSCTL_OUT(req,&result,sizeof(result));
}
SYSCTL_PROC(_kern,OID_AUTO,x9_bt_boot,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED,NULL,0,boot_sysctl,"I","MT6653 complete initialization errno, not ROM status");
#endif
