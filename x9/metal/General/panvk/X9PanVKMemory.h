/* SPDX-License-Identifier: MIT */
#pragma once
#include "kmod/pan_kmod.h"
#include "../X9RenderABI.h"
/* Explicit native constructor. Takes ownership of sync_ops only on success.
 * Not registered with DRM enumeration: the queue/timeline port must provide
 * its real sync provider before enabling this backend in device discovery. */
struct pan_kmod_dev *x9_pan_kmod_create(const struct pan_kmod_allocator *,struct util_sync_provider *);
/* Synchronous native queue entry. Callers must resolve dependency waits before
 * entry and signal their timeline only after a validated completion. */
int x9_pan_kmod_group_create(struct pan_kmod_dev *,const struct X9RenderGroup *,struct X9RenderGroup *);
int x9_pan_kmod_group_destroy(struct pan_kmod_dev *,uint32_t);
int x9_pan_kmod_submit(struct pan_kmod_dev *,uint32_t,const struct X9RenderStream *,uint32_t,struct X9RenderCompletion *);

int x9_pan_kmod_unmap_cpu(struct pan_kmod_bo *,void *);

int x9_pan_kmod_caps(struct pan_kmod_dev *,struct X9RenderCaps *);

const struct drm_panthor_csif_info *x9_pan_kmod_csif(struct pan_kmod_dev *);

int x9_pan_kmod_group_check(struct pan_kmod_dev *,uint32_t);

int x9_pan_kmod_sync_cpu(struct pan_kmod_bo *,uint64_t,uint64_t,enum pan_kmod_bo_sync_type);

struct pan_kmod_bo *x9_pan_kmod_import_host(struct pan_kmod_dev *,struct pan_kmod_vm *,void *,uint64_t);
