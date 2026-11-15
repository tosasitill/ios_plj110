/* Native INIT/QUERY/START/output resources. No automatic platform registration. */
#ifndef X9_VDEC_CONTROL_H
#define X9_VDEC_CONTROL_H
#ifdef KERNEL
#include "x9_vdec_shared.h"
#include "x9_vcp_bus.h"
struct x9_vdec_control;
/* Actual native resources, not arbitrary IOVAs or a readiness bitmap.
 * Both input and output are already DEVICE-owned when passed to qualify_start.
 * Capacities include the native arena's owned zero padding; no pixel format,
 * ACP alias, hardware-prefetch requirement or DMA retirement is implied. */
struct x9_vdec_control_start_resources {
    uint64_t epoch, instance_token;
    uint64_t input_token; /* AP buffer identity, same namespace as output tokens */
    uint64_t input_slice_token; /* actual native arena handle, NOT AP identity */
    struct x9_vcp_instance_slot slot;
    struct x9_vdec_dma_extent input;
    uint32_t bytes_used;
    uint32_t dynamic_low_latency; /* exact AP byte, range0..255 */
    uint64_t timestamp; /* raw caller timestamp, no clock/POC arithmetic */
    struct x9_vcp_output_record output;
};
struct x9_vdec_control_hardware {
    void *cookie;
    int (*clock)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_clock_request *);
    int (*wait)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_wait_request *, enum x9_vcp_wait_outcome *);
    /* Mandatory to decode. Actually qualify current firmware/ABI, layout,
     * IRQ/clock and MAIN BS route through THIS owned normal codec domain,
     * with ACP disabled and NO separate non-ACP attachment. Other routes are
     * not supported by this native publisher; do not invent a second alias.
     * Actual initialized mode must be INPUT_DRIVEN_PUT_FRM (2), with output
     * supplied via a005. The immutable decoded observation is NOT ownership
     * authority; the platform must check its real routing/layout/clock state.
     * Must NOT reenter control/shared/bus APIs, write VSI, mark pixels complete
     * or return a successful empty stub. Native code owns precise NC stores
     * AFTER this qualification, within the physical bus call reservation.
     * Native input PoC handoff is already done. Not a pixel/completion ACK. */
    int (*qualify_start)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_init *, const struct x9_vcp_init_result *,
        const struct x9_vcp_start *, const struct x9_vdec_control_start_resources *,
        const struct x9_vcp_decode_snapshot *);
};
struct x9_vdec_control_status {
    uint64_t epoch, notifications;
    uint32_t init_attempted, bound, recheck_pending, last_errno;
    uint32_t start_attempted, start_replied;
    uint32_t output_attempted, output_prepared, output_notified;
    uint32_t input_attempted, input_prepared;
    uint32_t slot_initialized, vsi_attempted, vsi_staged;
    struct x9_vdec_control_start_resources resources; /* not a completed frame */
    struct x9_vcp_output_record output; /* DEVICE extents, NOT pixel layout */
    struct x9_vcp_init_result init;
    struct x9_vcp_start_result start;
    struct x9_vcp_output_notification last_notification;
    struct x9_vcp_ring_snapshot rings[3]; /* observations ONLY, never dequeue */
};
/* Bus must ALREADY be genuinely RUNNING with exactly this published global
 * allocation. Memory must own these same two arenas/epoch/parent. Retains both
 * memory/shared handles; caller serializes the bus lifetime through discard.
 * hardware.clock/wait are the real owner's mandatory operations, NOT success
 * stubs. Native code supplies actual SHM/alloc/VP and notification handlers.
 * This does not create a controller, load firmware, start a bus or set READY.
 * All API outputs are disjoint, resident host storage; thread context only. */
int x9_vdec_control_create(struct x9_vcp_bus *, struct x9_vdec_shared *,
    struct x9_vdec_memory *, const struct x9_vdec_control_hardware *hardware,
    struct x9_vdec_control **);
/* Sends physical a000, handles reverse operations, retains b000, resolves the
 * complete live slot using the captured HFRP remap table and uniquely claims
 * it in this global allocation, then sets ONLY the evidenced AP ipi_blocked
 * field to0 through its native NC map. Once only, including uncertain sends. */
int x9_vdec_control_init(struct x9_vdec_control *, uint32_t timeout_ms,
    struct x9_vcp_init_result *, struct x9_vcp_bus_call_log *);
/* Before START only: no interleaving QUERY with unresolved picture DMA.
 * QUERY replies still need separately owned capability-data resolution.
 * No fabricated profiles, decode completion, output or START callback. */
int x9_vdec_control_query(struct x9_vdec_control *, const struct x9_vcp_query *,
    uint32_t timeout_ms, struct x9_vcp_query_result *, struct x9_vcp_bus_call_log *);
/* One output registration BEFORE START. Allocate real codec arena slices,
 * assign a fresh native buffer token/index0, retain all handles, hand off to
 * PoC under the bus call reservation and send physical a005/service2. Size
 * arguments are capacities selected by the native layout owner, not caller
 * IOVAs or a declaration of NV12/P010. 1/2 planes; unused plane size must0.
 * Whole-domain retirement alone currently permits slice reuse/free. b005 is
 * an instance resource notification ONLY; no acceptance/pixel/retirement ACK.
 * Once attempted (even allocation/preparation/send failure), no second output
 * operation this instance. Does not register the missing highest-level owner.
 * Result/log and input capacities must be disjoint, resident host storage. */
int x9_vdec_control_register_output(struct x9_vdec_control *, uint32_t plane_count,
    const uint32_t plane_bytes[X9_VCP_OUTPUT_PLANES], uint32_t timeout_ms,
    struct x9_vcp_output_notification *, struct x9_vcp_bus_call_log *);
/* Copies a stable resident kernel Annex-B snapshot into a real PRIVATE codec
 * slice, retaining its native handle before copying. buffer_bytes is the
 * hardware layout owner's selected input capacity, >= bytes_used, NOT an
 * IOVA or guessed prefetch contract. Whole rounded capacity is owned/zeroed.
 * Requires this instance's published output and hardware.qualify_start.
 * Inside the physical bus's whole-call reservation, PoC handoff records the
 * actual input IOVA/capacity/token BEFORE hardware qualification and precise
 * native stores of AP bs token/main IOVA/non-ACP0/timestamp/latency. Does not
 * overwrite firmware-owned error/mode/FB fields, copy an entire VSI snapshot,
 * or clean its shared cachelines. Current publisher supports mode2 only.
 * a001 uses this instance's actual alias and actual slice capacity, never
 * caller-supplied token/alias/IOVA. flags remain wire data, not cache authority.
 * Once attempted, including allocation/copy/publication/send failures: no
 * retry/reuse. b001 does not retire input/output or prove pixels. No EOS path.
 * All buffers/results must be stable, resident, mutually disjoint host memory. */
int x9_vdec_control_decode_start(struct x9_vdec_control *, const uint8_t *annexb,
    uint32_t bytes_used, uint32_t buffer_bytes, uint32_t flags,
    uint64_t timestamp, uint32_t dynamic_low_latency, uint32_t timeout_ms,
    struct x9_vcp_start_result *, struct x9_vcp_bus_call_log *);
int x9_vdec_control_get_status(struct x9_vdec_control *, struct x9_vdec_control_status *);
/* After any INIT attempt, only physically STOPPED bus + both genuinely RETIRED
 * domains may drop this binding. DEINIT/c006/b001 are not retirement proofs.
 * Partial/uncertain paths retain handles; no bus reentry from callbacks. */
int x9_vdec_control_discard(struct x9_vdec_control **);
#endif
#endif
