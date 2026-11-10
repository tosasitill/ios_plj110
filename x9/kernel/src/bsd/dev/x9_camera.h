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
#define X9_CAMERA_RETAINED 16u /* Failed drain/restore; dependencies must stay held. */
#define X9_CAMERA_MAX_BYTES (16u * 1024u * 1024u)
struct x9_camera_status {
    uint32_t version, mode, flags, supported_mask;
    uint32_t sensor, last_errno, width, height;
    uint64_t generation, frames, rejected;
    uint32_t chip_id[2], probe_mask, probe_flags;
    /* Former reserved[4]; unchanged 88B ABI. Cached journal, NOT MMIO or
     * actual rail/clock state. Only nonzero after incomplete cold restore. */
    uint32_t cold_retained, cold_restore_step, reserved[2];
};
#define X9_CAM_COLD_PINS 1u
#define X9_CAM_COLD_MCLK 2u
#define X9_CAM_COLD_SENSOR_BUS 4u
#define X9_CAM_COLD_WL_BUS 8u
#define X9_CAM_COLD_WL_RAILS 16u
#define X9_CAM_COLD_DOVDD 32u
#define X9_CAM_COLD_AFVDD 64u
#define X9_CAM_COLD_VS 128u
#define X9_CAM_COLD_WAKE 256u
#define X9_CAM_COLD_ROUTES 512u
#define X9_CAM_COLD_PMIF 1024u
#define X9_CAM_COLD_SENSOR_PARENT 2048u
#define X9_CAM_COLD_WL_PARENT 4096u
enum x9_camera_cold_restore_step { X9_CAM_COLD_NONE, X9_CAM_COLD_QUIESCE,
    X9_CAM_COLD_RESTORE_DOVDD, X9_CAM_COLD_RESTORE_WL, X9_CAM_COLD_RESTORE_AFVDD,
    X9_CAM_COLD_RESTORE_WAKE, X9_CAM_COLD_RESTORE_PINS, X9_CAM_COLD_RESTORE_SENSOR_BUS,
    X9_CAM_COLD_RESTORE_WL_BUS, X9_CAM_COLD_RESTORE_ROUTES,
    X9_CAM_COLD_RESTORE_CLOCK, X9_CAM_COLD_RESTORE_VS, X9_CAM_COLD_RESTORE_DONE,
    X9_CAM_COLD_RESTORE_PMIF, X9_CAM_COLD_RESTORE_WL_PARENT,
    X9_CAM_COLD_RESTORE_SENSOR_PARENT };
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
    X9_CAM_STAGE_DONE, X9_CAM_STAGE_CLOCK, X9_CAM_STAGE_CONFIG }; /* Append only. */
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
/* Powered, read-only receiver preflight. Shares the single per-boot MM
 * transaction with PROBE_MM_CLOCKS; use one or the other, not both.
 * PROBE request {.version=1,.sensor=1/2}, all other bytes zero. GET is
 * cache-only. No IRQ/status-clear reads, route commit, PHY or DMA writes.
 * Stable disabled controls are observations, NEVER a lease or drain proof. */
#define X9_CAM_RX_ATTEMPTED 1u
#define X9_CAM_RX_SNAPSHOT 2u
#define X9_CAM_RX_STABLE 4u
#define X9_CAM_RX_DISABLED 8u
#define X9_CAM_RX_RESTORED 16u
#define X9_CAM_RX_LATCHED 32u
#define X9_CAM_RX_NEED_OWNER 1u
#define X9_CAM_RX_NEED_RATE 2u
#define X9_CAM_RX_NEED_PHY 4u
#define X9_CAM_RX_NEED_ROUTE 8u
#define X9_CAM_RX_NEED_SMI 16u
#define X9_CAM_RX_NEED_DMA_MAP 32u
#define X9_CAM_RX_NEED_DRAIN 64u
#define X9_CAM_RX_NEED_SNAPSHOT 128u
enum x9_camera_receiver_stage { X9_CAM_RX_STAGE_NONE, X9_CAM_RX_STAGE_MM,
    X9_CAM_RX_STAGE_MAP, X9_CAM_RX_STAGE_SAMPLE, X9_CAM_RX_STAGE_CHECK,
    X9_CAM_RX_STAGE_RESTORE, X9_CAM_RX_STAGE_DONE };
