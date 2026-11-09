/* X9 single-owner CCCI AT transport. No caller writes CCIF directly. */
#ifndef X9_MODEM_BRIDGE_H
#define X9_MODEM_BRIDGE_H
#include <stdint.h>
#define X9_AT_COMMAND_MAX 256u
#define X9_AT_RESPONSE_MAX 2048u
enum x9_at_result { X9_AT_OK = 0, X9_AT_MODEM_ERROR = 1, X9_AT_TIMEOUT = 2,
    X9_AT_OFFLINE = 3, X9_AT_OVERFLOW = 4, X9_AT_CANCELLED = 5 };
#ifdef __cplusplus
extern "C" {
#endif
/* Nonblocking. token 0 means offline/full/invalid. A token must be taken or cancelled. */
uint64_t x9_md_at_submit(const char *command, uint32_t timeout_ms);
int x9_md_at_take(uint64_t token, char *response, uint32_t capacity, int *result);
void x9_md_at_cancel(uint64_t token);
int x9_md_at_online(void);
/* Native MIPC over ttyCMIPC1 (observed Android RIL fd), CCCI RX216/TX217.
 * Frames include the 16-byte header. submit assigns a nonreused wire txid.
 * No sent-request cancellation: a mutating caller MUST retain its token after
 * timeout/client deletion and consume late CNF before releasing ownership. */
#define X9_MIPC_FRAME_MAX 4096u
uint64_t x9_md_mipc_submit(const uint8_t *frame, uint32_t length);
int x9_md_mipc_take(uint64_t token, uint8_t *frame, uint32_t capacity, uint32_t *length);
int x9_md_mipc_event(uint8_t *frame, uint32_t capacity, uint32_t *length);
int x9_md_mipc_online(void);
#ifdef __cplusplus
}
#endif
#endif
