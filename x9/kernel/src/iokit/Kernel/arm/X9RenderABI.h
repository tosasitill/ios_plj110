/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
/* Native transport, not Linux ioctl numbers. All scalar records have explicit
 * widths. Pointer payloads always refer to the caller's own address space. */
#define X9_RENDER_ABI 1u
#define X9_RENDER_SERVICE "SweetX9Render"
#define X9_RENDER_MAX_STREAMS 3u
#define X9_RENDER_MAX_MAPPINGS 1024u
#define X9_RENDER_MIN_VA 0x40000000ULL
#define X9_RENDER_MAX_VA 0x400000000ULL /* 16GiB; PanVK data heap starts at 4GiB */
#define X9_RENDER_BUFFER_LIMIT (128ULL<<20)
#define X9_RENDER_CLIENT_LIMIT (512ULL<<20)
enum X9RenderOperation {
 X9RenderGetCaps=0, X9RenderAllocate=1, X9RenderImportHost=2,
 X9RenderRelease=3, X9RenderMap=4, X9RenderUnmap=5,
 X9RenderCache=6, X9RenderCreateGroup=7, X9RenderSubmit=8,
 X9RenderWait=9, X9RenderDestroyGroup=10, X9RenderTimestamp=11,
 /* Capability X9RenderAsyncSubmit only. SubmitAsync: X9RenderSubmission in,
  * X9RenderCompletion out (accepted: status 0, completed_id 0); the submission
  * runs later on the kernel worker in global FIFO order. FenceWait:
  * X9RenderWaitRequest in (timeout_ns 0 = poll, max 10 s), X9RenderCompletion
  * out once that submission completed (CPU-visible), kIOReturnTimeout before. */
 X9RenderSubmitAsync=12, X9RenderFenceWait=13,
 /* Frontend cache scope (rev252bx). Only on a type-1 scope connection (no GPU
  * context): X9RenderScopeRequest in, nothing out. Register/Unregister mark an
  * imported host range the frontend tracks; Acquire/Release count command
  * buffers that reference it (commit .. completion); HoldAll/ReleaseAll pin
  * every range of the task (a command buffer whose surfaces are not all known).
  * With boot-arg x9_cache_scope=1, submission maintenance skips an imported BO
  * only when a registered range of the same task covers it, no overlapping
  * range is acquired and nothing is held. Anything else keeps full maintenance. */
 X9RenderScope=14
};
enum X9RenderScopeOperation { X9ScopeRegister=1u, X9ScopeUnregister=2u, X9ScopeAcquire=3u, X9ScopeRelease=4u,
 X9ScopeHoldAll=5u, X9ScopeReleaseAll=6u };
#define X9_RENDER_SCOPE_CLIENT 1u
enum X9RenderBufferFlags { X9RenderGPURead=1u, X9RenderGPUWrite=2u, X9RenderGPUExecute=4u };
enum X9RenderCacheOperation { X9RenderClean=1u, X9RenderInvalidate=2u };
enum X9RenderCapabilities { X9RenderBufferObjects=1u, X9RenderHostImport=2u, X9RenderCSFSubmit=4u, X9RenderTiler=8u, X9RenderFragment=16u, X9RenderSubmissionCacheOwnership=32u, X9RenderAsyncSubmit=64u };
#define X9_RENDER_CACHE_ATOM 256u
struct X9RenderCaps {
 uint32_t abi,bytes,capabilities,host_page_bytes;
 uint64_t gpu_id,gpu_features,shader_present,timestamp_frequency;
 uint32_t core_features,l2_features,tiler_features,mem_features,mmu_features;
 uint32_t max_threads,max_workgroup_threads,max_barrier_threads,thread_features,tls_alloc;
 uint32_t texture_features[4],afbc_features,cs_registers,cs_unpreserved_registers;
 uint32_t max_streams,max_mappings,reserved;
 uint64_t va_start,va_end,max_buffer_bytes,max_client_bytes;
};
struct X9RenderAllocation {
 uint32_t abi,flags;
 uint64_t bytes,host_address;
};
struct X9RenderBuffer {
 uint32_t abi,handle,memory_type,flags;
 uint64_t bytes;
};
struct X9RenderHandle { uint32_t abi,handle; };
struct X9RenderMapping {
 uint32_t abi,handle,flags,reserved;
 uint64_t buffer_offset,gpu_address,bytes;
};
struct X9RenderCacheRequest {
 uint32_t abi,handle,operation,reserved;
 uint64_t offset,bytes;
};
struct X9RenderGroup {
 uint32_t abi,handle,stream_count,priority;
 uint64_t compute_mask,fragment_mask,tiler_mask;
 uint32_t max_compute,max_fragment,max_tiler,reserved;
 uint64_t tiler_heap_bytes;
};
struct X9RenderStream {
 uint32_t index,reserved;
 uint64_t gpu_address,bytes;
};
struct X9RenderSubmission {
 uint32_t abi,group,stream_count,flags;
 uint64_t submission_id;
 struct X9RenderStream streams[X9_RENDER_MAX_STREAMS];
};
struct X9RenderCompletion {
 uint32_t abi,status,group,reserved;
 uint64_t submission_id,completed_id;
 uint32_t fault_status,exception_type;
 uint64_t fault_address;
};
struct X9RenderScopeRequest {
 uint32_t abi,operation;
 uint64_t address,bytes;
};
struct X9RenderWaitRequest {
 uint32_t abi,group;
 uint64_t submission_id,timeout_ns;
};
#if defined(__cplusplus)
static_assert(sizeof(struct X9RenderMapping)==40,"mapping ABI");
static_assert(sizeof(struct X9RenderSubmission)==96,"submission ABI");
static_assert(sizeof(struct X9RenderScopeRequest)==24,"scope ABI");
#endif
