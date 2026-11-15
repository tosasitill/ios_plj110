#include "heif_items.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define T(a,b,c,d) X9_HEIF_TYPE(a,b,c,d)
struct box { uint32_t type; uint64_t payload, end; };
struct cursor { const uint8_t *data; uint64_t at, end; };
struct extent { uint64_t offset, bytes, index; };
struct item {
    uint32_t id, type, hidden, protection, data_reference, method;
    uint32_t info_seen, location_seen, extent_count, association_count;
    uint32_t index_size, iloc_groups, iloc_first, iloc_count;
    uint64_t base;
    struct extent extents[X9_HEIF_MAX_EXTENTS];
    uint16_t associations[X9_HEIF_MAX_ASSOCIATIONS]; /* bit15 essential */
};
struct reference { uint32_t type, from, to; };
struct x9_heif_document {
    const uint8_t *data;
    uint64_t bytes;
    uint32_t primary, item_count, property_count, reference_count;
    struct x9_heif_span idat;
    uint32_t idat_seen;
    struct item items[X9_HEIF_MAX_ITEMS];
    struct x9_heif_property properties[X9_HEIF_MAX_PROPERTIES];
    struct reference references[X9_HEIF_MAX_REFERENCES];
};
static int integer(struct cursor *c, unsigned bytes, uint64_t *out)
{
    if (bytes > 8 || c->at > c->end || bytes > c->end-c->at) return EBADMSG;
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) v = (v << 8) | c->data[c->at++];
    *out = v; return 0;
}
static int u32(struct cursor *c, unsigned bytes, uint32_t *out)
{
    uint64_t v = 0;
    int e = bytes > 4 ? EINVAL : integer(c,bytes,&v);
    if (!e) *out = (uint32_t)v;
    return e;
}
static int full(struct cursor *c, uint32_t *version, uint32_t *flags)
{
    int e = u32(c,1,version);
    return e ? e : u32(c,3,flags);
}
static int next_box(struct cursor *c, struct box *b)
{
    uint64_t start = c->at, size = 0;
    int e = integer(c,4,&size);
    int to_end = !size;
    if (!e) e = u32(c,4,&b->type);
    if (!e && size == 1) e = integer(c,8,&size);
    if (e) return e;
    if (to_end) size = c->end-start;
    if (size < c->at-start || size > c->end-start) return EBADMSG;
    b->payload = c->at; b->end = start+size; c->at = b->end;
    return 0;
}
static struct cursor payload(const struct x9_heif_document *d, const struct box *b)
{ return (struct cursor){d->data,b->payload,b->end}; }
static int finish(const struct cursor *c) { return c->at == c->end ? 0 : EBADMSG; }
static struct item *item_add(struct x9_heif_document *d, uint32_t id)
{
    if (!id) return NULL;
    for (unsigned i = 0; i < d->item_count; i++) if (d->items[i].id == id) return &d->items[i];
    if (d->item_count == X9_HEIF_MAX_ITEMS) return NULL;
    struct item *i = &d->items[d->item_count++]; i->id = id; return i;
}
static const struct item *item_find(const struct x9_heif_document *d, uint32_t id)
{
    for (unsigned i = 0; i < d->item_count; i++) if (d->items[i].id == id) return &d->items[i];
    return NULL;
}
static int parse_infe(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t version = 0, flags = 0, id = 0, protection = 0, type = 0;
    int e = full(&c,&version,&flags);
    if (!e && (version != 2 && version != 3)) e = ENOTSUP;
    if (!e && (flags & ~1u)) e = ENOTSUP;
    if (!e) e = u32(&c,version == 2 ? 2u : 4u,&id);
    if (!e) e = u32(&c,2,&protection);
    if (!e) e = u32(&c,4,&type);
    if (!e && (!id || !type)) e = EBADMSG;
    if (e) return e;
    /* Every infe v2/3 has a terminated item_name; MIME/URI tails remain
     * opaque because they cannot be selected as an hvc1/grid/iden image. */
    const uint8_t *zero = memchr(d->data+c.at,0,(size_t)(c.end-c.at));
    if (!zero) return EBADMSG;
    c.at = (uint64_t)(zero-d->data)+1;
    if ((type == X9_HEIF_HVC1 || type == X9_HEIF_GRID || type == X9_HEIF_IDEN) && finish(&c))
        return EBADMSG;
    struct item *i = item_add(d,id);
    if (!i) return ENOSPC;
    if (i->info_seen) return EEXIST;
    i->info_seen = 1; i->type = type; i->protection = protection; i->hidden = flags & 1;
    return 0;
}
static int parse_iinf(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t version = 0, flags = 0, count = 0;
    int e = full(&c,&version,&flags);
    if (!e && (version > 1 || flags)) e = ENOTSUP;
    if (!e) e = u32(&c,version ? 4u : 2u,&count);
    if (!e && count > X9_HEIF_MAX_ITEMS) e = ENOSPC;
    for (unsigned n = 0; n < count && !e; n++) {
        struct box child;
        e = next_box(&c,&child);
        if (!e && child.type != T('i','n','f','e')) e = EBADMSG;
        if (!e) e = parse_infe(d,&child);
    }
    return e ? e : finish(&c);
}
static int parse_iloc(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t version = 0, flags = 0, a = 0, z = 0, count = 0;
    int e = full(&c,&version,&flags);
    if (!e && (version > 2 || flags)) e = ENOTSUP;
    if (!e) e = u32(&c,1,&a);
    if (!e) e = u32(&c,1,&z);
    unsigned offset_size = a >> 4, length_size = a & 15u, base_size = z >> 4;
    unsigned index_size = version ? z & 15u : 0;
    if (!e && (offset_size > 8 || length_size > 8 || base_size > 8 || index_size > 8 ||
        (!version && (z & 15u)))) e = ENOTSUP;
    if (!e) e = u32(&c,version == 2 ? 4u : 2u,&count);
    if (!e && count > X9_HEIF_MAX_ITEMS) e = ENOSPC;
    for (unsigned n = 0; n < count && !e; n++) {
        uint32_t id = 0, method = 0, ref = 0, extents = 0;
        uint64_t base = 0;
        e = u32(&c,version == 2 ? 4u : 2u,&id);
        if (!e && version) e = u32(&c,2,&method);
        if (!e && (!id || (method & ~15u))) e = EBADMSG;
        if (!e) e = u32(&c,2,&ref);
        if (!e) e = integer(&c,base_size,&base);
        if (!e) e = u32(&c,2,&extents);
        if (!e && extents > X9_HEIF_MAX_EXTENTS) e = ENOSPC;
        if (e) break;
        struct item *i = item_add(d,id);
        if (!i) { e = ENOSPC; break; }
        if (i->location_seen) { e = EEXIST; break; }
        i->location_seen = 1; i->method = method; i->data_reference = ref;
        i->index_size = index_size;
        i->base = base; i->extent_count = extents;
        for (unsigned k = 0; k < extents && !e; k++) {
            struct extent *ex = &i->extents[k];
            if (index_size) e = integer(&c,index_size,&ex->index);
            if (!e) e = integer(&c,offset_size,&ex->offset);
            if (!e) e = integer(&c,length_size,&ex->bytes);
        }
    }
    return e ? e : finish(&c);
}
static int parse_iref(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t version = 0, flags = 0, groups = 0;
    int e = full(&c,&version,&flags);
    if (!e && (version > 1 || flags)) e = ENOTSUP;
    while (c.at < c.end && !e) {
        /* Empty reference groups still consume parser work and can make an
         * iloc ordinal ambiguous. Bound groups as well as stored edges. */
        if (++groups > X9_HEIF_MAX_REFERENCES) return ENOSPC;
        struct box child;
        uint32_t from = 0, count = 0;
        e = next_box(&c,&child);
        if (e) break;
        struct cursor r = payload(d,&child);
        e = u32(&r,version ? 4u : 2u,&from);
        if (!e) e = u32(&r,2,&count);
        if (!e && (!from || count > X9_HEIF_MAX_REFERENCES-d->reference_count)) e = EBADMSG;
        if (!e && child.type == T('i','l','o','c')) {
            struct item *source = item_add(d,from);
            if (!source) e = ENOSPC;
            else {
                /* Keep group boundaries: ordinal1 is not the first edge
                 * after blindly merging two potentially different groups. */
                if (++source->iloc_groups == 1) {
                    source->iloc_first = d->reference_count;
                    source->iloc_count = count;
                }
            }
        }
        for (unsigned k = 0; k < count && !e; k++) {
            uint32_t to = 0;
            e = u32(&r,version ? 4u : 2u,&to);
            if (!e && !to) e = EBADMSG;
            if (!e) d->references[d->reference_count++] = (struct reference){child.type,from,to};
        }
        if (!e) e = finish(&r);
    }
    return e ? e : finish(&c);
}
static int parse_ipco(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    int e = 0;
    while (c.at < c.end && !e) {
        struct box child;
        e = next_box(&c,&child);
        if (!e && d->property_count == X9_HEIF_MAX_PROPERTIES) e = ENOSPC;
        if (!e) d->properties[d->property_count++] = (struct x9_heif_property){
            child.type,0,{child.payload,child.end-child.payload}};
    }
    return e ? e : finish(&c);
}
static int parse_ipma(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t version = 0, flags = 0, count = 0;
    int e = full(&c,&version,&flags);
    if (!e && (version > 1 || (flags & ~1u))) e = ENOTSUP;
    if (!e) e = u32(&c,4,&count);
    if (!e && count > X9_HEIF_MAX_ITEMS) e = ENOSPC;
    for (unsigned n = 0; n < count && !e; n++) {
        uint32_t id = 0, associations = 0;
        e = u32(&c,version ? 4u : 2u,&id);
        if (!e) e = u32(&c,1,&associations);
        if (e) break;
        struct item *i = item_add(d,id);
        if (!i) { e = id ? ENOSPC : EBADMSG; break; }
        if (associations > X9_HEIF_MAX_ASSOCIATIONS-i->association_count) { e = ENOSPC; break; }
        for (unsigned k = 0; k < associations && !e; k++) {
            uint32_t a = 0;
            e = u32(&c,flags & 1u ? 2u : 1u,&a);
            if (!e) {
                uint32_t index = a & (flags & 1u ? 0x7fffu : 0x7fu);
                uint32_t essential = a & (flags & 1u ? 0x8000u : 0x80u);
                if (!index) { if (essential) e = EBADMSG; continue; }
                i->associations[i->association_count++] = (uint16_t)(index | (essential ? 0x8000u : 0));
            }
        }
    }
    return e ? e : finish(&c);
}
static int parse_iprp(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t ipco = 0, ipma = 0;
    int e = 0;
    while (c.at < c.end && !e) {
        struct box child;
        e = next_box(&c,&child);
        if (e) break;
        if (child.type == T('i','p','c','o')) e = ipco++ ? EEXIST : parse_ipco(d,&child);
        else if (child.type == T('i','p','m','a')) { ipma++; e = parse_ipma(d,&child); }
    }
    if (!e && (!ipco || !ipma)) e = EBADMSG;
    return e ? e : finish(&c);
}
static int parse_meta(struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t version = 0, flags = 0, seen = 0;
    int e = full(&c,&version,&flags);
    if (!e && (version || flags)) e = ENOTSUP;
    while (c.at < c.end && !e) {
        struct box child;
        unsigned bit = 0;
        e = next_box(&c,&child);
        if (e) break;
        switch (child.type) {
        case T('p','i','t','m'): bit = 1; break;
        case T('i','i','n','f'): bit = 2; break;
        case T('i','l','o','c'): bit = 4; break;
        case T('i','p','r','p'): bit = 8; break;
        case T('i','r','e','f'): bit = 16; break;
        case T('i','d','a','t'): bit = 32; break;
        default: break;
        }
        if (seen & bit) { e = EEXIST; break; }
        seen |= bit;
        if (bit == 1) {
            struct cursor p = payload(d,&child);
            e = full(&p,&version,&flags);
            if (!e && (version > 1 || flags)) e = ENOTSUP;
            if (!e) e = u32(&p,version ? 4u : 2u,&d->primary);
            if (!e && !d->primary) e = EBADMSG;
            if (!e) e = finish(&p);
        } else if (bit == 2) e = parse_iinf(d,&child);
        else if (bit == 4) e = parse_iloc(d,&child);
        else if (bit == 8) e = parse_iprp(d,&child);
        else if (bit == 16) e = parse_iref(d,&child);
        else if (bit == 32) {
            d->idat = (struct x9_heif_span){child.payload,child.end-child.payload};
            d->idat_seen = 1;
        }
    }
    if (!e && (seen & 15u) != 15u) e = EBADMSG;
    return e ? e : finish(&c);
}
static int parse_ftyp(const struct x9_heif_document *d, const struct box *b)
{
    struct cursor c = payload(d,b);
    uint32_t brand = 0, minor = 0;
    int matched = 0, e = u32(&c,4,&brand);
    if (!e) e = u32(&c,4,&minor);
    (void)minor;
    while (!e) {
        if (brand == T('h','e','i','c') || brand == T('h','e','i','x') || brand == T('m','i','f','1')) matched = 1;
        if (c.at == c.end) break;
        e = u32(&c,4,&brand);
    }
    return e ? e : matched ? 0 : ENOTSUP;
}
int x9_heif_open(const uint8_t *data, size_t bytes, struct x9_heif_document **out)
{
    if (!data || !out || *out || !bytes) return EINVAL;
    if (bytes > X9_HEIF_MAX_FILE) return EMSGSIZE;
    if (bytes > UINTPTR_MAX-(uintptr_t)data) return EINVAL;
    struct x9_heif_document *d = calloc(1,sizeof(*d));
    if (!d) return ENOMEM;
    d->data = data; d->bytes = bytes;
    struct cursor c = {data,0,bytes};
    uint32_t ftyp = 0, meta = 0;
    int e = 0;
    while (c.at < c.end && !e) {
        struct box b;
        e = next_box(&c,&b);
        if (e) break;
        if (b.type == T('f','t','y','p')) e = ftyp++ ? EEXIST : parse_ftyp(d,&b);
        else if (b.type == T('m','e','t','a')) e = meta++ ? EEXIST : parse_meta(d,&b);
    }
    if (!e && (!ftyp || !meta)) e = EBADMSG;
    for (unsigned n = 0; n < d->item_count && !e; n++) {
        const struct item *i = &d->items[n];
        if (!i->info_seen) e = EBADMSG;
        for (unsigned k = 0; k < i->association_count && !e; k++)
            if ((i->associations[k] & 0x7fffu) > d->property_count) e = EBADMSG;
    }
    for (unsigned n = 0; n < d->reference_count && !e; n++)
        if (!item_find(d,d->references[n].from) || !item_find(d,d->references[n].to)) e = EBADMSG;
    if (!e && !item_find(d,d->primary)) e = EBADMSG;
    if (e) free(d); else *out = d;
    return e;
}
void x9_heif_close(struct x9_heif_document **slot)
{ if (slot && *slot) { free(*slot); *slot = NULL; } }
int x9_heif_primary(const struct x9_heif_document *d, uint32_t *id)
{
    if (!d || !id) return EINVAL;
    *id = d->primary; return 0;
}
int x9_heif_span_data(const struct x9_heif_document *d, struct x9_heif_span span,
    const uint8_t **data)
{
    if (!data) return EINVAL;
    *data = NULL;
    if (!d) return EINVAL;
    if (span.offset > d->bytes || span.bytes > d->bytes-span.offset) return EBADMSG;
    *data = d->data+span.offset; return 0;
}
/* Resolve the logical DAG once per read. The document and its borrowed
 * source remain const and can be read concurrently. No cached whole-item
 * buffers or exponential assembly of a large source to take a small slice. */
