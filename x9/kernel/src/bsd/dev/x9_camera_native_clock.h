/* In-TU native SENINF parent lifetime; no public hardware lease. */
#ifndef X9_CAMERA_NATIVE_CLOCK_H
#define X9_CAMERA_NATIVE_CLOCK_H
struct cam_native_clock_receipt {
    uint64_t sensor_epoch, mm_epoch;
    uint32_t sensor, entry, expected, prior, pcw, fenc, vcore[3];
    vm_offset_t pll;
    bool seen, owned, uncertain, update_pending, restored, conflict;
};
#endif
