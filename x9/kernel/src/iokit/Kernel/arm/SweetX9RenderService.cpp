/* SPDX-License-Identifier: MIT */
#include "X9RenderKernel.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
extern "C" int sweet_x9_arg(const char *name, int def);
#include <kern/clock.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <vm/vm_map.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
extern "C" vm_map_address_t phystokv(uint64_t pa);
#include <vm/memory_types.h>
extern "C" unsigned int pmap_cache_attributes(ppnum_t pn);
extern "C" int sweet_x9_render_memlimit(task_t owner);
/* A fresh write-combined BO's pages were zero-filled by the VM through the
 * cacheable physical aperture. Whether those lines were written back before
 * the pages turned non-cacheable is not something to rely on: with this
 * allocation the first 3-stream job hung on a sync object (stream 2 WAIT
 * 0xc1000000), consistent with the GPU reading stale DRAM, and a dirty line
 * evicted later would overwrite GPU results. Clean+invalidate every line of
 * the BO's physical pages by DC CIVAC through the physical aperture (the
 * maintenance applies to the PA whatever the alias), not through
 * flush_dcache64, which some XNU configurations compile to a no-op.
 * Afterwards DRAM holds the zeros and no CPU cache holds the pages. */
static bool render_wc_scrub(IOMemoryDescriptor *m,uint64_t bytes) {
 uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 const uint64_t line=4ULL<<((ctr>>16)&15);
 __asm__ volatile("dsb sy":::"memory");
 for(uint64_t off=0;off<bytes;) {
  IOByteCount span=0;uint64_t pa=m->getPhysicalSegment(off,&span,kIOMemoryMapperNone);
  if(!pa||!span||(pa&(line-1)))return false;
  if(span>bytes-off)span=bytes-off;
  uint64_t va=phystokv(pa);if(!va)return false;
  for(uint64_t at=va;at<va+span;at+=line)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
  off+=span;
 }
 __asm__ volatile("dsb sy":::"memory");
 return true;
}
static const X9RenderEngine *render_engine;
static bool render_published;
extern "C" { int sweet_x9_render_payload_ready=0; }
/* Observations only: each lookup locks the map independently, so this is not
 * an atomic snapshot and must never be used to authorize an import. Inspect
 * every bounded submap level because each parent's protection also matters. */
