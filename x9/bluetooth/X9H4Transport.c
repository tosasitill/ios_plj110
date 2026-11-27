/* 20H392 arm64 native H4 -> X9 BSD serial adapter.
 * Keep Apple H4 parser, RX loop, stack locking, TX and close. Replace
 * POSIX.open (otherwise UART.SK/Skywalk) and prepend character-vnode read
 * cancellation to POSIX.close before calling the unchanged native close.
 * Optional X9Vendor16 routing is a separately gated, version-locked adapter.
 * No firmware, vendor capability constants, BlueTool success fabrication,
 * or device writes occur in the constructor. */
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <unistd.h>
#include "X9Bootstrap16.h"

#define BASE UINT64_C(0x100000000)
#define OPEN_SLOT UINT64_C(0x100844168)
#define CLOSE_SLOT UINT64_C(0x100844178)
#define NATIVE_OPEN UINT64_C(0x100066bdc)
#define NATIVE_CLOSE UINT64_C(0x1000674d0)
#define NATIVE_WRITE UINT64_C(0x100067610)
static intptr_t slide;
extern int x9_vendor16_install(intptr_t);
static uintptr_t address(uint64_t va) { return (uintptr_t)(va+slide); }
static uint32_t *u32(uint64_t va) { return (uint32_t *)address(va); }
static uintptr_t *uptr(uint64_t va) { return (uintptr_t *)address(va); }
static const uint8_t original_uuid[16]={
    0xaa,0x94,0x24,0xe8,0x06,0x48,0x34,0xe2,0xb3,0xcf,0xe6,0xc7,0x93,0xb9,0xd4,0x71
};
/* BSD vnode close drains in-flight read references BEFORE d_close. The
 * stock POSIX transport is a socket, whose drain wakes the RX thread;
 * cu.bluetooth is a character vnode with fo_no_drain. Cancel only its read
 * admission before entering native close; never discard queued/on-wire TX. */
#define X9_SERIAL_CANCEL_READ _IO('X', 1)
_Static_assert((uint32_t)X9_SERIAL_CANCEL_READ == 0x20005801u,
               "native cancel-read ioctl ABI");
static uint32_t serial_close(void)
{
    typedef uint32_t (*close_fn)(void);
    int fd=(int32_t)*u32(0x1008441c8);
    if(fd!=-1) {
        /* Exact native close at 1000674f8..500 sets this flag BEFORE
         * closing its socket. RX 100067af0..b18 then marks exited/signals
         * the native condition, rather than taking unexpected-exit logging.
         * Preserve that ordering when the cancel ioctl wakes a vnode read. */
        uint32_t stopped=*u32(0x1008494e8);
        *u32(0x1008494e8)=1;
        if(ioctl(fd,X9_SERIAL_CANCEL_READ)<0) {
            int error=errno;
            *u32(0x1008494e8)=stopped;
            x9_bt_note("X9 H4: native RX cancellation failed errno=%d; close not entered\n",error);
            return 0x4b5;
        }
    }
    return ((close_fn)address(NATIVE_CLOSE))();
}
static int serial_open(uint32_t mode,const char *configuration)
{
    (void)configuration;
    x9_bt_note("X9 H4: POSIX.open mode=%u\n",mode);
    /* The native H4 driver calls POSIX.open with mode0; do not silently
     * accept H4DS/H4BC callers with different flow-control semantics. */
    if(mode) return 0x65;
    if((int32_t)*u32(0x1008441c8)!=-1 || *uptr(0x1008494e0)) return 0x4b1;
    /* An explicit transport reopen resumes the worker before any UART byte
     * is accepted. It does not fabricate ready status or start a second
     * initializer, and ownership errors remain native-open failures. */
    int result=x9_bootstrap16_resume();
    if(result) {
        x9_bt_note("X9 H4: complete MT6653 initializer not ready (errno=%d)\n",result);
        return 0x4b1;
    }
    int fd=open("/dev/cu.bluetooth",O_RDWR|O_NOCTTY);
    if(fd<0) return 0x4b1;
    struct termios line;
    int error=0;
    if(ioctl(fd,TIOCEXCL)<0 || tcgetattr(fd,&line)<0) error=errno;
    if(!error) {
        cfmakeraw(&line);
        line.c_cflag |= CREAD|CLOCAL;
        if(tcsetattr(fd,TCSANOW,&line)<0) error=errno;
    }
    if(error) { close(fd); return 0x4b1; }
    *u32(0x1008441c8)=(uint32_t)fd;
    *u32(0x1008441cc)=*u32(0x1008441d0)=UINT32_MAX; /* No Skywalk kqueues. */
    *u32(0x1008494e8)=*u32(0x1008494ec)=0; /* RX stop/exited. */
    /* Exact tail contract recovered at 0x1000671cc..0x100067228.
     * Initialize H4 buffer before starting RxLoop, removing the original
     * start-before-buffer race without changing its receive callbacks. */
    typedef void (*buffer_fn)(void *,uint32_t,uint32_t);
    ((buffer_fn)address(0x1000e6948))((void *)address(0x1008484e0),4096,0x4c0);
    typedef int (*cond_fn)(void *);
    if(!((cond_fn)address(0x10009bc1c))((void *)address(0x1008494f0))) {
        close(fd); *u32(0x1008441c8)=UINT32_MAX; return 0x4b1;
    }
    typedef int (*thread_fn)(void *,void *(*)(void *),void *);
    int created=((thread_fn)address(0x10009ba90))((void *)address(0x100849520),
                       (void *(*)(void *))address(0x100067908),NULL);
    if(!(created&1)) {
        close(fd); *u32(0x1008441c8)=UINT32_MAX;
        ((cond_fn)address(0x10009bc40))((void *)address(0x1008494f0));
        return 0x4b1;
    }
    x9_bt_note("X9 H4: native RxLoop/TX on cu.bluetooth; board owns baud and initialization\n");
    return 0;
}
/* Log 433 teardown crash: the function at 0x1000f3b44 calls the transport
 * thunk at 0x1000e6aac, which loads the current table from 0x100844850,
 * checks only its type word, and tail-jumps to table+0x60. The H4 table
 * (0x100844228, type 1) has +0x60 == NULL in the stock image, so shutdown
 * jumps to 0. The guard below replaces ONLY the BL at 0x1000f3b4c inside
 * that one teardown function; the thunk's other caller (0x1003ca25c) and
 * the table itself are untouched (no slot filling). It replicates the
 * thunk's exact register contract (clobbers x8/x9 only, x30 preserved):
 * table NULL, type 0, or a NULL +0x60 slot returns instead of jumping;
 * any real slot tail-jumps to the unmodified thunk. The next teardown
 * step calls the +0x20 thunk, whose H4 slot is non-NULL (0x1000685c0). */
