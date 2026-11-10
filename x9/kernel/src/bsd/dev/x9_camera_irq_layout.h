/* This boot's GIC/CAMSV topology only. No NS controller or DMA grant. */
#ifndef X9_CAMERA_IRQ_LAYOUT_H
#define X9_CAMERA_IRQ_LAYOUT_H
#include <stdint.h>
#define X9_CAM_IRQ_LAYOUT_BYTES 64u
#define X9_CAM_IRQ_LK_TOPOLOGY 1u
#define X9_CAM_IRQ_LINES 3u
enum { X9_CAM_IRQ_HYBRID, X9_CAM_IRQ_SOF, X9_CAM_IRQ_DEBUG };
struct x9_camera_irq_layout {
    uint32_t version, flags, gicd_base, gicd_bytes;
    uint32_t outer_base, inner_base, bank_bytes, irq_cells;
    uint32_t intid[X9_CAM_IRQ_LINES], trigger[X9_CAM_IRQ_LINES];
    uint32_t reserved[2];
};
static inline int x9_camera_irq_layout_valid(const struct x9_camera_irq_layout *q)
{
    return q && q->version==1 && q->flags==X9_CAM_IRQ_LK_TOPOLOGY &&
        q->gicd_base==0x06800000 && q->gicd_bytes==0x40000 &&
        q->outer_base==0x3a600000 && q->inner_base==0x3a608000 &&
        q->bank_bytes==0x1000 && q->irq_cells==4 &&
        q->intid[0]==0x26d+32 && q->intid[1]==0x273+32 && q->intid[2]==0x279+32 &&
        q->trigger[0]==4 && q->trigger[1]==4 && q->trigger[2]==4 &&
        !q->reserved[0] && !q->reserved[1];
}
#endif
