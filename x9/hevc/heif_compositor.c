#include "heif_compositor.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define T(a,b,c,d) X9_HEIF_TYPE(a,b,c,d)
enum operation { CROP = 1, ROTATE, MIRROR };
struct transform {
    uint32_t kind, argument, x, y, width, height, resample;
    double left, top, aperture_width, aperture_height;
};
struct node {
    struct x9_heif_geometry geometry;
    uint32_t type, rows, columns, state, subtree_height, transform_count;
    uint32_t premultiplied, auxiliary_source;
    uint32_t derived[X9_HEIF_MAX_ITEMS], auxiliary[X9_HEIF_MAX_ITEMS];
    int32_t overlay_x[X9_HEIF_MAX_ITEMS], overlay_y[X9_HEIF_MAX_ITEMS];
    struct transform transforms[X9_HEIF_MAX_ASSOCIATIONS];
};
struct x9_heif_plan {
    const struct x9_heif_document *document;
    uint32_t count, root, resampling, overlays;
    enum x9_heif_clap_policy clap_policy;
    x9_heif_aspect_source aspect_source;
    void *aspect_context; /* creation only, never retained in a returned plan */
    struct node nodes[X9_HEIF_MAX_ITEMS];
};
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int64_t signed32(uint32_t n)
{ return n & 0x80000000u ? (int64_t)n-4294967296LL : (int64_t)n; }
/* All bounds are exact before converting coordinates to double. Full uint32
 * denominators can make the common origin denominator 65 bits and numerator
 * 79 bits; int64 is NOT sufficient for rational aperture sizes. __int128 is
 * bounded here by fixed uint32 inputs and the <=16384 image extent. */
static int aperture_axis(uint32_t image, uint32_t size_n, uint32_t size_d,
    uint32_t offset_n, uint32_t offset_d, enum x9_heif_clap_policy policy,
    uint32_t *origin, uint32_t *size, double *start_out, double *extent_out,
    uint32_t *resample)
{
    if (!size_n || !size_d || !offset_d) return EBADMSG;
    __int128 remaining = (__int128)image*size_d-size_n;
    if (remaining < 0) return EBADMSG;
    __int128 denominator = (__int128)2*size_d*offset_d;
    __int128 numerator = remaining*offset_d + (__int128)2*signed32(offset_n)*size_d;
    if (numerator < 0 || numerator > (__int128)2*remaining*offset_d) return EBADMSG;
    uint32_t fractional = size_n % size_d != 0 || numerator % denominator != 0;
    if (fractional && policy != X9_HEIF_CLAP_LINEAR_CEIL) return ENOTSUP;
    *origin = (uint32_t)(numerator/denominator);
    *size = (uint32_t)(((uint64_t)size_n+size_d-1)/size_d);
    *start_out = (double)numerator/(double)denominator;
    *extent_out = (double)size_n/size_d;
    *resample |= fractional;
    return 0;
}
/* The resolved source pair describes the pre-item-transform lattice. Retain
 * spacings for exact crops/mirrors and exchange axes on odd quarter turns.
 * LINEAR_CEIL centers span the exact aperture, so a crop multiplies h/v by
 * (width_n * height_d * out_height)/(width_d * out_width * height_n).
 * Carry factors through ALL ordered transforms before cross-cancelling and
 * multiplying: an intermediate large ratio may be reduced by a later crop.
 * This is the explicit rasterization policy's metric, not derived inheritance
 * or a claim that HEIF mandates ceil. No double participates in this ratio. */
