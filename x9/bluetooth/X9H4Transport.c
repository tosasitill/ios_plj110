/* 20H392 arm64 native H4 -> X9 BSD serial adapter.
 * Keep Apple H4 parser, RX loop, stack locking, TX and close. Replace only
 * POSIX.open: in this build that slot otherwise opens UART.SK/Skywalk.
 * Optional X9Vendor16 routing is a separately gated, version-locked adapter.
 * No firmware, vendor capability constants, BlueTool success fabrication,
 * or device writes occur in the constructor. */
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <unistd.h>

#define BASE UINT64_C(0x100000000)
#define OPEN_SLOT UINT64_C(0x100844168)
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
static int serial_open(uint32_t mode,const char *configuration)
{
    (void)configuration;
    /* The native H4 driver calls POSIX.open with mode0; do not silently
     * accept H4DS/H4BC callers with different flow-control semantics. */
    if(mode) return 0x65;
    int result=ENXIO; size_t n=sizeof(result);
    if(sysctlbyname("kern.x9_bt_boot",&result,&n,NULL,0) || n!=sizeof(result) || result) {
        fprintf(stderr,"X9 H4: complete MT6653 initializer not ready (errno=%d)\n",result);
        return 0x4b1;
    }
    if((int32_t)*u32(0x1008441c8)!=-1 || *uptr(0x1008494e0)) return 0x4b1;
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
    fprintf(stderr,"X9 H4: native RxLoop/TX on cu.bluetooth; board owns baud and initialization\n");
    return 0;
}
__attribute__((constructor)) static void install(void)
{
    const struct mach_header_64 *h=(const struct mach_header_64 *)_dyld_get_image_header(0);
    if(!h || h->magic!=MH_MAGIC_64 || h->cputype!=CPU_TYPE_ARM64 || h->cpusubtype!=0 ||
       h->filetype!=MH_EXECUTE || h->sizeofcmds>65536) return;
    const uint8_t *at=(const uint8_t *)(h+1),*end=at+h->sizeofcmds;
    int locked=0;
    for(uint32_t k=0;k<h->ncmds;k++) {
        if((size_t)(end-at)<sizeof(struct load_command)) return;
        const struct load_command *c=(const struct load_command *)at;
        if(c->cmdsize<sizeof(*c) || c->cmdsize>(size_t)(end-at)) return;
        if(c->cmd==LC_UUID && c->cmdsize==sizeof(struct uuid_command))
            locked=!memcmp(((const struct uuid_command *)c)->uuid,original_uuid,sizeof(original_uuid));
        at+=c->cmdsize;
    }
    if(!locked) { fprintf(stderr,"X9 H4: unsupported bluetoothd UUID; no table changed\n"); return; }
    slide=_dyld_get_image_vmaddr_slide(0);
    if(*uptr(OPEN_SLOT)!=address(NATIVE_OPEN) ||
       *uptr(0x100844178)!=address(NATIVE_CLOSE) ||
       *uptr(0x100844180)!=address(NATIVE_WRITE)) {
        fprintf(stderr,"X9 H4: unexpected resolved transport slots; no table changed\n"); return;
    }
    /* __DATA is writable in this version. Neither text nor chained fixups
     * are edited; the unchanged original image remains the replay input. */
    if(x9_vendor16_install(slide)<0) return;
    *uptr(OPEN_SLOT)=(uintptr_t)serial_open;
}
