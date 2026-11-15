/* Preserve CMPhoto/VT color interpretation without modifying decoded pixels. */
#ifndef X9_VT_IMAGE_METADATA_H
#define X9_VT_IMAGE_METADATA_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "hevc_bitstream.h"

/* Raw SEI147 code point and its independent standard CM alternative string.
 * Unknown/unspecified code points stay raw, never become invented labels.
 * Merge is conflict-checked and allocation-atomic; no ordinary transfer
 * overwrite here. These helpers return errno values, not OSStatus. */
int x9_hevc_merge_preferred_transfer(CFMutableDictionaryRef, const struct x9_hevc_hdr *);
/* Owned TransferFunction/Gamma dictionary: preferred CM/raw value when
 * present, ordinary source value otherwise. Raw unknown clears the effective
 * transfer instead of falling back to an explicitly superseded SPS value.
 * CICP4/5 carry exact gamma2.2/2.8. No inferred raw code from CFString aliases. */
int x9_hevc_copy_transfer_extensions(CFDictionaryRef, CFDictionaryRef *);

/* Exact original CM/container bytes, before any SEI enrichment. Optional HDR
 * absent remains absent; all three CFData fields and optional raw preferred
 * transfer code/standard alternative string are validated before success.
 * Ambient illuminance/xy obey D.3.39's numeric constraints. Owned raw value
 * snapshot, cleared on failure; no inferred SEI provenance. */
OSStatus x9_hevc_read_hdr_description(CMVideoFormatDescriptionRef, struct x9_hevc_hdr *);

/* Owned immutable format snapshot: preserve every original extension and
 * merge only present resolved/persistent SEI137/144/147/148. CM/container and SEI must
 * agree byte-for-byte when both exist; absent SEI does not erase CM metadata.
 * Source format is never mutated. Cleared output on failure. */
OSStatus x9_hevc_copy_hdr_description(CMVideoFormatDescriptionRef,
                                    const struct x9_hevc_hdr *, CMVideoFormatDescriptionRef *);

/* The pixel buffer contains the already-cropped visible picture. Do not copy
 * the compressed description's coded-size clean aperture onto it a second
 * time. Color/ICC/HDR and sample-aspect metadata keep their original values;
 * Preferred transfer is resolved into this owned buffer's transfer/gamma, not
 * merely kept as an unused CM alternative key. No guessed BT.709 defaults
 * or software tone mapping. Exact 24/4/8-byte HDR
 * payloads only; missing HDR clears stale HDR on this owned output buffer. */
OSStatus x9_hevc_attach_image_metadata(CMVideoFormatDescriptionRef,
                                     CVPixelBufferRef);
/* Read-only exact CM->callback HDR and effective preferred-transfer/gamma
 * identity check, including absence. Never
 * repairs/relabels a borrowed VT pixel to make this validation succeed. */
OSStatus x9_hevc_validate_hdr_metadata(CMVideoFormatDescriptionRef, CVPixelBufferRef);
#endif