#define X9_CAM_RX_OUT_WORDS 14u
#define X9_CAM_RX_SV_WORDS 17u
struct x9_camera_receiver_sample {
    uint32_t mac[4]; /* CTRL, VERSION, PHY_CTRL, PRBS_GEN */
    uint32_t async_cfg, async_split, afifo;
    uint32_t out[2][X9_CAM_RX_OUT_WORDS]; /* outer / inner, fixed offsets */
    uint32_t sv[2][X9_CAM_RX_SV_WORDS]; /* outer / inner, central/DMA/CQ */
};
struct x9_camera_receiver {
    uint32_t version, sensor, flags, stage, last_errno, blockers, busy, changed;
    uint64_t mac_base, out_base, out_inner_base, sv_base;
    struct x9_camera_receiver_sample before, after;
    struct x9_camera_mm_status mm;
};
#define X9_CAMERA_GET_RECEIVER _IOR('C', 0x98, struct x9_camera_receiver)
#define X9_CAMERA_PROBE_RECEIVER _IOWR('C', 0x99, struct x9_camera_receiver)
/* Explicit cold sensor configuration, NOT CSI/capture. Request is exactly
 * {.version=1,.sensor=1/2}; root/mode3/FWRITE. Shares probe's once-per-sensor
 * success / first-hardware-error latch: choose probe OR standby in a boot.
 * GET is cache-only. Tables are checked before acquiring power. No stream,
 * reset-command, group-hold, OTP, EEPROM or actuator TABLE commands are accepted.
 * Front-only fab read uses fixed 0a02=7f/0a00=1 then 0a00=0; no OTP programming.
 * Successful readback describes a temporary standby, then reset/power restore;
 * it is NOT persistent configuration, PHY-rate measurement or calibration. */
#define X9_CAM_CFG_TABLE_VALID 1u
#define X9_CAM_CFG_ATTEMPTED 2u
#define X9_CAM_CFG_STANDBY 4u
#define X9_CAM_CFG_WRITTEN 8u
#define X9_CAM_CFG_VERIFIED 16u
#define X9_CAM_CFG_RESTORED 32u
#define X9_CAM_CFG_LATCHED 64u
#define X9_CAM_CFG_FAB_READ 128u /* 3 fab bytes read + exit write ACK; not calibration. */
#define X9_CAM_CFG_FAB_B4 256u /* ready=1, fab=b4/01; requires two init patches. */
#define X9_CAM_CFG_NEED_FACTORY 1u /* QSC/LRC/actuator work not performed. */
#define X9_CAM_CFG_NEED_RECEIVER 2u
#define X9_CAM_CFG_NEED_DMA_MAP 4u
#define X9_CAM_CFG_NEED_DRAIN 8u
enum x9_camera_config_stage { X9_CAM_CFG_NONE, X9_CAM_CFG_TABLE_CHECK,
    X9_CAM_CFG_COLD, X9_CAM_CFG_IDLE, X9_CAM_CFG_INIT, X9_CAM_CFG_PREVIEW,
    X9_CAM_CFG_MIRROR, X9_CAM_CFG_READBACK, X9_CAM_CFG_RESTORE, X9_CAM_CFG_DONE,
    X9_CAM_CFG_FAB, X9_CAM_CFG_FAB_EXIT, X9_CAM_CFG_FAB_PATCH }; /* Append only. */
#define X9_CAM_CFG_SAMPLES 48u
struct x9_camera_config_sample { uint32_t reg, expected, observed; };
struct x9_camera_config {
    uint32_t version, sensor, flags, stage, last_errno, restore_errno;
    uint32_t init_pairs, preview_pairs, attempted, written;
    /* table_index is pair offset in INIT/PREVIEW, sample offset in READBACK. */
    uint32_t table_index, reg, expected, observed, read_count, blockers;
    struct x9_camera_probe probe;
    /* Only the first read_count entries have observed bytes. Unread values
     * are UINT32_MAX; never interpret zero-initialized/unread CSR as evidence. */
    struct x9_camera_config_sample sample[X9_CAM_CFG_SAMPLES];
};
#define X9_CAMERA_GET_CONFIG _IOR('C', 0x9a, struct x9_camera_config)
#define X9_CAMERA_CONFIG_STANDBY _IOWR('C', 0x9b, struct x9_camera_config)
#ifdef KERNEL
/* Platform adapter must own/restore only its camera resources. Calls below
 * run in thread context, serialized, and must not re-enter camera APIs.
 * No MT6993 capture adapter is registered: registering a dummy
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
    /* Version2 requires immutable, frame-bound completion metadata below.
     * Version1's late sensor-register sampling cannot identify frame state. */
    uint32_t version;
    /* power(true) follows sensor->power, power(false) restores ownership in
     * reverse order, including failures halfway through power(true). */
    int (*power)(const struct x9_camera_sensor *, int on);
    int (*read8)(const struct x9_camera_sensor *, uint16_t reg, uint8_t *);
    int (*write8)(const struct x9_camera_sensor *, uint16_t reg, uint8_t);
    /* on: set up CSI VC0/DT0x2b, RAW10 DMA and SMMU before sensor stream-on.
     * off: stop/drain DMA before returning, even after partial init failure.
     * generation tags every completion; old completions are rejected.
     * Nonzero off retains DMA mappings/clocks/rails: caller must NOT call
     * power(false), retry cleanup, or start another sensor in this boot. */
    int (*capture)(const struct x9_camera_sensor *, int on, uint64_t generation);
};
#define X9_CAMERA_PLATFORM_VERSION 2u
/* Kernel-only record, not an ioctl or the userland RAW10 header. Provider
 * binds this to one owned DMA slot/request AFTER PASS1_DONE/group/tag/inner
 * sequence, error checks and CPU cache sync. capture_id is a nonzero, strictly
 * increasing 64-bit owned-request identity within generation, not an unchecked
 * 32-bit IRQ counter. Metadata is the control state applied to THIS frame
 * (tracked with SOF/control-delay ownership), never registers sampled later.
 * If the binding is uncertain, do not submit the frame. reserved must be0. */
