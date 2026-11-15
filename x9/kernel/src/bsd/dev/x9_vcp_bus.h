/* Native rv_v2 mailbox transport. No automatic decoder registration/READY. */
#ifndef X9_VCP_BUS_H
#define X9_VCP_BUS_H
#ifdef KERNEL
#include "x9_vcp_wire.h"
struct x9_vcp_bus;
enum x9_vcp_bus_phase {
    X9_VCP_BUS_RESERVED, X9_VCP_BUS_COLD, X9_VCP_BUS_STARTING, X9_VCP_BUS_WAIT_READY,
    X9_VCP_BUS_RUNNING, X9_VCP_BUS_QUARANTINED, X9_VCP_BUS_STOPPING,
    X9_VCP_BUS_STOPPED
};
enum x9_vcp_bus_core { X9_VCP_BUS_CORE_VCP = 0, X9_VCP_BUS_CORE_MMUP = 1 };
/* Real bootstrap owner, not a configurable prerequisites flag. Every callback
 * is mandatory, bounded, errno-returning and must NOT reenter this bus.
 * guard runs under bus_lock; it must never wait for a bus-using worker.
 * guard proves current exclusive VCP/MMUP clock/reset/IRQ+mailbox ownership,
 * firmware signature/residency pairing and the installed SHM mappings/cache
 * for this cold session. MM power and FDT topology are checked separately by
 * native code and cannot substitute for these prerequisites.
 * shared/logger records are CURRENT owned global bootstrap allocations, NOT
 * per-service type8 SHM, CPU PA or inferred addresses. There is ONE complete
 * logger extent: MMUP's GPR receives its start, VCP's receives its end. The
 * captured firmware uses these values as protection-region boundaries, not
 * two independently sized logger allocations. Never claim/alias a second
 * logger in the next mblock. The entire shared allocation remains owned,
 * translated and cache-synced, including all later reserved blocks. Native
 * code checks bounds, publishes cfg+44/48/4c and performs the real reset SMC:
 * MMUP1 cmd6/READY then VCP0 cmd3/READY, once per core. Owner never clears or
 * releases these slots/cores in parallel. No cached Android READY or inferred
 * reset success. halt_and_drain must stop both physical producers and all
 * upper-layer DMA/worker users before permitting transport release. Stop
 * invokes it outside bus_lock with STOPPING phase and retained lifetime/pins,
 * so workers can return from rejected polls instead of deadlocking the drain.
 * The callback MUST retain clock/reset/MMIO ownership and keep both producers
 * halted after returning, through native stop verification and discard.
 * guard must remain usable in STOPPING and STOPPED for independent physical
 * checks at stop and immediately before discard/unpin. STOPPED permits retired
 * SHM, but still requires clock/reset/MMIO ownership and halted producers.
 * resources_retired is a read-only, non-waiting check under bus_lock: return0
 * ONLY after all epoch-dependent DMA, mappings and workers have been retired.
 * It is required even if bind/start failed before this bus exposed anything:
 * the owner may already have installed mappings using the reserved epoch.
 * Native code requires two cfg+70 samples for BOTH cores: gated + all FDT-
 * present harts halted, AXI not busy. Callback success alone cannot release
 * the bus; these samples still do NOT prove upper-layer DMA/worker drain.
 * There is no default provider or success stub; callbacks are not yet bound. */
struct x9_vcp_bus_owner {
    void *cookie;
    uint64_t power_token;
    int (*guard)(void *, uint64_t epoch, uint32_t phase);
    int (*halt_and_drain)(void *, uint64_t epoch);
    int (*resources_retired)(void *, uint64_t epoch);
};
struct x9_vcp_bus_status {
    uint64_t epoch, sent, tx_consumed, received;
    uint64_t reset_smc_a0[2]; /* meaningful only when reset_smc_bitmap bit set */
    uint32_t reset_smc_bitmap;
    /* released_bitmap records irreversible release ATTEMPTS, before writes;
     * neither it nor SMC a0=0 implies physical READY or codec completion. */
    uint32_t phase, released_bitmap, ready_bitmap, queued, last_errno;
    /* Last native stop samples, indexed [sample][CORE_*]. No cached Android
     * registers or synthetic callback-derived values. Bitmap3 requires both
     * physical samples and the retained owner guard; not a recovery grant. */
    uint32_t halt_status[2][2], halt_checked_bitmap, halt_samples;
    uint32_t service_shm_replied; /* physical d005 consumption, NOT codec READY */
};
/* Phase 1: reads CURRENT ADT topology and pins the native MM parent. Reserves
 * a singleton and boot-unique epoch, available through get_status BEFORE the
 * owner creates/installs DMA. Freezes callbacks/cookie, but invokes no owner
 * callback and creates no IO mappings on success. Refuses pre-KMEM creation.
 * On failure a non-NULL out is a retained cleanup handle, never success.
 * Caller must serialize resource preparation against discard/stop. */
