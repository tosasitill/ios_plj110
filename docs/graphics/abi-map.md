# ABI Map — Seven Layers

- Audit date: 2026-11-02, HEAD `5b3caad`
- "ABI" here means any boundary with a stable or de-facto contract: a versioned struct
  set, a vtable, a C symbol set, or a build-time manifest. Internal C++ calls are listed
  only where they are the real contract.
- Status values: `PRESENT` (contract exists and is used), `IMPLICIT` (contract exists but
  is unversioned), `ABSENT` (no boundary; direct calls).

```
+--------------------------------------------------------------+
| 7. Plugin / manifest      x9_render_external_manifest.h       |
+--------------------------------------------------------------+
| 6. Display / scanout      IOMobileFramebuffer + LK GUI        |
+--------------------------------------------------------------+
| 5. Kernel render + CSF    X9RenderABI (12 selectors)          |
+--------------------------------------------------------------+
| 4. Native GPU service     X9RenderTransport / X9RenderResources|
+--------------------------------------------------------------+
| 3. Vulkan driver          PanVK v14 + pan_kmod_ops            |
+--------------------------------------------------------------+
| 2. Translation            metal2vulkan (Metal -> Vulkan)      |
+--------------------------------------------------------------+
| 1. Metal runtime          Metal.framework façade (nmb_* ABI)  |
+--------------------------------------------------------------+
```

---

## Layer 1 — Metal runtime façade

- Boundary: Objective-C Metal classes + C symbols `nmb_*` (`NmbBuffer`, `NmbTexture`,
  `NmbCommandBuffer`, `NmbQueue`, ...).
- Producer: `NativeBridgeMetalFrontend.mm`, `NativeMetalObjects.rs`,
  `NativeMetalExtras.rs`, `NativeMetalBridge.rs`.
- Consumer: applications and system frameworks linked against the Metal shim.
- Contract: `NativeMetalBridge.h`, `NativeMetalObjects.h`, `NativeMetalExtras.h`.
- Versioning: none (`IMPLICIT`); symbols exported with fixed signatures.
- Data crossing: opaque pointers, `MTLTextureDescriptor`/IOSurface planes, bytes and
  strides; no GPU handles.
- Status: `PRESENT`.
- Coupling notes: the façade decides nothing about routes; it forwards to metal2vulkan.
  IOSurface identity is carried here (`nmb_texture_new_on_surface`) and lost after import.

## Layer 2 — Translation (metal2vulkan)

- Boundary: Rust API of `metal2vulkan-runtime` (`CommandBuffer::commit`, texture/buffer
  plans, `HostPageTexturePolicy`).
- Producer/consumer: Layer 1 calls in; Layer 3 (Vulkan) is called out.
- Contract: Rust types plus the semantics documented in `in_place.rs` and
  `linear_plan.rs`; route verdicts (`Verdict`, `ShadowCause`, `Refusal`) are the
  observable policy surface.
- Versioning: crate version / source pin only (`IMPLICIT`).
- Data crossing: `VkImage`/`VkBuffer` handles, host pointers, row pitches, verdicts.
- Status: `PRESENT`.
- Coupling notes: the six texture routes are decided here (`call-graph.md` §2). The
  downstream native ABI cannot express "this is an IOSurface plane".

## Layer 3 — Vulkan driver (PanVK v14 + pan_kmod)

- Boundary (external): Vulkan 1.x entrypoints implemented by PanVK.
- Boundary (internal, the real contract): `struct pan_kmod_ops` in
  `x9/metal/General/panvk/X9PanVKMemory.c:256` and the `x9_pan_kmod_*` / `x9_panvk_*`
  helper symbols.
- Producer: Mesa `panvk_vX_device.c`, `panvk_image.c`, `panvk_device_memory.c`,
  `csf/panvk_vX_gpu_queue.c`.
- Consumer: the X9 backend `.c` files.
- Contract fields: `bo_alloc`, `bo_free`, `bo_import`, `bo_export`, `bo_mmap`,
  `flush_bo_map_syncs`, `bo_wait`, `vm_create`, `vm_destroy`, `vm_bind`, `vm_query_state`,
  `query_timestamp`, `dev_query_user_va_range`, `dev_destroy`.
- Versioning: Mesa source pin `300b5fd7…`; no runtime version negotiation (`IMPLICIT`).
- Data crossing: BO handles (opaque pointers), VM ops, host pointers, `X9RenderCaps`.
- Status: `PRESENT`.
- Coupling notes: any new GPU feature must be expressed in this table; kernel-only changes
  cannot add capability. fd import/export are `ENOTSUP` by design.
- PanVK-side X9 hooks: `x9_pan_kmod_create_native` (`X9PanVKSync.c:307`),
  `x9_pan_kmod_csif` (`X9PanVKMemory.c:359`), `x9_panvk_group_submit`
  (`X9PanVKQueueBridge.inc:52`), `x9_pan_kmod_group_check` (`X9PanVKMemory.c:364`),
  `x9_pan_kmod_import_host` (`:384`).

## Layer 4 — Native GPU service transport

- Boundary: `x9_render_open` / `x9_render_call` / `x9_render_map_buffer` /
  `x9_render_unmap_buffer` / `x9_render_submission_cache_atom` in
  `x9/metal/General/X9RenderTransport.{c,h}`, plus the userspace registry
  `X9RenderResources.{c,h}`.
