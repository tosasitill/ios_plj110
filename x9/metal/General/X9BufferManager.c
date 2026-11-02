/* SPDX-License-Identifier: MIT */
#include "X9BufferManager.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Entry {
 uint64_t key;
 struct X9BufferDesc desc;
 struct X9BufferLifecycle life;
};

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct Entry *entries;
static uint32_t count,capacity;

static uint64_t now_ns(void) {
 struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
 return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}

static struct Entry *find(uint64_t key) {
 for(uint32_t i=0;i<count;i++)if(entries[i].key==key)return &entries[i];
 return NULL;
}

static void remove_at(uint32_t at) {
 entries[at]=entries[--count];
}

static bool grow(void) {
 if(count<capacity)return true;
 uint32_t next=capacity?capacity*2:64;
 if(next<capacity)return false;
 struct Entry *grown=realloc(entries,(size_t)next*sizeof(*grown));
 if(!grown)return false;
 entries=grown;capacity=next;return true;
}

bool x9_buffer_register(uint64_t key,const struct X9BufferDesc *desc) {
 if(!key||!desc||desc->kind==X9BufferKindUnknown||desc->plane_count>X9_BUFFER_MAX_PLANES)return false;
 pthread_mutex_lock(&lock);
 bool ok=false;
 if(!find(key)&&grow()) {
  struct Entry *e=&entries[count++];
  memset(e,0,sizeof(*e));
  e->key=key;e->desc=*desc;e->desc.abi=X9_BUFFER_ABI;
  e->life.gpu_state=X9BufferGpuIdle;
  e->life.display_state=X9BufferDisplayIdle;
  e->life.created_ns=now_ns();
  ok=true;
 }
 pthread_mutex_unlock(&lock);
 return ok;
}

void x9_buffer_unregister(uint64_t key) {
 if(!key)return;
 pthread_mutex_lock(&lock);
 struct Entry *e=find(key);
 if(e) {
  if(e->life.gpu_refs||e->life.display_refs)e->life.retired=1;
  else remove_at((uint32_t)(e-entries));
 }
 pthread_mutex_unlock(&lock);
}

bool x9_buffer_gpu_acquire(uint64_t key) {
 if(!key)return false;
 pthread_mutex_lock(&lock);
 struct Entry *e=find(key);
 bool ok=false;
 if(e&&!e->life.retired&&e->life.gpu_refs!=UINT32_MAX) {
  e->life.gpu_refs++;e->life.gpu_state=X9BufferGpuInUse;e->life.gpu_uses++;
  ok=true;
 }
 pthread_mutex_unlock(&lock);
 return ok;
}

bool x9_buffer_gpu_release(uint64_t key) {
 if(!key)return false;
 pthread_mutex_lock(&lock);
 struct Entry *e=find(key);
 bool ok=false;
 if(e&&e->life.gpu_refs) {
  if(!--e->life.gpu_refs)e->life.gpu_state=X9BufferGpuComplete;
  ok=true;
  if(e->life.retired&&!e->life.display_refs)remove_at((uint32_t)(e-entries));
 }
 pthread_mutex_unlock(&lock);
 return ok;
}

bool x9_buffer_display_acquire(uint64_t key) {
 if(!key)return false;
 pthread_mutex_lock(&lock);
 struct Entry *e=find(key);
 bool ok=false;
 if(e&&!e->life.retired&&e->life.display_refs!=UINT32_MAX) {
  e->life.display_refs++;e->life.display_state=X9BufferDisplayInUse;e->life.display_uses++;
  ok=true;
 }
 pthread_mutex_unlock(&lock);
 return ok;
}

bool x9_buffer_display_release(uint64_t key) {
 if(!key)return false;
 pthread_mutex_lock(&lock);
 struct Entry *e=find(key);
 bool ok=false;
 if(e&&e->life.display_refs) {
  if(!--e->life.display_refs)e->life.display_state=X9BufferDisplayReleased;
  ok=true;
  if(e->life.retired&&!e->life.gpu_refs)remove_at((uint32_t)(e-entries));
 }
 pthread_mutex_unlock(&lock);
 return ok;
}

bool x9_buffer_describe(uint64_t key,struct X9BufferDesc *desc,struct X9BufferLifecycle *life) {
 if(!key||!desc||!life)return false;
 pthread_mutex_lock(&lock);
 struct Entry *e=find(key);
 bool ok=false;
 if(e) {
  *desc=e->desc;*life=e->life;ok=true;
 }
 pthread_mutex_unlock(&lock);
 return ok;
}

uint32_t x9_buffer_live_count(void) {
 pthread_mutex_lock(&lock);
 uint32_t live=0;
 for(uint32_t i=0;i<count;i++)if(!entries[i].life.retired)live++;
 pthread_mutex_unlock(&lock);
 return live;
}
