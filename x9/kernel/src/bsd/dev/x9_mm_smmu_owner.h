/* Kernel-only singleton normal MM controller, not an NS ownership oracle.
 * Actual native bootstrap must first own/delegate controller/storage/writers.
 * No SMC/HVC result, saved Android mapping or user-supplied PA is a grant. */
#ifndef X9_MM_SMMU_OWNER_H
#define X9_MM_SMMU_OWNER_H
#ifdef KERNEL
#include "x9_mm_smmu.h"
#include "x9_camera_smmu.h"

struct x9_mm_smmu_owner;
struct x9_mm_smmu_owner_region {
    uintptr_t va;
    uint64_t pa;
    uint32_t bytes, reserved; /* resident contiguous normal WB, 16K aligned */
};
enum x9_mm_smmu_owner_op {
    X9_MM_OWNER_ADOPT=1, X9_MM_OWNER_LEASE, X9_MM_OWNER_INSTALL,
    X9_MM_OWNER_RETIRE, X9_MM_OWNER_CAMERA_INSTALL,
    X9_MM_OWNER_CAMERA_CHECK, X9_MM_OWNER_CAMERA_RETIRE
};
struct x9_mm_smmu_owner_authority {
    void *context;
    /* enter acquires ALL non-owner CPU/firmware table/queue writers, and
     * proves current NS delegation/storage lifetime/cache-line exclusion.
     * guard independently rechecks this grant plus epoch/stopped endpoint.
     * RETIRE additionally requires source/AXI/IRQ/worker retirement.
     * LEASE proves fresh reset epoch and no live reference to reserved
     * SID/ASID. INSTALL proves exclusive fresh CD/tree/backing; the vdec
     * DMA wrapper independently verifies their real PA<->VA correspondence.
     * Camera arguments come only from the camera exported private object.
     * All calls bounded positive errno, no reentry; enter unwinds on error.
     * ADOPT/LEASE have no mapping; neither function manufactures authority.
     * leave releases serialization only, NEVER storage/power/delegation. */
    int (*enter)(void *, uint32_t op, uint32_t sid, uint32_t asid,
        uint64_t epoch, const struct x9_mm_smmu_private_mapping *,
        const struct x9_camera_smmu_export *);
    int (*guard)(void *, uint32_t op, uint32_t sid, uint32_t asid,
        uint64_t epoch, const struct x9_mm_smmu_private_mapping *,
        const struct x9_camera_smmu_export *);
    void (*leave)(void *, uint32_t op, uint32_t sid, uint32_t asid,
        uint64_t epoch, uint32_t state);
};
struct x9_mm_smmu_owner_adoption {
    uint64_t epoch, power_token;
    uint32_t power_client, reserved;
    struct x9_mm_smmu_owner_region strtab, strtab_l2, cmdq, evtq;
    struct x9_mm_smmu_owner_authority authority;
};
/* Once per boot; imports only REAL installed storage, independently pmap
 * checks every resident16K page, snapshots all L1 plus SID0..255, pins power.
 * No global register writes, new queues, table replacement or takeover.
 * Supports linear LOG2SIZE8 or two-level SPLIT8/LOG2SIZE8..20 with actual
 * existing complete first L2. No other L2 is guessed/mapped. Failure never
 * grants a lease. Successful owner retained through this boot; no destroy.
 * Must wire every camera host/other native producer through this singleton;
 * a competing legacy private-controller owner makes admission invalid. */
int x9_mm_smmu_owner_adopt(const struct x9_mm_smmu_owner_adoption *,
    struct x9_mm_smmu_owner **);
/* Only a0/b0; ASID1 stays reserved for camera. Token boot-unique. Nonzero
 * ASID allocated from actual IDR support, scanned against earlier leases;
 * controller authority must exclude external ASID/CD references. These two
 * whole-domain leases are once per boot, NOT per VT session/frame. Retired
 * cookies remain tombstoned; release never regrants them to a new epoch. */
int x9_mm_smmu_owner_lease(struct x9_mm_smmu_owner *, uint32_t sid,
    uint64_t epoch, uint64_t *token, uint32_t *asid);
/* Stable owner-resident cookie; use with x9_vdec_dma_install/retire, NOT
 * directly with caller-selected mapping/backing. No private PA interface. */
int x9_mm_smmu_owner_install_io(struct x9_mm_smmu_owner *, uint64_t token,
    struct x9_mm_smmu_install_io *);
int x9_mm_smmu_owner_release_lease(struct x9_mm_smmu_owner *, uint64_t *token);
/* Camera80/ASID1 enters the SAME singleton/authority/queue lock. Storage
 * mappings match the actual imported controller. Caller still uses camera's
 * own targeted BBM and command engine, never its private whole-table template.
 * Camera's optional stream guard checks foreign STEs against this owner's
 * current ledger and adopts empty completed queue history only at a fresh
 * operation boundary. The legacy sole-producer policy remains unchanged
 * for a host without that callback. This adapter does not install itself
 * into the camera or establish cold NS ownership. */
int x9_mm_smmu_owner_camera_host(struct x9_mm_smmu_owner *, uint64_t epoch,
    struct x9_camera_smmu_host *);
/* For camera's future owner-aware foreign-STE guard: exact retained current
 * descriptor only; caller already holds camera host acquisition. */
int x9_mm_smmu_owner_stream_guard(void *, uint64_t lease, uint32_t sid,
    const uint64_t words[8]);
#endif
#endif
