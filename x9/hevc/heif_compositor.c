#include "heif_compositor.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define T(a,b,c,d) X9_HEIF_TYPE(a,b,c,d)
enum operation { CROP = 1, ROTATE, MIRROR };
struct transform { uint32_t kind, argument, x, y, width, height; };
struct node {
    struct x9_heif_geometry geometry;
    uint32_t type, rows, columns, state, subtree_height, transform_count;
    uint32_t derived[X9_HEIF_MAX_ITEMS], auxiliary[X9_HEIF_MAX_ITEMS];
    struct transform transforms[X9_HEIF_MAX_ASSOCIATIONS];
};
struct x9_heif_plan {
    const struct x9_heif_document *document;
    uint32_t count, root;
    struct node nodes[X9_HEIF_MAX_ITEMS];
};
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int64_t signed32(uint32_t n)
{ return n & 0x80000000u ? (int64_t)n-4294967296LL : (int64_t)n; }
/* Aperture origin = (image - aperture)/2 + signed rational offset. With
 * dimensions gated at16384, these products fit int64 even for uint32
 * denominators. Require exact integer samples rather than truncating. */
static int aperture_axis(uint32_t image, uint32_t size_n, uint32_t size_d,
    uint32_t offset_n, uint32_t offset_d, uint32_t *origin, uint32_t *size)
{
    if (!size_n || !size_d || !offset_d) return EBADMSG;
    if (size_n % size_d) return ENOTSUP;
    uint32_t extent = size_n/size_d;
    if (!extent || extent > image) return EBADMSG;
    int64_t denominator = 2*(int64_t)offset_d;
    int64_t numerator = (int64_t)(image-extent)*offset_d + 2*signed32(offset_n);
    if (numerator % denominator) return ENOTSUP;
    int64_t start = numerator/denominator;
    if (start < 0 || start > (int64_t)(image-extent)) return EBADMSG;
    *origin = (uint32_t)start; *size = extent; return 0;
}
static int transforms(struct x9_heif_plan *plan, const struct x9_heif_image *image,
    struct node *node)
{
    uint32_t width = image->width, height = image->height;
    for (unsigned n = 0; n < image->property_count; n++) {
        const struct x9_heif_property *p = &image->properties[n];
        const uint8_t *raw = NULL;
        struct transform op = {0};
        int e = x9_heif_span_data(plan->document,p->payload,&raw);
        if (e) return e;
        if (p->type == T('c','l','a','p')) {
            op.kind = CROP;
            e = aperture_axis(width,be32(raw),be32(raw+4),be32(raw+16),be32(raw+20),&op.x,&op.width);
            if (!e) e = aperture_axis(height,be32(raw+8),be32(raw+12),be32(raw+24),be32(raw+28),&op.y,&op.height);
        } else if (p->type == T('i','r','o','t')) {
            op.kind = ROTATE; op.argument = raw[0];
            op.width = op.argument & 1u ? height : width;
            op.height = op.argument & 1u ? width : height;
        } else if (p->type == T('i','m','i','r')) {
            op.kind = MIRROR; op.argument = raw[0]; op.width = width; op.height = height;
        }
        if (e) return e;
        if (op.kind) {
            node->transforms[node->transform_count++] = op;
            width = op.width; height = op.height;
        }
    }
    node->geometry.width = width; node->geometry.height = height;
    return 0;
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
    if (!e) {
        node->type = image->type; node->rows = image->rows; node->columns = image->columns;
        node->geometry.coded_width = image->width; node->geometry.coded_height = image->height;
        node->geometry.derived_count = image->derived_count;
        e = transforms(p,image,node);
    }
    for (unsigned n = 0; n < image->derived_count && !e; n++) {
        e = visit(p,image->derived[n],depth+1,&node->derived[n]);
        if (!e && p->nodes[node->derived[n]].subtree_height+1 > node->subtree_height)
            node->subtree_height = p->nodes[node->derived[n]].subtree_height+1;
    }
    if (!e && node->type == X9_HEIF_IDEN) {
        const struct x9_heif_geometry *g = &p->nodes[node->derived[0]].geometry;
        if (g->width != image->width || g->height != image->height) e = EBADMSG;
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
int x9_heif_plan_create(const struct x9_heif_document *document, uint32_t id,
    struct x9_heif_plan **out)
{
    if (!document || !out || *out) return EINVAL;
    struct x9_heif_plan *p = calloc(1,sizeof(*p));
    if (!p) return ENOMEM;
    p->document = document;
    int e = id ? 0 : x9_heif_primary(document,&id);
    if (!e) e = visit(p,id,0,&p->root);
    if (e) free(p); else *out = p;
    return e;
}
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
    size_t live_bytes;
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
        c->live_bytes > X9_HEIF_MAX_RASTER_BYTES || bytes > X9_HEIF_MAX_RASTER_BYTES-c->live_bytes) return EMSGSIZE;
    uint8_t *data = calloc(1,(size_t)bytes);
    if (!data) return ENOMEM;
    *r = (struct x9_heif_raster){w,h,(uint32_t)c->format,(size_t)stride,(size_t)bytes,data};
    c->live_bytes += (size_t)bytes; return 0;
}
static void release(struct composition *c, struct x9_heif_raster *r)
{ c->live_bytes -= r->bytes; x9_heif_raster_close(r); }
static int apply(struct composition *c, const struct transform *op, struct x9_heif_raster *r)
{
    /* Do not allocate/rewrite for identity operations. */
    if ((op->kind == ROTATE && !op->argument) ||
        (op->kind == CROP && !op->x && !op->y && op->width == r->width && op->height == r->height)) return 0;
    struct x9_heif_raster dst = {0};
    int e = allocate(c,op->width,op->height,&dst);
    if (e) return e;
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
    if (n->type == X9_HEIF_HVC1) {
        e = c->source(c->context,n->geometry.id,c->format,r->width,r->height,r->data,r->stride);
    } else {
        for (unsigned k = 0; k < n->geometry.derived_count && !e; k++) {
            e = render(c,n->derived[k]);
            if (e) break;
            const struct x9_heif_raster *tile = &c->cache[n->derived[k]];
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
    return e;
}
int x9_heif_compose_packed(const struct x9_heif_plan *plan, enum x9_heif_raster_format format,
    x9_heif_raster_source source, void *context, struct x9_heif_raster *out)
{
    if (!out) return EINVAL;
    /* Don't discard a caller's existing owned raster. */
    if (out->data) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!plan || !source || !pixel_bytes(format)) return EINVAL;
    for (unsigned n = 0; n < plan->count; n++)
        if (plan->nodes[n].geometry.auxiliary_count) return ENOTSUP;
    struct composition *c = calloc(1,sizeof(*c));
    if (!c) return ENOMEM;
    c->plan = plan; c->format = format; c->pixel_bytes = pixel_bytes(format);
    c->source = source; c->context = context;
    int e = render(c,plan->root);
    if (!e) {
        *out = c->cache[plan->root];
        memset(&c->cache[plan->root],0,sizeof(*out));
    }
    for (unsigned n = 0; n < plan->count; n++) x9_heif_raster_close(&c->cache[n]);
    free(c); return e;
}
