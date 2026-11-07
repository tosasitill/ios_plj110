/* SPDX-License-Identifier: MIT */
#include "X9RenderTransport.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct X9RenderConnection { io_connect_t port; struct X9RenderCaps caps; };
static _Thread_local char x9_render_error[192];
static int failure(const char *operation,kern_return_t result) {
 snprintf(x9_render_error,sizeof(x9_render_error),"%s: IOKit=0x%08x",operation,result);
 errno=result==kIOReturnNotPrivileged?EACCES:result==kIOReturnNoMemory?ENOMEM:result==kIOReturnUnsupported?ENOTSUP:
  result==kIOReturnTimeout?ETIMEDOUT:EIO;
 return -1;
}
const char *x9_render_last_error(void) {return x9_render_error;}
uint64_t x9_render_submission_cache_atom(void) {
 struct X9RenderCaps caps={0};
 struct X9RenderConnection *connection=x9_render_open(&caps);if(!connection)return 0;
 x9_render_close(connection);
 return (caps.capabilities&X9RenderSubmissionCacheOwnership)?X9_RENDER_CACHE_ATOM:0;
}
struct X9RenderConnection *x9_render_open(struct X9RenderCaps *caps) {
 io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(X9_RENDER_SERVICE));
 if(!service){snprintf(x9_render_error,sizeof(x9_render_error),"%s service unavailable",X9_RENDER_SERVICE);errno=ENODEV;return NULL;}
 io_connect_t port=IO_OBJECT_NULL;
 kern_return_t result=IOServiceOpen(service,mach_task_self(),0,&port);IOObjectRelease(service);
 if(result!=KERN_SUCCESS){failure("open native renderer",result);return NULL;}
 struct X9RenderCaps actual={0};size_t bytes=sizeof(actual);
 result=IOConnectCallStructMethod(port,X9RenderGetCaps,NULL,0,&actual,&bytes);
 if(result!=KERN_SUCCESS){failure("native caps",result);IOServiceClose(port);return NULL;}
 if(bytes!=sizeof(actual)||actual.abi!=X9_RENDER_ABI||actual.bytes!=sizeof(actual)||
    actual.gpu_id!=0xe8800010ULL||actual.host_page_bytes!=16384||
    actual.max_streams<X9_RENDER_MAX_STREAMS||!actual.max_mappings||
    actual.va_start>=actual.va_end||actual.va_start<X9_RENDER_MIN_VA||actual.va_end>X9_RENDER_MAX_VA||
    (actual.capabilities&(X9RenderBufferObjects|X9RenderHostImport|X9RenderCSFSubmit|X9RenderTiler|X9RenderFragment))!=(X9RenderBufferObjects|X9RenderHostImport|X9RenderCSFSubmit|X9RenderTiler|X9RenderFragment)) {
  snprintf(x9_render_error,sizeof(x9_render_error),"native ABI/capability mismatch; complete CSF tiler/fragment backend required");
  IOServiceClose(port);errno=ENOTSUP;return NULL;
 }
 struct X9RenderConnection *connection=calloc(1,sizeof(*connection));
 if(!connection){IOServiceClose(port);errno=ENOMEM;return NULL;}
 connection->port=port;connection->caps=actual;
 if(caps)*caps=actual;return connection;
}
/* Frontend cache scope (rev252bx): one type-1 connection per process, opened on
 * first use. A kernel without the scope client refuses the open; the frontend
 * then stops calling (maintenance stays full). Returns false when unavailable. */
#include <pthread.h>
static pthread_mutex_t x9_scope_lock=PTHREAD_MUTEX_INITIALIZER;
static io_connect_t x9_scope_port=IO_OBJECT_NULL;
static int x9_scope_state;   /* 0 untried, 1 open, -1 unavailable */
bool x9_render_scope(uint32_t operation,uint64_t address,uint64_t bytes) {
 pthread_mutex_lock(&x9_scope_lock);
 if(!x9_scope_state) {
  io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(X9_RENDER_SERVICE));
  kern_return_t r=service?IOServiceOpen(service,mach_task_self(),X9_RENDER_SCOPE_CLIENT,&x9_scope_port):KERN_FAILURE;
  if(service)IOObjectRelease(service);
  x9_scope_state=r==KERN_SUCCESS?1:-1;
 }
 bool ok=false;
 if(x9_scope_state==1) {
  struct X9RenderScopeRequest q={X9_RENDER_ABI,operation,address,bytes};
  ok=IOConnectCallStructMethod(x9_scope_port,X9RenderScope,&q,sizeof(q),NULL,NULL)==KERN_SUCCESS;
 }
 pthread_mutex_unlock(&x9_scope_lock);
 return ok;
}
void x9_render_close(struct X9RenderConnection *connection) {
 if(!connection)return;
 IOServiceClose(connection->port);free(connection);
}
int x9_render_call(struct X9RenderConnection *connection,uint32_t operation,const void *input,size_t input_bytes,void *output,size_t output_bytes) {
 if(!connection||operation>X9RenderFenceWait||(input_bytes&&!input)||(output_bytes&&!output)) {errno=EINVAL;return -1;}
 size_t actual=output_bytes;
 kern_return_t result=IOConnectCallStructMethod(connection->port,operation,input,input_bytes,output,&actual);
 if(result!=KERN_SUCCESS) {
  failure("native operation",result);
  /* These allocation/import refusals publish no handle; all other operations
   * retain their existing fail-closed policy, including cache/map ENOMEM. */
  if((operation==X9RenderAllocate||operation==X9RenderImportHost)&&result==kIOReturnNoResources)errno=ENOSPC;
  if(operation==X9RenderImportHost&&result==kIOReturnNotPermitted)errno=EACCES;
  return -1;
 }
 if(actual!=output_bytes){snprintf(x9_render_error,sizeof(x9_render_error),"native reply size %zu != %zu",actual,output_bytes);errno=EPROTO;return -1;}
 return 0;
}
void *x9_render_map_buffer(struct X9RenderConnection *connection,uint32_t memory_type,uint64_t *bytes) {
 if(!connection||!bytes){errno=EINVAL;return NULL;}
 mach_vm_address_t address=0;mach_vm_size_t size=0;
 kern_return_t result=IOConnectMapMemory64(connection->port,memory_type,mach_task_self(),&address,&size,kIOMapAnywhere);
 if(result!=KERN_SUCCESS){failure("map native resource",result);return NULL;}
 if(!address||!size||size>connection->caps.max_buffer_bytes||((address|size)&16383)) {
  IOConnectUnmapMemory64(connection->port,memory_type,mach_task_self(),address);
  snprintf(x9_render_error,sizeof(x9_render_error),"invalid native resource mapping");errno=EPROTO;return NULL;
 }
 *bytes=size;return (void *)(uintptr_t)address;
}
int x9_render_unmap_buffer(struct X9RenderConnection *connection,uint32_t memory_type,void *address) {
 if(!connection||!address){errno=EINVAL;return -1;}
 kern_return_t result=IOConnectUnmapMemory64(connection->port,memory_type,mach_task_self(),(mach_vm_address_t)(uintptr_t)address);
 return result==KERN_SUCCESS?0:failure("unmap native resource",result);
}
