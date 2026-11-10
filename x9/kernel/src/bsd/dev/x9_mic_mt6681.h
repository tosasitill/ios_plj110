/* MT6681 cold-owned codec sequencer; no implicit SCP/I2C ownership. */
#ifndef X9_MIC_MT6681_H
#define X9_MIC_MT6681_H
#include "x9_microphone.h"
#ifdef KERNEL
/* The board must prove exclusive codec ownership and a fresh reset, acquire
 * SCP wake/bus arbitration, enable GPIO9 and MT6687 DCXO, and hold AFE clocks
 * and MOSI/MISO pads before calling this code. Every start is cold-owned;
 * borrowed Android/SCP/other audio state is NOT supported. read/write use
 * address 0x6b, 16-bit BIG-ENDIAN register, 8-bit value; positive BSD errno.
 * Each transport transaction must have a bounded timeout. No blind retry. */
#define X9_MT6681_COLD_OWNED 1u
#define X9_MT6681_MTKAIFV4 2u
#define X9_MT6681_R_MISO1 4u
#define X9_MT6681_REQUIRED 7u
struct x9_mic_mt6681_io {
    uint32_t version, flags;
    void *cookie;
    int (*read8)(void *, uint16_t, uint8_t *);
    int (*write8)(void *, uint16_t, uint8_t);
};
/* One journal for restoration and final RW qualification. value is the
 * requested masked value, not a second snapshot or a hardware observation. */
struct x9_mic_codec_saved { uint16_t reg; uint8_t mask, old, value; };
struct x9_mic_mt6681 {
    struct x9_mic_mt6681_io io;
    volatile uint32_t *afe;
    struct x9_mic_hardware_status status;
    uint32_t old_pad, old_test;
    uint8_t audio_saved[11];
    bool touched, audio_snapshot, afe_snapshot, poisoned;
    struct x9_mic_analog_status analog;
    /* Persistent per-write RW field journal, including a failed write. */
    struct x9_mic_codec_saved analog_saved[80];
    uint32_t analog_pending;
    uint64_t analog_deadline;
    bool analog_owned;
    struct x9_mic_digital_status *digital;
    struct x9_mic_codec_saved digital_saved[64];
    uint32_t digital_pending;
    uint64_t digital_deadline;
    bool digital_owned, digital_uncertain;
};
/* Caller serializes all operations, including status snapshots. This is a
 * power/calibration prerequisite, NOT an ADC/PCM backend and never sets READY.
 * Both MISO0 and MISO1 must cross a cycle boundary; phase0 fallback is rejected.
 * The explicit analog_l diagnostic does not enable ADDA/PCM. */
int x9_mic_mt6681_power_calibrate(struct x9_mic_mt6681 *,
    const struct x9_mic_mt6681_io *, volatile uint32_t *afe);
/* Real cold init + VAUD18 monitor, without touching AFE/pads/loopback/ADC.
 * Requires COLD_OWNED; unlike power_calibrate it does not claim MTKAIF pads.
 * Caller must unprepare, then return GPIO9 to the proven off baseline. */
int x9_mic_mt6681_power_on(struct x9_mic_mt6681 *,
    const struct x9_mic_mt6681_io *);
/* Upgrade io.flags to REQUIRED only AFTER the board has acquired AFE/pads.
 * Reuses the successful power_on, never repeats cold initialization. */
int x9_mic_mt6681_calibrate_powered(struct x9_mic_mt6681 *, volatile uint32_t *);
/* Call only after DMA/ADC are stopped, while transport/XO are still held.
 * On error retain resources; caller must NOT power down an unquiesced codec.
 * Success still requires board unprepare to return GPIO9/rails to off baseline:
 * MTC/DA_INTF trigger sequences are not reversible register snapshots. */
int x9_mic_mt6681_unprepare(struct x9_mic_mt6681 *);
int x9_mic_mt6681_verify_capture(struct x9_mic_mt6681 *);
int x9_mic_mt6681_analog_l(struct x9_mic_mt6681 *);
/* Requires powered, calibrated and RC-verified ADC_L; leaves SRC/TX active
 * only for the board's bounded RX diagnostic. Stop before clock/pad release. */
int x9_mic_mt6681_digital_l(struct x9_mic_mt6681 *, struct x9_mic_digital_status *);
int x9_mic_mt6681_digital_stop(struct x9_mic_mt6681 *);
/* Read-only bounded check before either end's shutdown. A conflict poisons
 * this transaction; this is not global codec ownership or a DMA drain. */
int x9_mic_mt6681_digital_restore_guard(struct x9_mic_mt6681 *);
#endif
#endif
