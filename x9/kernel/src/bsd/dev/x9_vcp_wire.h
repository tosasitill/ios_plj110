/* Bounded rv_v2 wire helpers, NOT a physical VCP or MM-SMMU provider.
 * C.57 firmware dispatcher/stock module evidence is recorded in NOTES.md.
 * A caller must first establish firmware/module pairing and DMA ownership.
 * No Linux pointers are dereferenced; AP addresses are opaque round-trip tokens.
 */
#ifndef X9_VCP_WIRE_H
#define X9_VCP_WIRE_H
#include <stdint.h>

#define X9_VCP_WIRE_BYTES 72u
#define X9_VCP_PAYLOAD_BYTES 64u
#define X9_VCP_VDEC_SERVICE 1u /* share_obj.id / firmware service dispatch */
#define X9_VCP_VDEC_OUTPUT_SERVICE 2u /* a005/b005 only; same transport OUT0/IN1 */
#define X9_VCP_VDEC_OUT_CHANNEL 0u /* physical IPI transport, NOT share_obj.id */
#define X9_VCP_VDEC_IN_CHANNEL 1u
#define X9_VCP_INIT 0xa000u
#define X9_VCP_INIT_DONE 0xb000u
#define X9_VCP_INIT_BYTES 24u
#define X9_VCP_INIT_REPLY_BYTES 32u
#define X9_VCP_CHECK_CODEC 0xc008u
#define X9_VCP_CHECK_CODEC_DONE 0xd008u
#define X9_VCP_CHECK_CODEC_BYTES 40u
#define X9_VCP_CODEC_HEVC 2u /* HEIF is 3, not interchangeable */
#define X9_VCP_FOURCC_HEVC 0x43564548u /* Linux HEVC, NOT Apple's hvc1 */
#define X9_VCP_START 0xa001u
#define X9_VCP_START_DONE 0xb001u
#define X9_VCP_START_BYTES 40u
#define X9_VCP_START_REPLY_BYTES 40u
#define X9_VCP_QUERY_CAP 0xa100u
#define X9_VCP_QUERY_CAP_DONE 0xb100u
#define X9_VCP_BACKUP 0xa101u /* NOT QUERY_CAP */
#define X9_VCP_QUERY_BYTES 40u
#define X9_VCP_QUERY_REPLY_BYTES 48u
#define X9_VCP_SHM_ALLOC 0xc005u
#define X9_VCP_SHM_ALLOC_DONE 0xd005u
#define X9_VCP_SHM_BYTES 64u
#define X9_VCP_MEM_SHM 8u
#define X9_VCP_COMMON_PREFIX_BYTES 0x50u
#define X9_VCP_SHM_MIN_BYTES 0x35950u
#define X9_VCP_COMMON_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_COMMON_SMMU_OFFSET 0x18u
#define X9_VCP_COMMON_VP_FLAGS_OFFSET 0x19u
#define X9_VCP_COMMON_VP_IOVA_OFFSET 0x20u
#define X9_VCP_INSTANCE_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_INSTANCE_SLOT_BYTES 0x19e8u
#define X9_VCP_INSTANCE_SLOT_COUNT 32u
#define X9_VCP_DECODE_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_DECODE_MAX_PLANES 8u /* accessed array capacity, NOT a format */
#define X9_VCP_GET_KERNEL_PARAM 0xc009u
#define X9_VCP_GET_KERNEL_PARAM_DONE 0xd009u
#define X9_VCP_KERNEL_PARAM_BYTES 40u
#define X9_VCP_KPARAM_VP_BUFFER 0u
#define X9_VCP_VP_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_VP_PATTERN_BYTES 192u
#define X9_VCP_VP_BUFFER_COUNT 3u
#define X9_VCP_VP_MEMORY_TYPE 2u /* stock HW_CACHE enum, not MAIR/cache policy */
#define X9_VCP_CODEC_ALLOC_MAX_BYTES 0x20000000u /* captured allocator bound */
#define X9_VCP_VP_Y 0u
#define X9_VCP_VP_C 1u
#define X9_VCP_VP_LENGTH 2u
#define X9_VCP_MEM_ALLOC 0xc005u /* per-instance variant, not service SHM */
#define X9_VCP_MEM_ALLOC_DONE 0xd005u
#define X9_VCP_MEM_FREE 0xc006u
#define X9_VCP_MEM_OP_BYTES 64u
#define X9_VCP_MEM_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_MEM_SW 0u
#define X9_VCP_MEM_HW 1u
#define X9_VCP_MEM_HW_CACHE 2u
#define X9_VCP_MEM_UBE_HW 3u
#define X9_VCP_WAITISR 0xc007u
#define X9_VCP_WAITISR_DONE 0xd007u
#define X9_VCP_WAITISR_BYTES 40u
#define X9_VCP_WAITISR_TIMEOUT_MS 500u
#define X9_VCP_HW_CORE 0u
#define X9_VCP_HW_LAT 1u
#define X9_VCP_LOCK_CORE 0xc001u
#define X9_VCP_UNLOCK_CORE 0xc002u
#define X9_VCP_LOCK_LAT 0xc003u
#define X9_VCP_UNLOCK_LAT 0xc004u
#define X9_VCP_CLOCK_BYTES 40u
#define X9_VCP_RING_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_RING_CAPACITY 64u
#define X9_VCP_RING_FREE_BS 0u
#define X9_VCP_RING_FREE_FB 1u
#define X9_VCP_RING_DISPLAY_FB 2u
#define X9_VCP_OUTPUT_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_FRAME_BUFFER 0xa005u
#define X9_VCP_FRAME_BUFFER_DONE 0xb005u /* resource notification, NOT pixels */
#define X9_VCP_FRAME_BUFFER_BYTES 64u
#define X9_VCP_OUTPUT_PLANES 2u /* supported encoder subset, NOT a format */
#define X9_VCP_OUTPUT_ASYNC 0xe000u
#define X9_VCP_OUTPUT_NOTIFY_BYTES 40u

