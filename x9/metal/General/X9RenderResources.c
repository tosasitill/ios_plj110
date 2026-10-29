/* SPDX-License-Identifier: MIT */
#include "X9RenderResources.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
struct Resource {
 struct X9RenderResourceView view;
 uint32_t memory_type;
 bool host_import,gpu_mapped,cpu_mapped;
};
struct X9RenderResources {
 pthread_mutex_t lock;
 struct X9RenderConnection *connection;
 struct X9RenderCaps caps;
 struct Resource slots[X9_RENDER_MAX_MAPPINGS];
 uint64_t bytes;
 bool poisoned;
};
static int invalid(int error) {errno=error;return -1;}
static struct Resource *find(struct X9RenderResources *r,uint32_t handle) {
 if(!handle)return NULL;
 for(unsigned i=0;i<X9_RENDER_MAX_MAPPINGS;i++)if(r->slots[i].view.handle==handle)return &r->slots[i];
 return NULL;
}
static uint64_t find_va(struct X9RenderResources *r,uint64_t bytes) {
 uint64_t at=r->caps.va_start;
 /* First fit over bounded live mappings; recycled VA is usable only after
  * successful native unmap, CPU unmap and resource release. */
 for(unsigned pass=0;pass<=X9_RENDER_MAX_MAPPINGS;pass++) {
  if(at>=r->caps.va_end||bytes>r->caps.va_end-at)return 0;
  uint64_t next=at;
  for(unsigned i=0;i<X9_RENDER_MAX_MAPPINGS;i++) {
   const struct X9RenderResourceView *v=&r->slots[i].view;
   if(v->handle&&v->gpu_address&&at<v->gpu_address+v->bytes&&v->gpu_address<at+bytes)
    if(next<v->gpu_address+v->bytes)next=v->gpu_address+v->bytes;
  }
  if(next==at)return at;at=next;
 }
 return 0;
}
struct X9RenderResources *x9_resources_open(struct X9RenderCaps *caps) {
 struct X9RenderResources *r=calloc(1,sizeof(*r));if(!r)return NULL;
 int error=pthread_mutex_init(&r->lock,NULL);if(error){free(r);errno=error;return NULL;}
 r->connection=x9_render_open(&r->caps);
 if(!r->connection){pthread_mutex_destroy(&r->lock);free(r);return NULL;}
 if(r->caps.max_mappings>X9_RENDER_MAX_MAPPINGS||r->caps.max_buffer_bytes>X9_RENDER_BUFFER_LIMIT||
    r->caps.max_client_bytes>X9_RENDER_CLIENT_LIMIT) {
  x9_render_close(r->connection);pthread_mutex_destroy(&r->lock);free(r);errno=EPROTO;return NULL;
 }
 if(caps)*caps=r->caps;return r;
}
/* On any uncertain teardown leave all state for connection close. In
 * particular never recycle a VA or buffer merely because CPU unmap worked. */
