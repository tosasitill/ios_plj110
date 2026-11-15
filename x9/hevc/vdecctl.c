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
#include "heif_compositor.h"
#include "heif_native.h"
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
    CVPixelBufferRef pixel = NULL;
    CMVideoFormatDescriptionRef format = NULL;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    CFDictionaryRef item_info = NULL;
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
        x9_hevc_irap_annexb(&c, cfg, au, au_bytes, NULL, 0, &needed, NULL)) {
        e = kVTVideoDecoderBadDataErr;
        fprintf(stderr, "unsupported/malformed standalone HEVC IRAP (Rext alpha is not Main; Main Still Picture requires IDR)\n"); goto done;
    }
    const uint8_t *ps[X9_HEVC_MAX_PARAMETER_SETS];
    size_t sizes[X9_HEVC_MAX_PARAMETER_SETS];
    for (unsigned i = 0; i < c.parameter_set_count; i++) {
        ps[i] = cfg + c.parameter_sets[i].offset;
        sizes[i] = c.parameter_sets[i].bytes;
    }
    if (!format) e = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, c.parameter_set_count, ps, sizes,
                                                           (int)c.length_size, NULL, &format);
    if (e) goto done;
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
    e = X9HEVCCopyHardwarePicture(sample,&pixel);
    if (!e && !pixel) e = kVTVideoDecoderBadDataErr;
    if (e) goto done;
    OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel);
    int bpp = pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange ||
              pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? 2 :
              pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
              pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ? 1 : 0;
    if (!bpp || CVPixelBufferGetPlaneCount(pixel) != 2 ||
        CVPixelBufferGetWidth(pixel) != c.width || CVPixelBufferGetHeight(pixel) != c.height) {
        e = kVTVideoDecoderBadDataErr; goto done;
    }
    e = CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    if (e) goto done;
    fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    created = fd >= 0;
    if (fd < 0) { perror("output (must be new)"); e = kVTParameterErr; }
    for (unsigned plane = 0; plane < 2 && !e; plane++) {
        const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pixel, plane);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane), row = c.width * (unsigned)bpp;
        if (!base || stride < row) { e = kVTVideoDecoderBadDataErr; break; }
        for (unsigned y = 0; y < (c.height >> plane) && !e; y++)
            if (write_all(fd, base + (size_t)y * stride, row)) e = kVTParameterErr;
    }
    OSStatus unlock = CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    if (!e) e = unlock;
    if (!e && fsync(fd)) e = kVTParameterErr;
    if (fd >= 0) { if (close(fd) && !e) e = kVTParameterErr; fd = -1; }
    if (e) { if (created) unlink(out_path); goto done; }
    printf("VT hardware picture: %ux%u depth=%u pixel=0x%08x callbacks=%u output=%s\n",
           c.width, c.height, c.bit_depth, (unsigned)pixel_format, 1u, out_path);
    rc = 0;
