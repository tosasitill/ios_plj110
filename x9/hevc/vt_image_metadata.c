#include "vt_image_metadata.h"
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <math.h>
#include <string.h>

static CFStringRef preferred_string(unsigned code)
{
    if (code == 2) return NULL;
    if (code == 4 || code == 5) return kCVImageBufferTransferFunction_UseGamma;
    return CVTransferFunctionGetStringForIntegerCodePoint((int)code);
}
static int preferred_read(CFDictionaryRef d, struct x9_hevc_hdr *hdr)
{
    CFTypeRef raw = d ? CFDictionaryGetValue(d,CFSTR("X9PreferredTransferCodePoint")) : NULL;
    CFTypeRef label = d ? CFDictionaryGetValue(d,kCMFormatDescriptionExtension_AlternativeTransferCharacteristics) : NULL;
    if (label && CFGetTypeID(label) != CFStringGetTypeID()) return EBADMSG;
    if (!raw) return 0; /* Never invert an alias string into bitstream provenance. */
    int64_t code = 0;
    if (CFGetTypeID(raw) != CFNumberGetTypeID() || CFNumberIsFloatType(raw) ||
        !CFNumberGetValue(raw,kCFNumberSInt64Type,&code) || code < 0 || code > 255) return EBADMSG;
    CFStringRef mapped = preferred_string((unsigned)code);
    if (label && (!mapped || !CFEqual(label,mapped))) return EBADMSG;
    hdr->preferred_transfer = (uint32_t)code; hdr->preferred_present = 1;
    return 0;
}
int x9_hevc_merge_preferred_transfer(CFMutableDictionaryRef d, const struct x9_hevc_hdr *hdr)
{
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID() || x9_hevc_hdr_validate(hdr)) return EINVAL;
    struct x9_hevc_hdr original = {0};
    int e = preferred_read(d,&original);
    if (e) return e;
    if (hdr->preferred_present && original.preferred_present &&
        hdr->preferred_transfer != original.preferred_transfer) return EBADMSG;
    if (!hdr->preferred_present && !original.preferred_present) return 0;
    unsigned code = hdr->preferred_present ? hdr->preferred_transfer : original.preferred_transfer;
    CFStringRef mapped = preferred_string(code);
    CFTypeRef label = CFDictionaryGetValue(d,kCMFormatDescriptionExtension_AlternativeTransferCharacteristics);
    if (label && (!mapped || !CFEqual(label,mapped))) return EBADMSG;
    int32_t value = (int32_t)code;
    CFNumberRef number = CFNumberCreate(NULL,kCFNumberSInt32Type,&value);
    if (!number) return ENOMEM;
    /* No mutation until conflicts and the one required allocation succeed. */
    CFDictionarySetValue(d,CFSTR("X9PreferredTransferCodePoint"),number);
    if (mapped) CFDictionarySetValue(d,kCMFormatDescriptionExtension_AlternativeTransferCharacteristics,mapped);
    CFRelease(number); return 0;
}
int x9_hevc_copy_transfer_extensions(CFDictionaryRef source, CFDictionaryRef *out)
{
    if (!out) return EINVAL;
    *out = NULL;
    if (source && CFGetTypeID(source) != CFDictionaryGetTypeID()) return EINVAL;
    struct x9_hevc_hdr hdr = {0};
    int e = preferred_read(source,&hdr);
    if (e) return e;
    CFTypeRef alternative = source ? CFDictionaryGetValue(source,kCMFormatDescriptionExtension_AlternativeTransferCharacteristics) : NULL;
    CFTypeRef transfer = hdr.preferred_present ? preferred_string(hdr.preferred_transfer) : alternative;
    if (!hdr.preferred_present && !alternative && source)
        transfer = CFDictionaryGetValue(source,kCMFormatDescriptionExtension_TransferFunction);
    if (transfer && CFGetTypeID(transfer) != CFStringGetTypeID()) return EBADMSG;
    CFTypeRef gamma = source ? CFDictionaryGetValue(source,kCMFormatDescriptionExtension_GammaLevel) : NULL;
    CFNumberRef new_gamma = NULL;
    if (hdr.preferred_present && (hdr.preferred_transfer == 4 || hdr.preferred_transfer == 5)) {
        double value = hdr.preferred_transfer == 4 ? 2.2 : 2.8;
        new_gamma = CFNumberCreate(NULL,kCFNumberDoubleType,&value);
        if (!new_gamma) return ENOMEM;
        gamma = new_gamma;
    } else if (hdr.preferred_present || alternative) {
        if (!transfer || !CFEqual(transfer,kCVImageBufferTransferFunction_UseGamma)) gamma = NULL;
    }
    if (gamma) {
        double value = 0;
        if (CFGetTypeID(gamma) != CFNumberGetTypeID() ||
            !CFNumberGetValue(gamma,kCFNumberDoubleType,&value) || !isfinite(value) || value <= 0) e = EBADMSG;
    }
    if (!e && transfer && CFEqual(transfer,kCVImageBufferTransferFunction_UseGamma) && !gamma) e = EBADMSG;
    CFMutableDictionaryRef d = !e ? CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks) : NULL;
    if (!e && !d) e = ENOMEM;
    if (!e) {
        if (transfer) CFDictionarySetValue(d,kCMFormatDescriptionExtension_TransferFunction,transfer);
        if (gamma) CFDictionarySetValue(d,kCMFormatDescriptionExtension_GammaLevel,gamma);
        *out = d;
    }
    if (new_gamma) CFRelease(new_gamma);
    return e;
}

