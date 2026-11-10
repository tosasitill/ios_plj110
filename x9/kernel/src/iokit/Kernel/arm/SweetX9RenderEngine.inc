/* SPDX-License-Identifier: MIT */
/* Included after SweetX9CSFResident.inc, inside SweetX9CSF's namespace. */
#include "x9_render_cs.inc"
constexpr unsigned render_ring_bytes=16384,render_storage_bytes=65536,render_packet_bytes=32;
constexpr unsigned render_body_bytes=32*sizeof(uint64_t),render_history=64;
struct RenderKernelGroup {
 X9RenderGroup request;
 X9CSFRenderGroup control;
 X9RenderResource storage[3];
 bool mapped[3];
 uint64_t producer[3];
 unsigned slot;
 /* x9cs (CSG slot steal): GPU VA index of the private storage (the slot
  * index without x9cs, a per-context entry index with it, so the ring's
  * absolute CALL addresses survive a move to another slot). bound=false: the
  * group holds no hardware slot; its CSG state (if it was ever started) is in
  * saved[] and comes back through RESUME on whatever slot it gets next. */
 unsigned va_index;
 bool bound,saved_valid;
 X9RenderContext *ctx;
 uint64_t last_use;
 uint8_t *saved;uint32_t saved_bytes;
};
#include "X9RenderCachePlan.inc"
/* rev252cn (x9dc): deferred client clean ranges (see render_dirty_stage below). */
#define X9_RENDER_DIRTY_MAX 256u
struct RenderDirtyEntry { uint32_t handle; uint64_t offset,bytes; };
#define X9_RENDER_DIRTY_SLOTS 2u
static bool render_dirty_enabled();
struct RenderKernelContext {
 RenderCachePlan cache_plan;
 bool cache_plan_valid;
 unsigned cache_plan_mappings;
 /* Deferred client clean ranges (x9dc). dirty_lock guards the record list and
  * the pending slots; it is a leaf lock taken without c->lock or the owner
  * lock (the record path holds neither). The submit path moves the record
  * list into the pending slot for its submission id under c->lock; publish
  * consumes that slot. */
 IOLock *dirty_lock;
 struct RenderDirtyEntry dirty[X9_RENDER_DIRTY_MAX];
 unsigned dirty_count;bool dirty_overflow;
 struct RenderDirtyEntry dirty_pending[X9_RENDER_DIRTY_SLOTS][X9_RENDER_DIRTY_MAX];
 unsigned dirty_pending_count[X9_RENDER_DIRTY_SLOTS];
 bool dirty_pending_overflow[X9_RENDER_DIRTY_SLOTS];
 RenderKernelGroup *groups[8];
 X9RenderCompletion history[render_history];
 unsigned next_history;
 uint32_t next_group;
 /* Async submission state, under render_async_lock only (see below). */
 struct AsyncDone { uint32_t group; IOReturn kr; X9RenderCompletion reply; } async_done[render_history];
 unsigned async_next,async_pending;
 uint64_t async_done_id;
 bool async_failed;
};
static bool render_engine_installed;
static bool render_async_enabled();                          /* async section below */
static void render_async_drain_unlocked(RenderKernelContext *);
struct RenderOwner {
 bool locked;
 RenderOwner():locked(sweet_x9_render_owner_enter()){}
 ~RenderOwner(){if(locked)sweet_x9_render_owner_leave();}
};
static bool render_ready() {
 return render_engine_installed&&resident&&resident->ready&&!resident->uncertain&&no_faults(resident->gpu);
}
/*
 * Resident ("parked") render group, opt-in x9_gpu_park=1 (rev252ap).
 * Before: every submit did flush+install_as(AS1)+CSG START/RESUME, then CSG
 * SUSPEND+flush+remove_as(AS1). Stock kbase keeps a group resident on its CSG
 * slot with its AS bound and only rings the doorbell for new ring work. Here
 * a completed submit leaves its group RUNNING (idle, extract==insert) on its
 * slot and AS1 pointing at the context's page table; only the GPU caches are
 * flushed so the CPU sees results. A following submit on the SAME context and
 * group publishes the new ring packet and rings the doorbell directly. Any
 * other use retires the parked group first through the unchanged proven
 * SUSPEND -> flush -> remove_as -> detached sequence: a submit on another
 * group/context, map/unmap/create/destroy/close/quiesce on that context,
 * resident end. The display tick treats a parked AS1 as idle (no GPU work
 * outstanding) but never stages through the GPU while parked. Page tables
 * cannot change while parked (map/unmap retire first), so the root is reused.
 * All calls hold the render owner lock.
 */
/* ACE-Lite GPU->CPU snoop probe: [0] plain cached load already saw the GPU's
 * fence write (snoop works), [1] only the invalidated load saw it (no snoop). */
static uint64_t render_snoop_probe[2];
struct RenderParked { X9RenderContext *c; RenderKernelGroup *g; uint64_t root; };
static RenderParked render_parked;
static uint64_t render_park_hits,render_park_retires,render_park_cold;
/* rev252cn diagnostics: retire reasons (101.txt: cold ~55% of submits, the
 * per-frame map/unmap churn of P1-A). Counted by the why string of
 * render_park_release_for; everything else lumps into "other". */
static uint64_t render_park_retire_map,render_park_retire_unmap,render_park_retire_other;
static void render_park_retire_note(const char *why) {
 if(why&&!strcmp(why,"map"))render_park_retire_map++;
 else if(why&&!strcmp(why,"unmap"))render_park_retire_unmap++;
 else render_park_retire_other++;
}
static bool render_park_enabled() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9_gpu_park",0)==1?1:0;IOLog("X9 RENDER PARK: resident group between submits %s (x9_gpu_park)\n",mode?"ON":"off");}
 return mode==1;
}
static bool render_retire_parked(const char *why) {
 if(!render_parked.g)return true;
 if(!resident||resident->uncertain){render_parked={};return false;}
 X9RenderContext *c=render_parked.c;RenderKernelGroup *g=render_parked.g;
 render_parked={};
 vm_offset_t gpu=resident->gpu;
 X9CSFQueueIO io=x9_csf_queue_access_io(&resident->slots[g->slot].access);
 uint64_t began=mach_absolute_time(),ns=0;
 if(x9_csf_group_halt(&io,&g->control,resident->epoch,true,1000000000ULL)!=kIOReturnSuccess||
    !flush_gpu(gpu)||!remove_as(gpu,resident->policy.saved_attr,resident->policy.saved_cfg,1)) {
  c->disabled=true;resident->uncertain=true;
  IOLog("X9 RENDER PARK: retire (%s) group=%u uncertain;AS/MCU/DMA refs retained,no reset\n",why,g->request.handle);
  return false;
 }
 x9_render_memory_detached(c);
 absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
 if(++render_park_retires<=8||render_park_retires%256==0)
  IOLog("X9 RENDER PARK: retired group=%u (%s) in %llu us;hits=%llu retires=%llu cold=%llu\n",
   g->request.handle,why,ns/1000,render_park_hits,render_park_retires,render_park_cold);
 return true;
}
static bool render_park_release_for(X9RenderContext *c,const char *why) {
 if(render_parked.g&&render_parked.c==c)render_park_retire_note(why);
 return !render_parked.g||render_parked.c!=c||render_retire_parked(why);
}
/*
 * x9pl=1 (needs x9_gpu_park=1): map/unmap on the parked context without
 * retiring it. 104.txt: PARK cold 1342 vs hits 739, retire_map 648 and
 * retire_unmap 576 - per-frame BO churn turned ~64% of submits into the full
 * SUSPEND/flush/remove_as + AS install/CSG start sequence. The parked group is
 * idle (the worker holds c->lock from publish to reap and map/unmap need it,
 * so no job of this context can be running) and AS1 still holds the
 * context's root, so the page table is edited in place exactly as Linux
 * panthor updates a live VM (mmu_hw_do_operation_locked, CSF GPUs): write the
 * PTEs, clean the dirty table pages to PoC, AS_LOCKADDR a naturally aligned
 * power-of-two region covering the range (log2-1 encoding, >= 32 KiB) and
 * AS_COMMAND LOCK, FLUSH_CACHES (L2/LSC clean+invalidate), then an explicit
 * AS_COMMAND UNLOCK (the GPU_CONTROL flush does not unlock, unlike
 * FLUSH_PT/FLUSH_MEM), waiting for AS idle after each command. No table page
 * is freed while attached (pruning waits for the next real detach). A
 * hardware step that does not complete marks the session uncertain like a
 * failed retire. Any other parked state falls back to the old retire.
 */
static uint64_t render_park_live_map,render_park_live_unmap;
static bool render_park_live_enabled() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9pl",0)==1?1:0;IOLog("X9 RENDER PARK: live map/unmap on the parked context %s (x9pl)\n",mode?"ON":"off");}
 return mode==1&&render_park_enabled();
}
static bool render_as_lock_region(vm_offset_t gpu,unsigned as,uint64_t start,uint64_t bytes) {
 const unsigned base=as*0x40;
 if(!bytes||start+bytes<start)return false;
 const uint64_t diff=start^(start+bytes-1);
 unsigned width=diff?64u-(unsigned)__builtin_clzll(diff):0u;
 if(width<15)width=15;                    /* AS_LOCK_REGION_MIN_SIZE 32 KiB */
 width-=1;
 const uint64_t region=(start&~((1ULL<<width)-1))|width;
 if(!idle_as(gpu,1000,as))return false;
 gw64(gpu,0x2410+base,region);gw(gpu,0x2418+base,2);   /* LOCKADDR, COMMAND LOCK */
 return idle_as(gpu,1000,as);
}
static bool render_as_unlock(vm_offset_t gpu,unsigned as) {
 const unsigned base=as*0x40;
 if(!idle_as(gpu,1000,as))return false;
 gw(gpu,0x2418+base,3);                   /* COMMAND UNLOCK */
 return idle_as(gpu,1000,as);
}
/* kIOReturnUnsupported: not applicable, caller takes the retire path. */
static IOReturn render_park_live_update(X9RenderContext *c,const X9RenderVMEntry *entry,bool map) {
 if(!render_park_live_enabled()||!render_parked.g||render_parked.c!=c||!c||!c->page_table||c->disabled||c->exposed||
    !entry||!resident||resident->uncertain)return kIOReturnUnsupported;
 RenderKernelGroup *g=render_parked.g;
 vm_offset_t gpu=resident->gpu;
 if(!g->control.running||g->control.uncertain||gr64(gpu,0x2440)!=render_parked.root)return kIOReturnUnsupported;
 IOReturn kr=map?x9_render_vm_map_live(c->page_table,entry):x9_render_vm_unmap_live(c->page_table,entry);
 if(kr!=kIOReturnSuccess)return kr;      /* leaves rolled back; nothing reached the GPU */
 if(x9_render_vm_publish_live(c->page_table)!=render_parked.root||
    !render_as_lock_region(gpu,1,entry->address,entry->bytes)||!flush_gpu(gpu)||!render_as_unlock(gpu,1)||!no_faults(gpu)) {
  c->disabled=true;resident->uncertain=true;
  IOLog("X9 RENDER PARK: live %s va=%llx bytes=%llu uncertain;AS/MCU/DMA refs retained,no reset\n",
   map?"map":"unmap",entry->address,entry->bytes);
  return kIOReturnIOError;
 }
 const uint64_t n=map?++render_park_live_map:++render_park_live_unmap;
 if(n<=4||n%1024==0)IOLog("X9 RENDER PARK: live %s #%llu va=%llx bytes=%llu group=%u kept parked\n",
  map?"map":"unmap",n,entry->address,entry->bytes,g->request.handle);
 return kIOReturnSuccess;
}
static RenderKernelGroup *render_group(X9RenderContext *c,uint32_t id) {
 auto *p=(RenderKernelContext *)c->engine_private;
 if(p)for(auto *g:p->groups)if(g&&g->request.handle==id)return g;
 return nullptr;
}
static uint64_t render_va(const RenderKernelGroup *g,unsigned stream) {
 return 0x10000000ULL+(uint64_t)g->va_index*0x100000+stream*render_storage_bytes;
}
/*
 * x9cs=1: CSG slot steal (rev252dd). 111.txt: the first eight GPU processes
 * each keep one of the eight hardware CSG slots from group create until
 * device close; the ninth (AegirPoster) got kIOReturnNoResources 32 times.
 * Every group that is not executing sits SUSPENDed on its slot (non-parked
 * submits end in CSG SUSPEND; a park retire suspends too), so its complete CS
 * register state (PanVK's subqueue context, scoreboard, heap state) is in that
 * slot's suspend buffer. Stealing follows kbase's scheduler: copy the
 * victim's suspend buffer out (after an L2 clean so the MCU's writes reached
 * RAM), and reprogram the slot for the new group without TERMINATE. The
 * victim's next submit takes a slot the same way, copies its saved state into
 * that slot's suspend buffer and starts with CSG RESUME. A configured but
 * never started victim has nothing to save and starts with START later.
 * Victims: least recently used bound group that is not running, not
 * uncertain, not parked, whose context lock we can take without waiting (it
 * holds every publish..reap pair, so no job of that context is in flight).
 * All slot bookkeeping is under the render owner lock.
 */
static RenderKernelGroup *render_slot_groups[8];
/* Free (not borrowed) slot whose CSG still reads SUSPEND from a stolen-from
 * group: the next configure needs reuse mode. */
