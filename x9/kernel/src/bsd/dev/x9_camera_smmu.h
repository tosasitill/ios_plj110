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
 */
int x9_camera_smmu_check(struct x9_camera_smmu *);
int x9_camera_smmu_cfgi(struct x9_camera_smmu *); /* STE80 + CD80/SSID0 + SYNC */
int x9_camera_smmu_tlbi(struct x9_camera_smmu *); /* NH/VMID0/ASID1 + SYNC */
int x9_camera_smmu_sync(struct x9_camera_smmu *);
#endif
#endif
