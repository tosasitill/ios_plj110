#include "heif_metadata.h"
#include "vt_image_metadata.h"
#include <CoreVideo/CoreVideo.h>
#include <errno.h>
#include <string.h>
static CFNumberRef number(uint32_t n)
{ int64_t value = n; return CFNumberCreate(NULL,kCFNumberSInt64Type,&value); }
static int put_number(CFMutableDictionaryRef d, CFStringRef key, uint32_t n)
{
    CFNumberRef v = number(n);
    if (!v) return ENOMEM;
    CFDictionarySetValue(d,key,v); CFRelease(v); return 0;
}
static uint32_t code16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t code32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int aspect_extension(const struct x9_heif_document *document,
    const struct x9_heif_property *p, CFMutableDictionaryRef info,
    CFMutableDictionaryRef extensions, uint32_t *seen)
{
    if (p->type != X9_HEIF_TYPE('p','a','s','p')) return 0;
    if (*seen) return EEXIST;
    if (p->payload.bytes != 8) return EBADMSG;
    const uint8_t *raw = NULL;
    int e = x9_heif_span_data(document,p->payload,&raw);
    if (e) return e;
    uint32_t h = code32(raw), v = code32(raw+4);
    if (!h || !v) return EBADMSG;
    CFDictionaryRef aspect = NULL;
    e = x9_hevc_copy_pixel_aspect(h,v,&aspect);
    if (!e) {
        CFDictionarySetValue(info,kCMFormatDescriptionExtension_PixelAspectRatio,aspect);
        if (extensions) CFDictionarySetValue(extensions,kCMFormatDescriptionExtension_PixelAspectRatio,aspect);
        *seen = 1;
    }
    if (aspect) CFRelease(aspect);
    return e;
}
static int put_cicp(CFMutableDictionaryRef d, const uint32_t codes[3])
{
    CFStringRef raw_keys[] = { CFSTR("X9ColorPrimariesCodePoint"),CFSTR("X9TransferCodePoint"),CFSTR("X9MatrixCodePoint") };
    CFStringRef keys[] = { kCMFormatDescriptionExtension_ColorPrimaries,
        kCMFormatDescriptionExtension_TransferFunction,kCMFormatDescriptionExtension_YCbCrMatrix };
    CFStringRef values[] = { codes[0] == 2 ? NULL : CVColorPrimariesGetStringForIntegerCodePoint((int)codes[0]),
        codes[1] == 2 ? NULL : CVTransferFunctionGetStringForIntegerCodePoint((int)codes[1]),
        codes[2] == 2 ? NULL : CVYCbCrMatrixGetStringForIntegerCodePoint((int)codes[2]) };
    int e = 0;
    for (unsigned n = 0; n < 3 && !e; n++) {
        e = put_number(d,raw_keys[n],codes[n]);
        CFDictionaryRemoveValue(d,keys[n]);
        if (values[n]) CFDictionarySetValue(d,keys[n],values[n]);
    }
    CFDictionaryRemoveValue(d,kCMFormatDescriptionExtension_GammaLevel);
    if (!e && (codes[1] == 4 || codes[1] == 5)) {
        /* CICP4/5 need the actual gamma, not just UseGamma. */
        double gamma = codes[1] == 4 ? 2.2 : 2.8;
        CFNumberRef level = CFNumberCreate(NULL,kCFNumberDoubleType,&gamma);
        if (!level) return ENOMEM;
        CFDictionarySetValue(d,keys[1],kCVImageBufferTransferFunction_UseGamma);
        CFDictionarySetValue(d,kCMFormatDescriptionExtension_GammaLevel,level);
        CFRelease(level);
    }
    return e;
}
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
        const uint32_t codes[] = { code16(raw+4),code16(raw+6),code16(raw+8) };
        e = put_cicp(extensions,codes);
        if (e) return e;
        CFDictionarySetValue(extensions,kCMFormatDescriptionExtension_FullRangeVideo,
            raw[10] & 0x80 ? kCFBooleanTrue : kCFBooleanFalse);
        *seen |= 2u;
    }
    return 0;
}
/* ISO BMFF mdcv/clli/amve are ordinary boxes, not FullBoxes. Their unchanged
 * 24/4/8-byte big-endian payloads use the same layout as CM/CV's HEVC SEI data.
 * Keep these item-local: they are not colr, an inherited graph color policy,
 * a claim about output mastering, or a request for a guessed tone mapper. */
