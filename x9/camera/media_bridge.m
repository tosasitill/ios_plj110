/* SPDX-License-Identifier: MIT
 * HakiOS FigCaptureDevice vendor adaptation; see LICENSE.hakios.
 * Exact 20H392 CMCapture UUID/IMP gates. Capture profile/software HEVC live
 * in media_profile.m; no audio or global GPU hooks.
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
#include "camera_status.h"
#include "camera_profile.h"
#include "../torch/media_callers.h"

extern int x9_camera_device_create(CFAllocatorRef, CFDictionaryRef, void **);
extern const void *_dyld_get_shared_cache_range(size_t *length);
static IMP originalVendor;
static id (*torchVendorWithCaller)(id,SEL,uintptr_t);
static uintptr_t slide;
static bool hooked;
static id ours;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t uuid[16]={0x45,0xd3,0x74,0x88,0x2c,0xc7,0x30,0x28,0x8a,0x4d,0x93,0x6b,0xce,0x7a,0x22,0x0b};

id x9_camera_media_vendor_with_caller(id self,SEL selector,uintptr_t returnAddress)
{
    /* Native capture is the selected provider only while its current status
     * independently proves both physical sensors ready. An Apple object can
     * exist without a useful camera; it must not mask a ready X9 provider.
     * Flashlight remains on its separately scoped bridge (native TorchLevel
     * is explicitly unsupported). Neither hook order changes that scope. */
    if(!x9_media_flashlight_caller(returnAddress,slide)) {
        pthread_mutex_lock(&lock);
        id result=nil;
        if(x9_camera_backend_ready()) {
            if(!ours) {
                id made=((id(*)(id,SEL))objc_msgSend)(self,sel_registerName("alloc"));
                ours=((id(*)(id,SEL,void*))objc_msgSend)(made,sel_registerName("initWithDeviceCreateFunction:"),
                                                       (void*)x9_camera_device_create);
                dprintf(2,"X9 CAMERA MEDIA: physical backend ready; physical-stream vendor %s\n",ours?"created":"failed");
            }
            /* Recheck after init too: construction is not a hardware lease. */
            if(ours && x9_camera_backend_ready())result=ours;
        }
        pthread_mutex_unlock(&lock);
        if(result)return result;
    }
    /* Preserve both the original provider and the exact FigFlashlight caller
     * when no native provider can currently be returned. */
    return torchVendorWithCaller ? torchVendorWithCaller(self,selector,returnAddress) :
        ((id(*)(id,SEL))originalVendor)(self,selector);
}
__attribute__((noinline)) id x9_camera_media_shared_vendor(id self,SEL selector)
{
    return x9_camera_media_vendor_with_caller(self,selector,(uintptr_t)__builtin_return_address(0));
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
    x9_camera_install_profile(slide);
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
