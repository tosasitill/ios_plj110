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
/* Source-derived, zero-MMIO receive plan, NOT a claim or hardware snapshot.
 * Request: version=1, sensor=1/2, every other byte zero. Separate command so
 * existing status/probe structs and ioctl sizes remain unchanged. */
#define X9_CAM_PIPE_SOURCE_PLAN 1u
#define X9_CAM_PIPE_SENSOR_PROBED 2u
#define X9_CAM_PIPE_NEED_ID 1u
#define X9_CAM_PIPE_NEED_MM_POWER 2u
#define X9_CAM_PIPE_NEED_PHY 4u
#define X9_CAM_PIPE_NEED_ROUTE 8u
#define X9_CAM_PIPE_NEED_DMA_MAP 16u
#define X9_CAM_PIPE_NEED_DRAIN 32u
struct x9_camera_pipeline {
    uint32_t version, sensor, flags, blockers;
    uint32_t csi_port, cphy, lanes, async;
    uint32_t width, height, stride, bytes;
    uint32_t vc, dt, outmux_candidate, camsv_candidate;
    uint32_t dma_bits, smmu_sid, vcdt, expected_size;
    uint32_t grab_pixels, grab_lines, format, dma_basic;
    uint64_t csi_base, camsv_base, smmu_base, dma_limit;
};
#define X9_CAMERA_GET_PIPELINE _IOWR('C', 0x93, struct x9_camera_pipeline)
/* Shared receive resources, separate 224-byte ABI. PROBE request is exactly
 * {.version=1}; GET returns the cached result without MMIO. mode3/root only.
 * This is CSI_BS_RX power + secure MM-SMMU power, NOT full MM camera clocks,
 * translation ownership, PHY setup or capture readiness. */
#define X9_CAM_RES_ATTEMPTED 1u
#define X9_CAM_RES_CSI_POWER 2u
#define X9_CAM_RES_SMMU_POWER 4u
#define X9_CAM_RES_SMMU_SNAPSHOT 8u
#define X9_CAM_RES_RESTORED 16u
#define X9_CAM_RES_LATCHED 32u
#define X9_CAM_RES_SMMU_INHERITED 64u
#define X9_CAM_RES_SMMU_QUIESCENT 128u /* No NS configuration; NOT ownership. */
#define X9_CAM_RES_SMMU_UNSUPPORTED 256u
#define X9_CAM_RES_SMMU_CHANGED 512u
#define X9_CAM_RES_NEED_CAM_POWER 1u
#define X9_CAM_RES_NEED_PHY 2u
#define X9_CAM_RES_NEED_ROUTE 4u
#define X9_CAM_RES_NEED_DMA_MAP 8u
#define X9_CAM_RES_NEED_DRAIN 16u
#define X9_CAM_RES_NEED_SMMU_OWNER 32u
#define X9_CAM_RES_NEED_SMMU_FEATURE 64u
#define X9_CAM_RES_NEED_CSI_POWER 128u
#define X9_CAM_RES_NEED_SMMU_POWER 256u
#define X9_CAM_RES_NEED_MM_INFRA 512u
enum x9_camera_resource_stage { X9_CAM_RES_STAGE_NONE, X9_CAM_RES_STAGE_MAP,
    X9_CAM_RES_STAGE_CSI, X9_CAM_RES_STAGE_SMMU_POWER,
    X9_CAM_RES_STAGE_SMMU_SNAPSHOT, X9_CAM_RES_STAGE_RESTORE,
    X9_CAM_RES_STAGE_DONE, X9_CAM_RES_STAGE_MM_INFRA };
