/* SPDX-License-Identifier: MIT */
#include "X9RenderMemory.h"
#include <kern/task.h>
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
IOReturn x9_render_memory_cache(X9RenderContext *c,X9RenderResource *r,const X9RenderCacheRequest *request) {
 if(!c||!r||!r->memory||!r->prepared||!request||request->abi!=X9_RENDER_ABI||
    request->handle!=r->handle||request->reserved||!request->operation||(request->operation&~3u)||
    !request->bytes||((request->offset|request->bytes)&(X9_RENDER_CACHE_ATOM-1))||
    request->offset>r->bytes||request->bytes>r->bytes-request->offset)return kIOReturnBadArgument;
 if(c->disabled)return kIOReturnNotReady;if(c->exposed)return kIOReturnBusy;
 /* The generic IOMemoryDescriptor incoherent-operation path can become a
  * no-op in ARM_COHERENT_IO builds. Use the same explicit cache instruction
  * contract as pan168, through a real descriptor-owned kernel mapping. */
 uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 uint64_t line=4ULL<<((ctr>>16)&15);
 /* Never discard dirty bytes outside the caller's owned cache lines. */
 if((request->offset|request->bytes)&(line-1))return kIOReturnNotAligned;
 // Cache maintenance needs an address, not a writable CPU alias. A read-only
 // import may have VM max_protection=READ: requesting RW here would undo the
 // direction fix by failing after import, on the first flush.
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
 if(!address||(address&(line-1))||length<r->bytes||address+length<address) {
  if(mapping){mapping->release();r->cache_mapping=nullptr;}return kIOReturnBadArgument;
 }
 uint64_t begin=address+request->offset,end=begin+request->bytes;
 __asm__ volatile("dsb sy":::"memory");
 for(uint64_t at=begin;at<end;at+=line) {
  if(request->operation==(X9RenderClean|X9RenderInvalidate))
   __asm__ volatile("dc civac, %0"::"r"(at):"memory");
  else if(request->operation==X9RenderClean)
   __asm__ volatile("dc cvac, %0"::"r"(at):"memory");
  else __asm__ volatile("dc ivac, %0"::"r"(at):"memory");
 }
 __asm__ volatile("dsb sy":::"memory");
 // A partial upload cannot discharge the allocation's initial zero-fill.
 if((request->operation&X9RenderClean)&&!request->offset&&request->bytes==r->bytes)
  r->initial_cache_clean=true;
 return kIOReturnSuccess;
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
