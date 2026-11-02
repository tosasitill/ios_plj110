# Call Graphs — X9 Graphics Stack

- Audit date: 2026-11-02, HEAD `5b3caad`
- Every step names a real function and file. Where a hop is a library method whose
  implementation was not read, the hop is marked `INFERENCE`.
- Source of truth: current working tree, not the target design.

---

## 1. Metal submission chain

```
App / CoreAnimation
  └─ [MTLCommandBuffer commit]
      NativeBridgeMetalFrontend.mm  (Obj-C façade; forwards to the C ABI)
      └─ nmb_command_buffer_commit()                      x9/metal/General/bridge/NativeMetalObjects.rs:1728
          └─ metal2vulkan CommandBuffer::commit()          .local/.../metal2vulkan-runtime/src/command_buffer/mod.rs:2424
              └─ CommandBuffer::commit_as(...)             same file (private)
                  └─ submit::submit_batch(...)             .../command_buffer/submit.rs:340
                      └─ vkQueueSubmit()                   (Vulkan entry)                      [INFERENCE: loader hop]
                          └─ panvk_per_arch(gpu_queue_submit)()   mesa-render-source/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c:1406
                              └─ panvk_queue_submit_ioctl()           same file:1242
                                  └─ x9_panvk_group_submit()          x9/metal/General/panvk/X9PanVKQueueBridge.inc:52
                                      └─ x9_pan_kmod_submit()         x9/metal/General/panvk/X9PanVKMemory.c:318
                                          └─ x9_render_call(X9RenderSubmit)   x9/metal/General/X9RenderTransport.c
                                              └─ SweetX9RenderService::operation()  SweetX9RenderService.cpp:351
                                                  └─ render_submit()                  SweetX9RenderEngine.inc:269
```

`render_submit` internal order (`SweetX9RenderEngine.inc`):

1. `RenderOwner owner` — takes `metal_service_lock` (`SweetX9CSF.cpp:657`).
2. Readiness + mapping count check.
3. `render_cache_plan_build()` — build both span phases (`X9RenderCachePlan.inc`).
4. Per-resource pre-submit maintenance loop (`:339-362`):
   - WC resource → skip;
   - coherent + `cpu_exposed` → skip;
   - never-exposed + `initial_cache_clean` → skip;
   - `cpu_exposed` → `render_cache_plan_resource()` → `x9_render_memory_cache_spans(..., Clean|Invalidate)`;
   - otherwise → whole-BO `x9_render_memory_cache(..., Clean|Invalidate, r.bytes)`.
5. Build ≤3 stream packets in the group's private storage:
   markers `0x58490101/02/04/05`, `CALL` of the PanVK-provided command stream,
   `WAIT 0xffff`, `SYNC_SET64` fence at `+0x8000`, all through the last four CS registers
   (`x9_cs_move48`, `x9_cs_move32`, `x9_cs_call`, `x9_cs_wait`).
   The command stream itself was built earlier by `X9PanVKSubmitPlan.c` (24 B segments).
6. `x9_render_memory_publish()` — `:396`.
7. `render_range_sync()` for each active stream — `:397`.
8. `flush_gpu()` — `SweetX9CSF.cpp:383` (gw `0x30 = 0x23304`, wait IRQ bit 17, ack).
9. `install_as(gpu, root, 1)` — `SweetX9CSF.cpp:349`.
10. User doorbell: `ml_io_map(0x48090000, 0x1000)` once, then write — `:421`.
11. Poll loop:
    - `x9_csf_group_health()`,
    - periodic `render_page_sync()` / page invalidate,
    - validate fence value equals `submission_id`,
    - guard bytes `0xa5` unchanged.
12. `x9_csf_group_halt(..., suspend=true, ...)` — `:454` → `X9CSFRenderQueue.cpp`.
13. `flush_gpu()`, `remove_as()` (`SweetX9CSF.cpp:363`), `x9_render_memory_detached()`.
14. Post-CIVAC for GPU-write resources.
15. Completion record appended to history; reply returned.

Failure path (`:589`): `c->disabled = true`, `resident->uncertain = true`, retain
AS/MCU/DMA/power refs, no reset, no retry.

