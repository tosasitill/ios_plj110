/* SPDX-License-Identifier: MIT */
#include "X9PanVKSync.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#define SYNC_LIMIT 4096u
#define BATCH_LIMIT 256u
#define FENCE_LIMIT 16384u
/* A fence is immutable except pending -> complete. Dependencies reference
 * older fence objects, not mutable sync handles. This is an acyclic graph. */
struct Fence {
 struct Fence *previous,*next,*a,*b;
 uint32_t refs;
 bool pending,done;
 /* Async submission: queued = accepted into the kernel's global FIFO;
  * ordered = everything this fence depends on is done or queued, so work
  * submitted after it executes after it (kernel FIFO order). Used only for a
  * submit's own wait list; host waits still require done. */
 bool queued,ordered;
};
/* One accepted async submission (or sync-only marker) awaiting completion. */
struct Completion {
 struct Completion *next;
 struct pan_kmod_dev *dev;
 uint32_t group,count;
 uint64_t id;
 struct Fence *fences[];
};
struct Point {struct Point *next;struct Fence *fence;uint64_t value;};
struct SyncObject {
 uint32_t handle;
 uint64_t completed,submitted;
 struct Fence *payload;
 struct Point *head,*tail;
};
struct Shared {
 pthread_mutex_t lock;
 pthread_cond_t changed;
 uint32_t refs,next_handle,fence_count,point_count;
 bool failed;
 struct Fence *first,*last;
 struct Fence signaled;
 struct SyncObject objects[SYNC_LIMIT];
 /* Completion thread (async submission only), under lock. */
 pthread_cond_t work;
 pthread_t thread;
 bool thread_started,stop;
 struct Completion *queue_head,*queue_tail;
 uint64_t async_jobs,async_waits_skipped;
};
struct Provider {struct util_sync_provider base;struct Shared *shared;};
static struct util_sync_provider *clone_provider(struct util_sync_provider *);
static int fail(int e){errno=e;return -1;}
static struct Shared *state(struct util_sync_provider *p){return ((struct Provider *)p)->shared;}
static struct SyncObject *lookup(struct Shared *s,uint32_t handle) {
 if(!handle)return NULL;
 for(unsigned i=0;i<SYNC_LIMIT;i++)if(s->objects[i].handle==handle)return &s->objects[i];
 return NULL;
}
static struct Fence *retain(struct Shared *s,struct Fence *f) {
 if(f&&f!=&s->signaled)f->refs++;
 return f;
}
static void drop(struct Shared *s,struct Fence *f){if(f&&f!=&s->signaled)f->refs--;}
static void collect(struct Shared *s) {
 /* New nodes depend only on older nodes. A backward pass collects children
  * without recursion even for long timeline/transfer chains. */
 struct Fence *f=s->last;
 while(f) {
  struct Fence *previous=f->previous;
  if(!f->refs) {
   if(f->previous)f->previous->next=f->next;else s->first=f->next;
   if(f->next)f->next->previous=f->previous;else s->last=f->previous;
   drop(s,f->a);drop(s,f->b);free(f);s->fence_count--;
  }
  f=previous;
 }
}
static struct Fence *new_fence(struct Shared *s,struct Fence *a,struct Fence *b,bool pending) {
 if(a&&a->done)a=NULL;if(b&&b->done)b=NULL;
 if(!pending&&!a&&!b)return &s->signaled;
 if(s->fence_count==FENCE_LIMIT){errno=ENOSPC;return NULL;}
 struct Fence *f=calloc(1,sizeof(*f));if(!f)return NULL;
 f->refs=1;f->a=retain(s,a);f->b=retain(s,b);f->pending=pending;
 f->previous=s->last;if(s->last)s->last->next=f;else s->first=f;
 s->last=f;s->fence_count++;return f;
}
static struct Point *new_point(struct Shared *s,uint64_t value,struct Fence *f) {
 if(s->point_count==FENCE_LIMIT){errno=ENOSPC;return NULL;}
 struct Point *point=calloc(1,sizeof(*point));if(!point)return NULL;
 point->value=value;point->fence=retain(s,f);s->point_count++;return point;
}
static void free_point(struct Shared *s,struct Point *p){drop(s,p->fence);free(p);s->point_count--;}
static void attach(struct Shared *s,struct SyncObject *o,struct Fence *f,struct Point *point) {
 drop(s,o->payload);o->payload=retain(s,f);
 if(point) {
  if(o->tail)o->tail->next=point;else o->head=point;
  o->tail=point;o->submitted=point->value;
 }
}
static void clear_object(struct Shared *s,struct SyncObject *o) {
 drop(s,o->payload);o->payload=NULL;
 while(o->head){struct Point *p=o->head;o->head=p->next;free_point(s,p);}
 o->tail=NULL;o->submitted=o->completed=0;
}
static void progress(struct Shared *s) {
 /* Topological order means one pass is sufficient. Completion is never
  * inferred from time, handles, availability, producer offsets or CPU output. */
 for(struct Fence *f=s->first;f;f=f->next) {
  if(!f->pending&&(!f->a||f->a->done)&&(!f->b||f->b->done))f->done=true;
  f->ordered=f->done||(f->pending&&f->queued)||
   (!f->pending&&(!f->a||f->a->ordered)&&(!f->b||f->b->ordered));
 }
 for(unsigned i=0;i<SYNC_LIMIT;i++) {
  struct SyncObject *o=&s->objects[i];if(!o->handle)continue;
  while(o->head&&o->head->fence->done) {
   struct Point *p=o->head;o->completed=p->value;o->head=p->next;
   if(!o->head)o->tail=NULL;free_point(s,p);
  }
  if(o->payload&&o->payload->done){drop(s,o->payload);o->payload=&s->signaled;}
 }
 collect(s);
}
/* NULL means no fence has been submitted for this value yet. */
static struct Fence *payload(struct Shared *s,struct SyncObject *o,uint64_t value) {
 if(!value)return o->payload;
 if(value<=o->completed)return &s->signaled;
 for(struct Point *p=o->head;p;p=p->next)if(p->value>=value)return p->fence;
 return NULL;
}
static int create(struct util_sync_provider *p,uint32_t flags,uint32_t *handle) {
 if(!handle||(flags&~DRM_SYNCOBJ_CREATE_SIGNALED))return fail(EINVAL);
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);int ret=-1;
 unsigned i=0;while(i<SYNC_LIMIT&&s->objects[i].handle)i++;
 if(s->failed)errno=EIO;
 else if(i==SYNC_LIMIT||s->next_handle==UINT32_MAX)errno=ENOSPC;
 else {
  struct SyncObject *o=&s->objects[i];memset(o,0,sizeof(*o));o->handle=++s->next_handle;
  if(flags&DRM_SYNCOBJ_CREATE_SIGNALED)o->payload=&s->signaled;
  *handle=o->handle;ret=0;
 }
 pthread_mutex_unlock(&s->lock);return ret;
}
static int destroy(struct util_sync_provider *p,uint32_t handle) {
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);struct SyncObject *o=lookup(s,handle);int ret=-1;
 if(!o)errno=EINVAL;
 else {clear_object(s,o);o->handle=0;collect(s);pthread_cond_broadcast(&s->changed);ret=0;}
 pthread_mutex_unlock(&s->lock);return ret;
}
static int change(struct util_sync_provider *p,const uint32_t *handles,const uint64_t *values,uint32_t count,bool reset) {
 if(count>BATCH_LIMIT||(count&&!handles))return fail(EINVAL);
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);int ret=-1;
 struct Fence *fences[BATCH_LIMIT]={0};struct Point *points[BATCH_LIMIT]={0};
 if(s->failed){errno=EIO;goto done;}
 progress(s);
 for(unsigned i=0;i<count;i++) {
  struct SyncObject *o=lookup(s,handles[i]);if(!o){errno=EINVAL;goto done;}
  for(unsigned j=0;j<i;j++)if(handles[j]==handles[i]){errno=EINVAL;goto done;}
  if(values&&(!values[i]||values[i]<=o->submitted)){errno=EINVAL;goto done;}
  if(values) {
   fences[i]=new_fence(s,o->payload,NULL,false);if(!fences[i])goto done;
   points[i]=new_point(s,values[i],fences[i]);if(!points[i])goto done;
  }
 }
 for(unsigned i=0;i<count;i++) {
  struct SyncObject *o=lookup(s,handles[i]);
  if(values){attach(s,o,fences[i],points[i]);points[i]=NULL;}
  else {clear_object(s,o);if(!reset)o->payload=&s->signaled;}
 }
 progress(s);pthread_cond_broadcast(&s->changed);ret=0;