int x9_vcp_bus_create(const struct x9_vcp_bus_owner *, struct x9_vcp_bus **);
/* Phase 2: owner has now installed resources using exactly the reserved epoch.
 * Validate complete CURRENT global shared/logger extents, call the real guard,
 * then create reclaimable IO mappings. No register/reset/slot write. On success
 * transitions RESERVED -> COLD; on bind failure retain owner/pin for cleanup,
 * not a retry with different resources. logger is the actual mblock[2], not
 * a two-element per-core array or an allocation beginning at its end.
 * This breaks the old create -> guard(epoch) -> DMA-before-create cycle. */
int x9_vcp_bus_bind(struct x9_vcp_bus *, uint64_t epoch,
                     const struct x9_vcp_shm_region *shared,
                     const struct x9_vcp_shm_region *logger);
/* Real receive baseline -> MMUP1 release/READY -> VCP0 release/READY. Physical
 * pin7 words on VCP mbox2/channel5 and MMUP mbox1/channel26. Single <=3s
 * deadline; old pending bits are rejected, not cleared/relabelled as fresh.
 * Matching SRAM size/bitmap is transport readiness, NOT codec capabilities. */
int x9_vcp_bus_start(struct x9_vcp_bus *, uint32_t timeout_ms);
/* OUT0 pin0/offset0, 72 bytes. Reads actual peer-consumption bit; a clear bit
 * means transport consumption only, NEVER firmware ACK/hardware completion.
 * Also services RX while waiting; no blind retransmit after uncertain write. */
int x9_vcp_bus_send(struct x9_vcp_bus *, const uint8_t[X9_VCP_WIRE_BYTES], uint32_t timeout_ms);
/* Thread-context physical IN1 pin9/offset72. Stable full dual snapshots while
 * the receive bit stays latched; queues owned bytes BEFORE clearing ONLY bit9.
 * Unknown pins are preserved. No guessed shared pointer, ACK or auto-dispatch.
 * poll returns nonfatal ENOSPC on software-ring backpressure, retaining the
 * physical receive bit. receive returns EAGAIN if empty, copies on success. */
int x9_vcp_bus_poll(struct x9_vcp_bus *);
int x9_vcp_bus_receive(struct x9_vcp_bus *, uint8_t[X9_VCP_WIRE_BYTES]);
int x9_vcp_bus_get_status(struct x9_vcp_bus *, struct x9_vcp_bus_status *);
/* Read-only native binding check, outside transactions/callbacks. Requires
 * RUNNING, the exact published global extent/epoch and the retained guard.
 * Matching extents confer no controller or firmware authority. */
int x9_vcp_bus_check_shared(struct x9_vcp_bus *, uint64_t epoch,
    const struct x9_vcp_shm_region *);
/* Stop is once-only, even on failure; uncertain dependencies remain pinned.
 * After real owner halt/drain, independently polls physical cfg+70 for at
 * most 500ms, using each core's CURRENT FDT hart count. Does not write SHUT,
 * assert reset, infer drain from a reboot signature, or retry recovery.
 * Discard requires resources_retired even for never-exposed RESERVED/COLD or
 * bind failure. An active transaction retains the bus through callbacks.
 * Exposed buses additionally require authoritatively STOPPED/fresh idle. */
int x9_vcp_bus_stop(struct x9_vcp_bus *);
int x9_vcp_bus_discard(struct x9_vcp_bus **);

