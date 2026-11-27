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

/* H2 step b: choose which atoms of a written range the probe samples.
 * Step a only ever looked at the first and last atom, so interior
 * staleness was invisible and the same two atoms were re-measured every
 * job. Here the caller passes a rotation cursor it advances per sampled
 * resource: the picks are base points (k*atoms)/max_atoms, each shifted
 * by the same rotation modulo atoms, so they stay distinct, and over a
 * full rotation every atom of the range is picked equally often. Pure
 * arithmetic on purpose: host-tested in x9/tools/test_x9_coh_pick.py. */
static inline unsigned x9_render_coh_pick_atoms(uint64_t atoms,uint64_t rotation,
 unsigned max_atoms,uint64_t *out) {
 if(!out||!atoms||!max_atoms)return 0;
 if(atoms<=max_atoms) {
  unsigned n=0;
  for(uint64_t i=0;i<atoms;i++)out[n++]=i;
  return n;
 }
 const uint64_t shift=rotation%atoms;
 unsigned n=0;
 for(unsigned k=0;k<max_atoms;k++)out[n++]=((uint64_t)k*atoms/max_atoms+shift)%atoms;
 return n;
}
