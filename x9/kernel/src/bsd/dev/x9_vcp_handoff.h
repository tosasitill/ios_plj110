/* CURRENT LK FDT mailbox topology and global memory layout. Not a power/clock/firmware, reset,
 * IRQ/controller ownership or DMA grant. Kept separate from codec READY. */
#ifndef X9_VCP_HANDOFF_H
#define X9_VCP_HANDOFF_H
#include <stdint.h>
#include "x9_vcp_global_layout.h"
#define X9_VCP_HANDOFF_VERSION 4u
#define X9_VCP_HANDOFF_BYTES 128u
#define X9_VCP_MBOX_COUNT 5u /* VCP0/1/2 then MMUP0/1 */
struct x9_vcp_mbox_layout { uint32_t base, set, clear, irq; };
struct x9_vcp_handoff {
    uint32_t version, topology, cores, sram_bytes;
    struct x9_vcp_mbox_layout mbox[X9_VCP_MBOX_COUNT];
    uint32_t cfg_base[2]; /* VCP core0 then MMUP core1; registers, not SRAM */
    uint32_t twohart[2]; /* CURRENT FDT, independently qualified per core */
    uint32_t global_layout; /* qualified complete CURRENT vcp-mem-tbl, not an IOVA */
    uint32_t vp_multiplier; /* CURRENT decoder DT, not a caller-selected size */
    uint32_t reserved[2];
};
static inline int x9_vcp_handoff_valid(const struct x9_vcp_handoff *h)
{
    if (!h || h->version != X9_VCP_HANDOFF_VERSION || h->topology != 1 || h->cores != 2 ||
        h->sram_bytes != 0x80000) return 0;
    for (unsigned i = 0; i < X9_VCP_MBOX_COUNT; i++) {
        uint32_t base = 0x31b80000u + i * 0x10000u;
        if (h->mbox[i].base != base || h->mbox[i].set != base + 0x100 ||
            h->mbox[i].clear != base + 0x10c || h->mbox[i].irq != 593u + i)
            return 0;
    }
    if (h->cfg_base[0] != 0x31a0a000 || h->cfg_base[1] != 0x31a0d000) return 0;
    /* This captured topology has TWO VCP harts, but only ONE MMUP hart.
     * core-nums=2 counts cores, not the harts within either core. */
    if (h->twohart[0] != 1 || h->twohart[1] != 0) return 0;
    if (h->global_layout != X9_VCP_GLOBAL_LAYOUT_CAPTURE1) return 0;
    if (h->vp_multiplier != 1) return 0;
    for (unsigned i = 0; i < 2; i++) if (h->reserved[i]) return 0;
    return 1;
}
#endif
