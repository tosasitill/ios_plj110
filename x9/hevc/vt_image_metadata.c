#include "vt_image_metadata.h"
#include <VideoToolbox/VideoToolbox.h>
#include <math.h>

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
    }
    return noErr;
}
