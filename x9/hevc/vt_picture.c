/* One independently decodable picture through the specified native VT codec.
 * This is shared by the raw-picture client and the HEIF raster backend. */
#include "vt_decoder.h"
#include "hevc_bitstream.h"
#include "vt_image_metadata.h"
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
struct picture_result {
    pthread_mutex_t lock;
    unsigned calls;
    OSStatus error;
    CVPixelBufferRef pixel;
};
static CFStringRef symbol(const char *name)
{ CFStringRef *p = dlsym(RTLD_DEFAULT,name); return p ? *p : NULL; }
static void picture_output(void *context, void *source, OSStatus error,
    VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts, CMTime duration)
{
    (void)pts; (void)duration;
    struct picture_result *r = context;
    pthread_mutex_lock(&r->lock);
    r->calls++;
    if (!error && (source != context || r->calls != 1 || !image ||
        CFGetTypeID(image) != CVPixelBufferGetTypeID() || (flags & kVTDecodeInfo_FrameDropped)))
        error = kVTVideoDecoderBadDataErr;
    if (error && !r->error) r->error = error;
    if (!r->error && !r->pixel) r->pixel = CVPixelBufferRetain(image);
    pthread_mutex_unlock(&r->lock);
}
static OSStatus using_hardware(VTDecompressionSessionRef session, CFStringRef key)
{
    CFTypeRef value = NULL;
    OSStatus e = VTSessionCopyProperty(session,key,NULL,&value);
    if (!e && (!value || CFGetTypeID(value) != CFBooleanGetTypeID() || !CFBooleanGetValue(value)))
        e = kVTVideoDecoderNotAvailableNowErr;
    if (value) CFRelease(value);
    return e;
}
OSStatus X9HEVCCopyHardwarePicture(CMSampleBufferRef sample, CVPixelBufferRef *out)
{
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (!sample || CFGetTypeID(sample) != CMSampleBufferGetTypeID() ||
        !CMSampleBufferDataIsReady(sample) || CMSampleBufferGetNumSamples(sample) != 1) return kVTParameterErr;
    CMVideoFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    if (!format || CMFormatDescriptionGetMediaType(format) != kCMMediaType_Video ||
        CMFormatDescriptionGetMediaSubType(format) != kCMVideoCodecType_HEVC) return kVTParameterErr;
    CFTypeRef atoms = CMFormatDescriptionGetExtension(format,kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    CFTypeRef hvcc = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ?
        CFDictionaryGetValue(atoms,CFSTR("hvcC")) : NULL;
    if (!hvcc || CFGetTypeID(hvcc) != CFDataGetTypeID() || CFDataGetLength(hvcc) <= 0 ||
        CFDataGetLength(hvcc) > X9_HEVC_MAX_CONFIG) return kVTVideoDecoderBadDataErr;
    struct x9_hevc_config config;
    if (x9_hevc_config_parse(CFDataGetBytePtr(hvcc),(size_t)CFDataGetLength(hvcc),&config))
        return kVTVideoDecoderUnsupportedDataFormatErr;
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    if (dimensions.width != (int32_t)config.width || dimensions.height != (int32_t)config.height)
        return kVTVideoDecoderBadDataErr;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t bytes = block ? CMBlockBufferGetDataLength(block) : 0, needed = 0;
    if (!bytes || bytes > X9_HEVC_MAX_INPUT) return kVTVideoDecoderBadDataErr;
    uint8_t *au = malloc(bytes);
    if (!au) return kVTAllocationFailedErr;
    OSStatus e = CMBlockBufferCopyDataBytes(block,0,bytes,au);
    if (!e && x9_hevc_irap_annexb(&config,CFDataGetBytePtr(hvcc),au,bytes,NULL,0,&needed,NULL))
        e = kVTVideoDecoderUnsupportedDataFormatErr;
    free(au);
    if (e) return e;
    e = X9HEVCRegisterDecoder();
    if (e) return e; /* no Apple/software/other hardware fallback */
    CFStringRef enable = symbol("kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder");
    CFStringRef require = symbol("kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder");
    CFStringRef decoder = symbol("kVTVideoDecoderSpecification_DecoderID");
    CFStringRef hardware = symbol("kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder");
    if (!enable || !require || !decoder || !hardware) return kVTPropertyNotSupportedErr;
    const void *keys[] = { CFSTR("X9NativeHEVC"),enable,require,decoder };
    const void *values[] = { kCFBooleanTrue,kCFBooleanTrue,kCFBooleanTrue,CFSTR(X9_HEVC_DECODER_ID) };
    CFDictionaryRef spec = CFDictionaryCreate(NULL,keys,values,4,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!spec) return kVTAllocationFailedErr;
    struct picture_result r = {0};
    if (pthread_mutex_init(&r.lock,NULL)) { CFRelease(spec); return kVTAllocationFailedErr; }
    VTDecompressionSessionRef session = NULL;
    VTDecompressionOutputCallbackRecord callback = { picture_output,&r };
    e = VTDecompressionSessionCreate(NULL,format,spec,NULL,&callback,&session);
    if (!e) e = using_hardware(session,hardware);
    if (!e) {
        /* FRESH native session, not the legacy independent-IDR ioctl. With
         * temporal processing clear, our decoder submits and DRAINs/RECEIVEs
         * through ENODATA before returning, including a standalone BLA/CRA
         * that requires output bumping. Still wait/teardown on all paths;
         * source identity and exactly-one callback errors remain sticky. */
        VTDecodeInfoFlags flags = 0;
        e = VTDecompressionSessionDecodeFrame(session,sample,0,&r,&flags);
        OSStatus wait = VTDecompressionSessionWaitForAsynchronousFrames(session);
        if (!e) e = wait;
        if (!e && (flags & kVTDecodeInfo_FrameDropped)) e = kVTVideoDecoderBadDataErr;
        if (!e) e = using_hardware(session,hardware);
    }
    if (session) { VTDecompressionSessionInvalidate(session); CFRelease(session); }
    CFRelease(spec);
    if (!e && (r.error || r.calls != 1 || !r.pixel)) e = r.error ? r.error : kVTVideoDecoderBadDataErr;
    if (!e) {
        OSType f = CVPixelBufferGetPixelFormatType(r.pixel);
        unsigned depth = (f == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
            f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange) ? 10 :
            (f == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
            f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ? 8 : 0;
        if (depth != config.bit_depth || !CVPixelBufferIsPlanar(r.pixel) ||
            CVPixelBufferGetPlaneCount(r.pixel) != 2 || CVPixelBufferGetWidth(r.pixel) != config.width ||
            CVPixelBufferGetHeight(r.pixel) != config.height || (config.width & 1u) || (config.height & 1u))
            e = kVTVideoDecoderBadDataErr;
        for (unsigned p = 0; p < 2 && !e; p++)
            if (CVPixelBufferGetWidthOfPlane(r.pixel,p) != (config.width >> p) ||
                CVPixelBufferGetHeightOfPlane(r.pixel,p) != (config.height >> p) ||
                CVPixelBufferGetBytesPerRowOfPlane(r.pixel,p) < (size_t)config.width*(depth == 10 ? 2u : 1u))
                e = kVTVideoDecoderBadDataErr;
    }
    if (!e) e = x9_hevc_validate_hdr_metadata(format,r.pixel);
    if (!e) { *out = r.pixel; r.pixel = NULL; }
    if (r.pixel) CVPixelBufferRelease(r.pixel);
    pthread_mutex_destroy(&r.lock);
    return e;
}
