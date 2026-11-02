/* SPDX-License-Identifier: MIT */
/* Included after PanVK device/command-buffer headers, only for X9_NATIVE_KMOD.
 * This is a real call-site adapter, not a replacement for device discovery,
 * Panthor-specific property getters, group allocation or the native engine. */
#include "X9PanVKQueue.h"
#include "X9PanVKTilerHeap.h"
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
struct x9_panvk_call_allocation { struct panvk_priv_mem memory; };
static int
x9_panvk_call_allocate(void *cookie,uint64_t bytes,struct X9PanVKCallBuffer *buffer,void **token)
{
 struct panvk_device *dev=cookie;
 struct x9_panvk_call_allocation *allocation=calloc(1,sizeof(*allocation));
 if(!allocation)return -1;
 /* A retained allocation reference must be sufficient to prevent pool reuse
  * after uncertain DMA; do not allocate from a pool that owns every BO. */
 if(dev->mempools.rw.props.owns_bos){free(allocation);errno=ENOTSUP;return -1;}
 allocation->memory=panvk_pool_alloc_mem(&dev->mempools.rw,
  (struct panvk_pool_alloc_info){.size=bytes,.alignment=64});
 if(!panvk_priv_mem_check_alloc(allocation->memory)){free(allocation);errno=ENOMEM;return -1;}
 *buffer=(struct X9PanVKCallBuffer){panvk_priv_mem_host_addr(allocation->memory),
  panvk_priv_mem_dev_addr(allocation->memory),bytes};
 if(!buffer->cpu){panvk_pool_free_mem(&allocation->memory);free(allocation);errno=ENOTSUP;return -1;}
 *token=allocation;return 0;
}
static int
x9_panvk_call_clean(void *cookie,void *token,uint64_t bytes)
{
 struct panvk_device *dev=cookie;struct x9_panvk_call_allocation *allocation=token;
 panvk_priv_mem_flush(allocation->memory,0,bytes);
 pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
 if(pan_kmod_vm_query_state(dev->kmod.vm)!=PAN_KMOD_VM_USABLE){errno=EIO;return -1;}
 return 0;
}
static void
x9_panvk_call_release(void *cookie,void *token)
{
 (void)cookie;struct x9_panvk_call_allocation *allocation=token;
 panvk_pool_free_mem(&allocation->memory);free(allocation);
}
static void
x9_panvk_call_quarantine(void *cookie,void *token)
{
 (void)cookie;(void)token;
 /* Keep allocation and BO reference indefinitely: native completion and DMA
  * teardown are uncertain. The native device is poisoned by the submit path. */
 mesa_loge("X9 native submit uncertain: CALL allocation retained; device lost");
}
static int
x9_panvk_group_submit(const struct panvk_device *dev,const struct drm_panthor_group_submit *submit)
{
 struct X9RenderCaps caps;
 if(x9_pan_kmod_caps(dev->kmod.dev,&caps))return -1;
 struct X9PanVKQueueBuffers buffers={.cookie=(void *)dev,
  .allocate=x9_panvk_call_allocate,.clean=x9_panvk_call_clean,
  .release=x9_panvk_call_release,.quarantine=x9_panvk_call_quarantine};
 return x9_panvk_queue_execute(dev->kmod.dev,submit,caps.cs_registers,
  MAX2(caps.cs_unpreserved_registers,4),&buffers,INT64_MAX);
}

