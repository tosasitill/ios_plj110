#include "vt_image_metadata.h"
#include <VideoToolbox/VideoToolbox.h>
#include <math.h>
#include <string.h>

OSStatus x9_hevc_read_hdr_description(CMVideoFormatDescriptionRef source, struct x9_hevc_hdr *out)
{
    if (!out) return kVTParameterErr;
    memset(out,0,sizeof(*out));
    if (!source || CMFormatDescriptionGetMediaType(source) != kCMMediaType_Video)
        return kVTParameterErr;
    struct x9_hevc_hdr parsed = {0};
    CFStringRef keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo };
    uint8_t *bytes[] = { parsed.mastering, parsed.light };
    uint32_t *present[] = { &parsed.mastering_present, &parsed.light_present };
    const CFIndex lengths[] = {24,4};
    for (unsigned i = 0; i < 2; i++) {
        CFTypeRef value = CMFormatDescriptionGetExtension(source,keys[i]);
        if (!value) continue;
        if (CFGetTypeID(value) != CFDataGetTypeID() || CFDataGetLength(value) != lengths[i])
            return kVTVideoDecoderBadDataErr;
        memcpy(bytes[i],CFDataGetBytePtr(value),(size_t)lengths[i]); *present[i] = 1;
    }
    *out = parsed; return noErr;
}

OSStatus x9_hevc_copy_hdr_description(CMVideoFormatDescriptionRef source,
    const struct x9_hevc_hdr *hdr, CMVideoFormatDescriptionRef *out)
{
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (!source || !hdr || CMFormatDescriptionGetMediaType(source) != kCMMediaType_Video ||
        hdr->mastering_present > 1 || hdr->light_present > 1) return kVTParameterErr;
    struct x9_hevc_hdr original_hdr;
    OSStatus e = x9_hevc_read_hdr_description(source,&original_hdr);
    if (e) return e;
    CFStringRef keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo };
    const uint8_t *bytes[] = { hdr->mastering, hdr->light };
    const CFIndex lengths[] = {24,4};
    const unsigned present[] = { hdr->mastering_present, hdr->light_present };
    const unsigned original_present[] = { original_hdr.mastering_present, original_hdr.light_present };
    const uint8_t *original_bytes[] = { original_hdr.mastering, original_hdr.light };
    for (unsigned i = 0; i < 2; i++) {
        if (original_present[i] && present[i] &&
            memcmp(original_bytes[i],bytes[i],(size_t)lengths[i]))
            return kVTVideoDecoderBadDataErr;
    }
    CFDictionaryRef original = CMFormatDescriptionGetExtensions(source);
    CFMutableDictionaryRef extensions = original ? CFDictionaryCreateMutableCopy(NULL,0,original) :
        CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!extensions) return kVTAllocationFailedErr;
    e = noErr;
    for (unsigned i = 0; i < 2; i++) {
        if (!present[i]) continue;
        CFDataRef value = CFDataCreate(NULL,bytes[i],lengths[i]);
        if (!value) { e = kVTAllocationFailedErr; break; }
        CFDictionarySetValue(extensions,keys[i],value); CFRelease(value);
    }
    if (!e) {
        CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(source);
        e = CMVideoFormatDescriptionCreate(NULL,CMFormatDescriptionGetMediaSubType(source),
            size.width,size.height,extensions,out);
        if (!e && !*out) e = kVTAllocationFailedErr;
    }
    CFRelease(extensions);
    if (e && *out) { CFRelease(*out); *out = NULL; }
    return e;
}

struct image_attachment {
    CFStringRef source, destination;
    CFTypeID type;
};

/* ICC and nclx-derived strings are both retained. Selection/precedence belongs
 * to the consumer's ColorSync path, not this decoder. Format-description keys
 * are mapped explicitly, including those that are not SDK aliases of CV keys.
 * Validate all present values before changing any attachment on the buffer. */
