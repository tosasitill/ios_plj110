/* Internal frame-bound controls. All entries require camera_control_lock.
 * next additionally requires camera_irq_lock, never its IRQ spin lock. */
#ifndef X9_CAMERA_NATIVE_AE_H
#define X9_CAMERA_NATIVE_AE_H
struct cam_native_ae_controls { uint32_t lines,gain,frame_lines; };
struct cam_native_ae_state {
    uint64_t epoch,queued_serial,applied_serial;
    uint32_t sensor,writes,advances;
    uint8_t counter;
    bool seeded,pending,hold_attempted,release_attempted,hold_ack,release_ack;
    int error;
    struct cam_native_ae_controls applied,wanted;
    /* ACKed OB selection and frame-bound selection are distinct receipts.
     * Last OB transaction retains every attempted register/value, even when
     * a posted START or the deadline leaves its outcome uncertain. */
    uint64_t ob_serial,ob_transactions;
    uint32_t ob_prior_type,ob_target_type,ob_acked_type,ob_applied_type;
    uint32_t ob_writes,ob_acks;
    uint16_t ob_regs[8];
    uint8_t ob_values[8];
};
static int cam_native_ae_seed_locked(uint64_t,const struct x9_camera_config *);
static int cam_native_ae_snapshot_locked(uint64_t,struct x9_camera_completed_frame *);
static int cam_native_ae_queue_locked(uint64_t,uint32_t,uint32_t,uint32_t);
static int cam_native_ae_next_locked(uint64_t,struct x9_camera_completed_frame *);
#endif
