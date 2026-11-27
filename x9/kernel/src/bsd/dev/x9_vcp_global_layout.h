/* Captured rv_v2 GLOBAL memory layout, separate from service type8's VSI.
 * Profile selection requires the CURRENT LK FDT's complete size table and
 * secure-dump policy to match. This is a layout, not a firmware/DMA grant. */
#ifndef X9_VCP_GLOBAL_LAYOUT_H
#define X9_VCP_GLOBAL_LAYOUT_H
#include <stdint.h>
#define X9_VCP_GLOBAL_LAYOUT_CAPTURE1 1u
#define X9_VCP_GLOBAL_BYTES 0x20e000u
#define X9_VCP_GLOBAL_VDEC_OFFSET 0x3000u
#define X9_VCP_GLOBAL_VDEC_BYTES 0x78000u
#define X9_VCP_GLOBAL_LOGGER_OFFSET 0x8d000u
#define X9_VCP_GLOBAL_LOGGER_BYTES 0x180000u
#define X9_VCP_GLOBAL_PROP_OFFSET 0x2000u
#define X9_VCP_GLOBAL_LOG_INFO_OFFSET 0x2800u
#define X9_VCP_GLOBAL_TEXT_BYTES 0x400u
/* The size lookup uses ARRAY INDEX, not the mblock's .num. Actual remap:
 * 9,8,3,4,5,6,0,1,2,11. Indices8/11 have swapped .num in the captured KO.
 * Index7 absent/zero; index10 secure dump is disabled and zero. No fallback
 * for a changed FDT: teach its paired layout explicitly before using it. */
static const uint32_t x9_vcp_global_mem_table[] = {
    0,0x78000, 1,0x12000, 2,0x180000, 3,0x400, 4,0x400, 5,0x400,
    6,0x400, 8,0x1000, 9,0x1000, 10,0, 11,0x1000
};
#endif
