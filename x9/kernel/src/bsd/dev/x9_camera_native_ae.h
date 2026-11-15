/* Internal frame-bound controls. All entries require camera_control_lock.
 * next additionally requires camera_irq_lock, never its IRQ spin lock. */
#ifndef X9_CAMERA_NATIVE_AE_H
#define X9_CAMERA_NATIVE_AE_H
static int cam_native_ae_seed_locked(uint64_t,const struct x9_camera_config *);
static int cam_native_ae_snapshot_locked(uint64_t,struct x9_camera_completed_frame *);
static int cam_native_ae_queue_locked(uint64_t,uint32_t,uint32_t,uint32_t);
static int cam_native_ae_next_locked(uint64_t,struct x9_camera_completed_frame *);
#endif