/* Bounded synchronous INIT/QUERY_CAP/START command transactions. Native code owns
 * OUT0/IN1 for the ENTIRE call, including reverse requests; raw send/poll/recv
 * and competing calls return EBUSY. callbacks run outside bus_lock and must
 * not reenter the bus or wait past the absolute uptime-ns deadline. stop can
 * cancel a call, and discard cannot free it while a callback is executing.
 * The provider must serialize its live instance/epoch records and register
 * fresh, unique tokens BEFORE calling; no speculative/resubmitted INIT.
 * Each callback returns errno; success means the ACTUAL requested operation
 * and durable ownership bookkeeping completed, not a promise to do it later.
 * Missing handlers/unknown messages fail closed, retain the packet journal
 * and quarantine; no generic successful ACK, retry or resource release.
 * c008 is handled natively for clear HEVC; c006 free is not yet supported.
 * The first AP request triggers service SHM from the firmware worker. Dispatch
 * its sentinel/type8 variant while awaiting b000/b100; do NOT wait for an
 * unsolicited SHM request before sending INIT/QUERY. Once replied, a duplicate
 * type8 request is rejected, not treated as a restart/instance allocation.
 * No automatic READY/profiles.
 */
struct x9_vcp_bus_reverse_ops {
    void *cookie;
    /* Required for a005. Under the whole-call reservation, actually publish
     * native output slices and retain their full records BEFORE exporting
     * addresses. Fill disjoint host copies only after real PoC handoff and
     * current instance/epoch checks. No synthetic mapping/retirement flags,
     * no bus reentry, and no reuse after a partial/uncertain publication. */
    int (*prepare_output)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_init *, const struct x9_vcp_init_result *,
        struct x9_vcp_output_request *, struct x9_vcp_output_record *);
    /* Required for START, invoked once BEFORE any a001 send, outside the bus
     * lock but with the entire call/lifetime reserved. Must actually validate
     * the retained b000 alias/full VSI slot, exact input and registered output
     * extents, current SID/epoch/clock/IRQ ownership and no unresolved prior
     * CONTROL call; publish the evidenced VSI fields and flush CPU-owned input/VSI
     * to PoC, retaining all journals before success. Not a caller-supplied
     * prerequisites flag. Even partial failure keeps resources/quarantine.
     * a001/b001 has no picture sequence: queued old ACKs are rejected before
     * preparation, but a delayed duplicate arriving after the RX baseline
     * cannot be disambiguated. Actual FW must obey one reply per serialized
     * command. Owner must reject failed/uncertain prior submissions and retain
     * their buffers, not use b001 to reclaim still-device-owned storage.
     */
    int (*prepare_start)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_init *, const struct x9_vcp_init_result *,
        const struct x9_vcp_start *);
    /* Matched b005/e000 are one-way events, NOT reverse requests needing ACK.
     * Record/schedule a current owned coherent ring/resource recheck. Do not
     * attribute b005 to a pending buffer (it contains no buffer token), mark
     * CPU ownership or emit pixels merely because this callback returned0.
     * No bus reentry. Missing callback fails closed, retaining last_rx.
     */
    int (*notification)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_output_notification *);
    /* Optional thread-context observation at the END of an async service
     * batch, still under its whole-call reservation. Retry retained transient
     * ring observations even when no further notification arrives. Must not
     * fabricate an event, reenter the bus, consume rings without independently
     * proven producer exclusion, or equate quiet transport with DMA retirement.
     * A pending observation may remain pending on return0. */
    int (*recheck)(void *, uint64_t epoch, uint64_t deadline_ns);
    int (*shm)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_shm_request *, struct x9_vcp_shm_mapping *);
    int (*alloc)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_mem_request *, struct x9_vcp_mem_allocation *);
    int (*clock)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_clock_request *);
    int (*wait)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_wait_request *, enum x9_vcp_wait_outcome *);
    int (*vp)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_vp_request *, struct x9_vcp_common_snapshot *, uint32_t *bitdepth);
};
struct x9_vcp_bus_call_log {
    uint64_t epoch;
    uint32_t request_attempted, received, reverse_replies, ack_matched;
    uint32_t last_errno;
    uint32_t prepare_attempted, prepared, notifications;
    uint32_t queue_empty_observed, batch_limit_reached; /* async/RX baseline */
    uint8_t last_rx[X9_VCP_WIRE_BYTES]; /* owned copy, including a rejected packet */
};
/* Results/log are disjoint writable host storage, retained by caller on error.
 * return0 means ONLY a matching firmware control ACK: aliases still require
 * owned-pool resolution/cache sync. It does not mean decode or pixel success.
 * One <=3s deadline covers all sends/reverse work and the final ACK. Firmware
 * errors retain signed status; timeout/protocol/handler errors do not retry.
 * QUERY must reference the same retained instance as init; no inferred token.
 */