static void render_import_regions(task_t owner,uint64_t address,uint64_t bytes,uint32_t flags) {
 static uint32_t refusals;
 uint32_t ticket=__atomic_load_n(&refusals,__ATOMIC_RELAXED);
 do {if(ticket>=8)return;}
 while(!__atomic_compare_exchange_n(&refusals,&ticket,ticket+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
 vm_map_t map=get_task_map_reference(owner);
 if(!map) {IOLog("X9 RENDER IMPORT VM: owner pid=%d no map\n",pid_from_task(owner));return;}
 uint64_t at=address,end=address+bytes;
 IOLog("X9 RENDER IMPORT VM: sample=%u owner_pid=%d caller_pid=%d start=%llx bytes=%llu flags=%x non-atomic\n",
  ticket,pid_from_task(owner),proc_selfpid(),(unsigned long long)address,(unsigned long long)bytes,flags);
 unsigned spans=0;
 while(at<end&&spans<12) {
  uint64_t next=end;
  bool valid=true;
  bool leaf=false;
  for(unsigned level=0;level<8;level++) {
   mach_vm_address_t start=at;mach_vm_size_t size=0;
   natural_t depth=level;
   vm_region_submap_short_info_data_64_t info={};
   mach_msg_type_number_t count=VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
   kern_return_t kr=mach_vm_region_recurse(map,&start,&size,&depth,
    (vm_region_recurse_info_t)&info,&count);
   IOLog("X9 RENDER IMPORT VM: sample=%u offset=%llu level=%u kr=%x start=%llx size=%llu depth=%u prot=%x max=%x submap=%u tag=%u\n",
    ticket,(unsigned long long)(at-address),level,kr,
    (unsigned long long)start,(unsigned long long)size,depth,info.protection,info.max_protection,info.is_submap,info.user_tag);
   if(kr!=KERN_SUCCESS||start>at||!size||start+size<start||start+size<=at) {valid=false;break;}
   if(start+size<next)next=start+size;
   if(!info.is_submap) {leaf=true;break;}
  }
  spans++;
  if(!valid||!leaf||next<=at)break;
  at=next;
 }
 IOLog("X9 RENDER IMPORT VM: sample=%u covered=%llu of=%llu spans=%u%s\n",ticket,
  (unsigned long long)(at-address),(unsigned long long)bytes,spans,at<end?" incomplete":"");
 vm_map_deallocate(map);
}
/* Invoked by the existing accelerator's caller-scoped property lookup.
 * Desktop discovery may precede the async GPU worker: wait for actual service
 * registration, without holding the GPU owner lock or creating a fake device. */
extern "C" bool sweet_x9_render_discovery_ready(void) {
 int mode=0;
 if(!__atomic_load_n(&sweet_x9_render_payload_ready,__ATOMIC_ACQUIRE)||
    (mode=sweet_x9_arg("x9_metal_render",1))!=1)return false;
 for(unsigned ms=0;ms<10000;ms++) {
  if(__atomic_load_n(&render_published,__ATOMIC_ACQUIRE))return true;
  IOSleep(1);
 }
 IOLog("X9 RENDER DISCOVERY: native service not registered within10s;no device returned\n");
 return false;
}
static bool render_caps_ready(X9RenderCaps *caps) {
 if(!render_engine||!render_engine->caps||!render_engine->submit||!render_engine->wait||
    !render_engine->group_create||!render_engine->group_destroy||!render_engine->cache||
    !render_engine->timestamp||!render_engine->quiesce||!render_engine->context_open||
    !render_engine->context_close||!render_engine->map||!render_engine->unmap)return false;
 bzero(caps,sizeof(*caps));
 if(!render_engine->caps(caps))return false;
 unsigned required=X9RenderBufferObjects|X9RenderHostImport|X9RenderCSFSubmit|X9RenderTiler|X9RenderFragment;
 return caps->abi==X9_RENDER_ABI&&caps->bytes==sizeof(*caps)&&caps->gpu_id==0xe8800010ULL&&
  caps->host_page_bytes==16384&&caps->max_streams==3&&caps->max_mappings==X9_RENDER_MAX_MAPPINGS&&
  (caps->capabilities&required)==required&&caps->va_start==X9_RENDER_MIN_VA&&caps->va_end==X9_RENDER_MAX_VA&&
  caps->max_buffer_bytes==X9_RENDER_BUFFER_LIMIT&&caps->max_client_bytes==X9_RENDER_CLIENT_LIMIT;
}
class SweetX9RenderUserClient : public IOUserClient {
 OSDeclareDefaultStructors(SweetX9RenderUserClient);
 X9RenderContext *context=nullptr;
 static bool good_flags(uint32_t flags) {return flags&&(flags&~7u)==0&&(!(flags&4u)||!(flags&2u));}
 void footprint(const char *phase) {
  if(!context||!context->owner)return;
  IOLog("X9 RENDER FOOTPRINT: phase=%s pid=%d native_bytes=%llu footprint=%llu limit=%llu;task ledger,native allocation bytes separate\n",
   phase,pid_from_task(context->owner),(unsigned long long)context->allocated_bytes,
   (unsigned long long)get_task_phys_footprint(context->owner),
   (unsigned long long)get_task_phys_footprint_limit(context->owner));
 }
 /* Kernel-side budget. The caps keep reporting X9_RENDER_CLIENT_LIMIT, which
  * plugins built before this change require to be exactly 512 MiB; enforcement
  * may be larger. backboardd reached 512 MiB within a minute of animations and
  * never recovered from the refusals (no desktop until reboot). Write-combined
  * BOs need no CPU maintenance, so a larger budget does not lengthen submits.
  * x9_gpu_client_mb=<MiB> overrides; never below the reported limit. */
 static uint64_t client_budget() {
  static uint64_t budget;
  if(!budget) {
   uint32_t mb=1024;PE_parse_boot_argn("x9_gpu_client_mb",&mb,sizeof(mb));
   uint64_t bytes=(uint64_t)mb<<20;
   budget=bytes<X9_RENDER_CLIENT_LIMIT?X9_RENDER_CLIENT_LIMIT:bytes;
  }
  return budget;
 }
 static bool write_combined_bos() {
  static int enabled=-1;
  if(enabled<0){int arg=1;PE_parse_boot_argn("x9_bo_wc",&arg,sizeof(arg));enabled=arg!=0;}
  return enabled!=0;
 }
 /* Every 2 s per client (and on a budget refusal): what is alive, how much
  * came and went, and how much has stayed for 30 s or more, to tell a leak
  * from a high working set. */
 void live_report(const char *why,bool force) {
  uint64_t now=mach_absolute_time(),period=0,old=0;
  nanoseconds_to_absolutetime(2000000000ULL,&period);nanoseconds_to_absolutetime(30000000000ULL,&old);
  if(!force&&context->live_report_at&&now-context->live_report_at<period)return;
  context->live_report_at=now;
  uint32_t count=0,wc=0,imported=0,aged=0,big=0;uint64_t bytes=0,wc_bytes=0,imported_bytes=0,aged_bytes=0;
  for(auto &r:context->resources)if(r.memory) {
   count++;bytes+=r.bytes;
   if(r.write_combined){wc++;wc_bytes+=r.bytes;}
   if(r.imported){imported++;imported_bytes+=r.bytes;}
   if(r.created&&now-r.created>=old){aged++;aged_bytes+=r.bytes;}
   if(r.bytes>=(8u<<20))big++;
  }
  IOLog("X9 RENDER LIVE: pid=%d why=%s resources=%u MiB=%llu wc=%u/%lluMiB imported=%u/%lluMiB big8M=%u old30s=%u/%lluMiB "
   "window alloc=%u/%lluMiB release=%u/%lluMiB mappings=%u budget=%lluMiB\n",
   pid_from_task(context->owner),why,count,bytes>>20,wc,wc_bytes>>20,imported,imported_bytes>>20,big,aged,aged_bytes>>20,
   context->window_allocs,context->window_alloc_bytes>>20,context->window_releases,context->window_release_bytes>>20,
   context->mapping_count,client_budget()>>20);
  context->window_allocs=context->window_releases=0;context->window_alloc_bytes=context->window_release_bytes=0;
 }
 X9RenderResource *resource(uint32_t handle) {
  for(auto &r:context->resources)if(r.memory&&r.handle==handle)return &r;
  return nullptr;
 }
 bool mapping_exists(X9RenderResource *r) {
  for(unsigned i=0;i<context->mapping_count;i++)if(context->mappings[i].resource==r)return true;
  return false;
 }
 bool stream_mapped(const X9RenderStream &stream) {
  if(stream.index>=3||stream.reserved||!stream.bytes||(stream.gpu_address&7)||(stream.bytes&7)||stream.bytes>(1u<<20))return false;
  for(unsigned i=0;i<context->mapping_count;i++) {
   const auto &m=context->mappings[i];
   if((m.flags&X9RenderGPURead)&&stream.gpu_address>=m.address&&
      stream.gpu_address-m.address<=m.bytes&&stream.bytes<=m.bytes-(stream.gpu_address-m.address))return true;
  }
  return false;
 }
 IOReturn operation(uint32_t selector,IOExternalMethodArguments *a) {
  if(selector==X9RenderGetCaps) {
   X9RenderCaps caps;
   if(a->structureInputSize||a->structureOutputSize!=sizeof(caps))return kIOReturnBadArgument;
   if(!render_caps_ready(&caps))return kIOReturnNotReady;
   memcpy(a->structureOutput,&caps,sizeof(caps));return kIOReturnSuccess;
  }
  if(context->disabled) {
   IOLog("X9 RENDER CLIENT: disabled operation=%u exposed=%u\n",selector,context->exposed);
   return kIOReturnNotReady;
  }
  if(selector==X9RenderAllocate||selector==X9RenderImportHost) {
   if(a->structureInputSize!=sizeof(X9RenderAllocation)||a->structureOutputSize!=sizeof(X9RenderBuffer))return kIOReturnBadArgument;
   const auto &request=*(const X9RenderAllocation *)a->structureInput;
   if(request.abi!=X9_RENDER_ABI||!good_flags(request.flags)||!request.bytes||(request.bytes&16383)||
      request.bytes>X9_RENDER_BUFFER_LIMIT) {
    IOLog("X9 RENDER MEM: refuse size=%llu used=%llu limit=%llu buffer_limit=%llu flags=%x abi=%x\n",
      (unsigned long long)request.bytes,(unsigned long long)context->allocated_bytes,(unsigned long long)X9_RENDER_CLIENT_LIMIT,(unsigned long long)X9_RENDER_BUFFER_LIMIT,request.flags,request.abi);
    return kIOReturnBadArgument;
   }
   if(request.bytes>=(4u<<20))IOLog("X9 RENDER MEM: alloc size=%llu used=%llu\n",(unsigned long long)request.bytes,(unsigned long long)context->allocated_bytes);
   if(selector==X9RenderAllocate&&request.host_address)return kIOReturnBadArgument;
   if(selector==X9RenderImportHost&&(!request.host_address||(request.host_address&16383)||request.host_address+request.bytes<request.host_address||
      request.host_address>=0x0000800000000000ULL||request.bytes>0x0000800000000000ULL-request.host_address))return kIOReturnBadArgument;
   // Budget exhaustion is not a malformed request or a lost GPU. Nothing has
   // been pinned, published or exposed yet: userspace may free and retry.
   if(context->allocated_bytes>client_budget()||request.bytes>client_budget()-context->allocated_bytes) {
    IOLog("X9 RENDER MEM: budget size=%llu used=%llu limit=%llu;no handle published,recoverable\n",
     (unsigned long long)request.bytes,(unsigned long long)context->allocated_bytes,(unsigned long long)client_budget());
    live_report("budget",true);
    return kIOReturnNoMemory;
   }
   unsigned slot=0;while(slot<X9_RENDER_KERNEL_MAPPINGS&&context->resources[slot].memory)slot++;
   if(slot==X9_RENDER_KERNEL_MAPPINGS||context->next_handle==UINT32_MAX) {
    IOLog("X9 RENDER MEM: no slot for size=%llu used=%llu\n",(unsigned long long)request.bytes,(unsigned long long)context->allocated_bytes);
    return kIOReturnNoResources;
   }
   // Direction is from the device's perspective: Out pins pages for GPU reads.
   // Never downgrade a requested write here. The caller must retry explicitly
   // with GPURead after a no-handle protection refusal, and retain that grant.
   IODirection direction=selector==X9RenderImportHost&&!(request.flags&X9RenderGPUWrite)?
    kIODirectionOut:kIODirectionInOut;
   const bool wc=selector==X9RenderAllocate&&write_combined_bos();
   IOMemoryDescriptor *memory=selector==X9RenderAllocate?
    (wc?IOBufferMemoryDescriptor::inTaskWithOptions(TASK_NULL,
      kIODirectionInOut|kIOMemoryMapperNone|kIOMemoryPageable|kIOMapWriteCombineCache,request.bytes,16384):
     IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut|kIOMemoryKernelUserShared,request.bytes,16384)):
    IOMemoryDescriptor::withAddressRange(request.host_address,request.bytes,direction,context->owner);
   if(!memory) {
    IOLog("X9 RENDER MEM: kernel allocation failed size=%llu used=%llu\n",(unsigned long long)request.bytes,(unsigned long long)context->allocated_bytes);
    return kIOReturnNoMemory;
   }
   if(memory->getLength()!=request.bytes) {
    IOLog("X9 RENDER MEM: descriptor length mismatch op=%u requested=%llu actual=%llu\n",selector,
     (unsigned long long)request.bytes,(unsigned long long)memory->getLength());
    memory->release();return kIOReturnBadArgument;
   }
   IOReturn kr=memory->prepare(direction);
   if(kr!=kIOReturnSuccess) {
    IOLog("X9 RENDER IMPORT: prepare refused op=%u host=%llx bytes=%llu kr=%x;no handle published\n",
     selector,(unsigned long long)request.host_address,(unsigned long long)request.bytes,kr);
    if(selector==X9RenderImportHost&&kr==KERN_PROTECTION_FAILURE)
     render_import_regions(context->owner,request.host_address,request.bytes,request.flags);
    memory->release();
    return selector==X9RenderImportHost&&kr==KERN_PROTECTION_FAILURE?kIOReturnNotPermitted:kr;
   }
   // A write-combined BO has no kernel mapping; its pageable pages come from
   // the VM zero-filled. Scrub them once (render_wc_scrub) instead of bzero.
   if(selector==X9RenderAllocate&&!wc)bzero(((IOBufferMemoryDescriptor *)memory)->getBytesNoCopy(),request.bytes);
   if(wc) {
    if(!render_wc_scrub(memory,request.bytes)) {
     IOLog("X9 RENDER MEM: write-combined scrub failed size=%llu;no handle published\n",(unsigned long long)request.bytes);
     memory->complete(direction);memory->release();return kIOReturnNoMemory;
    }
    static unsigned told;
    if(told<4) {
     told++;
     IOByteCount span=0;uint64_t pa=memory->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
     IOLog("X9 RENDER WC: size=%llu first_pa=%llx wimg=%x (VM_WIMG_WCOMB=6 expected)\n",
      (unsigned long long)request.bytes,(unsigned long long)pa,pa?pmap_cache_attributes((ppnum_t)(pa>>PAGE_SHIFT)):0xffffffffu);
    }
   }
   /* An imported host range whose every physical page is write-combined
    * (e.g. SweetSurface IOSurfaces) has no cacheable alias: the WIMG mode is
    * per physical page and shared by all its mappings. Treat it like a WC BO
    * and skip per-submission CIVAC (rev246: ~82 MB cleaned and ~47 MB
    * invalidated per job across all imports). One cacheable page keeps the
    * full maintenance. Boot token x9iw disables this. */
   bool import_wc=false;
   if(selector==X9RenderImportHost) {
    static int off=-1;if(off<0){int v=0;off=PE_parse_boot_argn("x9iw",&v,sizeof(v))?1:0;}
    import_wc=!off;
    for(uint64_t at=0;import_wc&&at<request.bytes;) {
     IOByteCount span=0;uint64_t pa=memory->getPhysicalSegment(at,&span,kIOMemoryMapperNone);
     if(!pa||!span){import_wc=false;break;}
     if(span>request.bytes-at)span=request.bytes-at;
     for(uint64_t page=pa&~((uint64_t)PAGE_MASK);page<pa+span;page+=PAGE_SIZE)
      if((pmap_cache_attributes((ppnum_t)(page>>PAGE_SHIFT))&VM_WIMG_MASK)!=VM_WIMG_WCOMB){import_wc=false;break;}
     at+=span;
    }
    static uint64_t imports=0,wc_imports=0,wc_bytes=0,cached_bytes=0;
    imports++;if(import_wc){wc_imports++;wc_bytes+=request.bytes;}else cached_bytes+=request.bytes;
    if(imports<=4||!(imports&63))
     IOLog("X9 RENDER IMPORT WC: imports=%llu wc=%llu wc_bytes=%llu cached_bytes=%llu last=%llu/%s%s\n",imports,wc_imports,wc_bytes,cached_bytes,
      (unsigned long long)request.bytes,import_wc?"wc":"cached",off?" (x9iw: skip disabled)":"");
   }
   uint32_t handle=++context->next_handle;
   context->resources[slot]={memory,request.bytes,handle,request.flags,true,selector==X9RenderImportHost,false,selector==X9RenderImportHost};
   context->resources[slot].write_combined=wc||import_wc;
   context->resources[slot].initial_cache_clean=wc||import_wc;
   context->resources[slot].created=mach_absolute_time();
   context->allocated_bytes+=request.bytes;
   context->window_allocs++;context->window_alloc_bytes+=request.bytes;
   live_report("periodic",false);
   if(request.bytes>=(4u<<20))footprint(selector==X9RenderImportHost?"import":"allocate");
   X9RenderBuffer reply={X9_RENDER_ABI,handle,handle,request.flags,request.bytes};
   memcpy(a->structureOutput,&reply,sizeof(reply));return kIOReturnSuccess;
  }
  if(selector==X9RenderRelease) {
   if(a->structureInputSize!=sizeof(X9RenderHandle)||a->structureOutputSize)return kIOReturnBadArgument;
   const auto &request=*(const X9RenderHandle *)a->structureInput;auto *r=resource(request.handle);
   if(request.abi!=X9_RENDER_ABI||!r)return kIOReturnBadArgument;
   if(context->exposed||mapping_exists(r))return kIOReturnBusy;
   if(r->cache_mapping){r->cache_mapping->release();r->cache_mapping=nullptr;}
   if(r->prepared)r->memory->complete(r->memory->getDirection());
   r->memory->release();context->allocated_bytes-=r->bytes;
   context->window_releases++;context->window_release_bytes+=r->bytes;
   bzero(r,sizeof(*r));return kIOReturnSuccess;
  }
  if(selector==X9RenderMap||selector==X9RenderUnmap) {
   if(a->structureInputSize!=sizeof(X9RenderMapping)||a->structureOutputSize)return kIOReturnBadArgument;
   const auto &request=*(const X9RenderMapping *)a->structureInput;auto *r=resource(request.handle);
   if(request.abi!=X9_RENDER_ABI||!r||request.reserved||!good_flags(request.flags)||
      (request.flags&~r->flags)||!request.bytes||((request.buffer_offset|request.gpu_address|request.bytes)&4095)||
      request.buffer_offset>r->bytes||request.bytes>r->bytes-request.buffer_offset||
      request.gpu_address<X9_RENDER_MIN_VA||request.gpu_address>=X9_RENDER_MAX_VA||request.bytes>X9_RENDER_MAX_VA-request.gpu_address)return kIOReturnBadArgument;
   if(context->exposed)return kIOReturnBusy;
   if(selector==X9RenderUnmap) {
    for(unsigned i=0;i<context->mapping_count;i++) {
     auto &m=context->mappings[i];
     if(m.resource==r&&m.offset==request.buffer_offset&&m.address==request.gpu_address&&m.bytes==request.bytes&&m.flags==request.flags) {
      IOReturn kr=render_engine->unmap(context,&m);
      if(kr!=kIOReturnSuccess)return kr;
      m=context->mappings[--context->mapping_count];return kIOReturnSuccess;
     }
    }
    return kIOReturnBadArgument;
   }
   if(context->mapping_count==X9_RENDER_KERNEL_MAPPINGS)return kIOReturnNoResources;
   for(unsigned i=0;i<context->mapping_count;i++) {
    const auto &m=context->mappings[i];
    if(request.gpu_address<m.address+m.bytes&&m.address<request.gpu_address+request.bytes)return kIOReturnBadArgument;
   }
   for(uint64_t offset=0;offset<request.bytes;offset+=4096) {
    IOByteCount span=0;uint64_t pa=r->memory->getPhysicalSegment(request.buffer_offset+offset,&span,kIOMemoryMapperNone);
    if(!pa||(pa&4095)||span<4096||pa>=(1ULL<<40)||
       (pa<0xfc910000ULL+5120u*2760u&&pa+4096>0xfc910000ULL))return kIOReturnBadArgument;
   }
   X9RenderVMEntry entry={r,request.buffer_offset,request.gpu_address,request.bytes,request.flags};
   IOReturn kr=render_engine->map(context,&entry);
   if(kr!=kIOReturnSuccess)return kr;
   context->mappings[context->mapping_count++]=entry;
   return kIOReturnSuccess;
  }
  if(selector==X9RenderCache) {
   if(a->structureInputSize!=sizeof(X9RenderCacheRequest)||a->structureOutputSize)return kIOReturnBadArgument;
   const auto &request=*(const X9RenderCacheRequest *)a->structureInput;auto *r=resource(request.handle);
   if(request.abi!=X9_RENDER_ABI||!r||request.reserved||!request.operation||(request.operation&~3u)||!request.bytes||
      ((request.offset|request.bytes)&(X9_RENDER_CACHE_ATOM-1))||
      request.offset>r->bytes||request.bytes>r->bytes-request.offset||context->exposed)return kIOReturnBadArgument;
   return render_engine->cache(context,r,&request);
  }
  if(selector==X9RenderCreateGroup) {
   if(a->structureInputSize!=sizeof(X9RenderGroup)||a->structureOutputSize!=sizeof(X9RenderGroup))return kIOReturnBadArgument;
   const auto &request=*(const X9RenderGroup *)a->structureInput;
   X9RenderCaps caps;if(!render_caps_ready(&caps))return kIOReturnNotReady;
   if(request.abi!=X9_RENDER_ABI||request.handle||request.reserved||request.stream_count!=3||request.priority>1||
      !request.compute_mask||!request.fragment_mask||!request.tiler_mask||request.tiler_mask!=1||
      ((request.compute_mask|request.fragment_mask)&~caps.shader_present)||
      !request.max_compute||request.max_compute>__builtin_popcountll(request.compute_mask)||
      !request.max_fragment||request.max_fragment>__builtin_popcountll(request.fragment_mask)||request.max_tiler!=1||
      !request.tiler_heap_bytes||request.tiler_heap_bytes>X9_RENDER_BUFFER_LIMIT||(request.tiler_heap_bytes&4095))return kIOReturnBadArgument;
   IOReturn kr=render_engine->group_create(context,&request,(X9RenderGroup *)a->structureOutput);
   if(kr==kIOReturnSuccess)IOLog("X9 RENDER GROUP: created pid=%d group=%u three CS streams\n",proc_selfpid(),((X9RenderGroup *)a->structureOutput)->handle);
   return kr;
  }
  if(selector==X9RenderDestroyGroup) {
   if(a->structureInputSize!=sizeof(X9RenderHandle)||a->structureOutputSize)return kIOReturnBadArgument;
   const auto &request=*(const X9RenderHandle *)a->structureInput;
   if(request.abi!=X9_RENDER_ABI||!request.handle)return kIOReturnBadArgument;
   return render_engine->group_destroy(context,request.handle);
  }
  if(selector==X9RenderSubmit) {
   if(a->structureInputSize!=sizeof(X9RenderSubmission)||a->structureOutputSize!=sizeof(X9RenderCompletion))return kIOReturnBadArgument;
   X9RenderSubmission request;memcpy(&request,a->structureInput,sizeof(request));
   if(request.abi!=X9_RENDER_ABI||!request.group||!request.stream_count||request.stream_count>3||request.flags||
      !request.submission_id||context->next_submission==UINT64_MAX||request.submission_id!=context->next_submission+1)return kIOReturnBadArgument;
   unsigned seen=0;
   for(unsigned i=0;i<request.stream_count;i++) {
    if(!stream_mapped(request.streams[i])||(seen&(1u<<request.streams[i].index)))return kIOReturnBadArgument;
    seen|=1u<<request.streams[i].index;
   }
   context->next_submission=request.submission_id;
   IOReturn kr=render_engine->submit(context,&request,(X9RenderCompletion *)a->structureOutput);
   if(kr!=kIOReturnSuccess)IOLog("X9 RENDER SUBMIT: failed pid=%d group=%u id=%llu kr=%x exposed=%u\n",proc_selfpid(),request.group,request.submission_id,kr,context->exposed);
   if(context->exposed)context->disabled=true; /* DMA lifetime unresolved: no retry. */
   return kr;
  }
  if(selector==X9RenderWait) {
   if(a->structureInputSize!=sizeof(X9RenderWaitRequest)||a->structureOutputSize!=sizeof(X9RenderCompletion))return kIOReturnBadArgument;
   const auto &request=*(const X9RenderWaitRequest *)a->structureInput;
   if(request.abi!=X9_RENDER_ABI||!request.group||!request.submission_id||request.submission_id>context->next_submission||request.timeout_ns>10000000000ULL)return kIOReturnBadArgument;
   return render_engine->wait(context,&request,(X9RenderCompletion *)a->structureOutput);
  }
  if(selector==X9RenderTimestamp) {
   if(a->structureInputSize||a->structureOutputSize!=sizeof(uint64_t))return kIOReturnBadArgument;
   return render_engine->timestamp((uint64_t *)a->structureOutput);
  }
  return kIOReturnUnsupported;
 }
public:
 bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *props) APPLE_KEXT_OVERRIDE {
  if(type||clientHasPrivilege(task,kIOClientPrivilegeForeground)!=kIOReturnSuccess)return false;
  if(!IOUserClient::initWithTask(task,security,type,props))return false;
  context=(X9RenderContext *)IOMallocZero(sizeof(X9RenderContext));if(!context)return false;
  context->lock=IOLockAlloc();if(!context->lock)return false;
  context->owner=task;task_reference(task);
  X9RenderCaps caps;if(!render_caps_ready(&caps))return false;
  if(sweet_x9_render_memlimit(task)!=0)return false;
  if(render_engine->context_open(context)!=kIOReturnSuccess)return false;
  context->engine_open=true;
  char caller[32]={0};proc_selfname(caller,sizeof(caller));
  IOLog("X9 RENDER CLIENT: opened caller=%s pid=%d real context\n",caller,proc_selfpid());footprint("open");return true;
 }
 IOReturn clientClose(void) APPLE_KEXT_OVERRIDE {if(!isInactive())terminate();return kIOReturnSuccess;}
 void free(void) APPLE_KEXT_OVERRIDE {
  if(context) {
   footprint("close");
   if(context->engine_open&&!render_engine->context_close(context)) {
    IOLog("X9 RENDER: unresolved client teardown;prepared resources/context retained,no reset\n");context=nullptr;
   }else {
    for(auto &r:context->resources)if(r.memory){if(r.cache_mapping){r.cache_mapping->release();r.cache_mapping=nullptr;}if(r.prepared)r.memory->complete(r.memory->getDirection());r.memory->release();}
    if(context->owner)task_deallocate(context->owner);if(context->lock)IOLockFree(context->lock);
    IOFree(context,sizeof(*context));context=nullptr;
   }
  }
  IOUserClient::free();
 }
 IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE {
  if(!context||!options||!memory||!type||
     current_task()!=context->owner||clientHasPrivilege(current_task(),kIOClientPrivilegeForeground)!=kIOReturnSuccess)return kIOReturnBadArgument;
  IOLockLock(context->lock);auto *r=resource(type);
  if(!r||context->disabled||context->exposed){IOLockUnlock(context->lock);return kIOReturnBadArgument;}
  if(!r->cpu_exposed) {
   // A formerly GPU-only allocation can be mapped later. The synchronous
   // submit has already suspended/flushed/detached the GPU; acquire its bytes
   // BEFORE handing out any CPU alias. CIVAC also preserves initial bzero on
   // an allocation that has not reached its first submission yet.
   X9RenderCacheRequest cache={X9_RENDER_ABI,r->handle,X9RenderClean|X9RenderInvalidate,0,0,r->bytes};
   IOReturn kr=render_engine->cache(context,r,&cache);
   if(kr!=kIOReturnSuccess){IOLockUnlock(context->lock);return kr;}
   // Do not undo this on failed mapping or unmap: duplicate VM mappings and
   // direct contents writes are not observable through the transport.
   r->cpu_exposed=true;
  }
  r->memory->retain();*memory=r->memory;
  // Read-only host imports must not acquire a writable CPU alias through the
  // raw userclient mapping API either. Driver-owned executable BOs were pinned
  // InOut and still need writable CPU mappings to upload their instructions.
  *options=r->memory->getDirection()==kIODirectionOut?kIOMapReadOnly:0;
  // Preserve the allocation's cache contract explicitly for every user view.
  // Submission skips CPU maintenance for these BOs; a default/cacheable view
  // would make that skip invalid. Host imports keep their existing mode.
  // A/B (boot-arg token x9r1): pre-efef710 default-cache user views.
  static int default_views=-1;
  if(default_views<0){int v=0;default_views=PE_parse_boot_argn("x9r1",&v,sizeof(v))?1:0;}
  if(r->write_combined&&!default_views)*options|=kIOMapWriteCombineCache;
  IOLockUnlock(context->lock);return kIOReturnSuccess;
 }
 IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE {
  if(!context||!args||!context->lock||current_task()!=context->owner||
     clientHasPrivilege(current_task(),kIOClientPrivilegeForeground)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
  if(args->structureInputSize>sizeof(X9RenderSubmission)||args->structureOutputSize>sizeof(X9RenderCaps)||
     args->scalarInputCount||args->scalarOutputCount||args->structureInputDescriptor||args->structureOutputDescriptor||
     (args->structureInputSize&&!args->structureInput)||(args->structureOutputSize&&!args->structureOutput))return kIOReturnBadArgument;
  IOLockLock(context->lock);
  if(args->structureOutputSize)bzero(args->structureOutput,args->structureOutputSize);
  IOReturn kr=operation(selector,args);IOLockUnlock(context->lock);return kr;
 }
};
OSDefineMetaClassAndStructors(SweetX9RenderUserClient,IOUserClient);
class SweetX9Render : public IOService {
 OSDeclareDefaultStructors(SweetX9Render);
public:
 IOReturn newUserClient(task_t task,void *security,UInt32 type,OSDictionary *props,IOUserClient **handler) APPLE_KEXT_OVERRIDE {
  if(!handler)return kIOReturnBadArgument;
  *handler=nullptr;
  auto *client=new SweetX9RenderUserClient;if(!client)return kIOReturnNoMemory;
  /* A denied init may return before IORegistryEntry has its reserved state.
   * Never call getProvider() on that object. Detach only after attach succeeds. */
  if(!client->initWithTask(task,security,type,props)) {
   IOLog("X9 RENDER CLIENT: init refused pid=%d type=%u;release before attach\n",proc_selfpid(),type);
   client->release();return kIOReturnNotPrivileged;
  }
  if(!client->attach(this)) {
   IOLog("X9 RENDER CLIENT: attach refused pid=%d\n",proc_selfpid());
   client->release();return kIOReturnNotPrivileged;
  }
  if(!client->start(this)) {
   IOLog("X9 RENDER CLIENT: start refused pid=%d\n",proc_selfpid());
   client->detach(this);client->release();return kIOReturnNotPrivileged;
  }
  *handler=client;return kIOReturnSuccess;
 }
};
OSDefineMetaClassAndStructors(SweetX9Render,IOService);
extern "C" void sweet_x9_render_install(const X9RenderEngine *engine) {if(!render_engine&&!render_published)render_engine=engine;}
extern "C" void sweet_x9_render_publish(void) {
 if(render_published)return;X9RenderCaps caps;if(!render_caps_ready(&caps))return;
 auto *service=new SweetX9Render;IOService *root=IOService::getServiceRoot();
 if(!service||!root||!service->init()||!service->attach(root)||!service->start(root)) {
  if(service){if(service->getProvider()==root)service->detach(root);service->release();}return;
 }
 service->setName(X9_RENDER_SERVICE);service->setProperty("X9RenderABI",X9_RENDER_ABI,32);service->setProperty("X9RenderReady",true);
 service->setProperty("X9RenderCapabilities",caps.capabilities,32);service->registerService(kIOServiceSynchronous);
 __atomic_store_n(&render_published,true,__ATOMIC_RELEASE);
 IOLog("X9 RENDER SERVICE: registered caps=0x%x;native Metal discovery allowed after guarded payload deployment\n",caps.capabilities);
}