- Producer: Layer 3 backend.
- Consumer: kernel IOService via `IOExternalMethodDispatch`.
- Contract: exact caps validation in `x9_render_open` (fields must match expected
  layout), `struct X9Render*` request/response copies.
- Versioning: `X9_RENDER_ABI` inside each struct (`PRESENT` for ABI word; no struct size).
- Data crossing: file-scoped connection, 12 selectors, struct copies only. No shared ring.
- Status: `PRESENT`.
- Coupling notes: `X9RenderResources` is a userspace VA allocator that currently has no
  external user besides itself (`grep` found no call sites in the factory or PanVK), so it
  is dead weight or a reserved seam.

## Layer 5 — Kernel render + CSF

- Boundary: `SweetX9RenderService::operation()` selectors and the engine vtable
  `X9RenderEngine` in `X9RenderKernel.h`.
- Producer: Layer 4.
- Consumer: `render_submit` / `render_wait` (`SweetX9RenderEngine.inc`), and beneath it the
  CSF resident/queue layer.
- Contract:
  - Requests: `X9RenderCaps`, `X9RenderAllocation`, `X9RenderBuffer`, `X9RenderMapping`,
    `X9RenderCacheRequest`, `X9RenderGroup`, `X9RenderSubmission`, `X9RenderWaitRequest`.
  - Reply: `X9RenderCompletion` (group, submission_id, completed_id, status,
    fault_status, exception_type, fault_address, reserved).
  - Engine vtable: caps, context_open/close, map/unmap, group_create/destroy, submit,
    wait, cache, timestamp, quiesce.
- Versioning: `X9_RENDER_ABI` in each request (`PRESENT`).
- Data crossing: structs plus GPU VA and fence values. No pointers except host import
  addresses (validated as user task addresses).
- Status: `PRESENT`.
- Coupling notes: `Submit` is synchronous (see `refactor-issues.md` API-1/2), and the
  global `metal_service_lock` is taken inside this layer (`SweetX9CSF.cpp:657`).

## Layer 6 — Display / scanout

- Boundary A: IOMobileFramebuffer client contract implemented by `AppleCLCD`
  (`swap`, `present`, `clientMemoryForType`, `addClient`/`removeClient`).
- Boundary B: the LK GUI submit contract (`x9_lk_gui_present`, `x9_lk_gui_busy`,
  `x9_lk_gui_probe`) implemented over the bootloader's DSI/mutex buffers
  (`pe_serial.c:734-738`; `x9_lk_console.h`).
- Producer: display workloop (`vsyncFired`) and the desktop worker thread.
- Consumer: panel hardware via LK-programmed DSI/mutex.
- Contract: swap record layout (`SWEET_FB_SWAP_REC_SIZE 0x46c`, 4 layers), surface
  registry (`SweetSurface` with `scanoutPhys`, `bytesPerRow`, `offset`), and the fixed LK
  buffer geometry (5120×2760, 1280-wide panel window).
- Versioning: none (`IMPLICIT`); the swap record offsets are hard-coded.
- Status: `PRESENT`.
- Coupling notes: display consumes CPU memory (framebuffer/copy), not GPU VA. There is no
  ABI by which the GPU can hand a render target to the display controller.

## Layer 7 — Plugin / manifest

- Boundary: generated kernel header `x9/kernel/generated/x9_render_external_manifest.h`,
  consumed at boot to validate the userspace plugin bundle (SHA-256, byte size, path).
- Producer: the build (`x9/build/build.py`, allowlist generator).
- Consumer: kernel plugin loader.
- Contract: exact bundle identity; any rebuild of the plugin requires regeneration
  (commits `f03f53d`, `5b3caad` are allowlist regenerations for revisions 228/229).
- Versioning: revision number in the build invocation (`PRESENT`, coarse).
- Status: `PRESENT`.
- Coupling notes: this is the strongest build-time ABI in the stack and the most common
  breakage point when kernel and userspace are built separately.

---

## What crosses each boundary (data-flow view)

| Boundary | Handles | Pointers | Version word | Async channel |
|---|---|---|---|---|
| 1 → 2 | Obj-C objects, IOSurface, bytes/strides | yes | no | command buffers |
| 2 → 3 | VkImage/VkBuffer, host ptr, pitch | yes | no | Vulkan queue |
| 3 → 4 | BO/VM handles, host ptr | yes | `X9_RENDER_ABI` | none (synchronous) |
| 4 → 5 | 12 selectors, structs | host address only | `X9_RENDER_ABI` | none |
| 5 → 6 | none | no | no | none |
| 6 → panel | CPU pixels / LK buffers | physical addresses | no | TE |
| 7 → 5 | bundle hash/size/path | no | revision | none |

## Deliberate non-ABIs

- `X9RenderKernel.h` internal structs (`X9RenderResource`, `X9RenderContext`) are kernel
  private and must not leak.
- `RenderCachePlan`, `RenderOwner`, `ResidentSession` are internal implementation state.
- `sweet_x9_gpu_desktop` / `gpu_owned_request` are kernel-internal C symbols shared
  between the worker and the old compute path; they are not a userspace ABI.

## Gaps that matter for decoupling

1. No async completion ABI at layers 4/5 (`Wait` is a history lookup).
2. No resource-set input at layer 5, so cache policy cannot be scoped per submission.
3. No display-consumable GPU VA at layer 6; display only sees CPU memory.
4. No structured route verdict at layer 2; routes are internal enums and counters.
5. Layer 7 is a hard build pin, so decoupling layers 1–6 still requires a coordinated
   build for any kernel-side change.
