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
#define X9_VDEC_STREAM (1u << 0)
#define X9_VDEC_REORDER (1u << 1)
#define X9_VDEC_DRAIN (1u << 2)
#define X9_VDEC_FRAME_TOKENS (1u << 3)
#define X9_VDEC_SESSION_REQUIRED 15u
/* Host delivery capability, NOT a firmware/session_ops prerequisite. */
#define X9_VDEC_OUTPUT_ACK (1u << 4)
#define X9_VDEC_SESSION_DELIVERY_REQUIRED (X9_VDEC_SESSION_REQUIRED | X9_VDEC_OUTPUT_ACK)
#define X9_VDEC_MAX_PENDING 32u
#define X9_VDEC_MAX_SESSIONS 16u
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
/* Session extension does not change the independent-picture v1 ioctls.
 * One session belongs to one clone fd. Duplicated fds share that session;
 * its last close destroys it even if the process dies. No DMA address crosses
 * this interface. Provider capabilities are not inferred from boot flags. */
struct x9_vdec_capabilities {
    uint32_t version, capabilities, max_pending, reserved;
    uint64_t generation;
};
struct x9_vdec_session {
    uint32_t version, config_bytes;
    uint64_t generation, session, config_address;
};
struct x9_vdec_submit {
    uint32_t version, input_bytes;
    uint64_t generation, session, input_address, token;
};
/* RECEIVE is nonblocking unless deadline_ms is nonzero (at most 5000ms).
 * EAGAIN means no display output yet, ENODATA means a completed drain.
 * Every submitted token returns exactly once, including non-output pictures
 * via X9_VDEC_OUTPUT_DROPPED. The token is the input identity, NEVER PTS/POC.
 * On pixel payload copyout failure the output remains retained for the next
 * receive. Legacy RECEIVE still requires the ioctl STRUCTURE to stay writable:
 * generic BSD ioctl's final structure copyout is outside that transaction.
 * New consumers must require OUTPUT_ACK and use PEEK/ACK below instead. */
#define X9_VDEC_OUTPUT_DROPPED 1u
struct x9_vdec_output {
    uint32_t version, deadline_ms;
    uint64_t generation, session, output_address, token;
    uint32_t output_capacity, output_bytes, width, height;
    uint32_t format, flags, result_flags, reserved;
    uint64_t completion;
};
/* PEEK returns the same validated output without releasing the CPU-owned
 * frame or removing its accepted token. Retry PEEK after either payload or
 * generic ioctl structure copyout failure. ACK is input-only: it consumes
 * exactly the held token/completion pair AFTER the caller has its result.
 * ACK is host delivery, never a firmware free/retirement/fence operation.
 * Legacy RECEIVE is rejected with EBUSY while a successful PEEK is retained.
 * Close/DESTROY still releases a held frame, including after generation loss. */
struct x9_vdec_output_ack {
    uint32_t version, reserved;
    uint64_t generation, session, token, completion;
};
#define X9_VDEC_GET_CAPABILITIES _IOR('V', 0x72, struct x9_vdec_capabilities)
#define X9_VDEC_SESSION_CREATE _IOWR('V', 0x73, struct x9_vdec_session)
#define X9_VDEC_SESSION_SUBMIT _IOW('V', 0x74, struct x9_vdec_submit)
#define X9_VDEC_SESSION_RECEIVE _IOWR('V', 0x75, struct x9_vdec_output)
#define X9_VDEC_SESSION_DRAIN _IOW('V', 0x76, struct x9_vdec_session)
#define X9_VDEC_SESSION_DESTROY _IOW('V', 0x77, struct x9_vdec_session)
#define X9_VDEC_SESSION_PEEK _IOWR('V', 0x78, struct x9_vdec_output)
#define X9_VDEC_SESSION_ACK _IOW('V', 0x79, struct x9_vdec_output_ack)
_Static_assert(sizeof(struct x9_vdec_capabilities) == 24, "vdec capabilities ABI");
_Static_assert(sizeof(struct x9_vdec_session) == 32, "vdec session ABI");
_Static_assert(sizeof(struct x9_vdec_submit) == 40, "vdec submit ABI");
_Static_assert(sizeof(struct x9_vdec_output) == 80, "vdec output ABI");
_Static_assert(sizeof(struct x9_vdec_output_ack) == 40, "vdec output ACK ABI");
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
/* A real codec instance retains its DPB and physical buffers across submit.
 * submit must consume/copy the host Annex B before returning; zero means the
 * token was accepted, errors mean NOT accepted. receive returns display-order
 * output and its original token; the CPU-owned frame/cookie and readable planes
 * must stay immutable until platform.release, including across submit/drain
 * and repeated host PEEKs. result_flags=DROPPED is a no-pixels retirement but
 * still requires a genuine completion/cookie. drain is bounded and must
 * make every accepted token available before receive returns ENODATA. After
 * ENODATA the same session may submit again; reference state/parameter sets
 * remain valid. destroy drains/quarantines before freeing DMA, even on fault.
 * These operations do not establish firmware/SMMU readiness by themselves. */
struct x9_vdec_session_ops {
    uint32_t capabilities, max_pending;
    int (*create)(const struct x9_hevc_config *, const uint8_t *parameter_sets,
                  uint32_t bytes, uint32_t deadline_ms, void **session);
    int (*submit)(void *session, const struct x9_hevc_config *, const uint8_t *annexb,
                  uint32_t bytes, uint64_t token, uint32_t deadline_ms);
    int (*receive)(void *session, uint32_t deadline_ms, struct x9_vdec_frame *,
                   uint64_t *token, uint32_t *result_flags);
    int (*drain)(void *session, uint32_t deadline_ms);
    void (*destroy)(void *session);
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
    const struct x9_vdec_session_ops *sessions;
};
int x9_vdec_register_platform(const struct x9_vdec_platform *);
/* Advisory exec-loader gate; not an owner reservation or readiness producer.
 * Userland must independently recheck GET_STATUS/GET_CAPABILITIES. */
int x9_vdec_autoload_available(void);
void sweet_x9_vdec_init(void);
#endif
#endif
