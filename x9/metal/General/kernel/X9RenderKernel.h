/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9RenderABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
/* Shared only by the native service and CSF engine. Never copied to userspace. */
struct X9RenderResource {
 IOMemoryDescriptor *memory;
 uint64_t bytes;
 uint32_t handle,flags;
 bool prepared;
};
struct X9RenderVMEntry {
 X9RenderResource *resource;
 uint64_t offset,address,bytes;
 uint32_t flags;
};
struct X9RenderPageTable;
struct X9RenderContext {
 IOLock *lock;
 task_t owner;
 X9RenderResource resources[X9_RENDER_MAX_MAPPINGS];
 X9RenderVMEntry mappings[X9_RENDER_MAX_MAPPINGS];
 uint64_t allocated_bytes,next_submission,completed_submission;
 uint32_t next_handle,mapping_count;
 X9RenderPageTable *page_table;
 void *engine_private;
 bool exposed,disabled,engine_open;
};
/* The engine installs this interface only after taking ownership of actual
 * hardware caps and resource/stream/heap support. NULL methods stay refused. */
struct X9RenderEngine {
 bool (*caps)(X9RenderCaps *);
 /* Failed open must unwind privately. close must detach all GPU mappings,
  * groups and page tables before returning true, even when no job ran. */
 IOReturn (*context_open)(X9RenderContext *);
 bool (*context_close)(X9RenderContext *);
 /* Map/unmap success includes hardware translation visibility. On uncertain
  * rollback the engine sets disabled/exposed and retains all backing memory. */
 IOReturn (*map)(X9RenderContext *,const X9RenderVMEntry *);
 IOReturn (*unmap)(X9RenderContext *,const X9RenderVMEntry *);
 IOReturn (*group_create)(X9RenderContext *,const X9RenderGroup *,X9RenderGroup *);
 IOReturn (*group_destroy)(X9RenderContext *,uint32_t);
 IOReturn (*submit)(X9RenderContext *,const X9RenderSubmission *,X9RenderCompletion *);
 IOReturn (*wait)(X9RenderContext *,const X9RenderWaitRequest *,X9RenderCompletion *);
 IOReturn (*cache)(X9RenderContext *,X9RenderResource *,const X9RenderCacheRequest *);
 IOReturn (*timestamp)(uint64_t *);
 bool (*quiesce)(X9RenderContext *);
};
extern "C" void sweet_x9_render_install(const X9RenderEngine *);
extern "C" void sweet_x9_render_publish(void);