#define FILE_SOURCE UINT32_MAX
struct read_extent { uint64_t offset, bytes; uint32_t source; };
struct read_node {
    uint32_t state, height, extent_count;
    uint64_t bytes;
    struct read_extent extents[X9_HEIF_MAX_EXTENTS];
};
struct item_read {
    const struct x9_heif_document *document;
    struct read_node nodes[X9_HEIF_MAX_ITEMS];
    struct x9_heif_span *spans;
    uint32_t count, capacity, visits;
    uint64_t bytes;
};
static int item_index(const struct x9_heif_document *d, uint32_t id, uint32_t *out)
{
    for (uint32_t n = 0; n < d->item_count; n++) if (d->items[n].id == id) {
        *out = n; return 0;
    }
    return ENOENT;
}
static int resolve_data(struct item_read *r, uint32_t index, uint32_t depth)
{
    const struct x9_heif_document *d = r->document;
    const struct item *i = &d->items[index];
    struct read_node *node = &r->nodes[index];
    if (node->state == 1) return ELOOP;
    if (depth >= X9_HEIF_MAX_ITEM_DATA_DEPTH) return E2BIG;
    if (node->state == 2)
        return node->height > X9_HEIF_MAX_ITEM_DATA_DEPTH-depth ? E2BIG : 0;
    if (i->method > 2 || i->data_reference || i->protection ||
        (i->method == 1 && !d->idat_seen)) return ENOTSUP;
    if (!i->location_seen || !i->extent_count) return ENODATA;
    if (i->method == 2) {
        if (!i->iloc_groups || !i->iloc_count) return EBADMSG;
        if (i->iloc_groups != 1) return ENOTSUP;
    }
    node->state = 1; node->height = 1; node->extent_count = i->extent_count;
    for (unsigned n = 0; n < i->extent_count; n++) {
        const struct extent *x = &i->extents[n];
        struct read_extent *out = &node->extents[n];
        uint64_t source_bytes = 0, source_offset = 0;
        out->source = FILE_SOURCE;
        if (i->method == 2) {
            uint64_t ordinal = i->index_size ? x->index : 1;
            if (!ordinal || ordinal > i->iloc_count) return EBADMSG;
            const struct reference *ref = &d->references[i->iloc_first+(uint32_t)ordinal-1];
            if (ref->type != T('i','l','o','c') || ref->from != i->id) return EBADMSG;
            int e = item_index(d,ref->to,&out->source);
            if (!e) e = resolve_data(r,out->source,depth+1);
            if (e) return e;
            const struct read_node *source = &r->nodes[out->source];
            source_bytes = source->bytes;
            if (source->height+1 > node->height) node->height = source->height+1;
        } else {
            if (x->index || !x->bytes) return ENOTSUP;
            struct x9_heif_span source = i->method == 1 ? d->idat : (struct x9_heif_span){0,d->bytes};
            source_bytes = source.bytes; source_offset = source.offset;
        }
        /* Method2's length0 denotes the entire source item. Do not silently
         * ignore base/offset, or change that into source_size-offset. */
        if (i->method == 2 && !x->bytes && (i->base || x->offset)) return ENOTSUP;
        out->bytes = x->bytes ? x->bytes : source_bytes;
        if (i->base > source_bytes || x->offset > source_bytes-i->base ||
            out->bytes > source_bytes-i->base-x->offset) return EBADMSG;
        out->offset = source_offset+i->base+x->offset;
        if (out->bytes > UINT64_MAX-node->bytes) return EBADMSG;
        node->bytes += out->bytes;
    }
    node->state = 2;
    return 0;
}
static int append_span(struct item_read *r, uint64_t offset, uint64_t bytes)
{
    if (!bytes) return 0;
    if (offset > r->document->bytes || bytes > r->document->bytes-offset) return EBADMSG;
    if (bytes > X9_HEIF_MAX_ITEM_DATA-r->bytes) return EMSGSIZE;
    if (r->count && r->spans[r->count-1].offset+r->spans[r->count-1].bytes == offset) {
        r->spans[r->count-1].bytes += bytes;
    } else {
        if (r->count == X9_HEIF_MAX_READ_SPANS) return E2BIG;
        if (r->count == r->capacity) {
            uint32_t capacity = r->capacity ? r->capacity*2 : X9_HEIF_MAX_EXTENTS;
            if (capacity > X9_HEIF_MAX_READ_SPANS) capacity = X9_HEIF_MAX_READ_SPANS;
            struct x9_heif_span *spans = realloc(r->spans,(size_t)capacity*sizeof(*spans));
            if (!spans) return ENOMEM;
            r->spans = spans; r->capacity = capacity;
        }
        r->spans[r->count++] = (struct x9_heif_span){offset,bytes};
    }
    r->bytes += bytes;
    return 0;
}
static int intersect_data(struct item_read *r, uint32_t index, uint64_t offset,
    uint64_t bytes, uint32_t depth)
{
    const struct read_node *node = &r->nodes[index];
    if (depth >= X9_HEIF_MAX_ITEM_DATA_DEPTH || ++r->visits > X9_HEIF_MAX_READ_VISITS) return E2BIG;
    if (node->state != 2 || offset > node->bytes || bytes > node->bytes-offset) return EBADMSG;
    if (!bytes) return 0;
    for (unsigned n = 0; n < node->extent_count && bytes; n++) {
        const struct read_extent *x = &node->extents[n];
        if (offset >= x->bytes) { offset -= x->bytes; continue; }
        uint64_t take = x->bytes-offset;
        if (take > bytes) take = bytes;
        int e = x->source == FILE_SOURCE ? append_span(r,x->offset+offset,take) :
            intersect_data(r,x->source,x->offset+offset,take,depth+1);
        if (e) return e;
        offset = 0; bytes -= take;
    }
    return bytes ? EBADMSG : 0;
}
int x9_heif_read_item(const struct x9_heif_document *d, uint32_t id,
    uint8_t *destination, size_t capacity, size_t *written)
{
    if (!written) return EINVAL;
    *written = 0;
    if (!d) return EINVAL;
    uint32_t index = 0;
    int e = item_index(d,id,&index);
    if (e) return e;
    struct item_read *r = calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    r->document = d;
    e = resolve_data(r,index,0);
    uint64_t total = r->nodes[index].bytes;
    if (!e && total > X9_HEIF_MAX_ITEM_DATA) e = EMSGSIZE;
    if (!e) e = intersect_data(r,index,0,total,0);
    if (!e && r->bytes != total) e = EBADMSG;
    if (e) goto done;
    *written = (size_t)total;
    if (!destination) goto done;
    if (capacity < total) { e = ENOSPC; goto done; }
    /* Reject overlap with borrowed input BEFORE any copy; a read must not
     * corrupt later extents, metadata or another image's immutable bytes. */
    uintptr_t start = (uintptr_t)destination, source = (uintptr_t)d->data;
    uintptr_t table = (uintptr_t)d;
    if (total > UINTPTR_MAX-start || d->bytes > UINTPTR_MAX-source || sizeof(*d) > UINTPTR_MAX-table ||
        (start < source+d->bytes && source < start+total) ||
        (start < table+sizeof(*d) && table < start+total)) { e = EINVAL; goto done; }
    size_t at = 0;
    for (unsigned n = 0; n < r->count; n++) {
        memcpy(destination+at,d->data+r->spans[n].offset,(size_t)r->spans[n].bytes);
        at += (size_t)r->spans[n].bytes;
    }
done:
    free(r->spans); free(r); return e;
}
int x9_heif_item_offset_reference(const struct x9_heif_document *d, uint32_t item,
    uint32_t index, uint32_t *id)
{
    if (!id) return EINVAL;
    *id = 0;
    if (!d || !item_find(d,item)) return EINVAL;
    uint32_t at = 0;
    for (unsigned n = 0; n < d->reference_count; n++) {
        const struct reference *r = &d->references[n];
        if (r->type == T('i','l','o','c') && r->from == item && at++ == index) {
            *id = r->to; return 0;
        }
    }
    return ENOENT;
}
static int property_parse(const struct x9_heif_document *d,
    const struct x9_heif_property *p, struct x9_heif_image *out, uint32_t *seen)
{
    struct cursor c = {d->data,p->payload.offset,p->payload.offset+p->payload.bytes};
    uint32_t version = 0, flags = 0, bit = 0;
    int e = 0;
    switch (p->type) {
    case T('i','s','p','e'):
        bit = 1; e = full(&c,&version,&flags);
        if (!e && (version || flags)) e = ENOTSUP;
        if (!e) e = u32(&c,4,&out->width);
        if (!e) e = u32(&c,4,&out->height);
        if (!e && (!out->width || !out->height)) e = EBADMSG;
        if (!e) e = finish(&c);
        break;
    case T('h','v','c','C'):
        bit = 2; out->hvcc = p->payload;
        if (!p->payload.bytes) e = EBADMSG;
        break;
    case T('p','i','x','i'):
        bit = 4; e = full(&c,&version,&flags);
        if (!e && (version || flags)) e = ENOTSUP;
        if (!e) e = u32(&c,1,&out->channels);
        if (!e && (!out->channels || out->channels > 4)) e = ENOTSUP;
        for (unsigned n = 0; n < out->channels && !e; n++) {
            e = u32(&c,1,&out->bit_depth[n]);
            if (!e && (!out->bit_depth[n] || out->bit_depth[n] > 32)) e = ENOTSUP;
        }
        if (!e) e = finish(&c);
        break;
    case T('i','r','o','t'): case T('i','m','i','r'):
        e = u32(&c,1,&flags);
        if (!e && (flags > (p->type == T('i','r','o','t') ? 3u : 1u))) e = EBADMSG;
        if (!e) e = finish(&c);
        break;
    case T('c','o','l','r'):
        e = u32(&c,4,&flags);
        if (!e && flags == T('n','c','l','x')) {
            if (c.end-c.at != 7 || (d->data[c.end-1] & 0x7fu)) e = EBADMSG;
        } else if (!e && (flags == T('r','I','C','C') || flags == T('p','r','o','f'))) {
            if (c.at == c.end) e = EBADMSG;
        } else if (!e && p->essential) e = ENOTSUP;
        break;
    case T('c','l','a','p'):
        if (c.end-c.at != 32) e = EBADMSG;
        /* Exact rational crop bytes stay ordered, including signed offsets.
         * This parser does not round/reinterpret them as an integer crop. */
        for (unsigned n = 0; n < 8 && !e; n++) {
            e = u32(&c,4,&flags);
            if (!e && ((n & 1u) || n == 0 || n == 2) && !flags) e = EBADMSG;
        }
        break;
    case T('a','u','x','C'):
        bit = 8; e = full(&c,&version,&flags);
        if (!e && (version || flags)) e = ENOTSUP;
        if (!e && (c.at == c.end || !d->data[c.at] ||
            !memchr(d->data+c.at,0,(size_t)(c.end-c.at)))) e = EBADMSG;
        break;
    default: if (p->essential) e = ENOTSUP; break;
    }
    if (!e && bit && (*seen & bit)) e = EEXIST;
    if (!e) *seen |= bit;
    return e;
}
int x9_heif_get_image(const struct x9_heif_document *d, uint32_t id, struct x9_heif_image *out)
{
    if (!out) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!d) return EINVAL;
    const struct item *i = item_find(d,id);
    if (!i) return ENOENT;
    if (i->protection || i->data_reference ||
        (i->type != X9_HEIF_HVC1 && i->type != X9_HEIF_GRID && i->type != X9_HEIF_IDEN)) return ENOTSUP;
    out->id = id; out->type = i->type; out->hidden = i->hidden;
    out->location_present = i->location_seen; out->construction_method = i->method;
    uint32_t seen = 0;
    uint64_t metadata_bytes = 0;
    int e = 0;
    for (unsigned n = 0; n < i->association_count && !e; n++) {
        uint32_t association = i->associations[n], index = association & 0x7fffu;
        struct x9_heif_property p = d->properties[index-1];
        p.essential = !!(association & 0x8000u);
        if (p.payload.bytes > X9_HEIF_MAX_ITEM_METADATA-metadata_bytes) { e = EMSGSIZE; break; }
        metadata_bytes += p.payload.bytes;
        out->properties[out->property_count++] = p;
        e = property_parse(d,&p,out,&seen);
    }
    if (!e && !(seen & 1u)) e = EBADMSG;
    if (!e && i->type == X9_HEIF_HVC1 && !(seen & 2u)) e = EBADMSG;
    if (!e && i->type != X9_HEIF_HVC1 && (seen & 2u)) e = EBADMSG;
    for (unsigned n = 0; n < d->reference_count && !e; n++) {
        const struct reference *r = &d->references[n];
        if (r->from != id || r->type != T('d','i','m','g')) continue;
        if (out->derived_count == X9_HEIF_MAX_ITEMS) e = ENOSPC;
        else out->derived[out->derived_count++] = r->to;
    }
    if (!e && i->type == X9_HEIF_HVC1 && out->derived_count) e = EBADMSG;
    if (!e && i->type == X9_HEIF_IDEN && out->derived_count != 1) e = EBADMSG;
    if (!e && i->type != X9_HEIF_IDEN) {
        size_t bytes = 0;
        e = x9_heif_read_item(d,id,NULL,0,&bytes);
        out->item_bytes = bytes;
    }
    if (!e && i->type == X9_HEIF_GRID) {
        uint8_t raw[12]; size_t bytes = 0;
        e = x9_heif_read_item(d,id,raw,sizeof(raw),&bytes);
        if (!e && (bytes < 4 || raw[0] || (raw[1] & ~1u))) e = EBADMSG;
        if (!e) {
            struct cursor c = {raw,4,bytes}; uint32_t width = 0, height = 0;
            out->rows = (uint32_t)raw[2]+1; out->columns = (uint32_t)raw[3]+1;
            e = u32(&c,raw[1] & 1u ? 4u : 2u,&width);
            if (!e) e = u32(&c,raw[1] & 1u ? 4u : 2u,&height);
            if (!e) e = finish(&c);
            if (!e && (width != out->width || height != out->height ||
                out->rows*out->columns != out->derived_count)) e = EBADMSG;
        }
    }
    if (e) memset(out,0,sizeof(*out));
    return e;
}
int x9_heif_auxiliary(const struct x9_heif_document *d, uint32_t master,
    uint32_t index, uint32_t *id)
{
    if (!id) return EINVAL;
    *id = 0;
    if (!d || !item_find(d,master)) return EINVAL;
    uint32_t at = 0;
    for (unsigned n = 0; n < d->item_count; n++) {
        uint32_t source = d->items[n].id;
        int matches = 0;
        for (unsigned k = 0; k < d->reference_count; k++) {
            const struct reference *r = &d->references[k];
            if (r->type == T('a','u','x','l') && r->from == source && r->to == master) matches = 1;
        }
        if (matches && at++ == index) { *id = source; return 0; }
    }
    return ENOENT;
}
