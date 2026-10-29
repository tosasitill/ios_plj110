/* SPDX-License-Identifier: MIT */
#include "X9PanVKMemory.h"
#include "kmod/pan_kmod_backend.h"
#include "../X9RenderTransport.h"
#include "drm-uapi/panthor_drm.h"
#include <errno.h>
#include <string.h>
#define X9_BO_FLAGS (PAN_KMOD_BO_FLAG_EXECUTABLE|PAN_KMOD_BO_FLAG_NO_MMAP|PAN_KMOD_BO_FLAG_WB_MMAP)
struct x9_dev {
 struct pan_kmod_dev base;
 struct X9RenderConnection *connection;
 struct X9RenderCaps caps;
 struct drm_panthor_csif_info csif;
 simple_mtx_t lock;
 struct pan_kmod_vm *vm;
 struct X9RenderMapping mappings[X9_RENDER_MAX_MAPPINGS];
 uint32_t count,groups[32],group_count,last_group;
 uint64_t last_submission;
 bool failed;
};
struct x9_bo {struct pan_kmod_bo base;uint32_t memory_type,native_flags;void *imported_host;};
static struct x9_dev *device(struct pan_kmod_dev *dev){return container_of(dev,struct x9_dev,base);}
static int error(int e){errno=e;return -1;}
static int call(struct x9_dev *d,uint32_t op,const void *in,size_t ilen,void *out,size_t olen) {
 if(d->failed)return error(EIO);
 int ret=x9_render_call(d->connection,op,in,ilen,out,olen);
 if(ret)d->failed=true;
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
 if(!size||size>d->caps.max_buffer_bytes||(flags&~X9_BO_FLAGS)|| (vm&&vm!=d->vm)){errno=EINVAL;return NULL;}
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
 if(ret){pan_kmod_dev_free(dev,b);return NULL;}
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
 if(b->imported_host)return b->imported_host;
 simple_mtx_lock(&d->lock);uint64_t bytes=0;
 void *ptr=d->failed?NULL:x9_render_map_buffer(d->connection,b->memory_type,&bytes);
 if(ptr&&bytes!=bo->size){x9_render_unmap_buffer(d->connection,b->memory_type,ptr);ptr=NULL;d->failed=true;errno=EPROTO;}
 simple_mtx_unlock(&d->lock);return ptr?ptr:MAP_FAILED;
}
int x9_pan_kmod_sync_cpu(struct pan_kmod_bo *bo,uint64_t offset,uint64_t bytes,enum pan_kmod_bo_sync_type type) {
 if(!bo||!bytes||offset>=bo->size||bytes>bo->size-offset||
    (type!=PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH&&type!=PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE))return error(EINVAL);
 struct x9_dev *d=device(bo->dev);
 uint64_t start=offset&~(uint64_t)(X9_RENDER_CACHE_ATOM-1);
 uint64_t end=(offset+bytes+X9_RENDER_CACHE_ATOM-1)&~(uint64_t)(X9_RENDER_CACHE_ATOM-1);
 if(end>bo->size)return error(EINVAL);
 struct X9RenderCacheRequest req={X9_RENDER_ABI,bo->handle,
  type==PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH?X9RenderClean:(X9RenderClean|X9RenderInvalidate),0,start,end-start};
 simple_mtx_lock(&d->lock);int ret=call(d,X9RenderCache,&req,sizeof(req),NULL,0);simple_mtx_unlock(&d->lock);
 return ret;
}
static int flush_maps(struct pan_kmod_dev *dev) {
 struct x9_dev *d=device(dev);int result=0;simple_mtx_lock(&d->lock);
 util_dynarray_foreach(&dev->pending_bo_syncs.array,struct pan_kmod_deferred_bo_sync,sync) {
  if(sync->start>sync->bo->size||sync->size>sync->bo->size-sync->start){result=error(EINVAL);d->failed=true;break;}
  if(!sync->size)continue;
  /* Use entire buffer: allocation owns every surrounding cache line. This
   * runs before submission; post-GPU invalidate belongs to completion. */
  struct X9RenderCacheRequest req={X9_RENDER_ABI,sync->bo->handle,
   sync->type==PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH?X9RenderClean:(X9RenderClean|X9RenderInvalidate),0,0,sync->bo->size};
  result=call(d,X9RenderCache,&req,sizeof(req),NULL,0);if(result)break;
 }
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
      (uint64_t)r->map.bo_offset>r->map.bo->size||r->va.size>r->map.bo->size-(uint64_t)r->map.bo_offset||d->count==X9_RENDER_MAX_MAPPINGS) {
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
 int ret=vm!=d->vm?error(EINVAL):call(d,X9RenderImportHost,&req,sizeof(req),&reply,sizeof(reply));
 if(!ret&&(reply.abi!=X9_RENDER_ABI||!reply.handle||!reply.memory_type||reply.flags!=req.flags||reply.bytes!=bytes)) {
  d->failed=true;ret=error(EPROTO);
 }
 simple_mtx_unlock(&d->lock);
 if(ret){pan_kmod_dev_free(dev,bo);return NULL;}
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
