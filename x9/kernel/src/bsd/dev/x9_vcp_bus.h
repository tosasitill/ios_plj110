/* Native rv_v2 mailbox transport. No automatic decoder registration/READY. */
#ifndef X9_VCP_BUS_H
#define X9_VCP_BUS_H
#ifdef KERNEL
#include "x9_vcp_wire.h"
struct x9_vcp_bus;
struct x9_vcp_bus_call_log;
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
    uint32_t retained_clients; /* host lifetime holds, NOT physical users/drain */
    uint32_t registered_instances, retained_routes; /* includes boot tombstones / live cookies */
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
/* Native async-client lifetime. Retain uses the SAME live RUNNING/shared/guard
 * check, then returns a boot-unique opaque host hold. Discard refuses while
 * any hold exists, including between bus transactions or after STOPPED.
 * Release refuses a still-registered instance using THIS hold. After route
 * removal, it only drops this exact hold; it does not stop, drain, release power,
 * retire mappings or grant DMA ownership. The client must close admission
 * and join all its callbacks before releasing. Token initially0 on retain;
 * resident outputs are disjoint from inputs. No reentry from bus callbacks. */
int x9_vcp_bus_retain_shared(struct x9_vcp_bus *, uint64_t epoch,
    const struct x9_vcp_shm_region *, uint64_t *token);
int x9_vcp_bus_release_shared(struct x9_vcp_bus *, uint64_t epoch, uint64_t *token);
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
 * The provider must register its frozen identity/ops against a retained host
 * hold BEFORE calling. Calls reject unregistered or substituted handlers.
 * One bus-wide registry routes foreign reverse/async packets by exact AP
 * instance token, followed by the existing typed parser (including service2
 * b005 and captured context exceptions). No token-to-pointer cast or routing
 * by context alone. Control ACKs are ONLY for the currently outstanding call;
 * a foreign/stale ACK is rejected, never used to finish another operation.
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
/* Owned latest packet/reply evidence. A successful reply_consumed is ONLY
 * physical transport consumption. sequence/counters are host observations,
 * not FW buffer identities, producer fences or per-buffer retirement. */
