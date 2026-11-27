/* HEIF derived-image geometry and packed-raster composition. No entropy
 * decoder, hardware registration or implicit color conversion lives here. */
#ifndef X9_HEIF_COMPOSITOR_H
#define X9_HEIF_COMPOSITOR_H
#include "heif_items.h"
#define X9_HEIF_MAX_DEPTH 32u
#define X9_HEIF_MAX_DIMENSION 16384u
#define X9_HEIF_MAX_RASTER_BYTES (256u * 1024u * 1024u)
struct x9_heif_plan;
enum x9_heif_clap_policy {
    X9_HEIF_CLAP_EXACT_SAMPLES = 0,
    /* Explicit rasterization policy, NOT a claim that HEIF mandates ceil.
     * Each rational aperture becomes ceil(width) x ceil(height) samples;
     * pixel centers span the EXACT aperture. Interpolation is linear-light,
     * premultiplied RGBA float; all later ordered transforms use that raster. */
    X9_HEIF_CLAP_LINEAR_CEIL = 1
};
struct x9_heif_geometry {
    uint32_t id, width, height; /* after this item's ordered transforms */
    uint32_t coded_width, coded_height; /* ispe, before item transforms */
    uint32_t derived_count, auxiliary_count;
    struct x9_heif_alpha_association alpha; /* container edge plus optional raw
        * producer-shaped subtype spans, NOT HEVC PRIMARY/layer or valid alpha */
    struct x9_heif_sample_aspect source_aspect; /* pre-item-transform, separate provenance;
        * explicit canvas pasp, or absent-pasp identity's unique child OUTPUT */
    uint32_t source_aspect_item_id; /* immediate dimg child for absent-pasp identity,
        * including an absent child metric; 0 for own/coded source provenance */
    uint64_t pixel_aspect_h, pixel_aspect_v; /* output ratio, 1..INT64_MAX;
        * absent resolved source remains0, never an inferred square default */
};
/* The document and its borrowed immutable bytes must outlive the plan. The
 * plan validates ALL reachable dimg/auxl nodes, auxC presence and single-alpha
 * prem source/target pairing, active-stack cycles, tile
 * coverage and ordered geometry before a raster callback can run. item_id0
 * selects primary. Dimensions/memory/depth limits are implementation bounds.
 * This original entrypoint uses EXACT_SAMPLES: fractional clean-aperture
 * sizes/origins return ENOTSUP, never unannounced rounding/resampling. The
 * with_policy entrypoint can explicitly plan LINEAR_CEIL rasterization.
 * Arbitrary aux types remain in the graph. Present pasp on a coded root has
 * an exact ordered OUTPUT metric: odd quarter turns exchange axes, mirror
 * and integer crops preserve spacing, explicit LINEAR_CEIL aperture extents
 * adjust spacing by exact extent/output sample counts. Cross-cancelled
 * output ratio must fit positive signed64 CFNumbers, else E2BIG, never rounded.
 * This is not square-pixel conversion or a HEIF mandated resampling policy.
 * grid/iden/iovl compare EVERY child's ordered OUTPUT ratio with the canvas's
 * pre-transform input metric before callbacks and again on cached rasters
 * at the actual sample-copy path. Agreeing non-square lattices can be copied
 * without resizing; canvas crop/rotation then determines its output metric.
 * An identity without its own pasp copies its unique child's ordered OUTPUT
 * metric after exact input-size agreement, then applies its own transforms.
 * Its present source origin is IDENTITY_OUTPUT, with the immediate child ID;
 * absent child remains absent. This does not rewrite raw container metadata.
 * Explicit mismatches or other non-square edges with an absent ratio reject.
 * Grid/overlay have no absent-metric inheritance, and no missing ratio becomes1:1.
 * iovl signed offsets are sample coordinates in an agreeing canvas lattice;
 * clipping/source-over do not scale them or resize the child. All edges,
 * including fully invisible placements, are preflighted; visible cached child
 * metrics are rechecked immediately before blending. Only the canvas's own
 * later transforms change its completed output metric.
 * Non-square auxiliary placement, mixed-lattice resampling and
 * full normative derived metric/inheritance are not implemented. These
 * container-only entrypoints resolve no coded metric beyond pasp; the native preflight
 * variant below resolves actual coded-source SPS/CM sample aspect. */
int x9_heif_plan_create(
    const struct x9_heif_document *, uint32_t item_id, struct x9_heif_plan **);
int x9_heif_plan_create_with_policy(const struct x9_heif_document *, uint32_t item_id,
    enum x9_heif_clap_policy, struct x9_heif_plan **);
/* Native metadata preflight variant, used by the real CM/VT raster adapter.
 * Query runs ONCE per reachable coded item during immutable plan creation,
 * before any raster callback. It must not decode, access hardware, retain the
 * borrowed image or recursively use this plan. Returned aspect is the actual
 * pre-transform CM source pair with PASP/selected-SPS/observed-CM provenance;
 * explicit raw pasp must agree rationally and keep PASP origin. No context or
 * query is retained by the returned plan. Whole-graph non-square source OR
 * output gates apply equally to resolved SPS/CM ratios, not just pasp.
 * The original entrypoints above remain container-only geometry planning,
 * not selected-SPS/CM resolution or proof of a callback's physical metric. */
