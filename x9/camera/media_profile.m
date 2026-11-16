/* SPDX-License-Identifier: MIT
 * 20H392 single-source capture profile and Apple software HEVC registration.
 * Original Apple assets stay unchanged. No fake synchronized camera, codec
 * substitution, hardware encoder claim, compression test or disk writes.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "camera_profile.h"
#include "camera_status.h"

static pthread_once_t codec_once = PTHREAD_ONCE_INIT;
static bool hevc_registered;
static void *codec_image, *vt_image; /* registered factory must stay loaded */
static IMP original_plist;
static int (*original_hevc_create)(void *, void *, void **);

/* The original 20H392 factory carries x0/x1 through and writes the object to
 * x2; its raw entry/exit were qualified before adding this passthrough. Log
 * actual selection/creation, not a fake compressed-frame/save completion. */
static int create_hevc(void *a, void *b, void **out)
{
    int result = original_hevc_create(a, b, out);
    dprintf(2, "X9 CAMERA VIDEO: real HEVC factory result=%d object=%u; not a recording receipt\n",
            result, out && *out != NULL);
    return result;
}

static bool image_uuid(const void *symbol, const uint8_t expected[16], const void **base)
{
    Dl_info info = {0};
    if (!symbol || !dladdr(symbol, &info) || !info.dli_fbase) return false;
    const struct mach_header_64 *mh = info.dli_fbase;
    if (mh->magic != MH_MAGIC_64 || mh->sizeofcmds > 65536) return false;
    const uint8_t *p = (const void *)(mh + 1), *end = p + mh->sizeofcmds;
    for (unsigned i = 0; i < mh->ncmds && p + sizeof(struct load_command) <= end; i++) {
        const struct load_command *c = (const void *)p;
        if (c->cmdsize < sizeof(*c) || c->cmdsize > (size_t)(end - p)) return false;
        if (c->cmd == LC_UUID && c->cmdsize == sizeof(struct uuid_command)) {
            if (memcmp(((const struct uuid_command *)c)->uuid, expected, 16)) return false;
            if (base) *base = info.dli_fbase;
            return true;
        }
        p += c->cmdsize;
    }
    return false;
}

static void register_hevc(void)
{
    static const uint8_t hevc_uuid[16] = {0x05,0x91,0x9c,0x35,0xb7,0x0d,0x3a,0xf2,
                                        0xb5,0xe6,0x7f,0xca,0xe8,0x04,0xf0,0x1f};
    static const uint8_t vt_uuid[16] = {0x4f,0xed,0xef,0x35,0x6b,0x79,0x35,0x86,
                                      0xb2,0x49,0x23,0x8e,0x04,0xa4,0xc9,0x51};
    codec_image = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
    vt_image = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_LOCAL);
    void *create = codec_image ? dlsym(codec_image, "HEVCVideoEncoder_CreateInstance") : NULL;
    int (*reg)(uint32_t, CFDictionaryRef, void *) = vt_image ? dlsym(vt_image, "VTRegisterVideoEncoderWithInfo") : NULL;
    const char *names[] = {"kVTVideoEncoderList_EncoderID", "kVTVideoEncoderList_EncoderName",
                           "kVTVideoEncoderList_IsHardwareAccelerated"};
    CFStringRef key[3];
    const void *vt_base = NULL;
    bool valid = image_uuid(create, hevc_uuid, NULL) && image_uuid((const void *)reg, vt_uuid, &vt_base);
    for (unsigned i = 0; i < 3 && valid; i++) {
        CFStringRef *slot = dlsym(vt_image, names[i]);
        const void *key_base = NULL;
        valid = image_uuid(slot, vt_uuid, &key_base) && key_base == vt_base;
        if (valid) {
            key[i] = *slot;
            valid = key[i] && CFGetTypeID(key[i]) == CFStringGetTypeID();
        }
    }
    if (!valid) {
        dprintf(2, "X9 CAMERA VIDEO: unsupported 20H392 HEVC/VT UUID or symbols; Photo only\n");
        return;
    }
    const void *value[] = {CFSTR("com.x9.videotoolbox.videoencoder.hevc.software"),
                           CFSTR("Apple VCPHEVC (software, X9)"), kCFBooleanFalse};
    CFDictionaryRef info = CFDictionaryCreate(NULL, (const void **)key, value, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!info) return;
    original_hevc_create = (int (*)(void *, void *, void **))create;
    int result = reg(0x68766331u /* hvc1, never avc1 */, info, (void *)create_hevc);
    CFRelease(info);
    hevc_registered = result == 0;
    dprintf(2, "X9 CAMERA VIDEO: software HEVC registration=%d; no hardware/encode/save claim\n", result);
}

