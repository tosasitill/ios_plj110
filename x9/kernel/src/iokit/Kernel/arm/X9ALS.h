/* X9 ALS ingress. MTK sensor/2.0 wire formats, not legacy nanohub packets. */
#ifndef X9_ALS_H
#define X9_ALS_H

#include <stdint.h>

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

#ifdef __cplusplus
extern "C" {
#endif
int x9_als_init(int diagnostic);
void x9_als_shutdown(void);
int x9_als_copy_sample(struct x9_als_sample *sample);

/* Called by a future SCP transport worker, never from hard IRQ context.
 * Configure from the LIGHT sensor_info returned by the sensor list before
 * passing complete share_mem_data records. gain is the sensor's units/lux.
 * On SCP reset/disconnect invalidate before reconnecting/configuring.
 * This module does not boot SCP, map a FIFO, or enable the physical sensor. */
int x9_als_configure_mtk(const void *info, unsigned int length);
int x9_als_receive_mtk(const void *record, unsigned int length);
void x9_als_invalidate_mtk(void);
#ifdef __cplusplus
}
#endif
#endif
