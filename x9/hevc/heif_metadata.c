#include "heif_metadata.h"
#include <CoreVideo/CoreVideo.h>
#include <errno.h>
static CFNumberRef number(uint32_t n)
{ int64_t value = n; return CFNumberCreate(NULL,kCFNumberSInt64Type,&value); }
static int put_number(CFMutableDictionaryRef d, CFStringRef key, uint32_t n)
{
    CFNumberRef v = number(n);
    if (!v) return ENOMEM;
    CFDictionarySetValue(d,key,v); CFRelease(v); return 0;
}
static uint32_t code16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
/* Keep all raw colr properties in ordered item info. Standard CICP strings
 * use the SDK's exact code-point mapping, not a guessed BT.709 default. ICC
 * and nclx can coexist; this bridge does not choose their ColorSync precedence.
 * Unknown CICP values remain in raw metadata and are not invented strings. */
static int color_extension(const struct x9_heif_document *document, const struct x9_heif_property *p,
    CFMutableDictionaryRef extensions, uint32_t *seen)
{
    if (p->type != X9_HEIF_TYPE('c','o','l','r')) return 0;
    const uint8_t *raw = NULL;
    int e = x9_heif_span_data(document,p->payload,&raw);
    if (e) return e;
    if (p->payload.bytes < 4) return EBADMSG;
    uint32_t type = (uint32_t)raw[0] << 24 | (uint32_t)raw[1] << 16 |
                    (uint32_t)raw[2] << 8 | raw[3];
    if (type == X9_HEIF_TYPE('r','I','C','C') || type == X9_HEIF_TYPE('p','r','o','f')) {
        if (p->payload.bytes == 4) return EBADMSG;
        if (*seen & 1u) return EEXIST;
        CFDataRef icc = CFDataCreate(NULL,raw+4,(CFIndex)p->payload.bytes-4);
        if (!icc) return ENOMEM;
        CFDictionarySetValue(extensions,kCMFormatDescriptionExtension_ICCProfile,icc);
        CFRelease(icc); *seen |= 1u;
    } else if (type == X9_HEIF_TYPE('n','c','l','x')) {
        if (p->payload.bytes != 11 || (raw[10] & 0x7fu)) return EBADMSG;
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
int x9_heif_copy_item_info(const struct x9_heif_document *document,
    const struct x9_heif_image *image, CFDictionaryRef *out, CFMutableDictionaryRef extensions)
{
    if (!document || !image || !out) return EINVAL;
    *out = NULL;
    CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef properties = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    CFMutableArrayRef aux = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    CFMutableArrayRef derived = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    uint32_t color_seen = 0;
    int e = !info || !properties || !aux || !derived ? ENOMEM : 0;
    if (!e) e = put_number(info,CFSTR("ItemID"),image->id);
    if (!e) e = put_number(info,CFSTR("Width"),image->width);
    if (!e) e = put_number(info,CFSTR("Height"),image->height);
    for (unsigned n = 0; n < image->property_count && !e; n++) {
        const struct x9_heif_property *p = &image->properties[n];
        CFNumberRef type = number(p->type);
        const uint8_t *bytes = NULL;
        e = x9_heif_span_data(document,p->payload,&bytes);
        if (e) { if (type) CFRelease(type); break; }
        CFDataRef raw = CFDataCreate(NULL,bytes,(CFIndex)p->payload.bytes);
        CFDictionaryRef record = NULL;
        if (!type || !raw) e = ENOMEM;
        if (!e) {
            const void *keys[] = { CFSTR("Type"),CFSTR("Essential"),CFSTR("Data") };
            const void *values[] = { type,p->essential ? kCFBooleanTrue : kCFBooleanFalse,raw };
            record = CFDictionaryCreate(NULL,keys,values,3,
                &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            if (!record) e = ENOMEM;
        }
        if (!e) { CFArrayAppendValue(properties,record); if (extensions) e = color_extension(document,p,extensions,&color_seen); }
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
    for (unsigned n = 0; n < image->derived_count && !e; n++) {
        CFNumberRef id = number(image->derived[n]);
        if (!id) e = ENOMEM;
        else { CFArrayAppendValue(derived,id); CFRelease(id); }
    }
    if (!e) {
        CFDictionarySetValue(info,CFSTR("DerivedIDs"),derived);
        CFDictionarySetValue(info,CFSTR("Properties"),properties);
        CFDictionarySetValue(info,CFSTR("AuxiliaryIDs"),aux);
        *out = info; info = NULL;
    }
    if (info) CFRelease(info);
    if (properties) CFRelease(properties);
    if (aux) CFRelease(aux);
    if (derived) CFRelease(derived);
    return e;
}
int x9_heif_copy_color_extensions(const struct x9_heif_document *document,
    const struct x9_heif_image *image, CFDictionaryRef *out)
{
    if (!document || !image || !out) return EINVAL;
    *out = NULL;
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!d) return ENOMEM;
    uint32_t seen = 0;
    int e = 0;
    for (unsigned n = 0; n < image->property_count && !e; n++)
        e = color_extension(document,&image->properties[n],d,&seen);
    if (e) CFRelease(d); else *out = d;
    return e;
}
