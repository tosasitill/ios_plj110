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
/* Independent diagnostics v1; old PCM/status ABI stays unchanged. Reading
 * this never powers the codec or starts calibration/recording. */
#define X9_MIC_HW_POWERED 1u
#define X9_MIC_HW_CALIBRATED 2u
#define X9_MIC_HW_CAPTURE 4u
#define X9_MIC_HW_RESTORE_FAILED 8u
enum x9_mic_hardware_stage {
    X9_MIC_HW_UNBOUND, X9_MIC_HW_RESOURCES, X9_MIC_HW_ID,
    X9_MIC_HW_INIT, X9_MIC_HW_CALIBRATION, X9_MIC_HW_ADC,
    X9_MIC_HW_DMA, X9_MIC_HW_STOPPED
};
struct x9_mic_hardware_status {
    uint32_t version, stage, flags, last_errno, restore_errno;
    uint32_t swcid, internal_ready, scanned_phases;
    int32_t chosen_phase[2], cycle[2];
    uint32_t monitor, last_register;
    uint64_t calibration_ns;
    uint32_t vaud18_monitor, reserved[3];
};
#define X9_MIC_GET_HARDWARE _IOR('M', 0x93, struct x9_mic_hardware_status)
/* Explicit root-only cold power diagnostic. Never starts ADC, calibration
 * loopback or DMA; no READY/source publication. One attempt per boot.
 * GET_BOARD is a snapshot only, including after a failed ioctl. */
#define X9_MIC_POWER_PROBE _IO('M', 0x94)
#define X9_MIC_BOARD_ATTEMPTED 1u
#define X9_MIC_BOARD_POWER_OK 2u
#define X9_MIC_BOARD_RESTORED 4u
#define X9_MIC_BOARD_RETAINED 8u
enum x9_mic_board_stage {
    X9_MIC_BOARD_UNTRIED, X9_MIC_BOARD_BASELINE, X9_MIC_BOARD_WAKE,
    X9_MIC_BOARD_BUS, X9_MIC_BOARD_XO, X9_MIC_BOARD_CODEC,
    X9_MIC_BOARD_RESTORE, X9_MIC_BOARD_OFF
};
struct x9_mic_board_status {
    uint32_t version, stage, flags, last_errno, restore_errno;
    uint32_t gpio9_before, gpio9_after, sleep_before, sleep_after;
    uint32_t sleep_stat, sleep_stat2, gate_before, gate_after;
    uint32_t bus_status, bus_start, bus_fifo, last_register;
    uint32_t xo_before, xo_after, timing_hz, reserved[4];
};
#define X9_MIC_GET_BOARD _IOR('M', 0x95, struct x9_mic_board_status)
/* Shares POWER_PROBE's one-attempt budget. Real cold power + MTKAIF loopback
 * phase scan only: never enables ADC/DMA or publishes a PCM source. */
#define X9_MIC_CALIBRATE _IO('M', 0x96)
#define X9_MIC_AFE_ATTEMPTED 1u
#define X9_MIC_AFE_ACQUIRED 2u
#define X9_MIC_AFE_CALIBRATED 4u
#define X9_MIC_AFE_RESTORED 8u
#define X9_MIC_AFE_RETAINED 16u
/* INIT/SIDEBANDS have no inverse in the vendor API; these mean ISSUED,
 * including errors, not that secure configuration was rolled back. */
#define X9_MIC_AFE_INIT_ISSUED 32u
#define X9_MIC_AFE_SIDEBANDS_ISSUED 64u
#define X9_MIC_AFE_SRAM_HELD 128u
enum x9_mic_afe_stage {
    X9_MIC_AFE_UNTRIED, X9_MIC_AFE_BASELINE, X9_MIC_AFE_SMC_INIT,
    X9_MIC_AFE_DOMAINS, X9_MIC_AFE_MUX, X9_MIC_AFE_GATES,
    X9_MIC_AFE_SMC_SRAM, X9_MIC_AFE_ENGINE, X9_MIC_AFE_PADS,
    X9_MIC_AFE_SCAN, X9_MIC_AFE_RESTORE, X9_MIC_AFE_OFF
};
struct x9_mic_afe_status {
    uint32_t version, stage, flags, last_errno, restore_errno;
    uint32_t ap_vote_before, ap_vote_after, all_vote_before, all_vote_after;
    uint32_t ack_before, ack_after, done;
    uint32_t vlp_before, vlp_after, fenc_before, fenc_after;
    uint32_t top0_before, top0_after, top4_before, top4_after;
    uint32_t engine_before, engine_after, engine_monitor;
    uint32_t spm_before, spm_after, cbip_before, cbip_after, last_register;
    /* Pins 98,100,101,102,103: mode[4:0], DIR8, IES9, SMT10, PU11, PD12.
     * 99/drive strength are never changed. IES is observed, not enabled by
     * the vendor input-schmitt-enable state. */
    uint32_t pins_before[5], pins_after[5];
    uint32_t foreign_vote_before, foreign_vote_after; /* XPU1..9 AUDIO bit, by XPU */
    int64_t smc_result[4]; /* INIT(0), SIDEBANDS(7), SRAM_REQ(3), REL(4) */
};
#define X9_MIC_GET_AFE _IOR('M', 0x97, struct x9_mic_afe_status)
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
