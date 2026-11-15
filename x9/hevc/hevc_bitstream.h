/* Bounded HEVC configuration/independent-picture parser, shared with XNU.
 * This is a syntax gate, NOT a decoder. Only base-layer Main/Main10 4:2:0
 * are accepted; legacy independent-IDR, standalone IRAP and stateful AU gates
 * are separate.
 * Main-compatible 8-bit Main Still Picture
 * uses the Main decode path; monochrome Rext alpha returns ENOTSUP (no fallback).
 */
#ifndef X9_HEVC_BITSTREAM_H
#define X9_HEVC_BITSTREAM_H
#include <stdint.h>
#include <stddef.h>
#define X9_HEVC_MAX_CONFIG (64u * 1024u)
#define X9_HEVC_MAX_INPUT (8u * 1024u * 1024u)
#define X9_HEVC_MAX_DIM 4096u
#define X9_HEVC_PROFILE_MAIN 1u
#define X9_HEVC_PROFILE_MAIN10 2u
#define X9_HEVC_PROFILE_MAIN_STILL 3u
#define X9_HEVC_MAX_PARAMETER_SETS (16u + 16u + 64u)
struct x9_hevc_parameter_set {
    uint32_t offset, bytes;
    uint8_t type, id, reference_id, reserved;
};
struct x9_hevc_config {
    uint32_t coded_width, coded_height, width, height, crop_x, crop_y;
    uint32_t profile, bit_depth, level, length_size, vps_id, sps_id, pps_id;
    /* profile is the unmodified SPS/hvcC value. decoder_profile is the
     * required Main/Main10 capability, NOT a newly advertised HW profile.
     * Compatibility flags retain their big-endian bit numbering: profile 1
     * is bit30, not bit1. This struct is private, not the vdec ioctl ABI. */
    uint32_t decoder_profile, profile_compatibility;
    uint32_t ps_offset[3], ps_bytes[3], annexb_bytes;
    uint32_t max_sub_layers, max_dpb, max_reorder;
    /* All distinct VPS/SPS/PPS from the immutable hvcC, grouped in decoder
     * dependency order. IDs, not array order, establish PPS->SPS->VPS links.
     * ps_offset/ps_bytes and *_id above retain ONE representative linked
     * chain for legacy callers, not a claim that the catalog has one set.
     * All SPS must currently share output geometry/depth/profile/layer count;
     * dimension-changing parameter sets need the native format-change path.
     * DPB/reorder above are maxima across every retained SPS. */
    uint32_t config_bytes, parameter_set_count;
    struct x9_hevc_parameter_set parameter_sets[X9_HEVC_MAX_PARAMETER_SETS];
};
/* One selected SPS, never a VUI table in the kernel's full catalog. Presence
 * and inferred values are separate: absent colour description means CICP2,
 * not BT709; absent signal/range and chroma location infer limited/type0.
 * Display-window offsets are in chroma units, NOT another conformance crop. */
struct x9_hevc_vui {
    uint8_t present, aspect_ratio_present, aspect_ratio_idc, overscan_present;
    uint8_t overscan_appropriate, video_signal_present, video_format, full_range;
    uint8_t colour_description_present, colour_primaries, transfer, matrix;
    uint8_t chroma_location_present, chroma_top, chroma_bottom, neutral_chroma;
    uint8_t field_seq, frame_field_info_present, display_window_present, timing_present;
    uint8_t poc_proportional, hrd_present, restriction_present, tiles_fixed;
    uint8_t motion_over_boundaries, restricted_ref_lists, reserved[2];
    uint32_t sar_width, sar_height, display_window[4];
    uint32_t num_units_in_tick, time_scale, ticks_poc_diff_one_minus1;
    uint32_t min_spatial_segmentation, max_bytes_per_pic_denom, max_bits_per_min_cu_denom;
    uint32_t log2_max_mv_horizontal, log2_max_mv_vertical;
    uint32_t pps_id, sps_id, vps_id;
};
/* Returns 0 or a BSD errno. Rejects unsupported formats rather than inventing
 * capabilities. The VPS/SPS/PPS spans refer to the unchanged hvcC snapshot. */
int x9_hevc_config_parse(const uint8_t *, size_t, struct x9_hevc_config *);
/* NULL dst performs the same validation and reports required size. */
int x9_hevc_idr_annexb(const struct x9_hevc_config *, const uint8_t *hvcc,
    const uint8_t *sample, size_t sample_bytes, uint8_t *dst, size_t capacity,
    size_t *written);
/* Raw syntax facts, not inferred NoRaslOutputFlag or hardware completion. */
struct x9_hevc_irap_picture {
    uint32_t nal_type, pps_id, no_output_of_prior_pics_flag;
};
/* Standalone base-layer BLA(16..18)/IDR(19..20)/CRA(21), TemporalId=0,
 * exactly one picture, matching slice type/PPS/no_output_of_prior_pics_flag.
 * Main-compatible Main Still Picture remains IDR-only. Checks bounded AU
 * framing/catalog plus AUD/prefix/VCL/suffix/EOS/EOB ordering; no leading or
 * inter pictures are accepted. Optional picture and required written are
 * cleared on failure. NULL dst validates and reports the AU's Annex B size.
 * Unlike legacy IDR, this does NOT prepend parameter sets: use a FRESH native
 * session initialized with the full catalog and DRAIN for this one picture.
 * Firmware must implement random-access/POC/DPB semantics; syntax cannot
 * establish those capabilities or make a pending CRA into a decoded frame. */
