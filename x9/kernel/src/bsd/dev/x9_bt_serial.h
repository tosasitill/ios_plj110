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
uint32_t x9_bt_serial_rx(const uint8_t *, uint32_t);
/* Board must commit only actually transmitted bytes using the peek token;
 * a close/reopen or failed initializer invalidates that token. */
uint32_t x9_bt_serial_tx_peek(uint8_t *, uint32_t, uint32_t *generation);
void x9_bt_serial_tx_commit(uint32_t, uint32_t generation);
bool x9_bt_serial_active(void);
#ifdef __cplusplus
}
#endif
#endif
