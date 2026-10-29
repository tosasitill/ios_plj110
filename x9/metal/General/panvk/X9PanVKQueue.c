/* SPDX-License-Identifier: MIT */
#include "X9PanVKQueue.h"
#include <errno.h>
#include <string.h>
int x9_panvk_queue_execute(struct pan_kmod_dev *dev,const struct drm_panthor_group_submit *request,
 uint32_t registers,uint32_t reserved,const struct X9PanVKQueueBuffers *ops,int64_t deadline) {
 if(!dev||!ops||!ops->allocate||!ops->clean||!ops->release||!ops->quarantine){errno=EINVAL;return -1;}
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
 for(unsigned i=0;i<3;i++)if(tokens[i]) {
  if(submitted&&ret)ops->quarantine(ops->cookie,tokens[i]);
  else ops->release(ops->cookie,tokens[i]);
 }
 x9_panvk_plan_destroy(plan);errno=saved;return ret;
 }
}
