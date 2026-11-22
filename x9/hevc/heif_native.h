#ifndef X9_HEIF_NATIVE_H
#define X9_HEIF_NATIVE_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "heif_compositor.h"
enum x9_heif_source_color_policy {
    X9_HEIF_USE_ICC_IF_PRESENT = 1,
    X9_HEIF_USE_NCLX = 2
};
enum x9_heif_graph_color_policy {
    X9_HEIF_GRAPH_COLOR_INHERIT_IDENTICAL = 1,
    X9_HEIF_GRAPH_COLOR_PER_ITEM = 2
};
/* Native coded-item decode -> explicit native pixel-transfer working RGB ->
 * derived packed composition. No default working color space: working_rgb_icc
 * must be a valid RGB ICC profile. Source policy is explicit when ICC/nclx
 * coexist; nclx matrix and actual pixel range still interpret YUV.
 * Returns owned straight-alpha RGBA IEEE-half little-endian CVPixelBuffer and mandatory info
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
 * A coded root propagates its actual pre-transform sample aspect through
 * ordered crop/mirror/rotation and the returned CVPixelBuffer, never merely
 * relabelled square. Source is explicit pasp, otherwise known actual-selected
 * PPS->SPS VUI, otherwise an observed CM format value with CM-only provenance;
 * absent CM stays absent, never a synthetic1:1. Native plan creation queries
 * CM/sample metadata for every reachable coded item without decoding and
 * retains no AU buffers. Color preflight and the actual decoded source
 * recheck the immutable plan's ratio/presence read-only. Raw pasp provenance
 * remains separate from SourcePixelAspectRatio/Origin (absent0, pasp1,
 * selected-SPS2, observed-CM3, identity-output4). An absent-pasp identity
 * preserves its unique exact-size child's ordered OUTPUT metric as input,
 * with SourcePixelAspectItemID naming that immediate child, then applies its
 * own transforms. Absent child stays0/0 with origin0; no raw pasp is invented.
 * Explicit grid/iden require each child's transformed output ratio to agree with the
 * canvas's explicit pre-transform pasp BEFORE hardware. The actual sample-copy
 * path rechecks completed cached raster ratios; agreeing non-square samples
 * preserve that lattice, followed by the canvas's ordered transforms. Grid
 * canvas/child absence cannot admit a non-square placement and never inherits;
 * identity preservation also rechecks presence on the actual cached raster.
 * Non-square overlay/auxiliary placement, mixed-lattice resampling and full
 * normative derived metric/inheritance remain unimplemented. Explicit LINEAR_CEIL
 * uses exact aperture/output-sample counts, not double-derived ratios.
 * Aux/alpha, fractional crop, Rext and ordinary ImageIO/CMPhoto routing are
 * still unsupported by THIS exact-samples entrypoint: any aux rejects before
 * the first hardware decode. Fractional sampling has the explicit options
 * entrypoint below. This
 * does not certify pixel-transfer HDR/tone-map behavior without runtime data.
 * iovl v0 supports signed16/32 placements and recorded-order source-over in
 * the explicitly selected extended-linear RGB space. Integral overlays also
 * use that pipeline: a matrix working ICC (or options' matrix intermediate)
 * is required. Nonzero-alpha canvas fill's unsigned16 RGB needs recognized
 * selected own/explicitly inherited colr, never an SPS/default RGB profile.
 * It converts once as opaque1x1 RGB, then uses raw alpha/65535. Alpha0 fill
 * needs no guessed RGB interpretation. Final float RGB is unassociated and
 * transferred as opaque; preserved CPU alpha is rounded ties-even to half
 * and restored into OWNED output, with truthful straight/opaque attachments.
 * This is the explicit renderer's linear-light policy, not a normative
 * ImageIO/CMPhoto blending or full auxiliary-alpha interpretation. Any
 * reachable prem source still rejects BEFORE decode, not silently ignored.
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
 * it (or an integral iovl overlay) requires a matrix-based working ICC:
 * derive its extended linear RGB space through CoreGraphics, transfer every
 * coded item into that space as binary32, apply ordered rational crops using
 * the selected ceil/pixel-center policy, then transfer ONCE to the requested
 * working ICC and RGBA-half. No interpolation/source-over in non-linear RGB.
 * Non-matrix/LUT ICCs reject BEFORE decode, not substituted with sRGB. Policy
 * and actual linear ICC are retained in info; ceil is an explicit render
 * choice, not a normative HEIF size inference. Aux/mixed colr/ordinary routes
 * remain unsupported. Pixel-transfer/HDR effect is not proven by this code. */
