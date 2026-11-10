/* Own-code NXP multimessage framing/word conversion, shared with iOS.
 * No vendor coefficients, calibration, route or hardware authorization. */
#ifndef X9_TFA_MULTIMSG_H
#define X9_TFA_MULTIMSG_H
#include <stdint.h>
#define X9_TFA_MULTIMSG_VERSION 1u
#define X9_TFA_MULTIMSG_COMMAND 0x008015u
#define X9_TFA_MULTIMSG_BE24_MAX 4000u
#define X9_TFA_MULTIMSG_LE32_MAX 5336u
enum { X9TFABE24=1u, X9TFALE32=2u };
struct x9_tfa_multimsg_info {
    uint32_t version, packing, bytes, messages;
    uint32_t payload_words, max_message_words, be24_bytes, le32_bytes;
}; /* 32B; includes header, length fields and final zero in byte totals */
struct x9_tfa_multimsg_message {
    uint32_t offset, bytes, words, command;
}; /* 16B; offset in input; command is unsigned low 24 bits, not an allowlist */
#ifdef __cplusplus
extern "C" {
#endif
/* Exact 0x008015 header, positive word-count records, final zero at EOF.
 * Packing is explicit, never inferred. Empty batches are rejected. LE32
 * payload words must have canonical signed-24 extension; count upper bits
 * must be zero. Whole input is scanned before any output changes.
 * Callers keep input immutable for the entire call, and pin its memory.
 * Output structs must not overlap input. No allocation, I/O or execution. */
int x9_tfa_multimsg_scan(const void *,uint32_t bytes,uint32_t packing,
                        struct x9_tfa_multimsg_info *);
/* Zero-based index, full scan first; ENOENT leaves output unchanged.
 * This byte view conveys no trust in any command or coefficient. */
int x9_tfa_multimsg_get(const void *,uint32_t bytes,uint32_t packing,
                       uint32_t index,struct x9_tfa_multimsg_message *);
/* Convert complete framing and all signed24 payload words, not PCM.
 * No in-place/overlapping conversion (including written_bytes). Source and
 * target format bounds both apply. Destination and written_bytes remain
 * unchanged on invalid input, insufficient capacity or alias rejection.
 * Same-format copying still validates the whole batch; no double wrapping.
 * Does NOT convert PM containers or grant Probus/calibration/unmute/Ready. */
int x9_tfa_multimsg_convert(const void *,uint32_t bytes,uint32_t packing,
                           uint32_t target,void *,uint32_t capacity,
                           uint32_t *written_bytes);
#ifdef __cplusplus
}
#endif
#endif
