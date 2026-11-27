/* Baseband Voice provider contract, pure half (L6/L1; see
 * docs/call-voice-audio-chain-20261127.md).
 *
 * iOS side (VirtualAudio from the fixed 20H392 root, static evidence
 * archived in x9/out/voice-provider-analysis/ and
 * x9/out/call-audio-analysis/): the VirtualAudio factory compares
 * the device UID "Baseband Voice"; on a match it creates a 592-byte
 * wrapper device holding "Baseband Downlink" / "Baseband Uplink"
 * streams. The wrapper is not synthesized from the UID: its base
 * constructor stores a REAL incoming AudioObjectID at device+0xb8,
 * and stream enumeration (vtable slot 88 -> 20a4c4) queries that
 * object's `stm#` property in the inpt and outp scopes with
 * AudioObjectGetPropertyDataSize / GetPropertyData, wrapping the
 * real stream IDs it gets back (20a29c -> 231740). The wrapper also
 * consumes `ttye` (4 bytes, -> CTSetTTYMode) and `ctsr` (8-byte
 * double, -> CTAudioSampleRateSwitchDidComplete). The module-level
 * AudioDeviceCreateIOProcID helper is proven, but the caller chain
 * from the Baseband Voice entry to a real hardware IOProc is NOT
 * closed, and neither is the ingress that hands the factory its
 * incoming object ID.
 *
 * Android side (this phone's factory MO route, mo-call-audio.txt):
 * the modem PCM path is modemPcmOpen + two Hostless_Speech opens
 * (downlink owner flags 0x10000000, uplink owner flags 0,
 * prepare=false) with mMdConfig {channels 2, rate 48000,
 * period_size 1024, period_count 2, format 0} and
 * SetModemSideSamplingRate 48000 => 48000; modem index 0. The AFE
 * trigger decision for the share-mem route is the DSP's
 * (use_adsp_share_mem makes the host skip memif/IRQ start/stop), so
 * no host-side provider may claim the trigger.
 *
 * This header is the contract skeleton that follows from those two
 * sides: a provider may only be presented to the factory when it is
 * a real (non-media) device object exposing one input and one output
 * stream at the modem PCM format, with a real IOProc behind it. No
 * provider in this tree qualifies today -- there is no PCM supplier
 * -- so the registry stays empty and the ingress stays fail-closed.
 * Advertising the UID or reusing the media Speaker device would not
 * satisfy the factory's real-stream enumeration and is refused by
 * construction here. */
#ifndef X9_VOICE_BBDEV_H
#define X9_VOICE_BBDEV_H

#include <stdint.h>
#include <string.h>

/* Factory modem PCM configuration (mo-call-audio.txt:633, this
 * phone's captured mMdConfig; format 0 is tinyalsa S16_LE). */
#define X9_VOICE_PCM_CHANNELS     2u
#define X9_VOICE_PCM_RATE         48000u
#define X9_VOICE_PCM_PERIOD_FRAMES 1024u
#define X9_VOICE_PCM_PERIOD_COUNT 2u
#define X9_VOICE_PCM_FORMAT_S16   0u
#define X9_VOICE_PCM_FRAME_BYTES  (X9_VOICE_PCM_CHANNELS * 2u) /* S16 */
#define X9_VOICE_PCM_PERIOD_BYTES \
 (X9_VOICE_PCM_PERIOD_FRAMES * X9_VOICE_PCM_FRAME_BYTES)
#define X9_VOICE_PCM_BUFFER_BYTES \
 (X9_VOICE_PCM_PERIOD_BYTES * X9_VOICE_PCM_PERIOD_COUNT)

struct x9_voice_bbdev_desc {
 int has_input_stream;   /* answers stm# in the inpt scope */
 int has_output_stream;  /* answers stm# in the outp scope */
 int has_ioproc;         /* a real IOProc path, not a stub */
 int is_media_device;    /* media playback owner (e.g. Speaker) */
 uint32_t channels;
 uint32_t rate;
 uint32_t format;        /* tinyalsa format; 0 = S16_LE */
};

/* A provider qualifies only when every factory-visible requirement
 * is met at the factory modem PCM format. Anything less would make
 * the wrapper enumerate phantom streams or stall its IOProc. */
static inline int x9_voice_bbdev_qualify(const struct x9_voice_bbdev_desc *d){
 if (!d) return 0;
 if (!d->has_input_stream || !d->has_output_stream) return 0;
 if (!d->has_ioproc) return 0;
 if (d->is_media_device) return 0;
 if (d->channels != X9_VOICE_PCM_CHANNELS) return 0;
 if (d->rate != X9_VOICE_PCM_RATE) return 0;
 if (d->format != X9_VOICE_PCM_FORMAT_S16) return 0;
 return 1;
}

struct x9_voice_bbdev_registry {
 int present;
 struct x9_voice_bbdev_desc desc;
};

static inline void x9_voice_bbdev_registry_init(
  struct x9_voice_bbdev_registry *r){
 memset(r, 0, sizeof(*r));
}

/* Register the (single) Baseband Voice provider. Unqualified
 * descriptors are refused (-2), as is a second registration (-1):
 * the factory wraps exactly one incoming object. Reserved for the
 * future PCM supplier; nothing in this tree calls it yet. */
static inline int x9_voice_bbdev_register(
  struct x9_voice_bbdev_registry *r, const struct x9_voice_bbdev_desc *d){
 if (!r || !d) return -1;
 if (r->present) return -1;
 if (!x9_voice_bbdev_qualify(d)) return -2;
 r->desc = *d;
 r->present = 1;
 return 0;
}

static inline void x9_voice_bbdev_unregister(
  struct x9_voice_bbdev_registry *r){
 if (r) r->present = 0;
}

/* The factory ingress decision: an incoming object ID may only be
 * handed to the wrapper when a qualified provider is registered. */
static inline int x9_voice_bbdev_ingress_open(
  const struct x9_voice_bbdev_registry *r){
 return r && r->present;
}

#endif
