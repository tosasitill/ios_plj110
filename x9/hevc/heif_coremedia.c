#include "heif_coremedia.h"
#include "heif_items.h"
#include "hevc_bitstream.h"
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static OSStatus parse_error(int e)
{
    if (!e) return noErr;
    if (e == ENOMEM) return kVTAllocationFailedErr;
    if (e == ENOTSUP) return kVTVideoDecoderUnsupportedDataFormatErr;
    return kVTVideoDecoderBadDataErr;
}
static CFNumberRef number(uint32_t n)
{ int64_t value = n; return CFNumberCreate(NULL,kCFNumberSInt64Type,&value); }
static int put_number(CFMutableDictionaryRef d, CFStringRef key, uint32_t n)
{
    CFNumberRef v = number(n);
    if (!v) return ENOMEM;
    CFDictionarySetValue(d,key,v); CFRelease(v); return 0;
}
static uint32_t code16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static int format_catalog(CMVideoFormatDescriptionRef format, const uint8_t *hvcc,
    const struct x9_hevc_config *original)
{
    CFTypeRef atoms = CMFormatDescriptionGetExtension(format,
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (!atoms || CFGetTypeID(atoms) != CFDictionaryGetTypeID()) return EBADMSG;
    CFTypeRef value = CFDictionaryGetValue(atoms,CFSTR("hvcC"));
    if (!value || CFGetTypeID(value) != CFDataGetTypeID() || CFDataGetLength(value) <= 0) return EBADMSG;
    const uint8_t *data = CFDataGetBytePtr(value);
    struct x9_hevc_config generated;
    int e = x9_hevc_config_parse(data,(size_t)CFDataGetLength(value),&generated);
    if (e) return e;
    if (generated.length_size != original->length_size ||
        generated.parameter_set_count != original->parameter_set_count) return EBADMSG;
    /* CM may reorder arrays, but it must not lose/replace retained parameter
     * sets or change sample framing. Verify identity by type/ID and raw NAL,
     * not hvcC header bytes that CoreMedia is allowed to regenerate. */
    for (unsigned n = 0; n < original->parameter_set_count; n++) {
        const struct x9_hevc_parameter_set *s = &original->parameter_sets[n];
        unsigned k = 0;
        while (k < generated.parameter_set_count &&
            (generated.parameter_sets[k].type != s->type || generated.parameter_sets[k].id != s->id)) k++;
        if (k == generated.parameter_set_count || generated.parameter_sets[k].bytes != s->bytes ||
            memcmp(hvcc+s->offset,data+generated.parameter_sets[k].offset,s->bytes)) return EBADMSG;
    }
    return 0;
}
/* Keep all raw colr properties in ordered item info. Standard CICP strings
 * use the SDK's exact code-point mapping, not a guessed BT.709 default. ICC
 * and nclx can coexist; this bridge does not choose their ColorSync precedence.
 * Unknown CICP values remain in raw metadata and are not invented strings. */
static int color_extension(const uint8_t *data, const struct x9_heif_property *p,
    CFMutableDictionaryRef extensions, uint32_t *seen)
{
    if (p->type != X9_HEIF_TYPE('c','o','l','r')) return 0;
    const uint8_t *raw = data+p->payload.offset;
    uint32_t type = (uint32_t)raw[0] << 24 | (uint32_t)raw[1] << 16 |
                    (uint32_t)raw[2] << 8 | raw[3];
    if (type == X9_HEIF_TYPE('r','I','C','C') || type == X9_HEIF_TYPE('p','r','o','f')) {
        if (*seen & 1u) return EEXIST;
        CFDataRef icc = CFDataCreate(NULL,raw+4,(CFIndex)p->payload.bytes-4);
        if (!icc) return ENOMEM;
        CFDictionarySetValue(extensions,kCMFormatDescriptionExtension_ICCProfile,icc);
        CFRelease(icc); *seen |= 1u;
    } else if (type == X9_HEIF_TYPE('n','c','l','x')) {
        if (*seen & 2u) return EEXIST;
        CFStringRef keys[] = { kCMFormatDescriptionExtension_ColorPrimaries,
            kCMFormatDescriptionExtension_TransferFunction, kCMFormatDescriptionExtension_YCbCrMatrix };
        CFStringRef values[] = { CVColorPrimariesGetStringForIntegerCodePoint((int)code16(raw+4)),
            CVTransferFunctionGetStringForIntegerCodePoint((int)code16(raw+6)),
            CVYCbCrMatrixGetStringForIntegerCodePoint((int)code16(raw+8)) };
        uint32_t transfer = code16(raw+6);
        if (transfer == 4 || transfer == 5) {
            /* CICP4/5 specify gamma2.2/2.8. UseGamma alone is ambiguous. */
            double gamma = transfer == 4 ? 2.2 : 2.8;
            CFNumberRef level = CFNumberCreate(NULL,kCFNumberDoubleType,&gamma);
            if (!level) return ENOMEM;
            values[1] = kCVImageBufferTransferFunction_UseGamma;
            CFDictionarySetValue(extensions,kCMFormatDescriptionExtension_GammaLevel,level);
            CFRelease(level);
        }
        for (unsigned n = 0; n < 3; n++)
            if (values[n]) CFDictionarySetValue(extensions,keys[n],values[n]);
        CFDictionarySetValue(extensions,kCMFormatDescriptionExtension_FullRangeVideo,
            raw[10] & 0x80 ? kCFBooleanTrue : kCFBooleanFalse);
        *seen |= 2u;
    }
    return 0;
}
static int item_info(const struct x9_heif_document *document, const uint8_t *data,
    const struct x9_heif_image *image, CFDictionaryRef *out, CFMutableDictionaryRef extensions)
{
    CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef properties = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    CFMutableArrayRef aux = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    uint32_t color_seen = 0;
    int e = !info || !properties || !aux ? ENOMEM : 0;
    if (!e) e = put_number(info,CFSTR("ItemID"),image->id);
    if (!e) e = put_number(info,CFSTR("Width"),image->width);
    if (!e) e = put_number(info,CFSTR("Height"),image->height);
    for (unsigned n = 0; n < image->property_count && !e; n++) {
        const struct x9_heif_property *p = &image->properties[n];
        CFNumberRef type = number(p->type);
        CFDataRef raw = CFDataCreate(NULL,data+p->payload.offset,(CFIndex)p->payload.bytes);
        CFDictionaryRef record = NULL;
        if (!type || !raw) e = ENOMEM;
        if (!e) {
            const void *keys[] = { CFSTR("Type"),CFSTR("Essential"),CFSTR("Data") };
            const void *values[] = { type,p->essential ? kCFBooleanTrue : kCFBooleanFalse,raw };
            record = CFDictionaryCreate(NULL,keys,values,3,
                &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            if (!record) e = ENOMEM;
        }
        if (!e) { CFArrayAppendValue(properties,record); e = color_extension(data,p,extensions,&color_seen); }
        if (record) CFRelease(record);
        if (type) CFRelease(type);
        if (raw) CFRelease(raw);
    }
    for (unsigned n = 0; n < X9_HEIF_MAX_ITEMS && !e; n++) {
        uint32_t id = 0;
        e = x9_heif_auxiliary(document,image->id,n,&id);
        if (e == ENOENT) { e = 0; break; }
        if (!e) {
            CFNumberRef v = number(id);
            if (!v) e = ENOMEM;
            else { CFArrayAppendValue(aux,v); CFRelease(v); }
        }
    }
    if (!e) {
        CFDictionarySetValue(info,CFSTR("Properties"),properties);
        CFDictionarySetValue(info,CFSTR("AuxiliaryIDs"),aux);
        *out = info; info = NULL;
    }
    if (info) CFRelease(info);
    if (properties) CFRelease(properties);
    if (aux) CFRelease(aux);
    return e;
}
OSStatus X9HEIFCopyItemSample(CFDataRef file, uint32_t id,
    CMSampleBufferRef *out, CFDictionaryRef *info_out)
{
    if (!out || !info_out || (void *)out == (void *)info_out) return kVTParameterErr;
    *out = NULL; *info_out = NULL;
    if (!file || CFGetTypeID(file) != CFDataGetTypeID() || CFDataGetLength(file) <= 0) return kVTParameterErr;
    const uint8_t *data = CFDataGetBytePtr(file);
    struct x9_heif_document *document = NULL;
    struct x9_heif_image *image = calloc(1,sizeof(*image));
    CFDictionaryRef info = NULL;
    CFMutableDictionaryRef extensions = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef format = NULL;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    uint8_t *au = NULL;
    struct x9_hevc_config config;
    size_t bytes = 0, needed = 0;
    int e = !image || !extensions ? ENOMEM : x9_heif_open(data,(size_t)CFDataGetLength(file),&document);
    if (!e && !id) e = x9_heif_primary(document,&id);
    if (!e) e = x9_heif_get_image(document,id,image);
    if (!e && image->type != X9_HEIF_HVC1) e = ENOTSUP;
    if (!e) e = x9_hevc_config_parse(data+image->hvcc.offset,(size_t)image->hvcc.bytes,&config);
    if (!e && (image->width != config.width || image->height != config.height)) e = EBADMSG;
    if (!e && image->channels) {
        if (image->channels != 3) e = ENOTSUP;
        for (unsigned n = 0; n < image->channels && !e; n++)
            if (image->bit_depth[n] != config.bit_depth) e = EBADMSG;
    }
    if (!e) e = x9_heif_read_item(document,id,NULL,0,&bytes);
    if (!e) { au = malloc(bytes); if (!au) e = ENOMEM; }
    if (!e) e = x9_heif_read_item(document,id,au,bytes,&bytes);
    if (!e) e = x9_hevc_idr_annexb(&config,data+image->hvcc.offset,au,bytes,NULL,0,&needed);
    if (!e) e = item_info(document,data,image,&info,extensions);
    OSStatus result = parse_error(e);
    if (e) goto done;
    const uint8_t *ps[X9_HEVC_MAX_PARAMETER_SETS];
    size_t sizes[X9_HEVC_MAX_PARAMETER_SETS];
    for (unsigned n = 0; n < config.parameter_set_count; n++) {
        ps[n] = data+image->hvcc.offset+config.parameter_sets[n].offset;
        sizes[n] = config.parameter_sets[n].bytes;
    }
    result = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL,config.parameter_set_count,
        ps,sizes,(int)config.length_size,extensions,&format);
    if (result) goto done;
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    if (dimensions.width != (int32_t)config.width || dimensions.height != (int32_t)config.height) {
        result = kVTVideoDecoderBadDataErr; goto done;
    }
    result = parse_error(format_catalog(format,data+image->hvcc.offset,&config));
    if (result) goto done;
    result = CMBlockBufferCreateWithMemoryBlock(NULL,NULL,bytes,NULL,NULL,0,bytes,0,&block);
    if (!result) result = CMBlockBufferReplaceDataBytes(au,block,0,bytes);
    if (result) goto done;
    CMSampleTimingInfo timing = { .duration = kCMTimeInvalid, .presentationTimeStamp = kCMTimeZero,
        .decodeTimeStamp = kCMTimeInvalid };
    result = CMSampleBufferCreateReady(NULL,block,format,1,1,&timing,1,&bytes,&sample);
    if (!result) { *out = sample; sample = NULL; *info_out = info; info = NULL; }
done:
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    if (format) CFRelease(format);
    if (extensions) CFRelease(extensions);
    if (info) CFRelease(info);
    x9_heif_close(&document); free(au); free(image);
    return result;
}
