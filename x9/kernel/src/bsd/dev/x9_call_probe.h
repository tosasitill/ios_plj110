/* X9 call-audio probe: pure decision/statistics logic (2026-11-28).
 *
 * Purpose: when a call produces no sound, split the cause into layers
 * without guessing (call-voice-audio-chain-20261127.md, "下轮真机判别清单"):
 *   L3  did the MD send anything on CCCI_PCM_RX (ch 4, Android /dev/ccci_aud,
 *       the SpeechOn 2f20/af20 carrier) during the call?  Any other CCCI
 *       channel whose traffic only moves while a call is up?
 *   L5  did the USIP speech EMI (SMEM id 24) change during the call, per
 *       subrange (PARAM / ADSP_PHONECALL / ADSP_SMARTPA)?
 *   L6  was the AP media AFE (speaker path) running at the same time?
 * Everything is observation: no CCCI TX, no EMI write, no AFE/ADSP MMIO.
 *
 * Driven by the CallList46 publications (iOS states 1 dialing, 2 incoming,
 * 3 active, 5 alerting, 9 ended). Kernel glue: x9_modem.inc
 * x9_md_call_probe(); gate x9_call_probe (0 off, 1 PARAM, 2 all EMI); the
 * probe binds the USIP owner itself, x9_voice_usip is not needed.
 */
#ifndef X9_CALL_PROBE_H
#define X9_CALL_PROBE_H
#include <stdint.h>
#include <stddef.h>

#define X9CP_CH 512u           /* CCCI logical channels counted (ids seen up to 216) */
#define X9CP_CCCI_PCM_RX 4u    /* eccci ccci_core.h: CCCI_PCM_RX */
#define X9CP_AUD_LOG_MAX 32u   /* ch4 packets printed per call */

enum x9cp_action { X9CP_NONE = 0, X9CP_START = 1, X9CP_SNAP = 2, X9CP_END = 3 };

struct x9cp_region { uint32_t nonzero, signature, first_nonzero; };

struct x9_call_probe {
 unsigned active, sim, id, state, calls;
 uint32_t base[X9CP_CH];       /* channel counters at the previous snapshot */
 uint32_t aud_logged;          /* ch4 packets printed this call */
 struct x9cp_region emi[3], emi_start[3];
 unsigned emi_valid;
};

/* One CallList46 observation -> what the glue should do. Duplicates (the
 * same call/state republished to several clients) are NONE. A state for a
 * different call while one is active ends the old one first (END), the
 * caller then feeds the observation again. */
static inline enum x9cp_action x9cp_observe(struct x9_call_probe *p, unsigned sim,
                                            unsigned id, unsigned state)
{
 if (state != 1 && state != 2 && state != 3 && state != 5 && state != 9) return X9CP_NONE;
 if (!p->active) {
  if (state == 9) return X9CP_NONE;            /* end of a call never seen */
  p->active = 1; p->sim = sim; p->id = id; p->state = state; p->calls++;
  p->aud_logged = 0; p->emi_valid = 0;
  return X9CP_START;
 }
 if (p->sim != sim || p->id != id) { p->active = 0; return X9CP_END; }
 if (p->state == state) return X9CP_NONE;
 p->state = state;
 if (state == 9) { p->active = 0; return X9CP_END; }
 return X9CP_SNAP;
}

/* Word statistics over an uncached/WC window. FNV-1a over (index,value) of
 * non-zero words only, so a region that is all zero has signature 0. */
static inline void x9cp_region_stat(const volatile uint32_t *w, size_t words,
                                    struct x9cp_region *r)
{
 uint32_t h = 0, nz = 0, first = 0xffffffffu;
 for (size_t i = 0; i < words; i++) {
  uint32_t v = w[i];
  if (!v) continue;
  if (!nz) { first = (uint32_t)i; h = 2166136261u; }
  nz++;
  uint32_t k[2] = { (uint32_t)i, v };
  for (unsigned j = 0; j < 2; j++)
   for (unsigned b = 0; b < 4; b++) { h ^= (k[j] >> (b * 8)) & 0xff; h *= 16777619u; }
 }
 r->nonzero = nz; r->signature = h; r->first_nonzero = first;
}

/* Channel deltas since the previous snapshot; rebases. Returns how many
 * (channel, delta) pairs were written (only non-zero deltas). */
static inline unsigned x9cp_channel_deltas(struct x9_call_probe *p, const uint32_t *now,
                                           uint32_t (*out)[2], unsigned cap)
{
 unsigned n = 0;
 for (unsigned c = 0; c < X9CP_CH; c++) {
  uint32_t d = now[c] - p->base[c];
  p->base[c] = now[c];
  if (d && n < cap) { out[n][0] = c; out[n][1] = d; n++; }
 }
 return n;
}

/* ch4 packets are printed only while a call is up, at most
 * X9CP_AUD_LOG_MAX per call; outside a call they are only counted. */
static inline int x9cp_log_aud(struct x9_call_probe *p)
{
 if (!p->active || p->aud_logged >= X9CP_AUD_LOG_MAX) return 0;
 p->aud_logged++;
 return 1;
}

#endif
