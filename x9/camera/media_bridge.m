/* SPDX-License-Identifier: MIT
 * HakiOS FigCaptureDevice vendor adaptation; see LICENSE.hakios.
 * Exact 20H392 CMCapture UUID/IMP gates. No audio, VT, global GPU hooks.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
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
static id sharedVendor(id self,SEL selector)
{
    id real=((id(*)(id,SEL))originalVendor)(self,selector);
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
    if(!m || !init || method_getNumberOfArguments(m)!=2 || method_getNumberOfArguments(init)!=3 ||
       (uintptr_t)method_getImplementation(m)!=0x1a1f40c84+slide) {
        dprintf(2,"X9 CAMERA MEDIA: unsupported vendor ABI/IMP; not hooking\n");return;
    }
    originalVendor=method_setImplementation(m,(IMP)sharedVendor);hooked=true;
    dprintf(2,"X9 CAMERA MEDIA: exact 20H392 vendor hook; no vendor until physical backend ready\n");
}
__attribute__((constructor)) static void initCamera(void)
{
    if(strcmp(getprogname(),"mediaserverd"))return;
    size_t length=0;const void *cache=_dyld_get_shared_cache_range(&length);
    if(!cache || !length)return;
    slide=(uintptr_t)cache-0x180000000;
    _dyld_register_func_for_add_image(imageAdded);
}
