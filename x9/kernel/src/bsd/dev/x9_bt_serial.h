/* Native iOS H4 byte-stream frontend. UART/power remain board-worker owned. */
#ifndef X9_BT_SERIAL_H
#define X9_BT_SERIAL_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int x9_bt_serial_init(void);
/* Only the complete board initializer may report error=0. ROM_READY is NOT
 * enough. A failure clears both FIFOs and wakes all blocked clients. */
void x9_bt_serial_result(int error, uint32_t hardware_baud);
/* Whole-frame admission: zero means retain/retry, never a partial enqueue.
 * If no daemon is open, returns length to discard pre-open/stale traffic. */
uint32_t x9_bt_serial_rx(const uint8_t *, uint32_t);
/* Claim a COMPLETE native H4 frame, retaining it in the queue until the
 * board's physical drain. A close cannot reopen while a claim is on wire.
 * Finish publishes errors atomically with queue progress/waiter wakeup. */
int x9_bt_serial_tx_claim(uint8_t *, uint32_t capacity, uint32_t *length,
                         uint32_t *generation);
void x9_bt_serial_tx_finish(uint32_t sent, uint32_t generation, int error);
bool x9_bt_serial_active(void);
#ifdef __cplusplus
}
#endif
#endif