static int hdr_extension(const struct x9_heif_document *document, const struct x9_heif_property *p,
    CFMutableDictionaryRef info, CFMutableDictionaryRef extensions, uint32_t *seen)
{
    CFStringRef key;
    uint32_t flag, length;
    if (p->type == X9_HEIF_TYPE('m','d','c','v')) {
        key = kCMFormatDescriptionExtension_MasteringDisplayColorVolume; flag = 1; length = 24;
    } else if (p->type == X9_HEIF_TYPE('c','l','l','i')) {
        key = kCMFormatDescriptionExtension_ContentLightLevelInfo; flag = 2; length = 4;
    } else if (p->type == X9_HEIF_TYPE('a','m','v','e')) {
        key = kCMFormatDescriptionExtension_AmbientViewingEnvironment; flag = 4; length = 8;
    } else return 0;
    if (*seen & flag) return EEXIST;
    if (p->payload.bytes != length) return EBADMSG;
    const uint8_t *raw = NULL;
    int e = x9_heif_span_data(document,p->payload,&raw);
    if (e) return e;
    if (flag == 4) {
        /* Also qualify direct metadata callers before creating any CFData;
         * a selected item from get_image has already checked these values. */
        struct x9_hevc_hdr check = { .ambient_present = 1 };
        memcpy(check.ambient,raw,8);
        if (x9_hevc_hdr_validate(&check)) return EBADMSG;
    }
    CFDataRef value = CFDataCreate(NULL,raw,length);
    if (!value) return ENOMEM;
    CFDictionarySetValue(info,key,value);
    if (extensions) CFDictionarySetValue(extensions,key,value);
    CFRelease(value); *seen |= flag; return 0;
}
static int auxiliary_info(const struct x9_heif_document *document,
    const struct x9_heif_auxiliary_info *aux, CFMutableDictionaryRef info)
{
    if (!aux->present) return 0;
    const uint8_t *urn = NULL, *subtype = NULL;
    int e = x9_heif_span_data(document,aux->urn,&urn);
    if (!e && aux->subtype.bytes) e = x9_heif_span_data(document,aux->subtype,&subtype);
    if (e) return e;
    /* Raw bytes are intentional: an unknown URN is not assumed UTF-8 and
     * this metadata key does not grant CoreMedia AuxiliaryTypeInfo semantics.
     * In particular the opaque tail is not passed to the AU165 parser. */
    CFDataRef type = CFDataCreate(NULL,urn,(CFIndex)aux->urn.bytes);
    CFDataRef data = CFDataCreate(NULL,subtype,(CFIndex)aux->subtype.bytes);
    if (!type || !data) e = ENOMEM;
    if (!e) {
        CFDictionarySetValue(info,CFSTR("AuxiliaryTypeURNBytes"),type);
        CFDictionarySetValue(info,CFSTR("AuxiliarySubTypeBytes"),data);
        CFDictionarySetValue(info,CFSTR("AuxiliaryTypeOrigin"),CFSTR("auxC-fullbox0-type-NUL-opaque-tail"));
        e = put_number(info,CFSTR("AuxiliaryTypeClassification"),aux->type);
    }
    if (!e && aux->envelope.recognized) {
        /* This is a raw byte-shape observation only. In particular no
         * normative SEI165/alpha validity, scalar interpretation, CoreMedia
         * AuxiliaryTypeInfo, or hardware/associated-layer grant is emitted. */
        const struct x9_heif_auxiliary_envelope *envelope = &aux->envelope;
        const uint8_t *nal = NULL, *payload = NULL;
        if (aux->type != X9_HEIF_AUXILIARY_HEVC_ALPHA ||
            aux->subtype.bytes < 13 || envelope->nal.offset != aux->subtype.offset+8 ||
            envelope->nal.bytes != aux->subtype.bytes-8 ||
            envelope->outer_bytes != aux->subtype.bytes-4 ||
            envelope->payload.offset < envelope->nal.offset+4 ||
            envelope->payload.offset > aux->subtype.offset+aux->subtype.bytes ||
            envelope->payload.bytes != aux->subtype.offset+aux->subtype.bytes-envelope->payload.offset)
            e = EBADMSG;
        if (!e) e = x9_heif_span_data(document,envelope->nal,&nal);
        if (!e) e = x9_heif_span_data(document,envelope->payload,&payload);
        CFDataRef nal_data = NULL, payload_data = NULL;
        if (!e) {
            nal_data = CFDataCreate(NULL,nal,(CFIndex)envelope->nal.bytes);
            payload_data = CFDataCreate(NULL,payload,(CFIndex)envelope->payload.bytes);
            if (!nal_data || !payload_data) e = ENOMEM;
        }
        if (!e) {
            CFDictionarySetValue(info,CFSTR("AuxiliarySubTypeEnvelopeShape"),
                CFSTR("CMPhoto-observed-inner-plus4-inner-4e01-a5-FFsize-payload"));
            CFDictionarySetValue(info,CFSTR("AuxiliarySubTypeEnvelopeNALBytes"),nal_data);
            CFDictionarySetValue(info,CFSTR("AuxiliarySubTypeEnvelopePayloadBytes"),payload_data);
            e = put_number(info,CFSTR("AuxiliarySubTypeEnvelopeOuterBytes"),envelope->outer_bytes);
        }
        if (nal_data) CFRelease(nal_data);
        if (payload_data) CFRelease(payload_data);
    }
    if (type) CFRelease(type);
    if (data) CFRelease(data);
    return e;
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
    CFMutableArrayRef item_offsets = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    CFMutableArrayRef prem = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    CFMutableArrayRef masters = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    uint32_t color_seen = 0, hdr_seen = 0, aspect_seen = 0;
    int e = !info || !properties || !aux || !derived || !item_offsets || !prem || !masters ? ENOMEM : 0;
    if (!e) e = put_number(info,CFSTR("ItemID"),image->id);
    if (!e) e = put_number(info,CFSTR("Width"),image->width);
    if (!e) e = put_number(info,CFSTR("Height"),image->height);
    if (!e) e = put_number(info,CFSTR("ConstructionMethod"),image->construction_method);
    if (!e) e = put_number(info,CFSTR("PremultipliedReferenceCount"),image->premultiplied_reference_count);
    if (!e) e = auxiliary_info(document,&image->auxiliary,info);
    if (!e && image->type == X9_HEIF_IOVL) {
        CFMutableArrayRef fill = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
        CFMutableArrayRef offsets = CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
        if (!fill || !offsets) e = ENOMEM;
        for (unsigned n = 0; n < 4 && !e; n++) {
            CFNumberRef v = number(image->canvas_fill_rgba[n]);
            if (!v) e = ENOMEM; else { CFArrayAppendValue(fill,v); CFRelease(v); }
        }
        for (unsigned n = 0; n < image->derived_count && !e; n++) {
            int64_t x = image->overlay_x[n], y = image->overlay_y[n];
            CFNumberRef nx = CFNumberCreate(NULL,kCFNumberSInt64Type,&x);
            CFNumberRef ny = CFNumberCreate(NULL,kCFNumberSInt64Type,&y);
            if (!nx || !ny) e = ENOMEM;
            else {
                const void *values[] = {nx,ny};
                CFArrayRef pair = CFArrayCreate(NULL,values,2,&kCFTypeArrayCallBacks);
                if (!pair) e = ENOMEM; else { CFArrayAppendValue(offsets,pair); CFRelease(pair); }
            }
            if (nx) CFRelease(nx); if (ny) CFRelease(ny);
        }
        if (!e) { CFDictionarySetValue(info,CFSTR("CanvasFillRGBA16"),fill);
            CFDictionarySetValue(info,CFSTR("OverlayOffsets"),offsets); }
        if (fill) CFRelease(fill); if (offsets) CFRelease(offsets);
    }
    if (!e) CFDictionarySetValue(info,CFSTR("LocationPresent"),image->location_present ? kCFBooleanTrue : kCFBooleanFalse);
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
        if (!e) e = hdr_extension(document,p,info,extensions,&hdr_seen);
        if (!e) e = aspect_extension(document,p,info,extensions,&aspect_seen);
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
    for (unsigned n = 0; n < X9_HEIF_MAX_REFERENCES && !e; n++) {
        uint32_t id = 0;
        e = x9_heif_item_offset_reference(document,image->id,n,&id);
        if (e == ENOENT) { e = 0; break; }
        if (!e) {
            CFNumberRef value = number(id);
            if (!value) e = ENOMEM;
            else { CFArrayAppendValue(item_offsets,value); CFRelease(value); }
        }
    }
    for (unsigned n = 0; n < image->premultiplied_reference_count && !e; n++) {
        uint32_t id = 0;
        e = x9_heif_premultiplied_reference(document,image->id,n,&id);
        if (!e) {
            CFNumberRef value = number(id);
            if (!value) e = ENOMEM; else { CFArrayAppendValue(prem,value); CFRelease(value); }
        }
    }
    for (unsigned n = 0; n < X9_HEIF_MAX_REFERENCES && !e; n++) {
        uint32_t id = 0;
        e = x9_heif_auxiliary_reference(document,image->id,n,&id);
        if (e == ENOENT) { e = 0; break; }
        if (!e) {
            CFNumberRef value = number(id);
            if (!value) e = ENOMEM;
            else { CFArrayAppendValue(masters,value); CFRelease(value); }
        }
    }
    if (!e) {
        CFDictionarySetValue(info,CFSTR("DerivedIDs"),derived);
        CFDictionarySetValue(info,CFSTR("Properties"),properties);
        CFDictionarySetValue(info,CFSTR("AuxiliaryIDs"),aux);
        CFDictionarySetValue(info,CFSTR("ItemOffsetIDs"),item_offsets);
        CFDictionarySetValue(info,CFSTR("PremultipliedReferenceIDs"),prem);
        CFDictionarySetValue(info,CFSTR("AuxiliaryMasterIDs"),masters);
        *out = info; info = NULL;
    }
    if (info) CFRelease(info);
    if (properties) CFRelease(properties);
    if (aux) CFRelease(aux);
    if (derived) CFRelease(derived);
    if (item_offsets) CFRelease(item_offsets);
    if (prem) CFRelease(prem);
    if (masters) CFRelease(masters);
    return e;
}
int x9_heif_resolve_hdr_extensions(CFMutableDictionaryRef extensions,
    const struct x9_hevc_hdr *hdr)
{
    if (!extensions || CFGetTypeID(extensions) != CFDictionaryGetTypeID() || !hdr ||
        x9_hevc_hdr_validate(hdr)) return EINVAL;
    CFStringRef keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo,
        kCMFormatDescriptionExtension_AmbientViewingEnvironment };
    const uint8_t *bytes[] = {hdr->mastering,hdr->light,hdr->ambient};
    uint32_t present[] = {hdr->mastering_present,hdr->light_present,hdr->ambient_present};
    const CFIndex lengths[] = {24,4,8};
    CFDataRef add[3] = {NULL,NULL,NULL};
    int e = 0;
    /* Validate conflicts and allocate all values before changing the caller's
     * extension dictionary. Container/SEI duplication must agree byte-for-byte;
     * neither source silently wins. No defaults for an absent message. */
    for (unsigned n = 0; n < 3 && !e; n++) {
        CFTypeRef value = CFDictionaryGetValue(extensions,keys[n]);
        if (value && (CFGetTypeID(value) != CFDataGetTypeID() ||
            CFDataGetLength(value) != lengths[n])) e = EBADMSG;
        if (!e && value && n == 2) {
            struct x9_hevc_hdr check = { .ambient_present = 1 };
            memcpy(check.ambient,CFDataGetBytePtr(value),8);
            if (x9_hevc_hdr_validate(&check)) e = EBADMSG;
        }
        if (!e && present[n]) {
            if (value && memcmp(CFDataGetBytePtr(value),bytes[n],(size_t)lengths[n])) e = EBADMSG;
            else if (!value) { add[n] = CFDataCreate(NULL,bytes[n],lengths[n]); if (!add[n]) e = ENOMEM; }
        }
    }
    if (!e) e = x9_hevc_merge_preferred_transfer(extensions,hdr);
    if (!e) for (unsigned n = 0; n < 3; n++) if (add[n]) CFDictionarySetValue(extensions,keys[n],add[n]);
    for (unsigned n = 0; n < 3; n++) if (add[n]) CFRelease(add[n]);
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
int x9_heif_resolve_color_extensions(CFDictionaryRef container,
    const struct x9_hevc_vui *v, const struct x9_hevc_hdr *hdr, CFDictionaryRef *out)
{
    if (!out) return EINVAL;
    *out = NULL;
    if (!container || CFGetTypeID(container) != CFDictionaryGetTypeID() || !v ||
        v->full_range > 1 || v->chroma_top > 5 || v->chroma_bottom > 5 ||
        (hdr && x9_hevc_hdr_validate(hdr))) return EINVAL;
    CFMutableDictionaryRef d = CFDictionaryCreateMutableCopy(NULL,0,container);
    if (!d) return ENOMEM;
    /* D.3.38 replaces the SPS fallback, not this API's explicit container
     * non-2 policy. Keep the caller's original VUI unchanged for provenance. */
    uint32_t codes[] = { v->colour_primaries,
        hdr && hdr->preferred_present ? hdr->preferred_transfer : v->transfer,v->matrix };
    CFStringRef raw_keys[] = { CFSTR("X9ColorPrimariesCodePoint"),CFSTR("X9TransferCodePoint"),CFSTR("X9MatrixCodePoint") };
    int e = 0;
    for (unsigned n = 0; n < 3 && !e; n++) {
        CFTypeRef value = CFDictionaryGetValue(container,raw_keys[n]);
        if (!value) continue;
        int64_t code = 0;
        if (CFGetTypeID(value) != CFNumberGetTypeID() || CFNumberIsFloatType(value) ||
            !CFNumberGetValue(value,kCFNumberSInt64Type,&code) || code < 0 || code > UINT16_MAX) e = EBADMSG;
        else if (code != 2) codes[n] = (uint32_t)code;
    }
    if (!e) e = put_cicp(d,codes);
    if (!e && hdr) e = x9_hevc_merge_preferred_transfer(d,hdr);
    CFTypeRef full = CFDictionaryGetValue(container,kCMFormatDescriptionExtension_FullRangeVideo);
    if (!e && full && CFGetTypeID(full) != CFBooleanGetTypeID()) e = EBADMSG;
    if (!e) {
        if (!full) full = v->full_range ? kCFBooleanTrue : kCFBooleanFalse;
        CFDictionarySetValue(d,kCMFormatDescriptionExtension_FullRangeVideo,full);
        CFStringRef locations[] = { kCVImageBufferChromaLocation_Left,kCVImageBufferChromaLocation_Center,
            kCVImageBufferChromaLocation_TopLeft,kCVImageBufferChromaLocation_Top,
            kCVImageBufferChromaLocation_BottomLeft,kCVImageBufferChromaLocation_Bottom };
        CFDictionarySetValue(d,kCMFormatDescriptionExtension_ChromaLocationTopField,locations[v->chroma_top]);
        CFDictionarySetValue(d,kCMFormatDescriptionExtension_ChromaLocationBottomField,locations[v->chroma_bottom]);
    }
    if (e) CFRelease(d); else *out = d;
    return e;
}
int x9_heif_copy_vui_info(const struct x9_hevc_vui *v, CFDictionaryRef *out)
{
    if (!v || !out) return EINVAL;
    *out = NULL;
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!d) return ENOMEM;
    int e = 0;
