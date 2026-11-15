/* SPDX-License-Identifier: MIT
 * HakiOS FigCaptureDevice vendor adaptation; see LICENSE.hakios.
 * Exact 20H392 CMCapture UUID/IMP gates. No audio, VT, global GPU hooks.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../kernel/src/bsd/dev/x9_camera.h"

extern int x9_camera_device_create(CFAllocatorRef, CFDictionaryRef, void **);
extern const void *_dyld_get_shared_cache_range(size_t *length);
static IMP originalVendor;
static id (*torchVendorWithCaller)(id,SEL,uintptr_t);
static uintptr_t slide;
static bool hooked;
static id ours;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t uuid[16]={0x45,0xd3,0x74,0x88,0x2c,0xc7,0x30,0x28,0x8a,0x4d,0x93,0x6b,0xce,0x7a,0x22,0x0b};

static bool backendReady(void)
{
    int fd=open(X9_CAMERA_DEVICE,O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    struct x9_camera_status s={0};int e=ioctl(fd,X9_CAMERA_GET_STATUS,&s);close(fd);
    return !e && s.version==1 && s.mode==2 && (s.flags&X9_CAMERA_READY) &&
        !(s.flags&X9_CAMERA_FAULT) && s.supported_mask==((1u<<1)|(1u<<2));
}
__attribute__((noinline)) id x9_camera_media_shared_vendor(id self,SEL selector)
{
    /* Preserve the exact FigFlashlight caller when torch was installed first.
     * Calling its IMP normally would substitute this library's return address. */
    id real=torchVendorWithCaller ? torchVendorWithCaller(self,selector,
        (uintptr_t)__builtin_return_address(0)) : ((id(*)(id,SEL))originalVendor)(self,selector);
    if(real)return real;
    /* Once made, the vendor's workers own the device. Reopening the exclusive
     * char device here would return EBUSY and revoke an active vendor. */
    pthread_mutex_lock(&lock);
    if(!ours && backendReady()) {
        id made=((id(*)(id,SEL))objc_msgSend)(self,sel_registerName("alloc"));
        ours=((id(*)(id,SEL,void*))objc_msgSend)(made,sel_registerName("initWithDeviceCreateFunction:"),
                                               (void*)x9_camera_device_create);
        dprintf(2,"X9 CAMERA MEDIA: physical backend ready; dual-stream vendor %s\n",ours?"created":"failed");
    }
    id result=ours;pthread_mutex_unlock(&lock);return result;
}
static bool matches(const struct mach_header *mh)
{
    if(mh->magic!=MH_MAGIC_64 || mh->sizeofcmds>65536)return false;
    const uint8_t *p=(const void*)((const struct mach_header_64*)mh+1),*end=p+mh->sizeofcmds;
    for(unsigned i=0;i<mh->ncmds && p+sizeof(struct load_command)<=end;i++) {
        const struct load_command *c=(const void*)p;
        if(c->cmdsize<sizeof(*c) || c->cmdsize>(size_t)(end-p))return false;
        if(c->cmd==LC_UUID && c->cmdsize==sizeof(struct uuid_command))
            return !memcmp(((const struct uuid_command*)c)->uuid,uuid,16);
        p+=c->cmdsize;
    }
    return false;
}
static void imageAdded(const struct mach_header *mh,__unused intptr_t imageSlide)
{
    if(hooked || !matches(mh))return;
    Class cls=objc_getClass("BWFigCaptureDeviceVendor");
    Method m=class_getClassMethod(cls,sel_registerName("sharedCaptureDeviceVendor"));
    Method init=class_getInstanceMethod(cls,sel_registerName("initWithDeviceCreateFunction:"));
    IMP current=m ? method_getImplementation(m) : NULL;
    IMP torchHook=(IMP)dlsym(RTLD_DEFAULT,"x9_torch_media_shared_vendor");
    id (*forward)(id,SEL,uintptr_t)=dlsym(RTLD_DEFAULT,"x9_torch_media_vendor_with_caller");
    Dl_info hookImage={0},forwardImage={0};
    /* Both symbols must come from the actual object owning the current IMP,
     * not a different/stale dylib that happens to export the helper name. */
    bool torchChain=current && torchHook && current==torchHook && forward &&
        dladdr((const void*)torchHook,&hookImage) && dladdr((const void*)forward,&forwardImage) &&
        hookImage.dli_fbase && hookImage.dli_fbase==forwardImage.dli_fbase;
    if(!m || !init || method_getNumberOfArguments(m)!=2 || method_getNumberOfArguments(init)!=3 ||
       ((uintptr_t)current!=0x1a1f40c84+slide && !torchChain)) {
        dprintf(2,"X9 CAMERA MEDIA: unsupported vendor ABI/IMP; not hooking\n");return;
    }
    torchVendorWithCaller=torchChain ? forward : NULL;
    originalVendor=method_setImplementation(m,(IMP)x9_camera_media_shared_vendor);hooked=true;
    dprintf(2,"X9 CAMERA MEDIA: exact 20H392 vendor hook%s; no vendor until physical backend ready\n",
        torchChain ? " (retaining torch caller gate)" : "");
}
__attribute__((constructor)) static void initCamera(void)
{
    if(strcmp(getprogname(),"mediaserverd"))return;
    size_t length=0;const void *cache=_dyld_get_shared_cache_range(&length);
    if(!cache || !length)return;
    slide=(uintptr_t)cache-0x180000000;
    _dyld_register_func_for_add_image(imageAdded);
}
