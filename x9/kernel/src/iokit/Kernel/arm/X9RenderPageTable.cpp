/* SPDX-License-Identifier: MIT */
#include "X9RenderPageTable.h"
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <libkern/libkern.h>
#include "X9GPUCache.h"
namespace {
/* Same table/leaf descriptor contract as pan168 SweetX9CSF.cpp. */
constexpr uint64_t pa_mask=0x000000fffffff000ULL;
constexpr unsigned table_limit=1152; /* bounded sparse tables; backing <=18MiB */
struct Table { IOBufferMemoryDescriptor *memory; uint64_t *entries,pa; bool dirty; };
}
struct X9RenderPageTable { Table tables[table_limit]; unsigned count; bool attached; };
static int allocate_table(X9RenderPageTable *vm) {
 unsigned index=0;while(index<vm->count&&vm->tables[index].memory)index++;
 if(index==table_limit)return -1;
 auto *m=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,16384,16384);
 if(!m)return -1;
 if(m->prepare(kIODirectionInOut)!=kIOReturnSuccess){m->release();return -1;}
 IOByteCount span=0;uint64_t pa=m->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
 auto *entries=(uint64_t *)m->getBytesNoCopy();
 if(!entries||!pa||(pa&16383)||(pa&~pa_mask)||span<4096) {
  m->complete(kIODirectionInOut);m->release();return -1;
 }
 bzero(entries,16384);if(index==vm->count)vm->count++;
 vm->tables[index]={m,entries,pa,true};return (int)index;
}
static uint64_t *leaf(X9RenderPageTable *vm,uint64_t va,bool create) {
 unsigned table=0;
 for(unsigned level=0;level<3;level++) {
  auto &entry=vm->tables[table].entries[(va>>(39-level*9))&511];
  if(!entry) {
   if(!create)return nullptr;
   int child=allocate_table(vm);if(child<0)return nullptr;
   entry=vm->tables[child].pa|0x403ULL;
   vm->tables[table].dirty=true;
  }
  if((entry&3)!=3)return nullptr;
  unsigned child=0;while(child<vm->count&&vm->tables[child].pa!=(entry&pa_mask))child++;
  if(child==vm->count)return nullptr;table=child;
 }
 /* Every caller which can write a leaf passes here, including rollback.
  * Read-only preflight may conservatively dirty a page; steady submissions
  * never call leaf(). CPU-only tables cannot be modified by imported aliases. */
 vm->tables[table].dirty=true;
 return &vm->tables[table].entries[(va>>12)&511];
}
/* Reclaim empty intermediate tables only while the address space is detached.
 * This avoids exhausting the table budget after repeated allocation churn. */
