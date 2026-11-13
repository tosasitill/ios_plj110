/* Shared state of the pre-XNU audio qualification (audio_handoff.c).
 * One fdt_collect table for every audio consumer; phandles are registered
 * per feature and verified together in the single second pass. */
#pragma once
#include "fdt.h"

enum { AN_ROOT, AN_SOC, AN_ADSPSYS, AN_CORE0, AN_CORE1, AN_GIC,
       AN_PINCTRL, AN_AFE, AN_SOUND, AN_IN4, AN_IN4_1, AN_IN4_2, AN_IN4_3,
       AN_OUT4, AN_OUT4_1, AN_SPMI, AN_PMIC3, AN_PMIC3_PC, AN_I2C6,
       AN_PA_L, AN_PA_R, AN_GPIO_L, AN_GPIO_L_PIN, AN_GPIO_R, AN_GPIO_R_PIN,
       AN_PMIC5, AN_REG5, AN_LNC08, AN_COUNT };
/* Feature bits whose qualification depends on phandle uniqueness. */
enum { AF_IRQ=1u, AF_I2S4=2u, AF_PA_RESET=4u, AF_LEFT_VDD=8u };

struct audio_fdt {
    struct fdt_collect c;
    uint32_t handle[FDT_COLLECT_MAX_HANDLES];
    uint8_t owner[FDT_COLLECT_MAX_HANDLES],feature[FDT_COLLECT_MAX_HANDLES];
    unsigned handles;
};
static inline int audio_fdt_need(struct audio_fdt *a,uint32_t handle,unsigned owner,unsigned feature)
{
    if(!handle||handle==UINT32_MAX||a->handles==FDT_COLLECT_MAX_HANDLES)return 0;
    a->handle[a->handles]=handle;a->owner[a->handles]=(uint8_t)owner;
    a->feature[a->handles++]=(uint8_t)feature;return 1;
}
/* adsp_handoff.c: fills h/q; *features = AF_* bits that passed pass-1 checks
 * (final grant after pass 2). Returns 1 when the shared layout qualified. */
struct x9_adsp_handoff;struct x9_adsp_audio_irq_layout;
int adsp_qualify(struct audio_fdt *,struct x9_adsp_handoff *,struct x9_adsp_audio_irq_layout *,unsigned *);
/* mic_dma_handoff.c: 1 and slot filled when this boot's DMA topology qualified. */
int mic_dma_qualify(const struct audio_fdt *,uint8_t *slot);
