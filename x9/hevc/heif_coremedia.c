#include "heif_coremedia.h"
#include "heif_items.h"
#include "heif_metadata.h"
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
static int format_hdr(CMVideoFormatDescriptionRef format, CFDictionaryRef requested)
{
    CFStringRef keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
        kCMFormatDescriptionExtension_ContentLightLevelInfo };
    const CFIndex lengths[] = {24,4};
    for (unsigned n = 0; n < 2; n++) {
        CFTypeRef expected = CFDictionaryGetValue(requested,keys[n]);
        CFTypeRef actual = CMFormatDescriptionGetExtension(format,keys[n]);
        if (!expected && !actual) continue;
        if (!expected || !actual || CFGetTypeID(expected) != CFDataGetTypeID() ||
            CFGetTypeID(actual) != CFDataGetTypeID() || CFDataGetLength(expected) != lengths[n] ||
            CFDataGetLength(actual) != lengths[n] ||
            !CFEqual(expected,actual)) return EBADMSG;
    }
    return 0;
}
OSStatus x9_heif_copy_item_sample(CFDataRef file, uint32_t id,
    CMSampleBufferRef *out, CFDictionaryRef *info_out, struct x9_hevc_vui *vui_out)
{
    if (!out || !info_out || (void *)out == (void *)info_out) return kVTParameterErr;
    *out = NULL; *info_out = NULL;
    if (vui_out) memset(vui_out,0,sizeof(*vui_out));
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
    struct x9_hevc_vui vui;
    struct x9_hevc_hdr hdr;
    struct x9_hevc_irap_picture picture;
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
    if (!e) e = x9_hevc_irap_annexb(&config,data+image->hvcc.offset,au,bytes,NULL,0,&needed,&picture);
    if (!e) e = x9_hevc_sample_vui(&config,data+image->hvcc.offset,(size_t)image->hvcc.bytes,au,bytes,&vui);
    if (!e) e = x9_hevc_sample_hdr(&config,data+image->hvcc.offset,(size_t)image->hvcc.bytes,au,bytes,&hdr);
    if (!e) e = x9_heif_copy_item_info(document,image,&info,extensions);
    if (!e) e = x9_heif_resolve_hdr_extensions(extensions,&hdr);
    CFDictionaryRef resolved = NULL, vui_info = NULL;
    if (!e) e = x9_heif_resolve_color_extensions(extensions,&vui,&resolved);
    if (!e) e = x9_heif_copy_vui_info(&vui,&vui_info);
    if (!e) {
        CFMutableDictionaryRef record = CFDictionaryCreateMutableCopy(NULL,0,info);
        CFMutableDictionaryRef new_extensions = CFDictionaryCreateMutableCopy(NULL,0,resolved);
        CFMutableDictionaryRef hdr_info = CFDictionaryCreateMutable(NULL,0,
            &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        int32_t nal_type = (int32_t)picture.nal_type;
        CFNumberRef type = CFNumberCreate(NULL,kCFNumberSInt32Type,&nal_type);
        int32_t sources[2] = { (int32_t)hdr.mastering_sources, (int32_t)hdr.light_sources };
        CFNumberRef source_values[2] = { CFNumberCreate(NULL,kCFNumberSInt32Type,&sources[0]),
            CFNumberCreate(NULL,kCFNumberSInt32Type,&sources[1]) };
        if (!record || !new_extensions || !type || !hdr_info || !source_values[0] || !source_values[1]) {
            if (record) CFRelease(record);
            if (new_extensions) CFRelease(new_extensions);
            e = ENOMEM;
        } else {
            CFStringRef hdr_keys[] = { kCMFormatDescriptionExtension_MasteringDisplayColorVolume,
                kCMFormatDescriptionExtension_ContentLightLevelInfo };
            for (unsigned n = 0; n < 2; n++) {
                CFTypeRef value = CFDictionaryGetValue(resolved,hdr_keys[n]);
                if (value) CFDictionarySetValue(hdr_info,hdr_keys[n],value);
            }
            CFDictionarySetValue(record,CFSTR("BitstreamVUI"),vui_info);
            CFDictionarySetValue(record,CFSTR("ResolvedColr"),resolved);
            CFDictionarySetValue(record,CFSTR("ResolvedHDRMetadata"),hdr_info);
            CFDictionarySetValue(record,CFSTR("SEIMasteringDisplayPresent"),hdr.mastering_present ? kCFBooleanTrue : kCFBooleanFalse);
            CFDictionarySetValue(record,CFSTR("SEIContentLightPresent"),hdr.light_present ? kCFBooleanTrue : kCFBooleanFalse);
            CFDictionarySetValue(record,CFSTR("SEIMasteringDisplaySources"),source_values[0]);
            CFDictionarySetValue(record,CFSTR("SEIContentLightSources"),source_values[1]);
            CFDictionarySetValue(record,CFSTR("IRAPNALUnitType"),type);
            CFDictionarySetValue(record,CFSTR("NoOutputOfPriorPicsFlag"),
                picture.no_output_of_prior_pics_flag ? kCFBooleanTrue : kCFBooleanFalse);
            CFRelease(info); info = record;
            CFRelease(extensions); extensions = new_extensions;
        }
        if (hdr_info) CFRelease(hdr_info);
        if (type) CFRelease(type);
        for (unsigned n = 0; n < 2; n++) if (source_values[n]) CFRelease(source_values[n]);
    }
    if (resolved) CFRelease(resolved);
    if (vui_info) CFRelease(vui_info);
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
    if (!result) result = parse_error(format_hdr(format,extensions));
    if (result) goto done;
    result = CMBlockBufferCreateWithMemoryBlock(NULL,NULL,bytes,NULL,NULL,0,bytes,0,&block);
    if (!result) result = CMBlockBufferReplaceDataBytes(au,block,0,bytes);
    if (result) goto done;
    CMSampleTimingInfo timing = { .duration = kCMTimeInvalid, .presentationTimeStamp = kCMTimeZero,
        .decodeTimeStamp = kCMTimeInvalid };
    result = CMSampleBufferCreateReady(NULL,block,format,1,1,&timing,1,&bytes,&sample);
    if (!result) {
        *out = sample; sample = NULL; *info_out = info; info = NULL;
        if (vui_out) *vui_out = vui;
    }
done:
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    if (format) CFRelease(format);
    if (extensions) CFRelease(extensions);
    if (info) CFRelease(info);
    x9_heif_close(&document); free(au); free(image);
    return result;
}
OSStatus X9HEIFCopyItemSample(CFDataRef file, uint32_t id,
    CMSampleBufferRef *out, CFDictionaryRef *info_out)
{ return x9_heif_copy_item_sample(file,id,out,info_out,NULL); }
