/* MT6993 connv3 common power, shared by the Wi-Fi and BT workers. */
#ifndef X9_CONN_POWER_H
#define X9_CONN_POWER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum x9_conn_radio { X9_CONN_WIFI = 1, X9_CONN_BT = 2 };
/* One lease per worker, not a GPIO-level inference of another radio's state.
 * Last release is permitted only after that worker has stopped its hardware. */
int x9_conn_power_acquire(enum x9_conn_radio);
int x9_conn_power_done(enum x9_conn_radio);
int x9_conn_power_release(enum x9_conn_radio);
uint32_t x9_conn_power_radios(void);
enum x9_conn_bt_pin_state { X9_CONN_BT_PRE, X9_CONN_BT_TX,
    X9_CONN_BT_RX, X9_CONN_BT_RESET_OFF };
int x9_conn_bt_pins(enum x9_conn_bt_pin_state);
#ifdef __cplusplus
}
#endif
#endif
