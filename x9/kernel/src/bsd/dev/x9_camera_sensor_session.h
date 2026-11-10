/* Kernel-private persistent cold sensor service, not a DMA/IOVA lease. */
#ifndef X9_CAMERA_SENSOR_SESSION_H
#define X9_CAMERA_SENSOR_SESSION_H
#include "x9_camera.h"
#include "x9_camera_outmux.h"
#include "x9_camera_mac.h"
#include "x9_camera_phy.h"
#ifdef KERNEL
/* Thread context, standalone callers: these acquire camera_control_lock.
 * A platform callback already holding that mutex must use the internal
 * helpers in the camera translation unit, not re-enter these entrypoints.
 * A token identifies our journal, not exclusive AP/SCP or DMA authority.
 * No stream-on or receiver/DMA start operation is exposed here.
 *
 * open sets *epoch=0 before admission. A nonzero epoch, INCLUDING on an
 * error, pins the one cold journal until ONE close attempt. Hardware errors
 * latch the boot: do not repeat open/configure/read/close to recover.
 * Successful open leaves reset deasserted and matched ID in standby, with
 * power, MCLK and AP buses held. Configure verifies the preview tables and
 * leaves that same sensor powered, not streaming.
 *
 * The optional receiver prepare holds the real register-clock/MM journal
 * through the sensor epoch, after configured standby. It samples disabled
 * receiver controls, but neither writes PHY/route/DMA nor grants ownership.
 * Failed admitted preparation also remains tied to this ONE sensor close.
 * Optional configure_mac BEFORE configure_outmux journals inactive packet
 * format / normal RAW10 identity / unsplit async and CAM_MAIN AFIFO; no PHY
 * mux or MAC lane enable. It restores AFTER OUTMUX on sensor close so the
 * async clock remains held through OUTMUX's restoration commit.
 * Optional prepare_analog AFTER configure_mac and BEFORE configure_outmux
 * initializes/calibrates both inactive analog banks using current LK
 * devinfo. No digital lane enable or rate/SMI/mapping grant. An unconsumed
 * calibration blocks receiver/MM/supply retirement. Close restores analog
 * AFTER OUTMUX and BEFORE MAC/async, keeping selected async CG through both.
 * Optional configure_timing measures the current CSI clock twice with real
 * meter BUSY rise/fall and applies fixed HS timing/CPHY POST, lane-map and
 * normal protocol controls, PHY lanes off. Front's >1500Mbps generic
 * deskew EN/LP11 are on, but DELAY_EN/PERIODIC/APPLY and IRQs remain off;
 * both sensors' ULPS/ALP/LRTE support flags0 are qualified, not enabled.
 * The dedicated inactive deskew reset proves only control assertion and
 * release, NOT calibration results, IRQ-history retirement or drain.
 * Timing must precede CTLE/OUTMUX. A pending meter/reset blocks teardown;
 * close stops generic deskew and restores owned digital controls before
 * analog off, with no late reset/cancel/retry to manufacture retirement.
 * Optional configure_ctle then applies front static EQ or main preview
 * EQ/CDR9 to both banks, before OUTMUX. Timing and generic PHY setup
 * must precede this final override. It has no APPLY pulse or lane start;
 * close restores the owned delay-force controls BEFORE analog power-off.
 * Optional configure_outmux then applies only the fixed inactive OUTMUX0
 * image route and confirms real shadow/inner consumption, still DL/IRQ off.
 * On success it also configures CAMSV0 tag0 RAW10 grab/format, A0 stride and
 * group/first/last tags, with actual outer/inner DB_EN consumption. No base
 * address, FBC/CQ/DMA/CMOS/VF enable or IRQ history read is performed. The
 * inactive CAMSV journal restores BEFORE OUTMUX; a pending field or DB
 * phase requires its actual target and keeps the lower dependencies held.
 * On success CAMSV also prepares this boot's GIC4 topology and a dormant
 * IRQ worker. That alone never claims/enables GIC or consumes IRQ history.
 * The normal controller's exact mapping/AP-IRQ lease is required before
 * arming a request. Normal begin consumes that actually armed S1 request's A0
 * address and tag0 FH_SPARE, then tag0 FBC_EN with unchanged empty counters
 * in both banks before GIC publication. After all three actual GIC lines
 * are published, begin issues ONE RCNT and requires two actual both-bank
 * RCNT+1/CNT1/unchanged WCNT/DROP/reserved/control-clear observations.
 * It then confirms group0 SW_DONE, SRAM/tag0 ERR, tag0 SOF and IMG0A DMA_EN
 * in both banks, then CMOS/FLUSH/VF in normal order. PHY/MAC lanes/OUTMUX DL
 * and sensor source stay off; this is an input-inactive consumer arm, not
 * a complete producer or a real frame. Incompatible inherited TG profiles
 * are refused before S1 arm, never repaired or considered native authority.
 * Cold configure_outmux still never writes an address, cookie or FBC.
 * Partial address/FBC/DB/credit/receive-enable publication retains
 * the cold epoch. No command retry/DB/reset cancels an issued credit; rearm
 * and CAMSV restore remain prohibited until real producer retirement exists.
 * Close first attempts actual TG_MODE_OFF/VF-off/TG-DB-up/down/CMOS-off,
 * then quiesces this input-inactive receive arm. Each phase's full actual
 * both-bank target must be observed; no replay, reset or unknown-up cancel.
 * STOPPED retains TG_MODE_OFF/FLUSH, not the original cold profile. It does
 * not cancel credit, change RAW ownership or claim AXI/map drain.
 * Sensor close retires GIC and queued/running worker pins BEFORE restoring
 * CAMSV/OUTMUX/PHY/MM. An unfinished RAW slot retains all
 * dependencies; this worker retirement is not full AXI/SMMU/map drain.
 * Its journal restores BEFORE dropping receiver/MM; a pending trigger has
 * no cancellation proof and blocks all dependent teardown.
 * Optional prepare_smi_clocks AFTER prepare_receiver and BEFORE MAC/PHY/
 * OUTMUX takes only CAMSV0's deduplicated subcomm1/2/3 and larb19/14/29
 * clocks. Before votes it captures all ten relevant XPU banks; only cold
 * gated/unvoted COMM33/L19 with unchanged L27/RAW_C sibling paths may have
 * their five misc controls initialized. Existing shared owners must match,
 * never have their profile replaced. Then it journals only CAMSV A0 L19/P0's
 * default OSTD and initial SRT controls.
 * All other ports, common BW/routing, MMU/bank/SID fields stay untouched.
 * Supplier/consumer effects, port controls and MMINFRA AL parent are checked
 * before dependent samples; no full SMI/QoS/IOVA or shared-common grant.
 * Close qualifies pending SMI targets BEFORE OUTMUX/PHY/MAC retirement;
 * then restores owned P0 and misc fields BEFORE these clocks, AFTER MAC/
 * PHY/OUTMUX and BEFORE MM/supplies. Posted target must be observed, not prior;
 * unconsumed clock requests and new owners retain dependencies. No DMA
 * producer is started, and zero LARB STAT/internal STAT is NOT full drain.
 * Sensor close first isolates reset/MCLK, then qualifies/retires that MM
 * lifetime, then restores sensor supplies. A receiver conflict retains both.
 * No standalone receiver-drop or caller-supplied drain assertion is exposed.
 * close includes the bound CAMSV GIC/worker retirement, never full DMA drain.
 * A receiver must drain before requesting a power unwind. An independently
 * journaled core capture attempt blocks close, even after partial failure.
 * No new ioctl, boot mode, READY or supported_mask is added.
 */
int x9_camera_sensor_session_open(uint32_t sensor, uint64_t *epoch,
    struct x9_camera_probe *);
int x9_camera_sensor_session_configure(uint64_t epoch, struct x9_camera_config *);
int x9_camera_sensor_session_prepare_receiver(uint64_t epoch, struct x9_camera_receiver *);
/* Only ID, standby/hold and fixed preview readback controls may be read.
 * No OTP, EEPROM, actuator, arbitrary table writer or stream command. */
int x9_camera_sensor_session_read8(uint64_t epoch, uint16_t reg, uint8_t *);
int x9_camera_sensor_session_close(uint64_t epoch, struct x9_camera_probe *);
#endif
#endif
