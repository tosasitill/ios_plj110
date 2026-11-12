/* Kernel-private PIO control exchange, NOT an Apple C++ ABI. */
#ifndef X9_USB_NATIVE_EP0_H
#define X9_USB_NATIVE_EP0_H
#include <stdint.h>
#define X9_USB_EP0_CAPACITY 65535u
enum x9_usb_ep0_event_kind { X9_EP0_SETUP = 1, X9_EP0_OUT_DATA, X9_EP0_COMPLETE };
enum x9_usb_ep0_reply_kind { X9_EP0_SEND_IN = 1, X9_EP0_RECEIVE_OUT,
                           X9_EP0_STATUS, X9_EP0_STALL };
struct x9_usb_ep0_event {
    uint64_t ticket, sequence;
    uint32_t kind, actual, actual_qualified, status_complete;
    int error;
    uint8_t setup[8];
};
#ifdef __cplusplus
extern "C" {
#endif
/* Only X9USBControlBridge's SAME controller gate may claim/ack/reply. Bytes
 * are copied, not retained; caller native request/descriptor pointers never
 * cross this boundary. Reply success is admission, NOT wire completion.
 * No method enables native mode, endpoints, profile, bus or loader.
 */
int x9_usb_native_ep0_pending(void);
int x9_usb_native_ep0_claim(struct x9_usb_ep0_event *, void *, uint32_t capacity);
int x9_usb_native_ep0_ack(uint64_t sequence);
int x9_usb_native_ep0_reply(uint64_t ticket, unsigned kind, const void *, uint32_t);
/* Sole USB worker only. Offline/reset never retire unconfirmed IN bytes as
 * success. A claimed event and its bytes stay stable until gate ack. */
void x9_usb_native_ep0_invalidate(int error);
void x9_usb_native_ep0_poll(unsigned available);
void x9_usb_native_control_notify(void);
#ifdef __cplusplus
}
#endif
#endif
