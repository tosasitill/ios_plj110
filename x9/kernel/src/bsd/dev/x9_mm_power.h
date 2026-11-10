/* Native shared MT6993 MM power references. Kernel-private tokens are not
 * VCP reset epochs, normal SID delegation, clock/DMA ownership or READY. */
#ifndef X9_MM_POWER_H
#define X9_MM_POWER_H
#ifdef KERNEL
#include <stdint.h>
#define X9_MM_POWER_CAMERA 1u
#define X9_MM_POWER_VCP 2u
/* Outputs must be writable/disjoint host storage, token initially zero.
 * The first parent owns a real secure GET, not an inherited Android reference;
 * subsequent parents share that reference until the last parent releases.
 * No GET/PUT is issued while another parent's reference/child remains live.
 * smc_a0 is zero for a software-only shared take/drop, not proof of an SMC.
 * Nonzero low32 a0 is refused without a compensating PUT.
 * Exhaustion/invalid arguments produce no SMC. No automatic/init-time GET. */
int x9_mm_power_acquire(uint32_t client, uint64_t *token, uint32_t *smc_a0);
/* Checks the live software reference and current HWV MM_INFRA ACK/STA plus
 * two idle MM-SMMU BACKUP1_STA bit3 observations before
 * MM bus access; never reads SMMU/MMUP with a missing reference. This does not
 * prove the target's clock, endpoint/reset epoch or normal-memory authority. */
int x9_mm_power_validate(uint64_t token, uint32_t client);
/* A distinct child token pins the parent against PUT through exposed DMA,
 * installed contexts/queues or quarantine. Only the owning backend may drop
 * it after its authoritative retirement/drain. No caller-supplied counter.
 * Parent and child tokens are boot-unique; stale/double drops fail. */
int x9_mm_power_pin(uint64_t parent, uint32_t client, uint64_t *child);
int x9_mm_power_validate_pin(uint64_t parent, uint64_t child, uint32_t client);
int x9_mm_power_unpin(uint64_t parent, uint64_t *child, uint32_t client);
/* Requires no children; the LAST parent issues one secure PUT. A nonzero PUT
 * is ambiguous: retain the token/dependencies, inhibit further power SMCs
 * this boot and never retry it. The current ATF can discard its internal
 * HWV error and still return0. Therefore the last PUT also needs two fresh
 * idle BACKUP1_STA bit3 reads; any busy sample retains the caller's journal
 * and faults this boot, even with smc_a0=0. No further GET/PUT or polling.
 * Retention after an attempted PUT is NOT proof that firmware retained its
 * old count or that hardware is still powered. An idle pair alone proves
 * neither the desired enable state, DMA drain nor normal SMMU ownership.
 * Success clears this caller's software token, not other suppliers. */
int x9_mm_power_release(uint64_t *token, uint32_t client, uint32_t *smc_a0);
#endif
#endif
