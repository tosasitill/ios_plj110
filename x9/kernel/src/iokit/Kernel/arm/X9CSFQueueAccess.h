/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9CSFRenderQueue.h"
/* Held for the entire powered firmware session. The engine owns descriptor
 * references/prepares and verifies MMIO mappings against the X9 FDT contract. */
struct X9CSFSharedRange {
 IOBufferMemoryDescriptor *memory;
 uint32_t gpu_va,bytes;
};
struct X9CSFQueueAccess {
 X9CSFSharedRange ranges[64];
 uint32_t range_count;
 X9CSFGroupInterface interface;
 vm_offset_t gpu,kernel_doorbell;
 uint64_t epoch;
 bool owned;
};
bool x9_csf_queue_access_init(X9CSFQueueAccess *,const X9CSFSharedRange *,uint32_t,
 const X9CSFGroupInterface *,vm_offset_t,vm_offset_t,uint64_t);
X9CSFQueueIO x9_csf_queue_access_io(X9CSFQueueAccess *);
/* Called only after the hardware owner confirms MCU stop/AS0 detach. */
void x9_csf_queue_access_invalidate(X9CSFQueueAccess *);
