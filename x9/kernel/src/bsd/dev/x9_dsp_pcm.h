/* MTK PCM shared records and task control. No Linux structs on the wire. */
#ifndef X9_DSP_PCM_H
#define X9_DSP_PCM_H
#include <stdint.h>
#include "x9_audio_dsp.h"
#define X9_DSP_PCM_RECORD_BYTES 96u
#define X9_DSP_DRAM_RECORD_BYTES 32u
/* Source ABI is audio_buf.h: bridge[6 x LE64], attr[4 x LE32],
 * thresholds/period[4 x LE32], hw/memif/irq/memory[4 x u8], LE32 counter,
 * u8 ignore_irq, seven zero padding bytes. End is exclusive for the DSP
 * genpool/AFE init_ring_buf_bridge path, NOT the AFE END register value. */
struct x9_dsp_pcm_buffer {
    uint64_t base, bytes, read, write, count;
    uint32_t format, rate, channels, direction;
    uint32_t start_threshold, stop_threshold, period_frames, periods, counter;
    uint8_t hardware, memif, irq, memory, ignore_irq;
};
/* Exact 96B encoding; decode output is unchanged on failure. These helpers
 * do not map memory, validate an EMI lease or authorize DMA. */
#ifdef __cplusplus
extern "C" {
#endif
int x9_dsp_pcm_encode_buffer(const struct x9_dsp_pcm_buffer *,void *,uint32_t);
int x9_dsp_pcm_decode_buffer(const void *,uint32_t,struct x9_dsp_pcm_buffer *);
/* audio_dsp_dram: physical, AP virtual, size, AP virtual again. Values are
 * explicit provider inputs, never inferred from a CPU virtual address. */
int x9_dsp_pcm_encode_dram(uint64_t bus,uint64_t ap_virtual,uint64_t bytes,
                           void *,uint32_t);
#ifdef __cplusplus
}
#endif
#if defined(KERNEL)
#include <stdbool.h>
#include "x9_adsp_transport.h"
enum { X9PCMInitialized=1u, X9PCMOpen=2u, X9PCMParams=4u,
       X9PCMPrepared=8u, X9PCMStartIssued=16u, X9PCMFault=32u,
       X9PCMAFEParams=64u, X9PCMAFEPrepared=128u };
enum { X9PCMRegionATOD, X9PCMRegionDTOA, X9PCMRegionRing, X9PCMRegionCount };
struct x9_dsp_pcm_region { void *cpu; uint64_t bus; uint32_t bytes; };
struct x9_dsp_pcm_binding {
    uint32_t version, core, generation, scene, control_timeout_ms, copy_timeout_ms;
    /* All three regions must have a real shared-DRAM/MPU/cache lease; they
     * remain immutable and pinned after ANY ambiguous error. */
    struct x9_dsp_pcm_region region[X9PCMRegionCount];
    void *context;
    /* Side-effect-free fresh validation of the combined board lease,
     * mapping/MPU/cache binding, task routing and the same READY generation.
     * No acquisition/reset, no cached-ready substitution. Called before
     * every shared-memory access and before/after mailbox requests. */
    int (*validate)(void *);
    int (*sync_device)(void *,unsigned region,uint32_t offset,uint32_t bytes);
    int (*sync_cpu)(void *,unsigned region,uint32_t offset,uint32_t bytes);
};
#define X9_DSP_PCM_BINDING_VERSION 1u
struct x9_dsp_pcm_session {
    struct x9_dsp_pcm_binding hw;
    struct x9_dsp_pcm_buffer buffer, afe_buffer;
    uint32_t flags, error, command, reply_param1, reply_param2, pending_frames;
    uint64_t submitted, consumed, requests, lease_checks, underruns;
    bool bound, used;
};
/* Serialized by the combined board backend; thread context only. Bind is
 * memory-only and does not make a task/backend Ready. A failed session may
 * not be zeroed/rebound against the same generation as a retry shortcut:
 * physical mute/quiesce, fresh reset and new-generation admission come first.
 * Each generation allows one OPEN: old queued notifications carry no PCM
 * session token. A later playback requires a new real reset/READY generation,
 * not reopening against indistinguishable old DTOA notifications.
 * No call unmutes the PA, enables AFE, releases memory or resets the DSP. */
#ifdef __cplusplus
extern "C" {
#endif
int x9_dsp_pcm_bind(struct x9_dsp_pcm_session *,const struct x9_dsp_pcm_binding *);
int x9_dsp_pcm_open(struct x9_dsp_pcm_session *);
int x9_dsp_pcm_hw_params(struct x9_dsp_pcm_session *,uint32_t period_frames);
int x9_dsp_pcm_afe_params(struct x9_dsp_pcm_session *,
                          const struct x9_dsp_pcm_buffer *,bool prepare);
int x9_dsp_pcm_prepare(struct x9_dsp_pcm_session *,uint32_t start_frames);
int x9_dsp_pcm_submit(struct x9_dsp_pcm_session *,const int16_t *,uint32_t frames);
int x9_dsp_pcm_start(struct x9_dsp_pcm_session *);
int x9_dsp_pcm_stop(struct x9_dsp_pcm_session *);
int x9_dsp_pcm_close(struct x9_dsp_pcm_session *);
/* Consume is DSP input-ring consumption, NEVER speaker playback position.
 * Caller dispatches only this task's generation-bound AUDIO IPI9 event.
 * observe is for a provider-proven core DTOA notification; it does not poll
 * arbitrary RAM and cannot infer whether a notification belongs to us. */
int x9_dsp_pcm_event(struct x9_dsp_pcm_session *,const struct x9_adsp_event *);
int x9_dsp_pcm_observe(struct x9_dsp_pcm_session *);
#ifdef __cplusplus
}
#endif
#endif
#endif
