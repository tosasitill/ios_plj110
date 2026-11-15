/* Own-code VideoToolbox decoder for native HEVC codec sessions.
 * 20H392 ABI: VCPHEVC base v1, decoder v2 (no native tile slots).
 * No constructor/interposition: registration is explicit and hardware-gated.
 * Apple VCPHEVC, CMPhoto/ImageIO container handling and alpha are untouched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>
#include "hevc_bitstream.h"
#include "vt_decoder.h"
#include "vt_image_metadata.h"
#include "../kernel/src/bsd/dev/x9_vdec.h"

typedef struct OpaqueCMBaseObject *Base;
typedef const void *ClassID;
struct base_class {
    uint64_t version, storage_size;
    Boolean (*equal)(Base, Base);
    OSStatus (*invalidate)(Base);
    void (*finalize)(Base);
    CFStringRef (*debug)(Base);
    OSStatus (*copy_property)(Base, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*set_property)(Base, CFStringRef, CFTypeRef);
    OSStatus (*barrier)(Base);
    const void *protocols, *reserved;
};
struct decoder_class {
    uint64_t version;
    OSStatus (*start)(Base, void *session, CMVideoFormatDescriptionRef);
    OSStatus (*decode)(Base, void *frame, CMSampleBufferRef, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    OSStatus (*supported)(Base, CFDictionaryRef *);
    void *reserved1, *reserved2;
    Boolean (*accept)(Base, CMVideoFormatDescriptionRef);
    OSStatus (*finish)(Base);
};
struct vtable { const void *reserved; const struct base_class *base; const struct decoder_class *decoder; };
_Static_assert(sizeof(struct base_class) == 88, "20H392 base v1");
_Static_assert(sizeof(struct decoder_class) == 64, "20H392 decoder v2");
_Static_assert(offsetof(struct decoder_class, decode) == 0x10, "VT decode dispatch");
_Static_assert(offsetof(struct decoder_class, finish) == 0x38, "VT finish dispatch");
static OSStatus (*create_object)(CFAllocatorRef, const struct vtable *, ClassID, Base *);
static void *(*storage)(Base);
static ClassID (*class_id)(void);
static OSStatus (*emit)(void *, void *, OSStatus, VTDecodeInfoFlags, CVImageBufferRef);
static OSStatus (*attributes)(void *, CFDictionaryRef);
static OSStatus (*register_decoder)(CMVideoCodecType, CFDictionaryRef, void *);
static CFStringRef hardware_key, required_key, using_hardware_key;
static pthread_mutex_t registration_lock = PTHREAD_MUTEX_INITIALIZER;
static Boolean registered;
struct pending_frame {
    uint64_t token;
    void *frame;
    CMVideoFormatDescriptionRef description;
    Boolean suppress, deferred;
};
struct state {
    pthread_mutex_t lock;
    Boolean lock_initialized, invalid, emitting;
    int fd;
    void *session;
    uint64_t generation, native_session, next_token;
    uint32_t max_pending, pending_count;
    struct pending_frame pending[X9_VDEC_MAX_PENDING];
    CFDataRef hvcc;
    struct x9_hevc_config config;
};
static CFStringRef symbol_key(const char *name)
{
    CFStringRef *key = dlsym(RTLD_DEFAULT, name);
    return key ? *key : NULL;
}
static int hardware_status(int fd, struct x9_vdec_status *s)
{
    memset(s, 0, sizeof(*s));
    return ioctl(fd, X9_VDEC_GET_STATUS, s) == 0 && s->version == X9_VDEC_VERSION &&
        s->mode == 2 && s->ready == 1 && s->prerequisites == X9_VDEC_REQUIRED &&
        s->generation && (s->profiles & X9_VDEC_MAIN) &&
        !(s->profiles & ~(X9_VDEC_MAIN | X9_VDEC_MAIN10)) && s->max_width && s->max_height &&
        s->max_width <= X9_HEVC_MAX_DIM && s->max_height <= X9_HEVC_MAX_DIM;
}
static int hardware_sessions(int fd, const struct x9_vdec_status *s,
                             struct x9_vdec_capabilities *c)
{
    memset(c, 0, sizeof(*c));
    return ioctl(fd, X9_VDEC_GET_CAPABILITIES, c) == 0 &&
        c->version == X9_VDEC_VERSION && !c->reserved && c->generation == s->generation &&
        c->capabilities == X9_VDEC_SESSION_REQUIRED &&
        c->max_pending && c->max_pending <= X9_VDEC_MAX_PENDING;
}
static CFDataRef configuration(CMVideoFormatDescriptionRef format, struct x9_hevc_config *c)
{
    if (!format || CMFormatDescriptionGetMediaType(format) != kCMMediaType_Video ||
        CMFormatDescriptionGetMediaSubType(format) != kCMVideoCodecType_HEVC) return NULL;
    CFTypeRef atoms = CMFormatDescriptionGetExtension(format,
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (!atoms || CFGetTypeID(atoms) != CFDictionaryGetTypeID()) return NULL;
    CFTypeRef data = CFDictionaryGetValue(atoms, CFSTR("hvcC"));
    if (!data || CFGetTypeID(data) != CFDataGetTypeID() ||
        x9_hevc_config_parse(CFDataGetBytePtr(data), (size_t)CFDataGetLength(data), c)) return NULL;
    CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(format);
    if (d.width != (int32_t)c->width || d.height != (int32_t)c->height) return NULL;
    return data;
}
static void video_cancel(struct state *s)
{
    s->invalid = true; s->session = NULL;
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->native_session = 0;
    for (unsigned i = 0; i < s->pending_count; i++) CFRelease(s->pending[i].description);
    s->pending_count = 0;
    if (s->hvcc) { CFRelease(s->hvcc); s->hvcc = NULL; }
}
static OSStatus invalidate(Base object)
{
    struct state *s = storage(object);
    if (!s->lock_initialized) return noErr;
    pthread_mutex_lock(&s->lock);
    video_cancel(s);
    pthread_mutex_unlock(&s->lock);
    return noErr;
}
static void finalize(Base object)
{
    struct state *s = storage(object);
    (void)invalidate(object);
    if (s->lock_initialized) { pthread_mutex_destroy(&s->lock); s->lock_initialized = false; }
}
static CFStringRef debug_description(Base object)
{
    (void)object;
    return CFRetain(CFSTR("X9 VCP HEVC native-session decoder (hardware gated)"));
}
static OSStatus copy_property(Base object, CFStringRef key, CFAllocatorRef allocator, void *out)
{
    (void)allocator;
    if (!key || !out) return kVTParameterErr;
    *(CFTypeRef *)out = NULL;
    if (!CFEqual(key, using_hardware_key))
        return kVTPropertyNotSupportedErr;
    struct state *s = storage(object);
    struct x9_vdec_status status;
    struct x9_vdec_capabilities caps;
    pthread_mutex_lock(&s->lock);
    Boolean ready = !s->invalid && s->fd >= 0 && hardware_status(s->fd, &status) &&
                    status.generation == s->generation && hardware_sessions(s->fd, &status, &caps);
    *(CFTypeRef *)out = CFRetain(ready ? kCFBooleanTrue : kCFBooleanFalse);
    pthread_mutex_unlock(&s->lock);
    return noErr;
}
static OSStatus set_property(Base object, CFStringRef key, CFTypeRef value)
{ (void)object; (void)key; (void)value; return kVTPropertyNotSupportedErr; }
static OSStatus supported(Base object, CFDictionaryRef *out)
{
    (void)object;
    if (!out) return kVTParameterErr;
    const void *keys[] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey };
    const void *values[] = { kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly };
    CFDictionaryRef value = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!value) return kVTAllocationFailedErr;
    const void *key = using_hardware_key;
    const void *v = value;
    *out = CFDictionaryCreate(NULL, &key, &v, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(value);
    return *out ? noErr : kVTAllocationFailedErr;
}
static OSStatus start(Base object, void *session, CMVideoFormatDescriptionRef format)
{
    struct state *s = storage(object);
    struct x9_hevc_config c;
    CFDataRef data = configuration(format, &c);
    if (!session || !data) return kVTVideoDecoderUnsupportedDataFormatErr;
    pthread_mutex_lock(&s->lock);
    OSStatus e = kVTVideoDecoderNotAvailableNowErr;
    struct x9_vdec_status status;
    struct x9_vdec_capabilities caps;
    int fd = -1;
    if (s->invalid || s->session) { e = kVTInvalidSessionErr; goto done; }
    fd = open(X9_VDEC_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0 || !hardware_status(fd, &status) || !hardware_sessions(fd, &status, &caps)) goto done;
    if (!(status.profiles & (1u << c.decoder_profile)) || c.coded_width > status.max_width ||
        c.coded_height > status.max_height || c.max_dpb > caps.max_pending) {
        e = kVTVideoDecoderUnsupportedDataFormatErr; goto done;
    }
    /* NV12/P010, full/video range: actual range is reported with each frame.
     * VT may convert to the consumer's preferred layout. No native tiles. */
    int32_t formats[] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
        kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
        kCVPixelFormatType_420YpCbCr10BiPlanarFullRange };
    CFNumberRef nums[4] = {0};
    for (unsigned i = 0; i < 4; i++) nums[i] = CFNumberCreate(NULL, kCFNumberSInt32Type, &formats[i]);
    if (!nums[0] || !nums[1] || !nums[2] || !nums[3]) {
        for (unsigned i = 0; i < 4; i++) if (nums[i]) CFRelease(nums[i]);
        e = kVTAllocationFailedErr; goto done;
    }
    CFArrayRef list = CFArrayCreate(NULL, (const void **)nums + (c.bit_depth == 10 ? 2 : 0), 2,
                                   &kCFTypeArrayCallBacks);
    for (unsigned i = 0; i < 4; i++) CFRelease(nums[i]);
    if (!list) { e = kVTAllocationFailedErr; goto done; }
    const void *key = kCVPixelBufferPixelFormatTypeKey, *value = list;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(list);
    if (!attrs) { e = kVTAllocationFailedErr; goto done; }
    e = attributes(session, attrs); CFRelease(attrs);
    if (!e) {
        s->hvcc = CFDataCreateCopy(NULL, data);
        if (!s->hvcc) { e = kVTAllocationFailedErr; goto done; }
        struct x9_vdec_session native = { .version = X9_VDEC_VERSION,
            .generation = status.generation, .config_bytes = (uint32_t)CFDataGetLength(s->hvcc),
            .config_address = (uint64_t)(uintptr_t)CFDataGetBytePtr(s->hvcc) };
        if (ioctl(fd, X9_VDEC_SESSION_CREATE, &native) || !native.session) {
            CFRelease(s->hvcc); s->hvcc = NULL;
            e = kVTVideoDecoderNotAvailableNowErr; goto done;
        }
        s->fd = fd; fd = -1; s->session = session; s->config = c;
        s->generation = status.generation; s->native_session = native.session;
        s->max_pending = caps.max_pending;
    }