static bool render_slot_dirty[8];
static void render_slot_giveback(unsigned s,bool reuse) {
 resident->slots[s].borrowed=false;if(reuse)render_slot_dirty[s]=true;
}
static uint64_t render_steals,render_rebinds,render_steal_fail;
static bool render_steal_enabled() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9cs",0)==1?1:0;IOLog("X9 RENDER SLOT: CSG slot steal %s (x9cs)\n",mode?"ON":"off");}
 return mode==1;
}
static bool render_group_save(RenderKernelGroup *v) {
 auto &slot=resident->slots[v->slot];
 const uint32_t bytes=slot.access.interface.suspend_bytes;
 if(v->control.suspended) {
  if(!bytes||bytes>0x100000u)return false;
  if(!v->saved||v->saved_bytes!=bytes) {
   if(v->saved)IOFree(v->saved,v->saved_bytes);
   v->saved=(uint8_t *)IOMalloc(bytes);v->saved_bytes=v->saved?bytes:0;
   if(!v->saved)return false;
  }
  unsigned char *src=cpu_address(resident->image,slot.suspend,bytes);
  if(!src||!flush_gpu(resident->gpu))return false;
  invalidate_cpu(src,bytes);memcpy(v->saved,src,bytes);
  v->saved_valid=true;
 }else v->saved_valid=false;   /* configured, never started: START later */
 render_slot_groups[v->slot]=nullptr;v->bound=false;
 return true;
}
/* Owner lock held; c->lock held by the caller for c. Returns the slot index
 * (borrowed, no group bound), sets *reuse when it was taken from a victim. */
static int render_slot_acquire(X9RenderContext *c,bool *reuse) {
 *reuse=false;
 for(unsigned i=0;i<resident->slot_count;i++)if(!resident->slots[i].borrowed) {
  resident->slots[i].borrowed=true;*reuse=render_slot_dirty[i];render_slot_dirty[i]=false;return (int)i;
 }
 if(!render_steal_enabled())return -1;
 bool tried[8]={};
 for(;;) {
  RenderKernelGroup *v=nullptr;
  for(unsigned i=0;i<resident->slot_count;i++) {
   auto *g=render_slot_groups[i];
   if(!g||tried[i]||!g->bound||g->control.running||g->control.uncertain||!g->control.touched||render_parked.g==g)continue;
   if(!v||g->last_use<v->last_use)v=g;
  }
  if(!v){render_steal_fail++;return -1;}
  tried[v->slot]=true;
  X9RenderContext *vc=v->ctx;
  const bool own=vc==c;
  if(!own&&!IOLockTryLock(vc->lock))continue;
  /* Re-check under the victim's lock (its worker may have just finished). */
  bool ok=v->bound&&!v->control.running&&!v->control.uncertain&&render_parked.g!=v;
  const unsigned s=v->slot;const bool was_suspended=v->control.suspended;
  if(ok)ok=render_group_save(v);
  if(!own)IOLockUnlock(vc->lock);
  if(!ok){IOLog("X9 RENDER SLOT: save of group=%u slot=%u failed;not stolen\n",v->request.handle,s);continue;}
  render_steals++;
  if(render_steals<=16||render_steals%256==0)
   IOLog("X9 RENDER SLOT: steal #%llu slot=%u from group=%u (%s,%s);rebinds=%llu\n",
    render_steals,s,v->request.handle,was_suspended?"suspended,state saved":"never started",own?"same context":"other context",render_rebinds);
  *reuse=true;return (int)s;
 }
}
/* Program slot s (borrowed by render_slot_acquire) for g: fresh user I/O with
 * insert=extract=producer (every stream is drained when a group is not
 * running), stream/group inputs, and the saved suspend image for RESUME. */
static IOReturn render_group_bind(X9RenderContext *c,RenderKernelGroup *g,unsigned s,bool reuse) {
 auto &slot=resident->slots[s];
 unsigned char *io=cpu_address(resident->image,slot.user_io,0x10000);
 if(!io)return kIOReturnNoMemory;
 bzero(io,0x10000);
 for(unsigned i=0;i<3;i++) {
  const uint32_t va=slot.user_io+i*host_page;
  if(!put_qword(resident->image,va,g->producer[i])||!put_qword(resident->image,va+8,g->producer[i])||
     !put_qword(resident->image,va+gpu_page,g->producer[i]))return kIOReturnNoMemory;
 }
 for(unsigned off=0;off<0x10000;off+=gpu_page)if(!clean_page(resident->image,slot.user_io+off))return kIOReturnIOError;
 const bool resume=g->saved_valid;
 X9CSFStreamBinding bindings[3];
 for(unsigned i=0;i<3;i++)bindings[i]={render_va(g,i),slot.user_io+i*host_page,slot.user_io+i*host_page+gpu_page,render_ring_bytes};
 X9CSFQueueIO qio=x9_csf_queue_access_io(&slot.access);
 g->control={};
 IOReturn kr=x9_csf_group_configure(&qio,&g->control,&slot.access.interface,&g->request,bindings,slot.suspend,resident->epoch,reuse);
 if(kr!=kIOReturnSuccess)return kr;
 if(resume) {
  const uint32_t bytes=slot.access.interface.suspend_bytes;
  unsigned char *dst=cpu_address(resident->image,slot.suspend,bytes);
  if(!dst||bytes!=g->saved_bytes){g->control.uncertain=true;return kIOReturnIOError;}
  memcpy(dst,g->saved,bytes);
  for(uint32_t off=0;off<bytes;off+=gpu_page)if(!clean_page(resident->image,slot.suspend+off)){g->control.uncertain=true;return kIOReturnIOError;}
  /* The L2 may still hold the previous owner's lines of this buffer. */
  if(!flush_gpu(resident->gpu)){g->control.uncertain=true;return kIOReturnIOError;}
  g->control.suspended=true;   /* x9_csf_group_start issues RESUME */
 }
 g->slot=s;g->bound=true;g->ctx=c;render_slot_groups[s]=g;
 return kIOReturnSuccess;
}
static bool render_span_sync(X9RenderResource &r,uint64_t off,uint64_t bytes) {
 vm_offset_t gpu=resident->gpu;
 if(!bytes||((off|bytes)&(gpu_page-1))||off>r.bytes||bytes>r.bytes-off)return false;
 IOByteCount span=0;uint64_t pa=r.memory->getPhysicalSegment(off,&span,kIOMemoryMapperNone);
 if(!pa||(pa&4095)||span<bytes||pa>=1ULL<<40||bytes>(1ULL<<40)-pa||!no_faults(gpu)||(gr(gpu,0x20)&(1u<<20)))return false;
 /* Stock v14 PA-range FULL operation on held, physically contiguous private
  * storage. Never bridge a hole or another allocation to save a command. */
 uint64_t old0=gr64(gpu,0xd0),old1=gr64(gpu,0xd8);
 gw64(gpu,0xd0,pa);gw64(gpu,0xd8,pa+bytes-1);gw(gpu,0x30,0x7308u);
 bool done=false;uint64_t end=deadline_ms(1000);unsigned cache_polls=0;
 do {
  if(!no_faults(gpu))break;
  if(gr(gpu,0x20)&(1u<<20)){done=true;break;}poll_pause(&cache_polls);
 }while(mach_absolute_time()<end);
 if(!done)return false;
 gw(gpu,0x24,1u<<20);gw64(gpu,0xd0,old0);gw64(gpu,0xd8,old1);return true;
}
static bool render_page_sync(X9RenderResource &r,uint64_t off) {
 return render_span_sync(r,off,gpu_page);
}
static bool render_range_sync(X9RenderResource &r) {
 for(uint64_t off=0;off<r.bytes;) {
  IOByteCount span=0;
  if(!r.memory->getPhysicalSegment(off,&span,kIOMemoryMapperNone))return false;
  if(span>r.bytes-off)span=r.bytes-off;
  if(!span||(span&(gpu_page-1))||!render_span_sync(r,off,span))return false;
  off+=span;
 }
 return true;
}
static void render_storage_clean(X9RenderResource &r) {
 auto *m=(IOBufferMemoryDescriptor *)r.memory;
 x9_gpu_clean((uint64_t)m->getBytesNoCopy(),(unsigned)r.bytes,0);
 __asm__ volatile("dsb sy":::"memory");
}
static bool render_caps(X9RenderCaps *caps) {
 RenderOwner owner;if(!owner.locked||!caps||!render_ready())return false;
 uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 uint64_t line=4ULL<<((ctr>>16)&15);
 if(line>X9_RENDER_CACHE_ATOM||X9_RENDER_CACHE_ATOM%line)return false;
 *caps=resident->caps;
 caps->capabilities=X9RenderBufferObjects|X9RenderHostImport|X9RenderCSFSubmit|X9RenderTiler|X9RenderFragment|X9RenderSubmissionCacheOwnership;
 if(render_async_enabled())caps->capabilities|=X9RenderAsyncSubmit;
 if(render_dirty_enabled())caps->capabilities|=X9RenderDeferredCache;
 return true;
}
static IOReturn render_context_open(X9RenderContext *c) {
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||c->engine_private)return kIOReturnNotReady;
 auto *p=(RenderKernelContext *)IOMallocZero(sizeof(RenderKernelContext));if(!p)return kIOReturnNoMemory;
 p->dirty_lock=IOLockAlloc();
 if(!p->dirty_lock){IOFree(p,sizeof(*p));return kIOReturnNoMemory;}
 IOReturn kr=x9_render_memory_open(c);
 if(kr!=kIOReturnSuccess){IOLockFree(p->dirty_lock);IOFree(p,sizeof(*p));return kr;}
 c->engine_private=p;return kIOReturnSuccess;
}
static IOReturn render_map(X9RenderContext *c,const X9RenderVMEntry *entry) {
 RenderOwner owner;
 if(!owner.locked){IOLog("X9 RENDER MAP: owner lock unavailable\n");return kIOReturnNotReady;}
 if(!render_ready()) {
  IOLog("X9 RENDER MAP: session not ready installed=%u resident=%u ready=%u uncertain=%u GPU_RAW=%x MMU_RAW=%x\n",
   render_engine_installed,resident!=nullptr,resident?resident->ready:0,resident?resident->uncertain:0,
   resident?gr(resident->gpu,0x20):0,resident?gr(resident->gpu,0x2000):0);
  return kIOReturnNotReady;
 }
 if(c&&c->engine_private)((RenderKernelContext *)c->engine_private)->cache_plan_valid=false;
 {IOReturn live=render_park_live_update(c,entry,true);if(live!=kIOReturnUnsupported)return live;}
 if(!render_park_release_for(c,"map"))return kIOReturnIOError;
 /* Invalidate before a mutation attempt, including failed transactions and
  * rollback. The service updates its mapping registry before releasing the
  * context lock; the next submit rebuilds from that completed state. */
 if(c&&c->engine_private)((RenderKernelContext *)c->engine_private)->cache_plan_valid=false;
 IOReturn kr=x9_render_memory_map(c,entry);
 if(kr!=kIOReturnSuccess)IOLog("X9 RENDER MAP: memory refused kr=%x disabled=%u exposed=%u table=%u\n",kr,c?c->disabled:0,c?c->exposed:0,c&&c->page_table);
 return kr;
}
static IOReturn render_unmap(X9RenderContext *c,const X9RenderVMEntry *entry) {
 RenderOwner owner;if(!owner.locked||!render_ready())return kIOReturnNotReady;
 if(c&&c->engine_private)((RenderKernelContext *)c->engine_private)->cache_plan_valid=false;
 {IOReturn live=render_park_live_update(c,entry,false);if(live!=kIOReturnUnsupported)return live;}
 if(!render_park_release_for(c,"unmap"))return kIOReturnIOError;
 return x9_render_memory_unmap(c,entry);
}
static IOReturn render_cache(X9RenderContext *c,X9RenderResource *r,const X9RenderCacheRequest *request) {
 RenderOwner owner;if(!owner.locked||!render_ready())return kIOReturnNotReady;
 return x9_render_memory_cache(c,r,request);
}
static IOReturn render_cache_defer(X9RenderContext *c,const X9RenderCacheRequest *request) {
 /* Called WITHOUT c->lock or the owner lock (vtable contract): the submitting
  * thread must never wait behind the worker's in-flight job. Only appends;
  * the handle and the range are validated when consumed, under c->lock, where
  * a stale or foreign handle simply falls back to the full walk. */
 if(!render_dirty_enabled()||!c||!c->engine_private||!request)return kIOReturnUnsupported;
 if(request->abi!=X9_RENDER_ABI||request->reserved||request->operation!=(X9RenderClean|X9RenderDefer)||
    !request->bytes||((request->offset|request->bytes)&(X9_RENDER_CACHE_ATOM-1))||
    request->offset+request->bytes<request->offset)return kIOReturnBadArgument;
 auto *p=(RenderKernelContext *)c->engine_private;
 IOLockLock(p->dirty_lock);
 if(p->dirty_count==X9_RENDER_DIRTY_MAX) {
  p->dirty_overflow=true;
  static uint64_t told;
  if(++told<=4||told%4096==0)
   IOLog("X9 RENDER DIRTY: record list full (%u);next submit falls back to the full walk\n",X9_RENDER_DIRTY_MAX);
 }else p->dirty[p->dirty_count++]={request->handle,request->offset,request->bytes};
 IOLockUnlock(p->dirty_lock);
 return kIOReturnSuccess;
}
static bool render_drop_storage(X9RenderContext *c,RenderKernelGroup *g) {
 if(c->exposed||resident->uncertain)return false;
 for(unsigned i=0;i<3;i++) {
  auto &r=g->storage[i];
  if(g->mapped[i]) {
   X9RenderVMEntry entry={&r,0,render_va(g,i),r.bytes,r.flags};
   if(x9_render_vm_unmap_kernel(c->page_table,&entry)!=kIOReturnSuccess)return false;
   g->mapped[i]=false;
  }
  if(r.memory){if(r.prepared)r.memory->complete(kIODirectionInOut);r.memory->release();bzero(&r,sizeof(r));}
 }
 return true;
}
static IOReturn render_create_group(X9RenderContext *c,const X9RenderGroup *request,X9RenderGroup *reply) {
 RenderOwner owner;if(!owner.locked||!render_ready())return kIOReturnNotReady;
 if(c&&!render_park_release_for(c,"create group"))return kIOReturnIOError;
 if(!c||!request||!reply||!c->engine_private||c->exposed)return kIOReturnNotReady;
 auto *p=(RenderKernelContext *)c->engine_private;
 if(p->next_group==UINT32_MAX)return kIOReturnNoResources;
 unsigned entry=0;while(entry<8&&p->groups[entry])entry++;
 if(entry==8)return kIOReturnNoResources;
 bool reuse=false;
 const int acquired=render_slot_acquire(c,&reuse);
 if(acquired<0) {
  static unsigned told;
  if(++told<=8||told%64==0)IOLog("X9 RENDER SLOT: no CSG slot for a new group (steals=%llu steal_fail=%llu x9cs=%u)\n",
   render_steals,render_steal_fail,render_steal_enabled());
  return kIOReturnNoResources;
 }
 const unsigned slot=(unsigned)acquired;
 auto *g=(RenderKernelGroup *)IOMallocZero(sizeof(RenderKernelGroup));
 if(!g){render_slot_giveback(slot,reuse);return kIOReturnNoMemory;}
 g->slot=slot;g->request=*request;g->request.handle=++p->next_group;g->ctx=c;g->last_use=mach_absolute_time();
 g->va_index=render_steal_enabled()?entry:slot;
 p->groups[entry]=g;
 for(unsigned i=0;i<3;i++) {
  auto *m=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,render_storage_bytes,host_page);
  if(!m)goto allocation_failure;
  auto &r=g->storage[i];r={m,render_storage_bytes,0,X9RenderGPURead|X9RenderGPUWrite,false,true,false};
  if(m->prepare(kIODirectionInOut)!=kIOReturnSuccess)goto allocation_failure;r.prepared=true;
  auto *bytes=(uint8_t *)m->getBytesNoCopy();if(!bytes)goto allocation_failure;
  bzero(bytes,render_storage_bytes);memset(bytes+0x8000,0xa5,host_page);
  const unsigned address=resident->caps.cs_registers-4,length=address+2;
  /* Immutable ring entries keep the already verified CALL-body reuse scheme.
   * Each body page has four full host-page slots; only inactive pages change. */
  for(unsigned packet=0;packet<render_ring_bytes/render_packet_bytes;packet++) {
   auto *words=(uint64_t *)(bytes+packet*render_packet_bytes);
   words[0]=x9_cs_move48(address,render_va(g,i)+host_page+(packet%4)*gpu_page);
   words[1]=x9_cs_move32(length,render_body_bytes);words[2]=x9_cs_call(address,length);words[3]=0;
  }
  X9RenderVMEntry mapping={&r,0,render_va(g,i),r.bytes,r.flags};
  if(x9_render_vm_map_kernel(c->page_table,&mapping)!=kIOReturnSuccess)goto allocation_failure;
  g->mapped[i]=true;render_storage_clean(r);
 }
 {
  IOReturn kr=render_group_bind(c,g,slot,reuse);
  if(kr!=kIOReturnSuccess) {
   IOLog("X9 RENDER SLOT: bind group=%u slot=%u reuse=%u kr=%x uncertain=%u\n",g->request.handle,slot,reuse,kr,g->control.uncertain);
   if(g->control.uncertain){resident->uncertain=true;c->disabled=true;return kr;}
   goto allocation_failure;
  }
 }
 *reply=g->request;return kIOReturnSuccess;
