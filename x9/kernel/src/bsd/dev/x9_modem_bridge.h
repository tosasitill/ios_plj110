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
 * no partial/synthetic CNF is returned and other MIPC services stay online.
 * -3 consumes a guarded dial rejected locally before physical TX, not a CNF. */
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
/* Staged call transport, separate from SMS/IMS and WDS. No automatic CMD
 * registration/approval. event returns raw MD masks (NOT AP SIM slots).
 * CMD8901 token owns its original id/txid/mask through physical RSP8902 TX.
 * reply requires an explicit, externally verified approval/cause policy;
 * success is permission to continue incoming handling, NOT ANSWER0907.
 * event/reply/take return conventions match messaging above. All command
 * identity tombstones remain until boot, even after success. No retries;
 * only eight retained commands until Android retirement rules are proven. */
int x9_md_mipc_call_event(uint8_t *frame, uint32_t capacity, uint32_t *length, uint32_t *token);
/* Native MO observation, copied under the MD lock. No registration, request,
 * number disclosure or fabricated idle baseline. GET_STATUS count is valid
 * only after a complete owned090a; ECPI state requires full4901 frames.
 * Returns1 copied, -1 unavailable; zero-initialized state means UNKNOWN. */
struct x9cw_outbound_state;
int x9_md_mipc_call_snapshot(struct x9cw_outbound_state *state);
/* Independent MT observation: complete incoming CMD identity plus complete
 * ECPI0/132/6/133. Never used to complete DIAL or to infer single-call count.
 * This API observes only; no approval/answer/registration is started. */
struct x9cw_inbound_state;
int x9_md_mipc_call_incoming_snapshot(struct x9cw_inbound_state *state);
/* Atomic outbound admission against a previously copied native baseline.
 * Same unique writer/token allocator, but checks the epoch/revision/count
 * again under the MD lock and at writer selection. A guarded staging lease
 * survives CNF/client loss and blocks other CALL mutations except the
 * guarded end path. Retirement requires END + owned empty + drained slots;
 * this still does not supply a production DIAL profile or admission caller. */
uint64_t x9_md_mipc_call_dial_submit(const uint8_t *frame, uint32_t length,
                                   const struct x9cw_outbound_state *baseline);
/* Owned complete current snapshot + matching real MO epoch/ID. Only SIM2
 * mode20 single ordinary call. Returns1 only with a valid target; no query,
 * approval or mutation is started by the getter. End submit rechecks under
 * MD lock and before TX. Uncertain sent owners persist across host timeout. */
struct x9cw_end_target;
int x9_md_mipc_call_end_snapshot(struct x9cw_end_target *target);
uint64_t x9_md_mipc_call_end_submit(const uint8_t *frame, uint32_t length,
                                  const struct x9cw_end_target *target);
void x9_md_mipc_call_end_abandon(uint64_t token);
int x9_md_mipc_call_reply(uint32_t token, unsigned approve, unsigned include_cause, uint32_t cause);
int x9_md_mipc_call_reply_take(uint32_t token, int *result);
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
