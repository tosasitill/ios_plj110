/* LK -> XNU ADSP shared-memory contract. Native LE, no CPU pointers.
 * Qualification is metadata ONLY: not EMI permission, power, firmware READY,
 * ownership, or a DMA/cache binding. Semantic slots do not expose a Linux
 * enum whose AUDIO index depends on CONFIG_MTK_USB_OFFLOAD. This contract
 * covers the PLJ110 USB-offload layout, including its XHCI reservation. */
#ifndef X9_ADSP_HANDOFF_H
#define X9_ADSP_HANDOFF_H
#include <stdint.h>
#define X9_ADSP_HANDOFF_BYTES 256u
#define X9_ADSP_LAYOUT_VERIFIED 1u
#define X9_ADSP_MEM_COUNT 15u
enum { X9ADSPMemLoggerA, X9ADSPMemDebugA, X9ADSPMemIPIA, X9ADSPMemDumpA,
       X9ADSPMemMbrainA, X9ADSPMemLoggerB, X9ADSPMemDebugB, X9ADSPMemIPIB,
       X9ADSPMemDumpB, X9ADSPMemMbrainB, X9ADSPMemC2C, X9ADSPMemPCIE,
       X9ADSPMemL2, X9ADSPMemXHCI, X9ADSPMemAudio };
struct x9_adsp_mem_block { uint32_t offset, bytes; };
struct x9_adsp_mbox_layout { uint32_t base, set, clear, reserved; };
struct x9_adsp_handoff {
    uint32_t version, flags, cores, allocated_bytes;
    uint64_t reserve_base, reserve_bytes;
    struct x9_adsp_mem_block block[X9_ADSP_MEM_COUNT];
    uint32_t cfg_base[2], cfg_bytes[2];
    struct x9_adsp_mbox_layout mbox[4];
    uint32_t system_base[2], system_bytes[2], reserved[2];
};
/* Validate the bounded, proved variant independently at both ends. Zero-size
 * source blocks are skipped (not assigned an address), exactly as in OPPO
 * adsp_mem_device_probe. The final allocation must fit the live grant. */
static inline int x9_adsp_handoff_valid(const struct x9_adsp_handoff *h)
{
    const uint32_t sizes[X9_ADSP_MEM_COUNT]={0x80000,0x80000,0x180000,0x400,
        0x400,0x80000,0x80000,0x100000,0x400,0,0x40000,0,0,0x80000,0x852000};
    if(!h)return 0;
    if(h->version!=1 || h->flags!=X9_ADSP_LAYOUT_VERIFIED || h->cores!=2 ||
       h->reserved[0] || h->reserved[1] || h->reserve_base<0x100000000ULL ||
       !h->reserve_bytes || h->reserve_bytes>0x4000000 ||
       ((h->reserve_base|h->reserve_bytes)&0x3fff) ||
       h->reserve_base>0x37fa00000ULL-h->reserve_bytes ||
       h->cfg_base[0]!=0x1a000000 || h->cfg_bytes[0]!=0x5000 ||
       h->cfg_base[1]!=0x1a00a000 || h->cfg_bytes[1]!=0x8000 ||
       h->system_base[0]!=0xe4000000 || h->system_bytes[0]!=0xf00000 ||
       h->system_base[1]!=0xe4f00000 || h->system_bytes[1]!=0x1900000)return 0;
    uint32_t at=0;
    for(unsigned i=0;i<X9_ADSP_MEM_COUNT;i++) {
        if(h->block[i].bytes!=sizes[i])return 0;
        if(!sizes[i]){if(h->block[i].offset)return 0;continue;}
        uint32_t n=(sizes[i]+0xfff)&~0xfffu;
        if(h->block[i].offset!=at || n>h->reserve_bytes-at)return 0;
        at+=n;
    }
    if(h->allocated_bytes!=at)return 0;
    for(unsigned i=0;i<4;i++) {
        uint32_t base=0x1a350000+i*0x10000;
        if(h->mbox[i].base!=base || h->mbox[i].set!=base+0x100 ||
           h->mbox[i].clear!=base+0x10c || h->mbox[i].reserved)return 0;
    }
    return 1;
}
#endif
