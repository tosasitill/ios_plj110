#ifndef X9_HEIF_COREMEDIA_H
#define X9_HEIF_COREMEDIA_H
#include <CoreMedia/CoreMedia.h>
#include <stdint.h>
/* Build ONE coded item's ready CM sample from immutable HEIF bytes. item_id0
 * selects primary; grid/iden are resolved by the compositor, not mislabelled
 * as coded HEVC. Item data and parameter sets are copied into CM ownership.
 * info is required, returns ItemID/Width/Height, ordered raw Properties and
 * AuxiliaryIDs. Transform/clap/auxiliary properties are NOT applied by VT;
 * the caller must process them to deliver a complete image. Metadata is not
 * silently discarded to make a color tile look like a finished HEIF file.
 * This does NOT register/select hardware, decode, or fall back to Apple SW.
 * Main/Main10/Main-compatible still IDR gates match the native codec parser;
 * monochrome Rext remains an explicit unsupported coded-item path for now. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyItemSample(CFDataRef file,
    uint32_t item_id, CMSampleBufferRef *sample, CFDictionaryRef *info);
#endif
