/* Bounded HEVC configuration/independent-picture parser, shared with XNU.
 * This is a syntax gate, NOT a decoder. Only base-layer Main/Main10 4:2:0
 * and IDR pictures are accepted. Main-compatible 8-bit Main Still Picture
 * uses the Main decode path; monochrome Rext alpha stays on Apple SW.
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
struct x9_hevc_config {
    uint32_t coded_width, coded_height, width, height, crop_x, crop_y;
    uint32_t profile, bit_depth, level, length_size, vps_id, sps_id, pps_id;
    /* profile is the unmodified SPS/hvcC value. decoder_profile is the
     * required Main/Main10 capability, NOT a newly advertised HW profile.
     * Compatibility flags retain their big-endian bit numbering: profile 1
     * is bit30, not bit1. This struct is private, not the vdec ioctl ABI. */
    uint32_t decoder_profile, profile_compatibility;
    uint32_t ps_offset[3], ps_bytes[3], annexb_bytes;
};
/* Returns 0 or a BSD errno. Rejects unsupported formats rather than inventing
 * capabilities. The VPS/SPS/PPS spans refer to the unchanged hvcC snapshot. */
int x9_hevc_config_parse(const uint8_t *, size_t, struct x9_hevc_config *);
/* NULL dst performs the same validation and reports required size. */
int x9_hevc_idr_annexb(const struct x9_hevc_config *, const uint8_t *hvcc,
    const uint8_t *sample, size_t sample_bytes, uint8_t *dst, size_t capacity,
    size_t *written);
#endif
