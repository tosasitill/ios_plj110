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
/* Named AIN0/PGA_L/ADC_L only: board microphone position is unproven.
 * Ordinary DCC/2.7V bias0+2, bounded RC readback then power-off restore.
 * Shares the cold diagnostic budget; no UL SRC/TX/DMA or READY publication. */
#define X9_MIC_ANALOG_L _IO('M', 0x98)
#define X9_MIC_ANALOG_ATTEMPTED 1u
#define X9_MIC_ANALOG_PREPARED 2u
#define X9_MIC_ANALOG_RC_OK 4u
#define X9_MIC_ANALOG_RESTORED 8u
#define X9_MIC_ANALOG_RETAINED 16u
enum x9_mic_analog_stage {
    X9_MIC_ANALOG_UNTRIED, X9_MIC_ANALOG_ID, X9_MIC_ANALOG_BASELINE,
    X9_MIC_ANALOG_INIT, X9_MIC_ANALOG_CLOCKS, X9_MIC_ANALOG_BIAS,
    X9_MIC_ANALOG_DCC, X9_MIC_ANALOG_PGA, X9_MIC_ANALOG_ADC,
    X9_MIC_ANALOG_RC, X9_MIC_ANALOG_RESTORE, X9_MIC_ANALOG_OFF
};
struct x9_mic_analog_status {
    uint32_t version, stage, flags, last_errno, restore_errno;
    uint32_t hwcid_h, hwcid_l, ain, adc, mic_type, bias_mv, gain_x2, rc_sw, rc_monitor;
    uint32_t pga_active, adc_active, bias0_active, bias2_active, dcc_active;
    uint32_t clksq_active, clocks_active;
    uint32_t pga_after, adc_after, bias0_after, bias2_after, dcc_after;
    uint32_t journal_count, journal_restored, last_register, reserved;
    uint64_t elapsed_ns;
};
#define X9_MIC_GET_ANALOG _IOR('M', 0x99, struct x9_mic_analog_status)
/* Explicit cold ADC_L -> codec UL1 -> R-MISO1 -> AFE UL0 diagnostic.
 * GET is memory-only. Shares the one-attempt budget; never starts a memif,
 * delivers PCM or publishes READY. READBACK is configuration, not sound proof.
 * Codec IIR is deliberately bypassed; AFE IIR has explicit 35Hz coefficients. */
#define X9_MIC_DIGITAL_L _IO('M', 0x9a)
#define X9_MIC_DIGITAL_ATTEMPTED 1u
#define X9_MIC_DIGITAL_CODEC_ON 2u
#define X9_MIC_DIGITAL_AFE_ON 4u
#define X9_MIC_DIGITAL_READBACK 8u
#define X9_MIC_DIGITAL_RESTORED 16u
#define X9_MIC_DIGITAL_RETAINED 32u
enum x9_mic_digital_stage {
    X9_MIC_DIGITAL_UNTRIED, X9_MIC_DIGITAL_PREREQUISITES,
    X9_MIC_DIGITAL_BASELINE, X9_MIC_DIGITAL_CODEC_CLOCKS,
    X9_MIC_DIGITAL_CODEC_SRC, X9_MIC_DIGITAL_CODEC_TX,
    X9_MIC_DIGITAL_AFE_CONFIG, X9_MIC_DIGITAL_AFE_ENABLE,
    X9_MIC_DIGITAL_OBSERVE, X9_MIC_DIGITAL_RESTORE, X9_MIC_DIGITAL_OFF
};
struct x9_mic_digital_status {
    uint32_t version, stage, flags, last_errno, restore_errno;
    uint32_t nominal_rate, delay_data, delay_cycle;
    /* SRC = CON0_2:CON0_1:CON0_0; TOP = AUDIO_TOP_CON0:AFE_TOP_CON0. */
    uint32_t codec_src, codec_top, codec_tx[2], codec_pad, codec_mux[2], codec_mode4;
    uint32_t codec_after_src, codec_after_top, codec_after_tx[2], codec_after_pad, codec_after_mode4;
    uint32_t afe_ul, afe_rx[2], afe_pad, afe_clock, afe_gate, afe_monitor[2];
    uint32_t afe_after_ul, afe_after_rx[2], afe_after_pad, afe_after_clock, afe_after_gate;
    uint32_t codec_journal_count, codec_journal_restored, afe_journal_count, afe_journal_restored;
    /* bit31 distinguishes AFE offsets from codec addresses; no MMIO on GET. */
    uint32_t last_register, reserved[5];
    uint64_t elapsed_ns;
};
#define X9_MIC_GET_DIGITAL _IOR('M', 0x9b, struct x9_mic_digital_status)
/* Cached VUL1 qualification/stream evidence, independent of existing ABIs.
 * digital-l only reads registers while its AFE lease is held. SRAM_REQUEST
 * is a power request, NOT an allocator or a DMA drain acknowledgement.
 * ELIGIBLE/IDLE/ROUTE_EMPTY must never be interpreted as PCM or READY. */
#define X9_MIC_DMA_ATTEMPTED 1u
#define X9_MIC_DMA_REGISTERS_VALID 2u
#define X9_MIC_DMA_IDLE 4u
#define X9_MIC_DMA_ROUTE_EMPTY 8u
#define X9_MIC_DMA_BOUND 16u
#define X9_MIC_DMA_STARTED 32u
#define X9_MIC_DMA_DRAINED 64u
#define X9_MIC_DMA_RESTORED 128u
#define X9_MIC_DMA_RETAINED 256u
#define X9_MIC_DMA_NEED_MAP 1u
#define X9_MIC_DMA_NEED_OWNER 2u
#define X9_MIC_DMA_NEED_COPY 4u
#define X9_MIC_DMA_NEED_QUIESCE 8u
#define X9_MIC_DMA_NEED_SOURCE_ID 16u
enum x9_mic_dma_stage { X9_MIC_DMA_UNBOUND, X9_MIC_DMA_BASELINE,
    X9_MIC_DMA_CONFIG, X9_MIC_DMA_ENABLE, X9_MIC_DMA_POLL,
    X9_MIC_DMA_DISABLE, X9_MIC_DMA_DRAIN, X9_MIC_DMA_RESTORE,
    X9_MIC_DMA_OFF };
struct x9_mic_dma_status {
    uint32_t version, stage, flags, last_errno, restore_errno;
    uint32_t blockers, eligible, adc_channel, ring_bytes, memif_con0;
    uint32_t active_memifs, last_register;
    uint32_t con0_before, con0_after, monitor_before, monitor_after;
    uint32_t route[2][7]; /* words 0,1,2,4,5,6,7; never read reserved word3 */
    uint32_t journal_count, journal_restored, pending_flags, restore_register;
    uint64_t base, end, cur_before, cur_after, observed_frames, dropped_frames, elapsed_ns;
};
#define X9_MIC_GET_DMA _IOR('M', 0x9c, struct x9_mic_dma_status)
#ifdef KERNEL
#include <stdbool.h>
/* Thread context only. start/stop are serialized and called WITHOUT the PCM
 * lock. stop must disable/drain DMA and restore owned rails/clocks even after
 * a partially failed start. Failure latches FAULT; no automatic retry. */
#define X9_MIC_BACKEND_COLD_ONESHOT 1u
struct x9_mic_backend {
    uint32_t version, flags; /* private v2, optional COLD_ONESHOT */
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
