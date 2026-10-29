/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9PanVKSubmitPlan.h"
/* Allocator and quarantine use the originating queue/device lifetime. Quarantine
 * must retain CPU/GPU backing on uncertain native submission. */
struct X9PanVKQueueBuffers {
 void *cookie;
 int (*allocate)(void *,uint64_t,struct X9PanVKCallBuffer *,void **);
 int (*clean)(void *,void *,uint64_t);
 void (*release)(void *,void *);
 void (*quarantine)(void *,void *);
};
int x9_panvk_queue_execute(struct pan_kmod_dev *,const struct drm_panthor_group_submit *,
 uint32_t,uint32_t,const struct X9PanVKQueueBuffers *,int64_t);
