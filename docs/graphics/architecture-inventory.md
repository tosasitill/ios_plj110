# Architecture Inventory — X9 Graphics Stack

- Audit date: 2026-11-02
- Branch: `feat/G1Ultra`, HEAD `5b3caad` (x9: regenerate kernel allowlist for revision 229)
- Scope: read-only inspection of the current working tree. No source file was modified.
- Method: every statement below comes from current source at this revision, not from the
  target architecture in the plan document.

## Status legend

| Tag | Meaning |
|---|---|
| `VERIFIED` | Function/file read in full and behavior follows from the code path. |
| `SOURCE CONFIRMED` | Specific source location read; behavior confirmed at that location. |
| `BUILD VERIFIED` | Confirmed by a build artifact/command, not re-run in this audit. |
| `DEVICE VERIFIED` | Confirmed on hardware (not done in this audit). |
| `NOT TESTED` | Stated behavior not exercised during this audit. |
| `INFERENCE` | Reasoned from code but not fully traced. |

No item in this document is `DEVICE VERIFIED` or `BUILD VERIFIED` unless explicitly stated.

---

## Module A — Kernel native render service (userspace ABI endpoint)

**Current implementation**
A single IOService exposes 12 selectors over `IOExternalMethodDispatch`
(`X9RenderGetCaps`, `Allocate`, `ImportHost`, `Release`, `Map`, `Unmap`, `Cache`,
`CreateGroup`, `DestroyGroup`, `Submit`, `Wait`, `Timestamp`). Inputs are the
fixed-size structs in `X9RenderABI.h`; there is no shared-memory ring and no
asynchronous queue in the ABI. `Submit` is synchronous from the caller's point of
view (it polls internally and returns a completion record).

**Entry points**
- `SweetX9RenderService::operation()` — `x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp:183`
  - GetCaps `:184`, Allocate/ImportHost `:194`, Release `:277`, Map/Unmap `:288`,
    Cache `:323`, CreateGroup `:331`, DestroyGroup `:345`, Submit `:351`, Wait `:367`,
    Timestamp `:373`.
- `clientMemoryForType()` — `:407` (exposes a client mapping; performs clean before exposure).
- `externalMethod()` — `:435` (validates owner task and foreground privilege, holds the
  context lock for the whole operation).
- Install/publish entry points — `:472` and `:483`.

**Dependencies**
- `X9RenderKernel.h` structs, `X9RenderABI.h` selector/flag constants.
- `X9RenderMemory.*`, `X9RenderPageTable.*`, `X9RenderCachePlan.inc`.
- Engine vtable installed via `sweet_x9_render_install()`; currently `SweetX9RenderEngine.inc`.
- `metal_service_lock` and the CSF resident session.

**Owned state**
Per open context: `lock`, `owner` task, `resources[4096]`, `mappings[4096]`,
`page_table`, `engine_private`, `exposed`, `disabled`, `uncertain`, `allocated_bytes`,
`mapping_count` (`X9RenderKernel.h`). `X9_RENDER_KERNEL_MAPPINGS` is 4096.

**Synchronization**
- One `IOLock` per context (`externalMethod` holds it for the entire selector).
- Global `metal_service_lock` serializes GPU ownership and CSF engine state
  (`SweetX9CSF.cpp:657,660`).
- The ABI is serialized per client context, not per buffer.

**Memory ownership**
- `Allocate`: `IOBufferMemoryDescriptor::inTaskWithOptions` (optionally write-combined),
  zero-filled unless WC (`:251`).
- `ImportHost`: `IOMemoryDescriptor::withAddressRange(..., context->owner)`; panel
  physical addresses are rejected; the descriptor is not owned by the service.
- `Release` frees the resource slot; mappings must already be gone.

**Current limitations** (`SOURCE CONFIRMED`)
1. Reported `max_mappings` is 1024 while the kernel holds 4096
   (`X9PanVKMemory.c:10` comment; `X9_RENDER_KERNEL_MAPPINGS` 4096).
2. Reported `max_client_bytes` is 512 MiB (`X9_RENDER_CLIENT_LIMIT`) while the actual
   budget defaults to 1024 MiB with a 512 MiB floor (`SweetX9RenderService.cpp:129-163`).
