#include "heif_native.h"
#include "heif_coremedia.h"
#include "heif_compositor.h"
#include "heif_metadata.h"
#include "vt_decoder.h"
#include <CoreGraphics/CoreGraphics.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define GRAPH_METADATA_LIMIT (16u * 1024u * 1024u)
struct native_item { uint32_t id, type; uint16_t canvas[4]; CFDictionaryRef color, declared, source; };
struct native_context {
    struct x9_heif_document *document;
    struct x9_heif_plan *plan;
    CFDataRef file, working, intermediate, transfer_profile;
    CGColorSpaceRef working_space, intermediate_space, transfer_space;
    int resampling, linear;
    size_t raster_budget;
    enum x9_heif_source_color_policy policy;
    enum x9_heif_graph_color_policy graph_color;
    CFMutableArrayRef items;
    unsigned count, frames;
    uint64_t metadata_bytes;
    OSStatus failure;
    struct native_item nodes[X9_HEIF_MAX_ITEMS];
};
static OSStatus parser_error(int e)
{
    return !e ? noErr : e == ENOMEM ? kVTAllocationFailedErr :
        e == ENOTSUP ? kVTVideoDecoderUnsupportedDataFormatErr : kVTVideoDecoderBadDataErr;
}
static int put_number(CFMutableDictionaryRef d, CFStringRef key, uint32_t n)
{
    int64_t value = n;
    CFNumberRef number = CFNumberCreate(NULL,kCFNumberSInt64Type,&value);
    if (!number) return ENOMEM;
    CFDictionarySetValue(d,key,number); CFRelease(number); return 0;
}
static int rgb_profile(CFDataRef data)
{
    if (!data || CFGetTypeID(data) != CFDataGetTypeID() || CFDataGetLength(data) <= 0 ||
        CFDataGetLength(data) > X9_HEIF_MAX_ITEM_METADATA) return 0;
    CGColorSpaceRef space = CGColorSpaceCreateWithICCData(data);
    int valid = space && CGColorSpaceGetModel(space) == kCGColorSpaceModelRGB &&
        CGColorSpaceGetNumberOfComponents(space) == 3;
    if (space) CGColorSpaceRelease(space);
    return valid;
}
static int linearize_rgb(CGColorSpaceRef base, CGColorSpaceRef *space, CFDataRef *profile)
{
    *space = NULL; *profile = NULL;
    /* CoreGraphics explicitly returns NULL for non-matrix spaces here. Do
     * not classify an arbitrary RGB ICC as linear from its label or tags. */
    CGColorSpaceRef linear = CGColorSpaceCreateExtendedLinearized(base);
    if (!linear) return ENOTSUP;
    int e = CGColorSpaceGetModel(linear) != kCGColorSpaceModelRGB ||
        CGColorSpaceGetNumberOfComponents(linear) != 3 ||
        !CGColorSpaceUsesExtendedRange(linear) || !CGColorSpaceSupportsOutput(linear) ? ENOTSUP : 0;
    CFDataRef icc = !e ? CGColorSpaceCopyICCData(linear) : NULL;
    if (!e && !rgb_profile(icc)) e = ENOTSUP;
    if (e) {
        if (icc) CFRelease(icc);
        CGColorSpaceRelease(linear);
    } else { *space = linear; *profile = icc; }
    return e;
}
static int source_color(CFDictionaryRef container, const struct x9_hevc_vui *vui,
    enum x9_heif_source_color_policy policy, CFDictionaryRef *out)
{
    *out = NULL;
    /* Progressive raster transfer only; never treat two fields as one
     * progressive image, or synthesize a field reconstruction. */
    if (vui->field_seq || vui->chroma_top != vui->chroma_bottom) return ENOTSUP;
    CFDictionaryRef colr = NULL;
    int e = x9_heif_resolve_color_extensions(container,vui,&colr);
    if (e) return e;
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!d) { CFRelease(colr); return ENOMEM; }
    CFStringRef keys[] = { kCMFormatDescriptionExtension_ICCProfile,kCMFormatDescriptionExtension_ColorPrimaries,
        kCMFormatDescriptionExtension_TransferFunction,kCMFormatDescriptionExtension_YCbCrMatrix,
        kCMFormatDescriptionExtension_GammaLevel,kCMFormatDescriptionExtension_FullRangeVideo,
        kCMFormatDescriptionExtension_ChromaLocationTopField,kCMFormatDescriptionExtension_ChromaLocationBottomField };
    for (unsigned n = 0; n < 8 && !e; n++) {
        CFTypeRef value = CFDictionaryGetValue(colr,keys[n]);
        /* Only the shared container/selected-SPS resolver is evidence.
         * CM's possibly synthesized extensions are never read here. */
        if (!value) continue;
        CFTypeID type = n == 0 ? CFDataGetTypeID() : n < 4 ? CFStringGetTypeID() :
            n == 4 ? CFNumberGetTypeID() : n == 5 ? CFBooleanGetTypeID() : CFStringGetTypeID();
        if (CFGetTypeID(value) != type) e = EBADMSG;
        else CFDictionarySetValue(d,keys[n],value);
    }
    CFTypeRef icc = CFDictionaryGetValue(d,keys[0]);
    if (!e && policy == X9_HEIF_USE_ICC_IF_PRESENT && icc) {
        if (!rgb_profile(icc)) e = ENOTSUP;
        /* Explicit ICC RGB interpretation; retain YUV matrix/range, but do
         * not ask the converter to choose between competing RGB profiles. */
        CFDictionaryRemoveValue(d,keys[1]); CFDictionaryRemoveValue(d,keys[2]); CFDictionaryRemoveValue(d,keys[4]);
    } else {
        CFDictionaryRemoveValue(d,keys[0]);
        CFTypeRef primaries = CFDictionaryGetValue(d,keys[1]), transfer = CFDictionaryGetValue(d,keys[2]);
        if (!e && (!primaries || !transfer)) e = ENOTSUP;
        if (!e && CFEqual(transfer,kCVImageBufferTransferFunction_UseGamma)) {
            CFTypeRef gamma = CFDictionaryGetValue(d,keys[4]); double level = 0;
            if (!gamma || !CFNumberGetValue(gamma,kCFNumberDoubleType,&level) || !isfinite(level) || level <= 0) e = ENOTSUP;
        } else CFDictionaryRemoveValue(d,keys[4]);
    }
    if (!e && !CFDictionaryGetValue(d,keys[3])) e = ENOTSUP; /* no guessed YUV matrix */
    CFRelease(colr);
    if (e) CFRelease(d); else *out = d;
    return e;
}
static int canvas_color(CFDictionaryRef color, enum x9_heif_source_color_policy policy,
    CFDictionaryRef *out)
{
    *out = NULL;
    CFMutableDictionaryRef d = CFDictionaryCreateMutableCopy(NULL,0,color);
    if (!d) return ENOMEM;
    CFTypeRef icc = CFDictionaryGetValue(d,kCMFormatDescriptionExtension_ICCProfile);
    int e = 0;
    if (policy == X9_HEIF_USE_ICC_IF_PRESENT && icc) {
        if (!rgb_profile(icc)) e = ENOTSUP;
        CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_ColorPrimaries);
        CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_TransferFunction);
        CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_GammaLevel);
    } else {
        CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_ICCProfile);
        CFTypeRef primaries = CFDictionaryGetValue(d,kCMFormatDescriptionExtension_ColorPrimaries);
        CFTypeRef transfer = CFDictionaryGetValue(d,kCMFormatDescriptionExtension_TransferFunction);
        if (!primaries || !transfer) e = ENOTSUP;
        else if (CFGetTypeID(primaries) != CFStringGetTypeID() ||
            CFGetTypeID(transfer) != CFStringGetTypeID()) e = EBADMSG;
        else if (CFEqual(transfer,kCVImageBufferTransferFunction_UseGamma)) {
            CFTypeRef gamma = CFDictionaryGetValue(d,kCMFormatDescriptionExtension_GammaLevel);
            double level = 0;
            if (!gamma || CFGetTypeID(gamma) != CFNumberGetTypeID() ||
                !CFNumberGetValue(gamma,kCFNumberDoubleType,&level) || !isfinite(level) || level <= 0) e = ENOTSUP;
        } else CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_GammaLevel);
    }
    /* iovl fill is unsigned16 RGB, not YCbCr/video-range samples. No SPS
     * default or ancestor's YUV matrix may reinterpret those RGB words. */
    CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_YCbCrMatrix);
    CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_FullRangeVideo);
    CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_ChromaLocationTopField);
    CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_ChromaLocationBottomField);
    if (e) CFRelease(d); else *out = d;
    return e;
}
static int prepare(struct native_context *c, uint32_t id, CFDictionaryRef inherited)
{
    /* Geometry/cycle/depth was already checked by the immutable plan. A
     * shared coded item cannot be cached under two source interpretations.
     * PER_ITEM always uses its immutable own colr + selected SPS, so ancestry
     * never changes its source interpretation or the cached target ICC. */
    for (unsigned n = 0; n < c->count; n++) if (c->nodes[n].id == id) {
        if (c->graph_color == X9_HEIF_GRAPH_COLOR_PER_ITEM) return 0;
        CFDictionaryRef effective = c->nodes[n].declared;
        if (inherited && CFDictionaryGetCount(inherited)) {
            if (CFDictionaryGetCount(effective) && !CFEqual(effective,inherited)) return ENOTSUP;
            effective = inherited;
        }
        if (!CFEqual(c->nodes[n].color,effective)) return ENOTSUP;
        return 0;
    }
    if (c->count == X9_HEIF_MAX_ITEMS) return ENOSPC;
    struct x9_heif_image *image = calloc(1,sizeof(*image));
    if (!image) return ENOMEM;
    CFDictionaryRef explicit_color = NULL, info = NULL;
    struct x9_heif_geometry geometry;
    int e = x9_heif_get_image(c->document,id,image);
    if (!e) e = x9_heif_plan_geometry(c->plan,id,&geometry);
    if (!e && (geometry.auxiliary_count || image->premultiplied_reference_count)) e = ENOTSUP;
    for (unsigned n = 0; n < image->property_count && !e; n++) {
        uint64_t bytes = image->properties[n].payload.bytes;
        if (bytes > GRAPH_METADATA_LIMIT-c->metadata_bytes) e = EMSGSIZE;
        else c->metadata_bytes += bytes;
    }
    if (!e) e = x9_heif_copy_color_extensions(c->document,image,&explicit_color);
    CFDictionaryRef color = explicit_color;
    if (!e && c->graph_color == X9_HEIF_GRAPH_COLOR_INHERIT_IDENTICAL &&
        inherited && CFDictionaryGetCount(inherited)) {
        if (CFDictionaryGetCount(explicit_color) && !CFEqual(explicit_color,inherited)) e = ENOTSUP;
        else color = inherited;
    }
    struct native_item *node = NULL;
    if (!e) {
        node = &c->nodes[c->count++];
        node->id = id; node->type = image->type; node->color = CFRetain(color);
        memcpy(node->canvas,image->canvas_fill_rgba,sizeof(node->canvas));
        node->declared = CFRetain(explicit_color);
        e = x9_heif_copy_item_info(c->document,image,&info,NULL);
    }
    CFDictionaryRef bitstream_info = NULL;
    if (!e && image->type == X9_HEIF_HVC1) {
        CMSampleBufferRef sample = NULL; CFDictionaryRef temporary_info = NULL;
        struct x9_hevc_vui vui;
        c->failure = x9_heif_copy_item_sample(c->file,id,&sample,&temporary_info,&vui);
        if (!c->failure && (!sample || !temporary_info)) c->failure = kVTVideoDecoderBadDataErr;
        if (c->failure) e = EIO;
        else {
            e = source_color(color,&vui,c->policy,&node->source);
            if (!e) e = x9_heif_copy_vui_info(&vui,&bitstream_info);
        }
        if (sample) CFRelease(sample);
        if (temporary_info) CFRelease(temporary_info);
    }
    if (!e && image->type == X9_HEIF_IOVL) {
        if (node->canvas[3]) e = canvas_color(color,c->policy,&node->source);
        else {
            /* Transparent fill has no observable RGB interpretation. Keep
             * declared colr without inventing a profile for invisible RGB. */
            node->source = CFDictionaryCreate(NULL,NULL,NULL,0,
                &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            if (!node->source) e = ENOMEM;
        }
    }
    if (!e) {
        CFMutableDictionaryRef record = CFDictionaryCreateMutableCopy(NULL,0,info);
        if (!record) e = ENOMEM;
        else {
            e = put_number(record,CFSTR("TransformedWidth"),geometry.width);
            if (!e) e = put_number(record,CFSTR("TransformedHeight"),geometry.height);
            if (!e) {
                CFDictionarySetValue(record,CFSTR("EffectiveColr"),color);
                CFDictionarySetValue(record,CFSTR("ColorInterpretation"),
                    image->type == X9_HEIF_IOVL ? (node->canvas[3] ?
                        CFSTR("declared-or-explicitly-inherited-rgb-fill-to-linear-icc") :
                        CFSTR("transparent-fill-no-rgb-interpretation-required")) :
                    c->graph_color == X9_HEIF_GRAPH_COLOR_PER_ITEM ?
                        (node->source ? CFSTR("own-colr-and-selected-sps-to-common-icc") :
                            CFSTR("declared-derived-colr-retained-not-applied")) :
                        CFSTR("identical-colr-inheritance-and-selected-sps"));
                if (node->source) CFDictionarySetValue(record,CFSTR("ResolvedSourceColor"),node->source);
                if (bitstream_info) CFDictionarySetValue(record,CFSTR("BitstreamVUI"),bitstream_info);
                CFArrayAppendValue(c->items,record);
            }
            CFRelease(record);
        }
    }
    if (bitstream_info) CFRelease(bitstream_info);
    for (unsigned n = 0; n < image->derived_count && !e; n++)
        e = prepare(c,image->derived[n],c->graph_color == X9_HEIF_GRAPH_COLOR_PER_ITEM ? NULL : color);
    if (explicit_color) CFRelease(explicit_color);
    if (info) CFRelease(info);
    free(image); return e;
}
static void attach_source(CVPixelBufferRef view, CFDictionaryRef color)
{
    CFStringRef cm[] = { kCMFormatDescriptionExtension_ICCProfile,kCMFormatDescriptionExtension_ColorPrimaries,
        kCMFormatDescriptionExtension_TransferFunction,kCMFormatDescriptionExtension_YCbCrMatrix,
        kCMFormatDescriptionExtension_GammaLevel,kCMFormatDescriptionExtension_ChromaLocationTopField,
        kCMFormatDescriptionExtension_ChromaLocationBottomField };
    CFStringRef cv[] = { kCVImageBufferICCProfileKey,kCVImageBufferColorPrimariesKey,
        kCVImageBufferTransferFunctionKey,kCVImageBufferYCbCrMatrixKey,kCVImageBufferGammaLevelKey,
        kCVImageBufferChromaLocationTopFieldKey,kCVImageBufferChromaLocationBottomFieldKey };
    for (unsigned n = 0; n < 7; n++) {
        CFTypeRef value = CFDictionaryGetValue(color,cm[n]);
        CVBufferRemoveAttachment(view,cv[n]);
        if (value) CVBufferSetAttachment(view,cv[n],value,kCVAttachmentMode_ShouldPropagate);
    }
    /* A stale derived CGColorSpace must not overrule the explicit policy. */
    CVBufferRemoveAttachment(view,kCVImageBufferCGColorSpaceKey);
    CVBufferRemoveAttachment(view,kCVImageBufferCleanApertureKey);
    CVBufferRemoveAttachment(view,kCVImageBufferPreferredCleanApertureKey);
}
/* Actual VT transfer, not a profile relabel. Check its returned attachment;
 * that still does not replace runtime color/HDR pixel verification. */
static OSStatus transfer_image(CVPixelBufferRef source, CVPixelBufferRef target, CFDataRef profile)
{
    VTPixelTransferSessionRef transfer = NULL;
    OSStatus e = VTPixelTransferSessionCreate(NULL,&transfer);
    if (!e) e = VTSessionSetProperty(transfer,kVTPixelTransferPropertyKey_ScalingMode,kVTScalingMode_Normal);
    if (!e) e = VTSessionSetProperty(transfer,kVTPixelTransferPropertyKey_DestinationICCProfile,profile);
    if (!e) e = VTPixelTransferSessionTransferImage(transfer,source,target);
    if (!e) {
        CFTypeRef actual = CVBufferCopyAttachment(target,kCVImageBufferICCProfileKey,NULL);
        if (!actual || CFGetTypeID(actual) != CFDataGetTypeID() || !CFEqual(actual,profile)) e = kVTPixelTransferNotSupportedErr;
        if (actual) CFRelease(actual);
    }
    if (transfer) { VTPixelTransferSessionInvalidate(transfer); CFRelease(transfer); }
    return e;
}
static OSStatus check_pixels(CVPixelBufferRef pixel, size_t budget)
{
    OSType format = CVPixelBufferGetPixelFormatType(pixel);
    size_t component_bytes = format == kCVPixelFormatType_128RGBAFloat ? 4 : 2;
    size_t w = CVPixelBufferGetWidth(pixel), h = CVPixelBufferGetHeight(pixel);
    if ((format != kCVPixelFormatType_128RGBAFloat && format != kCVPixelFormatType_64RGBAHalf) ||
        CVPixelBufferIsPlanar(pixel) || !w || !h || w > X9_HEIF_MAX_DIMENSION || h > X9_HEIF_MAX_DIMENSION)
        return kVTVideoDecoderBadDataErr;
    OSStatus e = CVPixelBufferLockBaseAddress(pixel,kCVPixelBufferLock_ReadOnly);
    if (e) return e;
    const uint8_t *base = CVPixelBufferGetBaseAddress(pixel);
    size_t stride = CVPixelBufferGetBytesPerRow(pixel), row = w*4*component_bytes;
    if (!base || stride < row || stride > budget/h ||
        stride*h > UINTPTR_MAX-(uintptr_t)base) e = kVTVideoDecoderBadDataErr;
    for (size_t y = 0; y < h && !e; y++) for (size_t x = 0; x < w && !e; x++)
        for (unsigned k = 0; k < 4; k++) {
            const uint8_t *p = base+y*stride+(x*4+k)*component_bytes;
            uint32_t bits = p[0] | (uint32_t)p[1]<<8;
            if (component_bytes == 4) bits |= (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
            uint32_t exponent = component_bytes == 4 ? 0x7f800000u : 0x7c00u;
            uint32_t one = component_bytes == 4 ? 0x3f800000u : 0x3c00u;
            /* These native sources have no alpha (aux rejects in preflight).
             * Verify opaque output rather than manufacturing alpha or assuming
             * a straight/premultiplied convention for non-opaque data. */
            if ((bits & exponent) == exponent || (k == 3 && bits != one)) e = kVTVideoDecoderBadDataErr;
        }
    OSStatus unlock = CVPixelBufferUnlockBaseAddress(pixel,kCVPixelBufferLock_ReadOnly);
    return e ? e : unlock;
}
static float load_float(const uint8_t *p)
{
    uint32_t bits = p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
    float value; memcpy(&value,&bits,sizeof(value)); return value;
}
static void store_float(uint8_t *p, float value)
{
    uint32_t bits; memcpy(&bits,&value,sizeof(bits));
    for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(bits>>(8*n));
}
static OSStatus native_canvas(struct native_context *c, const struct native_item *node,
    uint32_t width, uint32_t height, uint8_t *destination, size_t stride)
{
    if (!node->canvas[3]) {
        for (uint32_t y = 0; y < height; y++) memset(destination+y*stride,0,(size_t)width*16);
        return noErr;
    }
    uint8_t raw[8], converted[16] = {0};
    for (unsigned n = 0; n < 4; n++) {
        uint16_t v = n == 3 ? UINT16_MAX : node->canvas[n];
        raw[2*n] = (uint8_t)v; raw[2*n+1] = (uint8_t)(v>>8);
    }
    CVPixelBufferRef source = NULL, target = NULL;
    /* Convert RGB as opaque. Apply the raw normalized alpha ourselves AFTER
     * ICC conversion; never ask VT to guess a premultiplied RGB convention. */
    OSStatus e = CVPixelBufferCreateWithBytes(NULL,1,1,kCVPixelFormatType_64RGBALE,
        raw,8,NULL,NULL,NULL,&source);
    if (!e) {
        attach_source(source,node->source);
        CVBufferSetAttachment(source,kCVImageBufferAlphaChannelIsOpaque,kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
        e = CVPixelBufferCreateWithBytes(NULL,1,1,kCVPixelFormatType_128RGBAFloat,
            converted,16,NULL,NULL,NULL,&target);
    }
    if (!e) e = transfer_image(source,target,c->transfer_profile);
    if (!e) e = check_pixels(target,c->raster_budget);
    if (!e) {
        float alpha = (float)node->canvas[3]/65535.0f;
        for (unsigned n = 0; n < 3; n++) store_float(converted+4*n,load_float(converted+4*n)*alpha);
        store_float(converted+12,alpha);
        for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++)
            memcpy(destination+y*stride+(size_t)x*16,converted,sizeof(converted));
    }
    if (source) CVPixelBufferRelease(source); if (target) CVPixelBufferRelease(target);
    return e;
}
static int native_raster(void *context, uint32_t id, enum x9_heif_raster_format format,
    uint32_t width, uint32_t height, uint8_t *destination, size_t stride)
{
    struct native_context *c = context;
    struct native_item *node = NULL;
    for (unsigned n = 0; n < c->count; n++) if (c->nodes[n].id == id) node = &c->nodes[n];
    if (!node || !node->source || format != (c->linear ? X9_HEIF_RGBA_FLOAT : X9_HEIF_RGBA_HALF)) return EINVAL;
    if (node->type == X9_HEIF_IOVL) {
        if (!c->linear) return ENOTSUP;
        OSStatus error = native_canvas(c,node,width,height,destination,stride);
        if (error) { if (!c->failure) c->failure = error; return EIO; }
        return 0; /* fill conversion is NOT a hardware-decoded frame */
    }
    CMSampleBufferRef sample = NULL;
    CFDictionaryRef info = NULL;
    CVPixelBufferRef decoded = NULL, view = NULL, target = NULL;
    int locked = 0;
    OSStatus e = X9HEIFCopyItemSample(c->file,id,&sample,&info);
    if (!e && (!sample || !info)) e = kVTVideoDecoderBadDataErr;
    if (!e) e = X9HEVCCopyHardwarePicture(sample,&decoded);
    if (!e && !decoded) e = kVTVideoDecoderBadDataErr;
    if (!e && (CVPixelBufferGetWidth(decoded) != width || CVPixelBufferGetHeight(decoded) != height))
        e = kVTVideoDecoderBadDataErr;
    OSType f = decoded ? CVPixelBufferGetPixelFormatType(decoded) : 0;
    CFTypeRef full = CFDictionaryGetValue(node->source,kCMFormatDescriptionExtension_FullRangeVideo);
    int actual_full = f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    if (!e && full && (int)CFBooleanGetValue(full) != actual_full) e = kVTVideoDecoderBadDataErr;
    if (!e) { e = CVPixelBufferLockBaseAddress(decoded,kCVPixelBufferLock_ReadOnly); locked = !e; }
    if (!e) {
        void *planes[2]; size_t widths[2], heights[2], strides[2];
        for (unsigned n = 0; n < 2; n++) {
            planes[n] = CVPixelBufferGetBaseAddressOfPlane(decoded,n);
            widths[n] = CVPixelBufferGetWidthOfPlane(decoded,n);
            heights[n] = CVPixelBufferGetHeightOfPlane(decoded,n);
            strides[n] = CVPixelBufferGetBytesPerRowOfPlane(decoded,n);
            size_t row = (size_t)width*(f == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
                f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange ? 2u : 1u);
            if (!heights[n] || strides[n] > X9_HEIF_MAX_RASTER_BYTES/heights[n]) {
                e = kVTVideoDecoderBadDataErr; continue;
            }
            uint64_t span = (uint64_t)strides[n]*(heights[n]-1)+row;
            if (!planes[n] || strides[n] < row || span > UINTPTR_MAX-(uintptr_t)planes[n]) e = kVTVideoDecoderBadDataErr;
        }
        /* Separate view owns ONLY attachments, not decoded storage. No
         * modification of a VT callback pixel lacking ImageBufferModifiable.
         * Retain+read lock the decoded owner through the whole transfer. */
        if (!e) e = CVPixelBufferCreateWithPlanarBytes(NULL,width,height,f,NULL,0,2,
            planes,widths,heights,strides,NULL,NULL,NULL,&view);
        if (!e) { CVBufferPropagateAttachments(decoded,view); attach_source(view,node->source); }
    }
    if (!e) e = CVPixelBufferCreateWithBytes(NULL,width,height,
        c->linear ? kCVPixelFormatType_128RGBAFloat : kCVPixelFormatType_64RGBAHalf,
        destination,stride,NULL,NULL,NULL,&target);
    if (!e) e = transfer_image(view,target,c->transfer_profile);
    if (!e) e = check_pixels(target,c->raster_budget);
    if (target) CVPixelBufferRelease(target);
    if (view) CVPixelBufferRelease(view);
    if (locked) { OSStatus unlock = CVPixelBufferUnlockBaseAddress(decoded,kCVPixelBufferLock_ReadOnly); if (!e) e = unlock; }
    if (decoded) CVPixelBufferRelease(decoded);
    if (sample) CFRelease(sample);
    if (info) CFRelease(info);
    if (e) { if (!c->failure) c->failure = e; return EIO; }
    c->frames++; return 0;
}
static void release_pixels(void *context, const void *bytes)
{ (void)context; free((void *)bytes); }
/* Positive normalized binary32 alpha -> binary16, round to nearest/ties-even.
 * RGB conversion stays in VT; only alpha's exact component encoding is here. */
static uint16_t alpha_half(float alpha)
{
    uint32_t bits; memcpy(&bits,&alpha,sizeof(bits));
    unsigned exponent = (bits>>23)&255u; uint32_t fraction = bits & 0x7fffffu;
    if (exponent < 102) return 0;
    unsigned shift = exponent < 113 ? 126-exponent : 13;
    uint32_t mantissa = exponent < 113 ? fraction | 0x800000u : fraction;
    uint32_t value = exponent < 113 ? mantissa>>shift : ((exponent-112)<<10) | (mantissa>>shift);
    uint32_t remainder = mantissa & ((1u<<shift)-1), halfway = 1u<<(shift-1);
    if (remainder > halfway || (remainder == halfway && (value & 1))) value++;
    return (uint16_t)value;
}
static OSStatus export_raster(struct native_context *c, struct x9_heif_raster *r, CVPixelBufferRef *out)
{
    CVPixelBufferRef source = NULL, target = NULL;
    uint8_t *bytes = NULL;
    float *alpha = NULL; int opaque = 1;
    size_t count = (size_t)r->width*r->height, alpha_bytes = count*sizeof(float);
    OSStatus e = noErr;
    if (c->linear) {
        /* Root float + half export + alpha preservation coexist. All three
         * allocations count toward the explicit CPU heap budget. */
        if (r->bytes > c->raster_budget || alpha_bytes > c->raster_budget-r->bytes ||
            count*8 > c->raster_budget-r->bytes-alpha_bytes) e = kVTAllocationFailedErr;
        if (!e) { alpha = malloc(alpha_bytes); if (!alpha) e = kVTAllocationFailedErr; }
        for (uint32_t y = 0; y < r->height && !e; y++) for (uint32_t x = 0; x < r->width && !e; x++) {
            uint8_t *p = r->data+y*r->stride+(size_t)x*16;
            float a = load_float(p+12);
            if (!isfinite(a) || a < 0 || a > 1) { e = kVTVideoDecoderBadDataErr; break; }
            alpha[(size_t)y*r->width+x] = a; if (a != 1) opaque = 0;
            /* Transfer unassociated RGB as OPAQUE, then restore alpha into
             * our OWN half buffer. No dependency on VT alpha propagation or
             * profile conversion of premultiplied/non-linear RGB. */
            for (unsigned q = 0; q < 3; q++) {
                double v = a ? (double)load_float(p+4*q)/a : 0;
                if (!isfinite(v) || v < -FLT_MAX || v > FLT_MAX) { e = kVTVideoDecoderBadDataErr; break; }
                store_float(p+4*q,(float)v);
            }
            store_float(p+12,1);
        }
    }
    if (!e) e = CVPixelBufferCreateWithBytes(NULL,r->width,r->height,
        c->linear ? kCVPixelFormatType_128RGBAFloat : kCVPixelFormatType_64RGBAHalf,
        r->data,r->stride,release_pixels,NULL,NULL,&source);
    if (!e) r->data = NULL; /* ONLY successful CV owner adoption transfers bytes */
    if (!e && c->linear) {
        size_t stride = (size_t)r->width*8, size = stride*r->height;
        if (!e) { bytes = calloc(1,size); if (!bytes) e = kVTAllocationFailedErr; }
        if (!e) e = CVPixelBufferCreateWithBytes(NULL,r->width,r->height,kCVPixelFormatType_64RGBAHalf,
            bytes,stride,release_pixels,NULL,NULL,&target);
        if (!e) bytes = NULL;
        if (!e) {
            /* The compositor's binary32 data is already in this explicit
             * derived linear space, so these are truthful source attachments. */
            CVBufferSetAttachment(source,kCVImageBufferICCProfileKey,c->transfer_profile,kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(source,kCVImageBufferCGColorSpaceKey,c->transfer_space,kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(source,kCVImageBufferAlphaChannelIsOpaque,kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
            e = transfer_image(source,target,c->working);
        }
        if (!e) e = check_pixels(target,c->raster_budget);
        if (!e) {
            e = CVPixelBufferLockBaseAddress(target,0);
            if (!e) {
                uint8_t *base = CVPixelBufferGetBaseAddress(target);
                size_t target_stride = CVPixelBufferGetBytesPerRow(target);
                for (uint32_t y = 0; y < r->height; y++) for (uint32_t x = 0; x < r->width; x++) {
                    float a = alpha[(size_t)y*r->width+x];
                    uint8_t *p = base+y*target_stride+(size_t)x*8;
                    uint16_t half = alpha_half(a);
                    p[6] = (uint8_t)half; p[7] = (uint8_t)(half>>8);
                    if (!half) memset(p,0,6); /* transparent RGB has no defined unassociated color */
                }
                e = CVPixelBufferUnlockBaseAddress(target,0);
            }
        }
        if (!e) { *out = target; target = NULL; }
    } else if (!e) { *out = source; source = NULL; }
    if (!e) {
        CVBufferSetAttachment(*out,kCVImageBufferAlphaChannelIsOpaque,opaque ? kCFBooleanTrue : kCFBooleanFalse,
            kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(*out,kCVImageBufferAlphaChannelModeKey,kCVImageBufferAlphaChannelMode_StraightAlpha,
            kCVAttachmentMode_ShouldPropagate);
    }
    free(alpha);
    free(bytes);
    if (source) CVPixelBufferRelease(source);
    if (target) CVPixelBufferRelease(target);
    return e;
}
OSStatus X9HEIFCopyHardwareImageWithColorOptions(CFDataRef file, uint32_t id,
    CFDataRef working, CFDataRef intermediate,
    enum x9_heif_source_color_policy policy, enum x9_heif_graph_color_policy graph_color,
    enum x9_heif_clap_policy clap_policy,
    uint64_t raster_budget, CVPixelBufferRef *out, CFDictionaryRef *info_out)
{
    if (!out || !info_out || (void *)out == (void *)info_out) return kVTParameterErr;
    *out = NULL; *info_out = NULL;
    if (!file || CFGetTypeID(file) != CFDataGetTypeID() || CFDataGetLength(file) <= 0 ||
        CFDataGetLength(file) > X9_HEIF_MAX_FILE || !rgb_profile(working) ||
        (intermediate && (!rgb_profile(intermediate) || clap_policy != X9_HEIF_CLAP_LINEAR_CEIL)) ||
        (policy != X9_HEIF_USE_ICC_IF_PRESENT && policy != X9_HEIF_USE_NCLX) ||
        (graph_color != X9_HEIF_GRAPH_COLOR_INHERIT_IDENTICAL && graph_color != X9_HEIF_GRAPH_COLOR_PER_ITEM) ||
        (clap_policy != X9_HEIF_CLAP_EXACT_SAMPLES && clap_policy != X9_HEIF_CLAP_LINEAR_CEIL) ||
        !raster_budget || raster_budget > SIZE_MAX || raster_budget > INT64_MAX) return kVTParameterErr;
    struct native_context *c = calloc(1,sizeof(*c));
    if (!c) return kVTAllocationFailedErr;
    /* CreateCopy may retain an immutable NoCopy CFData whose bytes still
     * belong to the caller. ICCs escape in output attachments/info;
     * use real owned byte copies, not an assumed immutable-copy allocation. */
    c->file = CFDataCreate(NULL,CFDataGetBytePtr(file),CFDataGetLength(file));
    c->working = CFDataCreate(NULL,CFDataGetBytePtr(working),CFDataGetLength(working)); c->policy = policy;
    c->graph_color = graph_color;
    if (intermediate) c->intermediate = CFDataCreate(NULL,CFDataGetBytePtr(intermediate),CFDataGetLength(intermediate));
    c->raster_budget = (size_t)raster_budget;
    c->working_space = c->working ? CGColorSpaceCreateWithICCData(c->working) : NULL;
    c->intermediate_space = c->intermediate ? CGColorSpaceCreateWithICCData(c->intermediate) : NULL;
    c->items = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pixel = NULL;
    struct x9_heif_raster raster = {0};
    struct x9_heif_geometry geometry;
    int e = !c->file || !c->working || !c->items || !info || !c->working_space ||
        (intermediate && (!c->intermediate || !c->intermediate_space)) ? ENOMEM : 0;
    if (!e && !CGColorSpaceSupportsOutput(c->working_space)) e = ENOTSUP;
    if (!e) e = x9_heif_open(CFDataGetBytePtr(c->file),(size_t)CFDataGetLength(c->file),&c->document);
    if (!e) e = x9_heif_plan_create_with_policy(c->document,id,clap_policy,&c->plan);
    if (!e) e = x9_heif_plan_geometry(c->plan,id,&geometry);
    if (!e) {
        c->resampling = x9_heif_plan_requires_resampling(c->plan);
        c->linear = x9_heif_plan_requires_linear_composition(c->plan);
        if (c->resampling < 0 || c->linear < 0) e = EINVAL;
        else if (c->linear) {
            /* Root float storage + final half + alpha plane coexist. Reject this
             * known budget failure before the first native hardware frame.
             * The compositor separately accounts ALL caches/intermediates;
             * VT's internal transfer/decoder storage is not this heap budget. */
            if ((uint64_t)geometry.width*geometry.height*28 > c->raster_budget) e = EMSGSIZE;
        }
        if (!e && (c->linear || c->intermediate))
            e = linearize_rgb(c->intermediate ? c->intermediate_space : c->working_space,
                &c->transfer_space,&c->transfer_profile);
        if (!e && !c->linear) {
            /* A supplied intermediate was qualified, but an integral plan
             * must not gain extra conversions just because it was supplied. */
            if (c->transfer_space) { CGColorSpaceRelease(c->transfer_space); c->transfer_space = NULL; }
            if (c->transfer_profile) CFRelease(c->transfer_profile);
            c->transfer_profile = CFRetain(c->working);
        }
    }
    if (!e) e = prepare(c,geometry.id,NULL); /* every source checked before hardware */
    if (!e) e = c->linear ? x9_heif_compose_linear_float(c->plan,native_raster,c,c->raster_budget,&raster) :
        x9_heif_compose_packed_with_budget(c->plan,X9_HEIF_RGBA_HALF,native_raster,c,c->raster_budget,&raster);
    OSStatus result = c->failure ? c->failure : parser_error(e);
    if (!result) result = export_raster(c,&raster,&pixel);
    if (!result) {
        e = put_number(info,CFSTR("ItemID"),geometry.id);
        if (!e) e = put_number(info,CFSTR("Width"),geometry.width);
        if (!e) e = put_number(info,CFSTR("Height"),geometry.height);
        if (!e) e = put_number(info,CFSTR("PixelFormat"),kCVPixelFormatType_64RGBAHalf);
        if (!e) e = put_number(info,CFSTR("NativeHardwareFrames"),c->frames);
        if (!e) e = put_number(info,CFSTR("SourceColorPolicy"),(uint32_t)policy);
        if (!e) e = put_number(info,CFSTR("GraphColorPolicy"),(uint32_t)graph_color);
        if (!e) e = put_number(info,CFSTR("CleanAperturePolicy"),(uint32_t)clap_policy);
        if (!e) {
            int64_t size = (int64_t)c->raster_budget;
            CFNumberRef number = CFNumberCreate(NULL,kCFNumberSInt64Type,&size);
            if (!number) e = ENOMEM;
            else { CFDictionarySetValue(info,CFSTR("RasterBudgetBytes"),number); CFRelease(number); }
        }
        result = parser_error(e);
        if (!result) {
            CFDictionarySetValue(info,CFSTR("Items"),c->items);
            CFDictionarySetValue(info,CFSTR("WorkingRGBICCProfile"),c->working);
            CFDictionarySetValue(info,CFSTR("GraphColorInterpretation"),
                graph_color == X9_HEIF_GRAPH_COLOR_PER_ITEM ?
                    CFSTR("explicit per-item sources; common ICC composition; iovl colr interprets fill only") :
                    CFSTR("identical inherited colr; conflicting contexts reject"));
            CFDictionarySetValue(info,CFSTR("LinearLightResampling"),c->resampling ? kCFBooleanTrue : kCFBooleanFalse);
            CFDictionarySetValue(info,CFSTR("LinearLightComposition"),c->linear ? kCFBooleanTrue : kCFBooleanFalse);
            CFDictionarySetValue(info,CFSTR("AlphaChannelMode"),kCVImageBufferAlphaChannelMode_StraightAlpha);
            CFDictionarySetValue(info,CFSTR("ExplicitResamplingIntermediateUsed"),
                c->linear && c->intermediate ? kCFBooleanTrue : kCFBooleanFalse);
            if (c->intermediate)
                CFDictionarySetValue(info,CFSTR("RequestedResamplingIntermediateRGBICCProfile"),c->intermediate);
            if (c->linear) {
                CFDictionarySetValue(info,CFSTR("LinearizationBaseSelection"),
                    c->intermediate ? CFSTR("explicit-intermediate") : CFSTR("working-icc"));
                CFDictionarySetValue(info,CFSTR("LinearizationBaseRGBICCProfile"),c->intermediate ? c->intermediate : c->working);
                CFDictionarySetValue(info,CFSTR("LinearRGBICCProfile"),c->transfer_profile);
                if (c->resampling) CFDictionarySetValue(info,CFSTR("CleanApertureSampling"),CFSTR("ceil extent; pixel centers; bilinear linear-light; edge extension"));
            }
            CFDictionarySetValue(info,CFSTR("HardwareDecodePerformed"),c->frames ? kCFBooleanTrue : kCFBooleanFalse);
            /* An iovl canvas may have no visible coded placement. Its real
             * fill pixels do not prove any decoder was selected or ran. */
            CFDictionarySetValue(info,c->frames ? CFSTR("DecoderID") : CFSTR("AllowedDecoderID"),
                CFSTR(X9_HEVC_DECODER_ID));
            CVBufferSetAttachment(pixel,kCVImageBufferICCProfileKey,c->working,kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(pixel,kCVImageBufferCGColorSpaceKey,c->working_space,kCVAttachmentMode_ShouldPropagate);
            *out = pixel; pixel = NULL; *info_out = info; info = NULL;
        }
    }
    if (pixel) CVPixelBufferRelease(pixel);
    x9_heif_raster_close(&raster);
    if (info) CFRelease(info);
    for (unsigned n = 0; n < c->count; n++) {
        if (c->nodes[n].color) CFRelease(c->nodes[n].color);
        if (c->nodes[n].declared) CFRelease(c->nodes[n].declared);
        if (c->nodes[n].source) CFRelease(c->nodes[n].source);
    }
    if (c->items) CFRelease(c->items);
    if (c->working_space) CGColorSpaceRelease(c->working_space);
    if (c->intermediate_space) CGColorSpaceRelease(c->intermediate_space);
    if (c->transfer_space) CGColorSpaceRelease(c->transfer_space);
    if (c->transfer_profile) CFRelease(c->transfer_profile);
    x9_heif_plan_close(&c->plan); x9_heif_close(&c->document);
    if (c->file) CFRelease(c->file);
    if (c->working) CFRelease(c->working);
    if (c->intermediate) CFRelease(c->intermediate);
    free(c); return result;
}
OSStatus X9HEIFCopyHardwareImageWithIntermediateICC(CFDataRef file, uint32_t id,
    CFDataRef working, CFDataRef intermediate,
    enum x9_heif_source_color_policy policy, enum x9_heif_clap_policy clap_policy,
    uint64_t raster_budget, CVPixelBufferRef *out, CFDictionaryRef *info_out)
{ return X9HEIFCopyHardwareImageWithColorOptions(file,id,working,intermediate,policy,
    X9_HEIF_GRAPH_COLOR_INHERIT_IDENTICAL,clap_policy,raster_budget,out,info_out); }
OSStatus X9HEIFCopyHardwareImageWithOptions(CFDataRef file, uint32_t id, CFDataRef working,
    enum x9_heif_source_color_policy policy, enum x9_heif_clap_policy clap_policy,
    uint64_t raster_budget, CVPixelBufferRef *out, CFDictionaryRef *info_out)
{ return X9HEIFCopyHardwareImageWithIntermediateICC(file,id,working,NULL,policy,clap_policy,raster_budget,out,info_out); }
OSStatus X9HEIFCopyHardwareImage(CFDataRef file, uint32_t id, CFDataRef working,
    enum x9_heif_source_color_policy policy, CVPixelBufferRef *out, CFDictionaryRef *info_out)
{ return X9HEIFCopyHardwareImageWithOptions(file,id,working,policy,X9_HEIF_CLAP_EXACT_SAMPLES,X9_HEIF_MAX_RASTER_BYTES,out,info_out); }
