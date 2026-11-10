/* X9 microphone ABI v1: fixed-width fields, no user pointers. */
#ifndef X9_MICROPHONE_H
#define X9_MICROPHONE_H
#include <stdint.h>
#include <sys/ioccom.h>
#define X9_MIC_DEVICE "/dev/x9-mic"
#define X9_MIC_STATUS_DEVICE "/dev/x9-mic-status"
#define X9_MIC_RATE 48000u
#define X9_MIC_FRAME_BYTES 2u
#define X9_MIC_SOURCE_INTERNAL 1u
#define X9_MIC_READY 1u
#define X9_MIC_RUNNING 2u
#define X9_MIC_FAULT 4u
#define X9_MIC_RING_FRAMES 32768u
struct x9_mic_status {
    uint32_t version, mode, flags, supported_sources;
    uint32_t rate, channels, sample_bits, last_errno;
    uint64_t generation, captured_frames, read_frames, dropped_frames;
    uint64_t last_host_time;
    uint32_t queued_frames, source, reserved[6];
};
struct x9_mic_config {
    uint32_t version, rate, channels, sample_bits, source, reserved[3];
};
#define X9_MIC_GET_STATUS _IOR('M', 0x90, struct x9_mic_status)
#define X9_MIC_START _IOW('M', 0x91, struct x9_mic_config)
#define X9_MIC_STOP _IO('M', 0x92)
#ifdef KERNEL
#include <stdbool.h>
/* Thread context only. start/stop are serialized and called WITHOUT the PCM
 * lock. stop must disable/drain DMA and restore owned rails/clocks even after
 * a partially failed start. Failure latches FAULT; no automatic retry. */
struct x9_mic_backend {
    uint32_t version;
    int (*start)(uint64_t generation);
    int (*stop)(void);
};
int x9_mic_register_backend(const struct x9_mic_backend *);
/* Only real, cache-synchronized PCM. No IRQ-context calls or test patterns.
 * last_host_time is the monotonic host time of the last sample in this block. */
int x9_mic_deliver(uint64_t generation, const int16_t *, uint32_t frames,
                   uint64_t last_host_time);
void x9_mic_backend_fault(uint64_t generation, int error);
void x9_mic_backend_drop(uint64_t generation, uint32_t frames);
void sweet_x9_microphone_init(void);
#endif
#endif