3. `Wait` is a lookup in a bounded history, not a blocking wait (`SweetX9RenderEngine.inc:593-597`).
4. Submit is synchronous; there is no per-submission out-of-order completion.
5. No fd / dma-buf import or export at the native layer.

**Refactoring opportunities**
- Decouple the service from the synchronous engine by making completion a separate
  observable object (fence page + event source) rather than a return value.
- Remove the 1024 vs 4096 and 512 MiB vs 1024 MiB reporting mismatch.
- Split "resource registry" from "engine context" so a client can exist without a GPU
  page table.

**Evidence**
`SweetX9RenderService.cpp:183-483`; `X9RenderKernel.h` (`X9_RENDER_KERNEL_MAPPINGS`,
`X9RenderResource`, `X9RenderContext`); `X9RenderABI.h`.

---

## Module B — Render resource registry and cache maintenance

**Current implementation**
`X9RenderMemory.cpp` owns:
- resource flag validation and per-resource cache span bookkeeping
  (`cache_begin/end`, `write_begin/end`, `cpu_exposed`, `initial_cache_clean`);
- the only `dc civac` / `dc cvac` / `dc ivac` emitters (`X9_CACHE_LINES`, `:129-141`);
- imported-alias handling: a kernel mapping is created lazily and the range is
  pre-faulted so cache maintenance touches resident lines (`:87-105`);
- WC BOs return success without maintenance (`:74`).

The 64 KiB budget and preemption-disabled runs are inside `X9_CACHE_LINES`.

**Entry points**
- `x9_render_memory_cache_spans()` — `:56` (span-bounded maintenance).
- `x9_render_memory_cache()` — `:164` (single-span wrapper).
- `x9_render_memory_publish()` / `x9_render_memory_detached()` / `x9_render_memory_publish_client()`
  (exposure gate; `detached()` only after confirmed quiescence).

**Dependencies**
- `X9RenderResource` bookkeeping from the service.
- `x9_gpu_clean()` in `X9GPUCache.h` (physical DC CVAC, 4 KiB runs) for page tables.

**Owned state**
Per-resource cache ranges and exposure flags; a cached `cache_mapping` for imported BOs.

**Synchronization**
Called with the context lock held; `publish` is the point where a client mapping becomes
visible. No global lock of its own.

**Memory ownership**
Does not own memory; only maintains it. Imported memory stays owned by the client task.
The lazily created kernel mapping is retained until resource release.

**Current limitations** (`SOURCE CONFIRMED`)
1. Scope is every reachable CPU-exposed resource of the context for a submission, not the
   submission's declared resources (see Module D).
2. Non-exposed private BOs are cleaned whole for their first GPU use
   (`SweetX9RenderEngine.inc:355-359`), which is O(BO) not O(used range).
3. GPU-side coherency is not programmed (`gpu_side_coherency_programmed=false`,
   `SweetX9CSFResident.inc`), so the opt-in coherent path is refused and CIVAC remains.
4. Maintenance cost is charged to the display-visible latency because the caller holds
   `metal_service_lock` across it.

**Refactoring opportunities**
- Take the exact per-submission resource set from the submit packet (PanVK already knows it).
- Separate "make CPU writes visible" from "invalidate GPU writes" into distinct
  submission phases with independent accounting.
- Move the pre-fault of imported aliases to import time, not per submit.

**Evidence**
`X9RenderMemory.cpp:17,56,74,87,105,129-141,164`; `X9RenderMemory.h`.

---

## Module C — GPU page table (native, kernel-built)

**Current implementation**
A three-level, 16 KiB-table page table built in kernel memory, with bounded sparse
allocation (`table_limit=1152`, ≤18 MiB backing). Physical addresses are 4 KiB-granular
within 16 KiB host allocations (`pa_mask=0x000000fffffff000`). Attributes are
`0x403 | (writable? (1<<6) : (3<<6)) | (cached? 0 : (1<<54))` (`attrs()`, `:80`).

**Entry points**
- `x9_render_vm_map()` / `x9_render_vm_unmap()` (validation, `prune()` on failure).
- `x9_render_vm_publish()` — `:117` (cleans only dirty tables, sets `attached=true`).
- `x9_render_vm_detach()` / `x9_render_vm_destroy()`.

**Dependencies**
`x9_gpu_clean(vm->tables[i].pa, 4096, 1)` for table coherency; `X9RenderResource` physical
resolution.

