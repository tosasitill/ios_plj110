/* SPDX-License-Identifier: MIT
 * FigFlashlight vendor/stream design adapted from HakiOS inject_media.c.
 * Copyright (c) 2025-2026 the HakiOS authors; X9 adaptation 2026.
 * See LICENSE.hakios. Narrow 20H392/CMCapture UUID + IMP + caller gates;
 * no fake camera, no blanket capture vendor, no Qualcomm MMIO.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "../kernel/src/bsd/dev/x9_torch.h"

static int torchFD = -1;
static uintptr_t cacheSlide;
static IMP originalVendor;
static pthread_mutex_t torchLock = PTHREAD_MUTEX_INITIALIZER;
static bool vendorHooked, prewarmHooked;
/* Live-device 20H392 cache extraction, not hackios's 20F66 addresses. */
static const uint8_t cmUUID[16] = {0x45,0xd3,0x74,0x88,0x2c,0xc7,0x30,0x28,0x8a,0x4d,0x93,0x6b,0xce,0x7a,0x22,0x0b};
static const uintptr_t flashlightCallers[] = {
    0x1a20007d0, /* IsAvailable */
    0x1a20009bc, /* NotifyForCurrentState */
    0x1a2000abc, 0x1a2000ae8, 0x1a2000b04, 0x1a2000b2c, /* setupDevice */
    0x1a1f3e5a8, /* teardownDevice */
};
extern const void *_dyld_get_shared_cache_range(size_t *length);

static bool available(void) {
    struct x9_torch_status s;
    return torchFD >= 0 && ioctl(torchFD, X9_TORCH_GET_STATUS, &s) == 0 &&
        s.version == 1 && s.mode == 3 && (s.flags & X9_TORCH_READY) &&
        !(s.flags & (X9_TORCH_LATCHED | X9_TORCH_OFF_FAILED));
}
static int setLevel(uint32_t level) {
    pthread_mutex_lock(&torchLock);
    ssize_t n = write(torchFD, &level, sizeof(level)); int e = n == sizeof(level) ? 0 : errno ? errno : EIO;
    struct x9_torch_status s = {0};
    int se = ioctl(torchFD, X9_TORCH_GET_STATUS, &s);
    dprintf(2, "X9 TORCH MEDIA: requested=%u write_errno=%d status_errno=%d confirmed=%u flags=%x\n",
            level, e, se ? errno : 0, s.level, s.flags);
    pthread_mutex_unlock(&torchLock); return e ? -e : se ? -EIO : 0;
}
@interface X9TorchDevice : NSObject @end
@implementation X9TorchDevice @end
@interface X9TorchStream : NSObject
- (NSDictionary *)supportedProperties;
- (int)setProperty:(id)key value:(id)value;
@end
@implementation X9TorchStream
- (NSDictionary *)supportedProperties { return @{}; } /* No HighCurrentTorchEnabled. */
- (int)setProperty:(id)key value:(id)value {
    if (![key isKindOfClass:[NSString class]]) return -EINVAL;
    if (![key isEqual:@"TorchLevel"]) return 0; /* Unrequested optional properties. */
    if (![value isKindOfClass:[NSNumber class]]) return -EINVAL;
    double v = [value doubleValue];
    if (!isfinite(v) || v < 0 || v > 1) return -EINVAL;
    return setLevel((uint32_t)(v * 1000 + 0.5)); /* Propagate rejection to FigFlashlight. */
}
@end
@interface X9TorchVendor : NSObject
- (int)registerClientWithPID:(int)pid clientDescription:(id)description clientPriority:(int)priority
    canStealFromClientsWithSamePriority:(BOOL)steal deviceSharingWithOtherClientsAllowed:(BOOL)share
    deviceAvailabilityChangedHandler:(id)handler;
- (id)copyDeviceForClient:(int)client informClientWhenDeviceAvailableAgain:(BOOL)inform error:(int *)error;
- (id)copyStreamForFlashlightWithPosition:(int)position deviceType:(int)type forDevice:(id)device;
- (void)takeBackDevice:(id)device forClient:(int)client informClientWhenDeviceAvailableAgain:(BOOL)inform;
- (BOOL)activeDeviceEquals:(id)device;
@end
@implementation X9TorchVendor
- (int)registerClientWithPID:(int)pid clientDescription:(id)description clientPriority:(int)priority
    canStealFromClientsWithSamePriority:(BOOL)steal deviceSharingWithOtherClientsAllowed:(BOOL)share
    deviceAvailabilityChangedHandler:(id)handler { return 1; }