struct x9_vcp_output_extent {
    uint64_t iova, bytes; /* full provider-owned device-writable extent, NOT PA */
};
struct x9_vcp_output_record {
    uint32_t context_id, firmware_vsi_address, buffer_index, plane_count;
    uint64_t instance_token, generation, buffer_token;
    struct x9_vcp_output_extent plane[X9_VCP_OUTPUT_PLANES], general;
};
struct x9_vcp_output_request {
    uint32_t firmware_vsi_address, buffer_index;
    uint64_t generation, buffer_token, y_iova, c_iova;
    uint64_t general_iova, general_bytes;
};
struct x9_vcp_init;
/* a005/64B, outer service2 (NOT INIT/START service1). Only a NONEMPTY output
 * registration for an existing clear instance; flush/EOS not inferred.
 * Retained INIT service1/AP token/context must match the durable output record;
 * request generation/firmware alias/token/index/Y/C/general must match it too.
 * The AP token is NOT encoded at payload8: that field is the firmware alias.
 * 1 or 2 nonempty, disjoint Y/C extents wholly below 36-bit DMA are supported;
 * index is a record identity, not a ring index (no guessed index64 wrap/bound).
 * Missing C requires record plane1=(0,0), request C=0. Missing general requires
 * record=(0,0), request IOVA=UINT64_MAX/size0; present size must fit positive
 * s32 without truncation. Every message independently zeroes id/reserved/tail;
 * never carry C/general size over from a previous registration as stock can.
 *
 * Inputs must be immutable host copies, not live VSI or Linux pointers. Caller
 * must serialize the current instance/generation and retain the complete record
 * BEFORE transport, prove alias slot/pairing, real mapped extent/SID/device-write
 * permission, power/clock/cache and DMA lifetime, and reject duplicate/pending
 * registrations. This byte encoder neither proves ownership nor records/sends
 * anything. No plane dimensions/layout are inferred: P01H/HYFBC is NOT P010.
 * b005/e000/display/free ring alone never permits delivery, unmap or release.
 * All inputs/output may overlap; error output zero. Encoded success is not ACK,
 * READY, hardware completion, a successful SMMU mapping or linear pixels.
 */
int x9_vcp_encode_output(uint32_t abi, const struct x9_vcp_init *,
                          const struct x9_vcp_output_request *,
                          const struct x9_vcp_output_record *,
                          uint8_t out[X9_VCP_WIRE_BYTES]);

struct x9_vcp_output_notification {
    uint32_t service_id, message_id, context_id, reserved;
    uint64_t instance_token, payload;
    int32_t firmware_status;
    uint32_t id, data;
};
/* Read ONLY one immutable 72B mailbox snapshot: b005/service2/40B or
 * e000/service1/40B. Match the retained INIT's fresh AP instance token; it is
 * NOT the output buffer token nor the firmware alias. Context is observed,
 * not compared to host context (captured sender starts it at zero). Return0
 * means a matched notification, even if b005 firmware_status is negative;
 * preserve signed status and opaque reserved/id/data/payload, not an errno or
 * proof of success. e000 status!=0 is outside the observed subset (ENOTSUP).
 * Captured b005 -14/zero-token error cannot identify an instance: reject it,
 * never attribute it to the sole pending buffer or clear that buffer's state.
 * No buffer index/token/completion/generation appears in this notification.
 * Caller must retain its own current instance/generation; b005 may only prompt
 * a resource recheck, e000 a coherent ring recheck. Neither grants delivery,
 * release, dequeue, DMA ownership, cache sync, an ACK or physical completion.
 * No state change, wakeup, live memory read or reply; input/output overlap is
 * allowed, and the output is zeroed on every parsing error.
 */
int x9_vcp_parse_output_notification(const struct x9_vcp_init *, const uint8_t *,
                                      uint32_t bytes,
                                      struct x9_vcp_output_notification *);

struct x9_vcp_clock_request {
    uint32_t service_id, message_id, request_context_id, reserved;
    uint64_t instance_token, payload;
    uint32_t id, data, hardware_id, acquire, clock_only;
};
/* c001..c004/40B/status0 matched to the retained AP token. CORE id0,
 * LAT id1. CORE payload!=0 means AO clock-only, NOT a hardware owner lease;
 * CORE payload0 and LAT payload0 mean full prepare/unprepare. Unobserved
 * LAT payload!=0 is rejected. data is preserved (stock uses its low byte
 * for DVFS), not a register address. No locks, clocks, IRQ or live writes.
 * Input/output may overlap; error output zero.
 */