**Owned state**
`X9RenderPageTable { Table tables[1152]; unsigned count; bool attached; }`.

**Synchronization**
Protected by the context lock; `prune()` only runs while detached.

**Memory ownership**
Kernel-owned table pages; no userspace mapping.

**Current limitations**
1. Table lifetime is tied to the render context; the display engine never consumes it.
2. No shared IOVA namespace with the display/M4U path (see Module G and Q11).
3. 4 KiB PTE granularity under 16 KiB host pages means alias/pre-fault logic must exist.

**Refactoring opportunities**
- Extract a VM/IOVA allocator independent of the GPU context.
- Allow the display controller to consume a published page table (only after hardware
  capability is confirmed; not assumed here).

**Evidence**
`X9RenderPageTable.cpp:9-13,22,50,80,94-104,111,117`.

---

## Module D — Cache plan (`RenderCachePlan`)

**Current implementation**
Builds two span sets per submission: phase 0 from every mapping, phase 1 from every
`X9RenderGPUWrite` mapping. Spans are sorted by (resource, begin), merged only when they
overlap within the same resource, and split per resource into `first[phase][4097]` indexes.
`holes` counts bytes inside the resource's bounding cache range not covered by a mapping;
`overlap` counts double-counted shared bytes. Per-resource execution calls
`x9_render_memory_cache_spans(..., X9RenderClean|X9RenderInvalidate, bytes)`.

**Entry points**
- `render_cache_plan_build()` — `X9RenderCachePlan.inc` (top-level function after the
  sort helpers).
- `render_cache_plan_resource()` — same file (per-resource span execution).

**Dependencies**
`X9RenderContext.resources/mappings`, 256-byte cache atom
(`X9_RENDER_CACHE_ATOM`), resource cache/write bounds.

**Owned state**
`RenderCachePlan { spans[2][4096]; count[2]; first[2][4097]; holes[2]; overlap[2]; }`,
allocated on the submit stack/engine, not persistent.

**Synchronization**
Built and consumed under the context/engine lock, before `flush_gpu`.

**Memory ownership**
No ownership; indexes into existing resource descriptors.

**Current limitations** (`SOURCE CONFIRMED`)
1. Reachability-based: any live mapping contributes, regardless of whether this
   submission touches it.
2. The plan is rebuilt per submit and is not cached across submits.
3. `holes` is partially accounted for reporting, not used to skip maintenance outside the
   per-mapping spans.
4. Correctness depends on resource spans covering all aliases; aliases through a
   different resource are maintained there, not here (`SweetX9RenderEngine.inc:332-334`).

**Refactoring opportunities**
- Feed the plan from the submission's resource list (the Vulkan/PanVK layer has it).
- Reuse a plan across identical submissions.
- Expose `holes/overlap/reach_saved` through a counter interface instead of logs.

**Evidence**
`X9RenderCachePlan.inc` (sort/merge/build/resource); `SweetX9RenderEngine.inc:339-362`.

---

## Module E — CSF engine (queue access, resident session, group control)

**Current implementation**
- `SweetX9CSF.cpp` orchestrates boot (`sweet_x9_render_bootstrap` `:673`), AS install
  (`install_as` `:349`), AS removal (`remove_as` `:363`), MCU stop (`:377`), and the
  full GPU-control flush (`flush_gpu` `:383`: gw `0x30=0x23304`, wait IRQ bit 17, ack).
- `SweetX9CSFResident.inc` parses the firmware, maps SMMU bypass (`0x48600000`) and
  doorbells, allocates per-slot user I/O pages (`0x04400000 + i*0x200000`), and records
  `coherent` / `uncertain` / `gpu_side_coherency_programmed=false`.
- `X9CSFQueueAccess.cpp` provides host-input region tracking, cache-by-range,
  `read32` (invalidates non-host-input lines only), `write32` (requires host input),
  `healthy()` (expects `0xe8000010` plus MMU/IRQ/MCU checks), doorbell and a 100 µs yield.
- `X9CSFRenderQueue.cpp` implements `wait_ack`, `notify` (clean 0x70 bytes of the group
  input page at 4096 granularity), and `x9_csf_group_configure/start/halt`. Halt uses
  SUSPEND to preserve state; TERMINATE stops each stream first.
- `X9CSFInterfaces.cpp` parses CSF 0.3 shared regions and rejects aliases/overlap.

