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
/* take: 1 complete frame, 0 pending, -1 unavailable/capacity, -2 an owned
 * oversized SMS0608 has been fully drained. -2 consumes that token once;
 * no partial/synthetic CNF is returned and other MIPC services stay online. */
int x9_md_mipc_take(uint64_t token, uint8_t *frame, uint32_t capacity, uint32_t *length);
int x9_md_mipc_event(uint8_t *frame, uint32_t capacity, uint32_t *length);
int x9_md_mipc_online(void);
/* SMS/IMS have a separate inbox: WDS must never consume their indications.
 * MD commands retain their original wire txid until a single reply is sent by
 * the MD owner. The nonreused local token is also the WMS ACK transaction ID.
 * event: 1 frame, 0 empty, -1 unavailable. INDs return token 0.
 * reply: 1 queued, 0 unavailable, -1 invalid/already replied.
 * reply_take: 1 physical TX completed (result 0 success), 0 pending, -1 invalid.
 * A failed TX result is returned once; the MD command remains quarantined
 * until boot, without ACK retry or recycling its original wire identity.
 * An ACK only confirms the command response transport, NOT SMS delivery. */
int x9_md_mipc_message_event(uint8_t *frame, uint32_t capacity, uint32_t *length, uint32_t *token);
int x9_md_mipc_message_reply(uint32_t token, unsigned success, uint8_t cause);
int x9_md_mipc_message_reply_take(uint32_t token, int *result);
/* x9_dual_sim=1: two fixed physical slots, indexed 0/1. Read-only AT via
 * MIPC 0003/0004 (8100 string); NEVER falls back to ttyC0 for SIM2.
 * Same nonreused wire token space as MIPC. Cancelled sent queries drain CNF. */
unsigned x9_md_sim_count(void);
/* MD DHL (CCB user 0) observation: read-only headers, consumes pages so the
 * MD ring never stalls. Called periodically from the QMI owner thread. */
void x9_md_log_poll(void);
/* First call starts the 10 s post-event save window of the MD DHL capture. */
void x9_md_log_mark(const char *what);
uint64_t x9_md_sim_at_submit(unsigned sim, const char *command);
int x9_md_sim_at_take(uint64_t token, char *response, uint32_t capacity, int *result);
void x9_md_sim_at_cancel(uint64_t token);
#ifdef __cplusplus
}
#endif
#endif
