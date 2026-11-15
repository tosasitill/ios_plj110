/* Manual iOS status/explicit VT independent-picture client. No device IO at
 * build time; never registers unless real kernel hardware prerequisites pass.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "hevc_bitstream.h"
#include "heif_items.h"
#include "heif_coremedia.h"
#include "vt_decoder.h"
#include "../kernel/src/bsd/dev/x9_vdec.h"
static uint8_t *read_file(const char *path, size_t limit, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || (size_t)n > limit || fseek(f, 0, SEEK_SET)) { fclose(f); errno = EINVAL; return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); errno = EIO; return NULL; }
    fclose(f); *bytes = (size_t)n;
    return p;
}
struct result { unsigned calls; OSStatus error; CVPixelBufferRef pixel; };
static void output(void *ref, void *source, OSStatus error, VTDecodeInfoFlags flags,
                   CVImageBufferRef image, CMTime pts, CMTime duration)
{
    (void)source; (void)flags; (void)pts; (void)duration;
    struct result *r = ref;
    r->calls++; r->error = error;
    if (image && r->calls == 1) r->pixel = CVPixelBufferRetain(image);
}
static int write_all(int fd, const uint8_t *p, size_t bytes)
{
    while (bytes) {
        ssize_t n = write(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; bytes -= (size_t)n;
    }
    return 0;
}
static int decode_vt(const char *config_path, const char *sample_path, const char *out_path,
                     const char *heif_path, uint32_t item_id)
{
    size_t cfg_bytes = 0, au_bytes = 0;
    uint8_t *cfg = heif_path ? NULL : read_file(config_path, X9_HEVC_MAX_CONFIG, &cfg_bytes);
    uint8_t *au = heif_path ? NULL : read_file(sample_path, X9_HEVC_MAX_INPUT, &au_bytes);
    struct x9_hevc_config c;
    struct result r = {0};
    CMVideoFormatDescriptionRef format = NULL;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    VTDecompressionSessionRef session = NULL;
    CFDictionaryRef spec = NULL;
    CFDictionaryRef item_info = NULL;
    CFTypeRef hw = NULL;
    int fd = -1, rc = 1, created = 0;
    OSStatus e = kVTParameterErr;
    if (heif_path) {
        size_t file_bytes = 0;
        uint8_t *raw = read_file(heif_path,X9_HEIF_MAX_FILE,&file_bytes);
        if (!raw) { perror("HEIF input"); goto done; }
        CFDataRef file = CFDataCreateWithBytesNoCopy(NULL,raw,(CFIndex)file_bytes,kCFAllocatorNull);
        e = file ? X9HEIFCopyItemSample(file,item_id,&sample,&item_info) : kVTAllocationFailedErr;
        if (file) CFRelease(file);
        free(raw); /* ready sample/format/properties now own all input bytes */
        if (e) goto done;
        format = (CMVideoFormatDescriptionRef)CFRetain(CMSampleBufferGetFormatDescription(sample));
        CFDictionaryRef atoms = CMFormatDescriptionGetExtension(format,
            kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
        CFDataRef hvcc = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ?
            CFDictionaryGetValue(atoms,CFSTR("hvcC")) : NULL;
        if (!hvcc || CFGetTypeID(hvcc) != CFDataGetTypeID() || CFDataGetLength(hvcc) <= 0 ||
            CFDataGetLength(hvcc) > X9_HEVC_MAX_CONFIG) { e = kVTVideoDecoderBadDataErr; goto done; }
        cfg_bytes = (size_t)CFDataGetLength(hvcc);
        CMBlockBufferRef input = CMSampleBufferGetDataBuffer(sample);
        au_bytes = input ? CMBlockBufferGetDataLength(input) : 0;
        if (!au_bytes || au_bytes > X9_HEVC_MAX_INPUT) { e = kVTVideoDecoderBadDataErr; goto done; }
        cfg = malloc(cfg_bytes); au = malloc(au_bytes);
        if (!cfg || !au) { e = kVTAllocationFailedErr; goto done; }
        memcpy(cfg,CFDataGetBytePtr(hvcc),cfg_bytes);
        e = CMBlockBufferCopyDataBytes(input,0,au_bytes,au);
        if (e) goto done;
        printf("selected coded HEIF item input; output is that raw tile, NOT a composed HEIF image\n");
    }
    if (!cfg || !au) { perror("input"); goto done; }
    size_t needed = 0;
    if (x9_hevc_config_parse(cfg, cfg_bytes, &c) ||
        x9_hevc_idr_annexb(&c, cfg, au, au_bytes, NULL, 0, &needed)) {
        e = kVTVideoDecoderBadDataErr;
        fprintf(stderr, "unsupported/malformed independent HEVC input (Rext alpha is not Main)\n"); goto done;
    }
    e = X9HEVCRegisterDecoder();
    if (e) { fprintf(stderr, "real hardware decoder unavailable: OSStatus=%d\n", (int)e); goto done; }
    const uint8_t *ps[X9_HEVC_MAX_PARAMETER_SETS];
    size_t sizes[X9_HEVC_MAX_PARAMETER_SETS];
    for (unsigned i = 0; i < c.parameter_set_count; i++) {
        ps[i] = cfg + c.parameter_sets[i].offset;
        sizes[i] = c.parameter_sets[i].bytes;
    }
    if (!format) e = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, c.parameter_set_count, ps, sizes,
                                                           (int)c.length_size, NULL, &format);
    if (e) goto done;
    CFStringRef *enable_key = dlsym(RTLD_DEFAULT,
        "kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder");
    CFStringRef *require_key = dlsym(RTLD_DEFAULT,
        "kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder");
    if (!enable_key || !*enable_key || !require_key || !*require_key) {
        e = kVTPropertyNotSupportedErr; goto done;
    }
    CFStringRef *id_key = dlsym(RTLD_DEFAULT, "kVTVideoDecoderSpecification_DecoderID");
    if (!id_key || !*id_key) { e = kVTPropertyNotSupportedErr; goto done; }
    const void *keys[] = { CFSTR("X9NativeHEVC"), *enable_key, *require_key, *id_key };
    const void *values[] = { kCFBooleanTrue, kCFBooleanTrue, kCFBooleanTrue, CFSTR(X9_HEVC_DECODER_ID) };
    spec = CFDictionaryCreate(NULL, keys, values, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!spec) { e = kVTAllocationFailedErr; goto done; }
    VTDecompressionOutputCallbackRecord callback = { .decompressionOutputCallback = output,
                                                   .decompressionOutputRefCon = &r };
    e = VTDecompressionSessionCreate(NULL, format, spec, NULL, &callback, &session);
    if (e) goto done;
    CFStringRef *hardware_key = dlsym(RTLD_DEFAULT,
        "kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder");
    if (!hardware_key || !*hardware_key) { e = kVTPropertyNotSupportedErr; goto done; }
    e = VTSessionCopyProperty(session, *hardware_key, NULL, &hw);
    if (e || !hw || !CFEqual(hw, kCFBooleanTrue)) { e = kVTVideoDecoderNotAvailableNowErr; goto done; }
    /* Make a CoreMedia-owned copy. No external sample lifetime assumptions. */
    if (!sample) {
        e = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, au_bytes, NULL, NULL, 0, au_bytes, 0, &block);
        if (!e) e = CMBlockBufferReplaceDataBytes(au, block, 0, au_bytes);
        if (e) goto done;
        CMSampleTimingInfo timing = { .duration = CMTimeMake(1, 1), .presentationTimeStamp = kCMTimeZero,
                                      .decodeTimeStamp = kCMTimeInvalid };
        e = CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &au_bytes, &sample);
    }
    if (e) goto done;
    VTDecodeInfoFlags flags = 0;
    e = VTDecompressionSessionDecodeFrame(session, sample, 0, NULL, &flags);
    if (!e) e = VTDecompressionSessionFinishDelayedFrames(session);
    if (!e) e = VTDecompressionSessionWaitForAsynchronousFrames(session);
    if (e || r.error || r.calls != 1 || !r.pixel) { if (!e) e = r.error ? r.error : kVTVideoDecoderBadDataErr; goto done; }
    OSType pixel_format = CVPixelBufferGetPixelFormatType(r.pixel);
    int bpp = pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange ||
              pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? 2 :
              pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
              pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ? 1 : 0;
    if (!bpp || CVPixelBufferGetPlaneCount(r.pixel) != 2 ||
        CVPixelBufferGetWidth(r.pixel) != c.width || CVPixelBufferGetHeight(r.pixel) != c.height) {
        e = kVTVideoDecoderBadDataErr; goto done;
    }
    e = CVPixelBufferLockBaseAddress(r.pixel, kCVPixelBufferLock_ReadOnly);
    if (e) goto done;
    fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    created = fd >= 0;
    if (fd < 0) { perror("output (must be new)"); e = kVTParameterErr; }
    for (unsigned plane = 0; plane < 2 && !e; plane++) {
        const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(r.pixel, plane);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(r.pixel, plane), row = c.width * (unsigned)bpp;
        if (!base || stride < row) { e = kVTVideoDecoderBadDataErr; break; }
        for (unsigned y = 0; y < (c.height >> plane) && !e; y++)
            if (write_all(fd, base + (size_t)y * stride, row)) e = kVTParameterErr;
    }
    OSStatus unlock = CVPixelBufferUnlockBaseAddress(r.pixel, kCVPixelBufferLock_ReadOnly);
    if (!e) e = unlock;
    if (!e && fsync(fd)) e = kVTParameterErr;
    if (fd >= 0) { if (close(fd) && !e) e = kVTParameterErr; fd = -1; }
    if (e) { if (created) unlink(out_path); goto done; }
    printf("VT hardware picture: %ux%u depth=%u pixel=0x%08x callbacks=%u output=%s\n",
           c.width, c.height, c.bit_depth, (unsigned)pixel_format, r.calls, out_path);
    rc = 0;