Back in the userspace stack:
- `x9_pan_kmod_submit()` (`X9PanVKMemory.c:318`) requires
  `reply.completed_id == submission_id`; otherwise `EPROTO` and `d->failed = true`.
- `X9PanVKSync.c` fence/timeline signal (`submit_sync_common`, `:256`).
- `vkQueueSubmit` returns only after the GPU work is complete (`SOURCE CONFIRMED`:
  the ABI is synchronous and the backend enforces it).

Completion observation:
```
nmb_command_buffer_status / nmb_command_buffer_wait   NativeMetalObjects.rs:1748/1736
  └─ CommandBuffer::status()/wait_until_completed_for()   metal2vulkan-runtime
      └─ vkWaitForFences / vkGetFenceStatus               [INFERENCE: loader hop]
          └─ panvk ... queue status → x9_pan_kmod_group_check()  X9PanVKMemory.c:364
              └─ render_wait()                                 SweetX9RenderEngine.inc:593
                  └─ search 64-entry history; kIOReturnNotFound otherwise
```
`render_wait` never blocks and has no event source; the kernel completion is observed
only through polling during `render_submit` or through the history table.

---

## 2. Texture / IOSurface chain — six distinct routes

Common prefix:

```
MTLTexture / MTLBuffer creation
  NativeBridgeMetalFrontend.mm
    surfaceBuffer()  (weak IOSurface→buffer cache, cap 256)
      NativeMetalObjects.rs nmb_buffer_new_no_copy_releasing :594
        no_copy() :608 → HostMemory::new → new_buffer_with_bytes_no_copy
    OR nmb_texture_new_on_surface()  NativeMetalExtras.rs:113
      → IoSurfacePlane [optional YCbCr]
  metal2vulkan plan:
    linear_plan.rs plane_rows_share_bytes :458
    linear_plan.rs iosurface_gpu_layout :462   (pitch rounded down to 16)
    linear_plan.rs overlap_held :493
    linear_plan.rs plan_buffer_texture :532
    linear_plan.rs plan_iosurface_plane :653
    in_place.rs in_place(policy, usage, class, verdict) :245
```

### Route A — Direct Host (imported host pages, in-place linear image)
1. IOSurface plane becomes a no-copy host buffer (pointer imported later).
2. `plan_iosurface_plane` → `in_place()` verdict with class `PagesClass::Pointer`.
3. Vulkan memory: `VkImportMemoryHostPointerInfoEXT` →
   `panvk_device_memory.c:85-145` → `x9_pan_kmod_import_host()` (`X9PanVKMemory.c:384`)
   → `X9RenderImportHost` (`SweetX9RenderService.cpp:194`, validation `:205`) →
   `IOMemoryDescriptor::withAddressRange(..., context->owner)`.
4. The image is a linear image aliasing those pages; no copy and no GPU-owned storage.
5. Requirements: 16 KiB-aligned host range and size (`:205-206`), native import aligned
   to 16 KiB (`X9PanVKMemory.c:384`), CPU exposure gated by `clientMemoryForType`.
- Status: `SOURCE CONFIRMED`.

### Route B — Host Pitched (explicit row pitch over host pages)
1. Same no-copy buffer import as Route A.
2. `iosurface_gpu_layout` chooses the GPU pitch (rounded down to 16); the plan asks the
   device for an image that lays rows at that pitch.
3. Accept only if the device makes such an image; otherwise `Refusal::Pitch` (or
   `Refusal::Placement`) → Route C.
4. Distinction from Route A: the row pitch is not the default linear pitch, so the
   verdict depends on the driver's image layout, not only on the first-move check.
- Status: `SOURCE CONFIRMED` (decision logic); acceptance depends on device verdict,
  `NOT TESTED` here.

### Route C — Shadow image (device-allocated, copied)
1. `in_place()` returns no in-place decision for any `ShadowCause`:
   `Exact` (policy), `RenderTarget`, `ShaderWrite` (ShaderWrite/ShaderAtomic),
   `CheckPending(class)`, `CheckFailed(class)`, `Unchecked(class)`, or
   `Refused(Refusal, why)` with `Rows|Detached|Image|Pitch|Placement|Source|DmaBuf`.
