/* One synchronous this-boot camera FDT collection. No hardware lease. */
#pragma once
#include "fdt.h"

enum { CN_ROOT, CN_SOC, CN_SV, CN_GIC, CN_TOP, CN_EFUSE,
       CN_CSI0, CN_CSI3, CN_CELL0, CN_CELL3, CN_CHOSEN, CN_CHOSEN0,
       CN_COUNT };
enum { CF_IRQ=1u, CF_CALIBRATION=2u };
struct camera_fdt {
    struct fdt_collect c;
    uint32_t handle[3];
    uint8_t owner[3],feature[3];
    unsigned handles;
};
static inline int camera_fdt_need(struct camera_fdt *a,uint32_t handle,unsigned owner,unsigned feature)
{
    if(!handle||handle==UINT32_MAX||a->handles==3||owner>=CN_COUNT||
       !(a->c.present&(1u<<owner)))return 0;
    a->handle[a->handles]=handle;a->owner[a->handles]=(uint8_t)owner;
    a->feature[a->handles++]=(uint8_t)feature;return 1;
}
struct x9_camera_irq_layout;
/* Candidates only; the single second pass must still prove every handle. */
int camera_irq_qualify(struct camera_fdt *,struct x9_camera_irq_layout *);
int camera_calibration_qualify(struct camera_fdt *,uint32_t [3]);