int x9_vcp_parse_clock_request(const struct x9_vcp_init *, const uint8_t *,
                               uint32_t bytes, struct x9_vcp_clock_request *);
/* Encode d001..d004 ONLY after the requested operation has actually completed
 * for this instance/generation; operation_complete=0 returns EAGAIN/zero out.
 * Full acquire requires real DMA/cache/power/lock/IRQ ownership; release needs
 * drain, IRQ retirement and safe inverse teardown. AO operations must match
 * their own counted lease, not a full hardware lease. The captured host may
 * return status0 after prepare/unprepare silently skipped work; do NOT copy
 * that behavior. No negative clock ACK ABI is inferred here. Caller supplies
 * the completed result, this function proves no ownership and sends nothing.
 * Reply uses the host context; input/output may overlap, error output zero.
 */
int x9_vcp_encode_clock_reply(const struct x9_vcp_init *,
                              const struct x9_vcp_clock_request *,
                              uint32_t operation_complete,
                              uint8_t out[X9_VCP_WIRE_BYTES]);

struct x9_vcp_ring_snapshot {
    uint32_t kind, read_index, write_index, count, entry_present;
    uint64_t buffer_token, timestamp;
    uint16_t field, frame_type;
    uint32_t raw_flags;
};
/* ONE complete immutable/coherent host VSI slot, explicit captured ABI only.
 * Peek shared FREE_BS(+0, stride8, metadata+0x200), FREE_FB(+0x210) or
 * DISPLAY_FB(+0x820); FB stride24, metadata+0x600 relative to each ring.
 * Both indices must be <64; count must equal (write-read)&63. count64 returns
 * EOVERFLOW: stock's shared transfer tests read!=write, so full-ring semantics
 * cannot be inferred from the host count-based queue. No automatic repair,
 * wrap of invalid index64, consumption, live write or full-slot stack copy.
 * Empty returns metadata with entry_present=0, no stale entry bytes. FB entry
 * token/timestamp/u16 field/u16 frame_type/u32 flags are raw observations;
 * BS only supplies token. Unknown/zero token is NOT a CPU pointer or owned
 * buffer: provider must match its instance-local live record/generation and
 * actual completion/cache/layout before delivery or release. A display entry,
 * IRQ condition, e000 or b005 alone never proves pixels or linear P010.
 * Input/output may overlap; on error output zero.
 */
int x9_vcp_parse_ring_snapshot(uint32_t abi, uint32_t kind, const uint8_t *,
                               uint32_t bytes, struct x9_vcp_ring_snapshot *);

struct x9_vcp_query {
    uint32_t service_id, context_id, capability_id;
    uint64_t instance_token, data_token;
};
struct x9_vcp_query_result {
    int32_t firmware_status;
    uint32_t firmware_data_address;
};

/* Host-owned complete 72-byte mailbox snapshot, not a live MMIO pointer.
 * These functions send nothing and do not make capabilities/READY available.
 * Outer id is the VDEC service ID=1 in BOTH directions. Transport OUT0/IN1
 * and mailbox IRQ/pin are different identities; never put OUT0 in this field.
 * On error the output is zero, except firmware_status on a matched error ACK.
 */
int x9_vcp_encode_query(const struct x9_vcp_query *, uint8_t out[X9_VCP_WIRE_BYTES]);
int x9_vcp_parse_query_reply(const struct x9_vcp_query *, const uint8_t *,
                             uint32_t bytes, struct x9_vcp_query_result *);

struct x9_vcp_init {
    uint32_t service_id, context_id;
    uint64_t instance_token; /* fresh, unique AP token, NOT a Linux pointer */
};
struct x9_vcp_init_result {
    int32_t firmware_status;
    uint32_t reply_context_id, firmware_vsi_address;
};
/* Captured decoder .text 0x25258/0x25410: a000/24 B payload, no fourcc.
 * Firmware may issue reverse requests (including c008 codec identification)
 * before b000; this encoder does not implement those or send transport data.
 * Only AFTER paired common/SHM, queue/cache/power ownership and reverse-message
 * handling exist may a provider submit these bytes. Token must not be reused
 * across in-flight or retained instances; UINT64_MAX is the SHM sentinel.
 * Input/output may overlap. Error output is zero.
 */
int x9_vcp_encode_init(const struct x9_vcp_init *, uint8_t out[X9_VCP_WIRE_BYTES]);
/* b000/32 B, matched by service/message/fresh AP token, never dereference token.
 * Unlike b100, b000 context echo is NOT established by the observed firmware:
 * retain reply_context_id for observation, don't invent a ctx_id equality gate.
 * Error ACK retains status/context but does NOT return a VSI address.
 * Success gives only an opaque 32-bit firmware alias, NOT a mapped CPU pointer,
 * complete VSI layout, codec selection, decode readiness or hardware ownership.
 * Provider must resolve the FULL required VSI span inside its verified pool
 * (excluding common/auxiliary), not stock's unchecked low28 subtraction.
 * Input/query/output may overlap; host snapshot only, not live MMIO/SHM.
 */
