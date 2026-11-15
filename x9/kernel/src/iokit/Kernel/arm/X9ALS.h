/* X9 ALS ingress. MTK sensor/2.0 wire formats, not legacy nanohub packets. */
#ifndef X9_ALS_H
#define X9_ALS_H

#include <stdint.h>
#include "../../../bsd/dev/x9_scp_handoff.h"

#define X9_ALS_MTK_INFO_BYTES 40u
#define X9_ALS_MTK_DATA_BYTES 36u
#define X9_ALS_MAX_LUX 200000u
#define X9_ALS_MAX_AGE_NS 5000000000ULL

struct x9_als_sample {
	uint64_t sequence;
	uint64_t host_time;       /* mach_absolute_time at receipt, NOT SCP ns */
	uint64_t scp_time_ns;
	uint32_t lux;             /* HID ALS level is integer lux, not 16.16 */
	uint32_t diagnostic;
};

enum x9_als_stage {
 X9_ALS_WAITING = 0, X9_ALS_LAYOUT, X9_ALS_REGION, X9_ALS_POWER,
 X9_ALS_CLOCK, X9_ALS_BOOT, X9_ALS_HOST_READY, X9_ALS_SHARED,
 X9_ALS_TIMESYNC, X9_ALS_LIST, X9_ALS_ENABLE, X9_ALS_STREAM,
 X9_ALS_DISABLE, X9_ALS_STOPPED
};
struct x9_als_status {
 uint32_t stage;
 int32_t error;
 uint32_t layout_valid, power_requested, firmware_ready, sensor_ready;
 uint32_t control_enabled, gain, report_interval_us, worker_running;
 uint64_t packets, rejected, records;
 int64_t last_smc;
};

#ifdef __cplusplus
extern "C" {
#endif
int x9_als_init(int diagnostic);
void x9_als_shutdown(void);
int x9_als_copy_sample(struct x9_als_sample *sample);
/* mode 3 only: one bounded native SCP startup per boot, never a reset loop.
 * LK/TFA must already have loaded this device's signed SCP firmware. No
 * partition writes, SRAM patching, AP sensor I2C or default sensor/gain. */
int x9_als_start_transport(void);
/* Shared sensorhub owner for another native HID (e.g. proximity). ALS0 may
 * initialize it, ALS1 may upgrade; ALS2 refuses. Idempotent, one worker per
 * boot. A shared caller pins the owner until reboot; ALS HID stop cannot
 * stop another sensor's transport. Repeated calls never reset/retry faults. */
int x9_als_start_shared_transport(void);
void x9_als_copy_status(struct x9_als_status *status);
uint32_t x9_als_get_report_interval_us(void);
/* Queue/coalesce desired cadence in native mode. Success is not an ACK;
 * get_report_interval_us returns the cadence applied by the real ACK. */
int x9_als_set_report_interval_us(uint32_t interval);

/* Called by a SCP transport worker, never from hard IRQ context.
 * Configure from the LIGHT sensor_info returned by the sensor list before
 * passing complete share_mem_data records. gain is the sensor's units/lux.
 * On SCP reset/disconnect invalidate before reconnecting/configuring.
 * Raw ingress is for mode 1. Mode 3 requires native READY/list/ENABLE ACK;
 * external configure cannot bypass that admission. */
int x9_als_configure_mtk(const void *info, unsigned int length);
int x9_als_receive_mtk(const void *record, unsigned int length);
void x9_als_invalidate_mtk(void);
#ifdef __cplusplus
}
#endif
#endif
