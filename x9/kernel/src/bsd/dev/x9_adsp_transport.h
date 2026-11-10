/* MT6993 ADSP-v3 mailbox owner. This does not register a speaker backend.
 * Board callbacks must implement real power/reset/firmware/SMC operations.
 * No automatic MMIO mapping, guessed firmware, or user-set READY exists. */
#ifndef X9_ADSP_TRANSPORT_H
#define X9_ADSP_TRANSPORT_H
#include "x9_audio.h"
#include "x9_audio_dsp.h"
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#define X9_ADSP_OUTER_HEADER 16u
#define X9_ADSP_RX_BYTES 240u /* 60 four-byte slots, NOT 60 eight-byte slots */
#define X9_ADSP_EVENT_BYTES (X9_ADSP_RX_BYTES - X9_ADSP_OUTER_HEADER)
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
    /* power_on holds clocks/sleep protection through the boot wait and must
     * fully unwind on error. After READY, runtime MMIO uses wake_lock. */
    int (*power_on)(void *);
    /* Failure retains an accessible domain lease, never partially gates the
     * domain. reset failure must also leave this lease intact for recovery. */
    int (*power_off)(void *);
    /* reset must halt this core, discard its previous command queue and
     * leave MMIO accessible until core_start. A successful reset is the
     * only way out of quarantine. Both must have bounded completion. */
    int (*stop_and_reset)(void *);
    int (*core_start)(void *);
    /* Implement OPPO adsp_pre_wake_lock/unlock semantics (including sleep
     * protection/semaphores). Failed lock must leave no partial ownership.
     * Held across TX consumption AND ACK, not just the MMIO write. */
    int (*wake_lock)(void *);
    int (*wake_unlock)(void *);
};
/* All entrypoints are thread-context, serialized per core. Callbacks may not
 * re-enter this transport or take the X9Audio service lock. Resources and
 * callback context live for the boot. register itself does NO MMIO. */
int x9_adsp_register_transport(const struct x9_adsp_platform *);
int x9_adsp_boot(uint32_t core); /* fresh reset -> core_start -> READY <=2s */
int x9_adsp_shutdown(uint32_t core);
/* MSG_ONLY/PAYLOAD only; kernel->DSP, NEED_ACK/BYPASS/DIRECT. On failure
 * after doorbell, do not retry: even a late matching ACK cannot unpoison
 * this session. reply is required only for NEED_ACK; never changed on error.
 * DSP reply param1/param2 are returned unchanged, NOT assumed to mean errno. */
int x9_adsp_request(uint32_t core, const struct x9_dsp_message *,
                    struct x9_dsp_message *reply, uint32_t ack_timeout_ms);
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