#define METRIC_FACTORS (1u + 3u * X9_HEIF_MAX_ASSOCIATIONS)
static uint64_t gcd(uint64_t a, uint64_t b)
{ while (b) { uint64_t r = a % b; a = b; b = r; } return a; }
static int metric_product(const uint64_t *factors, unsigned count, uint64_t *out)
{
    uint64_t product = 1;
    for (unsigned n = 0; n < count; n++) {
        if (!factors[n]) return EBADMSG;
        if (product > (uint64_t)INT64_MAX/factors[n]) return E2BIG;
        product *= factors[n];
    }
    *out = product; return 0;
}
static int transforms(struct x9_heif_plan *plan, const struct x9_heif_image *image,
    struct node *node)
{
    uint32_t width = image->width, height = image->height;
    uint64_t horizontal[METRIC_FACTORS] = {node->geometry.source_aspect.horizontal};
    uint64_t vertical[METRIC_FACTORS] = {node->geometry.source_aspect.vertical};
    uint64_t *h = horizontal, *v = vertical;
    unsigned count = 1;
    for (unsigned n = 0; n < image->property_count; n++) {
        const struct x9_heif_property *p = &image->properties[n];
        const uint8_t *raw = NULL;
        struct transform op = {0};
        int e = x9_heif_span_data(plan->document,p->payload,&raw);
        if (e) return e;
        if (p->type == T('c','l','a','p')) {
            op.kind = CROP;
            e = aperture_axis(width,be32(raw),be32(raw+4),be32(raw+16),be32(raw+20),plan->clap_policy,
                &op.x,&op.width,&op.left,&op.aperture_width,&op.resample);
            if (!e) e = aperture_axis(height,be32(raw+8),be32(raw+12),be32(raw+24),be32(raw+28),plan->clap_policy,
                &op.y,&op.height,&op.top,&op.aperture_height,&op.resample);
        } else if (p->type == T('i','r','o','t')) {
            op.kind = ROTATE; op.argument = raw[0];
            op.width = op.argument & 1u ? height : width;
            op.height = op.argument & 1u ? width : height;
        } else if (p->type == T('i','m','i','r')) {
            op.kind = MIRROR; op.argument = raw[0]; op.width = width; op.height = height;
        }
        if (e) return e;
        if (op.kind) {
            if (h[0] && op.kind == CROP &&
                ((uint64_t)be32(raw) != (uint64_t)be32(raw+4)*op.width ||
                 (uint64_t)be32(raw+8) != (uint64_t)be32(raw+12)*op.height)) {
                /* Origin-only interpolation changes placement, not spacing.
                 * Both axes use exact, already-validated positive factors. */
                if (count > METRIC_FACTORS-3) return E2BIG;
                h[count] = be32(raw); h[count+1] = be32(raw+12); h[count+2] = op.height;
                v[count] = be32(raw+4); v[count+1] = op.width; v[count+2] = be32(raw+8);
                count += 3;
            } else if (op.kind == ROTATE && (op.argument & 1u)) {
                uint64_t *temporary = h; h = v; v = temporary;
            }
            node->transforms[node->transform_count++] = op;
            plan->resampling |= op.resample;
            width = op.width; height = op.height;
        }
    }
    if (h[0]) {
        /* Preserve the raw pair when no sampling metric changed. Otherwise
         * only the equivalent rational pair is significant. All factors are
         * positive, and at most three are appended per property association. */
        if (count > 1) for (unsigned i = 0; i < count; i++)
            for (unsigned j = 0; j < count; j++) {
                uint64_t common = gcd(h[i],v[j]); h[i] /= common; v[j] /= common;
            }
        int e = metric_product(h,count,&node->geometry.pixel_aspect_h);
        if (!e) e = metric_product(v,count,&node->geometry.pixel_aspect_v);
        if (e) return e;
    }
    node->geometry.width = width; node->geometry.height = height;
    return 0;
}
static int source_aspect(const struct x9_heif_image *image, const struct x9_heif_sample_aspect *aspect)
{
    /* Validate only own/coded-source resolution. IDENTITY_OUTPUT is assigned
     * later from a completed child, never trusted from the source callback. */
    uint64_t h = aspect->horizontal, v = aspect->vertical;
    if ((!h != !v) || h > INT64_MAX || v > INT64_MAX ||
        aspect->origin < X9_HEIF_ASPECT_ABSENT || aspect->origin > X9_HEIF_ASPECT_COREMEDIA ||
        ((aspect->origin == X9_HEIF_ASPECT_ABSENT) != !h)) return EBADMSG;
    if (image->pixel_aspect_h) {
        if (aspect->origin != X9_HEIF_ASPECT_PASP ||
            (__uint128_t)h*image->pixel_aspect_v != (__uint128_t)v*image->pixel_aspect_h) return EBADMSG;
    } else if (aspect->origin == X9_HEIF_ASPECT_PASP) return EBADMSG;
    return 0;
}
/* grid/iden/iovl place child OUTPUT samples in the canvas INPUT lattice, before
 * the canvas's own transforms. Explicit present ratios must agree on EVERY
 * dimg edge, not just tile0, raw pasp, shared config or the root declaration.
 * Absent-pasp identity preserves its unique child's completed output metric,
 * including presence. Grid/overlay do not infer an absent canvas metric or
 * reinterpret a mismatched lattice. Preserve its legacy absent/square path
 * without filling in1:1; missing evidence never admits a non-square placement. */
