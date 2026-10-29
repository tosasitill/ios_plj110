#pragma once
#include <stddef.h>
#include <stdint.h>
/* Pure ADT patch helper and resident-root handoff, shared by host checks. */
int resident_patch_adt(void *, size_t, uint64_t, uint64_t);
const char *resident_layout(const void *, size_t, uint64_t *, uint64_t *);
const char *usb_receiver_boot(uintptr_t, const void *, size_t);

const char *resident_layout_ufs(const void *,size_t,uint64_t *,uint64_t *);