**Entry points**
`x9_csf_group_configure`, `x9_csf_group_start`, `x9_csf_group_halt`,
`x9_csf_group_health`, `x9_csf_queue_*`, `sweet_x9_render_bootstrap`.

**Dependencies**
Firmware payload, resident session, native render context, `metal_service_lock`.

**Owned state**
Resident session (firmware image, MMIO windows, slot pages), per-group control and
producer/consumer indices, health state.

**Synchronization**
All callers hold `metal_service_lock` via owner enter/leave. Display only uses TryLock.

**Memory ownership**
Kernel-owned firmware/shared regions and per-slot I/O pages; no user mapping.

**Current limitations**
1. Health monitoring is polled (`x9_csf_group_health`) rather than interrupt-driven.
2. One resident session, one VM, one group at a time effectively.
3. Any fault sets `uncertain` and retains refs; no reset or retry.

**Refactoring opportunities**
Replace polling with an IRQ-driven completion source; separate group lifecycle from
connection lifecycle.

**Evidence**
`SweetX9CSF.cpp:349,363,377,383,657-683`; `SweetX9CSFResident.inc`;
`X9CSFQueueAccess.cpp`; `X9CSFRenderQueue.cpp`; `X9CSFInterfaces.cpp`.

---

## Module F — Native render submit path

**Current implementation** (`SweetX9RenderEngine.inc:269-591`)
1. Take `RenderOwner` (global lock) and check readiness/mapping count.
2. Build/refresh `RenderCachePlan`.
3. Pre-submit maintenance per resource (WC skip, coherent-exposed skip, never-exposed
   initial clean, plan spans, whole-BO for non-exposed).
4. Build up to 3 stream packets: markers `0x58490101..05`, `CALL` of the PanVK command
   buffer, `WAIT 0xffff`, `SYNC_SET64` fence at `+0x8000`, using the last 4 CS registers.
5. `x9_render_memory_publish()`; per-stream `render_range_sync`; `flush_gpu`; `install_as(AS1)`.
6. Ring the user doorbell via `ml_io_map(0x48090000)` (mapped once).
7. Poll: health checks, periodic page sync, fence value validate, guard byte `0xa5`.
8. `x9_csf_group_halt(SUSPEND)`; `flush_gpu`; `remove_as`; `x9_render_memory_detached`.
9. Post-CIVAC for GPU-write resources; completion reply; timing logs.

Task 6 added userspace baseline telemetry (`NATIVE BRIDGE`): commit-to-completion
P50/P95/P99, submit-call duration, commit→scheduled queue latency and in-flight
depth (`NativeBridgeMetalFrontend.mm:105-121,1162,1238-1250,2045-2053`). The
kernel phase timings and cache byte averages remain the `X9 CACHE`/`X9 GPU`
baseline; no lock, queue or cache behavior changed.

Failure path (`:589`): sets `c->disabled=true`, `resident->uncertain=true`, retains
AS/MCU/DMA/power references, no reset, no retry.

**Entry points**
`render_submit()` `:269`; `render_wait()` `:593`; `render_close()` `:599`.

**Dependencies**
Modules B–E; PanVK submit plan output (`X9RenderStream[]`).

**Owned state**
Reused per-group buffers, producer index, `expected[3]` fence values, completion history
(64 entries).

**Synchronization**
Serialized by `metal_service_lock` for the entire call, including all cache maintenance
and the polling wait.

**Memory ownership**
Does not own client BOs; owns the private stream storage and fence pages.

**Current limitations**
1. Everything is synchronous inside one lock: cache maintenance + submit + wait.
2. `render_wait` only finds completions still in history and never blocks.
3. Partial multi-stream failure poisons the whole context (`uncertain`).
4. No per-submission cache accounting exposed to userspace.

**Refactoring opportunities**
Split prepare / submit / complete; hold the global lock only for register programming and
doorbell; move cache maintenance to the resource layer keyed by the submission set.

**Evidence**
`SweetX9RenderEngine.inc:61,200-222,269-591,593-597`; logs at `X9 RENDER DIAG` /
`X9 RENDER MARK` in the same file; userspace baseline in
`NativeBridgeMetalFrontend.mm:105-121,1162,1238-1250,2045-2053`; plan:
`docs/graphics/submission-baseline.md`.

---

## Module G — Display, framebuffer, surfaces

