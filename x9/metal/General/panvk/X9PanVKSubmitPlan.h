/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9PanVKSync.h"
#include "drm-uapi/panthor_drm.h"
struct X9PanVKSubmitPlan;
struct X9PanVKCallBuffer {void *cpu;uint64_t gpu,bytes;};
/* Accepts independent external waits, ordered commands on each subqueue and
 * terminal signals. Waits become a group-wide prerequisite; signals complete
 * after every stream. A wait on a same-batch output is rejected. */
struct X9PanVKSubmitPlan *x9_panvk_plan_create(const struct drm_panthor_group_submit *);
void x9_panvk_plan_destroy(struct X9PanVKSubmitPlan *);
uint64_t x9_panvk_plan_stream_bytes(const struct X9PanVKSubmitPlan *,unsigned);
int x9_panvk_plan_emit(const struct X9PanVKSubmitPlan *,uint32_t cs_registers,uint32_t kernel_registers,
 const struct X9PanVKCallBuffer [3],struct X9RenderStream [3],uint32_t *);
int x9_panvk_plan_submit(struct pan_kmod_dev *,const struct X9PanVKSubmitPlan *,
 const struct X9RenderStream *,uint32_t,int64_t,struct X9RenderCompletion *);