allocation_failure:
 if(!render_drop_storage(c,g)){c->disabled=true;return kIOReturnIOError;}
 render_slot_giveback(slot,reuse);
 p->groups[entry]=nullptr;IOFree(g,sizeof(*g));return kIOReturnNoMemory;
}
static void render_group_free(RenderKernelContext *p,RenderKernelGroup *g) {
 for(auto &entry:p->groups)if(entry==g){entry=nullptr;break;}
 for(auto &owner:render_slot_groups)if(owner==g)owner=nullptr;
 if(g->saved)IOFree(g->saved,g->saved_bytes);
 IOFree(g,sizeof(*g));
}
static IOReturn render_destroy_locked(X9RenderContext *c,RenderKernelGroup *g) {
 if(!render_ready()||c->exposed) {
  IOLog("X9 RENDER CLOSE: group=%u refused ready=%u exposed=%u\n",g->request.handle,render_ready(),c->exposed);return kIOReturnNotReady;}
 if(!g->bound&&render_steal_enabled()) {
  /* Stolen earlier: no slot, no firmware state; only the private storage. */
  if(!render_drop_storage(c,g)){c->disabled=true;return kIOReturnIOError;}
  IOLog("X9 RENDER CLOSE: group=%u released without a slot (stolen earlier,saved=%u)\n",g->request.handle,g->saved_valid);
  render_group_free((RenderKernelContext *)c->engine_private,g);return kIOReturnSuccess;
 }
 auto &slot=resident->slots[g->slot];X9CSFQueueIO io=x9_csf_queue_access_io(&slot.access);
 /* A suspended CSG does not acknowledge a direct TERMINATE on this firmware
  * (pan192: global DB ACK matched; CSG REQ=0,ACK=0x30000002). Restore its
  * already-drained context before composing the existing RESUME and
  * CS STOP -> CSG TERMINATE handshakes. Never resume a faulted or partially executed job. */
 const bool resume_empty=g->control.suspended;
 if(g->control.uncertain||g->control.running) {
  IOLog("X9 RENDER CLOSE: group=%u slot=%u uncertain=%u running=%u;retain\n",g->request.handle,g->slot,g->control.uncertain,g->control.running);
  return kIOReturnNotReady;}
 if(resume_empty) {
  vm_offset_t gpu=resident->gpu;
  /* 170.txt: with x9_gpu_park another context's group (backboardd) is almost
   * always parked RUNNING on its slot, so this check refused the close of every
   * crashed client whose group was suspended ("unresolved client teardown" x6);
   * its CSG slot was never released, and once all 8 leaked, a restarted
   * SpringBoard got no slot (VK -3) and crash-looped. Retire the other parked
   * group first, as render_submit does for "other context" (owner lock held). */
  if(render_parked.g&&render_parked.g!=g&&!render_retire_parked("close of another context")) {
   IOLog("X9 RENDER CLOSE: group=%u slot=%u parked group retire failed;retain\n",g->request.handle,g->slot);return kIOReturnIOError;}
  if(!no_faults(gpu)||gr64(gpu,0x2440)) {
   IOLog("X9 RENDER CLOSE: group=%u slot=%u faults or AS1 busy (%llx);retain\n",g->request.handle,g->slot,gr64(gpu,0x2440));return kIOReturnNotReady;}
  for(unsigned i=0;i<resident->slot_count;i++) {
   unsigned state=output_word(resident->image,resident->slots[i].access.interface.group_output)&7;
   if(state==1||state==3) {
    IOLog("X9 RENDER CLOSE: group=%u slot=%u CSG slot %u state %u still active;retain\n",g->request.handle,g->slot,i,state);
    return kIOReturnBusy;}
  }
  for(unsigned i=0;i<3;i++) {
   uint32_t user=slot.user_io+i*host_page;
   if(output_qword(resident->image,user)!=g->producer[i]||
      output_qword(resident->image,user+gpu_page)!=g->producer[i]) {
    IOLog("X9 RENDER CLOSE: group=%u stream=%u not drained;retain without resume\n",g->request.handle,i);
    return kIOReturnNotReady;
   }
  }
  uint64_t root=x9_render_memory_publish(c);
  if(!root)return kIOReturnNotReady;
  if(!flush_gpu(gpu)||!install_as(gpu,root,1)||
     x9_csf_group_start(&io,&g->control,resident->epoch,1000000000ULL)!=kIOReturnSuccess) {
   c->disabled=true;resident->uncertain=true;
   IOLog("X9 RENDER CLOSE: empty resume uncertain;AS/DMA refs retained,no reset or retry\n");
   return kIOReturnIOError;
  }
  IOLog("X9 RENDER CLOSE: group=%u slot=%u drained queues resumed for STOP/TERMINATE;no work published\n",g->request.handle,g->slot);
 }
 IOReturn halt=x9_csf_group_halt(&io,&g->control,resident->epoch,false,1000000000ULL);
 if(halt!=kIOReturnSuccess) {
  IOLog("X9 RENDER CLOSE: group=%u slot=%u halt=%x running=%u group_uncertain=%u;retain session\n",
   g->request.handle,g->slot,halt,g->control.running,g->control.uncertain);
  resident->uncertain=true;c->disabled=true;return kIOReturnIOError;
 }
 if(resume_empty) {
  if(!flush_gpu(resident->gpu)||!remove_as(resident->gpu,resident->policy.saved_attr,resident->policy.saved_cfg,1)) {
   c->disabled=true;resident->uncertain=true;
   IOLog("X9 RENDER CLOSE: terminated group but AS/cache detach uncertain;retain resources\n");
   return kIOReturnIOError;
  }
  x9_render_memory_detached(c);
  IOLog("X9 RENDER CLOSE: group=%u slot=%u STOP/TERMINATE acknowledged;AS1 detached before release\n",g->request.handle,g->slot);
 }
 if(!render_drop_storage(c,g)) {
  IOLog("X9 RENDER CLOSE: group=%u slot=%u storage release uncertain\n",g->request.handle,g->slot);
  c->disabled=true;return kIOReturnIOError;
 }
 if(!sweet_x9_csf_resident_release_slot(g->slot,resident->epoch)) {
  IOLog("X9 RENDER CLOSE: group=%u slot=%u resident release uncertain;session_uncertain=%u\n",
   g->request.handle,g->slot,resident->uncertain);
  c->disabled=true;return kIOReturnIOError;
 }
 render_slot_dirty[g->slot]=false;
 render_group_free((RenderKernelContext *)c->engine_private,g);return kIOReturnSuccess;
}
static IOReturn render_destroy_group(X9RenderContext *c,uint32_t id) {
 RenderOwner owner;if(!owner.locked||!c)return kIOReturnNotReady;
 if(!render_park_release_for(c,"destroy group"))return kIOReturnIOError;
 auto *g=render_group(c,id);return g?render_destroy_locked(c,g):kIOReturnBadArgument;
}
/* The page table is the authority for reachability, including argument-buffer
 * pointers and aliased GPU VAs. A caller's upload list is never sufficient.
 * Rebuild after every map/unmap attempt, including same-count remaps and
 * rollback. Resource permissions/backing are immutable while mapped; CPU
 * exposure is evaluated at execution. No large kernel stack arrays. */
static bool render_cache_bounds(X9RenderContext *c) {
 if(c->mapping_count>X9_RENDER_KERNEL_MAPPINGS)return false;
 for(auto &r:c->resources) {
  r.cache_begin=r.write_begin=r.bytes;r.cache_end=r.write_end=0;
 }
 uintptr_t base=(uintptr_t)&c->resources[0],end=(uintptr_t)&c->resources[X9_RENDER_KERNEL_MAPPINGS];
 for(unsigned i=0;i<c->mapping_count;i++) {
  const auto &m=c->mappings[i];uintptr_t p=(uintptr_t)m.resource;
  if(p<base||p>=end||(p-base)%sizeof(X9RenderResource))return false;
  auto &r=*m.resource;
  if(!r.memory||!m.bytes||!m.flags||(m.flags&~r.flags)||
     ((m.offset|m.bytes)&(X9_RENDER_CACHE_ATOM-1))||m.offset>r.bytes||m.bytes>r.bytes-m.offset)return false;
  uint64_t limit=m.offset+m.bytes;
  if(m.offset<r.cache_begin)r.cache_begin=m.offset;
  if(limit>r.cache_end)r.cache_end=limit;
  if(m.flags&X9RenderGPUWrite) {
   if(m.offset<r.write_begin)r.write_begin=m.offset;
   if(limit>r.write_end)r.write_end=limit;
  }
 }
 return true;
}
/* rev252cn (x9dc): the client records each CPU->GPU flush here (lock-free
 * path, no context/owner lock) instead of running the inline CIVAC; the next
 * submit's publish phase maintains exactly these ranges and skips the full
 * fallback walk for the resources they cover. Ordering needs no tag: the
 * submit path moves this list into a pending slot for its submission id (under
 * c->lock, after every record that belongs to that submit completed), so
 * publish(S) can never consume S+1's ranges. Overflow or an unknown handle
 * falls back to the full walk for that submit. The two pending slots are keyed
 * by submission-id parity (two in-flight jobs under x9if). */
static bool render_dirty_enabled() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9dc",0)==1?1:0;
  IOLog("X9 RENDER DIRTY: deferred client clean ranges %s (x9dc)\n",mode?"ON":"off");}
 return mode==1;
}
/* rev252cn (x9if): two jobs in flight. The worker publishes submission S+1
 * (parked-resume path: no AS/CSG programming) while S is still running, so the
 * GPU never idles between jobs. The reap order stays FIFO and a completion
 * still means CPU-visible results. Requires park (the resume path is the only
 * publish that touches no AS state) and async submission. */
static bool render_pipeline_enabled() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9if",0)==1?1:0;
  IOLog("X9 RENDER PIPELINE: two jobs in flight %s (x9if; needs x9_gpu_park and x9_submit_async)\n",mode?"ON":"off");}
 return mode==1;
}
/* The per-stream fence lives in the private storage page at +0x8000 (slot 0,
 * unchanged for every depth-1 path). A second in-flight job needs a second
 * fence location because its publish would otherwise bzero job 1's fence value
 * before its reap read it. Slot 1 sits at +0x8300, clear of the marker words
 * (0x8080..0x8288) and inside the one gpu_page that render_page_sync covers.
 * Guards (0xa5 fill at group create) and the SYNC_SET64 packet encoding are
 * identical per slot. */
static constexpr unsigned render_fence_stride=0x300u;
static uint64_t render_fence_offset(unsigned slot) {return 0x8000u+(uint64_t)slot*render_fence_stride;}
/* Publish/reap split of render_submit (rev252cn): everything through the
 * doorbell is publish; fence poll, park/flush and post-cache are reap. The
 * state carries exactly what publish computes and reap cannot recompute.
 * Telemetry accumulators and the scope snapshot are per-phase; the scope
 * snapshot is retaken at reap (a command buffer's acquire outlives its own
 * post maintenance, so the snapshot at reap time sees the same holds). */
