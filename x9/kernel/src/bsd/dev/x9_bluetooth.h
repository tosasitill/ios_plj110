/* X9 MT6653 bootstrap transport ABI. This is NOT the AppleBT user-client ABI. */
#ifndef X9_BLUETOOTH_H
#define X9_BLUETOOTH_H
#include <stdint.h>
#define X9_BT_ABI_VERSION 1u
#define X9_BT_FRAME_MAX 4096u
#define X9_BT_SECTION_MAX 32u
enum x9_bt_state {
    X9_BT_DISABLED, X9_BT_PASSIVE, X9_BT_WAIT_POWER, X9_BT_ROM_QUERY,
    X9_BT_ROM_READY, X9_BT_FAILED
};
enum x9_bt_selector {
    X9_BT_GET_STATUS, X9_BT_GET_SECTION, X9_BT_SEND_H4, X9_BT_RECV_H4
};
struct x9_bt_status {
    uint32_t version, state, error, chip_id;
    uint32_t baud, firmware_bytes, firmware_sections, firmware_ready;
    uint64_t tx_bytes, rx_bytes, rx_frames, rx_dropped;
    uint64_t bad_frames, uart_errors, timeouts, tx_frames;
};
struct x9_bt_section {
    uint32_t type, offset, bytes, address, download_bytes, bin_type, mode;
};
#ifdef __cplusplus
extern "C" {
#endif
void x9_bluetooth_start(void);
void x9_bluetooth_status(struct x9_bt_status *status);
int x9_bluetooth_section(uint32_t index, struct x9_bt_section *section);
int x9_bluetooth_send(const void *packet, uint32_t length);
int x9_bluetooth_receive(void *packet, uint32_t *length);
#ifdef __cplusplus
}
#endif
#endif
