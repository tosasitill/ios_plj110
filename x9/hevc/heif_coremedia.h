#ifndef X9_HEIF_COREMEDIA_H
#define X9_HEIF_COREMEDIA_H
#include <CoreMedia/CoreMedia.h>
#include <stdint.h>
#include "hevc_bitstream.h"
/* Build ONE coded item's ready CM sample from immutable HEIF bytes. item_id0
 * selects primary; grid/iden are resolved by the compositor, not mislabelled
 * as coded HEVC. Item data and parameter sets are copied into CM ownership.
 * info is required, returns ItemID/Width/Height, ordered raw Properties and
 * AuxiliaryIDs, selected BitstreamVUI and ResolvedColr. Container CICP2 or
 * missing fields use the picture's actual PPS->SPS, never CM defaults.
 * Transform/clap/auxiliary properties are NOT applied by VT;
 * the caller must process them to deliver a complete image. Metadata is not
 * silently discarded to make a color tile look like a finished HEIF file.
 * This does NOT register/select hardware, decode, or fall back to Apple SW.
 * Main/Main10/Main-compatible still IDR gates match the native codec parser;
 * monochrome Rext remains an explicit unsupported coded-item path for now. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyItemSample(CFDataRef file,
    uint32_t item_id, CMSampleBufferRef *sample, CFDictionaryRef *info);
/* Internal adapter also receives the parsed VUI without reconstructing it
 * from possibly synthesized CM extensions. Same owned outputs/gates. */
OSStatus x9_heif_copy_item_sample(CFDataRef file, uint32_t item_id,
    CMSampleBufferRef *sample, CFDictionaryRef *info, struct x9_hevc_vui *vui);
#endif
