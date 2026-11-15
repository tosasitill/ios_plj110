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
#define X9_HEIF_TYPE(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define X9_HEIF_HVC1 X9_HEIF_TYPE('h','v','c','1')
#define X9_HEIF_GRID X9_HEIF_TYPE('g','r','i','d')
#define X9_HEIF_IDEN X9_HEIF_TYPE('i','d','e','n')
struct x9_heif_document;
struct x9_heif_span { uint64_t offset, bytes; };
struct x9_heif_property {
    uint32_t type, essential;
    struct x9_heif_span payload; /* excludes box header, includes full-box prefix */
};
struct x9_heif_image {
    uint32_t id, type, width, height, hidden;
    uint32_t channels, bit_depth[4]; /* raw pixi; absent remains0 */
    uint32_t rows, columns, derived_count;
    uint32_t derived[X9_HEIF_MAX_ITEMS]; /* ordered dimg, no dedup/sorting */
    uint32_t property_count;
    struct x9_heif_property properties[X9_HEIF_MAX_ASSOCIATIONS];
    struct x9_heif_span hvcc; /* coded image only; unchanged hvcC payload */
    uint64_t item_bytes;
};
/* Data must remain resident and immutable until close, across every read and
 * image lookup. This parser owns its tables, never the input. A successful
 * open proves structure only; get_image/read_item check the selected item.
 * Protected/external/method2 items are retained, then explicitly ENOTSUP on
 * selection rather than incorrectly treated as file/idat offsets. */
int x9_heif_open(const uint8_t *data, size_t bytes, struct x9_heif_document **);
void x9_heif_close(struct x9_heif_document **);
int x9_heif_primary(const struct x9_heif_document *, uint32_t *id);
/* Borrow a bounded metadata span from THIS document. Returned bytes retain
 * the document's immutable input lifetime; no second unverified file pointer. */
int x9_heif_span_data(const struct x9_heif_document *, struct x9_heif_span,
    const uint8_t **data);
/* Resolve hvc1/grid/iden geometry, ordered dimg and all associated properties.
 * Unknown essential properties fail ENOTSUP; known transform/color/auxiliary
 * properties are retained, NOT applied. Compositors must process them before
 * claiming an image. Missing/duplicate required properties fail closed. */
int x9_heif_get_image(const struct x9_heif_document *, uint32_t id, struct x9_heif_image *);
/* Concatenate bounded method0 file extents or method1 idat extents in their
 * recorded order, including base offsets. NULL destination queries exact size
 * after the SAME bounds checks. No hidden read of external data references. */
int x9_heif_read_item(const struct x9_heif_document *, uint32_t id,
    uint8_t *destination, size_t capacity, size_t *written);
/* Enumerate auxl source items referencing master, including non-alpha aux.
 * No auxiliary is silently merged/discarded; caller resolves auxC from its
 * own ordered properties. ENOENT is end-of-enumeration. */
int x9_heif_auxiliary(const struct x9_heif_document *, uint32_t master,
    uint32_t index, uint32_t *id);
#endif
