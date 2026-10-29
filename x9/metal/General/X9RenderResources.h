/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9RenderTransport.h"
#ifdef __cplusplus
extern "C" {
#endif
struct X9RenderResources;
struct X9RenderResourceView {
 uint32_t handle,flags;
 uint64_t bytes,gpu_address;
 void *cpu_address;
};
/* One manager owns its connection. Operations serialize internally. Host
 * imports must remain allocated and CPU ownership must obey GPU completion. */
struct X9RenderResources *x9_resources_open(struct X9RenderCaps *);
void x9_resources_close(struct X9RenderResources *);
int x9_resources_allocate(struct X9RenderResources *,uint64_t,uint32_t,struct X9RenderResourceView *);
int x9_resources_import(struct X9RenderResources *,void *,uint64_t,uint32_t,struct X9RenderResourceView *);
int x9_resources_cache(struct X9RenderResources *,uint32_t,uint64_t,uint64_t,uint32_t);
int x9_resources_release(struct X9RenderResources *,uint32_t);
#ifdef __cplusplus
}
#endif