**Current implementation**
- `AppleCLCD::swap()` `SweetFramebuffer.cpp:1181` only records the latest frame and the
  held surfaces.
- Display cadence in `vsyncFired()` `:950`; GPU ownership is taken with
  `sweet_x9_render_display_try_enter()` `:1045` and never blocks the display.
- Composition is CPU: `present()` `:1676` does direct present first (`:1480`), then
  staged GPU desktop begin (`:1727`), multi-layer X9 composition into a shadow/flip
  buffer, and `x9QueuePresent()` `:1986`.
- `directPresent()` requires `fVig0` and a surface with `scanoutPhys` `:1500`.
  When a flip buffer exists it copies instead of scanning the client surface `:1517-1522`.
- `x9QueuePresent()` `:917` / `x9PresentFired()` `:926` issue an LK GUI submit on a
  100 µs timer with a 50 ms bound and fresh-TE requirement.
- `SweetX9DesktopWorker.inc` runs the staged GPU path on a separate kernel thread with
  its own private 1256×2760 pool (stride 5120 → 5184), max 3 published GPU frames, CPU
  readback `memcmp` verification, and generation matching.
- Task 7 extracted display-buffer validation and the legacy copy primitives into
  `X9DisplayBackend.inc` (`x9_display::Surface`, `Scanout`, `validate`,
  `mapped_addresses`, `row`, `rows`, `scaled`), included at
  `SweetFramebuffer.cpp:1474`. `directPresent()` now uses `validate()` and
  keeps the lazy `kernelAddress()` call behind the same checks; every call site
  keeps its previous name and order.

**Entry points**
`swap`, `present`, `directPresent`, `scanFrom`, `x9QueuePresent`, `x9PresentFired`,
`vsyncFired`, `sweet_x9_desktop_frame_begin/queue/poll`.

**Dependencies**
`SweetSurface` registry (`sweet_surface_lock`, WC kernel mapping for buffer surfaces,
`scanoutPhys`), IOMobileFramebuffer clients, LK GUI submit, `x9_fb_present_seq`.

**Owned state**
`fPending`/`fPendHold`/`fShowHold`/`fRetireHold`, composition buffers, desktop frame pool
and state machine, `x9_fb_present_seq`.

**Synchronization**
- `fLock` guards framebuffer state.
- Lock order is `fLock → GPU owner` and only via TryLock (`SweetFramebuffer.cpp:1041-1056`).
- Display workloop never takes `metal_service_lock` blockingly; provider lock is TryLock.

**Memory ownership**
Framebuffer buffers and composition shadows are kernel-owned; client IOSurfaces are held
by `SweetSurface` refcounts.

**Current limitations** (`SOURCE CONFIRMED`)
1. Panel content is produced by CPU composition; GPU output is staged and verified by CPU
   readback before publication.
2. `directPresent` is not a GPU scanout path: with a flip buffer it copies; the surface
   must already have `scanoutPhys`.
3. Display latency includes waiting for `fLock` and a GPU-owner TryLock, but not the GPU
   submission itself (it defers instead).
4. No shared GPU/display IOVA domain is programmed.

**Refactoring opportunities**
Make the display path consume a published, cache-clean render target directly (requires
hardware confirmation). Separate composition from scanout registration.

**Evidence**
`SweetFramebuffer.cpp:40,194-196,899-1100,1181,1205,1474,1482-1530,1552-1586,1679-1990`;
`X9DisplayBackend.inc` (all);
`SweetSurface.cpp` (`sweet_surface_lock`, `kernelAddress`, `scanoutPhys`);
`SweetX9DesktopWorker.inc` (all); audit: `docs/graphics/display-backend.md`.

---

## Module H — Metal runtime and native bridge

**Current implementation**
`X9GeneralMetalFactory.mm` gates device discovery and opens `native_bridge_metal_open`.
Discovery itself sits behind `bridge/NativeBackendProbe.{h,mm}` since Phase 1: the probe
file is the only part of this module that imports IOKit and the native render transport,
and the only place that knows the installed PanVK loader path. The factory and frontend
import no IOKit and no transport header.
`NativeBridgeMetalFrontend.mm` implements the Objective-C Metal façade over the Rust
bridge and imports IOSurfaces into buffers (`surfaceBuffer()` weak cache, cap 256;
`newTextureWithDescriptor:iosurface:plane:` `:1516`; route statistics log `:1551`).
`NativeMetalObjects.rs` `nmb_buffer_new_no_copy_releasing` `:594` → `no_copy()` `:608`
creates a host-memory buffer. `NativeMetalExtras.rs` `nmb_texture_new_on_surface` `:113`
creates a texture on an IOSurface plane (optional YCbCr).

