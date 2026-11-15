/* Native INIT/QUERY/START/output resources. No automatic platform registration. */
#ifndef X9_VDEC_CONTROL_H
#define X9_VDEC_CONTROL_H
#ifdef KERNEL
#include "x9_vdec_shared.h"
#include "x9_vcp_bus.h"
struct x9_vdec_control;
/* Thread-context opaque host owner. Callers must serialize handle lifetime
 * against discard, including status/manual service/quiesce. The private lock
 * serializes operations, not a reference acquired before entering an API.
 * Native discard joins its own worker; that does not join external callers. */
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
    /* Latest OFFERED output, not proof of this input's selected/display FB.
     * Firmware may use an older registered buffer for its DPB/output. */
    struct x9_vcp_output_record output;
    uint32_t submission_index, registered_outputs; /* native journal identities */
};
struct x9_vdec_control_hardware {
    /* All hardware callbacks run within a bus reservation and control state
     * lock(s). They must NOT reenter ANY control/shared/memory/bus API or wait
     * for a control/pump while holding owner/DMA/IRQ locks. Likewise bus-owner
     * guard and lower DMA callbacks must not acquire a control lock. External
     * callers must not hold such lower locks across a control operation.
     * Foreign routing uses bounded try-lock entry; it never recursively locks
     * the current caller or executes parallel hardware callbacks. */
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
    uint64_t service_batches, service_packets;
    uint64_t foreign_packets;
    struct x9_vcp_bus_route_journal foreign_call; /* final foreign packet/reply/error */
    uint64_t worker_wakeups, worker_contentions;
    uint32_t worker_enabled, worker_joined, worker_last_errno, worker_bus_phase;
    uint32_t closing; /* host admission only, NOT DMA/firmware stopped */
    struct x9_vcp_bus_call_log service_call; /* last reserved/failed batch, incl packet */
    uint32_t input_records, output_records; /* includes partial/error attempts */
    uint32_t ring_valid_bitmap; /* independently parsed rings in latest recheck */
    struct x9_vdec_control_start_resources resources; /* latest attempt, NOT a completed frame */
    struct x9_vcp_output_record output; /* latest offered extents, NOT pixel layout */
    struct x9_vcp_init_result init;
    struct x9_vcp_start_result start;
    struct x9_vcp_output_notification last_notification;
    struct x9_vcp_ring_snapshot rings[3]; /* last raw observations, check bitmap; never dequeue */
};
/* Bounded durable native records, NOT hardware/DPB/ring capacity. All older
 * slices remain owned even after the latest status fields change. This stage
 * never reuses published backing: only whole-domain retirement frees it. */
#define X9_VDEC_CONTROL_MAX_RECORDS 64u
struct x9_vdec_control_input_journal {
    struct x9_vdec_control_start_resources resources;
    struct x9_vcp_start_result result;
    struct x9_vcp_bus_call_log call;
    uint32_t input_prepared, vsi_attempted, vsi_staged, control_replied, last_errno;
    uint32_t free_bs_observed; /* token-matched head ONLY, no read/count write */
    struct x9_vcp_ring_snapshot free_bs;
};
struct x9_vdec_control_output_journal {
    struct x9_vcp_output_record resources;
    struct x9_vcp_output_notification notification;
    struct x9_vcp_bus_call_log call;
    uint32_t prepared, notified, last_errno;
    uint32_t free_fb_observed, display_fb_observed; /* no read/count write or pixels */
    struct x9_vcp_ring_snapshot free_fb, display_fb;
};
/* Bus must ALREADY be genuinely RUNNING with exactly this published global
 * allocation. Memory must own these same two arenas/epoch/parent. Retains both
 * memory/shared handles and a native bus lifetime hold through discard, then
 * registers its fully initialized frozen identity/ops for bus-wide routing.
 * Registration failure rolls back refs, worker, lock and the bus hold.
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
 * field to0 through its native NC map. Once only, including uncertain sends.
 * EBUSY before reservation exposes nothing and leaves INIT fresh. ONLY full
 * success arms the control-owned, non-overlapping kernel thread-call
 * pump. It services real reverse/async packets even after b001 and rechecks
 * pending observations on empty RX. This is not a DMA/ring completion grant. */
int x9_vdec_control_init(struct x9_vdec_control *, uint32_t timeout_ms,
    struct x9_vcp_init_result *, struct x9_vcp_bus_call_log *);
/* Before START only: no interleaving QUERY with unresolved picture DMA.
 * QUERY replies still need separately owned capability-data resolution.
 * No fabricated profiles, decode completion, output or START callback. */
int x9_vdec_control_query(struct x9_vdec_control *, const struct x9_vcp_query *,
    uint32_t timeout_ms, struct x9_vcp_query_result *, struct x9_vcp_bus_call_log *);