int x9_hevc_irap_annexb(const struct x9_hevc_config *, const uint8_t *hvcc,
    const uint8_t *sample, size_t sample_bytes, uint8_t *dst, size_t capacity,
    size_t *written, struct x9_hevc_irap_picture *picture);
/* Stateful video AU gate. Firmware owns reference picture sets and POC/DPB,
 * not this parser. Allows base-layer TRAIL/TSA/STSA/RADL/RASL/BLA/IDR/CRA;
 * excludes reserved types and multilayer profiles. One complete picture per
 * sample, any retained PPS, all slices in one picture use the SAME PPS;
 * in-band parameter sets must match one unchanged retained hvcC set.
 * New parameter sets require a format-description/session change.
 * config Annex B is supplied once to session create, not before every frame. */
int x9_hevc_au_annexb(const struct x9_hevc_config *, const uint8_t *hvcc,
    const uint8_t *sample, size_t sample_bytes, uint8_t *dst, size_t capacity,
    size_t *written);
int x9_hevc_parameter_sets_annexb(const struct x9_hevc_config *, const uint8_t *hvcc,
    uint8_t *dst, size_t capacity, size_t *written);
/* Validate the whole one-picture AU gate, then follow that picture's actual
 * PPS->SPS. hvcC length must equal config_bytes. No representative-SPS guess,
 * synthesized CM extensions or SEI override; output is cleared on failure. */
int x9_hevc_sample_vui(const struct x9_hevc_config *, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes, struct x9_hevc_vui *);
/* Resolved base-layer prefix SEI in THIS complete AU, not
 * persistent video state or a transfer/tone-map policy. Direct TemporalId
 * must match the picture. Bytes are the original big-endian
 * payloads, after canonical emulation-prevention removal. No numeric scaling
 * or RGB-primary reorder. Equal repeated messages are legal; conflicting
 * values, malformed SEI lengths/trailing bits fail with cleared output.
 * Mid-picture prefix repeats require the same type before the first VCL.
 * D.2.1 defines suffix137/144/147/148 as reserved payloads, not these messages.
 * Scalable nesting resolves layers/operation layer sets using the actual
 * picture PPS->SPS->VPS; VCL-associated nested HDR requires NAL TemporalId0
 * and every MaxTemporalId6 (D.3.24), not the direct SEI's Tid equality rule.
 * Non-base scoped messages are not promoted. Unknown payload syntax and
 * other nesting families remain uninterpreted, not complete SEI validation.
 * SEI148 retains exact8 nominal-environment bytes; illuminance must be
 * nonzero and both xy values <=50000. SEI147 retains the exact preferred u8,
 * including unsupported code points; no label inference or tone mapper. */
#define X9_HEVC_HDR_DIRECT 1u
#define X9_HEVC_HDR_SCALABLE_LAYERS 2u
#define X9_HEVC_HDR_SCALABLE_OPERATIONS 4u
#define X9_HEVC_HDR_SOURCES 7u
struct x9_hevc_hdr {
    uint32_t mastering_present, light_present, ambient_present, preferred_present;
    /* Bitstream provenance only, ORed for agreeing repetitions. CM/container
     * readers leave these zero; presence/zero numeric payload are distinct. */
    uint32_t mastering_sources, light_sources, ambient_sources, preferred_sources;
    uint32_t preferred_transfer; /* exact u8 code point, not an inferred CFString code */
    uint8_t mastering[24], light[4], ambient[8];
};
/* Shared shape/provenance and ambient numeric constraints for bitstream and
 * exact CM/container snapshots. Does not invent absent fields or mastering
 * chromaticities, rescale values, or validate complete colour conformance. */
int x9_hevc_hdr_validate(const struct x9_hevc_hdr *);
int x9_hevc_sample_hdr(const struct x9_hevc_config *, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes, struct x9_hevc_hdr *);
/* Complete AU/catalog/SEI validation before publishing any facts. EOS/EOB are
 * observed NAL units, not inferred from DRAIN or no_output_of_prior_pics_flag.
 * The metadata gate is bounded, not a complete validator of unknown SEI
 * semantics or reserved payloads. Output is cleared on failure. */
struct x9_hevc_au_metadata {
    uint32_t nal_type, temporal_id, pps_id, no_output_of_prior_pics_flag;
    uint32_t eos_present, eob_present;
    struct x9_hevc_hdr hdr;
};
int x9_hevc_sample_metadata(const struct x9_hevc_config *, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes,
    struct x9_hevc_au_metadata *);
/* Prospective, resolved base-SEI-only state; zero initializes a fresh bitstream.
 * Commit ONLY after the native provider accepts SUBMIT. Container/CM values
 * are deliberately not stored here. No external HandleCraAsBlaFlag is used.
 * Mastering, ambient and preferred transfer persist from the CLVS first picture;
 * late CLLI is accepted and
 * retained for current/future inputs. clvs is a local nonzero identity derived
 * ONLY at the genuine boundaries below, not a provider token or POC. The VT
 * owner may use it to prepare same-CLVS pending metadata replacements; this
 * helper alone does not relabel pending or already-emitted pictures. */
struct x9_hevc_hdr_sequence {
    struct x9_hevc_hdr hdr;
    uint64_t clvs;
    uint32_t started, eos_present, eob_present;
};
int x9_hevc_hdr_sequence_next(const struct x9_hevc_hdr_sequence *,
    const struct x9_hevc_au_metadata *, struct x9_hevc_hdr_sequence *);
#endif
