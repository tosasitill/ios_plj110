/* Kernel-private connection from the real receiver/DMA controller to the
 * existing MT6993 cold sensor service. No default/dummy engine is installed. */
#ifndef X9_CAMERA_NATIVE_SENSOR_H
#define X9_CAMERA_NATIVE_SENSOR_H
#ifdef KERNEL
#include "x9_camera.h"
struct x9_camera_native_engine {
    uint32_t version;
    void *context;
    /* Required real controller admission, before any sensor power mutation.
     * Checks availability of normal mapping/route/rate/retirement machinery;
     * not a callback-address, register-idle or sensor-ID capability test. */
    int (*admit)(void *);
    /* Receives the actually configured, still non-streaming cold epoch,
     * after real inactive MM/PHY/SMI/source preparation. Installs/pins the
     * controller's mapping/rate/request lifetime before capture is attempted.
     * The actual MM token is cam_mm.receive.power_token, NOT cam_receive's
     * unrelated mode3 resource-probe token. */
    int (*prepare)(void *, uint64_t epoch, const struct x9_camera_sensor *);
    /* Control mutex held; frame mutex NOT held. Owns normal-domain attach,
     * immutable request/credit publication, completion and full stop/drain.
     * off must also accept a partially failed on without reissuing work.
     * No callback may re-enter a camera API that takes the control mutex. */
    int (*capture)(void *, uint64_t epoch, const struct x9_camera_sensor *,
        int on, uint64_t generation, const struct x9_camera_config *);
    /* Frame-bound AE scheduler. Must retain current applied metadata while
     * a request is merely queued, apply the actual group-held sensor update
     * at its owned boundary, and bind the resulting controls to later SOF.
     * Called with control mutex, without the worker's frame mutex. */
    int (*exposure)(void *, uint64_t epoch, const struct x9_camera_sensor *,
        uint32_t exposure_lines, uint32_t gain_q10, uint32_t frame_lines);
    /* Also called after a failed prepare, before core attempted capture.
     * Any exported/installed normal map or pending DMA retains the complete
     * sensor/MM/receiver journal until the actual engine proves retirement.
     * No caller-supplied "drained" bit or cleanup-success assumption. */
    int (*retirement_guard)(void *, uint64_t epoch, const struct x9_camera_sensor *);
};
#define X9_CAMERA_NATIVE_ENGINE_VERSION 1u
/* Standalone thread entry, serializes admission and publishes the adapter
 * only after the real engine admits and both physical IDs match/retire.
 * No provider calls this yet. Registration alone is not a physical frame. */
int x9_camera_native_sensor_register(const struct x9_camera_native_engine *);
#endif
#endif
