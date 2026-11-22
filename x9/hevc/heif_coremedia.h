#ifndef X9_HEIF_COREMEDIA_H
#define X9_HEIF_COREMEDIA_H
#include <CoreMedia/CoreMedia.h>
#include <stdint.h>
#include "hevc_bitstream.h"
#include "heif_items.h"
/* Build ONE coded item's ready CM sample from immutable HEIF bytes. item_id0
 * selects primary; grid/iden/iovl are resolved by the compositor, not mislabelled
 * as coded HEVC. Item data and parameter sets are copied into CM ownership.
 * info is required, returns ItemID/Width/Height, ordered raw Properties and
 * AuxiliaryIDs, ConstructionMethod/LocationPresent, ordered ItemOffsetIDs,
 * selected BitstreamVUI, ResolvedColr, actual IRAPNALUnitType and raw
 * NoOutputOfPriorPicsFlag (not inferred NoRaslOutputFlag). Container CICP2 or
 * missing fields use the picture's actual PPS->SPS, never CM defaults.
 * This item's descriptive pasp uses the standard CM/CV sample-aspect keys,
 * preserving unsigned32 raw spacings in info and verifying the CM ratio.
 * Without pasp, a known ratio from the actual selected PPS->SPS is explicitly
 * requested/verified, not the catalog's representative SPS. Unspecified VUI
 * may retain an actually returned CM ratio with COREMEDIA-only provenance;
 * absent CM stays absent. Reserved/zero Extended_SAR without pasp rejects,
 * never guessed square. SourcePixelAspectRatio/Origin describe the actual
 * pre-item-transform CM sample separately from raw container properties.
 * It describes the coded sample, not a square-pixel conversion or a claim
 * that this untransformed sample is the fully rendered item.
 * Transform/clap/auxiliary properties are NOT applied by VT;
 * the caller must process them to deliver a complete image. Metadata is not
 * silently discarded to make a color tile look like a finished HEIF file.
 * This does NOT register/select hardware, decode, or fall back to Apple SW.
 * Main/Main10 standalone BLA/IDR/CRA gates match the native session parser;
 * Main-compatible Main Still Picture remains IDR-only. Each coded item must
 * decode in a fresh native session and drain; no leading/inter pictures.
 * monochrome Rext remains an explicit unsupported coded-item path for now. */
__attribute__((visibility("default"))) OSStatus X9HEIFCopyItemSample(CFDataRef file,
    uint32_t item_id, CMSampleBufferRef *sample, CFDictionaryRef *info);
/* Internal adapter also receives the parsed VUI and AU SEI without reconstructing them
 * from possibly synthesized CM extensions. Same owned outputs/gates;
 * optional aspect is the actual CM pair plus explicit source provenance. */
OSStatus x9_heif_copy_item_sample(CFDataRef file, uint32_t item_id,
    CMSampleBufferRef *sample, CFDictionaryRef *info, struct x9_hevc_vui *vui,
    struct x9_hevc_hdr *hdr, struct x9_heif_sample_aspect *aspect);
#endif