done: {
 int saved=errno;
 for(unsigned i=0;i<count;i++){if(points[i])free_point(s,points[i]);drop(s,fences[i]);}
 collect(s);pthread_mutex_unlock(&s->lock);errno=saved;return ret;
 }
}
static int reset(struct util_sync_provider *p,const uint32_t *h,uint32_t n){return change(p,h,NULL,n,true);}
static int signal_binary(struct util_sync_provider *p,const uint32_t *h,uint32_t n){return change(p,h,NULL,n,false);}
static int signal_timeline(struct util_sync_provider *p,const uint32_t *h,uint64_t *v,uint32_t n) {
 if(n&&!v)return fail(EINVAL);return change(p,h,v,n,false);
}
static int wait_common_ex(struct util_sync_provider *p,uint32_t *handles,uint64_t *values,unsigned count,int64_t deadline,unsigned flags,uint32_t *first,bool ordered_ok);
static int wait_common(struct util_sync_provider *p,uint32_t *handles,uint64_t *values,unsigned count,int64_t deadline,unsigned flags,uint32_t *first) {
 return wait_common_ex(p,handles,values,count,deadline,flags,first,false);
}
static int wait_common_ex(struct util_sync_provider *p,uint32_t *handles,uint64_t *values,unsigned count,int64_t deadline,unsigned flags,uint32_t *first,bool ordered_ok) {
 const unsigned supported=DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL|DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT|DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
 if(count>BATCH_LIMIT||(count&&!handles)||(flags&~supported)||deadline<0)return fail(EINVAL);
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);int ret=-1;
 struct Fence *captured[BATCH_LIMIT]={0};
 for(;;) {
  if(s->failed){errno=EIO;break;}
  progress(s);unsigned ready=0,first_ready=0;bool missing=false;
  for(unsigned i=0;i<count;i++) {
   if(!captured[i]) {
    struct SyncObject *o=lookup(s,handles[i]);if(!o){errno=EINVAL;goto done;}
    captured[i]=retain(s,payload(s,o,values?values[i]:0));
   }
   if(!captured[i])missing=true;
   else if((flags&DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)||captured[i]->done||(ordered_ok&&captured[i]->ordered)){if(!ready)first_ready=i;ready++;}
  }
  if(!count||((flags&DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)?ready==count:ready!=0)) {
   if(first)*first=first_ready;ret=0;break;
  }
  if(missing&&!(flags&DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)){errno=EINVAL;break;}
  struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))break;
  uint64_t ns=(uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
  if(ns>=(uint64_t)deadline){errno=ETIME;break;}
  uint64_t remaining=(uint64_t)deadline-ns;
  struct timespec relative={(time_t)(remaining/1000000000ULL),(long)(remaining%1000000000ULL)};
  int err=deadline==INT64_MAX?pthread_cond_wait(&s->changed,&s->lock):pthread_cond_timedwait_relative_np(&s->changed,&s->lock,&relative);
  if(err&&err!=ETIMEDOUT){errno=err;break;}
 }