static int
x9_panvk_group_create(struct pan_kmod_dev *dev,struct drm_panthor_group_create *request,uint64_t heap_bytes)
{
 if(!request||request->vm_id||request->queues.count!=3||request->queues.stride!=sizeof(struct drm_panthor_queue_create)||
    !request->queues.array||request->priority>PANTHOR_GROUP_PRIORITY_MEDIUM){errno=ENOTSUP;return -1;}
 const struct drm_panthor_queue_create *queues=(const void *)(uintptr_t)request->queues.array;
 for(unsigned i=0;i<3;i++)if(queues[i].priority!=1||queues[i].ringbuf_size!=64*1024){errno=ENOTSUP;return -1;}
 struct X9RenderGroup input={.abi=X9_RENDER_ABI,.stream_count=3,.priority=request->priority,
  .compute_mask=request->compute_core_mask,.fragment_mask=request->fragment_core_mask,.tiler_mask=request->tiler_core_mask,
  .max_compute=request->max_compute_cores,.max_fragment=request->max_fragment_cores,.max_tiler=request->max_tiler_cores,
  .tiler_heap_bytes=heap_bytes};
 struct X9RenderGroup output={0};
 int ret=x9_pan_kmod_group_create(dev,&input,&output);
 if(!ret)request->group_handle=output.handle;
 return ret;
}

/* Diagnostic A/B (exact boot-arg token x9tb): link 16 tiler heap chunks
 * (32 MiB) instead of Mesa's fixed initial 5 (10 MiB). Default unchanged. */
static uint32_t
x9_panvk_tiler_chunks(uint32_t requested)
{
 static int big=-1;
 if(big<0) {
  int m=0;char args[4096]={0};size_t bytes=sizeof(args)-1;
  if(!sysctlbyname("kern.bootargs",args,&bytes,NULL,0)) {
   args[sizeof(args)-1]=0;
   for(const char *p=args;*p;) {
    while(*p&&(unsigned char)*p<=32)p++;
    const char *end=p;while(*end&&(unsigned char)*end>32)end++;
    if(end-p==4&&!memcmp(p,"x9tb",4))m=1;
    p=end;
   }
  }
  big=m;
 }
 return big&&requested<16?16:requested;
}
static int
x9_panvk_tiler_create(struct panvk_device *dev,struct panvk_tiler_heap *heap,struct drm_panthor_tiler_heap_create *request)
{
 if(request->vm_id||!request->initial_chunk_count||request->initial_chunk_count>request->max_chunks){errno=EINVAL;return -1;}
 {
  const uint32_t chunks=x9_panvk_tiler_chunks(request->max_chunks);
  x9_panvk_log("X9 TILER HEAP: proc=%s chunk=%u requested=%u linked=%u bytes=%llu;x9tb=32MiB A/B",
   getprogname(),request->chunk_size,request->max_chunks,chunks,(unsigned long long)chunks*request->chunk_size);
  request->max_chunks=chunks;
 }
 uint64_t bytes=x9_panvk_heap_bytes(request->chunk_size,request->max_chunks);
 if(!bytes){errno=ENOTSUP;return -1;}
 uint64_t address=panvk_as_alloc(dev,PANVK_NO_EXEC_VA_HEAP,bytes,16384);
 if(!address){errno=ENOMEM;return -1;}
 struct X9PanVKTilerHeapInfo info={0};
 struct X9PanVKTilerHeap *native=x9_panvk_heap_create(dev->kmod.vm,address,request->chunk_size,request->max_chunks,&info);
 if(!native) {
  if(pan_kmod_vm_query_state(dev->kmod.vm)==PAN_KMOD_VM_USABLE)panvk_as_free(dev,address,bytes);
  return -1;
 }
 heap->x9_heap=native;heap->x9_address=address;heap->x9_bytes=bytes;
 request->handle=info.handle;request->tiler_heap_ctx_gpu_va=info.context_address;
 request->first_heap_chunk_gpu_va=info.first_chunk_address;return 0;
}
static int
x9_panvk_tiler_destroy(struct panvk_device *dev,struct panvk_tiler_heap *heap)
{
 if(!heap->x9_heap){errno=EINVAL;return -1;}
 if(x9_panvk_heap_destroy(heap->x9_heap))return -1;
 panvk_as_free(dev,heap->x9_address,heap->x9_bytes);
 heap->x9_heap=NULL;heap->x9_address=heap->x9_bytes=0;return 0;
}