typedef int (*x9_heif_aspect_source)(void *context, const struct x9_heif_image *,
    struct x9_heif_sample_aspect *);
int x9_heif_plan_create_with_source_aspects(const struct x9_heif_document *, uint32_t item_id,
    enum x9_heif_clap_policy, x9_heif_aspect_source, void *context, struct x9_heif_plan **);
/* Includes ALL reachable nodes, not just root transforms. Negative errno on
 * invalid plan; 0 means exact copies suffice, 1 means linear resampling. */
int x9_heif_plan_requires_resampling(const struct x9_heif_plan *);
/* iovl source-over always needs premultiplied linear-light RGB, even if all
 * geometry is integral. Composition and resampling are separate requirements. */
int x9_heif_plan_requires_linear_composition(const struct x9_heif_plan *);
void x9_heif_plan_close(struct x9_heif_plan **);
int x9_heif_plan_geometry(
    const struct x9_heif_plan *, uint32_t item_id, struct x9_heif_geometry *);
enum x9_heif_raster_format {
    X9_HEIF_RGBA8 = 1, X9_HEIF_RGBA16LE, X9_HEIF_RGBA_HALF, X9_HEIF_RGBA_FLOAT,
    X9_HEIF_GRAY8, X9_HEIF_GRAY16LE
};
struct x9_heif_raster {
    uint32_t width, height, format;
    size_t stride, bytes;
    uint8_t *data; /* compositor-owned malloc; release with raster_close */
    uint64_t pixel_aspect_h, pixel_aspect_v; /* output ratio, positive signed64, absent0;
        * a consumer must not silently assume square when these differ */
};
/* Callback fills ONE untransformed coded item's visible raster, or an iovl
 * canvas's declared RGBA16 fill CONVERTED to the selected working space; never retains
 * destination. All callbacks in one composition MUST supply the SAME working
 * color interpretation/component encoding and alpha convention selected by
 * the caller. YUV/ICC/CICP conversion and native decode belong to that backend;
 * packed composition only copies/reorders exact samples; the explicit linear
 * entrypoint below additionally resamples. Neither certifies that supplied
 * pixels originated in hardware. A nonzero errno discards the
 * whole composition; callback must not recursively use/close this plan.
 * Destination is tightly packed, with format's canonical component encoding.
 */
typedef int (*x9_heif_raster_source)(void *context, uint32_t coded_item_id,
    enum x9_heif_raster_format format, uint32_t width, uint32_t height,
    uint8_t *destination, size_t stride);
/* Actual row-major grid/identity/crop/mirror/rotation execution; reusable
 * dimg nodes are decoded once per call, never confused with placement order.
 * Any reachable auxiliary/prem reference causes ENOTSUP BEFORE the first source callback:
 * alpha/gain/depth merge has not been implemented and is never dropped. All
 * intermediate/cached rasters share a bounded memory budget. On error out is
 * zeroed and no partial image is returned. out must be zero-initialized;
 * passing an already-owned raster returns EINVAL without discarding it.
 * No working-space defaults. This is an internal backend core, not an
 * ImageIO/CMPhoto entrypoint or an automatic native raster adapter. */
int x9_heif_compose_packed(
    const struct x9_heif_plan *, enum x9_heif_raster_format,
    x9_heif_raster_source, void *context, struct x9_heif_raster *out);
/* Same exact-copy contract with an explicit nonzero <=SIZE_MAX heap budget. */
int x9_heif_compose_packed_with_budget(const struct x9_heif_plan *, enum x9_heif_raster_format,
    x9_heif_raster_source, void *context, uint64_t raster_budget, struct x9_heif_raster *out);
/* Same graph/cache/aux/memory gates as packed, but the caller explicitly
 * supplies a single LINEAR-LIGHT RGB space with premultiplied alpha. RGBA is
 * IEEE binary32 little-endian, finite RGB (negative/extended values allowed),
 * alpha in[0,1], with zero RGB when alpha0. No gamma defaults, clipping,
 * color conversion or tone-map. iovl source-over blends each clipped signed
 * placement in recorded dimg order; canvas fill MUST be converted by source.
 * Fractional apertures use bilinear pixel-center sampling with edge extension
 * only within a previously EXACT-bounds-validated source aperture. Ordinary
 * compose_packed refuses interpolation or overlays, even float format.
 * raster_budget is an explicit nonzero CPU heap budget for ALL cached and
 * intermediate rasters, <=SIZE_MAX. It is not a device/decoder capacity or a
 * memory reservation; real allocation failure still propagates. The legacy
 * packed entrypoint retains its original 256MiB implementation budget. */
int x9_heif_compose_linear_float(const struct x9_heif_plan *,
    x9_heif_raster_source, void *context, uint64_t raster_budget, struct x9_heif_raster *out);
void x9_heif_raster_close(struct x9_heif_raster *);
#endif
