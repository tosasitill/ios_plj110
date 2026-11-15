/* Android MT6653 pre-open/download/post-open protocol. Hardware is explicit. */
#ifndef X9_BT_SESSION_H
#define X9_BT_SESSION_H
#include "x9_bt_download.h"
#include <stdbool.h>
enum x9_bt_session_stage {
    X9_BT_SESSION_INPUT, X9_BT_SESSION_PRE_ON, X9_BT_SESSION_POWER,
    X9_BT_SESSION_32K, X9_BT_SESSION_ROM, X9_BT_SESSION_BAUD,
    X9_BT_SESSION_XO, X9_BT_SESSION_DOWNLOAD, X9_BT_SESSION_FEATURES,
    X9_BT_SESSION_ROUTING, X9_BT_SESSION_OWN, X9_BT_SESSION_FUNCTION,
    X9_BT_SESSION_CONTROLLER_CONFIG, X9_BT_SESSION_DONE
};
struct x9_bt_session_ops {
    struct x9_bt_download_ops io;
    /* Ordinary HCI completions use a 16-bit HCI opcode, NOT the one-byte
     * WMT opcode in event E4. This transport has the same sole worker owner. */
    int (*hci_exchange)(void *, const uint8_t *, size_t, uint8_t *, size_t *, uint32_t);
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
    bool driver_owned, function_powered;
    uint32_t vendor_completed, vendor_constructor;
    uint16_t vendor_opcode;
    uint8_t vendor_status; /* 0xff until a valid controller status exists. */
    struct x9_bt_download_result download;
};
int x9_bt_session_run(const struct x9_bt_session_ops *,
                     const struct x9_bt_session_board *, struct x9_bt_session_result *);
int x9_bt_session_connfem(const struct x9_bt_download_ops *);
/* One actual selected Android vendor command, with its original H4 bytes.
 * Does not select/default a configuration, retry TX, or fabricate replies.
 * status=0xff means no valid status was received. Success requires Complete,
 * never merely Command Status (accepted). No payload is logged. */
int x9_bt_session_vendor_command(const struct x9_bt_session_ops *,
                                 const uint8_t *, size_t, uint8_t *status);
#endif