done:
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&s->lock);
    return e;
}
static Boolean accept(Base object, CMVideoFormatDescriptionRef format)
{
    struct state *s = storage(object);
    struct x9_hevc_config c;
    CFDataRef data = configuration(format, &c);
    pthread_mutex_lock(&s->lock);
    Boolean yes = !s->invalid && s->fd >= 0 && s->native_session &&
                  s->hvcc && data && CFEqual(data, s->hvcc);
    pthread_mutex_unlock(&s->lock);
    return yes; /* changed sets require a new native codec/DPB, not a host reset */
}
static OSStatus finish(Base object);
static OSStatus pixel_buffer(const struct x9_vdec_picture *r, const uint8_t *raw,
                            CMVideoFormatDescriptionRef description, CVPixelBufferRef *out)
{
    OSType format = r->format == X9_VDEC_P010 ?
        (r->flags & X9_VDEC_FULL_RANGE ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange :
                                       kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange) :
        (r->flags & X9_VDEC_FULL_RANGE ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange :
                                       kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!empty) return kVTAllocationFailedErr;
    const void *key = kCVPixelBufferIOSurfacePropertiesKey, *value = empty;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(empty);
    if (!attrs) return kVTAllocationFailedErr;
    OSStatus e = CVPixelBufferCreate(NULL, r->width, r->height, format, attrs, out);
    CFRelease(attrs);
    if (e) return e;
    e = CVPixelBufferLockBaseAddress(*out, 0);
    if (e) { CVPixelBufferRelease(*out); *out = NULL; return e; }
    size_t row = r->width * (r->format == X9_VDEC_P010 ? 2 : 1), offset = 0;
    if (CVPixelBufferGetPlaneCount(*out) != 2 || !CVPixelBufferGetIOSurface(*out)) e = kVTVideoDecoderBadDataErr;
    for (unsigned plane = 0; plane < 2 && !e; plane++) {
        uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(*out, plane);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(*out, plane), rows = r->height >> plane;
        if (!dst || stride < row || CVPixelBufferGetHeightOfPlane(*out, plane) < rows) {
            e = kVTVideoDecoderBadDataErr; break;
        }
        for (size_t y = 0; y < rows; y++) {
            memcpy(dst + y * stride, raw + offset, row);
            memset(dst + y * stride + row, 0, stride - row); offset += row;
        }
    }
    OSStatus unlock = CVPixelBufferUnlockBaseAddress(*out, 0);
    if (!e) e = unlock;
    if (!e) e = x9_hevc_attach_image_metadata(description, *out);
    if (e) { CVPixelBufferRelease(*out); *out = NULL; }
    return e;
}
#include "vt_video_sessions.inc"
static const struct base_class base = {
    .version = 1, .storage_size = sizeof(struct state), .invalidate = invalidate,
    .finalize = finalize, .debug = debug_description,
    .copy_property = copy_property, .set_property = set_property
};
static const struct decoder_class decoder = {
    .version = 2, .start = start, .decode = decode_frame, .supported = supported,
    .accept = accept, .finish = finish
};
static const struct vtable table = { .base = &base, .decoder = &decoder };
static OSStatus factory(void *identifier, CFAllocatorRef allocator, Base *out)
{
    (void)identifier;
    if (!out) return kVTParameterErr;
    *out = NULL;
    OSStatus e = create_object(allocator, &table, class_id(), out);
    if (e) return e;
    struct state *s = storage(*out);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    /* Emit may synchronously call invalidate on this same thread. Recursive
     * serialization permits that, while preventing another thread from
     * canceling a VT frame between its ledger retirement and emission. */
    pthread_mutexattr_t attr;
    int err = pthread_mutexattr_init(&attr);
    if (!err) {
        err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        if (!err) err = pthread_mutex_init(&s->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    if (err) {
        CFRelease(*out); *out = NULL; return kVTAllocationFailedErr;
    }
    s->lock_initialized = true;
    return noErr;
}
/* Explicit opt-in only. No dlopen constructor and no Apple library patch.
 * Registration affects the calling process, not remote mediaserverd.
 * Ordinary VT sessions are excluded by RequiredSpecificationEntries. */
__attribute__((visibility("default"))) OSStatus X9HEVCRegisterDecoder(void)
{
    pthread_mutex_lock(&registration_lock);
    OSStatus e = kVTVideoDecoderNotAvailableNowErr;
    int fd = open(X9_VDEC_DEVICE, O_RDONLY | O_CLOEXEC);
    struct x9_vdec_status status;
    struct x9_vdec_capabilities caps;
    if (fd < 0 || !hardware_status(fd, &status) || !hardware_sessions(fd, &status, &caps)) goto done;
    if (registered) { e = noErr; goto done; }
    create_object = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
    storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
    class_id = dlsym(RTLD_DEFAULT, "VTVideoDecoderGetClassID");
    emit = dlsym(RTLD_DEFAULT, "VTDecoderSessionEmitDecodedFrame");
    attributes = dlsym(RTLD_DEFAULT, "VTDecoderSessionSetPixelBufferAttributes");
    register_decoder = dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
    hardware_key = symbol_key("kVTVideoDecoder_IsHardwareAccelerated");
    required_key = symbol_key("kVTVideoDecoderInfo_RequiredSpecificationEntries");
    /* Present in the observed 20H392 image, although newer SDKs annotate
     * the public iOS declaration as 17+. Resolve the actual runtime symbol. */
    using_hardware_key = symbol_key("kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder");
    if (!create_object || !storage || !class_id || !emit || !attributes ||
        !register_decoder || !hardware_key || !required_key || !using_hardware_key) {
        e = kVTVideoDecoderUnsupportedDataFormatErr; goto done;
    }
    const void *key = CFSTR("X9NativeHEVC"), *value = kCFBooleanTrue;
    CFDictionaryRef requirement = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!requirement) { e = kVTAllocationFailedErr; goto done; }
    const void *keys[] = { hardware_key, required_key, CFSTR("CMClassImplementationID") };
    const void *values[] = { kCFBooleanTrue, requirement, CFSTR(X9_HEVC_IMPLEMENTATION) };
    CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(requirement);
    if (!info) { e = kVTAllocationFailedErr; goto done; }
    e = register_decoder(kCMVideoCodecType_HEVC, info, (void *)factory);
    CFRelease(info);
    if (!e) registered = true;
done:
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&registration_lock);
    return e;
}