bool x9_camera_software_hevc_ready(void)
{
    if (strcmp(getprogname(), "mediaserverd")) return false;
    pthread_once(&codec_once, register_hevc);
    return hevc_registered;
}

bool x9_camera_video_profile_ready(void)
{
    return original_plist && x9_camera_software_hevc_ready();
}

/* Original D22 devices remain the identity/position authority. Replace only
 * their capability dictionaries with the actual single-source X9 formats.
 * Native preview is 4:3; the renderer crops to each requested video aspect.
 * Nothing advertises 4K/60, HDR, stabilization, depth or a virtual dual group.
 */
static NSDictionary *device_profile(NSDictionary *original, bool front, bool video)
{
    NSMutableDictionary *d = [original mutableCopy];
    NSString *photo = front ? @"7MP" : @"12MP";
    unsigned pw = front ? 3088 : 4032, ph = front ? 2320 : 3024;
    unsigned vw = front ? 3264 : 4096, vh = front ? 2448 : 3072;
    NSMutableDictionary *formats = [@{photo: @{@"CaptureStreamFormat": @"Photo", @"Width": @(pw),
                                               @"Height": @(ph), @"MaxFrameRateOverride": @30}} mutableCopy];
    NSMutableDictionary *streams = [@{@"Photo": @{@"SensorWidth": @(pw), @"SensorHeight": @(ph),
                                                  @"MaxFrameRate": @30}} mutableCopy];
    NSMutableDictionary *presets = [@{@"AVCaptureSessionPresetPhoto":
                                      @{@"DeviceFormat": photo, @"MinFrameRate": @15, @"MaxFrameRate": @30}} mutableCopy];
    if (video) {
        streams[@"X9-Video-30"] = @{@"SensorWidth": @(vw), @"SensorHeight": @(vh), @"MaxFrameRate": @30};
        NSArray *sizes = @[@[@"1080p30", @1920, @1080, @"AVCaptureSessionPreset1920x1080"],
                           @[@"720p30", @1280, @720, @"AVCaptureSessionPreset1280x720"],
                           @[@"640x480", @640, @480, @"AVCaptureSessionPreset640x480"]];
        for (NSArray *size in sizes) {
            formats[size[0]] = @{@"CaptureStreamFormat": @"X9-Video-30", @"Width": size[1],
                                @"Height": size[2], @"MaxFrameRateOverride": @30,
                                @"MinFrameRateOverride": @2};
            presets[size[3]] = @{@"DeviceFormat": size[0], @"MinFrameRate": @30, @"MaxFrameRate": @30,
                                 @"VideoCodec": @"hvc1"};
        }
        presets[@"AVCaptureSessionPresetHigh"] = @"AVCaptureSessionPreset1920x1080";
        presets[@"Common"] = @{@"MinFrameRate": @30, @"MaxFrameRate": @30, @"VideoCodec": @"hvc1",
            @"VideoCompressionProperties": @{@"MaxKeyFrameIntervalDuration": @1, @"AverageBitRate": @6000000}};
    }
    d[@"MultiStream"] = @{@"AVCaptureDeviceFormats": formats, @"FigCaptureStreamFormats": streams,
                           @"AVCaptureSessionPresets": presets};
    /* Known original D22 feature switches whose providers are not present. */
    for (NSString *key in @[@"hasAutoFocus", @"hasFlash", @"hasTorch", @"smoothAutoFocusSupported",
                            @"sphereModeStillActivePreviewSupported", @"multiPassIspMBNRSupported",
                            @"displayPipeRequiresPacked10BitPixelFormatForP3Input",
                            @"wideColorSupported", @"smartCameraSupported", @"faceTrackingSupported"])
        d[key] = @NO;
    for (NSString *key in @[@"synchronizedStreamUniqueIDs", @"synchronizedStreamsSwitchOverParameters",
                            @"synchronizedStreamsSlaveFrameRates"])
        [d removeObjectForKey:key];
    NSDictionary *result = [[d copy] autorelease];
    [formats release]; [streams release]; [presets release]; [d release];
    return result;
}

