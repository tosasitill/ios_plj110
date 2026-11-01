/* SPDX-License-Identifier: MIT */
#include "X9PanVKMemory.h"
#include "kmod/pan_kmod_backend.h"
#include "../X9RenderTransport.h"
#include "drm-uapi/panthor_drm.h"
#include <errno.h>
#include "util/log.h"
#include <string.h>
#define X9_BO_FLAGS (PAN_KMOD_BO_FLAG_EXECUTABLE|PAN_KMOD_BO_FLAG_NO_MMAP|PAN_KMOD_BO_FLAG_WB_MMAP)
/* Native mappings this device may hold. The caps still report the ABI's 1024
 * (X9_RENDER_MAX_MAPPINGS) for older plugins; kernels from 2026-11-01 accept
 * X9_RENDER_KERNEL_MAPPINGS (4096). With 1024, backboardd filled this table
 * after a minute of animations and the whole device was failed (DeviceLost).
 * Against an older kernel the kernel's own limit still refuses cleanly. */
#define X9_PANVK_MAX_MAPPINGS 4096u
struct x9_dev {
 struct pan_kmod_dev base;
 struct X9RenderConnection *connection;
 struct X9RenderCaps caps;
 struct drm_panthor_csif_info csif;
 simple_mtx_t lock;
 struct pan_kmod_vm *vm;
 struct X9RenderMapping mappings[X9_PANVK_MAX_MAPPINGS];
 uint32_t count,groups[32],group_count,last_group;
 uint64_t last_submission;
 uint64_t cache_ops,cache_range_bytes,cache_bo_bytes,cache_merged;
 bool failed;
};
struct x9_bo {struct pan_kmod_bo base;uint32_t memory_type,native_flags;void *imported_host;};
static struct x9_dev *device(struct pan_kmod_dev *dev){return container_of(dev,struct x9_dev,base);}
static int error(int e){errno=e;return -1;}
static int call(struct x9_dev *d,uint32_t op,const void *in,size_t ilen,void *out,size_t olen) {
 if(d->failed)return error(EIO);
 int ret=x9_render_call(d->connection,op,in,ilen,out,olen);
 if(ret) {
  int saved_errno=errno;
  bool recoverable=(op==X9RenderAllocate||op==X9RenderImportHost)&&(saved_errno==ENOMEM||saved_errno==ENOSPC);
  x9_panvk_log("X9 PANVK CALL: op=%u ret=%d errno=%d recoverable=%d detail=%s",op,ret,saved_errno,recoverable,x9_render_last_error());
  if(!recoverable)d->failed=true;
  errno=saved_errno;
 }
 return ret;
}
static void destroy(struct pan_kmod_dev *dev) {
 struct x9_dev *d=device(dev);
 /* Common cleanup may flush pending cache operations; keep transport alive. */
 pan_kmod_dev_cleanup(dev);x9_render_close(d->connection);
 simple_mtx_destroy(&d->lock);pan_kmod_dev_free(dev,d);
}
static struct pan_kmod_va_range va_range(const struct pan_kmod_dev *dev) {
 const struct x9_dev *d=container_of(dev,const struct x9_dev,base);
 return (struct pan_kmod_va_range){d->caps.va_start,d->caps.va_end-d->caps.va_start};
}
static struct pan_kmod_bo *allocate(struct pan_kmod_dev *dev,struct pan_kmod_vm *vm,uint64_t size,uint32_t flags) {
 struct x9_dev *d=device(dev);
 if(!size||size>d->caps.max_buffer_bytes||(flags&~X9_BO_FLAGS)|| (vm&&vm!=d->vm)) {
  x9_panvk_log("X9 PANVK ALLOC: refuse size=%llu max=%llu flags=%x bits=%x vm=%d/%d",(unsigned long long)size,
    (unsigned long long)d->caps.max_buffer_bytes,flags,flags&~X9_BO_FLAGS,vm!=NULL,(vm==d->vm));
  errno=EINVAL;return NULL;
 }
 uint64_t bytes=(size+16383)&~16383ULL;
 struct x9_bo *b=pan_kmod_dev_alloc(dev,sizeof(*b));if(!b)return NULL;
 uint32_t permissions=X9RenderGPURead|((flags&PAN_KMOD_BO_FLAG_EXECUTABLE)?X9RenderGPUExecute:X9RenderGPUWrite);
 struct X9RenderAllocation request={X9_RENDER_ABI,permissions,bytes,0};struct X9RenderBuffer reply={0};
 simple_mtx_lock(&d->lock);
 int ret=call(d,X9RenderAllocate,&request,sizeof(request),&reply,sizeof(reply));
 if(!ret&&(reply.abi!=X9_RENDER_ABI||!reply.handle||!reply.memory_type||reply.bytes!=bytes||reply.flags!=permissions)) {
  d->failed=true;ret=error(EPROTO);
 }
 simple_mtx_unlock(&d->lock);
 if(ret) {
  x9_panvk_log("X9 PANVK ALLOC: kernel refused size=%llu bytes=%llu ret=%d errno=%d used_failed=%d",(unsigned long long)size,
    (unsigned long long)bytes,ret,errno,(int)d->failed);
  pan_kmod_dev_free(dev,b);return NULL;
 }
 if(bytes>=(4u<<20))x9_panvk_log("X9 PANVK ALLOC: size=%llu ok",(unsigned long long)bytes);
 b->memory_type=reply.memory_type;b->native_flags=permissions;
 pan_kmod_bo_init(&b->base,dev,vm,bytes,flags,reply.handle);return &b->base;
}
static void free_bo(struct pan_kmod_bo *bo) {
 struct x9_dev *d=device(bo->dev);struct x9_bo *b=container_of(bo,struct x9_bo,base);
 pan_kmod_bo_cleanup(bo);simple_mtx_lock(&d->lock);
 /* Retain kernel backing until connection teardown if the caller drops a BO
  * still mapped or the GPU's ownership cannot be established. */
 for(unsigned i=0;i<d->count;i++)if(d->mappings[i].handle==bo->handle)d->failed=true;
 struct X9RenderHandle request={X9_RENDER_ABI,bo->handle};
 if(!d->failed)call(d,X9RenderRelease,&request,sizeof(request),NULL,0);
 simple_mtx_unlock(&d->lock);pan_kmod_dev_free(bo->dev,b);
}
static struct pan_kmod_bo *import_bo(struct pan_kmod_dev *dev,int fd){(void)dev;(void)fd;errno=ENOTSUP;return NULL;}
static int export_bo(struct pan_kmod_bo *bo){(void)bo;return error(ENOTSUP);}
static void *map_cpu(struct pan_kmod_bo *bo,int prot,int flags,void *hint) {
 struct x9_dev *d=device(bo->dev);struct x9_bo *b=container_of(bo,struct x9_bo,base);
 if(hint||flags!=MAP_SHARED||prot!=(PROT_READ|PROT_WRITE)||(bo->flags&PAN_KMOD_BO_FLAG_NO_MMAP)){errno=ENOTSUP;return MAP_FAILED;}
 if(b->imported_host) {
  // Vulkan's map entrypoint requests Read+Write. Do not return a read-only
  // owner's pointer as a writable mapping; no-copy users retain their pointer.
  if((prot&PROT_WRITE)&&!(b->native_flags&X9RenderGPUWrite)){errno=EACCES;return MAP_FAILED;}
  return b->imported_host;
 }
 simple_mtx_lock(&d->lock);uint64_t bytes=0;
 void *ptr=d->failed?NULL:x9_render_map_buffer(d->connection,b->memory_type,&bytes);
 if(ptr&&bytes!=bo->size){x9_render_unmap_buffer(d->connection,b->memory_type,ptr);ptr=NULL;d->failed=true;errno=EPROTO;}
 simple_mtx_unlock(&d->lock);return ptr?ptr:MAP_FAILED;
}
/* Offset/size are BO-relative, not virtual addresses. Round outward only
 * inside the allocation's cache atoms; never wrap or widen to unrelated BOs.
 * This describes THIS cache operation, not all CPU writes or GPU resources. */