2. metal2vulkan allocates a shadow image (`shadow.rs`) and copies host content in/out.
3. The shadow's memory is a native allocation:
   `pan_kmod_bo_alloc()` → `X9PanVKMemory.c` allocate → `X9RenderAllocate`.
- Status: `SOURCE CONFIRMED` (causes and refusal list).

### Route D — Owned GPU buffer (no host pages)
1. `MTLBuffer` created without no-copy semantics → `nmb_buffer_new` path in
   `NativeMetalObjects.rs`.
2. `VkBuffer` + normal allocation → `pan_kmod_bo_alloc()` (`panvk_device_memory.c`)
   → `X9PanVKMemory.c` allocate → `X9RenderAllocate` (`SweetX9RenderService.cpp:194`)
   → `IOBufferMemoryDescriptor::inTaskWithOptions` (optionally WC) + zero-fill.
3. CPU visibility only through `vkMapMemory` → `bo_mmap`/`unmap_cpu` →
   `clientMemoryForType` (clean before expose).
- Status: `SOURCE CONFIRMED`.

### Route E — Render Target (attachment usage)
1. Usage contains `RenderTarget` (or is `Unknown`, which implies every usage).
2. `in_place()` returns shadow with `ShadowCause::RenderTarget` unconditionally; the
   texture never aliases host pages as an attachment.
3. If a pass renders into a texture that did not declare `RenderTarget`, the usage-shadow
   and attachment-stand-in paths (`texture/usage_shadow.rs`, `attachment_stand_in.rs`)
   keep the copy limited to texels.
- Status: `SOURCE CONFIRMED`; device behavior `NOT TESTED`.

### Route F — Sampled Texture (sample-only usage)
1. Usage has none of `RenderTarget`, `ShaderWrite`, `ShaderAtomic`.
2. Eligible for in place when policy is `HostPageTexturePolicy::InPlace` and the class
   verdict passed (`PagesClass::Pointer`, `Fresh`, or `DmaBuf`).
3. Before the verdict is known: `ShadowCause::CheckPending` → shadow; a failed or
   unchecked class → shadow.
4. For the X9 ABI, `DmaBuf` class is unreachable because fd import returns `ENOTSUP`
   (`X9PanVKMemory.c:90`); only `Pointer` (and `Fresh`, if the runtime creates fresh
   host pages) apply.
- Status: `SOURCE CONFIRMED` for the decision; `SOURCE CONFIRMED` for the X9 dma-buf
  refusal.

### Route summary table

| Route | Storage owner | Copy? | Decided by |
|---|---|---|---|
| A Direct Host | client host pages | no | `in_place()` + `PagesClass::Pointer` |
| B Host Pitched | client host pages | no | pitch match in `iosurface_gpu_layout` |
| C Shadow | native BO | yes | any `ShadowCause` / `Refusal` |
| D Owned Buffer | native BO | n/a (written by GPU/CPU) | buffer creation path |
| E Render Target | native BO (shadow) | yes | `ShadowCause::RenderTarget` |
| F Sampled Texture | host pages if verdict passed, else BO | conditional | usage + class verdict |

---

## 3. Display chain