done: {
 int saved=errno;for(unsigned i=0;i<count;i++)drop(s,captured[i]);collect(s);
 pthread_mutex_unlock(&s->lock);errno=saved;return ret;
 }
}
static int wait_binary(struct util_sync_provider *p,uint32_t *h,unsigned n,int64_t t,unsigned f,uint32_t *i){return wait_common(p,h,NULL,n,t,f,i);}
static int wait_timeline(struct util_sync_provider *p,uint32_t *h,uint64_t *v,unsigned n,int64_t t,unsigned f,uint32_t *i) {
 if(n&&!v)return fail(EINVAL);return wait_common(p,h,v,n,t,f,i);
}
static int query(struct util_sync_provider *p,uint32_t *h,uint64_t *v,uint32_t n,uint32_t flags) {
 if(n>BATCH_LIMIT||(n&&(!h||!v))||(flags&~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED))return fail(EINVAL);
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);int ret=-1;
 if(s->failed){errno=EIO;goto done;}progress(s);
 for(unsigned i=0;i<n;i++)if(!lookup(s,h[i])){errno=EINVAL;goto done;}
 for(unsigned i=0;i<n;i++){struct SyncObject *o=lookup(s,h[i]);v[i]=flags?o->submitted:o->completed;}
 ret=0;