static int cache_request(struct pan_kmod_bo *bo,uint64_t offset,uint64_t bytes,
 enum pan_kmod_bo_sync_type type,struct X9RenderCacheRequest *req) {
 if(!bo||!bytes||offset>=bo->size||bytes>bo->size-offset||
    (type!=PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH&&type!=PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE))return error(EINVAL);
 uint64_t start=offset&~(uint64_t)(X9_RENDER_CACHE_ATOM-1);
 if(offset+bytes>UINT64_MAX-(X9_RENDER_CACHE_ATOM-1))return error(EINVAL);
 uint64_t end=(offset+bytes+X9_RENDER_CACHE_ATOM-1)&~(uint64_t)(X9_RENDER_CACHE_ATOM-1);
 if(end>bo->size)return error(EINVAL);
 *req=(struct X9RenderCacheRequest){X9_RENDER_ABI,bo->handle,
  type==PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH?X9RenderClean:(X9RenderClean|X9RenderInvalidate),0,start,end-start};
 return 0;
}
/* Caller holds d->lock. Byte counts describe completed cache calls, not GPU
 * work, a complete dirty set or bytes saved inside render_submit. */
static int cache_call(struct x9_dev *d,struct pan_kmod_bo *bo,const struct X9RenderCacheRequest *req) {
 int ret=call(d,X9RenderCache,req,sizeof(*req),NULL,0);
 if(!ret) {
  d->cache_ops++;d->cache_range_bytes+=req->bytes;d->cache_bo_bytes+=bo->size;
  if(d->cache_ops<=4||d->cache_ops%128==0)
   x9_panvk_log("X9 PANVK CACHE RANGE: calls=%llu range_bytes_avg=%llu bo_bytes_avg=%llu merged=%llu;explicit ops only,submission fallback retained",
    (unsigned long long)d->cache_ops,(unsigned long long)(d->cache_range_bytes/d->cache_ops),
    (unsigned long long)(d->cache_bo_bytes/d->cache_ops),(unsigned long long)d->cache_merged);
 }
 return ret;
}
int x9_pan_kmod_sync_cpu(struct pan_kmod_bo *bo,uint64_t offset,uint64_t bytes,enum pan_kmod_bo_sync_type type) {
 struct X9RenderCacheRequest req;
 if(cache_request(bo,offset,bytes,type,&req))return -1;
 struct x9_dev *d=device(bo->dev);
 simple_mtx_lock(&d->lock);int ret=cache_call(d,bo,&req);simple_mtx_unlock(&d->lock);
 return ret;
}
static int flush_maps(struct pan_kmod_dev *dev) {
 struct x9_dev *d=device(dev);int result=0;simple_mtx_lock(&d->lock);
 struct pan_kmod_bo *pending_bo=NULL;struct X9RenderCacheRequest pending={0};
 util_dynarray_foreach(&dev->pending_bo_syncs.array,struct pan_kmod_deferred_bo_sync,sync) {
  if(!sync->bo||sync->bo->dev!=dev||sync->start>sync->bo->size||sync->size>sync->bo->size-sync->start){result=error(EINVAL);d->failed=true;break;}
  if(!sync->size)continue;
  /* Deferred uploads (imports included) already carry exact ranges. Merge
   * consecutive overlapping/adjacent operations only for the SAME BO and
   * operation. Never reorder an invalidate past a clean or across another BO
   * which could be an alias. No merging across submits or immediate calls. */
  struct X9RenderCacheRequest req;
  if(cache_request(sync->bo,sync->start,sync->size,sync->type,&req)){result=-1;d->failed=true;break;}
  if(pending_bo==sync->bo&&pending.operation==req.operation&&
     req.offset<=pending.offset+pending.bytes&&pending.offset<=req.offset+req.bytes) {
   uint64_t start=MIN2(pending.offset,req.offset),end=MAX2(pending.offset+pending.bytes,req.offset+req.bytes);
   pending.offset=start;pending.bytes=end-start;d->cache_merged++;
  }else {
   if(pending_bo){result=cache_call(d,pending_bo,&pending);if(result)break;}
   pending_bo=sync->bo;pending=req;
  }
 }
 if(!result&&pending_bo)result=cache_call(d,pending_bo,&pending);
 simple_mtx_unlock(&d->lock);return result;
}
static bool wait_bo(struct pan_kmod_bo *bo,int64_t timeout,bool read_only) {
 /* Conservatively wait for the last submission on the entire connection.
  * Until command-resource dependency tracking exists, every BO is included. */
 (void)read_only;
 struct x9_dev *d=device(bo->dev);simple_mtx_lock(&d->lock);
 bool ready=false;
 if(d->failed)errno=EIO;
 else if(!d->last_submission||!d->last_group)ready=true;
 else {
  uint64_t ns=timeout<0?10000000000ULL:MIN2((uint64_t)timeout,10000000000ULL);
  struct X9RenderWaitRequest req={X9_RENDER_ABI,d->last_group,d->last_submission,ns};
  struct X9RenderCompletion reply={0};
  if(!call(d,X9RenderWait,&req,sizeof(req),&reply,sizeof(reply))) {
   ready=reply.abi==X9_RENDER_ABI&&reply.group==req.group&&reply.submission_id==req.submission_id&&
    reply.completed_id==req.submission_id&&!reply.status&&!reply.fault_status&&!reply.exception_type&&!reply.fault_address&&!reply.reserved;
   if(!ready){d->failed=true;errno=EPROTO;}
  }
 }
 simple_mtx_unlock(&d->lock);return ready;
}
static struct pan_kmod_vm *create_vm(struct pan_kmod_dev *dev,uint32_t flags,uint64_t start,uint64_t size) {
 struct x9_dev *d=device(dev);
 if(flags||!size||start<d->caps.va_start||start>=d->caps.va_end||size>d->caps.va_end-start){errno=ENOTSUP;return NULL;}
 simple_mtx_lock(&d->lock);
 if(d->vm||d->failed){simple_mtx_unlock(&d->lock);errno=EBUSY;return NULL;}
 struct pan_kmod_vm *vm=pan_kmod_dev_alloc(dev,sizeof(*vm));
 if(vm){pan_kmod_vm_init(vm,dev,0,flags);d->vm=vm;}
 simple_mtx_unlock(&d->lock);return vm;
}
static int unmap_slot(struct x9_dev *d,unsigned i) {
 if(call(d,X9RenderUnmap,&d->mappings[i],sizeof(d->mappings[i]),NULL,0))return -1;
 d->mappings[i]=d->mappings[--d->count];return 0;
}
static void destroy_vm(struct pan_kmod_vm *vm) {
 struct x9_dev *d=device(vm->dev);simple_mtx_lock(&d->lock);
 if(d->group_count)d->failed=true; /* groups must be destroyed before their VM */
 while(d->count&&!d->failed)if(unmap_slot(d,d->count-1))break;
 d->vm=NULL;simple_mtx_unlock(&d->lock);
 pan_kmod_vm_cleanup(vm);pan_kmod_dev_free(vm->dev,vm);
}
static int bind_vm(struct pan_kmod_vm *vm,enum pan_kmod_vm_op_mode mode,struct pan_kmod_vm_op *ops,uint32_t count) {
 struct x9_dev *d=device(vm->dev);
 if(mode!=PAN_KMOD_VM_OP_MODE_IMMEDIATE||count>X9_RENDER_MAX_MAPPINGS||(count&&!ops))return error(ENOTSUP);
 simple_mtx_lock(&d->lock);int result=0;
 if(d->failed||d->vm!=vm){result=error(EIO);goto done;}
 for(unsigned op=0;op<count;op++) {
  struct pan_kmod_vm_op *r=&ops[op];
  if(r->flags||r->signal.count||r->wait.count||!r->va.size||((r->va.start|r->va.size)&4095)||
     r->va.start<d->caps.va_start||r->va.start>=d->caps.va_end||r->va.size>d->caps.va_end-r->va.start){result=error(EINVAL);break;}
  if(r->type==PAN_KMOD_VM_OP_TYPE_MAP) {
   if(!r->map.bo||r->map.bo->dev!=vm->dev||r->map.bo_offset<0||((uint64_t)r->map.bo_offset&4095)||
      (uint64_t)r->map.bo_offset>r->map.bo->size||r->va.size>r->map.bo->size-(uint64_t)r->map.bo_offset||d->count==X9_PANVK_MAX_MAPPINGS) {
    result=error(EINVAL);break;
   }
   struct x9_bo *b=container_of(r->map.bo,struct x9_bo,base);
   struct X9RenderMapping request={X9_RENDER_ABI,b->base.handle,b->native_flags,0,(uint64_t)r->map.bo_offset,r->va.start,r->va.size};
   result=call(d,X9RenderMap,&request,sizeof(request),NULL,0);if(result)break;
   d->mappings[d->count++]=request;
  }else if(r->type==PAN_KMOD_VM_OP_TYPE_UNMAP) {
   /* PanVK unmaps its adjacent double-mapped descriptor ring in one range.
    * Validate coverage first, then remove the exact native mappings. Partial
    * native mappings require a split implementation and remain refused. */
   uint64_t at=r->va.start,end=at+r->va.size;
   while(at<end) {
    unsigned i=0;while(i<d->count&&d->mappings[i].gpu_address!=at)i++;
    if(i==d->count||d->mappings[i].bytes>end-at){result=error(ENOTSUP);break;}
    at+=d->mappings[i].bytes;
   }
   if(result)break;
   at=r->va.start;
   while(at<end) {
    unsigned i=0;while(i<d->count&&d->mappings[i].gpu_address!=at)i++;
    uint64_t bytes=d->mappings[i].bytes;
    if((result=unmap_slot(d,i)))break;at+=bytes;
   }
   if(result)break;
  }else {result=error(ENOTSUP);break;}
 }
 /* A multi-op request can partially succeed. Fail the VM instead of retrying
  * or presenting the remaining operations as an atomic rollback. */
 if(result)d->failed=true;
done:simple_mtx_unlock(&d->lock);return result;
}
static enum pan_kmod_vm_state vm_state(struct pan_kmod_vm *vm) {
 struct x9_dev *d=device(vm->dev);simple_mtx_lock(&d->lock);bool failed=d->failed;simple_mtx_unlock(&d->lock);
 return failed?PAN_KMOD_VM_FAULTY:PAN_KMOD_VM_USABLE;
}
static uint64_t timestamp(const struct pan_kmod_dev *dev) {
 struct x9_dev *d=device((struct pan_kmod_dev *)dev);uint64_t value=0;simple_mtx_lock(&d->lock);
 call(d,X9RenderTimestamp,NULL,0,&value,sizeof(value));simple_mtx_unlock(&d->lock);return value;
}
static const struct pan_kmod_ops ops={
 .dev_destroy=destroy,.dev_query_user_va_range=va_range,.bo_alloc=allocate,.bo_free=free_bo,
 .bo_import=import_bo,.bo_export=export_bo,.bo_mmap=map_cpu,.flush_bo_map_syncs=flush_maps,.bo_wait=wait_bo,
 .vm_create=create_vm,.vm_destroy=destroy_vm,.vm_bind=bind_vm,.vm_query_state=vm_state,.query_timestamp=timestamp,
};
struct pan_kmod_dev *x9_pan_kmod_create(const struct pan_kmod_allocator *allocator,struct util_sync_provider *sync) {
 if(!allocator||!sync||!sync->finalize){errno=EINVAL;return NULL;}
 struct x9_dev *d=pan_kmod_alloc(allocator,sizeof(*d));if(!d)return NULL;
 d->connection=x9_render_open(&d->caps);if(!d->connection){pan_kmod_free(allocator,d);return NULL;}
 const struct X9RenderCaps *c=&d->caps;
 if(c->cs_registers<4||c->cs_registers>256||(c->cs_registers&1)||c->cs_unpreserved_registers>c->cs_registers||!c->max_threads||!c->max_workgroup_threads||!(c->thread_features>>24)||!(c->thread_features&0x3fffff)||!c->timestamp_frequency) {
  x9_render_close(d->connection);pan_kmod_free(allocator,d);errno=EPROTO;return NULL;
 }
 struct pan_kmod_driver driver={0};
 if(pan_kmod_dev_init(&d->base,-1,PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL,&driver,allocator,&ops,sync)) {
  x9_render_close(d->connection);pan_kmod_free(allocator,d);return NULL;
 }
 simple_mtx_init(&d->lock,mtx_plain);
 d->base.props=(struct pan_kmod_dev_props){.gpu_id=c->gpu_id,.gpu_variant=c->core_features&255,.gpu_features=c->gpu_features,
  .shader_present=c->shader_present,.tiler_features=c->tiler_features,.mem_features=c->mem_features,.mmu_features=c->mmu_features,
  .l2_features=c->l2_features,.max_threads_per_core=c->max_threads,.max_threads_per_wg=c->max_workgroup_threads,
  .max_tasks_per_core=c->thread_features>>24,.num_registers_per_core=c->thread_features&0x3fffff,
  .max_tls_instance_per_core=c->max_threads,.afbc_features=c->afbc_features,.gpu_can_query_timestamp=true,
  .timestamp_frequency=c->timestamp_frequency,.timestamp_cycles_to_ns_factor=1e9/(double)c->timestamp_frequency,
  .allowed_group_priorities_mask=PAN_KMOD_GROUP_ALLOW_PRIORITY_LOW|PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM,
  .supported_bo_flags=X9_BO_FLAGS,.pgsize_bitmap=PAN_PGSIZE_4K,.is_io_coherent=false};
 /* Frozen Mesa cs_builder.h defines CS_MAX_SB_COUNT16 for PAN_ARCH>=11.
  * x9_render_open has already required the exact e8800010 v14 GPU. */
 d->csif=(struct drm_panthor_csif_info){.cs_slot_count=c->max_streams,.cs_reg_count=c->cs_registers,.scoreboard_slot_count=16,
  .unpreserved_cs_reg_count=c->cs_unpreserved_registers};
 memcpy(d->base.props.texture_features,c->texture_features,sizeof(c->texture_features));return &d->base;
}

