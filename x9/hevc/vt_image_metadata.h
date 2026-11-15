/* Preserve CMPhoto/VT color interpretation without modifying decoded pixels. */
#ifndef X9_VT_IMAGE_METADATA_H
#define X9_VT_IMAGE_METADATA_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

/* The pixel buffer contains the already-cropped visible picture. Do not copy
 * the compressed description's coded-size clean aperture onto it a second
 * time. Color/ICC/HDR and sample-aspect metadata keep their original values;
 * no guessed BT.709 defaults or software tone mapping. Exact 24/4-byte HDR
 * payloads only; missing HDR clears stale HDR on this owned output buffer. */
OSStatus x9_hevc_attach_image_metadata(CMVideoFormatDescriptionRef,
                                     CVPixelBufferRef);
/* Read-only exact CM->callback HDR identity check, including absence. Never
 * repairs/relabels a borrowed VT pixel to make this validation succeed. */
OSStatus x9_hevc_validate_hdr_metadata(CMVideoFormatDescriptionRef, CVPixelBufferRef);
#endif