/* raster_budget is a caller-selected CPU heap limit, 1..min(SIZE_MAX,INT64_MAX),
 * not hardware RAM/DPB capacity. Float root + final half + float alpha plane
 * coexist (28bytes per root pixel); that peak rejects before decode.
 * ALL compositor caches/intermediates also share
 * this limit. VT/decoder/driver storage is separate, and allocation can fail
 * below the budget. Applies to exact-copy plans too; the original entrypoint
 * explicitly passes its unchanged256MiB budget. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyHardwareImageWithOptions(
    CFDataRef file, uint32_t item_id, CFDataRef working_rgb_icc,
    enum x9_heif_source_color_policy, enum x9_heif_clap_policy, uint64_t raster_budget,
    CVPixelBufferRef *image, CFDictionaryRef *info);
/* Explicit resampling intermediate, WITHOUT changing the final working ICC.
 * NULL intermediate_rgb_icc is exactly the WithOptions behavior. A non-NULL
 * profile requires LINEAR_CEIL and must be a matrix-based RGB ICC from which
 * CoreGraphics can derive an output-capable extended-linear RGB ICC. It is
 * NOT assumed linear: only the actually derived ICC interprets float pixels.
 * The caller selects primaries/gamut; there is no default sRGB substitution.
 * This permits a LUT/non-matrix final working ICC, provided it supports output
 * and the actual VT transfers support both selected profiles. Each coded item
 * transfers directly to the derived linear ICC, then composition runs there,
 * and one final transfer converts to working_rgb_icc/RGBA-half.
 * An explicit intermediate is qualified before decode even for an integral
 * LINEAR_CEIL plan; it is unused when there is no overlay/fractional sampling
 * (exact working-ICC copies remain). Overlays always use the linear pipeline.
 * Info distinguishes the owned requested intermediate, whether it was used,
 * the actual linearization base and the derived linear ICC. All input bytes
 * must remain unchanged during the call; no borrowed ICC bytes escape. Other
 * unsupported cases/hardware gates and heap-budget semantics are unchanged.
 * This does not certify arbitrary LUT support, gamut/HDR effects or hardware
 * completion without runtime evidence. Existing entrypoints retain their ABI. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyHardwareImageWithIntermediateICC(
    CFDataRef file, uint32_t item_id, CFDataRef working_rgb_icc, CFDataRef intermediate_rgb_icc,
    enum x9_heif_source_color_policy, enum x9_heif_clap_policy, uint64_t raster_budget,
    CVPixelBufferRef *image, CFDictionaryRef *info);
/* Explicit graph color interpretation. INHERIT_IDENTICAL is EXACTLY the
 * existing entrypoints' behavior: missing child colr inherits its ancestor,
 * different explicit/inherited or reused-item interpretations reject.
 * PER_ITEM is a caller-selected rendering choice, not a claim about normative
 * HEIF derived-image inheritance or ordinary ImageIO/CMPhoto behavior:
 * interpret each coded item using ONLY its own colr and selected PPS->SPS VUI,
 * then actually transfer it to the common working ICC (or actual derived
 * linear ICC for resampling) before placement/composition. Different source
 * profiles and reuse under differently tagged ancestors are allowed without
 * caching a source under multiple interpretations. Derived colr is retained
 * as declared metadata, NOT applied to already-normalized composed samples;
 * iovl's declared colr interprets ONLY its RGB canvas fill before placement.
 * the final profile remains the caller's working_rgb_icc.
 * Missing/unspecified own+SPS source fields still reject before hardware
 * decode, even if an ancestor has a complete colr. No guessed default gamut,
 * profile relabel, or fallback to the inheritance policy. Info records this
 * choice and each node's interpretation. All hardware/aux/heap/ICC ownership
 * gates of WithIntermediateICC remain. Runtime color/HDR support and full
 * mixed-derived HEIF semantics are not certified by this explicit renderer.
 * Existing exports retain their ABI and always select INHERIT_IDENTICAL. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyHardwareImageWithColorOptions(
    CFDataRef file, uint32_t item_id, CFDataRef working_rgb_icc, CFDataRef intermediate_rgb_icc,
    enum x9_heif_source_color_policy, enum x9_heif_graph_color_policy,
    enum x9_heif_clap_policy, uint64_t raster_budget,
    CVPixelBufferRef *image, CFDictionaryRef *info);
#endif
