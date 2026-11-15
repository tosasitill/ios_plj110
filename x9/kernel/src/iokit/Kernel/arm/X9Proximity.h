/* MT6993 front proximity. Sensorhub owns TCS3720 power/I2C/calibration. */
#ifndef X9_PROXIMITY_H
#define X9_PROXIMITY_H
#include <stdint.h>
#define X9_PROX_MTK_TYPE 8u
#define X9_PROX_INFO_BYTES 40u
#define X9_PROX_DATA_BYTES 36u
struct x9_prox_sample {
 /* host_time is mach_absolute_time at receipt. scp_time_ns is the legacy
  * name for unconverted wire time, not a qualified host measurement time.
  * A confirmed on-change level does not expire by receipt/measurement age. */
 uint64_t sequence, host_time, scp_time_ns;
 int32_t oneshot, steps, auxiliary;
 uint32_t near, diagnostic;
};
struct x9_prox_status {
 uint32_t mode, ready, configured, desired, enabled, pending, interval_us;
 int32_t error;
 uint64_t accepted, rejected;
};
/* A single shared SCP owner registers this, never a second SCP boot/reset.
 * request posts desired state to its worker, it MUST NOT block the HID gate.
 * It must report a matched ENABLE/DISABLE ACK with x9_prox_native_control.
 * owner state is reset/invalidated before a disconnect. callbacks outlive
 * the HID service; no hard-IRQ callbacks and no caller-held backend locks. */
struct x9_prox_transport {
 int (*request)(uint32_t enabled, uint32_t interval_us);
};
#ifdef __cplusplus
extern "C" {
#endif
int x9_prox_init(int mode);
void x9_prox_shutdown(void);
int x9_prox_copy_sample(struct x9_prox_sample *sample);
void x9_prox_copy_status(struct x9_prox_status *status);
int x9_prox_set_enabled(uint32_t enabled, uint32_t interval_us);
int x9_prox_bind_transport(const struct x9_prox_transport *transport);
int x9_prox_native_configure(const void *info, unsigned length);
void x9_prox_native_control(uint32_t enabled, int error);
void x9_prox_native_disconnect(int error);
int x9_prox_native_data(const void *record, unsigned length);
/* External ingress is mode 1 ONLY; cannot qualify native READY/power. */
int x9_prox_configure_mtk(const void *info, unsigned length);
int x9_prox_receive_mtk(const void *record, unsigned length);
#ifdef __cplusplus
}
#endif
#endif
