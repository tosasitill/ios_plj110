/* Shared kernel/iOS ABI: real SCP accelerometer, in iOS device axes and g.
 * Android's SCP has already applied gsensor_1 direction=4. Do NOT rotate it
 * a second time. Android measures specific force; iOS uses its negative.
 * No orientation/compass samples are fabricated from absent sensors. */
#ifndef X9_MOTION_H
#define X9_MOTION_H
#include <stdint.h>
#define X9_MOTION_ABI 1u
#define X9_MOTION_MTK_TYPE 1u
#define X9_MOTION_INFO_BYTES 40u
#define X9_MOTION_DATA_BYTES 36u
#define X9_MOTION_MAX_AGE_NS 500000000ULL
struct x9_motion_sample {
 uint32_t version, bytes;
 uint64_t sequence, host_time, scp_time;
 int32_t raw[3], fixed_g[3];
};
struct x9_motion_status {
 uint32_t version, bytes, active, configured, desired, enabled;
 uint32_t gain, requested_us, applied_us, pending;
 int32_t error;
 uint32_t reserved;
 uint64_t accepted, rejected;
};
#ifdef __cplusplus
extern "C" {
#endif
int x9_motion_init(void);
void x9_motion_shutdown(void);
int x9_motion_set_interval(uint32_t);
int x9_motion_copy_sample(struct x9_motion_sample *);
void x9_motion_copy_status(struct x9_motion_status *);
int x9_motion_native_configure(const void *, unsigned);
void x9_motion_native_control(uint32_t, uint32_t, int);
void x9_motion_native_disconnect(int);
int x9_motion_native_data(const void *, unsigned);
#ifdef __cplusplus
}
#endif
#endif