OSStatus x9_hevc_attach_image_metadata(CMVideoFormatDescriptionRef format,
                                     CVPixelBufferRef pixel)
{
    if (!format || !pixel ||
        CMFormatDescriptionGetMediaType(format) != kCMMediaType_Video)
        return kVTParameterErr;
    const struct image_attachment entries[] = {
        { kCMFormatDescriptionExtension_ICCProfile,
          kCVImageBufferICCProfileKey, CFDataGetTypeID() },
        { kCMFormatDescriptionExtension_ColorPrimaries,
          kCVImageBufferColorPrimariesKey, CFStringGetTypeID() },
        { kCMFormatDescriptionExtension_TransferFunction,
          kCVImageBufferTransferFunctionKey, CFStringGetTypeID() },
        { kCMFormatDescriptionExtension_YCbCrMatrix,
          kCVImageBufferYCbCrMatrixKey, CFStringGetTypeID() },
        { kCMFormatDescriptionExtension_GammaLevel,
          kCVImageBufferGammaLevelKey, CFNumberGetTypeID() },
        { kCMFormatDescriptionExtension_ChromaLocationTopField,
          kCVImageBufferChromaLocationTopFieldKey, CFStringGetTypeID() },
        { kCMFormatDescriptionExtension_ChromaLocationBottomField,
          kCVImageBufferChromaLocationBottomFieldKey, CFStringGetTypeID() },
        { kCMFormatDescriptionExtension_PixelAspectRatio,
          kCVImageBufferPixelAspectRatioKey, CFDictionaryGetTypeID() },
        { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
          kCVImageBufferMasteringDisplayColorVolumeKey, CFDataGetTypeID() },
        { kCMFormatDescriptionExtension_ContentLightLevelInfo,
          kCVImageBufferContentLightLevelInfoKey, CFDataGetTypeID() },
    };
    const size_t count = sizeof(entries) / sizeof(entries[0]);
    CFTypeRef values[sizeof(entries) / sizeof(entries[0])] = {0};
    for (size_t i = 0; i < count; i++) {
        values[i] = CMFormatDescriptionGetExtension(format, entries[i].source);
        if (!values[i]) continue;
        if (CFGetTypeID(values[i]) != entries[i].type)
            return kVTVideoDecoderBadDataErr;
        if (entries[i].type == CFDataGetTypeID() &&
            CFDataGetLength(values[i]) <= 0)
            return kVTVideoDecoderBadDataErr;
        if ((CFEqual(entries[i].source,kCMFormatDescriptionExtension_MasteringDisplayColorVolume) &&
                CFDataGetLength(values[i]) != 24) ||
            (CFEqual(entries[i].source,kCMFormatDescriptionExtension_ContentLightLevelInfo) &&
                CFDataGetLength(values[i]) != 4))
            return kVTVideoDecoderBadDataErr;
        if (CFEqual(entries[i].source,
                    kCMFormatDescriptionExtension_PixelAspectRatio)) {
            CFDictionaryRef aspect = values[i];
            CFTypeRef horizontal = CFDictionaryGetValue(aspect,
                kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing);
            CFTypeRef vertical = CFDictionaryGetValue(aspect,
                kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing);
            int64_t h = 0, v = 0;
            if (!horizontal || !vertical ||
                CFGetTypeID(horizontal) != CFNumberGetTypeID() ||
                CFGetTypeID(vertical) != CFNumberGetTypeID() ||
                !CFNumberGetValue(horizontal, kCFNumberSInt64Type, &h) ||
                !CFNumberGetValue(vertical, kCFNumberSInt64Type, &v) ||
                h <= 0 || v <= 0 || h > INT32_MAX || v > INT32_MAX)
                return kVTVideoDecoderBadDataErr;
        }
        if (entries[i].type == CFNumberGetTypeID()) {
            double gamma = 0;
            if (!CFNumberGetValue(values[i], kCFNumberDoubleType, &gamma) ||
                !isfinite(gamma) || gamma <= 0)
                return kVTVideoDecoderBadDataErr;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (values[i]) CVBufferSetAttachment(pixel, entries[i].destination,
            values[i], kCVAttachmentMode_ShouldPropagate);
        else if (CFEqual(entries[i].source,kCMFormatDescriptionExtension_MasteringDisplayColorVolume) ||
            CFEqual(entries[i].source,kCMFormatDescriptionExtension_ContentLightLevelInfo))
            CVBufferRemoveAttachment(pixel,entries[i].destination);
    }
    return noErr;
}
OSStatus x9_hevc_validate_hdr_metadata(CMVideoFormatDescriptionRef format, CVPixelBufferRef pixel)
{
    if (!format || !pixel || CMFormatDescriptionGetMediaType(format) != kCMMediaType_Video)
        return kVTParameterErr;
    CFStringRef cm[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo };
    CFStringRef cv[] = { kCVImageBufferMasteringDisplayColorVolumeKey,
        kCVImageBufferContentLightLevelInfoKey };
    const CFIndex lengths[] = {24,4};
    for (unsigned n = 0; n < 2; n++) {
        CFTypeRef expected = CMFormatDescriptionGetExtension(format,cm[n]);
        CFTypeRef actual = CVBufferCopyAttachment(pixel,cv[n],NULL);
        OSStatus e = noErr;
        if (expected || actual) {
            if (!expected || !actual || CFGetTypeID(expected) != CFDataGetTypeID() ||
                CFGetTypeID(actual) != CFDataGetTypeID() || CFDataGetLength(expected) != lengths[n] ||
                CFDataGetLength(actual) != lengths[n] || !CFEqual(expected,actual))
                e = kVTVideoDecoderBadDataErr;
        }
        if (actual) CFRelease(actual);
        if (e) return e;
    }
    return noErr;
}
