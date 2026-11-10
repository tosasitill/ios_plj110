/* Own-code factory Re25/SpeakerBoost Q16 codec; no calibration authority. */
#ifndef X9_TFA_RE25_H
#define X9_TFA_RE25_H
#include <stdint.h>
#include "x9_tfa_multimsg.h"
#define X9_TFA_RE25_VERSION 1u
#define X9_TFA_RE25_SCALE 65536u
enum { X9TFARe25Command=1u, X9TFARe25Batch=2u };
struct x9_tfa_re25_limits {
    uint32_t minimum_mohm[2],maximum_mohm[2];
}; /* Explicit independent bounds from the actual board/profile authority. */
struct x9_tfa_re25_pair {
    uint32_t version,packing,q16[2],mohm[2];
}; /* 24B; observed numbers only, NOT calibration-done or channel authority. */
#ifdef __cplusplus
extern "C" {
#endif
/* Family-2 soft-DSP SetRe25C: selector/0x81/0x05, TWO independent Q16
 * coefficients. Explicit command (9/12B) or canonical batch (18/24B).
 * This is byte framing, not inferred cold/warm firmware state. Selector and
 * LEFT/RIGHT -> DSP channel order must already be proved by the supplier.
 * Both inputs and both explicit bounds must be positive, ordered, <=65534.
 * Zero/erased MTP, mono duplication, default 7399 and calibration triggers
 * are NEVER synthesized. This routine does not send, authenticate, set MTP,
 * validate PA/firmware state or grant playback/unmute. Whole-call immutable
 * pinned inputs; outputs may not alias either input or each other. Failure
 * leaves dest/size unchanged. Batch can enter the native SET's real policy. */
int x9_tfa_re25_encode(const uint32_t mohms[2],
    const struct x9_tfa_re25_limits *,uint32_t packing,uint32_t form,
    uint8_t selector,void *dest,uint32_t capacity,uint32_t *written_bytes);
/* Exact TWO raw GetRe25C reply words, not a header, status or batch.
 * Canonical signed24 BE24/LE32, positive only. Check independent bounds in
 * the same quantized Q16 domain as encode. mohms is floor(q16*1000/65536),
 * so a boundary value can print one mOhm below its unquantized source;
 * compare authoritative calibration using q16, NOT rounded mohms alone.
 * 64-bit scaling avoids unsigned32 overflow on otherwise canonical words.
 * Response identity/actual GET completion/route and calibration-done must
 * be independently proved by the production exchange supplier. This pure
 * codec also compiles for iOS; numbers/limits supplied to it are not trust.
 * Input and limits immutable/pinned; output disjoint, unchanged on error. */
int x9_tfa_re25_decode(const void *,uint32_t bytes,uint32_t packing,
    const struct x9_tfa_re25_limits *,struct x9_tfa_re25_pair *);
#ifdef __cplusplus
}
#endif
#endif
