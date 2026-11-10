/* X9 camera phase 1. Stable user ABI; no pointers or size_t on the wire. */
#ifndef X9_CAMERA_H
#define X9_CAMERA_H
#include <stdint.h>
#include <sys/ioccom.h>
#define X9_CAMERA_DEVICE "/dev/x9-camera"
#define X9_CAMERA_BACK 1u
#define X9_CAMERA_FRONT 2u
#define X9_CAMERA_READY 1u
#define X9_CAMERA_STREAMING 2u
#define X9_CAMERA_FAULT 4u
#define X9_CAMERA_MAX_BYTES (16u * 1024u * 1024u)
struct x9_camera_status {
    uint32_t version, mode, flags, supported_mask;
    uint32_t sensor, last_errno, width, height;
    uint64_t generation, frames, rejected;
    uint32_t chip_id[2], reserved[6];
};
#define X9_CAMERA_GET_STATUS _IOR('C', 0x90, struct x9_camera_status)
#ifdef KERNEL
/* Platform adapter must own/restore only its camera resources. Calls below
 * run in thread context, serialized, and must not re-enter camera APIs.
 * No adapter exists yet for MT6993 power/SENINF/RAW DMA: registering a dummy
 * one, bypassing SMMU, or feeding patterns is NOT a hardware implementation. */
enum x9_camera_power_id { X9_CAM_MCLK, X9_CAM_RESET, X9_CAM_AVDD,
    X9_CAM_AVDD1, X9_CAM_AFVDD, X9_CAM_DVDD, X9_CAM_DOVDD,
    X9_CAM_DRIVE, X9_CAM_SCL_AP, X9_CAM_SDA_AP };
struct x9_camera_power_step { uint32_t id, value, delay_us; };
struct x9_camera_sensor {
    uint32_t sensor, bus, slave, chip_id, width, height, bayer;
    uint32_t line_ns, frame_lines, frame_max, exposure_step, exposure_min;
    uint32_t gain_unity, gain_max, cphy, lanes;
    const struct x9_camera_power_step *power;
    uint32_t power_count;
};
struct x9_camera_platform {
    uint32_t version;
    /* power(true) follows sensor->power, power(false) restores ownership in
     * reverse order, including failures halfway through power(true). */
    int (*power)(const struct x9_camera_sensor *, int on);
    int (*read8)(const struct x9_camera_sensor *, uint16_t reg, uint8_t *);
    int (*write8)(const struct x9_camera_sensor *, uint16_t reg, uint8_t);
    /* on: set up CSI VC0/DT0x2b, RAW10 DMA and SMMU before sensor stream-on.
     * off: stop/drain DMA before returning, even after partial init failure.
     * generation tags every completion; old completions are rejected. */
    int (*capture)(const struct x9_camera_sensor *, int on, uint64_t generation);
};
int x9_camera_register_platform(const struct x9_camera_platform *);
int x9_camera_deliver(uint32_t sensor, uint64_t generation, const void *raw,
                      uint32_t stride, uint32_t bytes, uint64_t time);
void sweet_x9_camera_init(void);
#endif
#endif
