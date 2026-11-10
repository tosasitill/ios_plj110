/* X9 output-only audio ABI. No Apple firmware or board calibration data. */
#ifndef X9_AUDIO_H
#define X9_AUDIO_H
#include <stdint.h>
#define X9_AUDIO_CLASS "X9Audio"
#define X9_AUDIO_MAGIC 0x58394155u
#define X9_AUDIO_VERSION 1u
/* Private kernel callback contract, independent of the unchanged PCM ABI. */
#define X9_AUDIO_BACKEND_VERSION 2u
#define X9_AUDIO_PREFILL_MIN (X9_AUDIO_PERIOD * 2u)
#define X9_AUDIO_PREFILL_MAX (X9_AUDIO_PERIOD * 8u)
#define X9_AUDIO_RATE 48000u
#define X9_AUDIO_CHANNELS 2u
#define X9_AUDIO_PERIOD 480u
#define X9_AUDIO_RING 65536u
#define X9_AUDIO_RING_BYTES (X9_AUDIO_RING * X9_AUDIO_CHANNELS * 2u)
enum { X9AudioStatus, X9AudioStart, X9AudioStop, X9AudioDSPStatusSelector,
       X9AudioBoardStatusSelector, X9AudioDSPBootSelector, X9AudioDSPStopSelector,
       X9AudioAFEStatusSelector, X9AudioStreamStatusSelector,
       X9AudioAmpStatusSelector, X9AudioAmpProbeSelector,
       X9AudioResourceStatusSelector, X9AudioClockStatusSelector,
       X9AudioClockProbeSelector, X9AudioPLLStatusSelector,
       X9AudioPLLProbeSelector, X9AudioPathStatusSelector,
       X9AudioPathProbeSelector, X9AudioI2C6StatusSelector };
enum { X9AudioControlMemory, X9AudioProducerMemory, X9AudioOutputMemory };
enum { X9AudioReady = 1u, X9AudioRunning = 2u, X9AudioFault = 4u };
enum {
    X9AudioNeedPower = 1u, X9AudioNeedFirmware = 2u,
    X9AudioNeedMailbox = 4u, X9AudioNeedMemory = 8u,
    X9AudioNeedAFE = 16u, X9AudioNeedAmplifier = 32u,
    X9AudioNeedRoute = 64u, X9AudioNeedBackend = 128u
};
#define X9_AUDIO_REQUIRED 255u
/* Memory 0 is read-only in the client. seq brackets kernel updates; an odd
 * seq must not be spun on by a realtime thread: return NotRunning and retry.
 * out_frames is the hardware play cursor, NOT an elapsed-time estimate.
 * out_queued is the first frame not yet copied to the physical backend. */
typedef struct {
    uint32_t magic, version, rate, channels, period_frames, ring_frames;
    uint32_t lead_frames, state, blockers, error, seq, generation;
    uint64_t ticks_per_frame_q32, out_anchor_host, out_frames, out_queued;
    uint64_t underruns, bad_producer, starts, stops;
} X9AudioControl;
/* Memory 1 is writable. Release-store written after PCM writes. It is the
 * end sample time on the current timeline, NOT a byte count or DMA address.
 * Every kernel read is range checked. Memory 2 holds LE interleaved S16. */
typedef struct { uint64_t written; } X9AudioProducer;
/* Separate read-only diagnostic ABI, selector 3 + one scalar core (0/1).
 * DSP Online only means a fresh mailbox READY was observed; it must NEVER
 * set speaker Ready. Not configured/offline is the default without a board. */
enum { X9DSPUnconfigured, X9DSPOffline, X9DSPBooting, X9DSPOnline, X9DSPQuarantined };
#define X9_AUDIO_DSP_STATUS_VERSION 2u
enum { X9DSPGuardNone, X9DSPGuardBoot, X9DSPGuardPoll, X9DSPGuardSend,
       X9DSPGuardWait, X9DSPGuardCleanup, X9DSPGuardStop };
typedef struct {
    uint32_t version, core, state, error, generation, event_pending;
    uint32_t last_ipi, last_message;
    uint64_t sent, received, matched_acks, stray_acks, malformed;
    uint64_t timeouts, wake_errors, ready_count, event_overflows;
    uint32_t primary_error, cleanup_error, lease_error, guard_stage;
    uint64_t lease_checks;
} X9AudioDSPStatus;
/* selector 4 is a cached board snapshot; 5/6 require admin open type 2.
 * Version separate from PCM ABI. Raw SMC returns and DPSW ack are diagnostics,
 * NOT a firmware revision, physical voltage reading, or speaker-ready bit. */
enum { X9BoardRegistered=1, X9BoardExclusive=2, X9BoardPowered=4,
       X9BoardStarted=8, X9BoardWake=16, X9BoardRegistersValid=32 };
#define X9_AUDIO_BOARD_STATUS_VERSION 3u
enum { X9BoardGuardLease, X9BoardGuardHWV, X9BoardGuardCFG };
typedef struct {
    uint32_t version, flags, error, restore_error;
    uint32_t owned_vote, ap_vote, all_vote, pd_done, pd_busy;
    uint32_t pending, sleep, dpsw_req, dpsw_ack, vlogic_ack, sram_ack, smc_op;
    uint64_t smc_result[4], smc_calls, resets, timeouts;
    uint32_t rollback_error, reserved;
    uint32_t lease_error, guard_space, guard_reg, guard_expected, guard_actual, guard_reserved;
    uint64_t lease_checks;
} X9AudioBoardStatus;
/* selector 8 v2 preserves PCM ABI v1 while exposing physical prefill/cursor.
 * A cleared Running flag is not proof that stop released physical DMA/PA. */
#define X9_AUDIO_STREAM_STATUS_VERSION 2u
typedef struct {
    uint32_t version, backend_held, stop_error, prefill_frames;
    uint64_t physical_played, physical_queued, physical_anchor;
    uint32_t prefill_committed, position_retries;
} X9AudioStreamStatus;
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* A physical driver registers only after owning clocks, firmware/mailboxes,
 * EMI/cache policy, AFE, actual PA model+calibration and speaker routing.
 * All callbacks are serialized by the service. submit must copy a complete
 * period before returning 0; submit EAGAIN consumes nothing. position returns
 * the raw physical cursor INCLUDING the hidden prefill, never subtracting it,
 * and an absolute underrun count (reset to 0 at each start). A transient
 * position EAGAIN is tolerated only for a bounded cursor-observation window.
 * start must copy/sync ALL private prefill before enabling DMA, and return
 * the physical prefill-frame-0 host anchor. It may not retain the PCM pointer.
 * The service subtracts the prefill from raw played and offsets the logical
 * sample-0 anchor. Producer and out_queued still start at 0, preserving PCM
 * ABI v1; no user VA/PA reaches these callbacks.
 * stop MUST mute/quiesce DMA even when returning an error. Registration is
 * boot-lifetime; there is intentionally no untested/null backend. */
struct x9_audio_backend {
    uint32_t version, verified, lead_frames;
    void *context;
    int (*start)(void *, const int16_t *prefill, uint32_t frames, uint64_t *physical_anchor);
    int (*stop)(void *);
    int (*submit)(void *, const int16_t *, uint32_t frames);
    int (*position)(void *, uint64_t *frames, uint64_t *underruns);
};
int x9_audio_register_backend(const struct x9_audio_backend *);
#ifdef __cplusplus
}
#endif
#endif
#endif