done:
    if (rc) fprintf(stderr, "decode failed: OSStatus=%d (no hardware success claimed)\n", (int)e);
    if (pixel) CVPixelBufferRelease(pixel);
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
                          const struct x9_heif_plan *plan, unsigned depth, struct item_walk *walk)
{
    if (depth == 32) return E2BIG;
    for (unsigned n = 0; n < depth; n++) if (walk->stack[n] == id) return ELOOP;
    for (unsigned n = 0; n < walk->seen_count; n++) if (walk->seen[n] == id) return 0;
    if (walk->seen_count == X9_HEIF_MAX_ITEMS) return ENOSPC;
    struct x9_heif_image *image = calloc(1,sizeof(*image));
    if (!image) return ENOMEM;
    int e = x9_heif_get_image(d,id,image);
    struct x9_heif_geometry geometry;
    if (!e) e = x9_heif_plan_geometry(plan,id,&geometry);
    if (e) { free(image); return e; }
    walk->stack[depth] = id; walk->seen[walk->seen_count++] = id;
    printf("item=%u type=%c%c%c%c size=%ux%u channels=%u hidden=%u bytes=%llu grid=%ux%u children=%u\n",
        id,(char)(image->type>>24),(char)(image->type>>16),(char)(image->type>>8),(char)image->type,
        image->width,image->height,image->channels,image->hidden,(unsigned long long)image->item_bytes,
        image->rows,image->columns,image->derived_count);
    printf("  ordered geometry=%ux%u; pixels/color/auxiliary merge not executed\n",
        geometry.width,geometry.height);
    printf("  location=%u construction_method=%u (item data, not decoded image references)\n",
        image->location_present,image->construction_method);
    printf("  prem references=%u (source unpremultiplication not executed)\n",image->premultiplied_reference_count);
    if (image->type == X9_HEIF_IOVL) {
        printf("  canvas fill RGBA16=%u,%u,%u,%u; requires linear source-over\n",
            image->canvas_fill_rgba[0],image->canvas_fill_rgba[1],image->canvas_fill_rgba[2],image->canvas_fill_rgba[3]);
        for (unsigned n = 0; n < image->derived_count; n++)
            printf("  placement[%u] item=%u x=%d y=%d\n",n,image->derived[n],image->overlay_x[n],image->overlay_y[n]);
    }
    for (unsigned n = 0; n < image->property_count; n++) {
        const struct x9_heif_property *p = &image->properties[n];
        printf("  property[%u]=%c%c%c%c essential=%u offset=%llu bytes=%llu (not applied)\n",
            n,(char)(p->type>>24),(char)(p->type>>16),(char)(p->type>>8),(char)p->type,p->essential,
            (unsigned long long)p->payload.offset,(unsigned long long)p->payload.bytes);
    }
    for (unsigned n = 0; n < X9_HEIF_MAX_REFERENCES && !e; n++) {
        uint32_t source = 0;
        e = x9_heif_item_offset_reference(d,id,n,&source);
        if (e == ENOENT) { e = 0; break; }
        if (!e) printf("  iloc-reference[%u]=%u (ordered byte source; not a dimg child)\n",n+1,source);
    }
    for (unsigned n = 0; n < image->derived_count && !e; n++) {
        printf("  dimg[%u]=%u\n",n,image->derived[n]);
        e = print_heif_item(d,image->derived[n],plan,depth+1,walk);
    }
    for (unsigned n = 0; n < X9_HEIF_MAX_ITEMS && !e; n++) {
        uint32_t auxiliary = 0;
        e = x9_heif_auxiliary(d,id,n,&auxiliary);
        if (e == ENOENT) { e = 0; break; }
        if (!e) { printf("  auxiliary[%u]=%u\n",n,auxiliary); e = print_heif_item(d,auxiliary,plan,depth+1,walk); }
    }
    free(image); return e;
}
static int heif_info(const char *path, enum x9_heif_clap_policy policy)
{
    size_t bytes = 0;
    uint8_t *data = read_file(path,X9_HEIF_MAX_FILE,&bytes);
    if (!data) { perror("HEIF input"); return 1; }
    struct x9_heif_document *d = NULL;
    struct x9_heif_plan *plan = NULL;
    uint32_t primary = 0;
    struct item_walk walk = {0};
    int e = x9_heif_open(data,bytes,&d);
    if (!e) e = x9_heif_primary(d,&primary);
    if (!e) e = x9_heif_plan_create_with_policy(d,primary,policy,&plan);
    if (!e) e = print_heif_item(d,primary,plan,0,&walk);
    if (e) fprintf(stderr,"HEIF item input rejected: errno=%d\n",e);
    else {
        printf("clean-aperture raster policy=%s\n",policy == X9_HEIF_CLAP_LINEAR_CEIL ? "linear-ceil" : "exact");
        puts("HEIF input graph only; no decode, transform, alpha merge or hardware success claimed");
    }
    x9_heif_plan_close(&plan); x9_heif_close(&d); free(data); return e ? 1 : 0;
}
static int decode_heif(const char *path, uint32_t id, const char *icc_path,
    enum x9_heif_source_color_policy policy, enum x9_heif_graph_color_policy graph_color,
    enum x9_heif_clap_policy clap_policy,
    uint64_t raster_budget, const char *intermediate_path, const char *output_path, const char *info_path)
{
    size_t bytes = 0, icc_bytes = 0, intermediate_bytes = 0;
    uint8_t *raw = read_file(path,X9_HEIF_MAX_FILE,&bytes);
    uint8_t *icc = read_file(icc_path,X9_HEIF_MAX_ITEM_METADATA,&icc_bytes);
    uint8_t *intermediate_raw = intermediate_path ? read_file(intermediate_path,X9_HEIF_MAX_ITEM_METADATA,&intermediate_bytes) : NULL;
    CFDataRef file = NULL, working = NULL, intermediate = NULL, plist = NULL;
    CFDictionaryRef info = NULL;
    CFErrorRef error = NULL;
    CVPixelBufferRef pixel = NULL;
    int out_fd = -1, info_fd = -1, out_created = 0, info_created = 0, locked = 0;
    size_t width = 0, height = 0;
    OSStatus e = kVTParameterErr;
    if (!raw || !icc || (intermediate_path && !intermediate_raw)) { perror("HEIF/working/intermediate ICC input"); goto done; }
    if (!strcmp(output_path,info_path)) goto done;
    file = CFDataCreateWithBytesNoCopy(NULL,raw,(CFIndex)bytes,kCFAllocatorNull);
    working = CFDataCreateWithBytesNoCopy(NULL,icc,(CFIndex)icc_bytes,kCFAllocatorNull);
    if (intermediate_path) intermediate = CFDataCreateWithBytesNoCopy(NULL,intermediate_raw,(CFIndex)intermediate_bytes,kCFAllocatorNull);
    if (!file || !working || (intermediate_path && !intermediate)) { e = kVTAllocationFailedErr; goto done; }
    e = X9HEIFCopyHardwareImageWithColorOptions(file,id,working,intermediate,policy,graph_color,
        clap_policy,raster_budget,&pixel,&info);
    if (!e && (!pixel || !info)) e = kVTVideoDecoderBadDataErr;
    if (e) goto done;
    plist = CFPropertyListCreateData(NULL,info,kCFPropertyListBinaryFormat_v1_0,0,&error);
    if (!plist) { e = kVTVideoDecoderBadDataErr; goto done; }
    e = CVPixelBufferLockBaseAddress(pixel,kCVPixelBufferLock_ReadOnly);
    if (e) goto done;
    locked = 1;
    width = CVPixelBufferGetWidth(pixel); height = CVPixelBufferGetHeight(pixel);
    if (!width || !height || width > X9_HEIF_MAX_DIMENSION || height > X9_HEIF_MAX_DIMENSION) {
        e = kVTVideoDecoderBadDataErr; goto done;
    }
    size_t stride = CVPixelBufferGetBytesPerRow(pixel), row = width*8;
    const uint8_t *base = CVPixelBufferGetBaseAddress(pixel);
    if (CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_64RGBAHalf ||
        CVPixelBufferIsPlanar(pixel) || !base || stride < row || stride > raster_budget/height ||
        (uint64_t)stride*(height-1)+row > UINTPTR_MAX-(uintptr_t)base) {
        e = kVTVideoDecoderBadDataErr; goto done;
    }
    out_fd = open(output_path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    out_created = out_fd >= 0;
    if (out_fd < 0) { perror("RGBA output (must be new)"); e = kVTParameterErr; goto done; }
    info_fd = open(info_path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    info_created = info_fd >= 0;
    if (info_fd < 0) { perror("metadata output (must be new)"); e = kVTParameterErr; goto done; }
    for (size_t y = 0; y < height && !e; y++)
        if (write_all(out_fd,base+y*stride,row)) e = kVTParameterErr;
    if (!e && write_all(info_fd,CFDataGetBytePtr(plist),(size_t)CFDataGetLength(plist))) e = kVTParameterErr;
    if (!e && (fsync(out_fd) || fsync(info_fd))) e = kVTParameterErr;
    if (close(out_fd) && !e) e = kVTParameterErr;
    out_fd = -1;
    if (close(info_fd) && !e) e = kVTParameterErr;
    info_fd = -1;
done:
    if (locked) { OSStatus unlock = CVPixelBufferUnlockBaseAddress(pixel,kCVPixelBufferLock_ReadOnly); if (!e) e = unlock; }
    if (out_fd >= 0) close(out_fd);
    if (info_fd >= 0) close(info_fd);
    if (e) {
        if (out_created) unlink(output_path);
        if (info_created) unlink(info_path);
        fprintf(stderr,"HEIF decode/compose failed: OSStatus=%d; no hardware image success claimed\n",(int)e);
    } else printf("native HEIF color raster: %zux%zu RGBA IEEE-half little-endian; "
        "explicit working ICC, ordered transforms applied; pixels=%s metadata=%s\n",
        width,height,output_path,info_path);
    if (pixel) CVPixelBufferRelease(pixel);
    if (plist) CFRelease(plist);
    if (error) CFRelease(error);
    if (info) CFRelease(info);
    if (file) CFRelease(file);
    if (working) CFRelease(working);
    if (intermediate) CFRelease(intermediate);
    free(raw); free(icc); free(intermediate_raw); return e ? 1 : 0;
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "decode-vt")) return decode_vt(argv[2], argv[3], argv[4],NULL,0);
    if ((argc == 3 || argc == 4) && !strcmp(argv[1],"heif-info")) {
        enum x9_heif_clap_policy policy = X9_HEIF_CLAP_EXACT_SAMPLES;
        if (argc == 4) {
            if (!strcmp(argv[3],"linear-ceil")) policy = X9_HEIF_CLAP_LINEAR_CEIL;
            else if (strcmp(argv[3],"exact")) return 2;
        }
        return heif_info(argv[2],policy);
    }
    if (argc == 5 && !strcmp(argv[1],"decode-heif-item")) {
        char *end = NULL;
        errno = 0; unsigned long id = strtoul(argv[3],&end,0);
        if (errno || !argv[3][0] || !end || *end || id > UINT32_MAX) return 2;
        return decode_vt(NULL,NULL,argv[4],argv[2],(uint32_t)id);
    }
    if (argc >= 8 && argc <= 13 && !strcmp(argv[1],"decode-heif")) {
        char *end = NULL;
        errno = 0; unsigned long id = strtoul(argv[3],&end,0);
        if (errno || !argv[3][0] || !end || *end || id > UINT32_MAX) return 2;
        enum x9_heif_source_color_policy policy;
        if (!strcmp(argv[5],"icc")) policy = X9_HEIF_USE_ICC_IF_PRESENT;
        else if (!strcmp(argv[5],"nclx")) policy = X9_HEIF_USE_NCLX;
        else return 2;
        enum x9_heif_graph_color_policy graph_color = X9_HEIF_GRAPH_COLOR_INHERIT_IDENTICAL;
        enum x9_heif_clap_policy clap = X9_HEIF_CLAP_EXACT_SAMPLES;
        int next = 6, options_end = argc-2;
        if (next < options_end && argv[next][0] != '-') {
            if (!strcmp(argv[next],"linear-ceil")) clap = X9_HEIF_CLAP_LINEAR_CEIL;
            else if (strcmp(argv[next],"exact")) return 2;
            next++;
        }
        uint64_t budget = clap == X9_HEIF_CLAP_LINEAR_CEIL ? UINT64_C(1024)*1024*1024 : X9_HEIF_MAX_RASTER_BYTES;
        if (next < options_end && argv[next][0] != '-') {
            errno = 0; end = NULL;
            unsigned long long mib = strtoull(argv[next],&end,10);
            if (errno || argv[next][0] < '0' || argv[next][0] > '9' || !end || *end || !mib ||
                mib > (uint64_t)INT64_MAX/(1024*1024) || mib > (uint64_t)SIZE_MAX/(1024*1024)) return 2;
            budget = (uint64_t)mib*1024*1024;
            next++;
        }
        const char *intermediate_path = NULL;
        while (next < options_end) {
            if (!strcmp(argv[next],"--per-item-colors")) {
                if (graph_color == X9_HEIF_GRAPH_COLOR_PER_ITEM) return 2;
                graph_color = X9_HEIF_GRAPH_COLOR_PER_ITEM; next++;
            } else if (!strcmp(argv[next],"--linear-intermediate")) {
                if (clap != X9_HEIF_CLAP_LINEAR_CEIL || intermediate_path || next+1 >= options_end ||
                    !argv[next+1][0] || !strncmp(argv[next+1],"--",2)) return 2;
                intermediate_path = argv[next+1]; next += 2;
            } else return 2;
        }
        if (next != options_end) return 2;
        return decode_heif(argv[2],(uint32_t)id,argv[4],policy,graph_color,clap,budget,
            intermediate_path,argv[argc-2],argv[argc-1]);
    }
    if (argc != 2 || strcmp(argv[1], "status")) {
        fprintf(stderr, "usage: %s status | decode-vt hvcC.bin access-unit.bin NEW.yuv | "
            "heif-info file.heic [exact|linear-ceil] | decode-heif-item file.heic item-id-or-0 NEW.yuv | "
            "decode-heif file.heic item-id-or-0 working-rgb.icc icc|nclx [exact|linear-ceil [raster-MiB]] "
            "[--linear-intermediate matrix-rgb.icc] [--per-item-colors] NEW.rgba16f NEW.info.plist\n"
            "heap budget default: exact=256MiB, linear-ceil=1024MiB; not hardware capacity\n"
            "intermediate requires linear-ceil; derives actual linear ICC, preserves final working ICC; no sRGB fallback\n"
            "per-item-colors explicitly normalizes own colr/SPS sources before composition; derived colr is metadata only\n",
            argv[0]); return 2;
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