#define FIELD(key, member) do { if (!e) e = put_number(d,CFSTR(key),v->member); } while (0)
    FIELD("Present",present); FIELD("PPSID",pps_id); FIELD("SPSID",sps_id); FIELD("VPSID",vps_id);
    FIELD("AspectRatioPresent",aspect_ratio_present); FIELD("AspectRatioIDC",aspect_ratio_idc);
    FIELD("SARWidth",sar_width); FIELD("SARHeight",sar_height);
    FIELD("OverscanPresent",overscan_present); FIELD("OverscanAppropriate",overscan_appropriate);
    FIELD("VideoSignalPresent",video_signal_present); FIELD("VideoFormat",video_format);
    FIELD("FullRange",full_range); FIELD("ColourDescriptionPresent",colour_description_present);
    FIELD("ColorPrimariesCodePoint",colour_primaries); FIELD("TransferCodePoint",transfer); FIELD("MatrixCodePoint",matrix);
    FIELD("ChromaLocationPresent",chroma_location_present); FIELD("ChromaTop",chroma_top); FIELD("ChromaBottom",chroma_bottom);
    FIELD("NeutralChroma",neutral_chroma); FIELD("FieldSequence",field_seq); FIELD("FrameFieldInfoPresent",frame_field_info_present);
    FIELD("DisplayWindowPresent",display_window_present);
    FIELD("DisplayWindowLeft",display_window[0]); FIELD("DisplayWindowRight",display_window[1]);
    FIELD("DisplayWindowTop",display_window[2]); FIELD("DisplayWindowBottom",display_window[3]);
    FIELD("TimingPresent",timing_present); FIELD("NumUnitsInTick",num_units_in_tick); FIELD("TimeScale",time_scale);
    FIELD("POCProportional",poc_proportional); FIELD("TicksPOCDiffOneMinus1",ticks_poc_diff_one_minus1); FIELD("HRDPresent",hrd_present);
    FIELD("RestrictionPresent",restriction_present); FIELD("TilesFixed",tiles_fixed);
    FIELD("MotionOverBoundaries",motion_over_boundaries); FIELD("RestrictedRefLists",restricted_ref_lists);
    FIELD("MinSpatialSegmentation",min_spatial_segmentation); FIELD("MaxBytesPerPicDenom",max_bytes_per_pic_denom);
    FIELD("MaxBitsPerMinCUDenom",max_bits_per_min_cu_denom);
    FIELD("Log2MaxMVHorizontal",log2_max_mv_horizontal); FIELD("Log2MaxMVVertical",log2_max_mv_vertical);
#undef FIELD
    if (e) CFRelease(d); else *out = d;
    return e;
}
