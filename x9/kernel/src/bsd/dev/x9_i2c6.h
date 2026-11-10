#ifndef X9_I2C6_H
#define X9_I2C6_H
#include <stdint.h>
#define X9_I2C6_STATUS_VERSION 1u
enum { X9I2C6Held=1u,X9I2C6Captured=2u,X9I2C6Configured=4u,
       X9I2C6Teardown=8u,X9I2C6Restored=16u };
typedef struct {
    uint32_t version,flags,generation,error;
    uint32_t dirty,uncertain,pending,terminal;
    uint32_t parent,dma_parent,nominal_hz,timing_hz; /* rate UPPER bounds, not measured */
    uint32_t slave,reg,reading,tx_bytes;
    uint64_t requests,completed,write_attempts;
    uint32_t bank_start[2],bank_status[2],bank_fifo[2];
    uint32_t reserved[4];
} X9AudioI2C6Status; /* 128B cached only; no bus/reset/Ready/whitelist grant */
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#define X9_I2C6_POLICY_VERSION 1u
struct x9_i2c6_policy {
    uint32_t version,generation;
    uint64_t ticket,reset_epoch;
    void *context;
    /* Mandatory actual PINNED controller authority: current-FDT GPIO114/115
     * mode1 routing/electrical ownership, I2C6 + APDMA physical clocks and
     * stable mux/PLL parents, both CSR banks and DMA, including actual
     * no-outstanding APDMA proof (not merely EN0), excluding ALL other
     * local/remote masters for the entire lease (including failed cleanup).
     * A one-time idle/gate snapshot or diagnostic flags cannot supply this.
     * Already held before acquire; released by caller AFTER bus release.
     * Bounded, no I2C6/PA/pads/AFE/clock lifecycle reentry. PA rail/reset
     * lifetime is independently pinned by the real PA parent's supplier. */
    int (*validate_controller)(void *,uint64_t ticket,uint32_t generation);
};
/* Native FIFO v2, fixed slaves34/35 only, no DMA, IRQ, reset, clock/pad or
 * rail writes. Checked <=98kHz timing, tHIGH/tLOW/EXT rational bounds and
 * one-round-up PLL clock bound; not a 400kHz/electrical qualification.
 * Requires native PA epoch/child and current handoff controller topology.
 * Output lease is NONZERO once the child is held, INCLUDING later failure;
 * that token authorizes explicit cleanup, NOT access/Ready/profile writes.
 * Holds the shared controller exclusion across calls, not just transfers. */
int x9_i2c6_acquire(const struct x9_i2c6_policy *,uint64_t *lease);
int x9_i2c6_validate(uint64_t ticket,uint64_t lease);
int x9_i2c6_read16(uint64_t ticket,uint64_t lease,uint8_t slave,uint8_t reg,uint16_t *);
int x9_i2c6_write16(uint64_t ticket,uint64_t lease,uint8_t slave,uint8_t reg,uint16_t);
/* Neither API is a trusted revision/profile/whitelist or unmute grant.
 * The immutable kernel TFA policy must authorize every value write above
 * this transport. Definite completed NACK may still allow a separate safety
 * mute transaction; timeout/unknown effects block ALL further transfers.
 * Historical errors are never erased by another transfer or validation.
 * Cleanup needs actual producer halt + PA parent's independent DL24 drain.
 * Only THIS terminal request is cleared; no START/DATA/reset replay. Idle
 * alone cannot resolve a START that might still produce a future response.
 * Once release starts, no new transfer even if cleanup is incomplete.
 * Lock: AFE/pads -> I2C6 -> PA -> power -> PMIF -> clock/board/AP leaf. */
int x9_i2c6_release(uint64_t ticket,uint64_t lease);
int x9_i2c6_status(X9AudioI2C6Status *); /* copy only; never validate/MMIO/recover */
#ifdef __cplusplus
}
#endif
#endif
#endif
