/* Call-voice sph_info construction, pure half (L3; see
 * docs/call-voice-audio-chain-20261127.md).
 *
 * The factory AP->MD speech-on message is MD 0x2f20 / type 25 with a
 * 128-byte body built by SpeechDriverNormal::configSpeechInfo in the
 * Android audio HAL (disassembly archived in
 * x9/out/call-md-voice-owner/hal-config-speech-info.asm; runtime
 * values from this phone's factory MO capture,
 * x9/out/android-calls-20261115-183941/mo-call-audio.txt:790,794).
 * configSpeechInfo zeroes the 128 bytes first, then stores:
 *   +0x00 app        (driver member; MO 0)
 *   +0x01 bt         (getBtInfo(); MO 0)
 *   +0x02 rate enum  (driver member; MO 3)
 *   +0x03 enh config (SpeechDriverOpenDSP::getAdspPhoneCallEnh())
 *   +0x05 / +0x54    (one HAL branch: +0x05 = 2 and +0x54 =
 *                     1 + AudioSmartPaController::isDualSmartPA();
 *                     MO smartPA config 2, so the branch was taken)
 *   +0x1c common parameter block, 24 bytes (MO log com par[0] = 6)
 *   +0x34 debug block, 32 bytes (MO log debug info[0] = 3)
 *   +0x55 hwif       (SpeechDriverOpenDSP::getAdspPhoneCallHwIf())
 *
 * The two dynamic fields this layer closes:
 *   - enh config (+0x03) and hwif (+0x55) come from the OpenDSP
 *     driver's members mAdspPhoneCallConfig / mAdspPhoneCallHwIf,
 *     read in its constructor from /dev/usip ioctls c00844f1 /
 *     c00844f3. The mtk-usip kernel driver serves those ioctls from
 *     values its probe read out of the speech-usip-mem device-tree
 *     node: adsp-phone-call-enh-enable and adsp-phone-call-hwif.
 *     This phone's live DT carries 3 and 0. The HAL copies are u32
 *     (4 bytes) despite the 8-byte ioctl size encoding.
 *   - Gating: when the ADSP option is disabled and the SCP support
 *     feature bit is clear, configSpeechInfo stores 0 for both
 *     fields instead of calling the getters.
 *
 * The MD consumes the body in its SPC handler: body+3 is saved, and
 * body+0x55 is saved to state+0x55 with a branch on hwif
 * (x9_voice_sph_md_state below mirrors that disassembly).
 *
 * Not placed, deliberately: the MO log also prints
 * enh_dynamic_ctrl = 0x3, but its body offset is not pinned (it may
 * be the +0x03 byte under a second name); the remaining head cluster
 * (+0x04..+0x1b: path / param / ext_dev / loopback / delays / mute /
 * usip param) is zero in the MO call but its individual offsets are
 * not all pinned either. The builder leaves every unpinned byte zero,
 * which matches the MO call's values for all of them except possibly
 * enh_dynamic_ctrl.
 *
 * Nothing here sends the message: the 0x2f20 transport is the
 * SpeechMessageQueue / SPC stream, which is not in this bridge's
 * ownership (the factory MO capture's 35 MIPC frames carry no speech
 * message). The builder exists so the byte contract is executable
 * and host-tested when a transport owner appears. */
#ifndef X9_VOICE_SPH_H
#define X9_VOICE_SPH_H

#include <stdint.h>
#include <string.h>

#define X9_VOICE_SPH_SIZE        128u
#define X9_VOICE_SPH_MSG_ID      0x2f20u
#define X9_VOICE_SPH_TYPE        25u
#define X9_VOICE_SPH_WRAP_SIZE   32u

struct x9_voice_sph_inputs {
 uint8_t app;         /* +0x00 */
 uint8_t bt;          /* +0x01 */
 uint8_t rate_enum;   /* +0x02 */
 uint8_t enh_config;  /* +0x03: raw getter value, pre-gating */
 uint8_t hwif;        /* +0x55: raw getter value, pre-gating */
 uint8_t branch5;     /* +0x05: written with the SmartPA config */
 uint8_t smartpa;     /* +0x54: 1 + isDualSmartPA() in that branch */
 uint8_t com_par0;    /* +0x1c: common block [0] */
 uint8_t debug0;      /* +0x34: debug block [0] */
 int adsp_active;     /* ADSP option on, or SCP feature bit set */
};