struct RenderJobState {
 X9RenderContext *c; RenderKernelGroup *g;
 const X9RenderSubmission *request; X9RenderCompletion *reply;
 unsigned active; uint64_t expected[3];
 X9CSFQueueIO io; vm_offset_t gpu;
 bool resume,park,pipeline,done,plan_rebuilt,coherent,scope_on,scope_hold;
 uint64_t root; unsigned fence_slot,snoop_seen,polls,scope_count;
 const char *phase;
 uint64_t times[8];
 uint64_t cache_bytes[2],shader_ready_seen;
 uint64_t private_skip[2],initial_bytes,fallback_bytes;
 uint64_t reach_saved[2],import_bytes[2],full_import_bytes[2];
 uint64_t coherent_skip[2],wc_skip[2],holes_saved[2],scope_skip[2],lc_skip[2],lc_bytes[2];
 uint64_t scope_uncovered,scope_uncovered_bytes,scope_uncovered_max;
 uint64_t scope_hold_bytes[2];
 uint64_t dirty_entries,dirty_bytes,dirty_skip_res;
 bool dirty_overflow;
 uint64_t end,next_sync;
 uint64_t gts_ring,gts_seen;   /* x9gt: GPU TIMESTAMP after the doorbell / when the fence was seen */
};
static IOReturn render_submit_publish(RenderJobState *st);
static IOReturn render_submit_reap(RenderJobState *st);
/*
 * x9gt=1: split the per-job fence wait (111.txt: wait_us ~5 ms/job at DDR
 * gear 9) into firmware pickup (doorbell -> GPU timestamp before the client
 * CALL), GPU execution (-> timestamp after the wait-all) and kernel notice
 * (-> fence seen by the poll loop). STORE_STATE TIMESTAMP and the GPU
 * TIMESTAMP register (0x98) read the same counter. Stored in the private fence
 * page at 0x8380 (+0x10 for fence slot 1): clear of the fences, the markers
 * and the 0xa5 guard bytes 0x8010..0x803f.
 */
static bool render_gpu_ts_enabled() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9gt",0)==1?1:0;IOLog("X9 RENDER GPUTS: per-job GPU timestamps %s (x9gt)\n",mode?"ON":"off");}
 return mode==1;
}
static uint64_t render_gpu_ts_offset(unsigned fence_slot){return 0x8380u+(uint64_t)fence_slot*0x10u;}
static inline uint64_t render_cs_store_timestamp(uint64_t reg,uint64_t offset) {
 return (40ULL<<56)|(reg<<40)|(0ULL<<32)|(offset&0xffffu);   /* STORE_STATE, state Timestamp, slot 0, immediate */
}
static void render_gpu_ts_account(RenderJobState *st) {
 static uint64_t n,bad,pickup,exec,notice,exec_max,pickup_max,notice_max,win_gts,win_abs;
 uint64_t start=~0ULL,stop=0;bool ok=true;
 for(unsigned i=0;i<3;i++)if(st->active&(1u<<i)) {
  auto *b=(uint8_t *)((IOBufferMemoryDescriptor *)st->g->storage[i].memory)->getBytesNoCopy()+render_gpu_ts_offset(st->fence_slot);
  invalidate_cpu(b,16);uint64_t t0,t1;memcpy(&t0,b,8);memcpy(&t1,b+8,8);
  if(!t0||!t1||t1<t0){ok=false;break;}
  if(t0<start)start=t0;if(t1>stop)stop=t1;
 }
 if(!ok||start<st->gts_ring||st->gts_seen<stop)bad++;
 else {
  n++;const uint64_t a=start-st->gts_ring,e=stop-start,c=st->gts_seen-stop;
  pickup+=a;exec+=e;notice+=c;
  if(e>exec_max)exec_max=e;if(a>pickup_max)pickup_max=a;if(c>notice_max)notice_max=c;
 }
 const uint64_t now_abs=mach_absolute_time(),now_gts=st->gts_seen;
 if(!win_abs){win_abs=now_abs;win_gts=now_gts;return;}
 uint64_t ns=0;absolutetime_to_nanoseconds(now_abs-win_abs,&ns);
 if(ns<2000000000ULL)return;
 /* Ticks -> us with the measured counter rate of this window. */
 const uint64_t ticks=now_gts-win_gts,khz=ns?ticks*1000000ULL/ns:0;
 auto us=[&](uint64_t t)->uint64_t{return khz?t*1000ULL/khz:0;};
 IOLog("X9 RENDER GPUTS: jobs=%llu bad=%llu counter_khz=%llu pickup_us=%llu exec_us=%llu notice_us=%llu max pickup/exec/notice_us=%llu/%llu/%llu;doorbell->CALL,CALL->wait-all,->fence seen\n",
  n,bad,khz,n?us(pickup/n):0,n?us(exec/n):0,n?us(notice/n):0,us(pickup_max),us(exec_max),us(notice_max));
 n=bad=pickup=exec=notice=exec_max=pickup_max=notice_max=0;win_abs=now_abs;win_gts=now_gts;
}
static IOReturn render_submit_uncertain(RenderJobState *st);
/* One snapshot buffer serves the whole engine: the worker is the only caller
 * and its phases run sequentially (publish1, publish2, reap1, reap2). */
static X9RenderScopeView render_scope_view[X9_RENDER_SCOPE_VIEW];
static bool render_scope_skips(RenderJobState *st,const X9RenderResource &r) {
 if(!st->scope_on||st->scope_hold||!r.imported||!r.host_address||r.host_address+r.bytes<r.host_address)return false;
 const uint64_t begin=r.host_address,end=r.host_address+r.bytes;bool covered=false;
 for(unsigned k=0;k<st->scope_count;k++) {
  const auto &v=render_scope_view[k];
  if(v.begin<end&&begin<v.end&&v.refs)return false;   /* referenced by a pending command buffer */
  if(v.regs&&v.begin<=begin&&end<=v.end)covered=true;
 }
 return covered;
}
static bool render_scope_covered(RenderJobState *st,const X9RenderResource &r) {
 if(!r.host_address)return false;
 const uint64_t begin=r.host_address,end=r.host_address+r.bytes;
 for(unsigned k=0;k<st->scope_count;k++)if(render_scope_view[k].regs&&render_scope_view[k].begin<=begin&&end<=render_scope_view[k].end)return true;
 return false;
}
/* Publish phase (c->lock + owner held): maintain exactly the staged ranges.
 * A resource with a consumed entry skips the fallback walk; every other
 * resource keeps it. A stale handle, an out-of-range entry or an overflowed
 * stage only costs the fallback, never correctness: clean is idempotent and
 * the fallback covers any resource the deferred path could not resolve. */
static void render_dirty_consume(RenderJobState *st) {
 auto *p=(RenderKernelContext *)st->c->engine_private;if(!p->dirty_lock)return;
 unsigned slot=(unsigned)(st->request->submission_id%X9_RENDER_DIRTY_SLOTS);
 unsigned count=p->dirty_pending_count[slot];
 if(p->dirty_pending_overflow[slot])count=0;
 if(count) {
  for(auto &r:st->c->resources)r.dirty_maintained=false;   /* stale marks from an earlier submit */
  X9RenderResource *last=nullptr;uint32_t last_handle=0;
  for(unsigned i=0;i<count;i++) {
   const auto &e=p->dirty_pending[slot][i];
   X9RenderResource *r=last_handle==e.handle?last:nullptr;
   if(!r)for(auto &cand:st->c->resources)if(cand.memory&&cand.handle==e.handle){r=&cand;break;}
   last=r;last_handle=r?e.handle:0;
   if(!r||e.offset>r->bytes||e.bytes>r->bytes-e.offset)continue;
   X9RenderCacheRequest req={X9_RENDER_ABI,e.handle,X9RenderClean,0,e.offset,e.bytes};
   if(x9_render_memory_cache(st->c,r,&req)!=kIOReturnSuccess)continue;
   r->dirty_maintained=true;st->dirty_entries++;st->dirty_bytes+=e.bytes;
  }
 }else for(auto &r:st->c->resources)r.dirty_maintained=false;
 p->dirty_pending_count[slot]=0;p->dirty_pending_overflow[slot]=false;
}
/* Submit enqueue (c->lock held, engine submit paths only): everything recorded
 * before this call belongs to this submission (client program order: the
 * plugin drains its sync queue right before composing the submit). Moving the
 * list into the parity slot under c->lock makes publish(S) exact even when the
 * client keeps recording for S+1 while the worker is still publishing S. */
