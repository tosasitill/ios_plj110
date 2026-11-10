#ifndef X9_PA_RESET_H
#define X9_PA_RESET_H
#include <stdint.h>
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#define X9_PA_RESET_POLICY_VERSION 1u
struct x9_pa_reset_policy {
    uint32_t version,generation;
    uint64_t ticket;
    void *context;
    /* Mandatory actual PINNED dual-side rail/reset resource ownership proof;
     * this physical lease outlives the PA child and excludes power-off or
     * foreign reset writers across the whole pulse/I2C lifetime, not just
     * an instantaneous voltage/DO snapshot. The current
     * FDT describes only LEFT VDD; neither it, PA ACK, regulator dummy or
     * a caller's boolean can qualify RIGHT supply. No default supplier.
     * Runs OUTSIDE the common PMIF mutex. No reset/pad/AFE/clock reentry. */
    int (*validate_power)(void *,uint64_t ticket,uint32_t generation);
    /* Independently prove DL24 has no outstanding access, not cached EN0.
     * Runs after actual producer halt; no reset/pad/AFE/clock reentry. */
    int (*validate_drained)(void *,uint64_t ticket,uint32_t generation,
                            uint64_t afe_bus,uint32_t bytes);
};
/* Current-FDT SID3 GPIO10/11 active-low reset, under production clock PA
 * child (distinct from pads). Must start with digital SW mode0 outputs LOW
 * and physical DI LOW; refuses inherited deasserted/high or alternate owner.
 * Real 1ms LOW -> 1ms HIGH plus fresh DO/DI proof mints a reset epoch.
 * This is not revision/profile/probus calibration/unmute authorization.
 * Any failure retains pending PMIF operation/journal/child until explicit
 * halted+drained cleanup. No low/high pulse replay on failed acquisition.
 * Backend serializes lifecycle and must close/drain its I2C6 child BEFORE
 * release. Native I2C6 consumes the downstream child; actual physical
 * rail/controller suppliers and a combined backend remain unregistered.
 * Lock: AFE/pads -> I2C6 -> PA reset -> power callback -> PMIF -> clock/board/AP.
 * Read-only validate may be used by the PA mute/I2C provider; do not call
 * it while holding the clock mutex or common PMIF mutex. */
int x9_pa_reset_acquire(const struct x9_pa_reset_policy *,uint64_t *epoch);
int x9_pa_reset_validate(uint64_t ticket,uint64_t epoch);
int x9_pa_reset_release(uint64_t ticket);
int x9_pa_reset_held(uint64_t ticket); /* cached, not fresh-reset proof */
/* Native I2C6 downstream child. Hold needs fresh no-error Online/reset
 * epoch; validate permits physical fault cleanup. The controller drops
 * ONLY after its own authoritative idle/FIFO/DMA and CSR restore proofs,
 * while actual DSP halt/DL24 drain and this PA parent's power are proved.
 * An open child makes PA release EBUSY BEFORE any reset/power mutation. */
int x9_pa_reset_bus_hold(uint64_t ticket,uint64_t epoch,uint64_t *lease);
int x9_pa_reset_bus_validate(uint64_t ticket,uint64_t epoch,uint64_t lease);
/* Same physical parent + actual halt/independent DL24 drain, WITHOUT
 * dropping the child. I2C6 must call this BEFORE resolving/restoring CSRs,
 * then bus_drop repeats it AFTER controller cleanup. No I2C mutation. */
int x9_pa_reset_bus_quiesced_validate(uint64_t ticket,uint64_t epoch,uint64_t lease);
int x9_pa_reset_bus_drop(uint64_t ticket,uint64_t epoch,uint64_t lease);
/* Explicit halted/drained cleanup of THIS owner's late PMIF operation.
 * May run while I2C6 child is retained; no GPIO/power/bus writes or reset
 * pulse replay. It cannot clear another user's valid response. */
int x9_pa_reset_resolve_pmif(uint64_t ticket);
#ifdef __cplusplus
}
#endif
#endif
#endif
