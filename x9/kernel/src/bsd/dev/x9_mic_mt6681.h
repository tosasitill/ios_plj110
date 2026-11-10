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
struct x9_mic_mt6681 {
    struct x9_mic_mt6681_io io;
    volatile uint32_t *afe;
    struct x9_mic_hardware_status status;
    uint32_t old_pad, old_test;
    uint8_t audio_saved[11];
    bool touched, audio_snapshot, afe_snapshot, poisoned;
};
/* Caller serializes all operations, including status snapshots. This is a
 * power/calibration prerequisite, NOT an ADC/PCM backend and never sets READY.
 * Both MISO0 and MISO1 must cross a cycle boundary; phase0 fallback is rejected.
 * Analog routing, DCC micbias/ADC RC tune and ADDA remain board responsibilities. */
int x9_mic_mt6681_power_calibrate(struct x9_mic_mt6681 *,
    const struct x9_mic_mt6681_io *, volatile uint32_t *afe);
/* Real cold init + VAUD18 monitor, without touching AFE/pads/loopback/ADC.
 * Requires COLD_OWNED; unlike power_calibrate it does not claim MTKAIF pads.
 * Caller must unprepare, then return GPIO9 to the proven off baseline. */
int x9_mic_mt6681_power_on(struct x9_mic_mt6681 *,
    const struct x9_mic_mt6681_io *);
/* Call only after DMA/ADC are stopped, while transport/XO are still held.
 * On error retain resources; caller must NOT power down an unquiesced codec.
 * Success still requires board unprepare to return GPIO9/rails to off baseline:
 * MTC/DA_INTF trigger sequences are not reversible register snapshots. */
int x9_mic_mt6681_unprepare(struct x9_mic_mt6681 *);
int x9_mic_mt6681_verify_capture(struct x9_mic_mt6681 *);
#endif
#endif
