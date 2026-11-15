#ifndef X9_HEIF_NATIVE_H
#define X9_HEIF_NATIVE_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
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
 * still unsupported: any aux rejects before the first hardware decode. This
 * does not certify pixel-transfer HDR/tone-map behavior without runtime data.
 * No software/unrelated-codec decode fallback; per-tile exact DecoderID and
 * real UsingHardware checks remain mandatory. file/working ICC must not mutate
 * during the call. Returned metadata/profile/pixels have owned bytes, not
 * borrowed NoCopy CFData storage. Outputs are NULL on failure. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyHardwareImage(
    CFDataRef file, uint32_t item_id, CFDataRef working_rgb_icc,
    enum x9_heif_source_color_policy source_policy,
    CVPixelBufferRef *image, CFDictionaryRef *info);
#endif