done:pthread_mutex_unlock(&s->lock);return ret;
}
static int transfer(struct util_sync_provider *p,uint32_t dh,uint64_t dp,uint32_t sh,uint64_t sp,uint32_t flags) {
 if(flags)return fail(EINVAL);
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);int ret=-1;
 struct Fence *fence=NULL;struct Point *point=NULL;
 struct SyncObject *dst=lookup(s,dh),*src=lookup(s,sh);
 if(s->failed){errno=EIO;goto done;}
 if(!dst||!src){errno=EINVAL;goto done;}progress(s);
 struct Fence *source=payload(s,src,sp);
 if(!source){errno=EINVAL;goto done;}
 if(dp&&dp<=dst->submitted){errno=EINVAL;goto done;}
 fence=new_fence(s,dp?dst->payload:NULL,source,false);if(!fence)goto done;
 if(dp){point=new_point(s,dp,fence);if(!point)goto done;}
 else clear_object(s,dst);
 attach(s,dst,fence,point);point=NULL;progress(s);
 pthread_cond_broadcast(&s->changed);ret=0;
done: {
 int saved=errno;if(point)free_point(s,point);drop(s,fence);collect(s);
 pthread_mutex_unlock(&s->lock);errno=saved;return ret;
 }
}
static int to_fd(struct util_sync_provider *p,uint32_t h,int *fd){(void)p;(void)h;if(fd)*fd=-1;return fail(ENOTSUP);}
static int from_fd(struct util_sync_provider *p,int fd,uint32_t *h){(void)p;(void)fd;if(h)*h=0;return fail(ENOTSUP);}
static int import_file(struct util_sync_provider *p,uint32_t h,int fd){(void)p;(void)h;(void)fd;return fail(ENOTSUP);}
static void finalize(struct util_sync_provider *p) {
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);bool last=!--s->refs;
 bool join=false;
 if(last&&s->thread_started){s->stop=true;join=true;pthread_cond_broadcast(&s->work);}
 pthread_mutex_unlock(&s->lock);
 if(join)pthread_join(s->thread,NULL);   /* drains every queued completion first */
 if(last){
  pthread_mutex_lock(&s->lock);
  for(unsigned i=0;i<SYNC_LIMIT;i++)clear_object(s,&s->objects[i]);collect(s);
  pthread_mutex_unlock(&s->lock);
 }
 if(last){pthread_cond_destroy(&s->work);pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->lock);free(s);}free(p);
}
static const struct util_sync_provider interface={
 .create=create,.destroy=destroy,.handle_to_fd=to_fd,.fd_to_handle=from_fd,.import_sync_file=import_file,.export_sync_file=to_fd,
 .wait=wait_binary,.reset=reset,.signal=signal_binary,.timeline_signal=signal_timeline,.timeline_wait=wait_timeline,
 .query=query,.transfer=transfer,.finalize=finalize,.clone=clone_provider,
};
static struct util_sync_provider *clone_provider(struct util_sync_provider *p) {
 struct Provider *copy=calloc(1,sizeof(*copy));if(!copy)return NULL;
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);
 if(s->refs==UINT32_MAX){pthread_mutex_unlock(&s->lock);free(copy);errno=EOVERFLOW;return NULL;}
 s->refs++;pthread_mutex_unlock(&s->lock);copy->base=interface;copy->shared=s;return &copy->base;
}
struct util_sync_provider *x9_sync_provider_create(void) {
 struct Provider *p=calloc(1,sizeof(*p));struct Shared *s=calloc(1,sizeof(*s));
 if(!p||!s){free(p);free(s);return NULL;}
 int err=pthread_mutex_init(&s->lock,NULL);if(err){free(p);free(s);errno=err;return NULL;}
 err=pthread_cond_init(&s->changed,NULL);
 if(err){pthread_mutex_destroy(&s->lock);free(p);free(s);errno=err;return NULL;}
 err=pthread_cond_init(&s->work,NULL);
 if(err){pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->lock);free(p);free(s);errno=err;return NULL;}
 s->refs=1;s->signaled.done=true;p->base=interface;p->shared=s;return &p->base;
}
/* Completion thread: waits for queued kernel submissions IN ORDER (the kernel
 * FIFO completes in order) and only then marks their signal fences done, so
 * every host wait and semaphore keeps meaning "GPU finished, CPU-visible". */
