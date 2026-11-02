/* SPDX-License-Identifier: MIT */
/* One metadata/lifecycle registry for the buffers the Metal runtime imports or
 * allocates. It owns no memory, performs no mapping and does not maintain
 * caches: the kernel render backend remains the allocator and cache owner.
 * Every call is advisory; a metadata miss must never refuse a Metal resource. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define X9_BUFFER_ABI 1u
#define X9_BUFFER_MAX_PLANES 3u

enum X9BufferKind {
 X9BufferKindUnknown=0, X9BufferKindIOSurface=1, X9BufferKindHostLinear=2,
 X9BufferKindShadow=3, X9BufferKindGPUOwned=4, X9BufferKindDisplay=5
};

/* Who is responsible for the memory's lifetime. */
enum X9BufferOwnership {
 X9BufferOwnershipUnknown=0, X9BufferOwnershipClient=1,
 X9BufferOwnershipImported=2, X9BufferOwnershipDriver=3,
 X9BufferOwnershipDisplay=4
};

enum X9BufferUsage {
 X9BufferUsageSampled=1u<<0, X9BufferUsageRenderTarget=1u<<1,
 X9BufferUsageStorage=1u<<2, X9BufferUsageVertex=1u<<3,
 X9BufferUsageIndex=1u<<4, X9BufferUsageIndirect=1u<<5,
 X9BufferUsageDisplay=1u<<6
};

enum X9BufferFormatDomain {
 X9BufferFormatUnknown=0, X9BufferFormatMetal=1, X9BufferFormatIOSurface=2
};

enum X9BufferGpuState {
 X9BufferGpuIdle=0, X9BufferGpuInUse=1, X9BufferGpuComplete=2
};

enum X9BufferDisplayState {
 X9BufferDisplayIdle=0, X9BufferDisplayInUse=1, X9BufferDisplayReleased=2
};

struct X9BufferPlane {
 uint64_t offset, stride, size;
};

struct X9BufferDesc {
 uint32_t abi, kind, ownership, usage;
 uint32_t format_domain, format, plane_count, reserved;
 uint64_t width, height, bytes, surface_id;
 struct X9BufferPlane planes[X9_BUFFER_MAX_PLANES];
 /* Key of the buffer a texture view sits on, 0 for a root buffer. */
 uint64_t parent;
};

struct X9BufferLifecycle {
 uint32_t gpu_state, display_state, retired, reserved;
 uint32_t gpu_refs, display_refs, reserved2[2];
 uint64_t gpu_uses, display_uses, created_ns;
};

/* Register a root buffer or a view. A duplicate key is refused, never merged. */
bool x9_buffer_register(uint64_t key,const struct X9BufferDesc *desc);
/* Drop the metadata. With live consumers the entry is retired and freed by the
 * last release; memory itself is released by its owner, never here. */
void x9_buffer_unregister(uint64_t key);
bool x9_buffer_gpu_acquire(uint64_t key);
bool x9_buffer_gpu_release(uint64_t key);
bool x9_buffer_display_acquire(uint64_t key);
bool x9_buffer_display_release(uint64_t key);
bool x9_buffer_describe(uint64_t key,struct X9BufferDesc *desc,struct X9BufferLifecycle *life);
uint32_t x9_buffer_live_count(void);

#ifdef __cplusplus
}
#endif
