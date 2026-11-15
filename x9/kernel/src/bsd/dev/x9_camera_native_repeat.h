#ifndef X9_CAMERA_NATIVE_REPEAT_H
#define X9_CAMERA_NATIVE_REPEAT_H
#ifdef KERNEL
/* Called after actual first irq_begin, BEFORE the actual source 0100-on.
 * Opt-in preserves the original independent one-credit implementation. */
int x9_camera_native_repeat_enable(uint64_t epoch);
/* After real DONE, FBC acceptance, CPU copy/retirement and worker retirement,
 * prepare the next same-domain slot. Source/PHY/SMMU/SMI stay held and live.
 * Returns only request preparation/queued success, NOT next frame success.
 * A subsequent actual no-credit SOF IAR publishes ONE RCNT; real later
 * SOF/DONE/cache remains the only frame path. No reset/credit cancellation.
 */
int x9_camera_native_repeat(uint64_t epoch,const struct x9_camera_camsv_request *,
    const struct x9_camera_camsv_dma *);
#endif
#endif