int x9_pan_kmod_group_create(struct pan_kmod_dev *dev,const struct X9RenderGroup *request,struct X9RenderGroup *reply) {
 if(!dev||dev->ops!=&ops||!request||!reply)return error(EINVAL);
 struct x9_dev *d=device(dev);simple_mtx_lock(&d->lock);int ret=-1;
 if(!d->vm||d->group_count==ARRAY_SIZE(d->groups)){errno=ENOSPC;goto done;}
 struct X9RenderGroup actual={0};
 ret=call(d,X9RenderCreateGroup,request,sizeof(*request),&actual,sizeof(actual));
 if(!ret) {
  if(actual.abi!=X9_RENDER_ABI||!actual.handle||actual.stream_count!=request->stream_count||actual.reserved||
     actual.compute_mask!=request->compute_mask||actual.fragment_mask!=request->fragment_mask||actual.tiler_mask!=request->tiler_mask||
     actual.priority!=request->priority||actual.max_compute!=request->max_compute||actual.max_fragment!=request->max_fragment||
     actual.max_tiler!=request->max_tiler||actual.tiler_heap_bytes!=request->tiler_heap_bytes)ret=error(EPROTO);
  for(unsigned i=0;!ret&&i<d->group_count;i++)if(d->groups[i]==actual.handle)ret=error(EPROTO);
  if(ret)d->failed=true;else {d->groups[d->group_count++]=actual.handle;*reply=actual;}
 }
done:simple_mtx_unlock(&d->lock);return ret;
}
int x9_pan_kmod_group_destroy(struct pan_kmod_dev *dev,uint32_t group) {
 if(!dev||dev->ops!=&ops||!group)return error(EINVAL);
 struct x9_dev *d=device(dev);simple_mtx_lock(&d->lock);
 unsigned i=0;while(i<d->group_count&&d->groups[i]!=group)i++;
 int ret=-1;
 if(i==d->group_count)errno=EINVAL;
 else {
  struct X9RenderHandle req={X9_RENDER_ABI,group};
  ret=call(d,X9RenderDestroyGroup,&req,sizeof(req),NULL,0);
  if(!ret){d->groups[i]=d->groups[--d->group_count];if(d->last_group==group)d->last_group=0;}
 }
 simple_mtx_unlock(&d->lock);return ret;
}
int x9_pan_kmod_submit(struct pan_kmod_dev *dev,uint32_t group,const struct X9RenderStream *streams,uint32_t count,struct X9RenderCompletion *reply) {
 if(!dev||dev->ops!=&ops||!streams||!reply||!count||count>X9_RENDER_MAX_STREAMS)return error(EINVAL);
 struct x9_dev *d=device(dev);
 /* Cache flush takes the native lock itself, so do it before entering it. */
 pan_kmod_flush_bo_map_syncs(dev);
 simple_mtx_lock(&d->lock);int ret=-1;
 if(d->failed){errno=EIO;goto done;}
 unsigned i=0;while(i<d->group_count&&d->groups[i]!=group)i++;
 if(i==d->group_count||d->last_submission==UINT64_MAX){errno=EINVAL;goto done;}
 struct X9RenderSubmission request={.abi=X9_RENDER_ABI,.group=group,.stream_count=count,.submission_id=d->last_submission+1};
 memcpy(request.streams,streams,count*sizeof(*streams));
 struct X9RenderCompletion actual={0};
 ret=call(d,X9RenderSubmit,&request,sizeof(request),&actual,sizeof(actual));
 if(!ret) {
  /* Native ABI currently requires synchronous completion plus cleanup. A
   * queued/pending acknowledgment is not accepted as rendering success. */
  if(actual.abi!=X9_RENDER_ABI||actual.group!=group||actual.submission_id!=request.submission_id||
     actual.completed_id!=request.submission_id||actual.status||actual.reserved||actual.fault_status||actual.exception_type||actual.fault_address) {
   d->failed=true;ret=error(EPROTO);
  }else {d->last_submission=request.submission_id;d->last_group=group;*reply=actual;}
 }
done:simple_mtx_unlock(&d->lock);return ret;
}

