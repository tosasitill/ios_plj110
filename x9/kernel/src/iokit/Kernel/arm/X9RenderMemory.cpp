/* SPDX-License-Identifier: MIT */
#include "X9RenderMemory.h"
#include <kern/task.h>
#include <kern/cpu_data.h>
#include <libkern/libkern.h>
IOReturn x9_render_memory_open(X9RenderContext *c) {
 if(!c||c->page_table||c->exposed||c->disabled)return kIOReturnBadArgument;
 c->page_table=x9_render_vm_create();return c->page_table?kIOReturnSuccess:kIOReturnNoMemory;
}
bool x9_render_memory_close(X9RenderContext *c) {
 if(!c||c->exposed)return false;
 if(!x9_render_vm_destroy(c->page_table))return false;
 c->page_table=nullptr;return true;
}
IOReturn x9_render_memory_map(X9RenderContext *c,const X9RenderVMEntry *entry) {
 if(!c||!c->page_table||c->disabled)return kIOReturnNotReady;
 if(c->exposed)return kIOReturnBusy;
 return x9_render_vm_map(c->page_table,entry);
}
IOReturn x9_render_memory_unmap(X9RenderContext *c,const X9RenderVMEntry *entry) {
 if(!c||!c->page_table||c->disabled)return kIOReturnNotReady;
 if(c->exposed)return kIOReturnBusy;
 return x9_render_vm_unmap(c->page_table,entry);
}
IOReturn x9_render_memory_cache_spans(X9RenderContext *c,X9RenderResource *r,
 const X9RenderCacheSpan *spans,unsigned count,uint32_t operation,uint64_t *completed_bytes) {
 if(!c||!r||!r->memory||!r->prepared||!completed_bytes||count>X9_RENDER_MAX_MAPPINGS||
    (count&&!spans)||!operation||(operation&~3u))return kIOReturnBadArgument;
 *completed_bytes=0;
 if(c->disabled)return kIOReturnNotReady;if(c->exposed)return kIOReturnBusy;
 if(!count)return kIOReturnSuccess;
 uint64_t total=0,prior_end=0;
 for(unsigned i=0;i<count;i++) {
  const auto &span=spans[i];
  if(span.begin>=span.end||span.end>r->bytes||
     ((span.begin|span.end)&(X9_RENDER_CACHE_ATOM-1))||
     (i&&span.begin<prior_end))return kIOReturnBadArgument;
  total+=span.end-span.begin;prior_end=span.end;
 }
 IOOptionBits options=kIOMapAnywhere;
 if(r->memory->getDirection()==kIODirectionOut)options|=kIOMapReadOnly;
 /* Native BOs already have a descriptor-owned kernel alias. Do not repeatedly
  * create/destroy VM mappings for every small explicit upload and submission.
  * Host imports retain their real pinned descriptor and permission-checked
  * temporary alias; no snapshot/copy can replace no-copy or IOSurface memory. */
 IOMemoryMap *mapping=nullptr;
 uint64_t address=0,length=r->memory->getLength();
 if(r->imported) {
  mapping=r->cache_mapping;
  if(!mapping) {
   mapping=r->memory->createMappingInTask(kernel_task,0,options);
   if(!mapping)return kIOReturnNoMemory;
   r->cache_mapping=mapping;
  }
  address=mapping->getAddress();length=mapping->getLength();
 }else address=(uint64_t)((IOBufferMemoryDescriptor *)r->memory)->getBytesNoCopy();
 if(!address||(address&(X9_RENDER_CACHE_ATOM-1))||length<r->bytes||address+length<address) {
  if(mapping){mapping->release();r->cache_mapping=nullptr;}return kIOReturnBadArgument;
 }

 /* Adapted from UniXNU Adreno740 8638683: close each bounded run's DSB
  * on the same CPU as its DC instructions. Cacheability/coherency is NOT
  * inferred. One batch spans only one descriptor; aliases across resources
  * are never deduplicated. 64KiB cap bounds preemption-off latency. */
 unsigned index=0;uint64_t cursor=spans[0].begin,runs=0;
 while(index<count) {
  disable_preemption();
  uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
  uint64_t line=4ULL<<((ctr>>16)&15);
  if(line>X9_RENDER_CACHE_ATOM||X9_RENDER_CACHE_ATOM%line||(address&(line-1))) {
   enable_preemption();return kIOReturnNotAligned;
  }
  uint64_t budget=64u<<10;
  __asm__ volatile("dsb sy":::"memory");
  while(index<count&&budget) {
   uint64_t bytes=spans[index].end-cursor;
   if(bytes>budget)bytes=budget;
   uint64_t begin=address+cursor,end=begin+bytes;
#define X9_CACHE_LINES(instruction) do { \
 uint64_t at=begin; \
 for(;end-at>=4*line;at+=4*line) { \
  __asm__ volatile(instruction ", %0\n\t" instruction ", %1\n\t" \
                   instruction ", %2\n\t" instruction ", %3" \
   ::"r"(at),"r"(at+line),"r"(at+2*line),"r"(at+3*line):"memory"); \
 } \
 for(;at<end;at+=line)__asm__ volatile(instruction ", %0"::"r"(at):"memory"); \
} while(0)
   if(operation==(X9RenderClean|X9RenderInvalidate)) { X9_CACHE_LINES("dc civac"); }
   else if(operation==X9RenderClean) { X9_CACHE_LINES("dc cvac"); }
   else { X9_CACHE_LINES("dc ivac"); }
#undef X9_CACHE_LINES
   budget-=bytes;cursor+=bytes;
   if(cursor==spans[index].end&&++index<count)cursor=spans[index].begin;
  }
  __asm__ volatile("dsb sy":::"memory");
  enable_preemption();runs++;
 }
 if((operation&X9RenderClean)&&total==r->bytes&&spans[0].begin==0)
  r->initial_cache_clean=true;
 *completed_bytes=total;
 /* Existing owner/context serialization covers this telemetry. Count phase
  * barriers versus old one-pair-per-range behavior; runs may be GREATER for
  * large buffers because CPU migration safety takes priority. */
 static uint64_t batches=0,range_total=0,run_total=0;
 range_total+=count;run_total+=runs;
 if(++batches%8192==0)IOLog("X9 RENDER cache-batch: batches=%llu ranges=%llu bounded_runs=%llu;64KiB CPU-pinned runs,no cache bytes omitted\n",batches,range_total,run_total);
 return kIOReturnSuccess;
}
IOReturn x9_render_memory_cache(X9RenderContext *c,X9RenderResource *r,const X9RenderCacheRequest *request) {
 if(!r||!request||request->abi!=X9_RENDER_ABI||request->handle!=r->handle||
    request->reserved||!request->bytes||request->offset>r->bytes||request->bytes>r->bytes-request->offset)
  return kIOReturnBadArgument;
 X9RenderCacheSpan span={request->offset,request->offset+request->bytes,0};uint64_t completed=0;
 return x9_render_memory_cache_spans(c,r,&span,1,request->operation,&completed);
}
uint64_t x9_render_memory_publish(X9RenderContext *c) {
 if(!c||!c->page_table||c->exposed||c->disabled)return 0;
 uint64_t root=x9_render_vm_publish(c->page_table);
 /* Mark before hardware publication. A failed AS install still requires
  * verified detach; ownership cannot be inferred from a return code. */
 if(root)c->exposed=true;return root;
}
void x9_render_memory_detached(X9RenderContext *c) {
 /* Only after engine-confirmed stop, full GPU flush and AS detach. */
 if(!c)return;x9_render_vm_mark_detached(c->page_table);c->exposed=false;
}