static void render_dirty_stage(RenderKernelContext *p,uint64_t submission_id) {
 if(!p->dirty_lock)return;
 unsigned slot=(unsigned)(submission_id%X9_RENDER_DIRTY_SLOTS);
 IOLockLock(p->dirty_lock);
 if(p->dirty_overflow)p->dirty_pending_overflow[slot]=true;
 else if(p->dirty_count)memcpy(p->dirty_pending[slot],p->dirty,p->dirty_count*sizeof(p->dirty[0]));
 p->dirty_pending_count[slot]=p->dirty_count;
 p->dirty_count=0;p->dirty_overflow=false;
 IOLockUnlock(p->dirty_lock);
}
static IOReturn render_submit_publish(RenderJobState *st) {
 X9RenderContext *c=st->c;const X9RenderSubmission *request=st->request;
 st->times[0]=mach_absolute_time();
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||!request||!st->reply)return kIOReturnNotReady;
 st->times[1]=mach_absolute_time();
 auto *g=render_group(c,request->group);if(!g||c->disabled)return kIOReturnBadArgument;
 st->g=g;
 /* Parked fast path: same context and group, still running, AS1 still ours.
  * The pipeline's second publish (x9if) reuses exactly this path while job 1
  * still executes: the worker arms render_parked with job 1's group and root,
  * so the detection below resumes without any AS/CSG programming. */
 const bool park=render_park_enabled();st->park=park;
 bool resume=false;
 if(render_parked.g) {
  if(park&&render_parked.c==c&&render_parked.g==g&&g->control.running&&!g->control.uncertain&&
     gr64(resident->gpu,0x2440)==render_parked.root) resume=true;
  else if(!render_retire_parked(render_parked.c==c?"other group":"other context"))return kIOReturnIOError;
 }
 g->last_use=mach_absolute_time();
 if(!g->bound) {
  /* x9cs: this group's slot was stolen; take one back (maybe stealing). */
  bool reuse=false;const int s=render_slot_acquire(c,&reuse);
  if(s<0) {
   static unsigned told;
   if(++told<=8||told%64==0)IOLog("X9 RENDER SLOT: no slot to rebind group=%u (steal_fail=%llu)\n",g->request.handle,render_steal_fail);
   return kIOReturnBusy;
  }
  IOReturn kr=render_group_bind(c,g,(unsigned)s,reuse);
  if(kr!=kIOReturnSuccess) {
   IOLog("X9 RENDER SLOT: rebind group=%u slot=%d reuse=%u resume=%u kr=%x uncertain=%u\n",g->request.handle,s,reuse,g->saved_valid,kr,g->control.uncertain);
   if(g->control.uncertain){resident->uncertain=true;c->disabled=true;return kIOReturnIOError;}
   render_slot_giveback((unsigned)s,reuse);return kr;
  }
  render_rebinds++;
  if(render_rebinds<=16||render_rebinds%256==0)
   IOLog("X9 RENDER SLOT: rebind #%llu group=%u slot=%d reuse=%u %s;steals=%llu\n",render_rebinds,g->request.handle,s,reuse,
    g->control.suspended?"RESUME from saved state":"fresh START",render_steals);
 }
 /* A parked context is NOT exposed (GPU has no work; see park below). The
  * pipeline's second publish runs with exposed still set from job 1: the
  * worker holds the context and owner locks across the whole pair, so no
  * client cache op can observe that window, and the pre-cache below only
  * writes back CPU-dirty lines (a clean never clobbers concurrent GPU
  * writes, which do not run through CPU cache lines). */
 if(c->exposed&&!st->pipeline)return kIOReturnNotReady;
 const uint64_t parked_root=resume?render_parked.root:0;
 render_parked={};   /* re-established only after reap */
 /* Early returns before any hardware step must leave the still-running idle
  * group tracked, or AS1 would stay installed with no owner. */
 struct ParkRestore {
  bool armed;RenderParked saved;
  ~ParkRestore(){if(armed&&!render_parked.g)render_parked=saved;}
 } park_restore{resume,{c,g,parked_root}};
 if(resume)render_park_hits++;else render_park_cold++;
 vm_offset_t gpu=resident->gpu;st->gpu=gpu;
 const bool coherent=resident->coherent;st->coherent=coherent;
 int wc_check=0;PE_parse_boot_argn("x9_wc_check",&wc_check,sizeof(wc_check));
 if(wc_check) {
  uint64_t began=mach_absolute_time(),checked=0,reset=0,ns=0;
  IOReturn kr=x9_render_memory_reassert_wc(c,&checked,&reset);
  absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
  static uint64_t calls=0,pages=0,resets=0,total_ns=0,last=0;
  calls++;pages+=checked;resets+=reset;total_ns+=ns;
  if(reset||kr!=kIOReturnSuccess||!last||mach_absolute_time()>=last) {
   IOLog("X9 RENDER WC CHECK: calls=%llu pages=%llu restored=%llu last_restored=%llu avg_us=%llu kr=%x\n",
    calls,pages,resets,reset,total_ns/calls/1000,kr);
   last=deadline_ms(2000);
  }
  if(kr!=kIOReturnSuccess)return kr;
 }
 const char *phase="storage-sync";st->phase=phase;
 if(!resume&&gr64(gpu,0x2440))return kIOReturnBusy;
 for(unsigned i=0;i<resident->slot_count;i++) {
  if(resume&&i==g->slot)continue;   /* our own idle running group */
  unsigned state=output_word(resident->image,resident->slots[i].access.interface.group_output)&7;
  if(state==1||state==3)return kIOReturnBusy;
 }
 if(!c->engine_private||c->mapping_count>X9_RENDER_KERNEL_MAPPINGS)return kIOReturnBadArgument;
 auto *kernel_context=(RenderKernelContext *)c->engine_private;
 auto *plan=&kernel_context->cache_plan;
 if(!kernel_context->cache_plan_valid||kernel_context->cache_plan_mappings!=c->mapping_count) {
  kernel_context->cache_plan_valid=false;
  if(!render_cache_bounds(c)||!render_cache_plan_build(c,plan))return kIOReturnBadArgument;
  kernel_context->cache_plan_mappings=c->mapping_count;
  kernel_context->cache_plan_valid=true;st->plan_rebuilt=true;
 }
 /* Deferred client cleans (x9dc): maintain the recorded ranges first, so the
  * fallback's invalidate below never runs ahead of a clean on the same BO.
  * A resource covered by a consumed entry skips the fallback walk; every
  * other resource keeps it - the walk is the safety net, not a hint. */
 /* Map/unmap and this submit hold the context lock. A mapped resource cannot
  * be released/reused and resource permissions/backing never mutate in place.
  * CPU exposure can change without a map: evaluate it and perform all actual
  * maintenance below on EVERY submit. Cached plan is not a clean/dirty claim.
  * Private queue-storage mappings are maintained separately, unchanged. */
 /* CPU exposure, not a missing memcpy notification, selects the fast path.
  * Every imported or ever CPU-mapped BO keeps a full fallback WITHIN ALL GPU
  * mappings' exact interval union (holes excluded). The bounds are kernel-owned,
  * derived from actual page-table mappings, not opt-in dirty hints. This covers
  * direct contents stores, shared aliases and indirect GPU accesses without
  * relying on an incomplete draw-resource list. Native never-exposed BOs
  * have only the kernel's initial zero-fill to clean, once. Later CPU mapping
  * must acquire them in clientMemoryForType before exposing a descriptor.
  * The context is serialized here, but retained CPU pointers and external
  * IOSurface aliases are NOT write-protected by that lock. They remain shared
  * and always synchronized; no exclusive ownership or HOST_COHERENT capability
  * is inferred from a flush or from user-provided access declarations.
  * CIVAC before GPU access removes dirty host lines as well as speculative
  * clean copies. CIVAC after confirmed GPU quiescence preserves dirty lines
  * of other, unused host resources in a BO while discarding stale clean ones.
  * Unmapped bytes cannot be touched through this context. An alias reachable
  * through another resource is maintained through that resource's descriptor.
  * The runtime must isolate independently owned resources to cache atoms.
  * The opt-in coherent path below is the ONLY exception: it assumes the GPU
  * snoops CPU caches, so exposed client bytes need no CIVAC here. Command and
  * fence storage keep their explicit syncs regardless, and flush_gpu, group
  * start/halt and every uncertainty check stay unchanged. */
 /* Frontend cache scope (x9_cache_scope=1, rev252bx): one snapshot of this
  * task's ranges serves pre and post. A range acquired at commit stays
  * acquired until the command buffer completed, which is after this job's
  * post maintenance, so every BO this job references is maintained. */
 if(render_dirty_enabled())render_dirty_consume(st);
 bool scope_hold=true;unsigned scope_count=0;
 const bool scope_on=x9_render_scope_enabled();
 if(scope_on)scope_count=x9_render_scope_snapshot(c->owner,render_scope_view,X9_RENDER_SCOPE_VIEW,&scope_hold);
 st->scope_on=scope_on;st->scope_count=scope_count;st->scope_hold=scope_hold;
 st->scope_uncovered=0;st->scope_uncovered_bytes=0;st->scope_uncovered_max=0;
 for(auto &r:c->resources)if(r.memory) {
  if(r.write_combined){st->wc_skip[0]+=r.bytes;continue;}   // never cached, see X9RenderResource
  if(r.cpu_exposed&&render_scope_skips(st,r)){st->scope_skip[0]+=r.bytes;continue;}
  if(scope_on&&r.imported&&r.cpu_exposed) {
   if(!render_scope_covered(st,r)) {
    /* rev252cb: imported CPU-exposed BOs that no registered range covers (what the frontend does not name yet). */
    st->scope_uncovered++;st->scope_uncovered_bytes+=r.bytes;if(r.bytes>st->scope_uncovered_max)st->scope_uncovered_max=r.bytes;
   }else st->scope_hold_bytes[0]+=r.bytes;   /* covered, held by a pending command buffer: maintained anyway */
  }
  if(coherent&&r.cpu_exposed) {
   st->coherent_skip[0]+=r.cache_end>r.cache_begin?r.cache_end-r.cache_begin:0;continue;
  }
  if(!r.cpu_exposed&&r.initial_cache_clean){st->private_skip[0]+=r.bytes;continue;}
  if(r.cpu_exposed&&r.dirty_maintained){st->dirty_skip_res++;continue;}   /* x9dc: maintained above */
  /* x9lc: an IOSurface import needs a clean only when the CPU may have written
   * it since the last one (write unlock moved the seed). */
  uint32_t lc_seed=0;const bool lc_linked=r.imported&&r.cpu_exposed&&r.surface_id;
  if(lc_linked&&!sweet_surface_gpu_needs_clean(r.surface_id,&lc_seed)){st->lc_skip[0]+=r.bytes;continue;}
  uint64_t bytes=0;
  IOReturn kr;
  if(r.cpu_exposed) {
   kr=render_cache_plan_resource(c,plan,r,0,&bytes);
   if(kr!=kIOReturnSuccess)return kr;
   st->reach_saved[0]+=r.bytes-bytes;st->fallback_bytes+=bytes;
   uint64_t bound=r.cache_end>r.cache_begin?r.cache_end-r.cache_begin:0;
   if(bytes>bound)return kIOReturnBadArgument;
   st->holes_saved[0]+=bound-bytes;
   if(r.imported){st->import_bytes[0]+=bytes;st->full_import_bytes[0]+=r.bytes;}
   if(lc_linked){sweet_surface_gpu_cleaned(r.surface_id,lc_seed);st->lc_bytes[0]+=bytes;}
  }else {
   bytes=r.bytes;st->initial_bytes+=bytes;
   if(!bytes)continue;
   X9RenderCacheRequest cache={X9_RENDER_ABI,r.handle,X9RenderClean|X9RenderInvalidate,0,0,bytes};
   kr=x9_render_memory_cache(c,&r,&cache);if(kr!=kIOReturnSuccess)return kr;
  }
  st->cache_bytes[0]+=bytes;
 }
 st->times[2]=mach_absolute_time();
 st->active=0;
 for(unsigned n=0;n<request->stream_count;n++) {
  const auto &stream=request->streams[n];unsigned i=stream.index;
  if(i>=3||!stream.bytes||((st->active)&(1u<<i))||stream.bytes>1u<<20||g->producer[i]>UINT64_MAX-render_packet_bytes)return kIOReturnBadArgument;
  st->active|=1u<<i;st->expected[i]=g->producer[i]+render_packet_bytes;
  auto *base=(uint8_t *)((IOBufferMemoryDescriptor *)g->storage[i].memory)->getBytesNoCopy();
  auto *body=(uint64_t *)(base+host_page+((g->producer[i]/render_packet_bytes)%4)*gpu_page);
  unsigned address=resident->caps.cs_registers-4,data=address+2;
  /* Private markers use the SYNC_SET32 operation proven in the X9 shader
   * path. They observe progress only; the 64-bit fence remains authoritative. */
  auto marker=[&](unsigned at,uint64_t offset,uint32_t value) {
   body[at]=x9_cs_move48(address,render_va(g,i)+offset);
   body[at+1]=x9_cs_move32(data,value);
   body[at+2]=(0x26ULL<<56)|((uint64_t)address<<40)|((uint64_t)data<<32);
   body[at+3]=x9_cs_wait(1);
  };
  if(render_gpu_ts_enabled()) {
   /* x9gt: same sequence without the three NOPs, plus a GPU timestamp
    * (STORE_STATE, state 0) right before the client CALL and right after
    * the wait-all, relative to the marker address still in the register. */
   const uint64_t ts=render_gpu_ts_offset(st->fence_slot);
   marker(0,0x8080,0x58490101);
   body[4]=render_cs_store_timestamp(address,ts-0x8080);
   body[5]=x9_cs_move48(address,stream.gpu_address);body[6]=x9_cs_move32(data,stream.bytes);body[7]=x9_cs_call(address,data);
   marker(8,0x8100,0x58490102);
   body[12]=x9_cs_wait(0xffff);body[13]=render_cs_store_timestamp(address,ts+8-0x8100);
   marker(14,0x8200,0x58490104);
   marker(18,0x8280,0x58490105);
   body[22]=x9_cs_move48(address,render_va(g,i)+0x8000);body[23]=x9_cs_move32(data,(uint32_t)request->submission_id);
   body[24]=x9_cs_move32(data+1,(uint32_t)(request->submission_id>>32));body[25]=x9_cs_sync_set64(address,data);body[26]=x9_cs_wait(1);
   marker(27,0x8180,0x58490103);
   body[31]=0;
   bzero(base+ts,16);
  }else {
  marker(0,0x8080,0x58490101);
  body[4]=x9_cs_move48(address,stream.gpu_address);body[5]=x9_cs_move32(data,stream.bytes);body[6]=x9_cs_call(address,data);
  marker(7,0x8100,0x58490102);
  body[11]=x9_cs_wait(0xffff);marker(12,0x8200,0x58490104);
  /* Completion is published only after all GPU work has drained. Cache
   * ownership is handed back later, after group suspend and the existing
   * GPU-control full flush succeeds; do not issue a second CS flush here. */
  body[16]=0;body[17]=0;body[18]=0;
  marker(19,0x8280,0x58490105);
  body[23]=x9_cs_move48(address,render_va(g,i)+0x8000);body[24]=x9_cs_move32(data,(uint32_t)request->submission_id);
  body[25]=x9_cs_move32(data+1,(uint32_t)(request->submission_id>>32));body[26]=x9_cs_sync_set64(address,data);body[27]=x9_cs_wait(1);
  marker(28,0x8180,0x58490103);
  }
  /* Clear only this job's fence slot: with two jobs in flight (x9if) the
   * other slot holds the previous job's still-unreaped fence value. */
  bzero(base+render_fence_offset(st->fence_slot),16);bzero(base+0x8080,8);bzero(base+0x8100,8);bzero(base+0x8180,8);
  bzero(base+0x8200,8);bzero(base+0x8280,8);render_storage_clean(g->storage[i]);
 }
 if(!st->active)return kIOReturnBadArgument;
 uint64_t root=resume?parked_root:x9_render_memory_publish(c);if(!root)return kIOReturnNotReady;
 st->root=root;
 park_restore.armed=false;   /* hardware phase: failures are uncertain, success re-parks */
 if(resume)c->exposed=true;  /* cold path: x9_render_memory_publish set it */
 for(unsigned i=0;i<3;i++)if(((st->active)&(1u<<i))&&!render_range_sync(g->storage[i]))return render_submit_uncertain(st);
 st->times[3]=mach_absolute_time();
 st->phase=resume?"gpu-flush(parked AS1)":"gpu-flush-and-as-install";
 if(!flush_gpu(gpu)||(!resume&&!install_as(gpu,root,1)))return render_submit_uncertain(st);
 {
  auto &slot=resident->slots[g->slot];X9CSFQueueIO io=x9_csf_queue_access_io(&slot.access);
  st->phase="user-io-prepare";
  for(unsigned i=0;i<3;i++) {
   uint32_t va=slot.user_io+i*host_page;
   /* The producer word check compares against the value this packet is
    * replacing, so the kernel-side producer commits only at stream-publish
    * below (rev252cn: publish, not reap, so the pipeline's second publish
    * sees expected values past job 1's packet). A failure before that leaves
    * the producer untouched and the next submit recomputes the same values. */
   if(output_qword(resident->image,va)!=g->producer[i]||
      (!st->pipeline&&output_qword(resident->image,va+gpu_page)!=g->producer[i])||
      !put_qword(resident->image,va+8,g->producer[i])||!clean_page(resident->image,va))return render_submit_uncertain(st);
  }
  st->phase=resume?"group-health(parked)":"group-start";
  if(resume?x9_csf_group_health(&io,&g->control,resident->epoch)!=kIOReturnSuccess:
     x9_csf_group_start(&io,&g->control,resident->epoch,1000000000ULL)!=kIOReturnSuccess)return render_submit_uncertain(st);
  st->phase="stream-publish";
  for(unsigned i=0;i<3;i++)if((st->active)&(1u<<i)) {
   uint32_t va=slot.user_io+i*host_page;
   if(!put_qword(resident->image,va,st->expected[i])||!clean_page(resident->image,va))return render_submit_uncertain(st);
   /* rev252cn: the kernel-side producer commits exactly here, together with
    * the user-io word it mirrors, so the pipeline's second publish sees
    * expected values past job 1's packet (reap no longer commits). */
   g->producer[i]=st->expected[i];
  }
  /* ml_io_map() takes permanent VA from the fixed 8 MiB io_submap and is
   * never released: mapping per submit exhausted it after ~400 jobs and
   * panicked in kmem_alloc (KERN_NO_SPACE). Map the user doorbell page once;
   * submits are serialized by the render owner lock held here. */
  static vm_offset_t user_doorbell;
  if(!user_doorbell)user_doorbell=ml_io_map(0x48090000,0x1000);
  vm_offset_t db=user_doorbell;if(!db)return render_submit_uncertain(st);
  gw(db,0,1);
  st->gts_ring=render_gpu_ts_enabled()?gr64(gpu,0x98):0;
 }
 st->times[4]=mach_absolute_time();
 return kIOReturnSuccess;
}
static IOReturn render_submit_reap(RenderJobState *st) {
 X9RenderContext *c=st->c;RenderKernelGroup *g=st->g;const X9RenderSubmission *request=st->request;
 auto &slot=resident->slots[g->slot];X9CSFQueueIO io=x9_csf_queue_access_io(&slot.access);
 auto *kernel_context=(RenderKernelContext *)c->engine_private;
 auto *plan=&kernel_context->cache_plan;
 st->phase="completion-wait";st->done=false;st->end=deadline_ms(10000);st->next_sync=0;st->polls=0;
 do {
  if(x9_csf_group_health(&io,&g->control,resident->epoch)!=kIOReturnSuccess)return render_submit_uncertain(st);
  st->shader_ready_seen|=gr64(st->gpu,0xa08);
  /* The X9 GPU has shown that system-scope SYNC_SET can remain in GPU
   * cache: synchronize only our held private fence pages before polling.
   * This does not acquire client BOs or assert command completion. The one
   * gpu_page at +0x8000 covers both fence slots (x9if slot 1 at +0x8300). */
  if(mach_absolute_time()>=st->next_sync) {
   st->phase="fence-cache-acquire";
   for(unsigned i=0;i<3;i++)if(((st->active)&(1u<<i))&&!render_page_sync(g->storage[i],0x8000))return render_submit_uncertain(st);
   // pan204's short jobs waited ~10.4ms for the next cache acquisition.
   // Keep the same private-page sync and all fence/extract/guard checks.
   // While poll_pause() is still spinning, acquire on every poll so a short
   // job is seen within ~16us; after that, acquire at1ms as before.
   st->next_sync=st->polls<poll_spin_limit?0:deadline_ms(1);st->phase="completion-wait";
  }
  st->done=true;
  for(unsigned i=0;i<3;i++)if((st->active)&(1u<<i)) {
   auto *fence=(uint8_t *)((IOBufferMemoryDescriptor *)g->storage[i].memory)->getBytesNoCopy()+render_fence_offset(st->fence_slot);
   /* ACE-Lite snoop probe (log only): the GPU's SYNC_SET64 to this
    * outer-shareable private page was flushed by render_page_sync above. A
    * plain cached load sees it only if that write-back snooped/invalidated
    * the CPU's line; the invalidated load always sees it. */
   uint64_t plain=0;if(x9_gpu_acelite_active){__asm__ volatile("dmb ish":::"memory");plain=*(volatile uint64_t *)fence;}
   invalidate_cpu(fence,64);uint64_t actual=0;memcpy(&actual,fence,8);
   if(x9_gpu_acelite_active&&actual==request->submission_id&&!(st->snoop_seen&(1u<<i))) {
    st->snoop_seen|=1u<<i;if(plain==actual)render_snoop_probe[0]++;else render_snoop_probe[1]++;
   }
   uint32_t sync_error=0;memcpy(&sync_error,fence+8,sizeof(sync_error));
   if(sync_error)return render_submit_uncertain(st);
   for(unsigned j=16;j<64;j++)if(fence[j]!=0xa5)return render_submit_uncertain(st);
   uint64_t insert=output_qword(resident->image,slot.user_io+i*host_page+gpu_page);
   /* Pipeline (x9if): a newer packet on the same stream may already have
    * advanced the insert past this job's expected value; in-order per-stream
    * consumption makes >= equivalent to == for the oldest unreaped job. */
   if(actual!=request->submission_id||
      (st->pipeline?insert<st->expected[i]:insert!=st->expected[i]))st->done=false;
  }
  if(st->done){if(st->gts_ring)st->gts_seen=gr64(st->gpu,0x98);break;}
  poll_pause(&st->polls);
 }while(mach_absolute_time()<st->end);
 if(!st->done)return render_submit_uncertain(st);
 st->times[5]=mach_absolute_time();
 if(st->gts_ring)render_gpu_ts_account(st);
 { // GPU busy time for the DVFS governor (sweet_x9_gpu_dependencies.c).
  uint64_t busy_ns=0;absolutetime_to_nanoseconds(st->times[5]-st->times[4],&busy_ns);
  __atomic_fetch_add(&sweet_x9_gpu_busy_ns,busy_ns,__ATOMIC_RELAXED);
 }
 if(st->park) {
  /* Leave the idle group running and AS1 bound; flush so the CPU sees results.
   * Pipeline (x9if): this flush runs while job 2 executes. FLUSH_CACHES
   * writes L2 dirty lines back and forces refetches; in-flight work reads
   * the same data from RAM. Device-validated under the x9if boot-arg. */
  st->phase="final-flush(parking)";
  if(!flush_gpu(st->gpu))return render_submit_uncertain(st);
 }else {
  st->phase="group-suspend";
  if(x9_csf_group_halt(&io,&g->control,resident->epoch,true,1000000000ULL)!=kIOReturnSuccess)return render_submit_uncertain(st);
  st->phase="final-flush-and-as-detach";
  if(!flush_gpu(st->gpu)||!remove_as(st->gpu,resident->policy.saved_attr,resident->policy.saved_cfg,1))return render_submit_uncertain(st);
 }
 if(st->park) {
  /* GPU idle with AS1 still bound: CPU-side operations (cache ops, CPU views,
   * releases of unmapped resources) are safe, so the context is not exposed.
   * Page-table changes still retire first (vm stays attached). Pipeline: the
   * worker holds the context and owner locks until both jobs reaped, so no
   * client op can run against the still-running second job. */
  render_parked={c,g,st->root};c->exposed=false;
 }else x9_render_memory_detached(c);
 st->times[6]=mach_absolute_time();
 /* The post-cache scope snapshot is retaken here: an acquire made at commit
  * stays held until the command buffer completed, which is after this post
  * maintenance, so this job's referenced ranges are still held. */
 st->scope_count=0;st->scope_hold=true;
 if(st->scope_on)st->scope_count=x9_render_scope_snapshot(c->owner,render_scope_view,X9_RENDER_SCOPE_VIEW,&st->scope_hold);
 for(auto &r:c->resources)if(r.memory&&(r.flags&X9RenderGPUWrite)) {
  if(r.write_combined){st->wc_skip[1]+=r.bytes;continue;}
  if(st->coherent&&r.cpu_exposed) {
   st->coherent_skip[1]+=r.write_end>r.write_begin?r.write_end-r.write_begin:0;continue;
  }
  if(r.cpu_exposed&&render_scope_skips(st,r)){st->scope_skip[1]+=r.bytes;continue;}
  if(st->scope_on&&r.imported&&r.cpu_exposed&&render_scope_covered(st,r))st->scope_hold_bytes[1]+=r.bytes;
  /* x9lc: no invalidate now; the next CPU acquire of the surface does it. */
  if(r.imported&&r.cpu_exposed&&r.surface_id){sweet_surface_gpu_wrote(r.surface_id);st->lc_skip[1]+=r.bytes;continue;}
  // No CPU consumer exists for a never-exposed allocation. Keep GPU flush,
  // CSG suspend and AS detach above; a fence alone does not return ownership.
  if(!r.cpu_exposed){st->private_skip[1]+=r.bytes;continue;}
  uint64_t bytes=0;
  IOReturn kr=render_cache_plan_resource(c,plan,r,1,&bytes);
  if(kr!=kIOReturnSuccess){c->disabled=true;resident->uncertain=true;return kr;}
  st->reach_saved[1]+=r.bytes-bytes;
  uint64_t bound=r.write_end>r.write_begin?r.write_end-r.write_begin:0;
  if(bytes>bound){c->disabled=true;resident->uncertain=true;return kIOReturnBadArgument;}
  st->holes_saved[1]+=bound-bytes;
  if(r.imported){st->import_bytes[1]+=bytes;st->full_import_bytes[1]+=r.bytes;}
  st->cache_bytes[1]+=bytes;
 }
 st->times[7]=mach_absolute_time();
 *st->reply={X9_RENDER_ABI,0,request->group,0,request->submission_id,request->submission_id,0,0,0};
 c->completed_submission=request->submission_id;
 {
  auto *p=(RenderKernelContext *)c->engine_private;p->history[p->next_history++%render_history]=*st->reply;
 }
 // Low-frequency timings, under the render owner lock. These include driver
 // cache/firmware waits and are NOT shader execution times or desktop FPS.
 {
  static uint64_t began=0,count=0,total[7]={},max_ns=0;
  static uint64_t cache_total[2]={0,0},ready_mask=0;
  static uint64_t skipped_total[2]={0,0},initial_total=0,fallback_total=0;
  static uint64_t reach_total[2]={0,0},import_total[2]={0,0},full_import_total[2]={0,0};
  static uint64_t coherent_total[2]={0,0};
  static uint64_t lc_total[2]={0,0},lc_clean_total=0;
  static uint64_t wc_total[2]={0,0};
  static uint64_t dirty_total_entries=0,dirty_total_bytes=0,dirty_total_skip=0;
  static uint64_t scope_total[2]={0,0},scope_entries=0,scope_holds=0;
  static uint64_t unc_n=0,unc_bytes=0,unc_max=0;
  unc_n+=st->scope_uncovered;unc_bytes+=st->scope_uncovered_bytes;if(st->scope_uncovered_max>unc_max)unc_max=st->scope_uncovered_max;
  scope_total[0]+=st->scope_skip[0];scope_total[1]+=st->scope_skip[1];scope_entries+=st->scope_count;if(st->scope_on&&st->scope_hold)scope_holds++;
  dirty_total_entries+=st->dirty_entries;dirty_total_bytes+=st->dirty_bytes;dirty_total_skip+=st->dirty_skip_res;
  static uint64_t hole_total[2]={0,0},span_total[2]={0,0};
  static uint64_t plan_rebuilds=0,plan_reuses=0;
  if(!began)began=st->times[0];
  uint64_t elapsed=0,whole=0;
  absolutetime_to_nanoseconds(st->times[7]-st->times[0],&whole);
  if(whole>max_ns)max_ns=whole;
  for(unsigned i=0;i<7;i++){uint64_t ns=0;absolutetime_to_nanoseconds(st->times[i+1]-st->times[i],&ns);total[i]+=ns;}
  if(st->plan_rebuilt)plan_rebuilds++;else plan_reuses++;
  for(unsigned i=0;i<2;i++){hole_total[i]+=st->holes_saved[i];span_total[i]+=plan->count[i];}
  count++;absolutetime_to_nanoseconds(st->times[7]-began,&elapsed);
  cache_total[0]+=st->cache_bytes[0];cache_total[1]+=st->cache_bytes[1];ready_mask|=st->shader_ready_seen;
  skipped_total[0]+=st->private_skip[0];skipped_total[1]+=st->private_skip[1];
  initial_total+=st->initial_bytes;fallback_total+=st->fallback_bytes;
  for(unsigned i=0;i<2;i++){reach_total[i]+=st->reach_saved[i];import_total[i]+=st->import_bytes[i];full_import_total[i]+=st->full_import_bytes[i];coherent_total[i]+=st->coherent_skip[i];wc_total[i]+=st->wc_skip[i];lc_total[i]+=st->lc_skip[i];}
  lc_clean_total+=st->lc_bytes[0];
  if(elapsed>=2000000000ULL) {
   IOLog("X9 RENDER timing: elapsed_ms=%llu jobs=%llu owner_us=%llu pre_cache_us=%llu storage_us=%llu start_us=%llu wait_us=%llu retire_us=%llu post_cache_us=%llu max_job_us=%llu;averages,not shader time\n",
    elapsed/1000000,count,total[0]/count/1000,total[1]/count/1000,total[2]/count/1000,total[3]/count/1000,
    total[4]/count/1000,total[5]/count/1000,total[6]/count/1000,max_ns/1000);
   IOLog("X9 RENDER cache: jobs=%llu pre_bytes_avg=%llu post_bytes_avg=%llu shader_ready_seen=%llx allowed=%llx;READY snapshots,not utilization\n",
    count,cache_total[0]/count,cache_total[1]/count,ready_mask,resident->caps.shader_present);
   IOLog("X9 RENDER cache-scope: jobs=%llu private_pre_skip_avg=%llu private_post_skip_avg=%llu initial_clean_avg=%llu full_fallback_avg=%llu;exact union of all GPU mappings,indirect included\n",
    count,skipped_total[0]/count,skipped_total[1]/count,initial_total/count,fallback_total/count);
   IOLog("X9 RENDER cache-dirty: jobs=%llu entries_avg=%llu bytes_avg=%llu skipped_res_avg=%llu;client clean ranges maintained at publish (x9dc)\n",
    count,count?dirty_total_entries/count:0,count?dirty_total_bytes/count:0,count?dirty_total_skip/count:0);
   IOLog("X9 RENDER cache-reach: jobs=%llu pre_skip_avg=%llu post_skip_avg=%llu import_pre_avg=%llu import_pre_full_avg=%llu import_post_avg=%llu import_post_full_avg=%llu;kernel mapping bounds,not dirty tracking\n",
    count,reach_total[0]/count,reach_total[1]/count,import_total[0]/count,full_import_total[0]/count,import_total[1]/count,full_import_total[1]/count);
   IOLog("X9 RENDER cache-coherent: jobs=%llu active=%u pre_skip_avg=%llu post_skip_avg=%llu;opt-in skips CIVAC on exposed client bytes,assumes GPU snoop\n",
    count,st->coherent?1:0,coherent_total[0]/count,coherent_total[1]/count);
   IOLog("X9 RENDER cache-lc: jobs=%llu pre_skip_avg=%llu post_skip_avg=%llu seed_clean_avg=%llu;IOSurface imports kept by the lock protocol (x9lc)\n",
    count,lc_total[0]/count,lc_total[1]/count,lc_clean_total/count);
   IOLog("X9 RENDER cache-manifest: jobs=%llu pre_mapped_spans_avg=%llu post_mapped_spans_avg=%llu pre_holes_saved_avg=%llu post_holes_saved_avg=%llu;all mapped indirect targets included,shared aliases retained\n",
    count,span_total[0]/count,span_total[1]/count,hole_total[0]/count,hole_total[1]/count);
   IOLog("X9 RENDER PARK: hits=%llu cold=%llu retires=%llu retire_map=%llu retire_unmap=%llu retire_other=%llu live_map=%llu live_unmap=%llu;hit = no AS install/CSG start/suspend/AS remove\n",
    render_park_hits,render_park_cold,render_park_retires,render_park_retire_map,render_park_retire_unmap,render_park_retire_other,
    render_park_live_map,render_park_live_unmap);
   if(x9_gpu_acelite_active)
    IOLog("X9 RENDER SNOOP: GPU->CPU fence probe coherent=%llu stale=%llu;stale>0 means GPU writes do not invalidate CPU lines\n",
     render_snoop_probe[0],render_snoop_probe[1]);
   IOLog("X9 RENDER cache-plan: builds=%llu reuses=%llu;mapping mutation invalidates,shared cache ops still per submission\n",plan_rebuilds,plan_reuses);
   plan_rebuilds=plan_reuses=0;
   for(unsigned i=0;i<2;i++)hole_total[i]=span_total[i]=0;
   IOLog("X9 RENDER cache-wc: jobs=%llu pre_skip_avg=%llu post_skip_avg=%llu;write-combined BOs need no CPU maintenance\n",
    count,wc_total[0]/count,wc_total[1]/count);
   static uint64_t hold_total[2]={0,0};
   hold_total[0]+=st->scope_hold_bytes[0];hold_total[1]+=st->scope_hold_bytes[1];
   IOLog("X9 RENDER cache-scope-hold: jobs=%llu pre_avg=%llu post_avg=%llu;covered ranges held by pending command buffers, maintained anyway\n",
    count,count?hold_total[0]/count:0,count?hold_total[1]/count:0);
   hold_total[0]=hold_total[1]=0;
   uint64_t ss[6];x9_render_scope_stats(ss);
   IOLog("X9 RENDER cache-frontend-scope: jobs=%llu active=%u pre_skip_avg=%llu post_skip_avg=%llu ranges_avg=%llu hold_jobs=%llu table: used=%llu registers=%llu refused=%llu unreg_miss=%llu placeholders=%llu holds=%llu;imported BOs no pending command buffer references (x9_cache_scope)\n",
    count,st->scope_on?1:0,scope_total[0]/count,scope_total[1]/count,scope_entries/count,scope_holds,ss[5],ss[0],ss[1],ss[2],ss[3],ss[4]);
   IOLog("X9 RENDER cache-uncovered: jobs=%llu imports_avg=%llu bytes_avg=%llu largest=%llu;imported CPU-exposed BOs no registered frontend range covers\n",
    count,unc_n/count,unc_bytes/count,unc_max);
   unc_n=unc_bytes=unc_max=0;
   scope_total[0]=scope_total[1]=scope_entries=scope_holds=0;
   dirty_total_entries=dirty_total_bytes=dirty_total_skip=0;
   for(unsigned i=0;i<2;i++)reach_total[i]=import_total[i]=full_import_total[i]=coherent_total[i]=wc_total[i]=lc_total[i]=0;
   lc_clean_total=0;
   cache_total[0]=cache_total[1]=ready_mask=0;
   skipped_total[0]=skipped_total[1]=initial_total=fallback_total=0;
   began=st->times[7];count=0;bzero(total,sizeof(total));max_ns=0;
  }
 }
 if(request->submission_id<=4||request->submission_id%120==0)
  IOLog("X9 RENDER GPU: group=%u sequence=%llu streams=%u independent fences/extract/guards;CSG suspended,AS1 detached,CPU ownership returned\n",
   request->group,request->submission_id,request->stream_count);
 return kIOReturnSuccess;
}
static IOReturn render_submit_uncertain(RenderJobState *st) {
 X9RenderContext *c=st->c;RenderKernelGroup *g=st->g;const X9RenderSubmission *request=st->request;
 const char *phase=st->phase;unsigned active=st->active;
 const uint64_t *expected=st->expected;vm_offset_t gpu=st->gpu;
 {
  auto &slot=resident->slots[g->slot];const auto &f=slot.access.interface;
  /* CS_STATUS_* are snapshots, not live registers. Stock MT6993 kbase
   * requests CSG STATUS_UPDATE (bit 5) before trusting the snapshot. */
  if(!strcmp(phase,"completion-wait") && g->control.running && no_faults(gpu)) {
   uint32_t ack=output_word(resident->image,f.group_output);
   uint32_t req=input_word(resident->image,f.group_input);
   uint32_t global_ack=output_word(resident->image,f.global_output,8);
   uint32_t global_req=input_word(resident->image,f.global_input,8);
   uint32_t bit=1u<<f.group_index,wanted=(req&~(1u<<5))|((ack^(1u<<5))&(1u<<5));
   uint32_t notify=(global_req&~bit)|((global_ack^bit)&bit);
   bool published=host_set(resident->image,f.group_input,wanted)&&clean_page(resident->image,f.group_input)&&
    host_set(resident->image,f.global_input+8,notify)&&clean_page(resident->image,f.global_input);
   bool accepted=false;
   if(published) {
    gw(resident->doorbell,0,1);uint64_t until=deadline_ms(250);
    do {
     uint32_t ga=output_word(resident->image,f.global_output,8),ca=output_word(resident->image,f.group_output);
     if(!((ga^notify)&bit)&&!((ca^wanted)&(1u<<5))){accepted=true;break;}
     if(!no_faults(gpu))break;sleep_ms();
    }while(mach_absolute_time()<until);
   }
   IOLog("X9 RENDER DIAG: status-update requested=%u acknowledged=%u\n",published,accepted);
  }
  /* One bounded PA-range sync, only on the private fence page, distinguishes
   * CPU visibility from execution progress. It does not retire or retry work. */
  if(!strcmp(phase,"completion-wait"))for(unsigned i=0;i<3;i++)if(active&(1u<<i)) {
   bool synced=render_page_sync(g->storage[i],0x8000);
   IOLog("X9 RENDER DIAG: stream=%u private-fence-page gpu-sync=%u\n",i,synced);
  }
  IOLog("X9 RENDER DIAG: phase=%s group=%u active=%x id=%llu GPU_RAW=%08x MMU_RAW=%08x CSG_REQ=%08x CSG_ACK=%08x GLB_REQ=%08x GLB_ACK=%08x\n",
   phase,g->slot,active,request->submission_id,gr(gpu,0x20),gr(gpu,0x2000),input_word(resident->image,f.group_input),
   output_word(resident->image,f.group_output),input_word(resident->image,f.global_input),output_word(resident->image,f.global_output));
  for(unsigned i=0;i<3;i++) {
   const auto &stream=f.streams[i];uint32_t user=slot.user_io+i*host_page;
   auto *fence=(uint8_t *)((IOBufferMemoryDescriptor *)g->storage[i].memory)->getBytesNoCopy()+render_fence_offset(st->fence_slot);
   invalidate_cpu(fence,64);uint64_t value=0;memcpy(&value,fence,8);
   if(active&(1u<<i)) {
    uint32_t marks[5];const unsigned off[5]={0x80,0x100,0x200,0x280,0x180};
    for(unsigned j=0;j<5;j++){invalidate_cpu(fence+off[j],64);memcpy(&marks[j],fence+off[j],4);}
    IOLog("X9 RENDER MARK: stream=%u enter=%08x return=%08x waited=%08x flushed=%08x fence-issued=%08x\n",i,marks[0],marks[1],marks[2],marks[3],marks[4]);
   }
   IOLog("X9 RENDER DIAG: stream=%u active=%u REQ=%08x ACK=%08x FAULT=%08x FATAL=%08x PC=%08x:%08x WAIT=%08x SCORE=%08x BLOCK=%08x insert=%llu extract=%llu expected=%llu fence=%llu\n",
    i,(active>>i)&1,input_word(resident->image,stream.input),output_word(resident->image,stream.output),
    output_word(resident->image,stream.output,0x80),output_word(resident->image,stream.output,0x84),
    output_word(resident->image,stream.output,0x44),output_word(resident->image,stream.output,0x40),
    output_word(resident->image,stream.output,0x48),output_word(resident->image,stream.output,0x5c),
    output_word(resident->image,stream.output,0x60),output_qword(resident->image,user),
    output_qword(resident->image,user+gpu_page),expected[i],value);
  }
 }
 c->disabled=true;resident->uncertain=true;
 IOLog("X9 RENDER GPU: submission uncertain;AS/MCU/DMA/power refs retained,no reset or retry\n");
 return kIOReturnIOError;
}
/* Synchronous submit (v1 selector; also the depth-1 async worker path shape):
 * publish and reap back to back, byte-identical behavior to pre-rev252cn. */