int x9_pan_kmod_unmap_cpu(struct pan_kmod_bo *bo,void *address) {
 if(!bo||bo->dev->ops!=&ops||!address)return error(EINVAL);
 struct x9_dev *d=device(bo->dev);struct x9_bo *b=container_of(bo,struct x9_bo,base);
 simple_mtx_lock(&d->lock);
 int ret=b->imported_host?(address==b->imported_host?0:error(EINVAL)):
  x9_render_unmap_buffer(d->connection,b->memory_type,address);
 if(ret)d->failed=true;
 simple_mtx_unlock(&d->lock);return ret;
}

int x9_pan_kmod_caps(struct pan_kmod_dev *dev,struct X9RenderCaps *caps) {
 if(!dev||dev->ops!=&ops||!caps)return error(ENODEV);
 struct x9_dev *d=device(dev);simple_mtx_lock(&d->lock);
 int ret=d->failed?error(EIO):0;if(!ret)*caps=d->caps;
 simple_mtx_unlock(&d->lock);return ret;
}

const struct drm_panthor_csif_info *x9_pan_kmod_csif(struct pan_kmod_dev *dev) {
 if(!dev||dev->ops!=&ops){errno=ENODEV;return NULL;}
 return &device(dev)->csif; /* immutable register limits from native caps */
}

