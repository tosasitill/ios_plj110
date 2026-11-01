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
};
#include "X9RenderCachePlan.inc"
struct RenderKernelContext {
 RenderCachePlan cache_plan;
 bool cache_plan_valid;
 unsigned cache_plan_mappings;
 RenderKernelGroup *groups[8];
 X9RenderCompletion history[render_history];
 unsigned next_history;
 uint32_t next_group;
};
static bool render_engine_installed;
struct RenderOwner {
 bool locked;
 RenderOwner():locked(sweet_x9_render_owner_enter()){}
 ~RenderOwner(){if(locked)sweet_x9_render_owner_leave();}
};
static bool render_ready() {
 return render_engine_installed&&resident&&resident->ready&&!resident->uncertain&&no_faults(resident->gpu);
}
static RenderKernelGroup *render_group(X9RenderContext *c,uint32_t id) {
 auto *p=(RenderKernelContext *)c->engine_private;
 if(p)for(auto *g:p->groups)if(g&&g->request.handle==id)return g;
 return nullptr;
}
static uint64_t render_va(const RenderKernelGroup *g,unsigned stream) {
 return 0x10000000ULL+(uint64_t)g->slot*0x100000+stream*render_storage_bytes;
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
 return true;
}
static IOReturn render_context_open(X9RenderContext *c) {
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||c->engine_private)return kIOReturnNotReady;
 auto *p=(RenderKernelContext *)IOMallocZero(sizeof(RenderKernelContext));if(!p)return kIOReturnNoMemory;
 IOReturn kr=x9_render_memory_open(c);if(kr!=kIOReturnSuccess){IOFree(p,sizeof(*p));return kr;}
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
 return x9_render_memory_unmap(c,entry);
}
static IOReturn render_cache(X9RenderContext *c,X9RenderResource *r,const X9RenderCacheRequest *request) {
 RenderOwner owner;if(!owner.locked||!render_ready())return kIOReturnNotReady;
 return x9_render_memory_cache(c,r,request);
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
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||!request||!reply||!c->engine_private||c->exposed)return kIOReturnNotReady;
 auto *p=(RenderKernelContext *)c->engine_private;
 if(p->next_group==UINT32_MAX)return kIOReturnNoResources;
 unsigned entry=0;while(entry<8&&p->groups[entry])entry++;
 unsigned slot=0;while(slot<resident->slot_count&&resident->slots[slot].borrowed)slot++;
 if(entry==8||slot==resident->slot_count)return kIOReturnNoResources;
 auto *g=(RenderKernelGroup *)IOMallocZero(sizeof(RenderKernelGroup));if(!g)return kIOReturnNoMemory;
 g->slot=slot;g->request=*request;g->request.handle=++p->next_group;
 resident->slots[slot].borrowed=true;p->groups[entry]=g;
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
  X9CSFStreamBinding bindings[3];
  for(unsigned i=0;i<3;i++)bindings[i]={render_va(g,i),resident->slots[slot].user_io+i*host_page,
   resident->slots[slot].user_io+i*host_page+gpu_page,render_ring_bytes};
  X9CSFQueueIO io=x9_csf_queue_access_io(&resident->slots[slot].access);
  IOReturn kr=x9_csf_group_configure(&io,&g->control,&resident->slots[slot].access.interface,
   request,bindings,resident->slots[slot].suspend,resident->epoch);
  if(kr!=kIOReturnSuccess) {
   if(g->control.uncertain){resident->uncertain=true;c->disabled=true;return kr;}
   goto allocation_failure;
  }
 }
 *reply=g->request;return kIOReturnSuccess;
