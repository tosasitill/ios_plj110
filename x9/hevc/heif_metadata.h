#ifndef X9_HEIF_METADATA_H
#define X9_HEIF_METADATA_H
#include "heif_items.h"
#include "hevc_bitstream.h"
#include <CoreMedia/CoreMedia.h>
/* Internal owned metadata shared by the CM sample and raster adapters.
 * Raw ordered properties, dimg and auxiliary IDs always survive. Optional
 * extensions receives both ICC and nclx without inventing their precedence. */
int x9_heif_copy_item_info(const struct x9_heif_document *,
    const struct x9_heif_image *, CFDictionaryRef *info, CFMutableDictionaryRef extensions);
/* Empty dictionary means no recognized color property, not a default color.
 * Known CICP mapping only; unknown code points remain in raw item info. */
int x9_heif_copy_color_extensions(const struct x9_heif_document *,
    const struct x9_heif_image *, CFDictionaryRef *extensions);
/* Container non-unspecified CICP wins per field. Only missing/CICP2 uses
 * the selected SPS. Unknown non-2 codes stay raw/unmapped, never fall back.
 * Range is container nclx if present, otherwise normative SPS inference.
 * The returned dictionary also has the real/default SPS chroma locations. */
int x9_heif_resolve_color_extensions(CFDictionaryRef container,
    const struct x9_hevc_vui *, CFDictionaryRef *extensions);
int x9_heif_copy_vui_info(const struct x9_hevc_vui *, CFDictionaryRef *info);
#endif