__attribute__((used)) static uintptr_t td_table_global;
__attribute__((used)) static uintptr_t td_thunk;
__attribute__((naked)) static void teardown_guard(void)
{
    __asm__ volatile(
        "adrp x8, _td_table_global@PAGE\n\t"
        "ldr  x8, [x8, _td_table_global@PAGEOFF]\n\t"
        "ldr  x8, [x8]\n\t"
        "cbz  x8, 1f\n\t"
        "ldr  w9, [x8]\n\t"
        "cbz  w9, 1f\n\t"
        "ldr  x9, [x8, #0x60]\n\t"
        "cbz  x9, 1f\n\t"
        "adrp x8, _td_thunk@PAGE\n\t"
        "ldr  x8, [x8, _td_thunk@PAGEOFF]\n\t"
        "br   x8\n\t"
        "1: ret\n\t");
}
static int install_teardown_guard(void)
{
    static const struct {
        uint64_t va; uint32_t words[6]; uint32_t count;
    } contract[] = {
        {0x1000f3b44,{0xa9bf7bfd,0x910003fd,0x97ffcbd8,0x94027bea},4},
        {0x1000e6aac,{0xd0003ae8,0xf9442908,0xb9400109,0x34000069,
                      0xf9403100,0xd61f0000},6},
    };
    for(size_t k=0;k<sizeof(contract)/sizeof(contract[0]);k++) {
        if(memcmp((void *)address(contract[k].va),contract[k].words,
                  contract[k].count*sizeof(uint32_t))) {
            x9_bt_note("X9 H4: unexpected teardown/thunk instructions; no table changed\n");
            return -1;
        }
    }
    td_table_global=address(0x100844850);
    td_thunk=address(0x1000e6aac);
    uintptr_t call=address(0x1000f3b4c);
    int64_t offset=(int64_t)((uintptr_t)teardown_guard-call);
    if(offset%4 || offset<-(INT64_C(1)<<27) || offset>=(INT64_C(1)<<27)) {
        x9_bt_note("X9 H4: teardown guard out of BL range (%lld); no table changed\n",
                   (long long)offset);
        return -1;
    }
    uint32_t word=0x94000000u|(uint32_t)((offset/4)&0x3ffffff);
    vm_size_t page=vm_page_size;
    uintptr_t target=call&~(page-1);
    kern_return_t kr=vm_protect(mach_task_self(),target,page,FALSE,
                                VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if(kr) {
        x9_bt_note("X9 H4: teardown guard COW failed kr=%d; no table changed\n",kr);
        return -1;
    }
    *(uint32_t *)call=word;
    sys_icache_invalidate((void *)call,4);
    kr=vm_protect(mach_task_self(),target,page,FALSE,
                  VM_PROT_READ|VM_PROT_EXECUTE);
    if(kr) {
        x9_bt_note("X9 H4: teardown guard R|X failed kr=%d\n",kr);
        return -1;
    }
    x9_bt_note("X9 H4: teardown +0x60 null-slot guard installed at 0x1000f3b4c\n");
    return 0;
}
__attribute__((constructor)) static void install(void)
{
    /* Image 0 is not guaranteed to be the main executable once
     * DYLD_INSERT_LIBRARIES is set; pick the one MH_EXECUTE image and use
     * its own slide. Every early return reports why (log bt3: silent). */
    const struct mach_header_64 *h=NULL; uint32_t index=UINT32_MAX,count=_dyld_image_count();
    for(uint32_t i=0;i<count;i++) {
        const struct mach_header_64 *m=(const struct mach_header_64 *)_dyld_get_image_header(i);
        if(m && m->magic==MH_MAGIC_64 && m->filetype==MH_EXECUTE) { h=m; index=i; break; }
    }
    x9_bt_note("X9 H4: constructor entered (%s) exe=%u/%u\n",getprogname(),index,count);
    if(!h) { x9_bt_note("X9 H4: no MH_EXECUTE image; no table changed\n"); return; }
    if(h->cputype!=CPU_TYPE_ARM64 || (h->cpusubtype&~CPU_SUBTYPE_MASK)!=0 || h->sizeofcmds>65536) {
        x9_bt_note("X9 H4: unexpected header cpu=%#x sub=%#x cmds=%u; no table changed\n",
                   (unsigned)h->cputype,(unsigned)h->cpusubtype,(unsigned)h->sizeofcmds);
        return;
    }
    const uint8_t *at=(const uint8_t *)(h+1),*end=at+h->sizeofcmds;
    int locked=0;
    for(uint32_t k=0;k<h->ncmds;k++) {
        const struct load_command *c=(const struct load_command *)at;
        if((size_t)(end-at)<sizeof(struct load_command) ||
           c->cmdsize<sizeof(*c) || c->cmdsize>(size_t)(end-at)) {
            x9_bt_note("X9 H4: malformed load command %u; no table changed\n",k); return;
        }
        if(c->cmd==LC_UUID && c->cmdsize==sizeof(struct uuid_command))
            locked=!memcmp(((const struct uuid_command *)c)->uuid,original_uuid,sizeof(original_uuid));
        at+=c->cmdsize;
    }
    if(!locked) { x9_bt_note("X9 H4: unsupported bluetoothd UUID; no table changed\n"); return; }
    slide=_dyld_get_image_vmaddr_slide(index);
    if(*uptr(OPEN_SLOT)!=address(NATIVE_OPEN) ||
       *uptr(CLOSE_SLOT)!=address(NATIVE_CLOSE) ||
       *uptr(0x100844180)!=address(NATIVE_WRITE)) {
        x9_bt_note("X9 H4: unexpected resolved transport slots; no table changed\n"); return;
    }
    /* Close cancellation depends on the original stop/exit handshake, not
     * merely the symbol name. Check every instruction used by that contract
     * before any transport slot or vendor text is changed. */
    static const struct {
        uint64_t va; uint32_t words[4]; uint32_t count;
    } close_contract[] = {
        {0x1000674d0,{0xd10183ff,0xa9025ff8,0xa90357f6,0xa9044ff4},4},
        {0x1000674f8,{0xd0003f08,0x52800029,0xb904e909},3},
        {0x100067af0,{0xb944eae8,0x34000148,0x940084ea,0xd0003f08},4},
        {0x100067b00,{0x52800029,0xb904ed09,0xd0003f00,0x9113c000},4},
    };
    for(size_t k=0;k<sizeof(close_contract)/sizeof(close_contract[0]);k++) {
        if(memcmp((void *)address(close_contract[k].va),close_contract[k].words,
                  close_contract[k].count*sizeof(uint32_t))) {
            x9_bt_note("X9 H4: unexpected native close/RX handshake; no table changed\n");
            return;
        }
    }
    if(install_teardown_guard()<0) return;
    /* __DATA is writable in this version. Apart from the single verified
     * teardown BL above, neither text nor chained fixups are edited; the
     * unchanged original image remains the replay input. */
    if(x9_vendor16_install(slide)<0) return;
    *uptr(OPEN_SLOT)=(uintptr_t)serial_open;
    *uptr(CLOSE_SLOT)=(uintptr_t)serial_close;
    x9_bt_note("X9 H4: POSIX open/close routed to cu.bluetooth\n");
}