**Entry points**
Factory open, `nmb_backend_{probe,loader_path,open}` (bridge boundary), `nmb_*` C ABI
exports, Objective-C texture/buffer creation.

**Dependencies**
metal2vulkan runtime, IOSurface, native render transport (`X9RenderTransport.c`, probe
file only).

**Owned state**
Weak IOSurface→buffer cache, per-object wrappers, route counters.

**Synchronization**
Objective-C object locks plus the bridge's own mutexes; no interaction with
`metal_service_lock` except through the ABI calls.

**Memory ownership**
Buffers may be no-copy over client memory; textures may be host images or shadow copies.

**Current limitations**
1. Route selection is policy-driven in metal2vulkan; the frontend cannot force a route.
2. Weak cache eviction can re-import the same surface.
3. No direct IOSurface fd/dma-buf route at the native layer.
4. The probe connection opened for discovery is a second `SweetX9Render` client for the
   lifetime of the probe call; no state is kept from it.

**Refactoring opportunities**
Make route selection observable/overridable per texture for bring-up diagnostics.

**Evidence**
`X9GeneralMetalFactory.mm`; `bridge/NativeBackendProbe.mm`;
`NativeBridgeMetalFrontend.mm:1516,1551`; `NativeMetalObjects.rs:594,608`;
`NativeMetalExtras.rs:113`.

---

## Module I — metal2vulkan

**Current implementation**
Translates Metal to Vulkan. Host-image policy lives in
`crates/metal2vulkan-runtime/src/in_place.rs` (`ShadowCause`, `Refusal`, `in_place()` at
`:245`) and `linear_plan.rs` (`plane_rows_share_bytes` `:458`, `iosurface_gpu_layout`
`:462` with pitch rounded down to 16, `overlap_held` `:493`, `plan_buffer_texture` `:532`,
`plan_iosurface_plane` `:653`).

**Entry points**
Runtime dispatch, image/buffer creation plans, route verdicts.

**Dependencies**
Vulkan loader, PanVK.

**Owned state**
Shadow images, staging buffers, per-object route metadata.

**Synchronization**
Vulkan-level synchronization only; submission goes through PanVK queues.

**Memory ownership**
Owns shadow/staging resources; host images alias client memory when the route permits.

**Current limitations**
Render-target or shader-write usage forces a shadow (`ShadowCause`); many IOSurface
geometries are refused rather than handled.

**Refactoring opportunities** — none required for Task 1; route telemetry is the useful
next step.

**Evidence**
`in_place.rs:245`; `linear_plan.rs:458-653`.

---

## Module J — PanVK v14 and the X9 pan_kmod backend

**Current implementation**
- Mesa PanVK v14 device creation calls `x9_pan_kmod_create_native()`
  (`panvk_vX_device.c:480-600`), `x9_pan_kmod_csif()`, and omits WSI entrypoints under X9.
- Images: `panvk_image.c:705,824,1441` handle X9 unmap/refusal/bind.
- Memory: `panvk_device_memory.c:85-145` maps `VkImportMemoryHostPointerInfoEXT` to
  `x9_pan_kmod_import_host()`; fd import goes to `pan_kmod_bo_import()` (ENOTSUP);
  normal allocation to `pan_kmod_bo_alloc()`.
- Queue: `csf/panvk_vX_gpu_queue.c:1242` `panvk_queue_submit_ioctl` → `x9_panvk_group_submit`
  `:1277`; `gpu_queue_submit` `:1406` hoists waits before resources; status check
  `:1549` → `x9_pan_kmod_group_check` `:1571`.
