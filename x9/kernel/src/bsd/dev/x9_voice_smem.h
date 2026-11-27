/* Call-voice EMI descriptor observation (first slice; see
 * docs/call-voice-audio-chain-20261127.md).
 *
 * On this device the AP<->MD speech path runs through the cacheable SMEM
 * region the LK tables carry as id 24 (USIP): the Android LK log of this
 * phone records it as AP=f0d00000 MD=42d00000 (MD view) size=0x60000
 * flags=2, and the production clear loop in x9_modem.inc already treats
 * id 24 as NCLR_FIRST / never cleared. Nothing in the tree maps, clears
 * or grants that region for voice today.
 *
 * This header is the pure, host-testable half of an observation hook:
 * it classifies the runtime descriptor that x9md_smem_init() assembled
 * from the LK tables. The Android values below are comparison constants
 * from that one log line, never runtime defaults: a mismatch is a
 * finding, not an error, and the evaluator changes no state. */
#ifndef X9_VOICE_SMEM_H
#define X9_VOICE_SMEM_H

#include <stdint.h>

#define X9_VOICE_SMEM_ID 24u          /* SMEM_USER_USIP on this LK */
#define X9_VOICE_SMEM_ANDROID_MD 0x42d00000u
#define X9_VOICE_SMEM_ANDROID_SIZE 0x60000u
#define X9_VOICE_SMEM_ANDROID_FLAGS 2u

struct x9_voice_smem_desc {
 uint32_t md_view;   /* MD-view base (md_offset + 0x40000000) */
 uint32_t size;
 uint32_t flags;     /* raw LK entry flags, semantics not decoded here */
 uint32_t ap_off;    /* nc regions only; 0xffffffff for cacheable ones */
 int in_lk;          /* descriptor came from this boot's LK table */
 int exists;         /* region is in the emulated SMEM table */
 int nclr_first;     /* SMF_NCLR_FIRST: first boot must not clear it */
};

struct x9_voice_smem_report {
 int present;        /* exists && in_lk && size != 0 */
 int android_match;  /* present && md/size/flags equal the Android log */
 uint32_t md_view;
 uint32_t size;
 uint32_t flags;
};

static inline void x9_voice_smem_evaluate(const struct x9_voice_smem_desc *d,
                                          struct x9_voice_smem_report *r){
 r->md_view=d->md_view;r->size=d->size;r->flags=d->flags;
 r->present=d->exists&&d->in_lk&&d->size!=0;
 r->android_match=r->present&&d->md_view==X9_VOICE_SMEM_ANDROID_MD&&
  d->size==X9_VOICE_SMEM_ANDROID_SIZE&&d->flags==X9_VOICE_SMEM_ANDROID_FLAGS;
}

#endif
