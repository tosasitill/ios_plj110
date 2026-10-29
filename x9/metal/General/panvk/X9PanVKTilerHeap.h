/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9PanVKMemory.h"
struct X9PanVKTilerHeap;
struct X9PanVKTilerHeapInfo {
 uint64_t context_address,first_chunk_address,allocation_bytes;
 uint32_t chunk_size,chunk_count,handle;
};
/* Bounded preallocated heap. Caller reserves allocation_bytes of GPU VA.
 * No runtime replenishment/shrinker is advertised by this implementation. */
uint64_t x9_panvk_heap_bytes(uint32_t chunk_size,uint32_t chunks);
struct X9PanVKTilerHeap *x9_panvk_heap_create(struct pan_kmod_vm *,uint64_t,uint32_t,uint32_t,struct X9PanVKTilerHeapInfo *);
int x9_panvk_heap_destroy(struct X9PanVKTilerHeap *);