- Backend (`x9/metal/General/panvk/`):
  - `X9PanVKMemory.c`: ops table `:256`; device create `:261`; group create `:289`;
    submit `:318` (requires `completed_id == submission_id`, else EPROTO + failed);
    `unmap_cpu` `:342`; caps `:352`; csif `:359`; group check `:364` (Wait on last
    submission); `import_host` `:384` (one read-only retry on EACCES); `bind_vm` `:205`
    (IMMEDIATE only, split unmap refused, partial failure poisons); `wait_bo` `:165`
    (waits last submission only); `flush_maps` `:141` (same-BO/same-op merge only).
  - `X9PanVKSubmitPlan.c`: builds CALL streams (24 B/segment, MOVE48/MOVE32/CALL in the
    last 4 registers), wait hoisting only when every active queue shares the dependency.
  - `X9PanVKSync.c`: `util_sync_provider`, fence DAG, timeline; fd import/export ENOTSUP;
    `submit_sync_common` `:256` → `x9_pan_kmod_submit`; `x9_pan_kmod_create_native` `:307`.
  - `X9PanVKQueue.c` (32 lines): CALL buffer lifecycle/quarantine.
  - `X9PanVKTilerHeap.c` (91 lines): 16 KiB context + chunk arena.
  - `X9PanVKQueueBridge.inc`: `x9_panvk_call_allocate/clean/release/quarantine`,
    `x9_panvk_group_submit` passes `caps.cs_registers` and
    `MAX2(caps.cs_unpreserved_registers,4)`.

**Entry points**
Vulkan `vkCreateDevice`, `vkAllocateMemory`, `vkQueueSubmit`, `vkWaitForFences`,
`vkMapMemory` → X9 pan_kmod ops.

**Dependencies**
Native render ABI, CSF engine, ioctl-free direct function calls (same binary).

**Owned state**
One connection, one VM, per-group call buffers and tiler heap, fence DAG.

**Synchronization**
PanVK queue serialization plus the native context lock and `metal_service_lock`.

**Memory ownership**
Host imports alias client memory; allocations are native BOs; CALL buffers are native
allocations with quarantine.

**Current limitations** (`SOURCE CONFIRMED`)
1. One VM, one group, immediate binds only.
2. No fd/dma-buf import or export; no external sync fd.
3. Submit is synchronous and returns EPROTO if the completion id does not match.
4. BO waits wait for the last submission of the connection, not for the BO.
5. Split unmap of the descriptor ring is refused.

**Refactoring opportunities**
- Track per-BO last-use submissions so waits and cache plans become precise.
- Allow non-immediate binds with a batched submission queue.

**Evidence**
`X9PanVKMemory.c` (all), `X9PanVKSubmitPlan.c`, `X9PanVKSync.c`,
`X9PanVKQueueBridge.inc`, Mesa files listed above. Boundary matrix, ownership
and address-space audit: `docs/graphics/panvk-backend-boundary.md`; enforced by
`x9/build/scripts/check-native-boundary.py` (Task 4).

---

## Module K — Build, packaging and ABI pinning

**Current implementation**
`x9/build/build.py all --revision N` drives the kernel, PanVK, metal2vulkan and payload
build. The kernel imports a generated allowlist
`x9/kernel/generated/x9_render_external_manifest.h` that pins the plugin bundle's
SHA-256, byte size and path. Third-party pins are recorded in `x9/BUILD.md:180-300`
(Mesa `300b5fd7…`, AIR `c84b4634…`, metal2vulkan `219f0136…`) and tree hashes in
`x9/build/local-source-lock.json`. `x9/build/local.json` points at the current root
manifest `.local/x9/x9/out/pan220-batched-cache-ranges/root-manifest.json`.

**Entry points**
`build.py`, allowlist generator, payload packaging.

**Dependencies**
Local source trees under `.local/x9/work/`, XNU object tree.

**Owned state**
Manifest headers, root manifests, source lock.

**Synchronization** — n/a (offline build).

**Memory ownership** — n/a.

**Current limitations** (`SOURCE CONFIRMED`)
1. Rebuilding the plugin requires regenerating the kernel allowlist, otherwise boot
   rejects it.
2. Documented metal2vulkan pin (`219f0136…`) differs from the tree present locally
   (`720ef1ce…` and `metal2vulkan-pan205-source`), a drift risk.
3. Revision numbers in commit history (227–229) track allowlist regeneration, so a
   mismatch between kernel and plugin is a common failure mode.

**Refactoring opportunities**
Generate the allowlist from the build graph automatically and fail early on drift.

**Evidence**
`x9/build/build.py`; `x9/BUILD.md:180-300`; `x9/build/local-source-lock.json`;
`x9/build/local.json`; `x9/kernel/generated/x9_render_external_manifest.h`.

