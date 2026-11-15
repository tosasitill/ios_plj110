/* Own-code VideoToolbox decoder for independent HEVC IDR pictures.
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
#include "hevc_bitstream.h"
#include "vt_decoder.h"
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
struct state {
    pthread_mutex_t lock;
    Boolean lock_initialized, invalid;
    int fd;
    void *session;
    uint64_t generation;
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
static OSStatus invalidate(Base object)
{
    struct state *s = storage(object);
    if (!s->lock_initialized) return noErr;
    pthread_mutex_lock(&s->lock);
    s->invalid = true; s->session = NULL;
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    if (s->hvcc) { CFRelease(s->hvcc); s->hvcc = NULL; }
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
    return CFRetain(CFSTR("X9 VCP HEVC independent-picture decoder (hardware gated)"));
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
    pthread_mutex_lock(&s->lock);
    Boolean ready = !s->invalid && s->fd >= 0 && hardware_status(s->fd, &status) &&
                    status.generation == s->generation;
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
    int fd = -1;
    if (s->invalid || s->session) { e = kVTInvalidSessionErr; goto done; }
    fd = open(X9_VDEC_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0 || !hardware_status(fd, &status)) goto done;
    if (!(status.profiles & (1u << c.decoder_profile)) || c.coded_width > status.max_width ||
        c.coded_height > status.max_height) { e = kVTVideoDecoderUnsupportedDataFormatErr; goto done; }
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
        s->fd = fd; fd = -1; s->session = session; s->config = c;
        s->generation = status.generation;
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
    Boolean yes = !s->invalid && s->hvcc && data && CFEqual(data, s->hvcc);
    pthread_mutex_unlock(&s->lock);
    return yes; /* parameter-set changes create a fresh independent session */
}
static OSStatus finish(Base object)
{
    struct state *s = storage(object);
    pthread_mutex_lock(&s->lock);
    OSStatus e = s->invalid ? kVTInvalidSessionErr : noErr;
    pthread_mutex_unlock(&s->lock);
    return e; /* no deferred frames or retained DPB */
}
static OSStatus pixel_buffer(const struct x9_vdec_picture *r, const uint8_t *raw, CVPixelBufferRef *out)
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
    if (e) { CVPixelBufferRelease(*out); *out = NULL; }
    return e;
}
static OSStatus decode_frame(Base object, void *frame, CMSampleBufferRef sample,
                            VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    if (info) *info = 0; /* fully synchronous; never claim async completion */
    if (!sample || !frame || CMSampleBufferGetNumSamples(sample) != 1 ||
        !CMSampleBufferDataIsReady(sample) || (flags & kVTDecodeFrame_EnableTemporalProcessing))
        return kVTVideoDecoderUnsupportedDataFormatErr;
    struct state *s = storage(object);
    CVPixelBufferRef pixel = NULL;
    uint8_t *input = NULL, *raw = NULL;
    void *session = NULL;
    OSStatus e = kVTVideoDecoderBadDataErr;
    pthread_mutex_lock(&s->lock);
    if (s->invalid || s->fd < 0 || !s->hvcc) { e = kVTInvalidSessionErr; goto done; }
    struct x9_hevc_config config;
    CFDataRef data = configuration(CMSampleBufferGetFormatDescription(sample), &config);
    if (!data || !CFEqual(data, s->hvcc)) { e = kVTVideoDecoderUnsupportedDataFormatErr; goto done; }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t bytes = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!bytes || bytes > X9_HEVC_MAX_INPUT) goto done;
    size_t output_bytes = (size_t)s->config.width * s->config.height * 3 /
                          (s->config.bit_depth == 10 ? 1u : 2u);
    input = malloc(bytes); raw = malloc(output_bytes);
    if (!input || !raw) { e = kVTAllocationFailedErr; goto done; }
    e = CMBlockBufferCopyDataBytes(block, 0, bytes, input);
    if (e) goto done;
    size_t needed = 0;
    if (x9_hevc_idr_annexb(&s->config, CFDataGetBytePtr(s->hvcc), input, bytes, NULL, 0, &needed)) {
        e = kVTVideoDecoderUnsupportedDataFormatErr; goto done;
    }
    struct x9_vdec_picture r = {
        .version = X9_VDEC_VERSION, .generation = s->generation,
        .config_address = (uint64_t)(uintptr_t)CFDataGetBytePtr(s->hvcc),
        .input_address = (uint64_t)(uintptr_t)input, .output_address = (uint64_t)(uintptr_t)raw,
        .config_bytes = (uint32_t)CFDataGetLength(s->hvcc),
        .input_bytes = (uint32_t)bytes, .output_capacity = (uint32_t)output_bytes
    };
    if (ioctl(s->fd, X9_VDEC_DECODE_IDR, &r)) { e = kVTVideoDecoderNotAvailableNowErr; goto done; }
    if (!r.completion || r.output_bytes != output_bytes || r.width != s->config.width ||
        r.height != s->config.height || r.format != (s->config.bit_depth == 10 ? X9_VDEC_P010 : X9_VDEC_NV12) ||
        (r.flags & ~X9_VDEC_FULL_RANGE)) { e = kVTVideoDecoderBadDataErr; goto done; }
    e = pixel_buffer(&r, raw, &pixel);
    if (!e) session = s->session;
done:
    free(input); free(raw);
    pthread_mutex_unlock(&s->lock);
    /* On errors return directly: VT's duct performs the error/drop emission.
     * On success emit exactly once using VT's frame token (not sample/PTS).
     * Emit outside our lock so synchronous client callbacks may invalidate. */
    if (!e) e = emit(session, frame, noErr, 0, pixel);
    if (pixel) CVPixelBufferRelease(pixel);
    return e;
}
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
    s->fd = -1;
    if (pthread_mutex_init(&s->lock, NULL)) {
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
    if (fd < 0 || !hardware_status(fd, &status)) goto done;
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
    const void *key = CFSTR("X9IndependentHEVC"), *value = kCFBooleanTrue;
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
