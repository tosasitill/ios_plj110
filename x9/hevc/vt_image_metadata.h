/* Preserve CMPhoto/VT color interpretation without modifying decoded pixels. */
#ifndef X9_VT_IMAGE_METADATA_H
#define X9_VT_IMAGE_METADATA_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "hevc_bitstream.h"

/* Exact original CM/container bytes, before any SEI enrichment. Optional HDR
 * absent remains absent; all three fields are type/length checked before success.
 * Ambient illuminance/xy obey D.3.39's numeric constraints. Owned raw value
 * snapshot, cleared on failure; no inferred SEI provenance. */
OSStatus x9_hevc_read_hdr_description(CMVideoFormatDescriptionRef, struct x9_hevc_hdr *);

/* Owned immutable format snapshot: preserve every original extension and
 * merge only present resolved/persistent SEI137/144/148. CM/container and SEI must
 * agree byte-for-byte when both exist; absent SEI does not erase CM metadata.
 * Source format is never mutated. Cleared output on failure. */
OSStatus x9_hevc_copy_hdr_description(CMVideoFormatDescriptionRef,
                                    const struct x9_hevc_hdr *, CMVideoFormatDescriptionRef *);

/* The pixel buffer contains the already-cropped visible picture. Do not copy
 * the compressed description's coded-size clean aperture onto it a second
 * time. Color/ICC/HDR and sample-aspect metadata keep their original values;
 * no guessed BT.709 defaults or software tone mapping. Exact 24/4/8-byte HDR
 * payloads only; missing HDR clears stale HDR on this owned output buffer. */
OSStatus x9_hevc_attach_image_metadata(CMVideoFormatDescriptionRef,
                                     CVPixelBufferRef);
/* Read-only exact CM->callback HDR identity check, including absence. Never
 * repairs/relabels a borrowed VT pixel to make this validation succeed. */
OSStatus x9_hevc_validate_hdr_metadata(CMVideoFormatDescriptionRef, CVPixelBufferRef);
#endif