```
IOMobileFramebuffer client / CoreAnimation
  └─ AppleCLCD::swap(rec)                      SweetFramebuffer.cpp:1181
      records latest frame, holds surfaces (fPending, fPendHold); no GPU work here
  └─ AppleCLCD::scheduleVsync()                (60/120 Hz timer)
      └─ AppleCLCD::vsyncFired()               SweetFramebuffer.cpp:950
          ├─ fLock
          ├─ if flush && sweet_board_x9():
          │    displayOwner = sweet_x9_render_display_try_enter()   :1045
          │      └─ SweetX9CSF.cpp:664  (TryLock metal_service_lock;
          │         refuses when resident uncertain or GPU busy)
          │    if fX9SubmitPending || x9_lk_gui_busy()==1 → defer (counters)
          ├─ take pending record + surface holds; unlock fLock
          ├─ console sync / flipBack if console dirty
          └─ AppleCLCD::present(rec)           SweetFramebuffer.cpp:1676
              ├─ AppleCLCD::directPresent(rec) :1480
              │    requires fVig0 != NULL and surface->scanoutPhys != 0
              │    one layer only
              │    if fFlip[0]: flipFrom(copy)          :1517
              │    else: dc clean + scanFrom(pa, stride) :1205 → VIG0/CTL0
              ├─ staged GPU desktop begin (X9 only):
              │    sweet_x9_desktop_frame_begin()      SweetX9DesktopWorker.inc
              │      copies panel rows into private input buffer (stride 5120)
              ├─ multi-layer X9 composition into shadow/flip buffer (:1755-1945)
              ├─ sweet_x9_desktop_frame_queue()        SweetX9DesktopWorker.inc
              │    └─ desktop_frame_spawn() → kernel_thread_start(desktop_frame_worker)
              │        └─ desktop_frame_worker()        SweetX9DesktopWorker.inc
              │            ├─ IOLockLock(metal_service_lock)
              │            ├─ sweet_x9_gpu_desktop(&job)  bsd/dev/sweet_x9_gpu_dependencies.c:366
              │            │    └─ gpu_owned_request(job, 1) → native Metal submit
              │            └─ IOLockUnlock(metal_service_lock)
              └─ AppleCLCD::x9QueuePresent()       SweetFramebuffer.cpp:917
                   └─ x9PresentFired()             :926
                       └─ x9_lk_gui_present()      pe_serial.c:734 → x9_lk_gui_step()
                           (fresh TE only; 100 µs timer, 50 ms bound)
  └─ AppleCLCD::scheduleVsync() again
```

Display-side result collection (same `vsyncFired` invocation, after `present`):

```
sweet_x9_desktop_frame_poll(panel, stride, w, h, x9_fb_present_seq, can_publish)
  SweetX9DesktopWorker.inc
  ├─ requires boot-arg x9_metal_system == 2
  ├─ TryLock metal_service_lock (observe only) and TryLock desktop_frame_lock
  ├─ state FrameReady or FrameCPUReady, generation match
  ├─ memcpy GPU/CPU result into panel rows
  ├─ per-row memcmp verification against source
  └─ on success: panel copy accepted, then AppleCLCD::x9QueuePresent()
```

Key ordering and coupling facts (`SOURCE CONFIRMED`):
- `fLock → GPU owner` is TryLock only; the comment at `SweetFramebuffer.cpp:1041-1046`
  records the lock-order rule.
- The display workloop never waits on GPU completion; it skips the tick
  (`fCadenceGpuBusy++`) or observes `FrameCPUReady` instead.
- The GPU worker holds `metal_service_lock` for the whole `sweet_x9_gpu_desktop` call
  (`SweetX9DesktopWorker.inc:45-49`), which includes the synchronous native submit.
- Publication requires a generation match and a CPU `memcmp` of the panel rows; max 3
  GPU-presented frames and 8 attempts per boot.

### Cache/synchronization points inside the display chain
| Point | Function | Maintenance |
|---|---|---|
| Surface used for scanout | `SweetSurface::kernelAddress()` | maps WC for buffer surfaces |
| direct present copy | `flipFrom` / `sweet_dcache_clean` (`SweetFramebuffer.cpp:1517-1521`) | clean CPU lines before scan |
| GPU result handoff | `sweet_x9_desktop_frame_poll` | CPU `memcmp` after copy |
| LK GUI submit | `x9_lk_gui_step()` (`x9_lk_console.h:149`) | fresh TE, no PTE cache ops |

---

## 4. Wait / fence chain (explicit)

```
vkWaitForFences / vkGetFenceStatus            (app / metal2vulkan)
  └─ PanVK queue status                      panvk_vX_gpu_queue.c:1549
      └─ x9_pan_kmod_group_check()            X9PanVKMemory.c:364
          └─ X9RenderWait                     SweetX9RenderService.cpp:367
              └─ render_wait()                SweetX9RenderEngine.inc:593
                  └─ linear search of p->history (64 entries)
```

Consequences:
- A fence is only observable after the synchronous `Submit` has returned; `Wait` does
  not add blocking.
- `pan_kmod_bo_wait` (`wait_bo`, `X9PanVKMemory.c:165`) waits on the connection's last
  submission, not on the requested BO; `read_only` is ignored.
- `flush_bo_map_syncs` (`flush_maps`, `:141`) merges only adjacent same-BO, same-operation
  cache requests; there is no cross-submit or cross-BO batching.