/* configSpeechInfo's gating rule for the two dynamic fields: the
 * getters are only consulted when the ADSP path is active. */
static inline void x9_voice_sph_resolve(const struct x9_voice_sph_inputs *in,
                                        uint8_t *enh_config, uint8_t *hwif){
 *enh_config = in->adsp_active ? in->enh_config : 0;
 *hwif = in->adsp_active ? in->hwif : 0;
}

/* Build the 128-byte sph_info body exactly as configSpeechInfo does
 * for the given inputs: zero first, then the pinned stores. */
static inline void x9_voice_sph_build(const struct x9_voice_sph_inputs *in,
                                      uint8_t body[X9_VOICE_SPH_SIZE]){
 uint8_t cfg, hwif;
 x9_voice_sph_resolve(in, &cfg, &hwif);
 memset(body, 0, X9_VOICE_SPH_SIZE);
 body[0x00] = in->app;
 body[0x01] = in->bt;
 body[0x02] = in->rate_enum;
 body[0x03] = cfg;
 body[0x05] = in->branch5;
 body[0x1c] = in->com_par0;
 body[0x34] = in->debug0;
 body[0x54] = in->smartpa;
 body[0x55] = hwif;
}

/* The SpeechMessageQueue wrapper (configPayload): a 32-byte sph_msg
 * header referencing the body by pointer. Only the fields the
 * archived disassembly pins are set: byte 0 = 1, u16 msg id at +2,
 * u16 type at +0x0c, u16 size at +0x0e, u64 body pointer at +0x10;
 * the rest is zero here and its full layout is not closed. */
static inline void x9_voice_sph_wrap(uint8_t wrap[X9_VOICE_SPH_WRAP_SIZE],
                                     const uint8_t *body){
 memset(wrap, 0, X9_VOICE_SPH_WRAP_SIZE);
 wrap[0] = 1;
 wrap[2] = (uint8_t)(X9_VOICE_SPH_MSG_ID & 0xff);
 wrap[3] = (uint8_t)(X9_VOICE_SPH_MSG_ID >> 8);
 wrap[0x0c] = (uint8_t)(X9_VOICE_SPH_TYPE & 0xff);
 wrap[0x0d] = (uint8_t)(X9_VOICE_SPH_TYPE >> 8);
 wrap[0x0e] = (uint8_t)(X9_VOICE_SPH_SIZE & 0xff);
 wrap[0x0f] = (uint8_t)(X9_VOICE_SPH_SIZE >> 8);
 uint64_t ptr = (uint64_t)(uintptr_t)body;
 for (unsigned i = 0; i < 8; i++) wrap[0x10 + i] = (uint8_t)(ptr >> (8 * i));
}

/* Mirror of the MD SPC handler's consumption of the two dynamic
 * fields (md-2f20-body-and-af20-consumer-islands.asm via the contract
 * JSON): out = {state+0x54, state+0x55, state+0x56} after the body is
 * saved. hwif != 0: +0x54 = 2, +0x56 = ((body+3 >> 3) & 3) + 1.
 * hwif == 0: +0x54 = (body+3 << 1) & 2, +0x56 = 0. */
static inline void x9_voice_sph_md_state(const uint8_t body[X9_VOICE_SPH_SIZE],
                                         uint8_t state[3]){
 state[1] = body[0x55];
 if (body[0x55]) {
  state[0] = 2;
  state[2] = (uint8_t)(((body[0x03] >> 3) & 3) + 1);
 } else {
  state[0] = (uint8_t)((body[0x03] << 1) & 2);
  state[2] = 0;
 }
}

/* This phone's factory MO call inputs (mo-call-audio.txt:790):
 * app 0, bt 0, rate enum 3, opendsp 3, smartPA config 2 with its
 * branch byte, com par[0] 6, debug info[0] 3, hwif 0 (live DT).
 * Recorded as an observed input set, not a runtime default. */
static const struct x9_voice_sph_inputs X9_VOICE_SPH_PLJ110_MO = {
 0, 0, 3, 3, 0, 2, 2, 6, 3, 1
};

#endif
