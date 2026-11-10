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
#define X9_CAMERA_PROBED 8u /* Sensor identity only; NEVER capture readiness. */
#define X9_CAMERA_MAX_BYTES (16u * 1024u * 1024u)
struct x9_camera_status {
    uint32_t version, mode, flags, supported_mask;
    uint32_t sensor, last_errno, width, height;
    uint64_t generation, frames, rejected;
    uint32_t chip_id[2], probe_mask, probe_flags, reserved[4];
};
#define X9_CAMERA_GET_STATUS _IOR('C', 0x90, struct x9_camera_status)
/* Separate probe-only ABI. Requests carry version=1 and sensor=1/2, all
 * other fields zero. Results preserve raw observations even on failure.
 * mode3 + root + FWRITE only; no stream-on, vendor tables or iOS injection. */
#define X9_CAM_PROBE_ATTEMPTED 1u
#define X9_CAM_PROBE_MATCHED 2u
#define X9_CAM_PROBE_RESTORED 4u
#define X9_CAM_PROBE_LATCHED 8u
#define X9_CAM_PROBE_CLOCK 16u /* Voter/clock/PLL-fence checked, not capture or measured Hz. */
#define X9_CAM_BLOCK_CLOCK 1u
#define X9_CAM_BLOCK_RAIL 2u
#define X9_CAM_BLOCK_ROUTE 4u
#define X9_CAM_BLOCK_I2C 8u
#define X9_CAM_BLOCK_INHERITED 16u
enum x9_camera_probe_stage { X9_CAM_STAGE_NONE, X9_CAM_STAGE_PREFLIGHT,
    X9_CAM_STAGE_PMIC, X9_CAM_STAGE_BUS, X9_CAM_STAGE_LDO,
    X9_CAM_STAGE_POWER, X9_CAM_STAGE_ID, X9_CAM_STAGE_RESTORE,
    X9_CAM_STAGE_DONE, X9_CAM_STAGE_CLOCK }; /* Append: preserve existing stage ABI. */
struct x9_camera_probe {
    uint32_t version, sensor, flags, stage;
    uint32_t chip_id, last_errno, restore_errno, blockers;
    uint32_t wl_id, wl_enable, mclk_cfg, mclk_status;
    uint32_t dovdd, afvdd, i2c_status[2];
    /* Former reserved[4], same 80-byte v1 ABI; request must still be zero.
     * AP EN is the entry snapshot; DONE/fence are last clock observations. */
    uint32_t hwv_vote, hwv_done, pll_con1, pll_fence;
};
#define X9_CAMERA_GET_PROBE _IOR('C', 0x91, struct x9_camera_probe)
#define X9_CAMERA_PROBE_SENSOR _IOWR('C', 0x92, struct x9_camera_probe)
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