static IOReturn render_submit(X9RenderContext *c,const X9RenderSubmission *request,X9RenderCompletion *reply) {
 RenderJobState st={};
 st.c=c;st.request=request;st.reply=reply;st.fence_slot=0;st.pipeline=false;
 if(render_dirty_enabled())render_dirty_stage((RenderKernelContext *)c->engine_private,request->submission_id);
 IOReturn kr=render_submit_publish(&st);
 if(kr!=kIOReturnSuccess)return kr;
 return render_submit_reap(&st);
}
static IOReturn render_wait(X9RenderContext *c,const X9RenderWaitRequest *request,X9RenderCompletion *reply) {
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||!request||!reply||!c->engine_private)return kIOReturnNotReady;
 auto *p=(RenderKernelContext *)c->engine_private;
 for(const auto &r:p->history)if(r.group==request->group&&r.submission_id==request->submission_id){*reply=r;return kIOReturnSuccess;}
 return kIOReturnNotFound;
}
static bool render_close(X9RenderContext *c) {
 if(c)render_async_drain_unlocked((RenderKernelContext *)c->engine_private);   /* before the owner lock */
 RenderOwner owner;if(!owner.locked||!c||!render_ready())return false;
 if(!render_park_release_for(c,"close")){IOLog("X9 RENDER CLOSE: own parked group retire failed\n");return false;}
 if(c->exposed){IOLog("X9 RENDER CLOSE: context still exposed;retain\n");return false;}
 auto *p=(RenderKernelContext *)c->engine_private;if(!p)return x9_render_memory_close(c);
 for(auto *g:p->groups)if(g&&render_destroy_locked(c,g)!=kIOReturnSuccess)return false;
 if(!x9_render_memory_close(c))return false;
 if(p->dirty_lock)IOLockFree(p->dirty_lock);
 IOFree(p,sizeof(*p));c->engine_private=nullptr;return true;
}
static IOReturn render_timestamp(uint64_t *value) {
 RenderOwner owner;if(!owner.locked||!value||!render_ready())return kIOReturnNotReady;
 *value=gr64(resident->gpu,0x98);return kIOReturnSuccess;
}
static bool render_quiesce(X9RenderContext *c) {
 RenderOwner owner;if(!owner.locked||!render_ready()||!c)return false;
 if(!render_park_release_for(c,"quiesce"))return false;
 return !c->exposed;
}
/*
 * Asynchronous submission, opt-in x9_submit_async=1 (rev252aq; design:
 * docs/graphics/submission-decoupling-design.md, decisions 1/2 accepted).
 * One global FIFO and one kernel worker: global order makes cross-process
 * producer->consumer order (client renders a surface, then backboardd
 * composites it) follow enqueue order. The worker runs render_submit's
 * publish+reap under the context lock and the owner lock, exactly as the
 * synchronous selector did, so every cache, AS, CSG and fence rule is
 * unchanged; a completion means CPU-visible results. Callers return right
 * after enqueue. Global sequence numbers give the display a lock-free present
 * fence (sweet_x9_render_async_*). Lock order: c->lock -> owner lock (worker,
 * as before); render_async_lock is a leaf held without either, never across
 * publish/reap.
 * rev252cn (x9if): the worker may publish the next FIFO job of the SAME
 * context and group while the previous job still runs (parked-resume path,
 * second fence slot), then reaps both in FIFO order. The context lock is held
 * across the whole pair, so map/unmap and the v1 selectors keep their
 * never-mid-job guarantee; the x9dc record path is the only client call that
 * stays lock-free during a job. FLUSH_CACHES and the post-cache invalidate of
 * job 1 run while job 2 executes - device-validated under the boot-arg.
 */
