/* PLJ110 haptic ABI. Fixed-width, no pointers, no Apple/private structures. */
#ifndef X9_HAPTIC_H
#define X9_HAPTIC_H
#include <stdint.h>
#include <sys/ioccom.h>
#define X9_HAPTIC_VERSION 1u
#define X9_HAPTIC_INPUT_RATE 48000u
#define X9_HAPTIC_RTP_RATE 12000u
#define X9_HAPTIC_MAX_FRAMES 1024u
#define X9_HAPTIC_RING 4096u
#define X9_HAPTIC_READY 1u
#define X9_HAPTIC_RUNNING 2u
#define X9_HAPTIC_FAULT 4u
#define X9_HAPTIC_THROTTLED 8u
#define X9_HAPTIC_OFF_FAILED 16u
#define X9_HAPTIC_RAW 0u
#define X9_HAPTIC_RETUNE 1u
/* Scene IDs are OUR ABI, not undocumented Apple SystemSoundIDs. The native
 * Actuator route uses PCM instead, preserving Apple's own timing/patterns. */
enum x9_haptic_scene {
    X9_HAPTIC_SELECTION=1, X9_HAPTIC_LIGHT, X9_HAPTIC_MEDIUM,
    X9_HAPTIC_HEAVY, X9_HAPTIC_SOFT, X9_HAPTIC_RIGID,
    X9_HAPTIC_SUCCESS, X9_HAPTIC_WARNING, X9_HAPTIC_ERROR,
    X9_HAPTIC_KEYBOARD, X9_HAPTIC_HOME, X9_HAPTIC_LONG_PRESS,
    X9_HAPTIC_NOTIFICATION, X9_HAPTIC_RINGTONE, X9_HAPTIC_ALARM,
    X9_HAPTIC_ACCESSIBILITY, X9_HAPTIC_GAME_TRANSIENT,
    X9_HAPTIC_GAME_CONTINUOUS, X9_HAPTIC_SCENE_COUNT
};
struct x9_haptic_status {
    uint32_t version, mode, flags, chip_id;
    uint32_t input_rate, rtp_rate, f0_millihz, gain;
    uint32_t queued_frames, last_errno, hw_state, hw_fault;
    uint64_t generation, accepted_frames, submitted_frames;
    uint64_t completed_streams, underruns, rejected_frames;
    uint64_t last_write_ns, last_stop_ns;
    uint64_t throttled_rtp_frames; /* duty-limit conversion, not rendered PCM */
};
struct x9_haptic_start {
    uint32_t version, conversion, intensity, sharpness;
    uint64_t generation; /* output; every subsequent PCM packet must match */
};
struct x9_haptic_packet {
    uint32_t version, frames;
    uint64_t generation, sequence; /* exactly increasing, first packet = 0 */
    int16_t samples[X9_HAPTIC_MAX_FRAMES]; /* 48kHz mono signed LE */
};
struct x9_haptic_effect {
    uint32_t version, scene, intensity, sharpness, duration_ms, reserved;
};
#define X9_HAPTIC_GET_STATUS _IOR('H',0,struct x9_haptic_status)
#define X9_HAPTIC_START _IOWR('H',1,struct x9_haptic_start)
#define X9_HAPTIC_STOP _IO('H',2)
#define X9_HAPTIC_EFFECT _IOW('H',3,struct x9_haptic_effect)
#define X9_HAPTIC_DRAIN _IO('H',4)
#define X9_HAPTIC_GET_STATUS_NOWAIT _IOR('H',5,struct x9_haptic_status)
#ifdef KERNEL
void sweet_x9_haptic_init(void);
int sweet_x9_haptic_mode(void);
#endif
#endif
