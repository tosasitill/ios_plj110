/* Immutable HEIF item input, not an image/HEVC decoder. Item IDs are resolved
 * within one document; offsets refer ONLY to its borrowed immutable bytes.
 * Grid/identity/auxiliary relationships and ordered properties remain intact
 * for the image compositor. No hardware capability, pixels or color-space
 * conversion is inferred by parsing the container. */
#ifndef X9_HEIF_ITEMS_H
#define X9_HEIF_ITEMS_H
#include <stddef.h>
#include <stdint.h>
#define X9_HEIF_MAX_FILE (256u * 1024u * 1024u)
#define X9_HEIF_MAX_ITEMS 256u
#define X9_HEIF_MAX_PROPERTIES 512u
#define X9_HEIF_MAX_ASSOCIATIONS 32u
#define X9_HEIF_MAX_EXTENTS 32u
#define X9_HEIF_MAX_REFERENCES 1024u
#define X9_HEIF_MAX_ITEM_DATA (8u * 1024u * 1024u)
#define X9_HEIF_MAX_ITEM_METADATA (4u * 1024u * 1024u)
/* Independent item-data dependency/expansion implementation limits, not
 * hardware dimensions or the dimg/aux compositor's separate graph budget. */
#define X9_HEIF_MAX_ITEM_DATA_DEPTH 32u
#define X9_HEIF_MAX_READ_SPANS 65536u
#define X9_HEIF_MAX_READ_VISITS (X9_HEIF_MAX_READ_SPANS * X9_HEIF_MAX_ITEM_DATA_DEPTH)
#define X9_HEIF_TYPE(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define X9_HEIF_HVC1 X9_HEIF_TYPE('h','v','c','1')
#define X9_HEIF_GRID X9_HEIF_TYPE('g','r','i','d')
#define X9_HEIF_IDEN X9_HEIF_TYPE('i','d','e','n')
#define X9_HEIF_IOVL X9_HEIF_TYPE('i','o','v','l')
struct x9_heif_document;
struct x9_heif_span { uint64_t offset, bytes; };
struct x9_heif_property {
    uint32_t type, essential;
    struct x9_heif_span payload; /* excludes box header, includes full-box prefix */
};
struct x9_heif_image {
    uint32_t id, type, width, height, hidden;
    uint32_t location_present, construction_method;
    uint32_t channels, bit_depth[4]; /* raw pixi; absent remains0 */
    uint32_t pixel_aspect_h, pixel_aspect_v; /* raw pasp u32; absent remains0 */
    uint32_t rows, columns, derived_count;
    uint32_t derived[X9_HEIF_MAX_ITEMS]; /* ordered dimg, no dedup/sorting */
    uint16_t canvas_fill_rgba[4]; /* iovl: unmodified unsigned16 RGB+alpha */
    int32_t overlay_x[X9_HEIF_MAX_ITEMS], overlay_y[X9_HEIF_MAX_ITEMS];
    uint32_t premultiplied_reference_count; /* prem is never silently ignored */
    uint32_t property_count;
    struct x9_heif_property properties[X9_HEIF_MAX_ASSOCIATIONS];
    struct x9_heif_span hvcc; /* coded image only; unchanged hvcC payload */
    uint64_t item_bytes;
};
/* Data must remain resident and immutable until close, across every read and
 * image lookup. This parser owns its tables, never the input. A successful
 * open proves structure only; get_image/read_item check the selected item.
 * Protected/external/unknown-method items are retained, then ENOTSUP on
 * selection. Method2 follows item-data references, never file/idat offsets. */
int x9_heif_open(const uint8_t *data, size_t bytes, struct x9_heif_document **);
void x9_heif_close(struct x9_heif_document **);
int x9_heif_primary(const struct x9_heif_document *, uint32_t *id);
/* Borrow a bounded metadata span from THIS document. Returned bytes retain
 * the document's immutable input lifetime; no second unverified file pointer. */
int x9_heif_span_data(const struct x9_heif_document *, struct x9_heif_span,
    const uint8_t **data);
/* Resolve hvc1/grid/iden/iovl geometry, ordered dimg and all associated properties.
 * iovl v0 retains exact unsigned16 RGBA fill and signed16/32 placement offsets.
 * Unknown essential properties fail ENOTSUP; known transform/color/auxiliary
 * properties are retained, NOT applied. Compositors must process them before
 * claiming an image. Missing/duplicate required properties fail closed. */
int x9_heif_get_image(const struct x9_heif_document *, uint32_t id, struct x9_heif_image *);
/* Concatenate method0 file, method1 idat or method2 item-offset extents in
 * recorded order, including base offsets. Method2 extent_index is a 1-based
 * index into ONE ordered iref/iloc group; omitted index_size implies1, while
 * encoded index0 is invalid. Length0 for method2 names the entire referenced
 * item only with base/offset0; nonzero offset plus implicit length is ENOTSUP,
 * not a guessed source-to-end slice. All explicit-length ranges must fit.
 * Sources are recursively logical byte sequences, not their first extent.
 * A two-phase read resolves all dependencies/bounds/cycles/depth, then builds
 * bounded intersected file spans before any copy. DAG reuse is legal and its
 * full height is counted. Large source sequences need not be materialized;
 * selected output is limited to MAX_ITEM_DATA, spans/visits to the above caps.
 * NULL destination has the SAME validation/fragmentation limits as a copy.
 * No external reads, partial output, or mutation of document/input bytes.
 * Method0/1 implicit lengths and ambiguous duplicate iref/iloc groups remain
 * explicit ENOTSUP; invalid indices/ranges/overflow fail, never clamp. */
int x9_heif_read_item(const struct x9_heif_document *, uint32_t id,
    uint8_t *destination, size_t capacity, size_t *written);
/* Enumerate raw ordered iref/iloc destinations (including repetitions), not
 * dimg children or decoded auxiliary images. ENOENT ends enumeration. */
int x9_heif_item_offset_reference(const struct x9_heif_document *, uint32_t item,
    uint32_t index, uint32_t *id);
/* Raw ordered prem targets, including repetitions. This records a source
 * association; no color unpremultiplication or auxiliary decode is implied. */
int x9_heif_premultiplied_reference(const struct x9_heif_document *, uint32_t item,
    uint32_t index, uint32_t *id);
/* Enumerate auxl source items referencing master, including non-alpha aux.
 * No auxiliary is silently merged/discarded; caller resolves auxC from its
 * own ordered properties. ENOENT is end-of-enumeration. */
int x9_heif_auxiliary(const struct x9_heif_document *, uint32_t master,
    uint32_t index, uint32_t *id);
#endif