int x9_vcp_parse_init_reply(const struct x9_vcp_init *, const uint8_t *,
                            uint32_t bytes, struct x9_vcp_init_result *);

struct x9_vcp_codec_check {
    uint32_t service_id, request_context_id;
    uint64_t instance_token;
    int32_t candidate_svp, codec_id; /* request status is SVP, NOT an errno */
    uint32_t reserved, data;
    uint64_t payload; /* opaque round-trip data, never a dereferenced pointer */
};
/* Parse c008/40 B from a host mailbox snapshot matched to a fresh INIT token.
 * Firmware can try other codecs/SVP values, including unsupported candidates.
 * Those are NOT framing errors: preserve signed fields for a rejection reply.
 * The request context is observational; stock replies with its own context.
 * Complete defined prefix and opaque data/payload are retained, not guessed.
 * No physical transport/state mutation. Error output is zero; overlap allowed.
 */
int x9_vcp_parse_codec_check(const struct x9_vcp_init *, const uint8_t *,
                             uint32_t bytes, struct x9_vcp_codec_check *);
/* ONLY an explicitly selected clear HEVC session (fourcc=HEVC) is supported.
 * Emit d008/40 B, own context and same token/codec/reserved/data/payload.
 * status=0 iff codec_id=2 AND candidate_svp=0; otherwise status=-1, return 0
 * because a well-framed negative reply is required for candidate enumeration.
 * Unsupported session fourcc returns ENOTSUP with zero output, not a success
 * or a guessed HEIF/SVP response. Success here ONLY means encoded bytes: not
 * codec capability, complete reverse handling, firmware init or hardware READY.
 * Caller must prove session pairing/ownership and serialize the reply. Input
 * and output may overlap; nothing dereferences tokens, payloads or addresses.
 */
int x9_vcp_encode_hevc_codec_reply(const struct x9_vcp_init *, uint32_t fourcc,
                                   const struct x9_vcp_codec_check *,
                                   uint8_t out[X9_VCP_WIRE_BYTES]);

struct x9_vcp_start {
    uint32_t service_id, context_id, firmware_vsi_address;
    uint32_t bytes_used, buffer_bytes, flags;
};
/* Captured decoder .text 0x257f4..0x25830 / 0x25870..0x25890:
 * a001/40 B uses a DIFFERENT prefix: offset8 = firmware VSI alias (u64),
 * NOT the AP token from a000/c008. Offset16 = bytes used, NOT status;
 * offset20 = buffer capacity, offset24 = flags, remaining three words zero.
 * Only nonempty input within buffer capacity is encoded; no reset/EOS path.
 * Provider must validate the whole alias slot, paired VSI/DMA/cache/queue,
 * reverse handlers and clocks/power before sending. No addresses/pointers
 * are dereferenced, no VSI fields are written, no transport or DMA is started.
 * flags are preserved wire data, not permission to skip cache maintenance.
 * Encoded success is NOT b001, hardware completion or a displayable frame.
 * Input/output may overlap. Error output is zero.
 */
int x9_vcp_encode_start(const struct x9_vcp_start *, uint8_t out[X9_VCP_WIRE_BYTES]);

struct x9_vcp_start_result {
    int32_t firmware_status;
    uint32_t reply_context_id;
    uint8_t opaque[20]; /* payload20..39; NOT dimensions or a completion token */
};
/* b001/40B matched by the retained AP instance token, NOT the firmware alias
 * encoded in a001. Captured host .text21870..21884 loads payload8 as the
 * instance, 21974..21998 validates it against the live context list,
 * 21a88..21aac handles payload16 errors and 21d44..21d4c/229bc wakes the
 * waiter on b001. Firmware 30b5fe..30b60c sends 40B. Context echo is not
 * established: preserve it and all trailing opaque bytes without interpreting
 * them. Matched nonzero signed firmware status returns EIO with result intact.
 * return0 means ONLY a firmware command reply, never DMA retirement, decoded
 * pixels, a ring dequeue, CPU ownership or readiness. Host snapshots only;
 * input/result may overlap and parsing errors clear result.
 */
int x9_vcp_parse_start_reply(const struct x9_vcp_init *, const uint8_t *,
    uint32_t bytes, struct x9_vcp_start_result *);

/* Captured c005/c006 mem object: payload24:type, 28:requested length,
 * 32:iova, 40:pa, 48:va. For INSTANCE allocations, pa == iova == DMA address,
 * NOT CPU PA; va is an opaque allocator object token, NOT a CPU mapping.
 * Service type8 has different semantics and is handled by the SHM helpers.
 */
