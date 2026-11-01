/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9CSFQueueAccess.h"
/* Begin returns2 only for an owned resident MCU session. -1 means retain
 * outer GPUEB refs,0 means clean skip/failure. Called under GPU owner lock. */
extern "C" int sweet_x9_csf_resident_begin(vm_offset_t);
/* End requires all firmware groups stopped/terminated and AS1 detached. */
extern "C" int sweet_x9_csf_resident_end(void);
extern "C" bool sweet_x9_csf_resident_caps(X9RenderCaps *);
/* Reserved AS0 user I/O and suspend pages for one of eight physical slots.
 * The caller must not change allocation ownership or use outside owner lock. */
struct X9CSFResidentSlot {
 X9CSFQueueAccess access;
 uint32_t user_io,suspend;
 bool borrowed;
};
extern "C" X9CSFResidentSlot *sweet_x9_csf_resident_slot(unsigned);
extern "C" bool sweet_x9_csf_resident_release_slot(unsigned,uint64_t epoch);

extern "C" bool sweet_x9_render_owner_enter(void);
extern "C" void sweet_x9_render_owner_leave(void);
extern "C" int sweet_x9_gpu_render_begin(void);
extern "C" void sweet_x9_render_bootstrap(void);