static int placement_metric(const struct x9_heif_geometry *canvas, uint64_t h, uint64_t v)
{
    uint64_t ch = canvas->source_aspect.horizontal, cv = canvas->source_aspect.vertical;
    if ((!h != !v) || h > INT64_MAX || v > INT64_MAX) return EBADMSG;
    if (canvas->source_aspect_item_id && (!!ch != !!h)) return EBADMSG;
    if (ch && h) return (__uint128_t)ch*v == (__uint128_t)cv*h ? 0 : ENOTSUP;
    return ch != cv || h != v ? ENOTSUP : 0;
}
static int visit(struct x9_heif_plan *p, uint32_t id, unsigned depth, uint32_t *index)
{
    /* Check active state first. Cached subtrees still contribute their full
     * height on a new path; DAG reuse must not bypass the recursion bound. */
    for (unsigned n = 0; n < p->count; n++) if (p->nodes[n].geometry.id == id) {
        if (p->nodes[n].state != 2) return ELOOP;
        if (depth >= X9_HEIF_MAX_DEPTH || p->nodes[n].subtree_height > X9_HEIF_MAX_DEPTH-depth) return E2BIG;
        *index = n; return 0;
    }
    if (depth >= X9_HEIF_MAX_DEPTH) return E2BIG;
    if (p->count == X9_HEIF_MAX_ITEMS) return ENOSPC;
    uint32_t at = p->count++;
    struct node *node = &p->nodes[at];
    node->geometry.id = id; node->state = 1; node->subtree_height = 1;
    struct x9_heif_image *image = calloc(1,sizeof(*image));
    if (!image) return ENOMEM;
    int e = x9_heif_get_image(p->document,id,image);
    if (!e && (image->width > X9_HEIF_MAX_DIMENSION || image->height > X9_HEIF_MAX_DIMENSION)) e = EMSGSIZE;
    /* Resolve actual container edges before even a metadata source callback.
     * pitm is not the associated HEVC PRIMARY; each master owns its pairing. */
    if (!e) e = x9_heif_resolve_alpha_association(p->document,id,&node->geometry.alpha);
    if (!e) {
        node->type = image->type; node->rows = image->rows; node->columns = image->columns;
        node->premultiplied = image->premultiplied_reference_count;
        node->auxiliary_source = image->auxiliary.present;
        if (node->type == X9_HEIF_IOVL) {
            p->overlays = 1;
            memcpy(node->overlay_x,image->overlay_x,sizeof(node->overlay_x));
            memcpy(node->overlay_y,image->overlay_y,sizeof(node->overlay_y));
        }
        node->geometry.coded_width = image->width; node->geometry.coded_height = image->height;
        node->geometry.derived_count = image->derived_count;
        struct x9_heif_sample_aspect aspect = { image->pixel_aspect_h,image->pixel_aspect_v,
            image->pixel_aspect_h ? X9_HEIF_ASPECT_PASP : X9_HEIF_ASPECT_ABSENT };
        if (node->type == X9_HEIF_HVC1 && p->aspect_source) {
            memset(&aspect,0,sizeof(aspect));
            e = p->aspect_source(p->aspect_context,image,&aspect);
        }
        if (!e) e = source_aspect(image,&aspect);
        if (!e) node->geometry.source_aspect = aspect;
    }
    for (unsigned n = 0; n < image->derived_count && !e; n++) {
        e = visit(p,image->derived[n],depth+1,&node->derived[n]);
        if (!e && p->nodes[node->derived[n]].subtree_height+1 > node->subtree_height)
            node->subtree_height = p->nodes[node->derived[n]].subtree_height+1;
    }
    if (!e && node->type == X9_HEIF_IDEN) {
        const struct x9_heif_geometry *g = &p->nodes[node->derived[0]].geometry;
        if (g->width != image->width || g->height != image->height) e = EBADMSG;
        else if (!image->pixel_aspect_h) {
            /* Identity copies EXACTLY the unique child's already-transformed
             * raster. Resolve this input before appending our own operations,
             * never use raw child pasp or mutate container/coded provenance.
             * The coded-source callback still cannot return this origin.
             * An absent child metric stays0/0, not an inferred square default. */
            node->geometry.source_aspect.horizontal = g->pixel_aspect_h;
            node->geometry.source_aspect.vertical = g->pixel_aspect_v;
            node->geometry.source_aspect.origin = g->pixel_aspect_h ?
                X9_HEIF_ASPECT_IDENTITY_OUTPUT : X9_HEIF_ASPECT_ABSENT;
            node->geometry.source_aspect_item_id = g->id;
        }
    }
    if (!e && node->type == X9_HEIF_GRID) {
        const struct x9_heif_geometry *tile = &p->nodes[node->derived[0]].geometry;
        uint64_t full_w = (uint64_t)node->columns*tile->width;
        uint64_t full_h = (uint64_t)node->rows*tile->height;
        if (image->width > full_w || image->height > full_h ||
            image->width <= full_w-tile->width || image->height <= full_h-tile->height) e = EBADMSG;
        for (unsigned n = 1; n < image->derived_count && !e; n++) {
            const struct x9_heif_geometry *other = &p->nodes[node->derived[n]].geometry;
            if (tile->width != other->width || tile->height != other->height) e = EBADMSG;
        }
    }
    /* Each node's transforms are calculated once, after child OUTPUT geometry
     * and any identity input metric are final. Cached DAG nodes retain that
     * immutable result; no replay/duplicate append or stale resampling flags. */
    if (!e) e = transforms(p,image,node);
    for (unsigned n = 0; n < X9_HEIF_MAX_ITEMS && !e; n++) {
        uint32_t auxiliary = 0;
        e = x9_heif_auxiliary(p->document,id,n,&auxiliary);
        if (e == ENOENT) { e = 0; break; }
        if (!e) e = visit(p,auxiliary,depth+1,&node->auxiliary[n]);
        if (!e) {
            node->geometry.auxiliary_count++;
            if (p->nodes[node->auxiliary[n]].subtree_height+1 > node->subtree_height)
                node->subtree_height = p->nodes[node->auxiliary[n]].subtree_height+1;
        }
    }
    free(image);
    if (!e) { node->state = 2; *index = at; }
    return e;
}
static int plan_create(const struct x9_heif_document *document, uint32_t id,
    enum x9_heif_clap_policy policy, x9_heif_aspect_source source, void *context, struct x9_heif_plan **out)
{
    if (!document || !out || *out || (policy != X9_HEIF_CLAP_EXACT_SAMPLES &&
        policy != X9_HEIF_CLAP_LINEAR_CEIL)) return EINVAL;
    struct x9_heif_plan *p = calloc(1,sizeof(*p));
    if (!p) return ENOMEM;
    p->document = document; p->clap_policy = policy;
    p->aspect_source = source; p->aspect_context = context;
    int e = id ? 0 : x9_heif_primary(document,&id);
    if (!e) e = visit(p,id,0,&p->root);
    p->aspect_source = NULL; p->aspect_context = NULL;
    /* Validate the WHOLE reachable graph before any raster callback. Each
     * grid/iden/iovl edge consumes transformed child samples in the declared
     * pre-transform canvas metric. DAG reuse never skips a new parent edge.
     * Only absent-pasp identity preserves a unique exact-size child's output;
     * no grid/overlay inheritance, mixed-lattice resampling or square default.
     * Overlay offsets/clipping are sample coordinates, not physical lengths;
     * matching non-square ratios need no offset scaling. Auxiliary placement
     * remains unsupported. Check even fully clipped overlay edges here. */
    for (unsigned n = 0; n < p->count && !e; n++) {
        const struct node *node = &p->nodes[n];
        if (node->type == X9_HEIF_GRID || node->type == X9_HEIF_IDEN ||
            node->type == X9_HEIF_IOVL) {
            for (unsigned k = 0; k < node->geometry.derived_count && !e; k++) {
                const struct x9_heif_geometry *child = &p->nodes[node->derived[k]].geometry;
                e = placement_metric(&node->geometry,child->pixel_aspect_h,child->pixel_aspect_v);
            }
        }
        for (unsigned k = 0; k < node->geometry.auxiliary_count && !e; k++) {
            const struct x9_heif_geometry *aux = &p->nodes[node->auxiliary[k]].geometry;
            if (aux->source_aspect.horizontal != aux->source_aspect.vertical ||
                aux->pixel_aspect_h != aux->pixel_aspect_v) e = ENOTSUP;
        }
    }
    if (e) free(p); else *out = p;
    return e;
}
int x9_heif_plan_create_with_policy(const struct x9_heif_document *document, uint32_t id,
    enum x9_heif_clap_policy policy, struct x9_heif_plan **out)
{ return plan_create(document,id,policy,NULL,NULL,out); }
int x9_heif_plan_create_with_source_aspects(const struct x9_heif_document *document, uint32_t id,
    enum x9_heif_clap_policy policy, x9_heif_aspect_source source, void *context, struct x9_heif_plan **out)
{ return source ? plan_create(document,id,policy,source,context,out) : EINVAL; }
int x9_heif_plan_create(const struct x9_heif_document *document, uint32_t id,
    struct x9_heif_plan **out)
{ return x9_heif_plan_create_with_policy(document,id,X9_HEIF_CLAP_EXACT_SAMPLES,out); }
int x9_heif_plan_requires_resampling(const struct x9_heif_plan *p)
{ return p ? (int)p->resampling : -EINVAL; }
int x9_heif_plan_requires_linear_composition(const struct x9_heif_plan *p)
{ return p ? !!(p->resampling || p->overlays) : -EINVAL; }
void x9_heif_plan_close(struct x9_heif_plan **p)
{ if (p && *p) { free(*p); *p = NULL; } }
int x9_heif_plan_geometry(const struct x9_heif_plan *p, uint32_t id,
    struct x9_heif_geometry *out)
{
    if (!p || !out) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!id) id = p->nodes[p->root].geometry.id;
    for (unsigned n = 0; n < p->count; n++) if (p->nodes[n].geometry.id == id) {
        *out = p->nodes[n].geometry; return 0;
    }
    return ENOENT;
}
static unsigned pixel_bytes(enum x9_heif_raster_format format)
{
    switch (format) {
    case X9_HEIF_RGBA8: return 4;
    case X9_HEIF_RGBA16LE: case X9_HEIF_RGBA_HALF: return 8;
    case X9_HEIF_RGBA_FLOAT: return 16;
    case X9_HEIF_GRAY8: return 1;
    case X9_HEIF_GRAY16LE: return 2;
    default: return 0;
    }
}
struct composition {
    const struct x9_heif_plan *plan;
    enum x9_heif_raster_format format;
    unsigned pixel_bytes;
    int linear;
    size_t live_bytes, budget;
    x9_heif_raster_source source;
    void *context;
    struct x9_heif_raster cache[X9_HEIF_MAX_ITEMS];
};
void x9_heif_raster_close(struct x9_heif_raster *r)
{ if (r) { free(r->data); memset(r,0,sizeof(*r)); } }
static int allocate(struct composition *c, uint32_t w, uint32_t h, struct x9_heif_raster *r)
{
    uint64_t stride = (uint64_t)w*c->pixel_bytes, bytes = stride*h;
    if (!w || !h || w > X9_HEIF_MAX_DIMENSION || h > X9_HEIF_MAX_DIMENSION ||
        c->live_bytes > c->budget || bytes > c->budget-c->live_bytes) return EMSGSIZE;
    uint8_t *data = calloc(1,(size_t)bytes);
    if (!data) return ENOMEM;
    *r = (struct x9_heif_raster){ .width = w, .height = h, .format = (uint32_t)c->format,
        .stride = (size_t)stride, .bytes = (size_t)bytes, .data = data };
    c->live_bytes += (size_t)bytes; return 0;
}
static void release(struct composition *c, struct x9_heif_raster *r)
{ c->live_bytes -= r->bytes; x9_heif_raster_close(r); }
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
    FLT_MAX_EXP == 128, "linear raster requires IEEE binary32");
