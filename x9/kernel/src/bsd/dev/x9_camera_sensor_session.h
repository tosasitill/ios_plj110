/* Kernel-private persistent cold sensor service, not a DMA/IOVA lease. */
#ifndef X9_CAMERA_SENSOR_SESSION_H
#define X9_CAMERA_SENSOR_SESSION_H
#include "x9_camera.h"
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
 * Sensor close first isolates reset/MCLK, then qualifies/retires that MM
 * lifetime, then restores sensor supplies. A receiver conflict retains both.
 * No standalone receiver-drop or caller-supplied drain assertion is exposed.
 * close is never DMA or completion-worker drain.
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