static id capture_plist(id self, SEL selector, NSURL *url)
{
    id original = ((id (*)(id, SEL, id))original_plist)(self, selector, url);
    /* Read the original first. Do not supply a profile if its real asset or
     * physical backend is unavailable, or modify other frameworks/plists. */
    if (![url isKindOfClass:[NSURL class]] || !url.isFileURL ||
        ![url.path hasSuffix:@"/CMCapture.framework/D22/AVCaptureSession.plist"] ||
        ![original isKindOfClass:[NSDictionary class]]) return original;
    uint32_t mask = x9_camera_backend_mask();
    if (!mask) return original;
    NSArray *devices = original[@"AVCaptureDevices"];
    if (![devices isKindOfClass:[NSArray class]] || devices.count > 16) return original;
    bool video = x9_camera_software_hevc_ready();
    NSMutableArray *selected = [NSMutableArray array];
    bool wide = false, front = false, tele = false;
    for (id item in devices) {
        if (![item isKindOfClass:[NSDictionary class]]) return original;
        NSString *uid = item[@"uniqueID"];
        if ([uid isEqual:@"com.apple.avfoundation.avcapturedevice.built-in_video:0"]) {
            if (wide) return original;
            wide = true; [selected addObject:device_profile(item, false, video)];
        } else if ([uid isEqual:@"com.apple.avfoundation.avcapturedevice.built-in_video:1"]) {
            if (front) return original;
            front = true; [selected addObject:device_profile(item, true, video)];
        } else if ([uid isEqual:@"com.apple.avfoundation.avcapturedevice.built-in_video:2"]) {
            if (tele) return original;
            tele = true;
            if (mask & (1u << X9_CAMERA_TELEPHOTO)) [selected addObject:device_profile(item, false, video)];
        } else if ([item[@"mediaType"] isEqual:@"soun"]) {
            [selected addObject:item]; /* original microphone identity/configuration stays intact */
        }
        /* Do not leave fake dual/depth/Pearl devices referring to absent streams. */
    }
    if (!wide || !front || ((mask & (1u << X9_CAMERA_TELEPHOTO)) && !tele)) return original;
    NSMutableDictionary *copy = [original mutableCopy];
    copy[@"AVCaptureDevices"] = selected;
    NSDictionary *common = original[@"AVCaptureDeviceCommonSettings"];
    if ([common isKindOfClass:[NSDictionary class]]) {
        NSMutableDictionary *c = [common mutableCopy];
        c[@"wideColorSupported"] = @NO; c[@"faceTrackingSupported"] = @NO; c[@"smartCameraSupported"] = @NO;
        c[@"HEVCPreferred"] = @(video);
        copy[@"AVCaptureDeviceCommonSettings"] = c;
        [c release];
    }
    id result = [[copy copy] autorelease];
    [copy release];
    dprintf(2, "X9 CAMERA PROFILE: D22 physical-only mask=%x Photo%s; original audio retained\n",
            mask, video ? "+1080p30 HEVC (software)" : " only");
    return result;
}

void x9_camera_install_profile(uintptr_t slide)
{
    Method method = class_getClassMethod(objc_getClass("FigCaptureFlatPlist"),
                                          sel_registerName("flatPlistWithContentsOfURL:"));
    if (!method || method_getNumberOfArguments(method) != 3 ||
        (uintptr_t)method_getImplementation(method) != 0x1a208b90cull + slide) {
        dprintf(2, "X9 CAMERA PROFILE: unsupported 20H392 plist loader; unchanged\n");
        return;
    }
    /* Caller has already qualified the CMCapture image UUID. */
    original_plist = method_setImplementation(method, (IMP)capture_plist);
    dprintf(2, "X9 CAMERA PROFILE: exact D22 URL-loader hook; requires physical backend\n");
}
