#ifndef X9_BT_HARDWARE_H
#define X9_BT_HARDWARE_H
#include "x9_bt_session.h"
void x9_bt_hardware_ops(struct x9_bt_session_ops *);
/* Worker owns TX, IRQ owns RX. No function programs CMM's baud or SSPM PLL. */
int x9_bt_hardware_map(void);
int x9_bt_hardware_uart(volatile uint32_t **);
int x9_bt_hardware_check(void);
/* WMT flow byte, not Android UART_FC enum. Called with RX/IRQ lock held. */
int x9_bt_hardware_config_uart(uint32_t baud, uint8_t flow);
/* Explicit closed/quiesced-transport lifecycle. sleep follows a successful
 * FW-own ACK+physical drain+masked UART IRQ; never releases the PMIC lease.
 * wake uses APB requests first, proves resources before endpoint/AP MMIO and
 * sets host-awake. Caller then restores its IRQ, optionally raw-FF/drains,
 * polls awake, sends driver-own and finally calls ownership_finish. */
int x9_bt_hardware_sleep(void);
int x9_bt_hardware_wake(void);
int x9_bt_hardware_awake(bool *awake, bool *need_ff);
int x9_bt_hardware_ownership_finish(void);
int x9_bt_hardware_stop(void);
#endif
