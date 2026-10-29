/* SPDX-License-Identifier: MIT */
#include "X9RenderKernel.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
static const X9RenderEngine *render_engine;
static bool render_published;
extern "C" { int sweet_x9_render_payload_ready=0; }
/* Invoked by the existing accelerator's caller-scoped property lookup.
 * Desktop discovery may precede the async GPU worker: wait for actual service
 * registration, without holding the GPU owner lock or creating a fake device. */
extern "C" bool sweet_x9_render_discovery_ready(void) {
 int mode=0;
 if(!__atomic_load_n(&sweet_x9_render_payload_ready,__ATOMIC_ACQUIRE)||
    !PE_parse_boot_argn("x9_metal_render",&mode,sizeof(mode))||mode!=1)return false;
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
  if(context->disabled)return kIOReturnNotReady;
  if(selector==X9RenderAllocate||selector==X9RenderImportHost) {
   if(a->structureInputSize!=sizeof(X9RenderAllocation)||a->structureOutputSize!=sizeof(X9RenderBuffer))return kIOReturnBadArgument;
   const auto &request=*(const X9RenderAllocation *)a->structureInput;
   if(request.abi!=X9_RENDER_ABI||!good_flags(request.flags)||!request.bytes||(request.bytes&16383)||
      request.bytes>X9_RENDER_BUFFER_LIMIT||request.bytes>X9_RENDER_CLIENT_LIMIT-context->allocated_bytes)return kIOReturnBadArgument;
   if(selector==X9RenderAllocate&&request.host_address)return kIOReturnBadArgument;
   if(selector==X9RenderImportHost&&(!request.host_address||(request.host_address&16383)||request.host_address+request.bytes<request.host_address||
      request.host_address>=0x0000800000000000ULL||request.bytes>0x0000800000000000ULL-request.host_address))return kIOReturnBadArgument;
   unsigned slot=0;while(slot<X9_RENDER_MAX_MAPPINGS&&context->resources[slot].memory)slot++;
   if(slot==X9_RENDER_MAX_MAPPINGS||context->next_handle==UINT32_MAX)return kIOReturnNoResources;
   IOMemoryDescriptor *memory=selector==X9RenderAllocate?
    IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut|kIOMemoryKernelUserShared,request.bytes,16384):
    IOMemoryDescriptor::withAddressRange(request.host_address,request.bytes,kIODirectionInOut,context->owner);
   if(!memory)return kIOReturnNoMemory;
   IOReturn kr=memory->prepare(kIODirectionInOut);
   if(kr!=kIOReturnSuccess){memory->release();return kr;}
   if(selector==X9RenderAllocate)bzero(((IOBufferMemoryDescriptor *)memory)->getBytesNoCopy(),request.bytes);
   uint32_t handle=++context->next_handle;
   context->resources[slot]={memory,request.bytes,handle,request.flags,true};context->allocated_bytes+=request.bytes;
   X9RenderBuffer reply={X9_RENDER_ABI,handle,handle,request.flags,request.bytes};
   memcpy(a->structureOutput,&reply,sizeof(reply));return kIOReturnSuccess;
  }
  if(selector==X9RenderRelease) {
   if(a->structureInputSize!=sizeof(X9RenderHandle)||a->structureOutputSize)return kIOReturnBadArgument;
   const auto &request=*(const X9RenderHandle *)a->structureInput;auto *r=resource(request.handle);
   if(request.abi!=X9_RENDER_ABI||!r)return kIOReturnBadArgument;
   if(context->exposed||mapping_exists(r))return kIOReturnBusy;
   if(r->prepared)r->memory->complete(kIODirectionInOut);
   r->memory->release();context->allocated_bytes-=r->bytes;bzero(r,sizeof(*r));return kIOReturnSuccess;
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
   if(context->mapping_count==X9_RENDER_MAX_MAPPINGS)return kIOReturnNoResources;
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
  if(render_engine->context_open(context)!=kIOReturnSuccess)return false;
  context->engine_open=true;
  char caller[32]={0};proc_selfname(caller,sizeof(caller));
  IOLog("X9 RENDER CLIENT: opened caller=%s pid=%d real context\n",caller,proc_selfpid());return true;
 }
 IOReturn clientClose(void) APPLE_KEXT_OVERRIDE {if(!isInactive())terminate();return kIOReturnSuccess;}
 void free(void) APPLE_KEXT_OVERRIDE {
  if(context) {
   if(context->engine_open&&!render_engine->context_close(context)) {
    IOLog("X9 RENDER: unresolved client teardown;prepared resources/context retained,no reset\n");context=nullptr;
   }else {
    for(auto &r:context->resources)if(r.memory){if(r.prepared)r.memory->complete(kIODirectionInOut);r.memory->release();}
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
  if(!r||context->disabled){IOLockUnlock(context->lock);return kIOReturnBadArgument;}
  r->memory->retain();*memory=r->memory;*options=0;IOLockUnlock(context->lock);return kIOReturnSuccess;
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
  auto *client=new SweetX9RenderUserClient;if(!client)return kIOReturnNoMemory;
  if(!client->initWithTask(task,security,type,props)||!client->attach(this)||!client->start(this)) {
   if(client->getProvider()==this)client->detach(this);client->release();return kIOReturnNotPrivileged;
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
