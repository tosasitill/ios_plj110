/* Preserve CMPhoto/VT color interpretation without modifying decoded pixels. */
#ifndef X9_VT_IMAGE_METADATA_H
#define X9_VT_IMAGE_METADATA_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

/* The pixel buffer contains the already-cropped visible picture. Do not copy
 * the compressed description's coded-size clean aperture onto it a second
 * time. Color/ICC/HDR and sample-aspect metadata keep their original values;
 * no guessed BT.709 defaults or software tone mapping. */
OSStatus x9_hevc_attach_image_metadata(CMVideoFormatDescriptionRef,
                                     CVPixelBufferRef);
#endif
