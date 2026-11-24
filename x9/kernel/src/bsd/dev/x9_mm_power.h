/* Native shared MT6993 MM_2 HWCCF references. Kernel-private tokens are not
 * VCP reset epochs, normal SID delegation, clock/DMA ownership or READY. */
#ifndef X9_MM_POWER_H
#define X9_MM_POWER_H
#ifdef KERNEL
#include <stdint.h>
#define X9_MM_POWER_CAMERA 1u
#define X9_MM_POWER_VCP 2u
/* Outputs must be writable/disjoint host storage, token initially zero.
 * The first parent adds AP BACKUP1 bit3 if absent, or borrows its existing
 * vote. Subsequent parents share it until the last parent releases.
 * Only our added bit is cleared, never a borrowed vote or another XPU.
 * smc_a0 is a legacy ABI field, always zero: this backend issues no SMC.
 * A failed SET returns a NONZERO token when the physical mutation was issued;
 * keep it/dependencies as a faulted journal, never retry or compensate.
 * Exhaustion/invalid arguments produce no vote. No init-time acquisition. */
int x9_mm_power_acquire(uint32_t client, uint64_t *token, uint32_t *smc_a0);
/* Checks the live software reference, AP/global MM_2 vote, idle BACKUP1_STA,
 * DT MM_2 two-bit power ACK and MM_INFRA ACK/STA twice before MM bus access.
 * Reads only HWV and the AO power-status page. This does not
 * prove the target's clock, endpoint/reset epoch or normal-memory authority. */
int x9_mm_power_validate(uint64_t token, uint32_t client);
/* A distinct child token pins the parent against unvote through exposed DMA,
 * installed contexts/queues or quarantine. Only the owning backend may drop
 * it after its authoritative retirement/drain. No caller-supplied counter.
 * Parent and child tokens are boot-unique; stale/double drops fail. */
int x9_mm_power_pin(uint64_t parent, uint32_t client, uint64_t *child);
int x9_mm_power_validate_pin(uint64_t parent, uint64_t child, uint32_t client);
int x9_mm_power_unpin(uint64_t parent, uint64_t *child, uint32_t client);
/* Requires no children; the LAST parent removes only our added AP bit.
 * Failed SET/CLR readback or completion retains the token/dependencies and
 * faults further physical operations this boot; no automatic replay.
 * Borrowed AP votes and all other XPUs remain intact. A last unvote need
 * not turn global power off, since other XPUs may still need it.
 * Retention after an attempted CLR does NOT prove hardware remains powered.
 * Power ACK/idle never prove DMA drain or normal SMMU ownership.
 * Success clears this caller's software token, not other suppliers. */
int x9_mm_power_release(uint64_t *token, uint32_t client, uint32_t *smc_a0);
/* Android mtk_smmu_power_get -> mtk_smi_mminfra_get_if_in_use equivalent for
 * the MM SMMU (mt6993_data_mm has no SMMU_CLK_AO_EN). MM_2 is the DT
 * mminfra-all-on-pwr-idx; requires it live plus all four MM_INFRA PM ACKs
 * (mtk-pd-chk-mt6993 mmpc 9c/a0/a4/a8). Adds no domain votes (log 385). */
int x9_mm_power_smmu_get(uint64_t token, uint32_t client);
int x9_mm_power_smmu_put(uint64_t token, uint32_t client);
/* Lock-free ACK gate for every MM SMMU MMIO access (AO pages only). */
bool x9_mm_power_smmu_live(void);
void x9_mm_power_smmu_dump(const char *tag);
#endif
#endif