static void *completion_main(void *arg) {
 struct Shared *s=arg;pthread_mutex_lock(&s->lock);
 for(;;) {
  while(!s->queue_head&&!s->stop)pthread_cond_wait(&s->work,&s->lock);
  struct Completion *c=s->queue_head;
  if(!c)break;   /* stop requested and nothing left */
  pthread_mutex_unlock(&s->lock);
  int ret=0;
  if(c->id&&c->id>x9_pan_kmod_async_completed(c->dev)) {
   struct X9RenderCompletion reply;unsigned tries=0;
   /* The kernel itself declares a job uncertain after 10 s; a userspace
    * ETIMEDOUT only means "not yet", so ask again a bounded number of times. */
   do ret=x9_pan_kmod_fence_wait(c->dev,c->group,c->id,10000000000ULL,&reply);
   while(ret&&errno==ETIMEDOUT&&++tries<3);
  }
  pthread_mutex_lock(&s->lock);
  s->queue_head=c->next;if(!s->queue_head)s->queue_tail=NULL;
  if(ret)s->failed=true;
  else for(unsigned i=0;i<c->count;i++)c->fences[i]->pending=false;
  for(unsigned i=0;i<c->count;i++)drop(s,c->fences[i]);
  free(c);
  progress(s);collect(s);pthread_cond_broadcast(&s->changed);
 }
 pthread_mutex_unlock(&s->lock);return NULL;
}
/* Called with s->lock held. Takes over one reference of each fence. */
static int enqueue_completion(struct Shared *s,struct pan_kmod_dev *dev,uint32_t group,uint64_t id,
 struct Fence **fences,uint32_t count) {
 struct Completion *c=calloc(1,sizeof(*c)+count*sizeof(c->fences[0]));if(!c)return -1;
 c->dev=dev;c->group=group;c->id=id;c->count=count;
 if(!s->thread_started) {
  int err=pthread_create(&s->thread,NULL,completion_main,s);
  if(err){free(c);errno=err;return -1;}
  s->thread_started=true;
 }
 for(unsigned i=0;i<count;i++){c->fences[i]=fences[i];fences[i]->queued=true;fences[i]=NULL;}
 if(s->queue_tail)s->queue_tail->next=c;else s->queue_head=c;
 s->queue_tail=c;s->async_jobs++;
 pthread_cond_signal(&s->work);return 0;
}
/* Device teardown: wait until the completion thread has observed every queued
 * submission, so it never uses the transport after it is closed. */