OSStatus x9_hevc_read_hdr_description(CMVideoFormatDescriptionRef source, struct x9_hevc_hdr *out)
{
    if (!out) return kVTParameterErr;
    memset(out,0,sizeof(*out));
    if (!source || CMFormatDescriptionGetMediaType(source) != kCMMediaType_Video)
        return kVTParameterErr;
    struct x9_hevc_hdr parsed = {0};
    CFStringRef keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo,
        kCMFormatDescriptionExtension_AmbientViewingEnvironment };
    uint8_t *bytes[] = { parsed.mastering, parsed.light, parsed.ambient };
    uint32_t *present[] = { &parsed.mastering_present, &parsed.light_present, &parsed.ambient_present };
    const CFIndex lengths[] = {24,4,8};
    for (unsigned i = 0; i < 3; i++) {
        CFTypeRef value = CMFormatDescriptionGetExtension(source,keys[i]);
        if (!value) continue;
        if (CFGetTypeID(value) != CFDataGetTypeID() || CFDataGetLength(value) != lengths[i])
            return kVTVideoDecoderBadDataErr;
        memcpy(bytes[i],CFDataGetBytePtr(value),(size_t)lengths[i]); *present[i] = 1;
    }
    if (preferred_read(CMFormatDescriptionGetExtensions(source),&parsed)) return kVTVideoDecoderBadDataErr;
    if (x9_hevc_hdr_validate(&parsed)) return kVTVideoDecoderBadDataErr;
    *out = parsed; return noErr;
}