struct x9_camera_completed_frame {
    uint32_t version, sensor, stride, bytes;
    uint64_t generation, capture_id, time;
    uint32_t exposure_lines, gain_code, frame_lines, reserved;
    const void *raw;
};
int x9_camera_register_platform(const struct x9_camera_platform *);
/* Thread-context, synchronous COPY from a completed, CPU/cache-visible RAW10
 * buffer. Provider owns mapping/cache maintenance and keeps raw immutable
 * until return. EBUSY means pinned CPU staging/backpressure: drop this frame,
 * do not replay it or retain the hardware slot waiting on userland. This
 * entrypoint validates record/range/order, but does not itself establish
 * PASS1_DONE/tag/sequence, control application, drain or a DMA lease.
 * A valid record rejected with EBUSY is retired: replay returns ESTALE. */
int x9_camera_deliver(const struct x9_camera_completed_frame *);
/* ISP8s single-tag completion adapter. Kernel-only; does not allocate/map,
 * reserve a route, collect read-clear IRQ registers or program DMA. The sole
 * IRQ/route owner supplies ONE coherent snapshot, with group_tags latched for
 * this frame at SOF and inner_sequence read from its tag's FH_SPARE register.
 * CQ setting-done and input EOF alone never qualify a completed RAW buffer. */
#define X9_CAMERA_CAMSV_VERSION 2u
struct x9_camera_s1;
struct x9_camera_camsv_irq {
    uint32_t version, reserved;
    uint64_t generation, capture_id, time;
    uint32_t first_tag, last_tag, group_tags[4];
    uint32_t inner_sequence, outer_sequence;
    uint32_t done_status, err_status, cq_status, reserved1;
};
struct x9_camera_camsv_request {
    /* Frame-bound control state, not late I2C sampling. time must be zero:
     * finish supplies the qualified IRQ completion time. raw/metadata stay
     * immutable until finish returns or the owner proves stop/drain. */
    struct x9_camera_completed_frame frame;
    uint64_t iova;
    uint32_t inner_sequence, tag, group, reserved;
};
struct x9_camera_camsv_dma {
    uint32_t version, reserved;
    void *context;
    uint64_t lease;
    /* Exact exported S1 backing, not an arbitrary CPU range or PA. CAMSV
     * itself performs DEVICE CIVAC -> qualified CPU IVAC -> pinned copy.
     * Native controller must prohibit replenishing FBC/RCNT or automatically
     * reusing this RAW until finish returns RETIRED AND real producer/AXI
     * retirement permits it; a CPU pin cannot stop hardware by itself. */
    struct x9_camera_s1 *backing;
    /* Mandatory legal domain/route/slot lease check, including exact
     * IOVA->raw range and cache policy. NOT PM_GET or a CPU-PA identity map.
     * check never reads RAW or mutates hardware, and proves immutable
     * backing while this worker owns the slot, including after PASS1_DONE.
     * When used with the native GIC worker, also proves sole CAMSV0 AP IRQ
     * AND tag0 FBC policy (not CCU/other owner) and prior source/request-history
     * retirement before FBC initialization/GIC/one-credit/source publication.
     * This includes sole normal IMG0A DMA/tag0/group0 permission, prior RAW
     * writers retired and the entire immutable legal DMA range retained.
     * Masked/zero registers or FBC_CNT0 are not this grant.
     * Callbacks cannot re-enter camera/CAMSV APIs. */
    int (*check)(void *, uint64_t lease, const struct x9_camera_camsv_request *);
};
enum x9_camera_camsv_slot_state {
    X9_CAMSV_EMPTY, X9_CAMSV_ARMED, X9_CAMSV_RETIRED, X9_CAMSV_RETAINED
};
/* Zero-initialize a kernel-owned slot before its first arm. After that only
 * these APIs mutate it. The owner serializes result consumption/rearm/stop
 * for each slot and pins the slot/callback context/raw/map
 * until all synchronous calls return. capture(off) stops and drains hardware
 * AND these workers before reporting success; this adapter cannot prove drain.
 * RETAINED is terminal in this boot: no memset/rearm/free/unmap on an error. */