struct RenderAsyncJob { X9RenderContext *c; X9RenderSubmission req; uint64_t seq; uint64_t t_enq, t_run; };
static constexpr unsigned render_async_depth=16;
static IOLock *render_async_lock;
static RenderAsyncJob render_async_jobs[render_async_depth];
static unsigned render_async_head,render_async_count;
static uint64_t render_async_enqueued,render_async_completed;
static bool render_async_running;
static uint64_t render_async_stats[4];   /* jobs, failures, queue-full waits, drains that waited */
static bool render_async_enabled() {return render_async_running;}
static void render_async_complete(RenderKernelContext *p,const RenderAsyncJob &job,IOReturn kr,const X9RenderCompletion &reply) {
 IOLockLock(render_async_lock);
 auto &d=p->async_done[p->async_next++%render_history];
 d.group=job.req.group;d.kr=kr;d.reply=reply;
 if(kr==kIOReturnSuccess)p->async_done_id=job.req.submission_id;else{p->async_failed=true;render_async_stats[1]++;}
 p->async_pending--;render_async_stats[0]++;
 __atomic_store_n(&render_async_completed,job.seq,__ATOMIC_RELEASE);
 IOLockWakeup(render_async_lock,&render_async_completed,false);
 IOLockUnlock(render_async_lock);
}
static void render_async_metrics(const RenderAsyncJob &job,IOReturn kr) {
 /* rev252bo metrics: single worker thread, so these need no lock beyond the
  * FIFO snapshot for the in-flight depth. queue = enqueue -> worker pop,
  * run = pop -> final completion, total = enqueue -> done, inflight =
  * enqueued-completed peak (2 under x9if when a pair is running). */
 static uint64_t w_start,w_jobs,w_fail,w_queue_ns,w_run_ns,w_total_max_ns,w_inflight_max;
 uint64_t now=mach_absolute_time(),q=0,r=0,t=0;
 absolutetime_to_nanoseconds(job.t_run-job.t_enq,&q);
 absolutetime_to_nanoseconds(now-job.t_run,&r);
 absolutetime_to_nanoseconds(now-job.t_enq,&t);
 if(!w_start)w_start=now;
 w_jobs++;w_queue_ns+=q;w_run_ns+=r;if(t>w_total_max_ns)w_total_max_ns=t;
 if(kr!=kIOReturnSuccess)w_fail++;
 uint64_t inflight=0;
 IOLockLock(render_async_lock);
 inflight=render_async_enqueued-render_async_completed;
 IOLockUnlock(render_async_lock);
 if(inflight>w_inflight_max)w_inflight_max=inflight;
 uint64_t el=0;absolutetime_to_nanoseconds(now-w_start,&el);
 if(el>=2000000000ULL){
  IOLog("X9 RENDER ASYNC window: ms=%llu jobs=%llu failed=%llu queue_avg_us=%llu run_avg_us=%llu enqueue_to_done_max_us=%llu inflight_max=%llu;per job, kernel FIFO only, not commit time\n",
   el/1000000ULL,w_jobs,w_fail,w_jobs?w_queue_ns/w_jobs/1000:0,w_jobs?w_run_ns/w_jobs/1000:0,w_total_max_ns/1000,w_inflight_max);
  w_jobs=w_fail=w_queue_ns=w_run_ns=w_total_max_ns=0;w_inflight_max=0;w_start=now;
 }
}
static void render_async_worker(void *,wait_result_t) {
 static RenderJobState states[2];   /* worker-only; phases run sequentially */
 for(;;) {
  IOLockLock(render_async_lock);
  while(!render_async_count)IOLockSleep(render_async_lock,&render_async_count,THREAD_UNINT);
  RenderAsyncJob job=render_async_jobs[render_async_head];
  render_async_head=(render_async_head+1)%render_async_depth;render_async_count--;
  IOLockWakeup(render_async_lock,&render_async_head,false);
  IOLockUnlock(render_async_lock);
  job.t_run=mach_absolute_time();   /* rev252bo: queue wait ends here */
  X9RenderContext *c=job.c;auto *p=(RenderKernelContext *)c->engine_private;
  X9RenderCompletion reply={};IOReturn kr;
  /* The context lock is held from publish through reap of the whole pair:
   * map/unmap and the v1 selectors keep their "never mid-job" guarantee, and
   * the x9dc record path is the only client op that stays lock-free. */
  IOLockLock(c->lock);
  bool failed;
  IOLockLock(render_async_lock);failed=p->async_failed;IOLockUnlock(render_async_lock);
  if(c->disabled||failed)kr=kIOReturnNotReady;
  else {
   RenderJobState *st=&states[0];
   *st=RenderJobState{};
   st->c=c;st->request=&job.req;st->reply=&reply;st->fence_slot=0;st->pipeline=false;
   kr=render_submit_publish(st);
   RenderAsyncJob job2={};bool pair=false;
   if(kr==kIOReturnSuccess&&render_pipeline_enabled()&&render_park_enabled()) {
    /* Pair with the next FIFO job when it is the same context and group:
     * its publish is the parked-resume path (no AS/CSG programming) and its
     * fence uses slot 1, so job 1's fence value survives until reaped. */
    IOLockLock(render_async_lock);
    if(render_async_count&&!p->async_failed) {
     auto &n=render_async_jobs[render_async_head];
     if(n.c==c&&n.req.group==job.req.group) {
      job2=n;render_async_head=(render_async_head+1)%render_async_depth;render_async_count--;
      IOLockWakeup(render_async_lock,&render_async_head,false);pair=true;
     }
    }
    IOLockUnlock(render_async_lock);
   }
   if(pair) {
    job2.t_run=mach_absolute_time();
    RenderJobState *s2=&states[1];
    *s2=RenderJobState{};
    X9RenderCompletion reply2={};
    s2->c=c;s2->request=&job2.req;s2->reply=&reply2;s2->fence_slot=1;s2->pipeline=true;
    render_parked={c,st->g,st->root};   /* arm the resume detection for job 2 */
    IOReturn kr2=render_submit_publish(s2);
    IOReturn kr1=render_submit_reap(st);
    if(kr1!=kIOReturnSuccess) {
     IOLog("X9 RENDER ASYNC: submission failed group=%u id=%llu kr=%x exposed=%u\n",job.req.group,job.req.submission_id,kr1,c->exposed);
     if(c->exposed)c->disabled=true;   /* DMA lifetime unresolved: no retry, as the sync selector */
    }
    render_async_complete(p,job,kr1,reply);
    render_async_metrics(job,kr1);
    /* Job 2: a failed publish keeps its own error; a failed job 1 reap leaves
     * the session uncertain, so job 2 completes as an error without reaping. */
    IOReturn krj2=kr2!=kIOReturnSuccess?kr2:(kr1!=kIOReturnSuccess?kIOReturnIOError:render_submit_reap(s2));
    if(krj2!=kIOReturnSuccess) {
     IOLog("X9 RENDER ASYNC: submission failed group=%u id=%llu kr=%x exposed=%u\n",job2.req.group,job2.req.submission_id,krj2,c->exposed);
     if(c->exposed)c->disabled=true;
    }
    render_async_complete(p,job2,krj2,reply2);
    render_async_metrics(job2,krj2);
   }else {
    if(kr==kIOReturnSuccess)kr=render_submit_reap(st);
    if(kr!=kIOReturnSuccess) {
     IOLog("X9 RENDER ASYNC: submission failed group=%u id=%llu kr=%x exposed=%u\n",job.req.group,job.req.submission_id,kr,c->exposed);
     if(c->exposed)c->disabled=true;   /* DMA lifetime unresolved: no retry, as the sync selector */
    }
    render_async_complete(p,job,kr,reply);
    render_async_metrics(job,kr);
   }
  }
  IOLockUnlock(c->lock);
  if(render_async_stats[0]<=4||render_async_stats[0]%600==0)
   IOLog("X9 RENDER ASYNC: jobs=%llu failed=%llu queue_full_waits=%llu drain_waits=%llu enqueued=%llu completed=%llu\n",
    render_async_stats[0],render_async_stats[1],render_async_stats[2],render_async_stats[3],
    render_async_enqueued,__atomic_load_n(&render_async_completed,__ATOMIC_ACQUIRE));
 }
}
static IOReturn render_submit_async(X9RenderContext *c,const X9RenderSubmission *request,X9RenderCompletion *reply) {
 if(!render_async_enabled()||!c||!c->engine_private||!request||!reply)return kIOReturnUnsupported;
 auto *p=(RenderKernelContext *)c->engine_private;
 if(c->disabled)return kIOReturnNotReady;
 /* Stage this submission's recorded dirty ranges (x9dc) under c->lock, before
  * the FIFO unlock: everything the client recorded before SubmitAsync belongs
  * to this submission, and records arriving later belong to the next one. */
 render_dirty_stage(p,request->submission_id);
 RenderAsyncJob job={c,*request,0,0,0};
 IOReturn kr=kIOReturnSuccess;
 IOLockUnlock(c->lock);   /* the worker may need c->lock to make room */
 IOLockLock(render_async_lock);
 if(render_async_count==render_async_depth)render_async_stats[2]++;
 while(render_async_count==render_async_depth&&!p->async_failed)IOLockSleep(render_async_lock,&render_async_head,THREAD_UNINT);
 if(p->async_failed)kr=kIOReturnNotReady;
 else {
  job.seq=__atomic_add_fetch(&render_async_enqueued,1,__ATOMIC_ACQ_REL);
  job.t_enq=mach_absolute_time();
  render_async_jobs[(render_async_head+render_async_count)%render_async_depth]=job;
  render_async_count++;p->async_pending++;
  IOLockWakeup(render_async_lock,&render_async_count,false);
 }
 IOLockUnlock(render_async_lock);
 IOLockLock(c->lock);
 if(kr==kIOReturnSuccess)*reply={X9_RENDER_ABI,0,request->group,0,request->submission_id,0,0,0,0};
 return kr;
}
static bool render_async_lockfree() {
 static int mode=-1;
 if(mode<0){mode=sweet_x9_arg("x9sl",0)==1?1:0;
  IOLog("X9 RENDER ASYNC: lock-free SubmitAsync %s (x9sl; needs x9_submit_async, off with x9dc)\n",mode?"ON":"off");}
 return mode==1&&render_async_enabled()&&!render_dirty_enabled();
}
static IOReturn render_submit_async_try(X9RenderContext *c,const X9RenderSubmission *request) {
 if(!render_async_enabled()||!c||!c->engine_private||!request)return kIOReturnUnsupported;
 auto *p=(RenderKernelContext *)c->engine_private;
 IOReturn kr=kIOReturnSuccess;
 IOLockLock(render_async_lock);
 if(p->async_failed)kr=kIOReturnNotReady;
 else if(render_async_count==render_async_depth){render_async_stats[2]++;kr=kIOReturnNoSpace;}
 else {
  RenderAsyncJob job={c,*request,0,0,0};
  job.seq=__atomic_add_fetch(&render_async_enqueued,1,__ATOMIC_ACQ_REL);
  job.t_enq=mach_absolute_time();
  render_async_jobs[(render_async_head+render_async_count)%render_async_depth]=job;
  render_async_count++;p->async_pending++;
  IOLockWakeup(render_async_lock,&render_async_count,false);
 }
 IOLockUnlock(render_async_lock);
 return kr;
}
static void render_async_wait_room(X9RenderContext *c) {
 if(!render_async_enabled()||!c||!c->engine_private)return;
 auto *p=(RenderKernelContext *)c->engine_private;
 IOLockLock(render_async_lock);
 while(render_async_count==render_async_depth&&!p->async_failed)IOLockSleep(render_async_lock,&render_async_head,THREAD_UNINT);
 IOLockUnlock(render_async_lock);
}
static IOReturn render_fence_wait(X9RenderContext *c,const X9RenderWaitRequest *request,X9RenderCompletion *reply) {
 if(!render_async_enabled()||!c||!c->engine_private||!request||!reply)return kIOReturnUnsupported;
 auto *p=(RenderKernelContext *)c->engine_private;
 uint64_t deadline=0;
 if(request->timeout_ns)clock_interval_to_deadline((uint32_t)(request->timeout_ns/1000),NSEC_PER_USEC,&deadline);
 IOReturn kr=kIOReturnTimeout;
 IOLockLock(render_async_lock);
 for(;;) {
  bool found=false;
  for(const auto &d:p->async_done)if(d.group==request->group&&d.reply.submission_id==request->submission_id&&d.reply.abi) {
   *reply=d.reply;kr=d.kr;found=true;break;
  }
  if(found)break;
  if(p->async_failed){kr=kIOReturnIOError;break;}
  if(request->submission_id<=p->async_done_id||
     (!p->async_pending&&request->submission_id<=__atomic_load_n(&c->completed_submission,__ATOMIC_ACQUIRE))) {
   *reply={X9_RENDER_ABI,0,request->group,0,request->submission_id,request->submission_id,0,0,0};
   kr=kIOReturnSuccess;break;
  }
  if(!p->async_pending){kr=kIOReturnNotFound;break;}
  if(!request->timeout_ns){kr=kIOReturnTimeout;break;}
  wait_result_t w=IOLockSleepDeadline(render_async_lock,&render_async_completed,deadline,THREAD_ABORTSAFE);
  if(w==THREAD_TIMED_OUT){kr=kIOReturnTimeout;break;}
  if(w!=THREAD_AWAKENED){kr=kIOReturnAborted;break;}
 }
 IOLockUnlock(render_async_lock);
 return kr;
}
static void render_async_drain_unlocked(RenderKernelContext *p) {
 if(!render_async_enabled()||!p)return;
 IOLockLock(render_async_lock);
 if(p->async_pending)render_async_stats[3]++;
 while(p->async_pending)IOLockSleep(render_async_lock,&render_async_completed,THREAD_UNINT);
 IOLockUnlock(render_async_lock);
}
static void render_drain(X9RenderContext *c) {
 if(!render_async_enabled()||!c||!c->engine_private)return;
 auto *p=(RenderKernelContext *)c->engine_private;
 IOLockLock(render_async_lock);bool busy=p->async_pending!=0;IOLockUnlock(render_async_lock);
 if(!busy)return;
 IOLockUnlock(c->lock);render_async_drain_unlocked(p);IOLockLock(c->lock);
}
/* osfmk/kern/thread.c; its prototype is XNU_KERNEL_PRIVATE in a header iokit does not see. */
extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t,void *,integer_t,thread_t *);
static bool render_async_start() {
 if(render_async_running)return true;
 if(sweet_x9_arg("x9_submit_async",0)!=1)return false;
 render_async_lock=IOLockAlloc();if(!render_async_lock)return false;
 thread_t thread;
 if(kernel_thread_start_priority(render_async_worker,nullptr,92 /* BASEPRI_PREEMPT (kern/sched.h, not exported to iokit) */,&thread)!=KERN_SUCCESS) {
  IOLockFree(render_async_lock);render_async_lock=nullptr;return false;
 }
 thread_deallocate(thread);render_async_running=true;
 IOLog("X9 RENDER ASYNC: worker started depth=%u;global FIFO,unchanged per-job sequence\n",render_async_depth);
 return true;
}
static const X9RenderEngine resident_engine={render_caps,render_context_open,render_close,render_map,render_unmap,
 render_create_group,render_destroy_group,render_submit,render_wait,render_cache,render_cache_defer,render_timestamp,render_quiesce,
 render_submit_async,render_fence_wait,render_drain,
 render_async_lockfree,render_submit_async_try,render_async_wait_room};
