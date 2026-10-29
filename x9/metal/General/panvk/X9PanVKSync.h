/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9PanVKMemory.h"
struct util_sync_provider *x9_sync_provider_create(void);
struct pan_kmod_dev *x9_pan_kmod_create_native(const struct pan_kmod_allocator *);
struct X9SyncPoint { uint32_t handle; uint64_t point; }; /* point0 = binary */
/* Waits use an absolute CLOCK_MONOTONIC deadline, matching Mesa syncobj ABI.
 * Dependencies finish before native submission. Output signals become complete
 * only after x9_pan_kmod_submit validates actual synchronous completion. */
int x9_pan_kmod_submit_sync(struct pan_kmod_dev *,uint32_t,
 const struct X9RenderStream *,uint32_t,const struct X9SyncPoint *,uint32_t,
 const struct X9SyncPoint *,uint32_t,int64_t,struct X9RenderCompletion *);

/* Host synchronization only, after validating prior synchronous group work.
 * Does not issue a GPU stream or fabricate a rendering completion record. */
int x9_pan_kmod_sync_only(struct pan_kmod_dev *,uint32_t,
 const struct X9SyncPoint *,uint32_t,const struct X9SyncPoint *,uint32_t,int64_t);
