/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9RenderABI.h"
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct X9RenderConnection;
/* Open requires real native caps. No compute-service or host-renderer fallback. */
struct X9RenderConnection *x9_render_open(struct X9RenderCaps *caps);
void x9_render_close(struct X9RenderConnection *);
int x9_render_call(struct X9RenderConnection *,uint32_t operation,const void *input,size_t input_bytes,void *output,size_t output_bytes);
void *x9_render_map_buffer(struct X9RenderConnection *,uint32_t memory_type,uint64_t *bytes);
int x9_render_unmap_buffer(struct X9RenderConnection *,uint32_t memory_type,void *address);
const char *x9_render_last_error(void);
/* Frontend cache scope (X9RenderScope on a type-1 connection); false when unavailable. */
bool x9_render_scope(uint32_t operation,uint64_t address,uint64_t bytes);
/* Returns0 unless this caller negotiated real synchronous cache ownership. */
uint64_t x9_render_submission_cache_atom(void);
#ifdef __cplusplus
}
#endif
