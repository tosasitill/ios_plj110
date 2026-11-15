/* Cold XNU-owned normal MM-SMMUv3 host; kernel-only, no user PA API. */
#ifndef X9_CAMERA_NATIVE_SMMU_H
#define X9_CAMERA_NATIVE_SMMU_H
#ifdef KERNEL
#include "x9_camera_smmu.h"
/* Caller already holds actual CAMERA MM power and stopped source/receiver.
 * Uses only an unconfigured normal bank, never an inherited table or HYP
 * queue. Allocates a complete linear table for actual SIDSIZE8..16, keeping
 * every non-camera stream's prior disabled-bank abort/bypass policy.
 * All controller writes are serialized with the native command engine.
 * Firmware redirection/config/producers and power changes fail live checks;
 * software lock does not assert that an unknown firmware agent is excluded.
 * Once backing is exported, *out remains pinned even if initialization fails.
 * Caller MUST retain the real MM power token on such an error. No reset,
 * SEC_INIT, borrowed mpool, MMIO-as-RAM mapping or partial-success READY.
 */
int x9_camera_native_smmu_start(uint64_t power_token,uint64_t iova,
    uint32_t bytes,struct x9_camera_smmu **out,struct x9_camera_smmu_export *view);
int x9_camera_native_smmu_check(struct x9_camera_smmu *);
/* AFTER the real IRQ stop has completed source OFF, spent credit and CPU/
 * worker/GIC receipts. Re-proves actual A0 producer/downstream bus idle,
 * SID80 BBM/CFGI/CD/TLBI/SYNC and complete cold normal-bank restoration.
 * All exported allocations stay pinned. No partial-source-start shortcut.
 * Public wrapper takes control; native internal caller already holds it.
 */
int x9_camera_native_smmu_retire(uint64_t epoch,struct x9_camera_smmu *);
/* Independently checked old-request teardown scope after complete restore,
 * NEVER a live DMA/arm permission. Actual MM power stays held across it. */
int x9_camera_native_smmu_retirement_check(uint64_t epoch,struct x9_camera_smmu *);
#endif
#endif