static bool prune(X9RenderPageTable *vm,unsigned table,unsigned level) {
 bool empty=true;
 for(unsigned slot=0;slot<512;slot++) {
  auto &entry=vm->tables[table].entries[slot];if(!entry)continue;
  if(level<3&&(entry&3)==3) {
   unsigned child=0;while(child<vm->count&&vm->tables[child].pa!=(entry&pa_mask))child++;
   if(child<vm->count&&vm->tables[child].memory&&prune(vm,child,level+1)) {
    vm->tables[child].memory->complete(kIODirectionInOut);vm->tables[child].memory->release();
    bzero(&vm->tables[child],sizeof(Table));entry=0;vm->tables[table].dirty=true;
   }
  }
  if(entry)empty=false;
 }
 return empty;
}
static bool valid(const X9RenderVMEntry *m,bool internal=false) {
 const uint64_t begin=internal?0x10000000ULL:X9_RENDER_MIN_VA;
 const uint64_t end=internal?0x11000000ULL:X9_RENDER_MAX_VA;
 return m&&m->resource&&m->resource->memory&&m->resource->prepared&&m->bytes&&
  (!internal||(!m->resource->handle&&!(m->flags&X9RenderGPUExecute)))&&
  m->flags&&!(m->flags&~7u)&&!((m->flags&4)&&(m->flags&2))&&!(m->flags&~m->resource->flags)&&
  !((m->address|m->offset|m->bytes)&4095)&&m->address>=begin&&
  m->address<end&&m->bytes<=end-m->address&&
  m->offset<=m->resource->bytes&&m->bytes<=m->resource->bytes-m->offset;
}
static bool physical(const X9RenderVMEntry *m,uint64_t offset,uint64_t *pa) {
 IOByteCount span=0;*pa=m->resource->memory->getPhysicalSegment(m->offset+offset,&span,kIOMemoryMapperNone);
 return *pa&& !(*pa&~pa_mask)&&span>=4096&&
  !(*pa<0xfc910000ULL+5120u*2760u&&*pa+4096>0xfc910000ULL);
}
static uint64_t attrs(uint32_t flags) {
 /* Cached index0 (0x88 implementation-defined write-back), RO/RW and
  * execute-never. With ACE-Lite active the leaf is outer shareable (kbase
  * SHARE_BOTH_BITS 2<<8): GPU L2 misses become snooping reads and L2
  * write-backs snoop-invalidate CPU lines, so per-submit CPU CIVAC is not
  * needed; the GPU L2 itself is still flushed/invalidated around every job.
  * x9_gpu_acelite_active never changes while a VM exists (set before the
  * first VM, never cleared), so unmap's exact PTE comparison stays valid. */
 return 0x403ULL|((flags&2)?(1ULL<<6):(3ULL<<6))|((flags&4)?0:(1ULL<<54))|
  (x9_gpu_acelite_active?(2ULL<<8):0);
}
X9RenderPageTable *x9_render_vm_create() {
 auto *vm=(X9RenderPageTable *)IOMallocZero(sizeof(X9RenderPageTable));
 if(vm&&allocate_table(vm)<0){IOFree(vm,sizeof(*vm));return nullptr;}return vm;
}
static IOReturn map(X9RenderPageTable *vm,const X9RenderVMEntry *m,bool internal) {
 if(!vm||!valid(m,internal))return kIOReturnBadArgument;
 if(vm->attached)return kIOReturnBusy;
 /* Preflight all destinations and allocate intermediate tables before writing
  * leaves. Failure prunes empty intermediate tables before returning. */
 for(uint64_t off=0;off<m->bytes;off+=4096) {
  uint64_t pa;if(!physical(m,off,&pa)){prune(vm,0,0);return kIOReturnBadArgument;}
  auto *pte=leaf(vm,m->address+off,true);if(!pte){prune(vm,0,0);return kIOReturnNoMemory;}
  if(*pte){prune(vm,0,0);return kIOReturnExclusiveAccess;}
 }
 uint64_t off=0;
 for(;off<m->bytes;off+=4096) {
  uint64_t pa;if(!physical(m,off,&pa))break;
  *leaf(vm,m->address+off,false)=pa|attrs(m->flags);
 }
 if(off==m->bytes)return kIOReturnSuccess;
 while(off){off-=4096;*leaf(vm,m->address+off,false)=0;}prune(vm,0,0);return kIOReturnBadArgument;
}
static IOReturn unmap(X9RenderPageTable *vm,const X9RenderVMEntry *m,bool internal) {
 if(!vm||!valid(m,internal))return kIOReturnBadArgument;
 if(vm->attached)return kIOReturnBusy;
 for(uint64_t off=0;off<m->bytes;off+=4096) {
  uint64_t pa;auto *pte=leaf(vm,m->address+off,false);
  if(!physical(m,off,&pa)||!pte||*pte!=(pa|attrs(m->flags)))return kIOReturnBadArgument;
 }
 for(uint64_t off=0;off<m->bytes;off+=4096)*leaf(vm,m->address+off,false)=0;
 prune(vm,0,0);
 return kIOReturnSuccess;
}
uint64_t x9_render_vm_publish(X9RenderPageTable *vm) {
 if(!vm||vm->attached)return 0;
 /* No cache-attribute/coherency assumption: skip only CPU-private page
  * tables which have not changed since their last completed clean. Parent
  * links, leaf writes, removals and rollbacks all retain dirty tracking.
  * Translation installation/invalidation and GPU detach are unchanged. */
 uint64_t cleaned=0,unchanged=0;
 for(unsigned i=0;i<vm->count;i++)if(vm->tables[i].memory) {
  /* ACE-Lite: TRANSCFG.PTW_SH is outer shareable, page walks snoop the CPU
   * caches (stock kbase_mmu_sync_pgd_cpu skips its clean unless NONE). */
  if(vm->tables[i].dirty&&!x9_gpu_acelite_active){x9_gpu_clean(vm->tables[i].pa,4096,1);cleaned+=4096;}
  else unchanged+=4096;
 }
 __asm__ volatile("dsb sy":::"memory");
 for(unsigned i=0;i<vm->count;i++)if(vm->tables[i].memory)vm->tables[i].dirty=false;
 static uint64_t calls=0,clean_total=0,skip_total=0;
 clean_total+=cleaned;skip_total+=unchanged;
 if(++calls%512==0) {
  IOLog("X9 RENDER table-cache: calls=%llu cleaned_avg=%llu unchanged_skip_avg=%llu;CPU-private tables only,TLB/AS protocol unchanged\n",calls,clean_total/calls,skip_total/calls);
 }
 vm->attached=true;return vm->tables[0].pa;
}
void x9_render_vm_mark_detached(X9RenderPageTable *vm) {
 /* Caller must have confirmed GPU stop, flush and AS detach, not just a fence. */
 if(vm)vm->attached=false;
}
bool x9_render_vm_destroy(X9RenderPageTable *vm) {
 if(!vm)return true;if(vm->attached)return false;
 for(unsigned i=0;i<vm->count;i++)if(vm->tables[i].memory) {
  vm->tables[i].memory->complete(kIODirectionInOut);vm->tables[i].memory->release();
 }
 IOFree(vm,sizeof(*vm));return true;
}

IOReturn x9_render_vm_map(X9RenderPageTable *vm,const X9RenderVMEntry *entry){return map(vm,entry,false);}
IOReturn x9_render_vm_unmap(X9RenderPageTable *vm,const X9RenderVMEntry *entry){return unmap(vm,entry,false);}
IOReturn x9_render_vm_map_kernel(X9RenderPageTable *vm,const X9RenderVMEntry *entry){return map(vm,entry,true);}
IOReturn x9_render_vm_unmap_kernel(X9RenderPageTable *vm,const X9RenderVMEntry *entry){return unmap(vm,entry,true);}