struct x9_camera_resources {
    uint32_t version, flags, stage, last_errno, restore_errno, blockers;
    /* CSI bank1: AP EN, global EN, STA, DONE, PM_ACK, ALL_STA; before/after. */
    uint32_t csi_before[6], csi_after[6], hwccf_before, hwccf_after;
    uint32_t smc_get, smc_put, idr[6];
    uint32_t cr0, cr0ack, cr1, cr2, gbpa, gerror, gerrorn, strtab_cfg;
    uint32_t irq_ctrl, irq_ack, cmdq_prod, cmdq_cons, evtq_prod, evtq_cons;
    uint64_t strtab_base, cmdq_base, evtq_base;
    uint32_t mm_ack_before, mm_ack_active, mm_ack_after, mm_ack_required;
    uint32_t reserved[4];
};
#define X9_CAMERA_GET_RESOURCES _IOR('C', 0x94, struct x9_camera_resources)
#define X9_CAMERA_PROBE_RESOURCES _IOWR('C', 0x95, struct x9_camera_resources)
/* Explicit MM clock/domain transaction, NOT capture. Independent 576-byte
 * ABI: PROBE request {.version=1,.sensor=1/2}, all other bytes zero; GET
 * returns the cached result without MMIO. Once per boot, mode3/root/FWRITE.
 * effects are PM_ACK (domains/infra), FENC (muxes), CG_STA (gates), or
 * backup STA (VMM). All values are full register words; mask selects our
 * AP/aggregate vote bits. Effect bits follow the fixed slot resource table.
 * Neither source-rate/DVFS ownership nor SMI/SMMU mapping is established. */
#define X9_CAM_MM_SLOTS 11u
enum x9_camera_mm_slot { X9_CAM_MM_AO, X9_CAM_MM_INFRA0, X9_CAM_MM_VMM,
    X9_CAM_MM_VCORE, X9_CAM_MM_MAIN, X9_CAM_MM_MRAW, X9_CAM_MM_CAM_MUX,
    X9_CAM_MM_CAMTM_MUX, X9_CAM_MM_SENINF_MUX, X9_CAM_MM_CAM_GATE,
    X9_CAM_MM_SV_GATE };
#define X9_CAM_MM_ATTEMPTED 1u
#define X9_CAM_MM_POWER 2u
#define X9_CAM_MM_CLOCKS 4u
#define X9_CAM_MM_RESTORED 8u
#define X9_CAM_MM_LATCHED 16u
#define X9_CAM_MM_SLOT_SEEN 1u
#define X9_CAM_MM_SLOT_OWNED 2u
#define X9_CAM_MM_SLOT_BORROWED 4u
#define X9_CAM_MM_SLOT_ACTIVE 8u
#define X9_CAM_MM_SLOT_RESTORED 16u
#define X9_CAM_MM_SLOT_RETAINED 32u
#define X9_CAM_MM_NEED_RECEIVE 1u
#define X9_CAM_MM_NEED_POWER 2u
#define X9_CAM_MM_NEED_CLOCKS 4u
#define X9_CAM_MM_NEED_RATE 8u
#define X9_CAM_MM_NEED_SMI 16u
#define X9_CAM_MM_NEED_DMA_MAP 32u
#define X9_CAM_MM_NEED_PHY 64u
#define X9_CAM_MM_NEED_ROUTE 128u
#define X9_CAM_MM_NEED_DRAIN 256u
#define X9_CAM_MM_RETAIN_CSI (1u<<30)
#define X9_CAM_MM_RETAIN_SECURE (1u<<31)
enum x9_camera_mm_stage { X9_CAM_MM_STAGE_NONE, X9_CAM_MM_STAGE_RECEIVE,
    X9_CAM_MM_STAGE_MAP, X9_CAM_MM_STAGE_TAKE, X9_CAM_MM_STAGE_RESTORE,
    X9_CAM_MM_STAGE_DONE };
struct x9_camera_mm_sample {
    uint32_t mask, flags;
    uint32_t before[3], active[3], after[3]; /* AP EN, aggregate EN, effect */
    uint32_t sta; /* Last observed HWV transaction status; NOT DONE. */
};
struct x9_camera_mm_status {
    uint32_t version, sensor, flags, stage, slot, last_errno, restore_errno, blockers;
    uint32_t cfg[3], retained; /* CAM/CAMTM byte, SENINF byte; slots + CSI30/secure31 */
    struct x9_camera_mm_sample sample[X9_CAM_MM_SLOTS];
};
#define X9_CAMERA_GET_MM_CLOCKS _IOR('C', 0x96, struct x9_camera_mm_status)
#define X9_CAMERA_PROBE_MM_CLOCKS _IOWR('C', 0x97, struct x9_camera_mm_status)
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
