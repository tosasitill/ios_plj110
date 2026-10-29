/* SPDX-License-Identifier: MIT */
#include "X9PanVKTilerHeap.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct HeapPart {struct pan_kmod_bo *bo;void *cpu;uint64_t address,bytes;};
struct X9PanVKTilerHeap {
 struct HeapPart parts[2]; /* context, then complete chunk arena */
 struct pan_kmod_vm *vm;
 uint64_t address,bytes;
 bool mapped,failed;
};
uint64_t x9_panvk_heap_bytes(uint32_t size,uint32_t chunks) {
 /* Stock header: low12 bits store chunk size in4KiB units. Chunk arena and
  * context have separate BOs so128MiB of chunks respects the per-BO limit. */
 if(size<16384||(size&16383)||size>0xfff000u||!chunks||chunks>1024)return 0;
 uint64_t bytes=(uint64_t)size*chunks;
 if(bytes>X9_RENDER_BUFFER_LIMIT)return 0;
 return bytes+16384;
}
struct X9PanVKTilerHeap *x9_panvk_heap_create(struct pan_kmod_vm *vm,uint64_t address,uint32_t size,uint32_t chunks,struct X9PanVKTilerHeapInfo *info) {
 uint64_t bytes=x9_panvk_heap_bytes(size,chunks);
 if(!vm||!info||!bytes||(address&16383)||address<X9_RENDER_MIN_VA||
    address>=X9_RENDER_MAX_VA||bytes>X9_RENDER_MAX_VA-address){errno=EINVAL;return NULL;}
 memset(info,0,sizeof(*info));
 struct X9PanVKTilerHeap *heap=calloc(1,sizeof(*heap));if(!heap)return NULL;
 heap->vm=vm;heap->address=address;heap->bytes=bytes;
 heap->parts[0]=(struct HeapPart){.address=address,.bytes=16384};
 heap->parts[1]=(struct HeapPart){.address=address+16384,.bytes=bytes-16384};
 for(unsigned i=0;i<2;i++) {
  struct HeapPart *part=&heap->parts[i];
  part->bo=pan_kmod_bo_alloc(vm->dev,vm,part->bytes,PAN_KMOD_BO_FLAG_WB_MMAP);
  if(!part->bo)goto failure;
  part->cpu=pan_kmod_bo_mmap(part->bo,PROT_READ|PROT_WRITE,MAP_SHARED,NULL);
  if(part->cpu==MAP_FAILED){part->cpu=NULL;goto failure;}
  memset(part->cpu,0,part->bytes);
 }
 /*32-byte context and64-byte chunk headers start zero. */
 for(unsigned i=0;i+1<chunks;i++) {
  uint64_t next=(address+16384+(uint64_t)(i+1)*size)|((uint64_t)size>>12);
  memcpy((char *)heap->parts[1].cpu+(uint64_t)i*size,&next,sizeof(next));
 }
 for(unsigned i=0;i<2;i++) {
  struct HeapPart *part=&heap->parts[i];
  pan_kmod_queue_bo_map_sync(part->bo,0,part->cpu,part->bytes,PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
 }
 pan_kmod_flush_bo_map_syncs(vm->dev);
 if(pan_kmod_vm_query_state(vm)!=PAN_KMOD_VM_USABLE){errno=EIO;goto failure;}
 struct pan_kmod_vm_op ops[2]={0};
 for(unsigned i=0;i<2;i++)ops[i]=(struct pan_kmod_vm_op){.type=PAN_KMOD_VM_OP_TYPE_MAP,
  .va={.start=heap->parts[i].address,.size=heap->parts[i].bytes},.map={.bo=heap->parts[i].bo,.bo_offset=0}};
 if(pan_kmod_vm_bind(vm,PAN_KMOD_VM_OP_MODE_IMMEDIATE,ops,2))goto failure;
 heap->mapped=true;
 *info=(struct X9PanVKTilerHeapInfo){address,address+16384,bytes,size,chunks,pan_kmod_bo_handle(heap->parts[0].bo)};return heap;
failure: {
 int saved=errno;
 /* A partial native bind faults the VM. BO release then leaves all uncertain
  * kernel resources retained until connection teardown, never recycled. */
 for(unsigned i=0;i<2;i++) {
  struct HeapPart *part=&heap->parts[i];
  if(part->cpu) {
   if(x9_pan_kmod_unmap_cpu(part->bo,part->cpu)) {
    /* Keep both parts and their ownership records after an uncertain unmap. */
    heap->failed=true;errno=EIO;return NULL;
   }
   part->cpu=NULL;
  }
 }
 for(unsigned i=0;i<2;i++)if(heap->parts[i].bo)pan_kmod_bo_put(heap->parts[i].bo);
 free(heap);errno=saved;return NULL;
 }
}
int x9_panvk_heap_destroy(struct X9PanVKTilerHeap *heap) {
 if(!heap){errno=EINVAL;return -1;}
 if(heap->failed){errno=EIO;return -1;}
 if(!pan_kmod_bo_wait(heap->parts[0].bo,10000000000LL,false))return -1;
 if(heap->mapped) {
  struct pan_kmod_vm_op op={.type=PAN_KMOD_VM_OP_TYPE_UNMAP,.va={.start=heap->address,.size=heap->bytes}};
  if(pan_kmod_vm_bind(heap->vm,PAN_KMOD_VM_OP_MODE_IMMEDIATE,&op,1))return -1;
  heap->mapped=false;
 }
 for(unsigned i=0;i<2;i++) {
  struct HeapPart *part=&heap->parts[i];
  if(part->cpu){if(x9_pan_kmod_unmap_cpu(part->bo,part->cpu))return -1;part->cpu=NULL;}
 }
 for(unsigned i=0;i<2;i++) {
  pan_kmod_bo_put(heap->parts[i].bo);heap->parts[i].bo=NULL;
  if(pan_kmod_vm_query_state(heap->vm)!=PAN_KMOD_VM_USABLE){heap->failed=true;errno=EIO;return -1;}
 }
 free(heap);return 0;
}
