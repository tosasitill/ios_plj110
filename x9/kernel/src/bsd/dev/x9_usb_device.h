/* Own-code MTU3 device transport ABI. Not Apple's IOUSBDeviceFamily ABI. */
#ifndef X9_USB_DEVICE_H
#define X9_USB_DEVICE_H
#include <stdint.h>
#define X9_USB_ABI 1u
#define X9_USB_CHUNK 4096u
#define X9_USB_SERVICE "X9USBDevice"
enum { X9_USB_STATUS, X9_USB_SEND, X9_USB_RECEIVE };
enum { X9_USB_OFF, X9_USB_INITIALIZING, X9_USB_READY, X9_USB_CONFIGURED,
       X9_USB_FAULT };
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
int x9_usb_dma_alloc(void **, uint64_t *);
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
