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
#endif