struct x9_vcp_bus_route_journal {
    uint64_t epoch, instance_token, sequence;
    uint32_t foreign, enter_attempted, entered, notification;
    uint32_t reply_encoded, reply_attempted, reply_consumed, last_errno;
    uint8_t rx[X9_VCP_WIRE_BYTES], reply[X9_VCP_WIRE_BYTES];
};
struct x9_vcp_bus_reverse_ops {
    void *cookie;
    /* Mandatory registry hooks for FOREIGN dispatch only. Current caller
     * already owns its state lock and is NEVER entered recursively. Runs
     * outside bus_lock with the whole call/lifetime retained. enter must
     * acquire the exact recipient's state with a bounded, deadline-aware
     * protocol, never an unbounded cross-control mutex wait. Return0 owns it;
     * failed enter owns nothing. leave records the FINAL packet/reply/error
     * journal (including transport errors), then releases it; called exactly
     * once after every successful enter. No bus/control reentry. Closing a
     * CPU pump must NOT remove this still-live FW routing obligation. */
    int (*route_enter)(void *, uint64_t epoch, uint64_t deadline_ns);
    void (*route_leave)(void *, const struct x9_vcp_bus_route_journal *);
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
    /* Optional observation of a parsed, current-route status0 b001. The
     * exact copied request/INIT and current call journal remain resident,
     * outside bus_lock with the whole reservation/current state lock held.
     * No bus/control reentry, live stores, layout/READY or DMA grant. Keep
     * raw samples on failure; an enclosing call_end failure must invalidate
     * whole-call success without erasing those observations. Reply context
     * is preserved raw: an echo of the request context is not established. */
    int (*observe_start_reply)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_init *, const struct x9_vcp_init_result *,
        const struct x9_vcp_start *, const struct x9_vcp_start_result *,
        const struct x9_vcp_bus_call_log *);
    /* Optional matched status0 b100 observer. Invoked before route/call end,
     * outside bus_lock but retaining the whole reservation/current state lock.
     * Resolve ONLY genuinely owned advertised SHM; never cast FW/AP addresses
     * or grant READY/DMA/layout. Staged observations must be invalidated if
     * the enclosing transaction subsequently fails. No bus/control reentry. */
    int (*observe_query_reply)(void *, uint64_t epoch, uint64_t deadline_ns,
        const struct x9_vcp_query *, const struct x9_vcp_query_result *);
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
    uint64_t last_recipient_token;
    uint32_t request_attempted, received, reverse_replies, ack_matched;
    uint32_t last_errno;
    uint32_t prepare_attempted, prepared, notifications;
    uint32_t queue_empty_observed, batch_limit_reached; /* async/RX baseline */
    uint32_t routed_packets, foreign_packets;
    uint8_t last_rx[X9_VCP_WIRE_BYTES]; /* owned copy, including a rejected packet */
};
struct x9_vcp_bus_instance_status {
    uint64_t epoch, host_token, instance_token, packets, foreign_packets;
    uint32_t exposed, retired; /* request attempt / cookie removed; NOT DMA ownership */
    struct x9_vcp_bus_route_journal last;
};
/* Frozen identity/cookie/ops tied to THIS exact host lifetime hold. Only on a
 * live RUNNING/shared/guard epoch, outside any call. No callback before the
 * first INIT/QUERY request attempt; uncertainty after an attempt retains the
 * cookie until STOPPED. At most32 registrations over one bus lifetime; removed
 * entries keep identity/journal tombstones and cannot reuse a wire token.
 * All cookies must be fully initialized before registration; callers serialize
 * their external handles. No firmware request, SHM bootstrap or READY here. */
int x9_vcp_bus_register_instance(struct x9_vcp_bus *, uint64_t epoch,
    uint64_t host_token, const struct x9_vcp_init *, const struct x9_vcp_bus_reverse_ops *);
/* Rejects ANY active whole call (including a foreign callback). Exposed routes
 * require physically STOPPED bus, not host quiesce. Unexposed create rollback
 * may unregister without stopping. Native control separately verifies both
 * RETIRED domains before dropping exposed resources/refs. Release_shared
 * refuses a hold with a live route; unregister never releases the hold. */
int x9_vcp_bus_unregister_instance(struct x9_vcp_bus *, uint64_t epoch, uint64_t host_token);
/* Read-only owned status, including errors/tombstones, even after stop/fault.
 * Does not revalidate DMA/physical producer authority or resume dispatch. */
int x9_vcp_bus_get_instance_status(struct x9_vcp_bus *, uint64_t epoch,
    uint64_t host_token, struct x9_vcp_bus_instance_status *);
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
/* Physical service1 a003 for an ALREADY initialized instance. Same whole-call
 * OUT0/IN1/lifetime reservation, fresh RX baseline, one <=3s absolute deadline
 * and real reverse/foreign notification dispatch as START. Match ONLY b003,
 * preserve signed status/context/opaque bytes, quarantine failures/uncertain
 * sends. No new SHM bootstrap, VSI writes, retry, firmware reset/clock lease
 * release or host-buffer recycle. Alias must match retained b000.
 * Success means ONLY this control reply; selectors0..2 have no established
 * drain/DPB/EOS semantics here. The caller must retain ALL prior DMA records
 * and independently qualify any later submission after this state mutation.
 */
int x9_vcp_bus_decode_reset(struct x9_vcp_bus *, const struct x9_vcp_init *,
    const struct x9_vcp_init_result *, const struct x9_vcp_reset *,
    const struct x9_vcp_bus_reverse_ops *, uint32_t timeout_ms,
    struct x9_vcp_reset_result *, struct x9_vcp_bus_call_log *);
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