int x9_pan_kmod_group_check(struct pan_kmod_dev *dev,uint32_t group) {
 if(!dev||dev->ops!=&ops)return error(EINVAL);
 struct x9_dev *d=device(dev);simple_mtx_lock(&d->lock);int ret=0;
 unsigned i=0;while(i<d->group_count&&d->groups[i]!=group)i++;
 if(d->failed)ret=error(EIO);
 else if(i==d->group_count)ret=error(EINVAL);
 else if(d->last_group==group&&d->last_submission) {
  struct X9RenderWaitRequest req={X9_RENDER_ABI,group,d->last_submission,0};
  struct X9RenderCompletion reply={0};
  ret=call(d,X9RenderWait,&req,sizeof(req),&reply,sizeof(reply));
  if(!ret&&(reply.abi!=X9_RENDER_ABI||reply.group!=group||reply.submission_id!=d->last_submission||
     reply.completed_id!=d->last_submission||reply.status||reply.reserved||reply.fault_status||reply.exception_type||reply.fault_address)) {
   d->failed=true;ret=error(EPROTO);
  }
 }
 /* Older groups have no pending jobs: this backend submits synchronously
  * under the device lock and has validated every intervening completion. */
 simple_mtx_unlock(&d->lock);return ret;
}

struct pan_kmod_bo *x9_pan_kmod_import_host(struct pan_kmod_dev *dev,struct pan_kmod_vm *vm,void *host,uint64_t bytes) {
 if(!dev||dev->ops!=&ops||!vm||vm->dev!=dev||!host||((uintptr_t)host&16383)||!bytes||(bytes&16383)||
    bytes>X9_RENDER_BUFFER_LIMIT||bytes>UINTPTR_MAX-(uintptr_t)host){errno=EINVAL;return NULL;}
 struct x9_dev *d=device(dev);struct x9_bo *bo=pan_kmod_dev_alloc(dev,sizeof(*bo));if(!bo)return NULL;
 struct X9RenderAllocation req={X9_RENDER_ABI,X9RenderGPURead|X9RenderGPUWrite,bytes,(uint64_t)(uintptr_t)host};
 struct X9RenderBuffer reply={0};simple_mtx_lock(&d->lock);
 int ret;
 if(vm!=d->vm||d->failed)ret=error(EIO);
 else {
  ret=x9_render_call(d->connection,X9RenderImportHost,&req,sizeof(req),&reply,sizeof(reply));
  if(ret&&errno==EACCES) {
   // The kernel guarantees this protection refusal published no handle. Retry
   // exactly once with an explicit read-only grant, not a copied allocation or
   // a VM-region guess. bind_vm carries native_flags to the GPU PTEs; stores
   // through this BO must fault rather than write pages the owner cannot write.
   req.flags=X9RenderGPURead;
   memset(&reply,0,sizeof(reply));
   ret=x9_render_call(d->connection,X9RenderImportHost,&req,sizeof(req),&reply,sizeof(reply));
  }
  if(ret) {
   int saved_errno=errno;
   x9_panvk_log("X9 PANVK IMPORT: host=%llx bytes=%llu flags=%x errno=%d detail=%s",
    (unsigned long long)req.host_address,(unsigned long long)bytes,req.flags,saved_errno,x9_render_last_error());
   /* The service explicitly guarantees that this refusal publishes no handle.
    * Protocol, mapping, cache and submission uncertainty still poison the VM. */
   if(saved_errno!=EACCES&&saved_errno!=ENOMEM&&saved_errno!=ENOSPC)d->failed=true;
   errno=saved_errno;
  }
 }
 if(!ret&&(reply.abi!=X9_RENDER_ABI||!reply.handle||!reply.memory_type||reply.flags!=req.flags||reply.bytes!=bytes)) {
  d->failed=true;ret=error(EPROTO);
 }
 simple_mtx_unlock(&d->lock);
 if(ret){pan_kmod_dev_free(dev,bo);return NULL;}
 if(req.flags==X9RenderGPURead)x9_panvk_log("X9 PANVK IMPORT: read-only grant host=%llx bytes=%llu handle=%u;GPU stores forbidden",
  (unsigned long long)req.host_address,(unsigned long long)bytes,reply.handle);
 bo->memory_type=reply.memory_type;bo->native_flags=req.flags;bo->imported_host=host;
 pan_kmod_bo_init(&bo->base,dev,vm,bytes,PAN_KMOD_BO_FLAG_IMPORTED|PAN_KMOD_BO_FLAG_WB_MMAP,reply.handle);
 /* Unlike bo_alloc's wrapper, this explicit import must install its BO lookup
  * entry itself. Each native import has its own kernel pin and unique handle. */
 simple_mtx_lock(&dev->handle_to_bo.lock);
 struct pan_kmod_bo **slot=util_sparse_array_get(&dev->handle_to_bo.array,reply.handle);
 if(!slot||*slot) {
  simple_mtx_unlock(&dev->handle_to_bo.lock);simple_mtx_lock(&d->lock);
  struct X9RenderHandle release={X9_RENDER_ABI,reply.handle};call(d,X9RenderRelease,&release,sizeof(release),NULL,0);
  simple_mtx_unlock(&d->lock);pan_kmod_dev_free(dev,bo);errno=ENOMEM;return NULL;
 }
 *slot=&bo->base;simple_mtx_unlock(&dev->handle_to_bo.lock);return &bo->base;
}