OSStatus x9_hevc_copy_hdr_description(CMVideoFormatDescriptionRef source,
    const struct x9_hevc_hdr *hdr, CMVideoFormatDescriptionRef *out)
{
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (!source || !hdr || CMFormatDescriptionGetMediaType(source) != kCMMediaType_Video ||
        x9_hevc_hdr_validate(hdr)) return kVTParameterErr;
    struct x9_hevc_hdr original_hdr;
    OSStatus e = x9_hevc_read_hdr_description(source,&original_hdr);
    if (e) return e;
    CFStringRef keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo,
        kCMFormatDescriptionExtension_AmbientViewingEnvironment };
    const uint8_t *bytes[] = { hdr->mastering, hdr->light, hdr->ambient };
    const CFIndex lengths[] = {24,4,8};
    const unsigned present[] = { hdr->mastering_present, hdr->light_present, hdr->ambient_present };
    const unsigned original_present[] = { original_hdr.mastering_present, original_hdr.light_present,
        original_hdr.ambient_present };
    const uint8_t *original_bytes[] = { original_hdr.mastering, original_hdr.light, original_hdr.ambient };
    for (unsigned i = 0; i < 3; i++) {
        if (original_present[i] && present[i] &&
            memcmp(original_bytes[i],bytes[i],(size_t)lengths[i]))
            return kVTVideoDecoderBadDataErr;
    }
    CFDictionaryRef original = CMFormatDescriptionGetExtensions(source);
    CFMutableDictionaryRef extensions = original ? CFDictionaryCreateMutableCopy(NULL,0,original) :
        CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!extensions) return kVTAllocationFailedErr;
    e = noErr;
    for (unsigned i = 0; i < 3; i++) {
        if (!present[i]) continue;
        CFDataRef value = CFDataCreate(NULL,bytes[i],lengths[i]);
        if (!value) { e = kVTAllocationFailedErr; break; }
        CFDictionarySetValue(extensions,keys[i],value); CFRelease(value);
    }
    if (!e) {
        int result = x9_hevc_merge_preferred_transfer(extensions,hdr);
        if (result) e = result == ENOMEM ? kVTAllocationFailedErr : kVTVideoDecoderBadDataErr;
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
int x9_hevc_read_pixel_aspect(CFDictionaryRef aspect, uint32_t *horizontal, uint32_t *vertical)
{
    if (!horizontal || !vertical || horizontal == vertical) return EINVAL;
    *horizontal = *vertical = 0;
    if (!aspect) return 0;
    if (CFGetTypeID(aspect) != CFDictionaryGetTypeID()) return EBADMSG;
    CFTypeRef h = CFDictionaryGetValue(aspect,kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing);
    CFTypeRef v = CFDictionaryGetValue(aspect,kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing);
    int64_t x = 0, y = 0;
    if (!h || !v || CFGetTypeID(h) != CFNumberGetTypeID() || CFGetTypeID(v) != CFNumberGetTypeID() ||
        !CFNumberGetValue(h,kCFNumberSInt64Type,&x) || !CFNumberGetValue(v,kCFNumberSInt64Type,&y) ||
        x <= 0 || y <= 0 || x > UINT32_MAX || y > UINT32_MAX) return EBADMSG;
    *horizontal = (uint32_t)x; *vertical = (uint32_t)y;
    return 0;
}

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
    struct x9_hevc_hdr hdr;
    OSStatus hdr_error = x9_hevc_read_hdr_description(format,&hdr);
    if (hdr_error) return hdr_error;
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
        { kCMFormatDescriptionExtension_AmbientViewingEnvironment,
          kCVImageBufferAmbientViewingEnvironmentKey, CFDataGetTypeID() },
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
                CFDataGetLength(values[i]) != 4) ||
            (CFEqual(entries[i].source,kCMFormatDescriptionExtension_AmbientViewingEnvironment) &&
                CFDataGetLength(values[i]) != 8))
            return kVTVideoDecoderBadDataErr;
        if (CFEqual(entries[i].source,
                    kCMFormatDescriptionExtension_PixelAspectRatio)) {
            uint32_t h, v;
            if (x9_hevc_read_pixel_aspect(values[i],&h,&v))
                return kVTVideoDecoderBadDataErr;
        }
        if (entries[i].type == CFNumberGetTypeID()) {
            double gamma = 0;
            if (!CFNumberGetValue(values[i], kCFNumberDoubleType, &gamma) ||
                !isfinite(gamma) || gamma <= 0)
                return kVTVideoDecoderBadDataErr;
        }
    }
    CFDictionaryRef transfer = NULL;
    int result = x9_hevc_copy_transfer_extensions(CMFormatDescriptionGetExtensions(format),&transfer);
    if (result) return result == ENOMEM ? kVTAllocationFailedErr : kVTVideoDecoderBadDataErr;
    unsigned preferred = hdr.preferred_present ||
        CMFormatDescriptionGetExtension(format,kCMFormatDescriptionExtension_AlternativeTransferCharacteristics) != NULL;
    for (size_t i = 0; i < count; i++) {
        unsigned is_transfer = CFEqual(entries[i].source,kCMFormatDescriptionExtension_TransferFunction) ||
            CFEqual(entries[i].source,kCMFormatDescriptionExtension_GammaLevel);
        if (is_transfer) values[i] = CFDictionaryGetValue(transfer,entries[i].source);
        if (values[i]) CVBufferSetAttachment(pixel, entries[i].destination,
            values[i], kCVAttachmentMode_ShouldPropagate);
        else if (CFEqual(entries[i].source,kCMFormatDescriptionExtension_MasteringDisplayColorVolume) ||
            CFEqual(entries[i].source,kCMFormatDescriptionExtension_ContentLightLevelInfo) ||
            CFEqual(entries[i].source,kCMFormatDescriptionExtension_AmbientViewingEnvironment) ||
            (preferred && is_transfer))
            CVBufferRemoveAttachment(pixel,entries[i].destination);
    }
    CFRelease(transfer);
    return noErr;
}
OSStatus x9_hevc_validate_hdr_metadata(CMVideoFormatDescriptionRef format, CVPixelBufferRef pixel)
{
    if (!format || !pixel || CMFormatDescriptionGetMediaType(format) != kCMMediaType_Video)
        return kVTParameterErr;
    struct x9_hevc_hdr hdr;
    OSStatus hdr_error = x9_hevc_read_hdr_description(format,&hdr);
    if (hdr_error) return hdr_error;
    CFTypeRef aspect = CMFormatDescriptionGetExtension(format,kCMFormatDescriptionExtension_PixelAspectRatio);
    if (aspect) {
        uint32_t eh, ev, ah, av;
        CFTypeRef actual = CVBufferCopyAttachment(pixel,kCVImageBufferPixelAspectRatioKey,NULL);
        int e = x9_hevc_read_pixel_aspect(aspect,&eh,&ev);
        if (!e) e = x9_hevc_read_pixel_aspect(actual,&ah,&av);
        /* Cross products fit uint64 even at UINT32_MAX. Equivalent reduced
         * spacing pairs are legal; a missing CV ratio cannot match. */
        if (!e && (!ah || (uint64_t)eh*av != (uint64_t)ah*ev)) e = EBADMSG;
        if (actual) CFRelease(actual);
        if (e) return kVTVideoDecoderBadDataErr;
    }
    CFStringRef cm[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo,
        kCMFormatDescriptionExtension_AmbientViewingEnvironment };
    CFStringRef cv[] = { kCVImageBufferMasteringDisplayColorVolumeKey,
        kCVImageBufferContentLightLevelInfoKey,
        kCVImageBufferAmbientViewingEnvironmentKey };
    const CFIndex lengths[] = {24,4,8};
    for (unsigned n = 0; n < 3; n++) {
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
    if (hdr.preferred_present ||
        CMFormatDescriptionGetExtension(format,kCMFormatDescriptionExtension_AlternativeTransferCharacteristics)) {
        CFDictionaryRef transfer = NULL;
        int result = x9_hevc_copy_transfer_extensions(CMFormatDescriptionGetExtensions(format),&transfer);
        if (result) return result == ENOMEM ? kVTAllocationFailedErr : kVTVideoDecoderBadDataErr;
        CFStringRef keys[] = { kCMFormatDescriptionExtension_TransferFunction,kCMFormatDescriptionExtension_GammaLevel };
        CFStringRef destinations[] = { kCVImageBufferTransferFunctionKey,kCVImageBufferGammaLevelKey };
        OSStatus e = noErr;
        for (unsigned i = 0; i < 2 && !e; i++) {
            CFTypeRef expected = CFDictionaryGetValue(transfer,keys[i]);
            CFTypeRef actual = CVBufferCopyAttachment(pixel,destinations[i],NULL);
            if ((expected || actual) && (!expected || !actual || !CFEqual(expected,actual)))
                e = kVTVideoDecoderBadDataErr;
            if (actual) CFRelease(actual);
        }
        CFRelease(transfer);
        if (e) return e;
    }
    return noErr;
}
