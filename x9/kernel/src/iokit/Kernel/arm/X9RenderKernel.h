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
 /* Monotonic CPU exposure: imports start exposed; returning a descriptor to
  * userspace exposes it forever, even after unmap. Retained contents pointers,
  * VM aliases and IOSurface writers need no dirty notification. Only a native
  * allocation that has NEVER been exposed may skip repeated CPU maintenance.
  * Private command/fence storage has its separate explicit sync path. */
 bool cpu_exposed,initial_cache_clean,imported;
 /* Rebuilt under the context + GPU owner locks from EVERY live GPU mapping,
  * not from a draw's resource list or dirty notifications. Bounding intervals
  * deliberately include holes between mappings and all indirect accesses.
  * Empty means not GPU-addressable in this submission's page table. */
 uint64_t cache_begin,cache_end,write_begin,write_end;
 /* Kernel alias retained only while this prepared import remains alive. */
 IOMemoryMap *cache_mapping;
 /* Native allocation made write-combined (UniXNU Adreno740 systemDescriptor):
  * pageable, no kernel mapping, the memory entry's cache mode applies to the
  * pages and every client view, so no CPU cache ever holds its lines and it
  * needs no CPU maintenance. Imports keep their owner's cacheable attribute. */
 bool write_combined;
 uint64_t created;   /* mach_absolute_time() at allocation/import */
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
 /* Live-resource telemetry window (X9 RENDER LIVE), under the context lock. */
 uint64_t live_report_at,window_alloc_bytes,window_release_bytes;
 uint32_t window_allocs,window_releases;
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