done:
    if (rc) fprintf(stderr, "decode failed: OSStatus=%d (no hardware success claimed)\n", (int)e);
    if (session) { VTDecompressionSessionInvalidate(session); CFRelease(session); }
    if (r.pixel) CVPixelBufferRelease(r.pixel);
    if (hw) CFRelease(hw);
    if (spec) CFRelease(spec);
    if (item_info) CFRelease(item_info);
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    if (format) CFRelease(format);
    if (fd >= 0) close(fd);
    free(cfg); free(au);
    return rc;
}
struct item_walk { uint32_t stack[32], seen[X9_HEIF_MAX_ITEMS], seen_count; };
static int print_heif_item(const struct x9_heif_document *d, uint32_t id,
                          unsigned depth, struct item_walk *walk)
{
    if (depth == 32) return E2BIG;
    for (unsigned n = 0; n < depth; n++) if (walk->stack[n] == id) return ELOOP;
    for (unsigned n = 0; n < walk->seen_count; n++) if (walk->seen[n] == id) return 0;
    if (walk->seen_count == X9_HEIF_MAX_ITEMS) return ENOSPC;
    struct x9_heif_image *image = calloc(1,sizeof(*image));
    if (!image) return ENOMEM;
    int e = x9_heif_get_image(d,id,image);
    if (e) { free(image); return e; }
    walk->stack[depth] = id; walk->seen[walk->seen_count++] = id;
    printf("item=%u type=%c%c%c%c size=%ux%u channels=%u hidden=%u bytes=%llu grid=%ux%u children=%u\n",
        id,(char)(image->type>>24),(char)(image->type>>16),(char)(image->type>>8),(char)image->type,
        image->width,image->height,image->channels,image->hidden,(unsigned long long)image->item_bytes,
        image->rows,image->columns,image->derived_count);
    for (unsigned n = 0; n < image->property_count; n++) {
        const struct x9_heif_property *p = &image->properties[n];
        printf("  property[%u]=%c%c%c%c essential=%u offset=%llu bytes=%llu (not applied)\n",
            n,(char)(p->type>>24),(char)(p->type>>16),(char)(p->type>>8),(char)p->type,p->essential,
            (unsigned long long)p->payload.offset,(unsigned long long)p->payload.bytes);
    }
    for (unsigned n = 0; n < image->derived_count && !e; n++) {
        printf("  dimg[%u]=%u\n",n,image->derived[n]);
        e = print_heif_item(d,image->derived[n],depth+1,walk);
    }
    for (unsigned n = 0; n < X9_HEIF_MAX_ITEMS && !e; n++) {
        uint32_t auxiliary = 0;
        e = x9_heif_auxiliary(d,id,n,&auxiliary);
        if (e == ENOENT) { e = 0; break; }
        if (!e) { printf("  auxiliary[%u]=%u\n",n,auxiliary); e = print_heif_item(d,auxiliary,depth+1,walk); }
    }
    free(image); return e;
}
static int heif_info(const char *path)
{
    size_t bytes = 0;
    uint8_t *data = read_file(path,X9_HEIF_MAX_FILE,&bytes);
    if (!data) { perror("HEIF input"); return 1; }
    struct x9_heif_document *d = NULL;
    uint32_t primary = 0;
    struct item_walk walk = {0};
    int e = x9_heif_open(data,bytes,&d);
    if (!e) e = x9_heif_primary(d,&primary);
    if (!e) e = print_heif_item(d,primary,0,&walk);
    if (e) fprintf(stderr,"HEIF item input rejected: errno=%d\n",e);
    else puts("HEIF input graph only; no decode, transform, alpha merge or hardware success claimed");
    x9_heif_close(&d); free(data); return e ? 1 : 0;
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "decode-vt")) return decode_vt(argv[2], argv[3], argv[4],NULL,0);
    if (argc == 3 && !strcmp(argv[1],"heif-info")) return heif_info(argv[2]);
    if (argc == 5 && !strcmp(argv[1],"decode-heif-item")) {
        char *end = NULL;
        errno = 0; unsigned long id = strtoul(argv[3],&end,0);
        if (errno || !argv[3][0] || !end || *end || id > UINT32_MAX) return 2;
        return decode_vt(NULL,NULL,argv[4],argv[2],(uint32_t)id);
    }
    if (argc != 2 || strcmp(argv[1], "status")) {
        fprintf(stderr, "usage: %s status | decode-vt hvcC.bin access-unit.bin NEW.yuv | "
            "heif-info file.heic | decode-heif-item file.heic item-id-or-0 NEW.yuv\n", argv[0]); return 2;
    }
    int fd = open(X9_VDEC_DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror(X9_VDEC_DEVICE); return 1; }
    struct x9_vdec_status s = {0};
    int rc = ioctl(fd, X9_VDEC_GET_STATUS, &s); close(fd);
    if (rc) { perror("X9_VDEC_GET_STATUS"); return 1; }
    if (s.version != X9_VDEC_VERSION) { fprintf(stderr, "unsupported kernel ABI\n"); return 1; }
    printf("version=%u mode=%u ready=%u prerequisites=0x%x/0x%x profiles=0x%x max=%ux%u "
           "generation=%llu requests=%llu completed=%llu rejected=%llu errno=%u\n",
           s.version, s.mode, s.ready, s.prerequisites, X9_VDEC_REQUIRED, s.profiles,
           s.max_width, s.max_height, (unsigned long long)s.generation,
           (unsigned long long)s.requests, (unsigned long long)s.completed,
           (unsigned long long)s.rejected, s.last_errno);
    return 0;
}