struct x9_camera_camsv_slot {
    struct x9_camera_camsv_request request;
    struct x9_camera_camsv_dma dma;
    uint32_t state, last_errno;
};
int x9_camera_camsv_arm(struct x9_camera_camsv_slot *,
    const struct x9_camera_camsv_request *, const struct x9_camera_camsv_dma *);
/* Thread context. EAGAIN with ARMED: no SW_GP_PASS1_DONE, keep waiting. EBUSY
 * with RETIRED: qualified frame dropped by CPU backpressure (do not replay).
 * Callback failures may return the SAME errno with RETAINED; always inspect
 * state, not errno alone. ESTALE may be an obsolete IRQ/generation. A retained
 * slot requires the owner's fault/drain path, not retry or dependency release. */
int x9_camera_camsv_finish(struct x9_camera_camsv_slot *, const struct x9_camera_camsv_irq *);
/* Thread-context standalone controller calls; acquire camera_control_lock.
 * In-TU platform callbacks already holding that lock use the locked helper.
 * Requires the prepared live-topology/receiver epoch and a REAL DMA lease.
 * begin arms one tag0/group0 request, publishes its actual S1 IOVA and
 * 32-bit FH metadata with both-bank DB consumption, initializes only tag0
 * FBC_EN with unchanged empty FBC1 counters, THEN arms GIC lines and emits
 * exactly one RCNT command. It requires actual both-bank RCNT+1(mod256),
 * CNT=1, unchanged WCNT/DROP/reserved and command self-clear, twice.
 * It then writes only group0 SW_DONE, SRAM/tag0 ERR, tag0 SOF source enables
 * and IMG0A DMA_EN, each once with two complete actual both-bank observations.
 * Begin then journals CMOS_EN, FLUSH_ALL_SRC_DIS and VFDATA_EN in normal
 * order, each with two complete actual both-bank targets. CQ, PHY/MAC lanes,
 * OUTMUX DL and sensor stream stay off. RECEIVE_ARMED/GATE_ARMED describe
 * accepted controls, not a started upstream producer, frame or drain.
 * After full consumer arm, begin binds the actually accepted FBC counters.
 * The sole SOF/DONE IAR owner captures ordinary tag0 FBC controls/counters;
 * before CPU/cache access its worker requires two full both-bank observations
 * of the original WCNT+1(mod256), CNT=0 and unchanged RCNT/DROP/reserved.
 * WCNT and CNT are independently monotonic after the actual owned SOF,
 * not assumed to update together or specifically at DONE. This count proof
 * does not enable input, relax SV's cold guard, or grant RAW/AXI/map retirement.
 * EMPTY/CREDIT are not a source/AXI/map drain or native ownership grant.
 * Address/metadata/credit failure retains this slot and dependent cold epoch;
 * GIC/CPU retirement alone does not permit revoking its backing/lease.
 * Keep that controller/request/RAW immutable until state says RETIRED; no
 * automatic DMA reuse. State is cache-only, not source/AXI/map drain. An
 * issued credit currently blocks rearm/restore: producer retirement is not
 * implemented and EN-off/reset is not permission to revoke that credit.
 * Close can stop this input-inactive TG first (TG_MODE_OFF, VF off, TG DB
 * bit8 up/down, CMOS off), then quiesce receive enables before masking GIC.
 * Every TG DB phase requires its own actual target; unknown up cannot be
 * canceled by down. Cold-profile compatibility is checked before S1 arm,
 * not treated as native authority. No reset, blind replay or counter rebase;
 * accepted credit and RAW/lease/dependencies stay retained even after stop.
 * There is still no registered native capture caller. */
int x9_camera_camsv_irq_begin(uint64_t, const struct x9_camera_camsv_request *,
    const struct x9_camera_camsv_dma *);
int x9_camera_camsv_irq_state(uint64_t, uint32_t *state, int *error);
bool x9_camera_irq_filter(uint32_t);
void sweet_x9_camera_init(void);
#endif
#endif
