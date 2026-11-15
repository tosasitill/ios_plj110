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
#endif
#endif