allocation_failure:
 if(!render_drop_storage(c,g)){c->disabled=true;return kIOReturnIOError;}
 resident->slots[slot].borrowed=false;p->groups[entry]=nullptr;IOFree(g,sizeof(*g));return kIOReturnNoMemory;
}
static IOReturn render_destroy_locked(X9RenderContext *c,RenderKernelGroup *g) {
 if(!render_ready()||c->exposed)return kIOReturnNotReady;
 auto &slot=resident->slots[g->slot];X9CSFQueueIO io=x9_csf_queue_access_io(&slot.access);
 /* A suspended CSG does not acknowledge a direct TERMINATE on this firmware
  * (pan192: global DB ACK matched; CSG REQ=0,ACK=0x30000002). Restore its
  * already-drained context before composing the existing RESUME and
  * CS STOP -> CSG TERMINATE handshakes. Never resume a faulted or partially executed job. */
 const bool resume_empty=g->control.suspended;
 if(g->control.uncertain||g->control.running)return kIOReturnNotReady;
 if(resume_empty) {
  vm_offset_t gpu=resident->gpu;
  if(!no_faults(gpu)||gr64(gpu,0x2440))return kIOReturnNotReady;
  for(unsigned i=0;i<resident->slot_count;i++) {
   unsigned state=output_word(resident->image,resident->slots[i].access.interface.group_output)&7;
   if(state==1||state==3)return kIOReturnBusy;
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
 auto *p=(RenderKernelContext *)c->engine_private;
 for(auto &entry:p->groups)if(entry==g){entry=nullptr;break;}
 IOFree(g,sizeof(*g));return kIOReturnSuccess;
}
static IOReturn render_destroy_group(X9RenderContext *c,uint32_t id) {
 RenderOwner owner;if(!owner.locked||!c)return kIOReturnNotReady;
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
static IOReturn render_submit(X9RenderContext *c,const X9RenderSubmission *request,X9RenderCompletion *reply) {
 uint64_t times[8]={mach_absolute_time(),0,0,0,0,0,0,0};
 uint64_t cache_bytes[2]={0,0},shader_ready_seen=0;
 uint64_t private_skip[2]={0,0},initial_bytes=0,fallback_bytes=0;
 uint64_t reach_saved[2]={0,0},import_bytes[2]={0,0},full_import_bytes[2]={0,0};
 uint64_t coherent_skip[2]={0,0};
 uint64_t wc_skip[2]={0,0};
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||!request||!reply||c->exposed)return kIOReturnNotReady;
 times[1]=mach_absolute_time();
 auto *g=render_group(c,request->group);if(!g||c->disabled)return kIOReturnBadArgument;
 vm_offset_t gpu=resident->gpu;
 const bool coherent=resident->coherent;
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
 const char *phase="storage-sync";
 if(gr64(gpu,0x2440))return kIOReturnBusy;
 for(unsigned i=0;i<resident->slot_count;i++) {
  unsigned state=output_word(resident->image,resident->slots[i].access.interface.group_output)&7;
  if(state==1||state==3)return kIOReturnBusy;
 }
 if(!c->engine_private||c->mapping_count>X9_RENDER_KERNEL_MAPPINGS)return kIOReturnBadArgument;
 auto *kernel_context=(RenderKernelContext *)c->engine_private;
 auto *plan=&kernel_context->cache_plan;
 bool plan_rebuilt=false;
 if(!kernel_context->cache_plan_valid||kernel_context->cache_plan_mappings!=c->mapping_count) {
  kernel_context->cache_plan_valid=false;
  if(!render_cache_bounds(c)||!render_cache_plan_build(c,plan))return kIOReturnBadArgument;
  kernel_context->cache_plan_mappings=c->mapping_count;
  kernel_context->cache_plan_valid=true;plan_rebuilt=true;
 }
 uint64_t holes_saved[2]={0,0};
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
 for(auto &r:c->resources)if(r.memory) {
  if(r.write_combined){wc_skip[0]+=r.bytes;continue;}   // never cached, see X9RenderResource
  if(coherent&&r.cpu_exposed) {
   coherent_skip[0]+=r.cache_end>r.cache_begin?r.cache_end-r.cache_begin:0;continue;
  }
  if(!r.cpu_exposed&&r.initial_cache_clean){private_skip[0]+=r.bytes;continue;}
  uint64_t bytes=0;
  IOReturn kr;
  if(r.cpu_exposed) {
   kr=render_cache_plan_resource(c,plan,r,0,&bytes);
   if(kr!=kIOReturnSuccess)return kr;
   reach_saved[0]+=r.bytes-bytes;fallback_bytes+=bytes;
   uint64_t bound=r.cache_end>r.cache_begin?r.cache_end-r.cache_begin:0;
   if(bytes>bound)return kIOReturnBadArgument;
   holes_saved[0]+=bound-bytes;
   if(r.imported){import_bytes[0]+=bytes;full_import_bytes[0]+=r.bytes;}
  }else {
   bytes=r.bytes;initial_bytes+=bytes;
   if(!bytes)continue;
   X9RenderCacheRequest cache={X9_RENDER_ABI,r.handle,X9RenderClean|X9RenderInvalidate,0,0,bytes};
   kr=x9_render_memory_cache(c,&r,&cache);if(kr!=kIOReturnSuccess)return kr;
  }
  cache_bytes[0]+=bytes;
 }
 times[2]=mach_absolute_time();
 unsigned active=0;uint64_t expected[3]={0,0,0};
 for(unsigned n=0;n<request->stream_count;n++) {
  const auto &stream=request->streams[n];unsigned i=stream.index;
  if(i>=3||!stream.bytes||(active&(1u<<i))||stream.bytes>1u<<20||g->producer[i]>UINT64_MAX-render_packet_bytes)return kIOReturnBadArgument;
  active|=1u<<i;expected[i]=g->producer[i]+render_packet_bytes;
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
  bzero(base+0x8000,16);bzero(base+0x8080,8);bzero(base+0x8100,8);bzero(base+0x8180,8);
  bzero(base+0x8200,8);bzero(base+0x8280,8);render_storage_clean(g->storage[i]);
 }
 if(!active)return kIOReturnBadArgument;
 uint64_t root=x9_render_memory_publish(c);if(!root)return kIOReturnNotReady;
 for(unsigned i=0;i<3;i++)if((active&(1u<<i))&&!render_range_sync(g->storage[i]))goto uncertain;
 times[3]=mach_absolute_time();
 phase="gpu-flush-and-as-install";
 if(!flush_gpu(gpu)||!install_as(gpu,root,1))goto uncertain;
 {
  auto &slot=resident->slots[g->slot];X9CSFQueueIO io=x9_csf_queue_access_io(&slot.access);
  phase="user-io-prepare";
  for(unsigned i=0;i<3;i++) {
   uint32_t va=slot.user_io+i*host_page;
   if(output_qword(resident->image,va+gpu_page)!=g->producer[i]||output_qword(resident->image,va)!=g->producer[i]||
      !put_qword(resident->image,va+8,g->producer[i])||!clean_page(resident->image,va))goto uncertain;
  }
  phase="group-start";
  if(x9_csf_group_start(&io,&g->control,resident->epoch,1000000000ULL)!=kIOReturnSuccess)goto uncertain;
  phase="stream-publish";
  for(unsigned i=0;i<3;i++)if(active&(1u<<i)) {
   uint32_t va=slot.user_io+i*host_page;
   if(!put_qword(resident->image,va,expected[i])||!clean_page(resident->image,va))goto uncertain;
  }
  /* ml_io_map() takes permanent VA from the fixed 8 MiB io_submap and is
   * never released: mapping per submit exhausted it after ~400 jobs and
   * panicked in kmem_alloc (KERN_NO_SPACE). Map the user doorbell page once;
   * submits are serialized by the render owner lock held here. */
  static vm_offset_t user_doorbell;
  if(!user_doorbell)user_doorbell=ml_io_map(0x48090000,0x1000);
  vm_offset_t db=user_doorbell;if(!db)goto uncertain;
  gw(db,0,1);phase="completion-wait";bool done=false;uint64_t end=deadline_ms(10000),next_sync=0;unsigned polls=0;
  times[4]=mach_absolute_time();
  do {
   if(x9_csf_group_health(&io,&g->control,resident->epoch)!=kIOReturnSuccess)goto uncertain;
   shader_ready_seen|=gr64(gpu,0xa08);
   /* The X9 GPU has shown that system-scope SYNC_SET can remain in GPU
    * cache: synchronize only our held private fence pages before polling.
    * This does not acquire client BOs or assert command completion. */
   if(mach_absolute_time()>=next_sync) {
    phase="fence-cache-acquire";
    for(unsigned i=0;i<3;i++)if((active&(1u<<i))&&!render_page_sync(g->storage[i],0x8000))goto uncertain;
    // pan204's short jobs waited ~10.4ms for the next cache acquisition.
    // Keep the same private-page sync and all fence/extract/guard checks.
    // While poll_pause() is still spinning, acquire on every poll so a short
    // job is seen within ~16us; after that, acquire at1ms as before.
    next_sync=polls<poll_spin_limit?0:deadline_ms(1);phase="completion-wait";
   }
   done=true;
   for(unsigned i=0;i<3;i++)if(active&(1u<<i)) {
    auto *fence=(uint8_t *)((IOBufferMemoryDescriptor *)g->storage[i].memory)->getBytesNoCopy()+0x8000;
    invalidate_cpu(fence,64);uint64_t actual=0;memcpy(&actual,fence,8);
    uint32_t sync_error=0;memcpy(&sync_error,fence+8,sizeof(sync_error));
    if(sync_error)goto uncertain;
    for(unsigned j=16;j<64;j++)if(fence[j]!=0xa5)goto uncertain;
    if(actual!=request->submission_id||output_qword(resident->image,slot.user_io+i*host_page+gpu_page)!=expected[i])done=false;
   }
   if(done)break;poll_pause(&polls);
  }while(mach_absolute_time()<end);
  if(!done)goto uncertain;
  times[5]=mach_absolute_time();
  phase="group-suspend";
  if(x9_csf_group_halt(&io,&g->control,resident->epoch,true,1000000000ULL)!=kIOReturnSuccess)goto uncertain;
  phase="final-flush-and-as-detach";
  if(!flush_gpu(gpu)||!remove_as(gpu,resident->policy.saved_attr,resident->policy.saved_cfg,1))goto uncertain;
 }
 x9_render_memory_detached(c);
 times[6]=mach_absolute_time();
 for(unsigned i=0;i<3;i++)if(active&(1u<<i))g->producer[i]=expected[i];
 for(auto &r:c->resources)if(r.memory&&(r.flags&X9RenderGPUWrite)) {
  if(r.write_combined){wc_skip[1]+=r.bytes;continue;}
  if(coherent&&r.cpu_exposed) {
   coherent_skip[1]+=r.write_end>r.write_begin?r.write_end-r.write_begin:0;continue;
  }
  // No CPU consumer exists for a never-exposed allocation. Keep GPU flush,
  // CSG suspend and AS detach above; a fence alone does not return ownership.
  if(!r.cpu_exposed){private_skip[1]+=r.bytes;continue;}
  uint64_t bytes=0;
  IOReturn kr=render_cache_plan_resource(c,plan,r,1,&bytes);
  if(kr!=kIOReturnSuccess){c->disabled=true;resident->uncertain=true;return kr;}
  reach_saved[1]+=r.bytes-bytes;
  uint64_t bound=r.write_end>r.write_begin?r.write_end-r.write_begin:0;
  if(bytes>bound){c->disabled=true;resident->uncertain=true;return kIOReturnBadArgument;}
  holes_saved[1]+=bound-bytes;
  if(r.imported){import_bytes[1]+=bytes;full_import_bytes[1]+=r.bytes;}
  cache_bytes[1]+=bytes;
 }
 times[7]=mach_absolute_time();
 *reply={X9_RENDER_ABI,0,request->group,0,request->submission_id,request->submission_id,0,0,0};
 c->completed_submission=request->submission_id;
 {
  auto *p=(RenderKernelContext *)c->engine_private;p->history[p->next_history++%render_history]=*reply;
 }
 // Low-frequency timings, under the render owner lock. These include driver
 // cache/firmware waits and are NOT shader execution times or desktop FPS.
 {
  static uint64_t began=0,count=0,total[7]={},max_ns=0;
  static uint64_t cache_total[2]={0,0},ready_mask=0;
  static uint64_t skipped_total[2]={0,0},initial_total=0,fallback_total=0;
  static uint64_t reach_total[2]={0,0},import_total[2]={0,0},full_import_total[2]={0,0};
  static uint64_t coherent_total[2]={0,0};
  static uint64_t wc_total[2]={0,0};
  static uint64_t hole_total[2]={0,0},span_total[2]={0,0};
  static uint64_t plan_rebuilds=0,plan_reuses=0;
  if(!began)began=times[0];
  uint64_t elapsed=0,whole=0;
  absolutetime_to_nanoseconds(times[7]-times[0],&whole);
  if(whole>max_ns)max_ns=whole;
  for(unsigned i=0;i<7;i++){uint64_t ns=0;absolutetime_to_nanoseconds(times[i+1]-times[i],&ns);total[i]+=ns;}
  if(plan_rebuilt)plan_rebuilds++;else plan_reuses++;
  for(unsigned i=0;i<2;i++){hole_total[i]+=holes_saved[i];span_total[i]+=plan->count[i];}
  count++;absolutetime_to_nanoseconds(times[7]-began,&elapsed);
  cache_total[0]+=cache_bytes[0];cache_total[1]+=cache_bytes[1];ready_mask|=shader_ready_seen;
  skipped_total[0]+=private_skip[0];skipped_total[1]+=private_skip[1];
  initial_total+=initial_bytes;fallback_total+=fallback_bytes;
  for(unsigned i=0;i<2;i++){reach_total[i]+=reach_saved[i];import_total[i]+=import_bytes[i];full_import_total[i]+=full_import_bytes[i];coherent_total[i]+=coherent_skip[i];wc_total[i]+=wc_skip[i];}
  if(elapsed>=2000000000ULL) {
   IOLog("X9 RENDER timing: elapsed_ms=%llu jobs=%llu owner_us=%llu pre_cache_us=%llu storage_us=%llu start_us=%llu wait_us=%llu retire_us=%llu post_cache_us=%llu max_job_us=%llu;averages,not shader time\n",
    elapsed/1000000,count,total[0]/count/1000,total[1]/count/1000,total[2]/count/1000,total[3]/count/1000,
    total[4]/count/1000,total[5]/count/1000,total[6]/count/1000,max_ns/1000);
   IOLog("X9 RENDER cache: jobs=%llu pre_bytes_avg=%llu post_bytes_avg=%llu shader_ready_seen=%llx allowed=%llx;READY snapshots,not utilization\n",
    count,cache_total[0]/count,cache_total[1]/count,ready_mask,resident->caps.shader_present);
   IOLog("X9 RENDER cache-scope: jobs=%llu private_pre_skip_avg=%llu private_post_skip_avg=%llu initial_clean_avg=%llu full_fallback_avg=%llu;exact union of all GPU mappings,indirect included\n",
    count,skipped_total[0]/count,skipped_total[1]/count,initial_total/count,fallback_total/count);
   IOLog("X9 RENDER cache-reach: jobs=%llu pre_skip_avg=%llu post_skip_avg=%llu import_pre_avg=%llu import_pre_full_avg=%llu import_post_avg=%llu import_post_full_avg=%llu;kernel mapping bounds,not dirty tracking\n",
    count,reach_total[0]/count,reach_total[1]/count,import_total[0]/count,full_import_total[0]/count,import_total[1]/count,full_import_total[1]/count);
   IOLog("X9 RENDER cache-coherent: jobs=%llu active=%u pre_skip_avg=%llu post_skip_avg=%llu;opt-in skips CIVAC on exposed client bytes,assumes GPU snoop\n",
    count,coherent?1:0,coherent_total[0]/count,coherent_total[1]/count);
   IOLog("X9 RENDER cache-manifest: jobs=%llu pre_mapped_spans_avg=%llu post_mapped_spans_avg=%llu pre_holes_saved_avg=%llu post_holes_saved_avg=%llu;all mapped indirect targets included,shared aliases retained\n",
    count,span_total[0]/count,span_total[1]/count,hole_total[0]/count,hole_total[1]/count);
   IOLog("X9 RENDER cache-plan: builds=%llu reuses=%llu;mapping mutation invalidates,shared cache ops still per submission\n",plan_rebuilds,plan_reuses);
   plan_rebuilds=plan_reuses=0;
   for(unsigned i=0;i<2;i++)hole_total[i]=span_total[i]=0;
   IOLog("X9 RENDER cache-wc: jobs=%llu pre_skip_avg=%llu post_skip_avg=%llu;write-combined BOs need no CPU maintenance\n",
    count,wc_total[0]/count,wc_total[1]/count);
   for(unsigned i=0;i<2;i++)reach_total[i]=import_total[i]=full_import_total[i]=coherent_total[i]=wc_total[i]=0;
   cache_total[0]=cache_total[1]=ready_mask=0;
   skipped_total[0]=skipped_total[1]=initial_total=fallback_total=0;
   began=times[7];count=0;bzero(total,sizeof(total));max_ns=0;
  }
 }
 if(request->submission_id<=4||request->submission_id%120==0)
  IOLog("X9 RENDER GPU: group=%u sequence=%llu streams=%u independent fences/extract/guards;CSG suspended,AS1 detached,CPU ownership returned\n",
   request->group,request->submission_id,request->stream_count);
 return kIOReturnSuccess;
uncertain:
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
   auto *fence=(uint8_t *)((IOBufferMemoryDescriptor *)g->storage[i].memory)->getBytesNoCopy()+0x8000;
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
static IOReturn render_wait(X9RenderContext *c,const X9RenderWaitRequest *request,X9RenderCompletion *reply) {
 RenderOwner owner;if(!owner.locked||!render_ready()||!c||!request||!reply||!c->engine_private)return kIOReturnNotReady;
 auto *p=(RenderKernelContext *)c->engine_private;
 for(const auto &r:p->history)if(r.group==request->group&&r.submission_id==request->submission_id){*reply=r;return kIOReturnSuccess;}
 return kIOReturnNotFound;
}
static bool render_close(X9RenderContext *c) {
 RenderOwner owner;if(!owner.locked||!c||c->exposed||!render_ready())return false;
 auto *p=(RenderKernelContext *)c->engine_private;if(!p)return x9_render_memory_close(c);
 for(auto *g:p->groups)if(g&&render_destroy_locked(c,g)!=kIOReturnSuccess)return false;
 if(!x9_render_memory_close(c))return false;
 IOFree(p,sizeof(*p));c->engine_private=nullptr;return true;
}
static IOReturn render_timestamp(uint64_t *value) {
 RenderOwner owner;if(!owner.locked||!value||!render_ready())return kIOReturnNotReady;
 *value=gr64(resident->gpu,0x98);return kIOReturnSuccess;
}
static bool render_quiesce(X9RenderContext *c) {
 RenderOwner owner;return owner.locked&&render_ready()&&c&&!c->exposed;
}
static const X9RenderEngine resident_engine={render_caps,render_context_open,render_close,render_map,render_unmap,
 render_create_group,render_destroy_group,render_submit,render_wait,render_cache,render_timestamp,render_quiesce};