struct x9_vcp_mem_object {
    uint32_t type, bytes;
    uint64_t iova, dma_address, allocation_token;
};
struct x9_vcp_mem_request {
    uint32_t service_id, message_id, request_context_id, reserved;
    uint64_t instance_token;
    struct x9_vcp_mem_object mem;
    uint32_t tail[2]; /* opaque; NOT initial-service prop/log addresses */
};
struct x9_vcp_mem_layout {
    uint32_t requested_bytes, allocation_bytes, extra_bytes;
};
struct x9_vcp_mem_allocation {
    uint64_t iova, allocation_token;
    uint32_t allocation_bytes;
    int32_t status; /* actual operation result, Linux wire values, not XNU errno */
};
struct x9_vcp_mem_record {
    uint64_t instance_token;
    struct x9_vcp_mem_object mem;
};
/* Only clear types0..3, explicit file ABI, nonzero length <= captured 512MiB
 * request bound. SW allocation = round_up(length,128)+128, but wire length
 * remains the requested length. Planning only: type is not a MAIR policy,
 * DMA device/translation/heap/cache/ownership cannot be inferred from it.
 */
int x9_vcp_mem_layout(uint32_t abi, uint32_t type, uint32_t requested_bytes,
                       struct x9_vcp_mem_layout *);
/* Full immutable/coherent host mailbox; matched fresh/retained AP INIT token.
 * Strict c005 status0/zero address request or c006 status0/nonzero object.
 * Secure/unknown/type8 rejected. Context, reserved and tail are retained;
 * request context is observational, stock replies with the host context.
 * No token/address dereference, allocation, free or live shared-memory access.
 * Error output zero, input/output overlap supported.
 */
int x9_vcp_parse_mem_request(const struct x9_vcp_init *, const uint8_t *,
                              uint32_t bytes, struct x9_vcp_mem_request *);
/* Encode ONLY c005->d005 from an actual provider allocation result. Before
 * status0, caller MUST own the full DMA/cache extent and have durably inserted
 * an instance-local allocation record; stock's kmalloc failure can otherwise
 * leave status0 with an untracked allocation. This helper proves neither.
 * Success requires sufficient allocation size, nonzero <36bit full IOVA span
 * and nonzero opaque allocator token. pa mirrors iova, never inferred CPU PA.
 * Negative status encodes a failure with all three address fields zero; caller
 * must separately roll back/drain any partial allocation. Positive status is
 * rejected. No free ACK encoder: exact match below is NOT completed release.
 * Error output zero; input/output overlap supported; sends nothing.
 */
int x9_vcp_encode_mem_alloc_reply(const struct x9_vcp_init *,
                                   const struct x9_vcp_mem_request *,
                                   const struct x9_vcp_mem_allocation *,
                                   uint8_t out[X9_VCP_WIRE_BYTES]);
/* Match ALL 32 mem bytes plus instance token against a provider-owned record,
 * not just va or iova. Does not remove the record or free/unmap/drain anything.
 * A stale/foreign/duplicate request must never release a different instance.
 * Caller still needs live record generation, serialization, DMA retirement
 * and completed unmap/cache teardown before any d006; no such ACK here.
 */
int x9_vcp_match_mem_free(const struct x9_vcp_mem_request *,
                          const struct x9_vcp_mem_record *);

struct x9_vcp_wait_request {
    uint32_t service_id, request_context_id, hardware_id, reserved;
    uint64_t instance_token, payload;
    uint32_t id, data;
};
enum x9_vcp_wait_outcome {
    X9_VCP_WAIT_PENDING = 0,
    X9_VCP_WAIT_IRQ_RECEIVED = 1,
    X9_VCP_WAIT_TIMED_OUT = 2,
    X9_VCP_WAIT_INTERRUPTED = 3
};
/* c007 status is hardware selection, NOT errno. Clear CORE0/LAT1 only;
 * preserve opaque id/data/payload. The request does not specify timeout:
 * captured host uses 500ms. Unknown/negative selectors rejected, no IRQ read.
 */
int x9_vcp_parse_wait_request(const struct x9_vcp_init *, const uint8_t *,
                               uint32_t bytes, struct x9_vcp_wait_request *);
/* d007/40B own context: status0 for ACTUAL instance/selected-hardware IRQ,
 * -1 for actual timeout, -2 for interrupted wait (NOT -ETIMEDOUT/-EINTR).
 * Pending/absent event returns EAGAIN and zero output, never an ACK. Caller
 * MUST serialize/consume a fresh owned completion for this wait and respect
 * power/IRQ/DMA generation; a bare register bit or cached event is insufficient.
 * IRQ ACK is not b001, output-ring completion or decoded/displayable pixels.
 * Host bytes only, no transport/IRQ acknowledge/condition clearing.
 */
int x9_vcp_encode_wait_reply(const struct x9_vcp_init *,
                              const struct x9_vcp_wait_request *,
                              enum x9_vcp_wait_outcome,
                              uint8_t out[X9_VCP_WIRE_BYTES]);

/* A file-identity-specific READ-ONLY subset of the slot accessed by decode.
 * All address/token fields are opaque observations, not CPU pointers or
 * mapping authorizations. The eight plane entries are layout capacity only;
 * unused entries can be stale and must not be used for decode/output.
 */
