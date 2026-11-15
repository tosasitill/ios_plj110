/* X9 independent HEVC-picture ABI. No AppleVXD protocol emulation. */
#ifndef X9_VDEC_H
#define X9_VDEC_H
#include <stdint.h>
#include <sys/ioccom.h>
#define X9_VDEC_VERSION 1u
#define X9_VDEC_DEVICE "/dev/x9-vdec"
#define X9_VDEC_MAX_OUTPUT (48u * 1024u * 1024u)
#define X9_VDEC_MAIN (1u << 1)
#define X9_VDEC_MAIN10 (1u << 2)
#define X9_VDEC_NV12 1u
#define X9_VDEC_P010 2u /* LE, most-significant 10 bits, NEVER P01H/HYFBC */
#define X9_VDEC_FIRMWARE (1u << 0)
#define X9_VDEC_IPI (1u << 1)
#define X9_VDEC_SMMU (1u << 2)
#define X9_VDEC_LINEAR (1u << 3)
#define X9_VDEC_HW_COMPLETION (1u << 4)
#define X9_VDEC_REQUIRED 31u
#define X9_VDEC_FULL_RANGE 1u
struct x9_vdec_status {
    uint32_t version, mode, ready, prerequisites;
    uint32_t profiles, max_width, max_height, last_errno;
    uint64_t generation, requests, completed, rejected;
};
/* Input: version, generation (from status), addresses and lengths/capacity.
 * All other fields MUST be zero. Output is tightly packed visible Y then UV.
 * Each request is self-contained: hvcC + one length-prefixed IDR picture.
 * ioctl success alone is insufficient: require a nonzero hardware completion
 * token and matching result layout. No userspace DMA address is accepted. */
struct x9_vdec_picture {
    uint32_t version, reserved;
    uint64_t generation, config_address, input_address, output_address;
    uint32_t config_bytes, input_bytes, output_capacity, output_bytes;
    uint32_t width, height, format, flags;
    uint64_t completion;
};
#define X9_VDEC_GET_STATUS _IOR('V', 0x70, struct x9_vdec_status)
#define X9_VDEC_DECODE_IDR _IOWR('V', 0x71, struct x9_vdec_picture)
_Static_assert(sizeof(struct x9_vdec_status) == 64, "vdec status ABI");
_Static_assert(sizeof(struct x9_vdec_picture) == 80, "vdec picture ABI");
#ifdef KERNEL
#include "../../../../hevc/hevc_bitstream.h"
struct x9_vdec_plane {
    const uint8_t *data;
    uint64_t bytes;
    uint32_t stride, reserved;
};
struct x9_vdec_frame {
    struct x9_vdec_plane plane[2];
    uint32_t coded_width, coded_height, crop_x, crop_y, width, height;
    uint32_t format, flags;
    uint64_t completion;
    void *cookie;
};
/* Resident platform provider, one serialized, bounded request at a time.
 * ready flags require actual VCP READY/QUERY_CAP, exact firmware ABI,
 * codec-domain mappings and a real completed hardware decode. No software
 * provider may register. decode() returns only after DMA has completed and
 * cache sync has made CPU-readable linear planes available. Input is host
 * memory, not DMA: copy it into provider-owned mapped storage before issuing
 * any job, and never retain the host pointer after returning. On any error it
 * must drain or quarantine all in-flight DMA itself; it must NOT return an
 * in-flight frame. release() may free only CPU-owned completed frames.
 * TIMEOUT must latch the provider unavailable until a safe platform recovery.
 * This front end neither boots VCP nor bypasses the MM SMMU. */
struct x9_vdec_platform {
    uint32_t version, prerequisites, profiles, max_width, max_height;
    int (*decode)(const struct x9_hevc_config *, const uint8_t *annexb,
                  uint32_t bytes, uint32_t deadline_ms, struct x9_vdec_frame *);
    void (*release)(struct x9_vdec_frame *);
};
int x9_vdec_register_platform(const struct x9_vdec_platform *);
void sweet_x9_vdec_init(void);
#endif
#endif
