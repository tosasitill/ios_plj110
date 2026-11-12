/* Own-code MTU3 device transport ABI. Not Apple's IOUSBDeviceFamily ABI. */
#ifndef X9_USB_DEVICE_H
#define X9_USB_DEVICE_H
#include <stdint.h>
#define X9_USB_ABI 1u
#define X9_USB_CHUNK 4096u
#define X9_USB_SERVICE "X9USBDevice"
enum { X9_USB_STATUS, X9_USB_SEND, X9_USB_RECEIVE, X9_USB_GET_DESCRIPTION,
       X9_USB_SEND_TRACKED, X9_USB_TX_RESULT };
enum { X9_USB_OFF, X9_USB_INITIALIZING, X9_USB_READY, X9_USB_CONFIGURED,
       X9_USB_FAULT };
/* ABI1's reserved word now reports physical bus-command state. */
#define X9_USB_BUS_FORCED_OFF 1u
#define X9_USB_BUS_CYCLING 2u
#define X9_USB_RAW_DRAINING 4u
enum { X9_USB_BUS_OFF, X9_USB_BUS_ON, X9_USB_BUS_CYCLE };
#define X9_USB_NATIVE_CYCLE_MS 50u
#define X9_USB_DESCRIPTION_ABI 1u
#define X9_USB_STRING_CAPACITY 64u
/* Separate ABI; the original 128-byte transport status remains unchanged.
 * ASCII subset of USB strings, not a device serial/UDID derived from storage. */
struct x9_usb_description {
    uint32_t version, vendor_id, product_id, device_id;
    uint32_t device_class, device_subclass, device_protocol, bcd_usb;
    char manufacturer[X9_USB_STRING_CAPACITY];
    char product[X9_USB_STRING_CAPACITY];
    char serial[X9_USB_STRING_CAPACITY];
};
struct x9_usb_description_status {
    uint32_t version, revision, mode, forced_off;
    struct x9_usb_description description;
};
#define X9_USB_TX_ABI 1u
enum { X9_USB_TX_QUEUED = 1, X9_USB_TX_ACTIVE, X9_USB_TX_COMPLETE,
       X9_USB_TX_CANCELLED, X9_USB_TX_UNCERTAIN };
/* Request IDs belong to one lease; completion is HWO/CPR + STOP, not enqueue.
 * A reset result remains readable with its ORIGINAL generation. Terminal
 * records occupy a bounded slot until explicitly consumed or lease close. */
struct x9_usb_tx_result {
    uint32_t version, generation, state;
    int32_t error;
    uint64_t request_id;
    uint32_t requested, actual;
    uint64_t reserved[4];
};
struct x9_usb_status {
    uint32_t version, state;
    int32_t error;
    uint32_t mode, generation, speed, configuration, data_alt;
    uint32_t link, native_apple_stack, trunk_version, qmu_error;
    uint32_t tx_pending, rx_pending, raw_enabled, reserved;
    uint64_t net_tx_frames, net_rx_frames, raw_tx_bytes, raw_rx_bytes;
    uint64_t dropped, resets, dma_completions, dma_errors;
};
#ifdef KERNEL
#ifdef __cplusplus
extern "C" {
#endif
void x9_usb_start(void);
void x9_usb_get_status(struct x9_usb_status *);
int x9_usb_claim(uint64_t *);
void x9_usb_release(uint64_t);
int x9_usb_send(uint64_t, uint32_t, const void *, uint32_t);
int x9_usb_receive(uint64_t, uint32_t, void *, uint32_t *);
int x9_usb_send_tracked(uint64_t, uint32_t, const void *, uint32_t, uint64_t *);
int x9_usb_get_tx_result(uint64_t, uint32_t, uint64_t, unsigned, struct x9_usb_tx_result *);
int x9_usb_dma_alloc(void **, uint64_t *);
/* Sole device worker only. Registry empty by default; the bulk QMU backend is
 * not installed today. available requires native_apple_stack AND a live bus.
 * Does not enable native mode, change identity or accept a user callback. */
void x9_usb_native_worker_poll(unsigned available, uint64_t now_ns);
/* Kernel-only, synchronous worker barrier. Cycle delay is 0..1000 ms.
 * Timeout is ambiguous: never replay. No native-family identity is implied. */
int x9_usb_bus_command(unsigned, uint32_t);
/* Metadata changes only after a completed explicit bus-off, not during a
 * cycle. Sole worker commits; success does not imply host enumeration. */
int x9_usb_set_description(const struct x9_usb_description *);
void x9_usb_get_description(struct x9_usb_description_status *);
int x9_mtu3_init(int);
uint32_t x9_mtu3_read(unsigned);
void x9_mtu3_write(unsigned, uint32_t);
uint32_t x9_mtu3_ip_read(unsigned);
void x9_mtu3_fifo_read(uint8_t *, unsigned);
void x9_mtu3_fifo_write(const uint8_t *, unsigned);
#ifdef __cplusplus
}
#endif
#endif
#endif