struct x9_vcp_decode_snapshot {
    uint32_t changed_info;
    uint64_t bs_iova, bs_non_acp_iova, fb_iova[X9_VCP_DECODE_MAX_PLANES];
    uint64_t bs_token, fb_token, timestamp;
    uint32_t plane_count, fb_index, error_map, error_code[2];
    uint32_t input_driven, output_async, low_power_mode, dynamic_low_latency;
    uint32_t bitdepth; /* captured c009 consumer reads VSI+0xf80; raw value */
    uint64_t general_iova;
    int32_t general_fd;
    uint32_t general_bytes, ipi_blocked;
};
/* Requires exactly one complete immutable/coherent 0x19e8-byte host slot.
 * Copies only evidenced spans into a small local snapshot before clearing out
 * (input/output may overlap; no full-slot kernel stack copy). ABI must match
 * the captured decoder; rejects plane count >8, preserves raw mode/error bits.
 * Empty/uninitialized/error snapshots are observable, NOT decode readiness.
 * No ring ownership, completion/fence, pixel layout, dimensions or full VSI
 * ABI is inferred. In particular b001/changed_info=0 is NOT proof of pixels.
 * Never pass live MMIO or live shared memory. On error output is zero.
 */
int x9_vcp_parse_decode_snapshot(uint32_t abi, const uint8_t *, uint32_t bytes,
                                struct x9_vcp_decode_snapshot *);

/* Initial service SHM request, not the per-instance memory allocator.
 * TCM 0x11782..0x11898: context=0, instance token=UINT64_MAX, type=8;
 * offsets 24:type, 28:len, 32:iova, 40:pa, 48:va, 56/60:prop/log.
 * d005 also confirmed by captured decoder; runtime pairing remains required.
 * Strictly rejects other memory types/variants rather than dereferencing the
 * sentinel instance token. The request/result must not alias one another.
 */
struct x9_vcp_shm_request {
    uint32_t service_id, context_id;
    uint64_t instance_token;
};
struct x9_vcp_shm_region {
    uint64_t iova;
    uint32_t bytes;
};
struct x9_vcp_shm_mapping {
    struct x9_vcp_shm_region vdec, prop, log;
    uint64_t cpu_token; /* opaque; never dereferenced or inferred from firmware */
};
/* Observed RV55 service partitioning INSIDE the VDEC block. These offsets
 * are neither physical addresses nor the separate packed prop/log regions.
 * Common's accessed fields are understood only for the explicit captured ABI
 * below. Its 80-byte size alone does NOT prove any other firmware's field ABI.
 * TCM 0x11872..0x1187e requires 0x33d50 + 0x800 + 0x1400 bytes before
 * service init; DRAM 0x30566c reserves the first 0x50 bytes for common data.
 */
struct x9_vcp_shm_span {
    uint32_t offset, bytes;
};
struct x9_vcp_shm_layout {
    struct x9_vcp_shm_span common, instance_pool, auxiliary[2], remainder;
};
struct x9_vcp_instance_slot {
    uint32_t index, offset, bytes; /* inside VDEC SHM, NOT PA/CPU pointer */
};
/* Resolve the observed b000 alias to ONE whole 0x19e8 pool slot. Requires
 * explicit captured ABI, provider-verified alias and full advertised HFRP
 * region. Reject common, auxiliary, interior slot addresses and window wrap.
 * 32-slot capacity is byte layout, not live allocation state or full VSI ABI.
 * Does not authorize a CPU pointer, field writes, mapping, cache or DMA work.
 * Error output is zero; region/output overlap is allowed.
 */
int x9_vcp_resolve_instance_slot(uint32_t abi, const struct x9_vcp_shm_region *,
                                 uint32_t firmware_alias, uint32_t vsi_address,
                                 struct x9_vcp_instance_slot *);
/* Size/offset planning only, no allocation, initialization or mapping.
 * On error out is zero. No source-only common VSI field is written.
 */
int x9_vcp_shm_work_layout(uint32_t block_bytes, struct x9_vcp_shm_layout *out);
int x9_vcp_parse_shm_request(const uint8_t *,
                            uint32_t bytes, struct x9_vcp_shm_request *);
/* Caller must own all three mapped/cache-synced regions and have initialized
 * the PAIRED common VSI (including enable_smmu) before sending this reply.
 * This only encodes bytes: it cannot initialize a guessed VSI or claim SHM
 * completed. Reply retains the request's outer service ID, NOT OUT channel0.
 * Stock's field named mem.pa holds the shared DMA IOVA, NOT physical memory.
 * A VDEC block smaller than X9_VCP_SHM_MIN_BYTES is rejected, as in the
 * observed firmware. All bytes of each advertised region must be mapped;
 * the minimum partition size is NOT sufficient mapping/cache ownership.
 */
int x9_vcp_encode_shm_reply(const struct x9_vcp_shm_request *,
                            const struct x9_vcp_shm_mapping *,
                            uint8_t out[X9_VCP_WIRE_BYTES]);