- (id)copyDeviceForClient:(int)client informClientWhenDeviceAvailableAgain:(BOOL)inform error:(int *)error {
    bool ok = available(); if (error) *error = ok ? 0 : -ENXIO;
    return ok ? [X9TorchDevice new] : nil;
}
- (id)copyStreamForFlashlightWithPosition:(int)position deviceType:(int)type forDevice:(id)device {
    return [device isKindOfClass:[X9TorchDevice class]] ? [X9TorchStream new] : nil;
}
- (void)takeBackDevice:(id)device forClient:(int)client informClientWhenDeviceAvailableAgain:(BOOL)inform {
    (void)setLevel(0);
}
- (BOOL)activeDeviceEquals:(id)device {
    return available() && (!device || [device isKindOfClass:[X9TorchDevice class]]);
}
@end

static id sharedVendor(id self, SEL selector) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0) - cacheSlide;
    id real = ((id (*)(id, SEL))originalVendor)(self, selector);
    if (real) return real; /* Never replace a genuine device vendor. */
    for (unsigned i = 0; i < sizeof(flashlightCallers)/sizeof(flashlightCallers[0]); i++) {
        if (caller == flashlightCallers[i]) {
            static X9TorchVendor *ours; static dispatch_once_t once;
            dispatch_once(&once, ^{ ours = [X9TorchVendor new];
                dprintf(2, "X9 TORCH MEDIA: FigFlashlight-only vendor created\n"); });
            return ours;
        }
    }
    return nil;
}
static void skipPortraitPrewarm(id self, SEL selector, int version, BOOL styles) {}
static bool matchingCMCapture(const struct mach_header *mh) {
    if (mh->magic != MH_MAGIC_64 || mh->sizeofcmds > 65536) return false;
    const uint8_t *p = (const uint8_t *)(mh + 1) + 4; /* mach_header_64 reserved */
    const uint8_t *end = p + mh->sizeofcmds;
    for (unsigned i = 0; i < mh->ncmds && p + sizeof(struct load_command) <= end; i++) {
        const struct load_command *c = (const void *)p;
        if (c->cmdsize < sizeof(*c) || c->cmdsize > (size_t)(end-p)) return false;
        if (c->cmd == LC_UUID && c->cmdsize == sizeof(struct uuid_command))
            return !memcmp(((const struct uuid_command *)c)->uuid, cmUUID, 16);
        p += c->cmdsize;
    }
    return false;
}
static void imageAdded(const struct mach_header *mh, intptr_t imageSlide) {
    if (torchFD < 0 || !matchingCMCapture(mh)) return;
    Method vendor = class_getClassMethod(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("sharedCaptureDeviceVendor"));
    if (!vendorHooked && vendor && (uintptr_t)method_getImplementation(vendor) == 0x1a1f40c84 + cacheSlide) {
        originalVendor = method_setImplementation(vendor, (IMP)sharedVendor); vendorHooked = true;
        dprintf(2, "X9 TORCH MEDIA: 20H392 sharedCaptureDeviceVendor hook installed\n");
    }
    /* hackios's narrow no-ISP portrait prewarm fix; not audio/video hooks. */
    Method prewarm = class_getClassMethod(objc_getClass("BWCoreImageFilterRenderer"),
        sel_registerName("prewarmPortraitFilterVersion:semanticStyleFilters:"));
    if (!prewarmHooked && prewarm && (uintptr_t)method_getImplementation(prewarm) == 0x1a2165b8c + cacheSlide) {
        method_setImplementation(prewarm, (IMP)skipPortraitPrewarm); prewarmHooked = true;
        dprintf(2, "X9 TORCH MEDIA: 20H392 portrait prewarm skipped (no native camera)\n");
    }
}
__attribute__((constructor)) static void initTorch(void) {
    if (strcmp(getprogname(), "mediaserverd")) return;
    size_t length = 0; const void *cache = _dyld_get_shared_cache_range(&length);
    if (!cache || !length) return;
    cacheSlide = (uintptr_t)cache - 0x180000000;
    torchFD = open(X9_TORCH_DEVICE, O_WRONLY | O_CLOEXEC);
    if (!available()) { if (torchFD >= 0) close(torchFD); torchFD = -1; return; }
    dprintf(2, "X9 TORCH MEDIA: backend opened; waiting for exact 20H392 CMCapture\n");
    _dyld_register_func_for_add_image(imageAdded);
}
__attribute__((destructor)) static void stopTorch(void) {
    if (torchFD >= 0) { (void)setLevel(0); close(torchFD); torchFD = -1; }
}
