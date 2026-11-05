/* SPDX-License-Identifier: MIT */
#include "X9PanVKQueue.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
/* Async submission: CALL buffers stay GPU-visible until their submission
 * completes, so they are retired here and released later, on a submitting
 * thread (the PanVK pool is not used from the completion thread). */
struct Retired { struct Retired *next; struct pan_kmod_dev *dev; uint64_t id; struct X9PanVKQueueBuffers ops; void *tokens[3]; };
static pthread_mutex_t retired_lock=PTHREAD_MUTEX_INITIALIZER;
static struct Retired *retired_head,*retired_tail;
static unsigned retired_count;
static void release_completed(struct pan_kmod_dev *dev,bool all_waited) {
 for(;;) {
  pthread_mutex_lock(&retired_lock);
  struct Retired *r=retired_head;
  if(!r||r->dev!=dev||(!all_waited&&r->id>x9_pan_kmod_async_completed(dev))){pthread_mutex_unlock(&retired_lock);return;}
  retired_head=r->next;if(!retired_head)retired_tail=NULL;retired_count--;
  pthread_mutex_unlock(&retired_lock);
  for(unsigned i=0;i<3;i++)if(r->tokens[i])r->ops.release(r->ops.cookie,r->tokens[i]);
  free(r);
 }
}
int x9_panvk_queue_execute(struct pan_kmod_dev *dev,const struct drm_panthor_group_submit *request,
 uint32_t registers,uint32_t reserved,const struct X9PanVKQueueBuffers *ops,int64_t deadline) {
 if(!dev||!ops||!ops->allocate||!ops->clean||!ops->release||!ops->quarantine){errno=EINVAL;return -1;}
 const bool async=x9_pan_kmod_async(dev);
 if(async)release_completed(dev,false);
 struct X9PanVKSubmitPlan *plan=x9_panvk_plan_create(request);if(!plan)return -1;
 struct X9PanVKCallBuffer buffers[3]={{0}};
 struct X9RenderStream streams[3]={{0}};
 void *tokens[3]={NULL,NULL,NULL};uint32_t count=0;bool submitted=false;int ret=-1;
 for(unsigned i=0;i<3;i++) {
  uint64_t bytes=x9_panvk_plan_stream_bytes(plan,i);if(!bytes)continue;
  if(ops->allocate(ops->cookie,bytes,&buffers[i],&tokens[i]))goto done;
  if(!tokens[i]){errno=EPROTO;goto done;}
 }
 if(x9_panvk_plan_emit(plan,registers,reserved,buffers,streams,&count))goto done;
 for(unsigned i=0;i<3;i++)if(tokens[i]&&ops->clean(ops->cookie,tokens[i],x9_panvk_plan_stream_bytes(plan,i)))goto done;
 struct X9RenderCompletion completion={0};
 /* Conservatively consider buffers device-visible from the start of native
  * submission. A failed call may have published commands before timing out. */
 submitted=true;
 ret=x9_panvk_plan_submit(dev,plan,streams,count,deadline,&completion);
done: {
 int saved=errno;
 uint64_t id=async?x9_pan_kmod_last_submission(dev,NULL):0;
 struct Retired *r=(async&&submitted&&!ret)?calloc(1,sizeof(*r)):NULL;
 if(r) {
  r->dev=dev;r->id=id;r->ops=*ops;memcpy(r->tokens,tokens,sizeof(tokens));
  pthread_mutex_lock(&retired_lock);
  if(retired_tail)retired_tail->next=r;else retired_head=r;
  retired_tail=r;retired_count++;
  pthread_mutex_unlock(&retired_lock);
 }else for(unsigned i=0;i<3;i++)if(tokens[i]) {
  /* Async with no record memory: keep the buffers (as quarantine) rather than
   * free memory the GPU may still read. */
  if((submitted&&ret)||(async&&submitted&&!ret))ops->quarantine(ops->cookie,tokens[i]);
  else ops->release(ops->cookie,tokens[i]);
 }
 x9_panvk_plan_destroy(plan);errno=saved;return ret;
 }
}