/* File-identity-matched MT6993 decoder, GNU build-id
 * a5f63a89f1658568bb7ffc2ae837e96df2361ea1: common accesses at offsets
 * 0/4/8/16 (fault), 24 (SMMU), 25/26 (VP publication flags) and 32..79
 * (2 x 3 DMA IOVAs). Firmware confirms the 80-byte prefix separately.
 * Padding 20..23 and 27..31 is not assigned a hardware meaning.
 * Selecting this constant is NOT evidence of firmware/module pairing.
 * The provider must establish that pairing before using these byte images.
 */
struct x9_vcp_common_snapshot {
    uint32_t fault_hw_id, fault_port, has_fault;
    uint64_t fault_iova;
    uint32_t enable_smmu, vp_published[2];
    uint64_t vp_iova[2][3]; /* opaque addresses, no ownership or mapping implied */
};
/* Creates a COLD, host-owned byte image: zeroed fault/VP state, SMMU=1.
 * Never use to reset a live/retained common block or clear a runtime fault.
 * There is deliberately no SMMU=0/bypass encoder. This neither maps memory
 * nor publishes VP buffers; six zero IOVAs with unpublished flags are valid.
 * Before copying to fresh provider-owned SHM: establish translations/cache
 * ownership for the entire advertised block, then sync before replying d005.
 * On error out is zero. No register, queue, live pointer or allocation access.
 */
int x9_vcp_common_cold_image(uint32_t abi,
                            uint8_t out[X9_VCP_COMMON_PREFIX_BYTES]);
/* Parses exactly 80 immutable/coherent host snapshot bytes, never live MMIO.
 * Boolean fields must be 0/1; input padding is ignored, not required to be zero.
 * Disabled SMMU, a reported fault and unpublished/nonzero IOVAs are observable
 * states, NOT parse errors or permission to submit work. Caller must decide
 * availability separately; no pointers are dereferenced or fault bits cleared.
 * Input/output may overlap; on error out is zero.
 */
int x9_vcp_parse_common_snapshot(uint32_t abi, const uint8_t *input,
                                uint32_t bytes, struct x9_vcp_common_snapshot *out);

struct x9_vcp_vp_request {
    uint32_t service_id, request_context_id;
    uint64_t instance_token;
    uint32_t reserved, data;
    uint64_t payload; /* opaque; c009 does NOT carry bitdepth here */
};
/* Only c009/40 B, id=0, status=0 matched to the retained fresh AP token.
 * Preserve context/reserved/data/payload; unlike c008 stock does NOT replace
 * c009's context. bitdepth is read from paired VSI+0xf80, not this message.
 * Unsupported kernel parameter/status variants fail without a reply.
 * Input/output may overlap; on error output is zero. No transport/state work.
 */
int x9_vcp_parse_vp_request(const struct x9_vcp_init *, const uint8_t *, uint32_t bytes,
                           struct x9_vcp_vp_request *);
/* Only encode after the provider owns paired/coherent SHM and all three
 * translated, initialized, device-synced VP buffers for THIS VSI bitdepth.
 * Requires a matching fresh common snapshot: SMMU=1, no fault, selected
 * group's publication flag=1 and three nonzero <36-bit opaque IOVAs.
 * These byte checks are NECESSARY, NOT proof of ownership/cache/mappings or
 * permission to publish a flag. Never make a stale/guessed common snapshot.
 * Stock may ACK with status0 even on allocation/cache failure; this helper
 * deliberately returns EAGAIN without encoding unless publication is seen.
 * No SHM/flag writes, allocation or cache operations. Preserves request's
 * context and opaque fields, emits d009/40 B. Not decoder READY or pixels.
 * Inputs/output may overlap. Error output is zero.
 */
int x9_vcp_encode_vp_reply(const struct x9_vcp_init *, const struct x9_vcp_vp_request *,
                          const struct x9_vcp_common_snapshot *, uint32_t bitdepth,
                          uint8_t out[X9_VCP_WIRE_BYTES]);

struct x9_vcp_vp_layout {
    uint32_t group_index, memory_type, buffer_bytes;
    uint32_t pattern_bytes[3], repetitions[3], written_bytes[3], untouched_bytes[3];
};
/* Captured common KO .text 0x5c48..0x5cec: three type2 buffers, length
 * 192*multiplier+16 and floor(length/pattern_len) full pattern copies.
 * Only bitdepth 8/10 and positive multipliers within the captured allocator's
 * 512MiB per-buffer limit (not a hardware/RAM capacity). live multi=1
 * means 208 B per buffer; 10-bit Y/C write192 B and leave16 B UNTOUCHED.
 * Plan only, not allocation/DMA/page rounding or a buffer/pixel format.
 */
int x9_vcp_vp_layout(uint32_t abi, uint32_t bitdepth, uint32_t multiplier,
                     struct x9_vcp_vp_layout *);
struct x9_vcp_vp_pattern {
    uint32_t bytes;
    uint8_t data[X9_VCP_VP_PATTERN_BYTES];
};
/* Generates ONE captured auxiliary black-pattern block in host-owned bytes,
 * not an image decoder or a linear P010 plane. bytes=4 or192; trailing data
 * in this output struct is zero, NOT a rule for the actual DMA buffer's tail.
 * Caller repeats exactly as planned, handles the untouched tail according to
 * proven allocation/firmware policy, and syncs before common publication.
 * Does not write any live buffers, cache, DMA, SHM or hardware state.
 */