---

## Module L — Buffer Manager (Task 5)

**Current implementation**
Userspace metadata and lifecycle registry inside the Metal bundle
(`x9/metal/General/X9BufferManager.c`), declared by `X9BufferManager.h`. It records,
per native handle key (`NmbBuffer*`/`NmbTexture*`), an `X9BufferDesc`
(`:51`: kind IOSurface/HostLinear/Shadow/GPUOwned/Display, ownership
Client/Imported/Driver/Display, usage bits, format domain, up to three planes)
and an `X9BufferLifecycle` (`:60`: separate GPU and display states, ref counts,
use counters, creation time).
- Registry: mutex-protected dynamic array, `register` `:42`, `unregister` `:59`
  (tombstone while referenced, remove otherwise), `describe` `:124`, `live_count` `:136`.
- GPU references: `gpu_acquire` `:70`, `gpu_release` `:83`.
- Display references: `display_acquire` `:97`, `display_release` `:110`.
- Frontend integration (`NativeBridgeMetalFrontend.mm`): `registerRootBuffer`
  `:192`, `registerBufferView` `:200`; registration at IOSurface buffer `:267`,
  `view:` `:359`, heap buffer/texture `:1388`/`:1401`, device buffers `:1571`/`:1579`/
  `:1588`, device texture `:1596`, planar IOSurface `:1625-1634`; unregistration in
  dealloc `:226`, `:288`; GPU acquire at encode `:1211-1212`, release at finish
  `:1172-1177`.

**Entry points**
Internal C API plus calls from the Metal frontend only. Not part of the X9 native
render ABI, no kernel selector, no IOKit dependency.

**Dependencies**
libpthread, libc monotonic clock, the Metal frontend's `NmbBuffer`/`NmbTexture`
handles. No dependency on XNU VM, CSF, or display structures.

**Owned state**
Registry entries: descriptor copy, lifecycle counters, retirement tombstone.
Ownership of the underlying memory is unchanged — IOSurface owns the surface,
the native service owns BOs, the frontend keeps its existing releases.

**Synchronization**
One internal mutex held only for array lookup and counter updates. All calls are
advisory: a failed registration or reference update cannot refuse a Metal resource.
GPU interval is the encode window between `commit` and `finish`.
Display interval is defined but not driven by any caller yet.

**Memory ownership**
Metadata only. No allocation, mapping, cache, or fence behavior changes; the
registry does not retain Metal objects.

**Current limitations** (`SOURCE CONFIRMED` / `NOT IMPLEMENTED`)
1. Shadow (`X9BufferKindShadow`) and Display (`X9BufferKindDisplay`) kinds exist but
   no registration site exists; metal2vulkan only exposes aggregate shadow counters,
   and scanout state is not visible to the process.
2. Modifier fields are zero; format descriptors are Metal/IOSurface domains only.
3. Metadata is advisory and unobserved outside the frontend; nothing reads
   `describe` yet.
4. Cross-process/display-server surfaces are not registered.

**Refactoring opportunities**
Wire shadow allocation inside metal2vulkan to `registerBufferView`, and let the
display backend publish scanout acquire/release once a display path is owned by
this process.

**Evidence**
`x9/metal/General/X9BufferManager.h`, `x9/metal/General/X9BufferManager.c`,
`NativeBridgeMetalFrontend.mm` lines above, `docs/graphics/buffer-manager.md`,
`docs/graphics/phase-3-report.md`.

---

## Cross-module summary

| Layer | Owns GPU submission? | Owns cache policy? | Owns display? | Owns memory? |
|---|---|---|---|---|
| Metal runtime (H) | no | no | no | wrapper only |
| Buffer Manager (L) | no (tracks refs) | no | no (tracks refs) | metadata only |
| metal2vulkan (I) | via Vulkan | no | no | shadows/staging |
| PanVK + X9 backend (J) | yes (Vulkan) | no | no | BOs |
| Native service (A) | yes (selectors) | yes (B/D) | no | allocations |
| CSF engine (E) | yes (hardware) | no | no | firmware/slots |
| Display (G) | no (TryLock) | no | yes | framebuffer |
| Build (K) | no | no | no | artifacts |

This separation is the factual basis for the six-layer map in `abi-map.md`.
