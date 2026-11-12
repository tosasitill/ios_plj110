/* Kernel-private MTU3 worker contract, not IOUSBDeviceFamily ABI. */
#ifndef X9_USB_NATIVE_QMU_H
#define X9_USB_NATIVE_QMU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct x9_usb_qmu_ticket { uint64_t serial; uint32_t endpoint, length; };
struct x9_usb_qmu_evidence {
    uint32_t stopped, hwo_clear, cpr_at_terminator, actual_qualified, actual;
    int error;
    const void *out_bytes;
};
/* Every function is sole-worker-only. Endpoint is USB address (01/81/03/83),
 * never an unqualified native endpoint object. enable is for a FUTURE native
 * control plane AFTER ABI/bootstrap and exclusive profile selection. It does
 * not set native_apple_stack, install a bridge, connect the bus or handle EP0.
 * Today native_apple_stack remains zero, so enable/validate/stage reject.
 */
int x9_usb_native_qmu_enable(uint32_t endpoint, unsigned usb_in);
int x9_usb_native_qmu_validate(uint32_t endpoint, unsigned usb_in);
int x9_usb_native_qmu_buffer(uint32_t endpoint, void **bounce);
int x9_usb_native_qmu_stage(uint32_t endpoint, uint32_t length,
                          struct x9_usb_qmu_ticket *);
/* Commit runs immediately before HWO, AFTER all rejectable hardware checks.
 * Nonzero means definitively unpublished. No owner call follows commit success
 * before cache/HWO publication. No callback or current-layout object in C. */
int x9_usb_native_qmu_publish(struct x9_usb_qmu_ticket,
                            int (*commit)(void *), void *context);
int x9_usb_native_qmu_discard(struct x9_usb_qmu_ticket);
/* EAGAIN = still active, no evidence consumed. stop=1 requests STOP and then
 * samples the SAME descriptor. Unknown partial actual is not invented. */
int x9_usb_native_qmu_probe(struct x9_usb_qmu_ticket, unsigned stop,
                          struct x9_usb_qmu_evidence *);
int x9_usb_native_qmu_finish(struct x9_usb_qmu_ticket, unsigned quarantine);
int x9_usb_native_qmu_barrier(void);
/* Readiness only: no endpoint/reset/identity/MMIO write. Requires a live,
 * configured native bulk pair, empty physical records and inactive queues. */
int x9_usb_native_qmu_rearm_ready(void);
void x9_usb_native_qmu_isolate(void);
int x9_usb_native_worker_quiesce(uint64_t now_ns);
#ifdef __cplusplus
}
#endif
#endif
