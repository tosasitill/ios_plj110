#ifndef X9_HEIF_NATIVE_H
#define X9_HEIF_NATIVE_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "heif_compositor.h"
enum x9_heif_source_color_policy {
    X9_HEIF_USE_ICC_IF_PRESENT = 1,
    X9_HEIF_USE_NCLX = 2
};
/* Native coded-item decode -> explicit native pixel-transfer working RGB ->
 * derived packed composition. No default working color space: working_rgb_icc
 * must be a valid RGB ICC profile. Source policy is explicit when ICC/nclx
 * coexist; nclx matrix and actual pixel range still interpret YUV.
 * Returns owned RGBA IEEE-half little-endian CVPixelBuffer and mandatory info
 * containing every item's ordered raw metadata plus working ICC and policy.
 * Container colr on derived images can supply missing tile colr; conflicting
 * inherited/explicit colr or reused-item contexts fail rather than relabeling
 * already-converted pixels. Missing/container CICP2 fields use that sample's
 * actual PPS->SPS VUI; still-unspecified fields fail, never CM defaults.
 * Container range overrides SPS range only when the actual decoded pixel
 * format agrees; a conflict fails rather than relabeling decoder storage.
 * Progressive chroma siting is explicitly attached to the transfer view;
 * field sequences/different top-bottom locations reject during preflight.
 * Unsupported CICP values never become guessed BT709/sRGB defaults.
 * Aux/alpha, fractional crop, Rext and ordinary ImageIO/CMPhoto routing are
 * still unsupported by THIS exact-samples entrypoint: any aux rejects before
 * the first hardware decode. Fractional sampling has the explicit options
 * entrypoint below. This
 * does not certify pixel-transfer HDR/tone-map behavior without runtime data.
 * No software/unrelated-codec decode fallback; per-tile exact DecoderID and
 * real UsingHardware checks remain mandatory. file/working ICC must not mutate
 * during the call. Returned metadata/profile/pixels have owned bytes, not
 * borrowed NoCopy CFData storage. Outputs are NULL on failure. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyHardwareImage(
    CFDataRef file, uint32_t item_id, CFDataRef working_rgb_icc,
    enum x9_heif_source_color_policy source_policy,
    CVPixelBufferRef *image, CFDictionaryRef *info);
/* Explicit fractional-clap rasterization, with the same mandatory hardware
 * source checks and output ABI as above. EXACT_SAMPLES preserves the original
 * no-resampling behavior. When a plan actually needs LINEAR_CEIL resampling,
 * it requires a matrix-based working ICC:
 * derive its extended linear RGB space through CoreGraphics, transfer every
 * coded item into that space as binary32, apply ordered rational crops using
 * the selected ceil/pixel-center policy, then transfer ONCE to the requested
 * working ICC and RGBA-half. No interpolation of non-linear/ICC-coded RGB.
 * Non-matrix/LUT ICCs reject BEFORE decode, not substituted with sRGB. Policy
 * and actual linear ICC are retained in info; ceil is an explicit render
 * choice, not a normative HEIF size inference. Aux/mixed colr/ordinary routes
 * remain unsupported. Pixel-transfer/HDR effect is not proven by this code. */
/* raster_budget is a caller-selected CPU heap limit, 1..min(SIZE_MAX,INT64_MAX),
 * not hardware RAM/DPB capacity. Float root + final half coexist; that known
 * peak rejects before decode. ALL compositor caches/intermediates also share
 * this limit. VT/decoder/driver storage is separate, and allocation can fail
 * below the budget. Applies to exact-copy plans too; the original entrypoint
 * explicitly passes its unchanged256MiB budget. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyHardwareImageWithOptions(
    CFDataRef file, uint32_t item_id, CFDataRef working_rgb_icc,
    enum x9_heif_source_color_policy, enum x9_heif_clap_policy, uint64_t raster_budget,
    CVPixelBufferRef *image, CFDictionaryRef *info);
#endif
