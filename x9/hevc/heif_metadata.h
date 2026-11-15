#ifndef X9_HEIF_METADATA_H
#define X9_HEIF_METADATA_H
#include "heif_items.h"
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
#endif