void x9_sync_drain(struct util_sync_provider *p) {
 if(!p||p->clone!=clone_provider)return;
 struct Shared *s=state(p);pthread_mutex_lock(&s->lock);
 while(s->queue_head)pthread_cond_wait(&s->changed,&s->lock);
 pthread_mutex_unlock(&s->lock);
}
static int submit_sync_common(struct pan_kmod_dev *dev,uint32_t group,const struct X9RenderStream *streams,uint32_t stream_count,
 const struct X9SyncPoint *waits,uint32_t wait_count,const struct X9SyncPoint *signals,uint32_t signal_count,
 int64_t deadline,struct X9RenderCompletion *reply,bool sync_only) {
 if(!dev||!dev->sync_ops||dev->sync_ops->clone!=clone_provider||(!sync_only&&(!reply||!streams||!stream_count||stream_count>3))||
    (sync_only&&(streams||stream_count))||wait_count>BATCH_LIMIT||signal_count>BATCH_LIMIT||
    (wait_count&&!waits)||(signal_count&&!signals))return fail(EINVAL);
 uint32_t handles[BATCH_LIMIT];uint64_t values[BATCH_LIMIT];
 const bool async=x9_pan_kmod_async(dev);
 for(unsigned i=0;i<wait_count;i++){handles[i]=waits[i].handle;values[i]=waits[i].point;}
 /* Async: a dependency already accepted into the kernel FIFO is satisfied for
  * ordering (it runs and completes first), so the CPU does not stall here. */
 if(wait_common_ex(dev->sync_ops,handles,values,wait_count,deadline,
  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL|DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,NULL,async))return -1;
 struct Shared *s=state(dev->sync_ops);pthread_mutex_lock(&s->lock);int ret=-1;
 struct Fence *fences[BATCH_LIMIT]={0};struct Point *points[BATCH_LIMIT]={0};
 if(s->failed){errno=EIO;goto rejected;}
 progress(s);
 /* Allocate every output before mutating the objects: reservation failure
  * cannot publish a partial batch of pending semaphore payloads. */
 for(unsigned i=0;i<signal_count;i++) {
  struct SyncObject *o=lookup(s,signals[i].handle);if(!o){errno=EINVAL;goto rejected;}
  for(unsigned j=0;j<i;j++)if(signals[j].handle==signals[i].handle){errno=EINVAL;goto rejected;}
  if(signals[i].point&&signals[i].point<=o->submitted){errno=EINVAL;goto rejected;}
  if(!signals[i].point&&o->payload&&!o->payload->done){errno=EBUSY;goto rejected;}
  fences[i]=new_fence(s,signals[i].point?o->payload:NULL,NULL,true);if(!fences[i])goto rejected;
  if(signals[i].point){points[i]=new_point(s,signals[i].point,fences[i]);if(!points[i])goto rejected;}
 }
 for(unsigned i=0;i<signal_count;i++){attach(s,lookup(s,signals[i].handle),fences[i],points[i]);points[i]=NULL;}
 pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
 if(async) {
  /* Enqueue (or, sync-only, depend on the newest enqueued id) and hand the
   * pending signal fences to the completion thread. */
  uint64_t id=0;uint32_t id_group=group;
  if(sync_only)id=x9_pan_kmod_last_submission(dev,&id_group);
  else {
   ret=x9_pan_kmod_submit_async(dev,group,streams,stream_count,&id);
   if(!ret&&reply){memset(reply,0,sizeof(*reply));reply->abi=X9_RENDER_ABI;reply->group=group;reply->submission_id=id;}
  }
  int saved=errno;pthread_mutex_lock(&s->lock);
  if(ret)s->failed=true;
  if(s->failed){if(!ret)saved=EIO;ret=-1;}
  else if(sync_only&&(!id||id<=x9_pan_kmod_async_completed(dev))) {
   for(unsigned i=0;i<signal_count;i++)fences[i]->pending=false;s->async_waits_skipped++;
  }else if(signal_count||!sync_only) {
   if(enqueue_completion(s,dev,id_group,id,fences,signal_count)){saved=errno;s->failed=true;ret=-1;}
  }
  progress(s);
  for(unsigned i=0;i<signal_count;i++)drop(s,fences[i]);   /* NULL once handed over */
  collect(s);pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);errno=saved;return ret;
 }
 ret=sync_only?x9_pan_kmod_group_check(dev,group):x9_pan_kmod_submit(dev,group,streams,stream_count,reply);
 int saved=errno;pthread_mutex_lock(&s->lock);
 if(ret)s->failed=true;
 if(s->failed){if(!ret)saved=EIO;ret=-1;}
 else for(unsigned i=0;i<signal_count;i++)fences[i]->pending=false;
 progress(s);
 for(unsigned i=0;i<signal_count;i++)drop(s,fences[i]);collect(s);
 pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);errno=saved;return ret;
rejected: {
 int saved=errno;
 for(unsigned i=0;i<signal_count;i++){if(points[i])free_point(s,points[i]);drop(s,fences[i]);}
 collect(s);pthread_mutex_unlock(&s->lock);errno=saved;return -1;
 }
}
int x9_pan_kmod_submit_sync(struct pan_kmod_dev *dev,uint32_t group,const struct X9RenderStream *streams,uint32_t stream_count,
 const struct X9SyncPoint *waits,uint32_t wait_count,const struct X9SyncPoint *signals,uint32_t signal_count,
 int64_t deadline,struct X9RenderCompletion *reply) {
 return submit_sync_common(dev,group,streams,stream_count,waits,wait_count,signals,signal_count,deadline,reply,false);
}
int x9_pan_kmod_sync_only(struct pan_kmod_dev *dev,uint32_t group,const struct X9SyncPoint *waits,uint32_t wait_count,
 const struct X9SyncPoint *signals,uint32_t signal_count,int64_t deadline) {
 /* The native queue is synchronous. Checking this group after resolving its
  * waits verifies prior work has completed; no GPU completion record is made. */
 return submit_sync_common(dev,group,NULL,0,waits,wait_count,signals,signal_count,deadline,NULL,true);
}
struct pan_kmod_dev *x9_pan_kmod_create_native(const struct pan_kmod_allocator *allocator) {
 struct util_sync_provider *sync=x9_sync_provider_create();if(!sync)return NULL;
 struct pan_kmod_dev *dev=x9_pan_kmod_create(allocator,sync);
 if(!dev){int error=errno;sync->finalize(sync);errno=error;}return dev;
}
