/* MT6993 ADSP-v3 mailbox owner. This does not register a speaker backend.
 * Board callbacks must implement real power/reset/firmware/SMC operations.
 * No automatic MMIO mapping, guessed firmware, or user-set READY exists. */
#ifndef X9_ADSP_TRANSPORT_H
#define X9_ADSP_TRANSPORT_H
#include <stdbool.h>
#include "x9_audio.h"
#include "x9_audio_dsp.h"
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#define X9_ADSP_OUTER_HEADER 16u
#define X9_ADSP_RX_BYTES 240u /* 60 four-byte slots, NOT 60 eight-byte slots */
#define X9_ADSP_EVENT_BYTES (X9_ADSP_RX_BYTES - X9_ADSP_OUTER_HEADER)
#define X9_ADSP_PLATFORM_VERSION 3u
struct x9_adsp_event {
    uint32_t id, length, generation;
    uint8_t data[X9_ADSP_EVENT_BYTES];
};
struct x9_adsp_platform {
    uint32_t version, core;
    /* Device-memory mappings: AP->DSP mbox0/2, DSP->AP mbox1/3.
     * At least 0x110 bytes each. SET(+0x100) and CLR(+0x10c) are also
     * send/receive status in this DT; the last 16 RX bytes are the VP pin
     * and must never be overwritten. Exclusive polling ownership required. */
    volatile uint32_t *tx, *rx;
    void *context;
    /* power_on holds clocks/sleep protection through the boot wait.
     * power_held reports a retained safety lease after an ambiguous
     * vote/unwind failure. After READY, runtime MMIO uses wake_lock. */
    int (*power_on)(void *);
    /* Failure retains an accessible domain lease, never partially gates the
     * domain. reset failure must also leave this lease intact for recovery. */
    int (*power_off)(void *);
    /* reset must halt this core, discard its previous command queue and
     * leave MMIO accessible until core_start. A successful reset is the
     * only way out of quarantine. Both must have bounded completion. */
    int (*stop_and_reset)(void *);
    int (*core_start)(void *);
    /* Implement the active platform wake semantics (v1 may use prelock/
     * semaphores; v3 must not invent those unregistered callbacks).
     * Failed lock must leave no partial ownership.
     * Held across TX consumption AND ACK, not just the MMIO write. */
    int (*wake_lock)(void *);
    int (*wake_unlock)(void *);
    bool (*power_held)(void *); /* cached ownership only, not accessibility */
    /* Private platform v3: fresh, bounded, side-effect-free validation of
     * this same power/clock/mailbox lease, both HALTED and running. AO-safe
     * checks precede any domain read. Never acquire/reset/wake or substitute
     * cached power_held for physical evidence; mappings remain pinned on
     * failure. Return positive errno; EAGAIN is fatal, not an empty RX pin.
     * stop/reset/start must independently qualify their own hardware paths.
     * wake_unlock also qualifies any hardware it touches on error; it may
     * release a software wake token without reading a gated mailbox.
     * After validation failure this operation performs NO mailbox or reset/
     * power-off accesses; explicit shutdown may qualify anew for recovery. */
    int (*validate)(void *);
};
/* All entrypoints are thread-context, serialized per core. Callbacks may not
 * re-enter this transport or take the X9Audio service lock. Resources and
 * callback context live for the boot. register itself does NO MMIO. */
int x9_adsp_register_transport(const struct x9_adsp_platform *);
int x9_adsp_boot(uint32_t core); /* fresh reset -> core_start -> READY <=2s */
int x9_adsp_shutdown(uint32_t core);
/* Combined DSP+AFE teardown: halt discards the core queue but retains its
 * parent power lease; release is allowed ONLY after a successful halt.
 * The speaker coordinator mutes/disables/drains AFE before halt, restores
 * AFE/pads/clocks before release, and releases its AP ticket LAST.
 * Neither call authorizes another session; boot still needs fresh READY. */
int x9_adsp_halt(uint32_t core);
int x9_adsp_release(uint32_t core);
/* MSG_ONLY/PAYLOAD only; kernel->DSP, NEED_ACK/BYPASS/DIRECT. On failure
 * after doorbell, do not retry: even a late matching ACK cannot unpoison
 * this session. reply is required only for NEED_ACK; never changed on error.
 * DSP reply param1/param2 are returned unchanged, NOT assumed to mean errno. */
int x9_adsp_request(uint32_t core, const struct x9_dsp_message *,
                    struct x9_dsp_message *reply, uint32_t ack_timeout_ms);
/* Private physical-DMA caller only. Caller holds its native region mutex;
 * callback context/memory remain pinned through faults and actual halt.
 * Validate this exact generation, registered pool/table/scene/indices and
 * immutable command policy, without re-entering transport or caller mutex.
 * Runs under transport lock before TX, throughout consumption/ACK wait and
 * before reply publication. EAGAIN from validator is fatal. No default or
 * userspace validator; plain x9_adsp_request still rejects DMA.
 * Lock: native IPI DMA -> transport -> board / physical-memory supplier. */
struct x9_adsp_request_guard {
    uint32_t version,generation;
    void *context;
    int (*validate)(void *,const struct x9_dsp_message *);
};
int x9_adsp_request_guarded(uint32_t core,const struct x9_dsp_message *,
    struct x9_dsp_message *reply,uint32_t ack_timeout_ms,
    const struct x9_adsp_request_guard *);
/* Quarantine blocks mailbox sends; it does not mute a PA or quiesce AFE.
 * The eventual speaker backend must stop those on EVERY transport error. */
int x9_adsp_poll(uint32_t core); /* bounded single-packet receive */
int x9_adsp_next_event(uint32_t core, struct x9_adsp_event *);
int x9_adsp_status(uint32_t core, X9AudioDSPStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
