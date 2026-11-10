/* Shared shim/XNU PLJ110 SCP layout contract. Native LE32, no pointers.
 * flags describes FDT qualification ONLY, never firmware/power readiness.
 * OPPO rv/scp_helper.c allocates NUMS_MEM_ID=30; DT id30 is skipped.
 * This is not the old CM4 layout and does not authorize MMIO or FIFO access. */
#ifndef X9_SCP_HANDOFF_H
#define X9_SCP_HANDOFF_H
#include <stdint.h>
#define X9_SCP_HANDOFF_BYTES 384u
#define X9_SCP_MEM_COUNT 30u
#define X9_SCP_LAYOUT_VERIFIED 1u
struct x9_scp_mem_region { uint32_t offset, size; };
struct x9_scp_pin { uint32_t channel, offset_bytes, message_bytes, pin_index; };
struct x9_scp_handoff {
 uint32_t version, flags;
 uint32_t reserve_base, reserve_size, dump_size, allocated_size;
 struct x9_scp_mem_region memory[X9_SCP_MEM_COUNT];
 /* OUT29 control, IN30 ACK, OUT31 notify, IN32 notify. */
 struct x9_scp_pin sensor_pin[4];
 uint32_t mbox_base, mbox_set, mbox_clear, mbox_init;
 uint32_t sram_base, sram_size, region_info_op;
 uint32_t reserved[7];
};
#endif