/* Use owned byte loads, not alignment/aliasing/endian assumptions about a
 * callback buffer. RGBA_FLOAT's wire component encoding is little-endian. */
static float component(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
    float value; memcpy(&value,&bits,sizeof(value)); return value;
}
static void put_component(uint8_t *p, float value)
{
    uint32_t bits; memcpy(&bits,&value,sizeof(bits));
    for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(bits>>(8*n));
}
static int validate_linear(const struct x9_heif_raster *r)
{
    for (uint32_t y = 0; y < r->height; y++)
        for (uint32_t x = 0; x < r->width; x++)
            for (unsigned k = 0; k < 4; k++) {
                float v = component(r->data+y*r->stride+(size_t)x*16+4*k);
                if (!isfinite(v) || (k == 3 && (v < 0 || v > 1))) return EBADMSG;
                if (k < 3 && component(r->data+y*r->stride+(size_t)x*16+12) == 0 && v != 0)
                    return EBADMSG;
            }
    return 0;
}
static void sample_axis(double position, uint32_t extent, uint32_t *low,
    uint32_t *high, double *weight)
{
    /* Source centers are at i+0.5. The validated aperture is inside the
     * source's [0,extent] support; extend edge samples, never read outside. */
    double index = position-0.5;
    if (index <= 0) { *low = *high = 0; *weight = 0; }
    else if (index >= extent-1) { *low = *high = extent-1; *weight = 0; }
    else { *low = (uint32_t)index; *high = *low+1; *weight = index-*low; }
}
static int resample(const struct transform *op, const struct x9_heif_raster *src,
    struct x9_heif_raster *dst)
{
    for (uint32_t y = 0; y < dst->height; y++) {
        uint32_t y0, y1; double ty;
        sample_axis(op->top+(y+0.5)*op->aperture_height/dst->height,src->height,&y0,&y1,&ty);
        for (uint32_t x = 0; x < dst->width; x++) {
            uint32_t x0, x1; double tx;
            sample_axis(op->left+(x+0.5)*op->aperture_width/dst->width,src->width,&x0,&x1,&tx);
            for (unsigned k = 0; k < 4; k++) {
                double a = component(src->data+y0*src->stride+(size_t)x0*16+4*k);
                double b = component(src->data+y0*src->stride+(size_t)x1*16+4*k);
                double d = component(src->data+y1*src->stride+(size_t)x0*16+4*k);
                double f = component(src->data+y1*src->stride+(size_t)x1*16+4*k);
                double top = a+(b-a)*tx, bottom = d+(f-d)*tx;
                double value = top+(bottom-top)*ty;
                if (!isfinite(value) || value < -FLT_MAX || value > FLT_MAX ||
                    (k == 3 && (value < 0 || value > 1))) return EBADMSG;
                put_component(dst->data+y*dst->stride+(size_t)x*16+4*k,(float)value);
            }
        }
    }
    return 0;
}
static int apply(struct composition *c, const struct transform *op, struct x9_heif_raster *r)
{
    /* Do not allocate/rewrite for identity operations. */
    if ((op->kind == ROTATE && !op->argument) ||
        (op->kind == CROP && !op->resample && !op->x && !op->y && op->width == r->width && op->height == r->height)) return 0;
    struct x9_heif_raster dst = {0};
    int e = allocate(c,op->width,op->height,&dst);
    if (e) return e;
    if (op->resample) {
        e = c->linear ? resample(op,r,&dst) : ENOTSUP;
        if (e) { release(c,&dst); return e; }
        release(c,r); *r = dst; return 0;
    }
    for (uint32_t y = 0; y < dst.height; y++) {
        if (op->kind == CROP) {
            memcpy(dst.data+y*dst.stride,r->data+(y+op->y)*r->stride+(size_t)op->x*c->pixel_bytes,dst.stride);
            continue;
        }
        for (uint32_t x = 0; x < dst.width; x++) {
            uint32_t sx = x, sy = y;
            if (op->kind == MIRROR) {
                /* imir0: mirror about vertical axis; imir1: horizontal. */
                if (!op->argument) sx = r->width-1-x;
                else sy = r->height-1-y;
            } else {
                /* irot counts anti-clockwise quarter turns. Inverse map
                 * destination to source, coordinates have downward +y. */
                switch (op->argument) {
                case 1: sx = r->width-1-y; sy = x; break;
                case 2: sx = r->width-1-x; sy = r->height-1-y; break;
                case 3: sx = y; sy = r->height-1-x; break;
                default: break;
                }
            }
            memcpy(dst.data+y*dst.stride+(size_t)x*c->pixel_bytes,
                r->data+sy*r->stride+(size_t)sx*c->pixel_bytes,c->pixel_bytes);
        }
    }
    release(c,r); *r = dst; return 0;
}
static int render(struct composition *c, uint32_t at)
{
    if (c->cache[at].data) return 0;
    const struct node *n = &c->plan->nodes[at];
    struct x9_heif_raster *r = &c->cache[at];
    int e = allocate(c,n->geometry.coded_width,n->geometry.coded_height,r);
    if (e) return e;
    if (n->type == X9_HEIF_HVC1 || n->type == X9_HEIF_IOVL) {
        /* iovl supplies the declared fill converted to the SAME explicit
         * linear working space as every child, not raw integer canvas RGB. */
        e = c->source(c->context,n->geometry.id,c->format,r->width,r->height,r->data,r->stride);
        if (!e && c->linear) e = validate_linear(r);
    }
    if (n->type != X9_HEIF_HVC1) {
        for (unsigned k = 0; k < n->geometry.derived_count && !e; k++) {
            if (n->type == X9_HEIF_IOVL) {
                const struct x9_heif_geometry *g = &c->plan->nodes[n->derived[k]].geometry;
                int64_t ox = n->overlay_x[k], oy = n->overlay_y[k];
                int64_t left = ox > 0 ? ox : 0, top = oy > 0 ? oy : 0;
                int64_t right = ox+g->width, bottom = oy+g->height;
                if (right > r->width) right = r->width;
                if (bottom > r->height) bottom = r->height;
                /* Signed64 intersection keeps negative16/32 offsets and
                 * additions from becoming huge unsigned placements. Fully
                 * invisible placements are legal and never read OOB. */
                if (left >= right || top >= bottom) continue;
                e = render(c,n->derived[k]);
                if (e) break;
                const struct x9_heif_raster *tile = &c->cache[n->derived[k]];
                /* Reused child rasters carry their completed OUTPUT metric.
                 * Compare with THIS canvas's pre-transform INPUT immediately
                 * before source-over; never use the child's raw pasp or the
                 * canvas's later crop/rotation metric to admit a placement. */
                e = placement_metric(&n->geometry,tile->pixel_aspect_h,tile->pixel_aspect_v);
                if (e) break;
                for (int64_t y = top; y < bottom && !e; y++)
                    for (int64_t x = left; x < right && !e; x++) {
                        const uint8_t *src = tile->data+(size_t)(y-oy)*tile->stride+(size_t)(x-ox)*16;
                        uint8_t *dst = r->data+(size_t)y*r->stride+(size_t)x*16;
                        double a = component(src+12);
                        for (unsigned q = 0; q < 4; q++) {
                            double value = component(src+4*q)+component(dst+4*q)*(1-a);
                            if (!isfinite(value) || value < -FLT_MAX || value > FLT_MAX ||
                                (q == 3 && (value < 0 || value > 1))) { e = EBADMSG; break; }
                            put_component(dst+4*q,(float)value);
                        }
                    }
                continue;
            }
            e = render(c,n->derived[k]);
            if (e) break;
            const struct x9_heif_raster *tile = &c->cache[n->derived[k]];
            e = placement_metric(&n->geometry,tile->pixel_aspect_h,tile->pixel_aspect_v);
            if (e) break;
            uint32_t x = n->type == X9_HEIF_GRID ? (k%n->columns)*tile->width : 0;
            uint32_t y = n->type == X9_HEIF_GRID ? (k/n->columns)*tile->height : 0;
            if (x >= r->width || y >= r->height) { e = EBADMSG; break; }
            uint32_t w = tile->width < r->width-x ? tile->width : r->width-x;
            uint32_t h = tile->height < r->height-y ? tile->height : r->height-y;
            for (uint32_t row = 0; row < h; row++)
                memcpy(r->data+(y+row)*r->stride+(size_t)x*c->pixel_bytes,
                    tile->data+row*tile->stride,(size_t)w*c->pixel_bytes);
        }
    }
    for (unsigned k = 0; k < n->transform_count && !e; k++) e = apply(c,&n->transforms[k],r);
    if (!e && c->linear) e = validate_linear(r);
    if (!e && (r->width != n->geometry.width || r->height != n->geometry.height)) e = EBADMSG;
    if (!e) {
        /* Publish every completed cached raster's OUTPUT metric, not just
         * root metadata. A parent's real sample-copy path consumes it above;
         * the canvas's own ordered transforms have already executed here. */
        r->pixel_aspect_h = n->geometry.pixel_aspect_h;
        r->pixel_aspect_v = n->geometry.pixel_aspect_v;
    }
    return e;
}
static int compose(const struct x9_heif_plan *plan, enum x9_heif_raster_format format,
    int linear, x9_heif_raster_source source, void *context, uint64_t budget, struct x9_heif_raster *out)
{
    if (!out) return EINVAL;
    /* Don't discard a caller's existing owned raster. */
    if (out->data) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!plan || !source || !pixel_bytes(format) || !budget || budget > SIZE_MAX) return EINVAL;
    if ((plan->resampling || plan->overlays) && !linear) return ENOTSUP;
    for (unsigned n = 0; n < plan->count; n++)
        if (plan->nodes[n].geometry.auxiliary_count || plan->nodes[n].premultiplied ||
            plan->nodes[n].auxiliary_source) return ENOTSUP;
    struct composition *c = calloc(1,sizeof(*c));
    if (!c) return ENOMEM;
    c->plan = plan; c->format = format; c->pixel_bytes = pixel_bytes(format); c->linear = linear;
    c->source = source; c->context = context; c->budget = (size_t)budget;
    int e = render(c,plan->root);
    if (!e) {
        *out = c->cache[plan->root];
        memset(&c->cache[plan->root],0,sizeof(*out));
    }
    for (unsigned n = 0; n < plan->count; n++) x9_heif_raster_close(&c->cache[n]);
    free(c); return e;
}
int x9_heif_compose_packed(const struct x9_heif_plan *plan, enum x9_heif_raster_format format,
    x9_heif_raster_source source, void *context, struct x9_heif_raster *out)
{ return compose(plan,format,0,source,context,X9_HEIF_MAX_RASTER_BYTES,out); }
int x9_heif_compose_packed_with_budget(const struct x9_heif_plan *plan, enum x9_heif_raster_format format,
    x9_heif_raster_source source, void *context, uint64_t budget, struct x9_heif_raster *out)
{ return compose(plan,format,0,source,context,budget,out); }
int x9_heif_compose_linear_float(const struct x9_heif_plan *plan,
    x9_heif_raster_source source, void *context, uint64_t budget, struct x9_heif_raster *out)
{ return compose(plan,X9_HEIF_RGBA_FLOAT,1,source,context,budget,out); }
