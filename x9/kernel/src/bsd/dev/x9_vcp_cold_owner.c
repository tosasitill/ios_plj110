/* Cold-start VCP/MMUP bootstrap owner: skeleton, NOT bound. See the header
 * and docs/hevc-cold-owner-todo-20261127.md. Every callback fails closed;
 * none of them touches a register, SMC, clock or IRQ. The reset-release
 * SMC itself (0xC200052C op6 MMUP, then op3 VCP) is native bus code
 * (x9_vcp_bus.c) and is NOT the owner's job; the owner's job is the
 * physical ownership the bus cannot prove for itself. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include "x9_vcp_bus.h"
#include "x9_vcp_cold_owner.h"

#ifdef KERNEL
/* guard must prove, for this cold session: exclusive VCP/MMUP
 * clock/reset/IRQ+mailbox ownership, firmware signature/residency pairing,
 * and the installed SHM mappings/cache. None of these exist yet:
 *  - no XNU-side VCP clock/reset/IRQ ownership acquisition exists in tree
 *    (x9_mm_power provides the MM power pin the bus validates separately;
 *    the bus header states it cannot substitute for these prerequisites);
 *  - residency pairing is a per-boot fact the P0 read-only probe
 *    (x9_vcp_probe.c) classifies A/B/C; it has never run on hardware;
 *  - the SHM ledger only exists once a real owner installs the shared /
 *    logger extents the bind phase validates.
 * Until all three are real, guard cannot return 0. */
static int
x9_vcp_cold_guard(void *cookie, uint64_t epoch, uint32_t phase)
{
    (void)cookie; (void)epoch; (void)phase;
    return ENOTSUP;
}

/* halt_and_drain must stop both physical producers and every upper-layer
 * DMA/worker user, then KEEP both producers halted. The VCP_SET_HALT
 * equivalent path is explicitly unevidenced: design doc section 5.4
 * requires tinysys/ATF evidence before implementation, and the Android
 * mmdvfs pm_disable path only decrements a count (no IRQ disable, halt
 * or clock clear), so it cannot serve as a drain. Do not implement from
 * the SMC reset ops: release is not halt. */
static int
x9_vcp_cold_halt_and_drain(void *cookie, uint64_t epoch)
{
    (void)cookie; (void)epoch;
    return ENOTSUP;
}

/* resources_retired is a read-only, non-waiting check that every
 * epoch-dependent DMA buffer, mapping and worker has retired. The bus
 * takes its own native cfg+70 double samples, but the header states
 * those do NOT prove upper-layer drain; the owner's half is its resource
 * ledger, which does not exist until guard/bind are real. Returning 0
 * from an empty-by-construction ledger would become a false proof the
 * moment any partial owner installs resources, so this fails closed. */
static int
x9_vcp_cold_resources_retired(void *cookie, uint64_t epoch)
{
    (void)cookie; (void)epoch;
    return ENOTSUP;
}

const struct x9_vcp_bus_owner x9_vcp_cold_owner = {
    .cookie = 0,
    .power_token = 0,
    .guard = x9_vcp_cold_guard,
    .halt_and_drain = x9_vcp_cold_halt_and_drain,
    .resources_retired = x9_vcp_cold_resources_retired,
};
#endif
