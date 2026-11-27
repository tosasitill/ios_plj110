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
IOReturn x9_render_memory_reassert_wc(X9RenderContext *,uint64_t *,uint64_t *);

/* Kernel-only batch. No user pointer or access declaration is accepted here.
 * Each span belongs to one retained resource, sorted and disjoint. */
struct X9RenderCacheSpan { uint64_t begin,end; unsigned resource; };
IOReturn x9_render_memory_cache_spans(X9RenderContext *,X9RenderResource *,
 const X9RenderCacheSpan *,unsigned,uint32_t,uint64_t *);

/* H2 coherency probe (log only, opt-in x9_coh_probe=1): compare plain cached
 * loads of one atom-aligned X9_RENDER_CACHE_ATOM range against the same loads
 * repeated after a CIVAC of that atom. Counts matching/stale qwords into
 * *match/*mismatch. Returns false when the atom cannot be addressed (the
 * caller counts nothing either way). Never skips maintenance: the sampled
 * atom leaves CIVAC-maintained exactly as the non-coherent path leaves it. */
bool x9_render_memory_coherency_sample(X9RenderResource *,uint64_t,uint64_t *,uint64_t *);
