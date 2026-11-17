/* This-boot LK FDT policy only. Not MM power, a SID reservation or DMA grant. */
#ifndef X9_CAMERA_SMMU_POLICY_H
#define X9_CAMERA_SMMU_POLICY_H
#include <stdint.h>
#define X9_CAM_SMMU_POLICY_BYTES 32u
#define X9_CAM_SMMU_POLICY_LK_FDT 1u
struct x9_camera_smmu_policy {
    uint32_t version,flags,tcu_prefetch,sid;
    uint32_t normal_base,normal_bytes,wrapper_base,wrapper_bytes;
};
static inline int x9_camera_smmu_policy_valid(const struct x9_camera_smmu_policy *p)
{
    /* Only the actual MT6993 MM/SID80 profile has been traced. A changed
     * compatible, ATS/DCP/coherent profile or register layout needs its
     * own source audit; an unqualified zero placeholder never defaults. */
    return p && p->version==1 && p->flags==X9_CAM_SMMU_POLICY_LK_FDT &&
        p->tcu_prefetch==2 && p->sid==0x80 &&
        p->normal_base==0x30800000 && p->normal_bytes==0x1e0000 &&
        p->wrapper_base==0x309e0000 && p->wrapper_bytes==0x10000;
}
#endif
