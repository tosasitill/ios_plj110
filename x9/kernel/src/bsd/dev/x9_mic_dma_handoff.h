#ifndef X9_MIC_DMA_HANDOFF_H
#define X9_MIC_DMA_HANDOFF_H
#include <stdint.h>

/* This boot's LK FDT -> ADT. No power, AIN0 or DMA retirement attestation.
 * The native 34-bit device mask is applied separately by the kernel. */
#define X9_MIC_DMA_HANDOFF_BYTES 64u
#define X9_MIC_DMA_TOPOLOGY_VERIFIED 1u
#define X9_MIC_DMA_DIRECT_BUS 2u
#define X9_MIC_DMA_EXPLICIT_CACHE 4u
#define X9_MIC_DMA_HANDOFF_FLAGS 7u
struct x9_mic_dma_handoff {
    uint32_t version,flags;
    uint64_t cpu_start,dma_start,window_bytes,afe_base,afe_bytes;
    uint32_t reserved[4];
};
#endif
