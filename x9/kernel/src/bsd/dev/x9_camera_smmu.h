/* Kernel-private normal-camera SMMUv3 backing/command engine.
 * No ioctl, borrowed queue, implicit boot takeover, or READY publication.
 */
#ifndef X9_CAMERA_SMMU_H
#define X9_CAMERA_SMMU_H
#ifdef KERNEL
#include "x9_camera_s1.h"
struct x9_camera_smmu;
struct x9_camera_smmu_export {
    struct x9_camera_s1_export map;
    uint64_t strtab_base, cmdq_base, evtq_base;
    uint32_t strtab_cfg, cr1, cr2, cmdq_shift, evtq_shift, asid;
};
/* One private controller namespace per boot, SID80/SSID0/ASID1. Capability
 * inputs must come from the powered native host; they are not a grant.
 * Wired RAW+CD+STE+CMDQ+EVTQ are allocated here, NOT borrowed from LK.
 * Template: SID80 translates, SID90 bypass, all other STEs abort. The
 * bootstrap MUST establish native NS authority and prove preservation of
 * the prior display/other-stream policy before installing this template.
 * This API neither does that nor writes STRTAB/CR0/GBPA/secure registers.
 * Caller owns handle lifetime and pins power across commands; thread only.
 */
int x9_camera_smmu_prepare(uint32_t idr0,uint32_t idr1,uint32_t idr5,
    uint64_t iova,uint32_t bytes,struct x9_camera_smmu **);
/* Once-only memory-address export. ALL storage retained before any address
 * escapes; no hardware attach/lease is implied. A failed host initialization
 * after export must retain allocations and shared power dependencies.
 */
int x9_camera_smmu_export(struct x9_camera_smmu *,struct x9_camera_smmu_export *);
int x9_camera_smmu_discard(struct x9_camera_smmu **); /* unpublished only */
/* These functions require the real native host to have installed this
 * object's own bases and exact configuration. No arbitrary opcode, SID,
 * ASID, HVC or CPU-PA-as-IOVA interface. Checks are necessary readbacks,
 * not themselves authority or proof of DMA/worker drain. Unknown changes,
 * queue/global errors, EVTQ events and command timeout are terminal.
 * Each submitted batch (including its trailing CS_NONE SYNC) is checked
 * against the private CPU queue record before publication/consumption.
 * On timeout, the SAME owner-pinned call performs one bounded, read-only
 * close observation: two actual full-target CONS samples plus exact own
 * storage/config/PROD/record are required to retire only that pending batch.
 * The original timeout stays terminal even if late consumption is proven;
 * no further command/retry or release of backing/power follows. This is
 * not RAW/AXI/IRQ/worker drain or permission to unmap/restore the controller.
 */
int x9_camera_smmu_check(struct x9_camera_smmu *);
int x9_camera_smmu_cfgi(struct x9_camera_smmu *); /* STE80 + CD80/SSID0 + SYNC */
int x9_camera_smmu_tlbi(struct x9_camera_smmu *); /* NH/VMID0/ASID1 + SYNC */
int x9_camera_smmu_sync(struct x9_camera_smmu *);

