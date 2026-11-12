/* Kernel-private PIO control exchange, NOT an Apple C++ ABI. */
#ifndef X9_USB_NATIVE_EP0_H
#define X9_USB_NATIVE_EP0_H
#include <stdint.h>
#define X9_USB_EP0_CAPACITY 65535u
enum x9_usb_ep0_event_kind { X9_EP0_SETUP = 1, X9_EP0_OUT_DATA, X9_EP0_COMPLETE,
                           X9_EP0_IN_DATA, X9_EP0_IN_ZLP, X9_EP0_OUT_PACKET };
enum x9_usb_ep0_reply_kind { X9_EP0_SEND_IN = 1, X9_EP0_RECEIVE_OUT,
                           X9_EP0_STATUS, X9_EP0_STALL,
                           X9_EP0_SEND_IN_REQUEST, X9_EP0_SEND_IN_ZLP,
                           X9_EP0_RECEIVE_OUT_PACKETS, X9_EP0_CONTINUE_OUT };
struct x9_usb_ep0_event {
    uint64_t ticket, sequence;
    uint32_t kind, actual, actual_qualified, status_complete;
    // OUT_PACKET only: actual is cumulative wire bytes, bytes contains JUST
    // this packet. A final zero packet has packet_length=0, not "no event".
    uint32_t packet_offset, packet_length, packet_final;
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
/* Same gate, asynchronous cancellation only. Never releases an accepted
 * native request or authorizes a callback/count without terminal evidence. */
int x9_usb_native_ep0_cancel(uint64_t ticket, int error);
/* Sole USB worker only. Offline/reset never retire unconfirmed IN bytes as
 * success. A claimed event and its bytes stay stable until gate ack. */
void x9_usb_native_ep0_invalidate(int error);
void x9_usb_native_ep0_poll(unsigned available);
/* Sole worker rearm guard; includes pending events and physical PIO state. */
int x9_usb_native_ep0_rearm_ready(void);
void x9_usb_native_control_notify(void);
#ifdef __cplusplus
}
#endif
#endif