int x9_vcp_bus_init(struct x9_vcp_bus *, const struct x9_vcp_init *,
    const struct x9_vcp_bus_reverse_ops *, uint32_t timeout_ms,
    struct x9_vcp_init_result *, struct x9_vcp_bus_call_log *);
int x9_vcp_bus_query(struct x9_vcp_bus *, const struct x9_vcp_init *,
    const struct x9_vcp_query *, const struct x9_vcp_bus_reverse_ops *,
    uint32_t timeout_ms, struct x9_vcp_query_result *, struct x9_vcp_bus_call_log *);
/* Nonempty START only; no reset/EOS/DEINIT inferred.
 * Require the retained successful b000 result, same service/context and exact
 * alias. Mandatory prepare_start performs actual submission preparation, not
 * an empty success callback. First drain existing RX under the reservation:
 * reject old control ACKs and dispatch real async/reverse work. Empty RX is
 * checked by a physical poll; at64 packets without an empty baseline fail
 * closed before preparation/new request. One <=3s deadline covers baseline, preparation, physical
 * send/peer consumption, all reverse work/notifications and the matching b001.
 * No retry after preparation or uncertain send. Signed firmware failure is
 * preserved in result even when the bus quarantines on EIO. Success is ONLY
 * b001: caller must separately establish HW completion, coherent output rings,
 * linear layout and CPU cache ownership before pixels/release. A later fresh
 * START may retain all earlier buffers rather than recycling them; it still
 * needs an actually resolved prior control call and independent qualification.
 * This does not register a platform or raise READY/profiles.
 */
int x9_vcp_bus_decode_start(struct x9_vcp_bus *, const struct x9_vcp_init *,
    const struct x9_vcp_init_result *, const struct x9_vcp_start *,
    const struct x9_vcp_bus_reverse_ops *, uint32_t timeout_ms,
    struct x9_vcp_start_result *, struct x9_vcp_bus_call_log *);
/* Physical service2 a005 for ONE fresh output record. Shares OUT0/IN1 with
 * INIT/QUERY/START; owner must exclude unresolved prior control calls for this
 * instance, not discard their live DMA records. Requires successful b000 and
 * real prepare_output. The same strict RX baseline precedes preparation; uses one <=3s
 * deadline across preparation, peer consumption, reverse work and a matching
 * b005 notification. The notification is recorded/callback-dispatched, NOT
 * counted as ack_matched and NOT attributed to a buffer (it has no token).
 * return0 means only send+matching INSTANCE resource notification, not output
 * acceptance, hardware completion, linear layout, CPU ownership or release.
 * Failure after preparation/send retains all potentially exposed resources;
 * no retry, flush, EOS, ring dequeue or successful empty callback. */
int x9_vcp_bus_register_output(struct x9_vcp_bus *, const struct x9_vcp_init *,
    const struct x9_vcp_init_result *, const struct x9_vcp_bus_reverse_ops *,
    uint32_t timeout_ms, struct x9_vcp_output_notification *,
    struct x9_vcp_bus_call_log *);
/* Bounded thread-context async pump for an ALREADY initialized live instance.
 * Reserve OUT0/IN1/lifetime exactly as a control call; drain owned RX before
 * one physical poll and service b005/e000 or actual reverse requests with the
 * SAME parsers/callbacks/replies. No new a001/a005, unsolicited SHM bootstrap,
 * retransmission or control-ACK acceptance. Stale ACK/unknown instance/message
 * fails closed, retaining last_rx and quarantining rather than dropping it.
 * max_packets is1..64; timeout_ms is1..3000 and bounds the WHOLE batch including
 * real WAITISR/clock/allocator callbacks and replies. No waiting for new input
 * when both software queue and one physical poll are empty: this is successful
 * idle, not a transport timeout. At the packet limit, unprocessed software or
 * latched physical RX stays owned for the next batch; no blind clear.
 * Optional recheck executes under the reservation even on an empty batch.
 * A return0/log is NOT codec completion, buffer acceptance, producer exclusion,
 * pixels or release; repeated scheduling belongs to the native session owner,
 * not an automatic worker or missing platform/provider success stub.
 * Results/log are stable resident disjoint host storage, retained on error. */
int x9_vcp_bus_service(struct x9_vcp_bus *, const struct x9_vcp_init *,
    const struct x9_vcp_init_result *, const struct x9_vcp_bus_reverse_ops *,
    uint32_t max_packets, uint32_t timeout_ms, struct x9_vcp_bus_call_log *);
#endif
#endif
