/* Android MT6653 pre-open/download/post-open protocol. Hardware is explicit. */
#ifndef X9_BT_SESSION_H
#define X9_BT_SESSION_H
#include "x9_bt_download.h"
#include <stdbool.h>
enum x9_bt_session_stage {
    X9_BT_SESSION_INPUT, X9_BT_SESSION_PRE_ON, X9_BT_SESSION_POWER,
    X9_BT_SESSION_32K, X9_BT_SESSION_ROM, X9_BT_SESSION_BAUD,
    X9_BT_SESSION_XO, X9_BT_SESSION_DOWNLOAD, X9_BT_SESSION_FEATURES,
    X9_BT_SESSION_ROUTING, X9_BT_SESSION_OWN, X9_BT_SESSION_DONE
};
struct x9_bt_session_ops {
    struct x9_bt_download_ops io;
    /* pre_on: real SSPM default-config + RX/TX request + hub-ready + BT
     * pinctrl; power_on: serialized connv3 first/second-radio operation.
     * No callback may return success merely because a PMIC GPIO is high. */
    int (*pre_on)(void *);
    int (*power_on)(void *);
    int (*rx_and_32k)(void *);
    int (*host_uart)(void *, uint32_t baud, uint8_t flow_control);
    /* Read actual hub sleep mechanism/inband support, never compile flags. */
    int (*hub_features)(void *, bool *hardware_sleep, bool *inband);
    int (*post_download)(void *, bool inband);
    /* Verify driver ownership/host-awake feedback after the FW ACK. */
    int (*verify_ownership)(void *);
};
struct x9_bt_session_board {
    uint8_t xo[25]; /* CONNXO record bytes 2..26, without its LE length. */
    uint32_t platform_config;
    uint8_t flow_control; /* 0x00 none, 0x40 HW, 0x80 MTK SW. */
    bool calibration_present, wakeup_callback;
};
struct x9_bt_session_result {
    enum x9_bt_session_stage stage;
    uint32_t chip_id, baud, max_baud;
    uint8_t firmware_flags, ap_flags;
    bool driver_owned;
    struct x9_bt_download_result download;
};
int x9_bt_session_run(const struct x9_bt_session_ops *,
                     const struct x9_bt_session_board *, struct x9_bt_session_result *);
int x9_bt_session_connfem(const struct x9_bt_download_ops *);
#endif