/* Prior-preserving live NS attachment. This path NEVER installs the private
 * whole-controller template or changes STRTAB/CMDQ/EVTQ/CR0/GBPA registers.
 * Supports linear LOG2SIZE8 or the normal two-level SPLIT8 layout with
 * LOG2SIZE8..20 and an already-populated full-span L2 for SID0..255.
 * CMDQ LOG2SIZE2..19, bounded by the actual IDR1.CMDQS, not by the private
 * template's one-page queue. Never allocates/publishes a missing L1 descriptor.
 * Mappings are provider-pinned, current coherent normal CPU mappings of the
 * ACTUAL installed storage, not old cached snapshots; page PA correspondence
 * is independently checked, not a PA cast. Whole cache lines touched by PoC
 * clean must have no adjacent CPU writer outside the acquired serialization.
 * enter obtains exclusive table/queue serialization across all producers,
 * including firmware, and proves NS permission, leased SID80/SSID0/ASID1,
 * unreferenced private backing/CD and exact cache policy. No other stream
 * may reference this ASID/CD/tree. For two-level tables, guard also proves
 * that unrelated L2 contents remain unchanged: this engine directly checks
 * all L1 descriptors and all STEs in the selected L2, not unmapped L2s.
 * INSTALL requires stopped endpoints and
 * retired prior DMA/history; RETIRE additionally proves full source/AXI/IRQ/
 * worker retirement. CHECK permits this stream's live DMA but no competing
 * table/queue writer. guard revalidates these facts before every mutation
 * and completion. Neither equal readbacks nor idle registers are a grant.
 * Callbacks are mandatory, bounded, positive-errno returning, never reenter
 * camera APIs, and keep their context/backing stable across the entire call.
 * enter unwinds itself on error; leave releases serialization only, NEVER
 * backing, mappings, supplier power or the lease. No userland PA interface.
 */
enum x9_camera_smmu_host_op {
    X9_CAM_SMMU_INSTALL=1, X9_CAM_SMMU_CHECK, X9_CAM_SMMU_RETIRE
};
struct x9_camera_smmu_host_region {
    uintptr_t va;
    uint64_t pa;
    uint32_t bytes, reserved; /* 16K aligned and physically contiguous */
};
#define X9_CAM_SMMU_HOST_L1_MAX 4096u /* at most32K of installed L1 descriptors */
#define X9_CAM_SMMU_HOST_CMDQ_SHIFT 19u /* at most8MiB of actual queue storage */
struct x9_camera_smmu_host {
    void *context;
    uint64_t lease;
    /* strtab/L2 regions at most32K; cmdq at most16<<CMDQ_SHIFT bytes.
     * The complete configured structures must fit, not a small prefix. */
    struct x9_camera_smmu_host_region strtab,cmdq;
    /* Zero for linear. For two-level, strtab pins the complete L1 and
     * strtab_l2 pins the actual first L2 separately. No physical aliases
     * with any other L1 target, queues or private backing are accepted. */
    struct x9_camera_smmu_host_region strtab_l2;
    int (*enter)(void *,uint64_t,unsigned,const struct x9_camera_smmu_export *);
    int (*guard)(void *,uint64_t,unsigned,const struct x9_camera_smmu_export *);
    void (*leave)(void *,uint64_t,unsigned);
};
/* Once-only attach accepts ONLY zero-invalid or canonical ABORT STE80.
 * It snapshots/preserves every other STE in SID0..255, including
 * display90/protected89, the full installed L1 when present and all live
 * controller registers, then uses atomic64 BBM/PoC clean and
 * exact inherited-queue CFGI80/CD80/TLBI-ASID1/SYNC consumption. One 100ms
 * total operation deadline, no reset/retry/whole-table copy or stale-dump fallback.
 * A timed-out published batch gets at most one additional10ms read-only
 * close observation inside the SAME acquired host serialization, before
 * fault latching/leave. The actual original queue record, full live guard
 * and two fresh full-target CONS samples alone may retire that batch's
 * pending ledger. The original ETIMEDOUT remains terminal: never resume
 * BBM, install/restore further STE words, report INSTALLED/RETIRED, detach,
 * release mappings/backing/suppliers or infer full DMA/AXI retirement.
 * Shared checking uses the retained host, never the private own-base engine.
 * Retire restores only original STE80 after exclusive owner-proven drain
 * and targeted invalidation/SYNC; it does not free exported allocations.
 * Any uncertain exposed-path failure is terminal, retaining all dependencies.
 * There is intentionally no boolean 'drained' or success-as-ACK callback.
 */
int x9_camera_smmu_attach(struct x9_camera_smmu *,const struct x9_camera_smmu_host *);
int x9_camera_smmu_attached_check(struct x9_camera_smmu *);
int x9_camera_smmu_detach(struct x9_camera_smmu *);
#endif
#endif
