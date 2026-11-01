/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9RenderPageTable.h"
/* Engine building blocks. Caller holds context->lock and the shared hardware
 * owner lock. These do not boot MCU, claim MMIO, or advertise render readiness. */
IOReturn x9_render_memory_open(X9RenderContext *);
bool x9_render_memory_close(X9RenderContext *);
IOReturn x9_render_memory_map(X9RenderContext *,const X9RenderVMEntry *);
IOReturn x9_render_memory_unmap(X9RenderContext *,const X9RenderVMEntry *);
IOReturn x9_render_memory_cache(X9RenderContext *,X9RenderResource *,const X9RenderCacheRequest *);
uint64_t x9_render_memory_publish(X9RenderContext *);
void x9_render_memory_detached(X9RenderContext *);

/* Kernel-only batch. No user pointer or access declaration is accepted here.
 * Each span belongs to one retained resource, sorted and disjoint. */
struct X9RenderCacheSpan { uint64_t begin,end; unsigned resource; };
IOReturn x9_render_memory_cache_spans(X9RenderContext *,X9RenderResource *,
 const X9RenderCacheSpan *,unsigned,uint32_t,uint64_t *);
