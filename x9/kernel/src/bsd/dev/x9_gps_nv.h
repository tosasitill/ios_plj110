/* Captured soc721a/v062 shared NV, kernel-only sole-worker APIs.
 * Not a persistent-storage backend, MCUSYS ACK or receiver-ready lease. */
#ifndef X9_GPS_NV_H
#define X9_GPS_NV_H
#include <stdint.h>
#define X9_GPS_NV_OFFSET 0x568000u
#define X9_GPS_NV_BYTES 0xc0000u
#define X9_GPS_NV_BLOCKS 21u
#define X9_GPS_NV_IO_MAX 4096u
struct x9_gps_nv_ops {
 void *context;
 /* Bounded, non-reentrant actual authority for this physical NV interval:
  * AP+BGF permissions/remap, power/upstream clocks, no conflicting cached
  * aliases or other AP writers. cold=1 additionally proves ALL old MCU/
  * BGF/NV DMA quiescent; retain that exclusion across the complete clear.
  * cold=0 keeps the same resources/epoch; current MCU may use its target
  * header, obeying the captured two-sided occupied protocol. Do not derive
  * this from NVLOCK logs, CTLR_CREATED, reset readback alone or BL-only SMC.
  * The same connv2 lease must remain retained on partial failure. */
 int (*check_owned)(void *, uint64_t generation, uint64_t lease,
     uint64_t physical, unsigned bytes, int cold);
};
struct x9_gps_nv_info {
 uint32_t capacity, size, version, host_flags, target_flags;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Only during real provider.provision, reset held, before HIF prepare.
 * Explicit opt-in: old v1..v4 suppliers do NOT authorize native NV merely
 * by registration. Maps exactly the768KiB region as device/noncacheable,
 * clears it once under cold authority, initializes21 host headers with
 * capacity=block_bytes-100 (data starts+96). Never lazily clears on RX.
 * Target headers stay zero after this qualified cold clear; no fake magic,
 * data/version or POST_ON. Unknown/failed authority must not touch NV.
 * Descriptor is copied; context/resources live until reboot. No retry,
 * disposal or automatic rebind, including when initialization is partial. */
int x9_gps_nv_prepare(uint64_t generation, const struct x9_gps_nv_ops *);
/* Only on the sole physical worker inside provision/control/MNL/CCIF
 * callbacks. Calls are non-reentrant; normal-world users cannot invoke
 * them. One-shot contention returns EAGAIN, never a fabricated NVLOCK ACK.
 * A valid target header is mandatory. Errors leave length/info unchanged;
 * read bytes may be partial on an authority fault and MUST be discarded.
 * Buffers must be kernel-owned, stable, and not alias mapped NV RAM.
 * Up to4096B per read/write; sparse writes and zero-length IO are rejected.
 * Metadata/version/counters never wrap. Invalid remote metadata fails
 * closed, not a host-header repair or target-header write. */
int x9_gps_nv_get_info(uint64_t, unsigned id, struct x9_gps_nv_info *);
int x9_gps_nv_read(uint64_t, unsigned id, unsigned offset,
    void *, unsigned bytes, unsigned *read_bytes);
int x9_gps_nv_write(uint64_t, unsigned id, unsigned offset,
    const void *, unsigned bytes);
int x9_gps_nv_invalidate(uint64_t, unsigned id);
int x9_gps_nv_set_open(uint64_t, unsigned id, int open);
#ifdef __cplusplus
}
#endif
#endif
