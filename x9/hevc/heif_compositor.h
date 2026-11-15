/* HEIF derived-image geometry and packed-raster composition. No entropy
 * decoder, hardware registration or implicit color conversion lives here. */
#ifndef X9_HEIF_COMPOSITOR_H
#define X9_HEIF_COMPOSITOR_H
#include "heif_items.h"
#define X9_HEIF_MAX_DEPTH 32u
#define X9_HEIF_MAX_DIMENSION 16384u
#define X9_HEIF_MAX_RASTER_BYTES (256u * 1024u * 1024u)
struct x9_heif_plan;
struct x9_heif_geometry {
    uint32_t id, width, height; /* after this item's ordered transforms */
    uint32_t coded_width, coded_height; /* ispe, before item transforms */
    uint32_t derived_count, auxiliary_count;
};
/* The document and its borrowed immutable bytes must outlive the plan. The
 * plan validates ALL reachable dimg/auxl nodes, active-stack cycles, tile
 * coverage and ordered geometry before a raster callback can run. item_id0
 * selects primary. Dimensions/memory/depth limits are implementation bounds.
 * Fractional clean-aperture sizes/origins return ENOTSUP: no rounding or
 * unannounced resampling. Arbitrary aux types remain in the graph. */
int x9_heif_plan_create(
    const struct x9_heif_document *, uint32_t item_id, struct x9_heif_plan **);
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
};
/* Callback fills ONE untransformed coded item's visible raster, never retains
 * destination. All callbacks in one composition MUST supply the SAME working
 * color interpretation/component encoding and alpha convention selected by
 * the caller. YUV/ICC/CICP conversion and native decode belong to that backend;
 * this core only copies/reorders exact packed samples. It does not certify
 * that supplied pixels originated in hardware. A nonzero errno discards the
 * whole composition; callback must not recursively use/close this plan.
 * Destination is tightly packed, with format's canonical component encoding.
 */
typedef int (*x9_heif_raster_source)(void *context, uint32_t coded_item_id,
    enum x9_heif_raster_format format, uint32_t width, uint32_t height,
    uint8_t *destination, size_t stride);
/* Actual row-major grid/identity/crop/mirror/rotation execution; reusable
 * dimg nodes are decoded once per call, never confused with placement order.
 * Any reachable auxiliary causes ENOTSUP BEFORE the first source callback:
 * alpha/gain/depth merge has not been implemented and is never dropped. All
 * intermediate/cached rasters share a bounded memory budget. On error out is
 * zeroed and no partial image is returned. out must be zero-initialized;
 * passing an already-owned raster returns EINVAL without discarding it.
 * No working-space defaults. This is an internal backend core, not an
 * ImageIO/CMPhoto entrypoint or an automatic native raster adapter. */
int x9_heif_compose_packed(
    const struct x9_heif_plan *, enum x9_heif_raster_format,
    x9_heif_raster_source, void *context, struct x9_heif_raster *out);
void x9_heif_raster_close(struct x9_heif_raster *);
#endif