static int release_locked(struct X9RenderResources *r,struct Resource *s) {
 if(s->gpu_mapped) {
  struct X9RenderMapping request={X9_RENDER_ABI,s->view.handle,s->view.flags,0,0,s->view.gpu_address,s->view.bytes};
  if(x9_render_call(r->connection,X9RenderUnmap,&request,sizeof(request),NULL,0))return -1;
  s->gpu_mapped=false;
 }
 if(s->cpu_mapped) {
  if(x9_render_unmap_buffer(r->connection,s->memory_type,s->view.cpu_address))return -1;
  s->cpu_mapped=false;
 }
 struct X9RenderHandle request={X9_RENDER_ABI,s->view.handle};
 if(x9_render_call(r->connection,X9RenderRelease,&request,sizeof(request),NULL,0))return -1;
 r->bytes-=s->view.bytes;memset(s,0,sizeof(*s));return 0;
}
static int create_locked(struct X9RenderResources *r,void *host,uint64_t bytes,uint32_t flags,struct X9RenderResourceView *out) {
 if(r->poisoned)return invalid(EIO);
 if(!bytes||(bytes&16383)||bytes>r->caps.max_buffer_bytes||!flags||(flags&~7u)||
    ((flags&4u)&&(flags&2u))||(host&&((uintptr_t)host&16383)))return invalid(EINVAL);
 if(bytes>r->caps.max_client_bytes-r->bytes)return invalid(ENOMEM);
 unsigned index=0;while(index<r->caps.max_mappings&&r->slots[index].view.handle)index++;
 if(index==r->caps.max_mappings)return invalid(ENOSPC);
 uint64_t va=find_va(r,bytes);if(!va)return invalid(ENOSPC);
 struct X9RenderAllocation request={X9_RENDER_ABI,flags,bytes,(uint64_t)(uintptr_t)host};
 struct X9RenderBuffer reply={0};
 if(x9_render_call(r->connection,host?X9RenderImportHost:X9RenderAllocate,&request,sizeof(request),&reply,sizeof(reply)))return -1;
 if(reply.abi!=X9_RENDER_ABI||!reply.handle||!reply.memory_type||reply.bytes!=bytes||reply.flags!=flags||find(r,reply.handle)) {
  r->poisoned=true;return invalid(EPROTO);
 }
 struct Resource *s=&r->slots[index];
 s->view=(struct X9RenderResourceView){reply.handle,flags,bytes,va,host};
 s->memory_type=reply.memory_type;s->host_import=host!=NULL;r->bytes+=bytes;
 if(!host) {
  uint64_t mapped_bytes=0;
  s->view.cpu_address=x9_render_map_buffer(r->connection,s->memory_type,&mapped_bytes);
  if(!s->view.cpu_address)goto rollback;
  s->cpu_mapped=true;
  if(mapped_bytes!=bytes){errno=EPROTO;goto rollback;}
 }
 struct X9RenderMapping map={X9_RENDER_ABI,reply.handle,flags,0,0,va,bytes};
 if(x9_render_call(r->connection,X9RenderMap,&map,sizeof(map),NULL,0)) {
  /* A failed map can leave uncertain hardware state. Do not attempt another
   * operation on that connection; close delegates quarantine to the KMD. */
  r->poisoned=true;return -1;
 }
 s->gpu_mapped=true;*out=s->view;return 0;
rollback: {
 int error=errno;
 if(release_locked(r,s))r->poisoned=true;
 errno=error;return -1;
 }
}
int x9_resources_allocate(struct X9RenderResources *r,uint64_t bytes,uint32_t flags,struct X9RenderResourceView *out) {
 if(!r||!out)return invalid(EINVAL);memset(out,0,sizeof(*out));
 pthread_mutex_lock(&r->lock);int result=create_locked(r,NULL,bytes,flags,out);pthread_mutex_unlock(&r->lock);return result;
}
int x9_resources_import(struct X9RenderResources *r,void *host,uint64_t bytes,uint32_t flags,struct X9RenderResourceView *out) {
 if(!r||!out||!host)return invalid(EINVAL);memset(out,0,sizeof(*out));
 pthread_mutex_lock(&r->lock);int result=create_locked(r,host,bytes,flags,out);pthread_mutex_unlock(&r->lock);return result;
}
int x9_resources_cache(struct X9RenderResources *r,uint32_t handle,uint64_t offset,uint64_t bytes,uint32_t operation) {
 if(!r)return invalid(EINVAL);
 pthread_mutex_lock(&r->lock);struct Resource *s=find(r,handle);int result=-1;
 if(r->poisoned)errno=EIO;
 else if(!s||!bytes||offset>s->view.bytes||bytes>s->view.bytes-offset||!operation||(operation&~3u))errno=EINVAL;
 else {
  struct X9RenderCacheRequest request={X9_RENDER_ABI,handle,operation,0,offset,bytes};
  result=x9_render_call(r->connection,X9RenderCache,&request,sizeof(request),NULL,0);
 }
 pthread_mutex_unlock(&r->lock);return result;
}
int x9_resources_release(struct X9RenderResources *r,uint32_t handle) {
 if(!r)return invalid(EINVAL);
 pthread_mutex_lock(&r->lock);struct Resource *s=find(r,handle);int result=-1;
 if(r->poisoned)errno=EIO;
 else if(!s)errno=EINVAL;
 else {result=release_locked(r,s);if(result)r->poisoned=true;}
 pthread_mutex_unlock(&r->lock);return result;
}
void x9_resources_close(struct X9RenderResources *r) {
 if(!r)return;
 pthread_mutex_lock(&r->lock);
 /* CPU mappings have their own task references; explicitly unmap even when
  * the native engine will retain GPU-visible backing pages on uncertainty. */
 for(unsigned i=0;i<X9_RENDER_MAX_MAPPINGS;i++) {
  struct Resource *s=&r->slots[i];
  if(s->cpu_mapped)x9_render_unmap_buffer(r->connection,s->memory_type,s->view.cpu_address);
 }
 x9_render_close(r->connection);pthread_mutex_unlock(&r->lock);
 pthread_mutex_destroy(&r->lock);free(r);
}
