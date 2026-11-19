/* This-boot MM SMMU combined interrupt topology; not an IRQ ownership grant. */
#ifndef X9_CAMERA_SMMU_IRQ_LAYOUT_H
#define X9_CAMERA_SMMU_IRQ_LAYOUT_H
#include <stdint.h>
#define X9_CAM_SMMU_IRQ_LAYOUT_BYTES 32u
#define X9_CAM_SMMU_IRQ_LK_TOPOLOGY 1u
struct x9_camera_smmu_irq_layout {
    uint32_t version,flags,gicd_base,gicd_bytes;
    uint32_t intid,trigger,irq_cells,reserved;
};
static inline int x9_camera_smmu_irq_layout_valid(const struct x9_camera_smmu_irq_layout *q)
{
    return q && q->version==1 && q->flags==X9_CAM_SMMU_IRQ_LK_TOPOLOGY &&
        q->gicd_base==0x06800000 && q->gicd_bytes==0x40000 &&
        q->intid==0x1df+32 && q->trigger==1 && q->irq_cells==4 && !q->reserved;
}
#endif