int x9_vcp_vp_pattern(uint32_t abi, uint32_t bitdepth, uint32_t kind,
                      struct x9_vcp_vp_pattern *);

/* Packed 36-bit HFRP IOVA from the actual GPR instruction sequence, not PA.
 * Alignment is needed because the high nibble occupies the low address bits.
 * No mapping, permission or ownership is implied by successful packing.
 */
int x9_vcp_pack_iova(uint64_t iova, uint32_t *packed);
uint64_t x9_vcp_unpack_iova(uint32_t packed);

#define X9_VCP_BOOT_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_BOOT_GPR_BYTES 12u /* cfg+44/+48/+4c, one bank */
#define X9_VCP_READY_ABI_MT6993_CAPTURE1 1u
#define X9_VCP_READY_BYTES 4u /* one SRAM-size word, NOT share_obj */
#define X9_VCP_CONTROLLER_VCP 0u
#define X9_VCP_CONTROLLER_MMUP 1u
#define X9_VCP_READY_CHANNEL 5u
#define X9_MMUP_READY_CHANNEL 26u
struct x9_vcp_ready_event {
    uint32_t controller, channel, ready_index, sram_bytes;
};
/* Parse one immutable host copy of a READY-channel payload, not a decoder
 * message. Only captured dual-core routing is supported: VCP channel5/index0,
 * MMUP channel26/index1. SRAM-size must equal the caller's current verified
 * vcp-sram-size DT value (capture: 0x80000), NOT firmware payload length.
 * Unlike stock's queue-before-size-check, validates before returning an event.
 * Caller must prove receipt on the named controller/channel in the current
 * reset epoch AFTER receiver setup/reset release, reject duplicates/stale
 * events and serialize the pair; the word has NO epoch, nonce or signature.
 * A matching word proves neither physical receipt nor freshness/residency,
 * pairing, MM mapping/power/cache/SHM or QUERY_CAP. Success only describes ONE
 * event's bytes; never sets READY/profiles, queues work, sends ACK or permits
 * DMA/decoder submission. No MMIO/SMC/IPI callback. Overlap supported; error
 * zeroes output. This parser does not implement single-core fallback routing.
 */
int x9_vcp_parse_ready_event(uint32_t abi, uint32_t controller,
                            uint32_t channel, uint32_t core_count,
                            uint32_t expected_sram_bytes, const uint8_t *payload,
                            uint32_t payload_bytes, struct x9_vcp_ready_event *);

struct x9_vcp_boot_snapshot {
    uint64_t shared_iova, mmup_logger_iova, vcp_logger_iova;
    uint32_t shared_bytes, reserved;
};
/* Inspect TWO immutable host snapshots of the actual cfg GPR triplets,
 * MMUP first, VCP second, each exactly 12 LE bytes. Both must advertise the
 * complete expected HFRP shared allocation (IOVA, not CPU PA); logger+size
 * arithmetic follows captured reset_vcp, not a guessed per-core layout.
 * expected_logger describes the recorded MMUP logger base/stride; VCP's GPR
 * must equal base+stride and still lie INSIDE shared. This does NOT prove a
 * second logger extent or its size/ownership (not present in the triplet).
 * Captured RV55 ffcc..ffd8 and MMUP 15506..1553c feed this value to protection
 * region setters: VCP gets the logger END boundary; MMUP gets its START.
 * There is no second logger allocation to infer at the VCP GPR address.
 * Caller supplies current provider-owned allocation records and serialized
 * fresh snapshots from the SAME boot/reset epoch; matching bytes cannot prove
 * freshness, firmware residency, READY/IPI event receipt, SMMU mapping/cache,
 * clock/power/queue ownership or safe LK->XNU preservation. In particular an
 * Android READY flag and shared addresses must not be reused after reboot.
 * No MMIO/SMC/IPI/read callback or GPR/SHM/READY write; success is consistency
 * only, NEVER readiness or permission to release reset/submit a decoder job.
 * Input/output overlap supported; every error zeroes output.
 */
int x9_vcp_parse_boot_gprs(uint32_t abi,
                          const struct x9_vcp_shm_region *expected_shared,
                          const struct x9_vcp_shm_region *expected_logger,
                          const uint8_t *mmup, uint32_t mmup_bytes,
                          const uint8_t *vcp, uint32_t vcp_bytes,
                          struct x9_vcp_boot_snapshot *);

/* Resolve only within a provider-verified firmware alias window for ONE mapped
 * shared block. TCM 0x10fdc..0x1102a retains the low 27 IOVA bits after a
 * remap-table lookup of high address bits; the alias is NOT the low 32 bits.
 * Both windows must not wrap the low-27-bit address space and
 * must have identical low bits. Never infer the alias from an untrusted ACK.
 * Returns an offset, never a CPU pointer or an SMMU mapping authorization.
 */
int x9_vcp_shared_offset(uint64_t dma_iova, uint32_t firmware_alias,
                         uint32_t block_bytes, uint32_t address,
                         uint32_t requested_bytes, uint32_t *offset);
#endif
