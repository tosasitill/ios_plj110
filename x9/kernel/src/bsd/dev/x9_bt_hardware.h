#ifndef X9_BT_HARDWARE_H
#define X9_BT_HARDWARE_H
#include "x9_bt_session.h"
void x9_bt_hardware_ops(struct x9_bt_session_ops *);
/* Worker owns TX, IRQ owns RX. No function programs CMM's baud or SSPM PLL. */
int x9_bt_hardware_map(void);
int x9_bt_hardware_uart(volatile uint32_t **);
int x9_bt_hardware_check(void);
int x9_bt_hardware_stop(void);
#endif