/* Register ONE fresh output per call. Allocate real codec arena slices,
 * assign a fresh native buffer token/record index, retain all handles, hand off to
 * PoC under the bus call reservation and send physical a005/service2. Size
 * arguments are capacities selected by the native layout owner, not caller
 * IOVAs or a declaration of NV12/P010. 1/2 planes; unused plane size must0.
 * Whole-domain retirement alone currently permits slice reuse/free. b005 is
 * an instance resource notification ONLY; no acceptance/pixel/retirement ACK.
 * Successful control calls permit subsequent fresh registrations, before or
 * between STARTs. Earlier published buffers are never overwritten/reused.
 * All registrations currently require identical plane counts/capacities;
 * format changes need the separate native negotiation path. Failed/uncertain
 * calls poison further admission and preserve their partial journal, except
 * EBUSY before reservation: no callbacks/allocations/send happened; native
 * host-only attempt state is restored and unique burned tokens are not reused. Does
 * not register the missing highest-level owner or identify a b005's buffer.
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
 * Each call retains a fresh input record. Only a successfully finished prior
 * control transaction permits another call; old input and every registered
 * output remain DEVICE-owned, not implicitly retired by b001. No retry of a
 * failed/uncertain submission, no allocation reuse, no EOS path. EBUSY before
 * reservation restores the host-only attempt (no callback/exposure occurred).
 * The bus
 * rejects queued old ACKs before new preparation/send. The protocol has no
 * picture sequence: delayed duplicate ACKs after that RX baseline cannot be
 * disambiguated, so the actual FW must obey one reply per serialized command.
 * All buffers/results must be stable, resident, mutually disjoint host memory. */
int x9_vdec_control_decode_start(struct x9_vdec_control *, const uint8_t *annexb,
    uint32_t bytes_used, uint32_t buffer_bytes, uint32_t flags,
    uint64_t timestamp, uint32_t dynamic_low_latency, uint32_t timeout_ms,
    struct x9_vcp_start_result *, struct x9_vcp_bus_call_log *);
/* Thread-context service for this bound live instance, including after b001.
 * Uses native SHM/alloc/VP/notification handlers and the actual hardware
 * clock/wait callbacks, under control_lock and the bus whole-call reservation.
 * One batch drains up to64 packets; a successful INIT also automatically
 * schedules subsequent batches through the same native implementation.
 * Empty RX succeeds immediately. A retained transient ring observation is
 * rechecked even with no new event, not discarded/synthesized as completion.
 * Timeout/protocol/callback failures retain journals/resources and poison this
 * instance. EBUSY before bus reservation does NOT poison a pending decode.
 * No new START, shared ring consumption, per-buffer retirement or READY. */
int x9_vdec_control_service(struct x9_vdec_control *, uint32_t timeout_ms,
    struct x9_vcp_bus_call_log *);
/* Irreversibly close THIS control's admission and cancel/join its CPU service
 * pump, including a self-rearmed pending invocation. Thread context, without
 * owner/shared/bus locks needed by a callback; never call from a hardware/bus
 * callback or the pump itself. Repeated calls on a retained object succeed.
 * Call before bus stop / DMA domain retirement. No new control transaction,
 * firmware halt, IRQ/AXI drain, ring consumption, cache ownership or free.
 * Foreign reverse/async handlers may still run through the retained bus route
 * after this CPU-pump join: firmware is not stopped by host closing. Owner
 * must stop/drain the bus before retiring domains; route removal separately
 * excludes all reserved callbacks. discard also performs this join, but still requires physical STOPPED and
 * both RETIRED domains after an INIT attempt. Failed discard remains closed. */
int x9_vdec_control_quiesce(struct x9_vdec_control *);
int x9_vdec_control_get_status(struct x9_vdec_control *, struct x9_vdec_control_status *);
/* Exact bus registry evidence for this retained control, including failed
 * enter, current-caller ACKs and foreign packet/reply/error records. Read-only
 * host snapshot, not proof of DMA retirement or hardware completion. */
int x9_vdec_control_get_route(struct x9_vdec_control *, struct x9_vcp_bus_instance_status *);
/* Resident disjoint host snapshots of prior records, including partial/error
 * attempts. Indices 0..record_count-1 are never wrapped/rebased. No live DMA
 * CPU access, token retirement, pointer lookup or hardware completion grant. */
int x9_vdec_control_get_input(struct x9_vdec_control *, uint32_t index,
    struct x9_vdec_control_input_journal *);
int x9_vdec_control_get_output(struct x9_vdec_control *, uint32_t index,
    struct x9_vdec_control_output_journal *);
/* After any INIT attempt, only physically STOPPED bus + both genuinely RETIRED
 * domains may drop this binding. DEINIT/c006/b001 are not retirement proofs.
 * Partial/uncertain paths retain handles; no bus reentry from callbacks. */
int x9_vdec_control_discard(struct x9_vdec_control **);
#endif
#endif
