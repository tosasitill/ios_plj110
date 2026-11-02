# Phase 0 Report — Read-Only Architecture Audit

- Date: 2026-11-02
- Branch / HEAD: `feat/G1Ultra` / `5b3caad`
- Task: Task 1 only (read-only audit). No source file was modified.
- Deliverables referenced: `architecture-inventory.md`, `call-graph.md`,
  `refactor-issues.md`, `abi-map.md`.

Tags used: `VERIFIED`, `SOURCE CONFIRMED`, `BUILD VERIFIED`, `DEVICE VERIFIED`,
`NOT TESTED`, `INFERENCE`.

Nothing in this report is `DEVICE VERIFIED`. No build was run during this audit.

---

## 1. Scope

**In scope**
- Current working tree at HEAD `5b3caad`: kernel X9 render/CSF/display sources, Metal
  shim and bridge, metal2vulkan runtime sources present locally, Mesa PanVK v14 sources
  present locally, X9 PanVK backend, build scripts and pins.
- Twelve questions from the task specification (answered in Appendix A).
- Static identification of coupling and decoupling seams.

**Out of scope**
- Any code change, patch generation, re-port, or behavior change (Task 2+).
- Hardware register experiments, GPU submission, cache, HSR, DCD, framebuffer fetch or
  display behavior changes.
- Build execution and device execution.
- Treating the target architecture in the plan document as evidence of current state.

**Method**
Every claim cites a function and file. Where only a decision function was read and the
device verdict is unknown, the item is tagged `SOURCE CONFIRMED` for the decision and
`UNKNOWN`/`NOT TESTED` for the outcome.

## 2. Existing Implementation

Six implemented layers plus a build-time manifest layer (`abi-map.md`):

1. **Metal runtime façade** — Objective-C classes over the `nmb_*` C ABI
   (`NativeBridgeMetalFrontend.mm`, `NativeMetalObjects.rs`, `NativeMetalExtras.rs`).
   `SOURCE CONFIRMED`.
2. **metal2vulkan** — Metal→Vulkan translation with host-image route policy
   (`in_place.rs`, `linear_plan.rs`). `SOURCE CONFIRMED`.
3. **PanVK v14** — Mesa Vulkan driver calling the X9 backend through `struct pan_kmod_ops`
   (`X9PanVKMemory.c:256`). `SOURCE CONFIRMED`.
4. **Native transport/service** — 12-selector ABI over `IOExternalMethodDispatch`
   (`X9RenderTransport.c`, `SweetX9RenderService.cpp:183`). `SOURCE CONFIRMED`.
5. **Kernel render + CSF** — synchronous submit, native page table, cache planning, CSF
   resident/queue control (`SweetX9RenderEngine.inc`, `X9RenderMemory.cpp`,
   `X9RenderPageTable.cpp`, `X9RenderCachePlan.inc`, `SweetX9CSF.cpp`). `SOURCE CONFIRMED`.
6. **Display** — CPU composition into framebuffer/shadows, LK GUI submit with fresh TE,
   optional staged GPU desktop worker with CPU readback verification
   (`SweetFramebuffer.cpp`, `SweetSurface.cpp`, `SweetX9DesktopWorker.inc`,
   `x9_lk_console.h`). `SOURCE CONFIRMED`.
7. **Plugin/manifest** — generated allowlist pinning the userspace bundle
   (`x9_render_external_manifest.h`). `SOURCE CONFIRMED`.

Current submission is synchronous end to end: `vkQueueSubmit` returns only after the GPU
fence value has been observed by the kernel (`X9PanVKMemory.c:318-340`;
`SweetX9RenderEngine.inc:400-455`). `SOURCE CONFIRMED`.

Cache maintenance is per-mapping span maintenance with holes excluded, plus skips for WC,
coherent-exposed (unreachable while `gpu_side_coherency_programmed=false`) and
never-exposed BOs. This supersedes the plan document's premise of an unconditional
whole-range CIVAC of every CPU-exposed BO. `SOURCE CONFIRMED`.

Display is not GPU scanout: the panel is fed by CPU-composed memory or by a copied-back
GPU result. `SOURCE CONFIRMED`.

## 3. Changes

**None.** This phase produced documentation only:

- `docs/graphics/architecture-inventory.md` (new)
- `docs/graphics/call-graph.md` (new)
- `docs/graphics/refactor-issues.md` (new)
- `docs/graphics/abi-map.md` (new)
- `docs/graphics/phase-0-report.md` (this file, new)
- `NOTES.md` handoff updated (per repository rules).

No source, build script, ABI header, manifest or patch was touched. `SOURCE CONFIRMED`
(git status shows only the new documentation paths).

## 4. Architecture

```
Metal.framework shim (Obj-C)                          layer 1
  └─ nmb_* C ABI
metal2vulkan (Rust)                                   layer 2
  └─ Vulkan
PanVK v14 (Mesa)                                      layer 3
  └─ pan_kmod_ops / x9_pan_kmod_* (direct calls)
userspace transport + 12-selector native ABI          layer 4
  └─ IOExternalMethod
kernel render service + native page table             layer 5
  └─ CSF resident / group / doorbell
CSF hardware (CSG/CS, 3 streams, fence page)          hardware
Display: IOMFB workloop + LK GUI + panel              layer 6
Build: allowlist/manifest                             layer 7
```

Key structural facts:
- GPU submission and display are **separate paths** that meet only through CPU memory
  (framebuffer composition and readback) and through `metal_service_lock`. `VERIFIED`.
- There is **no** display-facing GPU VA handoff. `SOURCE CONFIRMED`.
- The native ABI is synchronous and single-context; there is no asynchronous completion
  object. `SOURCE CONFIRMED`.
- Cache policy lives entirely in the kernel and is keyed to the context's mapping set, not
  to the submission. `SOURCE CONFIRMED`.

## 5. ABI Impact

No ABI changed in this phase. Structural observations (`SOURCE CONFIRMED`):

| Boundary | Versioning | Gap |
|---|---|---|
| Metal ↔ metal2vulkan | none | route policy not expressible from above |
| metal2vulkan ↔ PanVK | none | standard Vulkan only |
| PanVK ↔ X9 backend | Mesa source pin | kmod ops table is the real contract |
| transport ↔ kernel | `X9_RENDER_ABI` word | no struct size/version, no resource set |
| kernel ↔ CSF | internal | polled completion, no event source |
| display ↔ panel | hard-coded offsets | no GPU-target registration |
| build ↔ plugin | SHA/size/path | manual regeneration required |

## 6. Memory Impact

No memory behavior changed in this phase. Current state (`SOURCE CONFIRMED`):

- `ImportHost` pins the client's task range for the resource lifetime; panel physical
  ranges are rejected (`SweetX9RenderService.cpp:205-206`).
- Imported resources get a lazily created kernel mapping and a per-cache-call pre-fault
  (`X9RenderMemory.cpp:87-105`).
- Allocations are 16 KiB-sized and limited to 128 MiB per buffer
  (`X9_RENDER_BUFFER_LIMIT`); the client budget defaults to 1024 MiB (floor 512 MiB)
  while caps report 512 MiB (`SweetX9RenderService.cpp:129`).
- GPU VA is a separate 16 GiB range `0x40000000..0x400000000` (`X9PanVKMemory.c:52`;
  `X9RenderPageTable.cpp`), not shared with display/M4U.
- Cache scope for a submit is all reachable mappings; non-exposed BOs are cleaned whole on
  first use (`SweetX9RenderEngine.inc:339-362`).

No measurement of actual cache-maintenance volume was made: `NOT TESTED`.

## 7. Synchronization Impact

No synchronization changed. Current state (`SOURCE CONFIRMED`):

- `render_submit` holds `metal_service_lock` from plan build through the completion poll
  (`SweetX9RenderEngine.inc:269`; owner at `SweetX9CSF.cpp:657`).
- Display takes the same lock only with TryLock and defers on failure
  (`SweetFramebuffer.cpp:1045`; `SweetX9CSF.cpp:664`).
- Lock order is `fLock → GPU owner`, never the reverse (`SweetFramebuffer.cpp:1041-1046`).
- The desktop GPU worker holds `metal_service_lock` for the whole
  `sweet_x9_gpu_desktop` call (`SweetX9DesktopWorker.inc:45-49`).
- Completion is polled; `render_wait` is a bounded history lookup and never blocks
  (`SweetX9RenderEngine.inc:593-597`).

Consequence: GPU submit duration manifests as skipped display ticks, not as blocking
display. This is a design property, not a measured regression: `INFERENCE` for the exact
latency contribution; `SOURCE CONFIRMED` for the mechanism.

## 8. Build Result

**Not run in this phase.** `NOT TESTED`.

Known build facts from the tree (`SOURCE CONFIRMED`):
- Build entry point: `x9/build/build.py all --revision N`.
- Kernel allowlist: `x9/kernel/generated/x9_render_external_manifest.h` pins the plugin
  bundle hash/size/path; revisions 228/229 were allowlist regenerations
  (`f03f53d`, `5b3caad`).
- Pins: Mesa `300b5fd7…`, AIR `c84b4634…`, metal2vulkan `219f0136…`
  (`x9/BUILD.md:180-300`); local trees include `metal2vulkan-pan205-source` and
  `720ef1ce…`, i.e. documented pin and local tree differ (`LIKELY` drift risk).
- Source hashes: `x9/build/local-source-lock.json`; root manifest:
  `.local/x9/x9/out/pan220-batched-cache-ranges/root-manifest.json` (`x9/build/local.json`).

Per repository rule since 2026-10-28, compile/package only; no extra test suites were run.

## 9. Runtime Result

**Not run.** No device was flashed, booted or probed during this audit. `NOT TESTED`.

Historical context only (from `NOTES.md`, not re-verified):
- Revision 229 experiments concern a lost triangle replay from an owned buffer.
- Revision 228 user report: all variants split/flicker.

These are recorded as history, not as current evidence. `NOT TESTED` at this HEAD.

## 10. Performance

**Not measured.** `NOT TESTED`.

Qualitative, code-derived expectations only (`INFERENCE` unless noted):
- Submit latency includes plan build + cache maintenance + `flush_gpu` + poll and is
  serialized under `metal_service_lock` (`SOURCE CONFIRMED` for structure).
- Display tick cost is shielded from that lock by TryLock, but GPU-busy ticks are skipped
  (`SOURCE CONFIRMED` for mechanism; cost `NOT TESTED`).
- Cache work scales with resident mappings, not submitted work (`SOURCE CONFIRMED` for
  scope; actual byte counts `NOT TESTED`).
- The staged desktop path includes a full-frame CPU copy and a CPU `memcmp` verification
  (`SweetX9DesktopWorker.inc`), so it cannot be a low-latency presentation path by design.
- No benchmark or counter was collected.

## 11. Regression

No change, therefore no regression surface. `VERIFIED` (docs only).

Risks introduced by this phase: none at runtime. The only repository effect is five new
Markdown files under `docs/graphics/` plus a `NOTES.md` handoff edit.

## 12. Rollback

Documentation-only rollback:
- Delete `docs/graphics/` and revert the `NOTES.md` handoff commit.
- No build artifact, manifest or pinned source is affected.
- No device state exists to unwind.

## 13. Remaining Issues

Open questions and their status:

1. **Async completion**: no interrupt/event-driven completion path exists; only polling and
   a bounded history. Direction: separate Submit from completion observation.
   `SOURCE CONFIRMED`.
2. **Per-submission cache scope**: the plan covers all reachable mappings. Direction: pass
   the resource/range set with the submission. `SOURCE CONFIRMED`.
3. **Global lock span**: cache maintenance and the completion poll run inside
   `metal_service_lock`. Direction: narrow the lock to AS/register/doorbell programming.
   `SOURCE CONFIRMED`.
4. **Display/GPU handoff**: only CPU memory crosses; no GPU scanout. Hardware capability
   for direct scanout or a shared IOVA domain is `UNKNOWN`.
5. **PanVK feature surface**: single VM/group, immediate binds only, no fd/dma-buf, no
   external sync fd, synchronous submit, last-submission BO waits. `SOURCE CONFIRMED`.
6. **Reporting mismatches**: 1024 vs 4096 mappings; 512 MiB vs 1024 MiB budget.
   `SOURCE CONFIRMED`.
7. **Build drift**: documented vs local metal2vulkan pin; manual allowlist regeneration.
   `LIKELY`.
8. **Dead/reserved code**: `X9RenderResources.c` has no external caller. `SOURCE CONFIRMED`.
9. **Unmeasured performance**: no counters for cache bytes, skipped ticks, or submit
   duration were collected in this audit. `NOT TESTED`.

### Recommended minimal-change order (for approval before Task 2)

Each item is scoped to be independently revertible. None is implemented in Task 1.

| # | Change | Files | Risk | Verification |
|---|---|---|---|---|
| 1 | Add submit-scoped resource list to the native ABI (additive field/flag) and use it to scope `RenderCachePlan`; keep reachability fallback | `X9RenderABI.h`, `SweetX9RenderService.cpp`, `X9RenderCachePlan.inc`, `SweetX9RenderEngine.inc`, PanVK submit plan | Medium: wrong list → stale data; fallback must remain | Build + boot; compare `X9 RENDER DIAG` cache bytes/holes logs against baseline; run existing desktop path |
| 2 | Narrow `metal_service_lock` to AS install + doorbell + halt; move cache maintenance outside it under per-resource serialization | `SweetX9RenderEngine.inc`, `SweetX9CSF.cpp` | High: lock-order/deadlock; must preserve `fLock → owner` rule | Static lock-order review; boot; GUI heartbeat `busy`/`gpu_defer` counters compared to baseline |
| 3 | Make completion observable asynchronously (event source or fence object) while keeping synchronous mode default | `X9RenderABI.h`, `SweetX9RenderEngine.inc`, `X9PanVKMemory.c`, `X9PanVKSync.c` | High: changes submit semantics | Boot with synchronous flag unchanged; verify `render_wait` hits/misses; then enable async in a separate step |
| 4 | Fix caps/budget reporting mismatch (report effective values or drop the boot-arg contract difference) | `SweetX9RenderService.cpp`, `X9PanVKMemory.c` | Low | Build; userspace reads caps; no behavior change expected |
| 5 | Auto-generate the plugin allowlist from the build graph and fail on drift | `x9/build/build.py`, manifest generator | Low–Medium: build failure on stale artifacts | CI build for revision N+1; compare generated hash with plugin bundle |
| 6 | Unify build-time pins into one verified lock file | `x9/BUILD.md`, `x9/build/local-source-lock.json`, `build.py` | Low | Build from clean checkout; hash check passes |
| 7 | Remove or wire `X9RenderResources.c` | `x9/metal/General/X9RenderResources.*`, factory | Low | Build; no runtime reference remains |

Items 1–3 are the only ones that touch GPU submission/cache behavior and must not start
without explicit approval. Items 4–7 are hygiene and can be done independently.

---

## Appendix A — Answers to the twelve Task 1 questions

**Q1. Where is the real Metal Runtime ↔ metal2vulkan boundary?**
At the `nmb_*` C ABI implemented in `NativeMetalObjects.rs` / `NativeMetalExtras.rs` and
consumed by the Objective-C façade. `MTLCommandBuffer commit` →
`nmb_command_buffer_commit` (`NativeMetalObjects.rs:1728`) → metal2vulkan
`CommandBuffer::commit` (`command_buffer/mod.rs:2424`). Texture/buffer creation crosses the
same boundary through Metal object wrappers; there is no direct call from the Metal shim
into PanVK. `SOURCE CONFIRMED`.

**Q2. How does PanVK v14 connect to the XNU Mali backend?**
Not through ioctls. PanVK is built with X9 hooks and calls the backend in-process:
`x9_pan_kmod_create_native` (`X9PanVKSync.c:307`) at device creation
(`panvk_vX_device.c:480-600`), `x9_pan_kmod_csif` (`:359`) for CS register limits,
`x9_pan_kmod_import_host` (`:384`) for host-pointer memory, `x9_pan_kmod_submit` (`:318`)
for submissions, and `x9_pan_kmod_group_check` (`:364`) for status. These are direct C
calls exposed through `struct pan_kmod_ops` (`:256`). `SOURCE CONFIRMED`.

**Q3. Which files/functions manage CSF, GPU VM, BO, Fence, Cache?**
- CSF: `SweetX9CSF.cpp` (`install_as:349`, `remove_as:363`, `stop_mcu:377`, `flush_gpu:383`),
  `SweetX9CSFResident.inc` (session, slot pages), `X9CSFRenderQueue.cpp`
  (`configure/start/halt`, `notify`, `wait_ack`), `X9CSFQueueAccess.cpp` (health, doorbell,
  read/write, yield), `X9CSFInterfaces.cpp` (shared-region parse).
- GPU VM: `X9RenderPageTable.cpp` (3-level table, `publish`, `detach`) and
  `X9PanVKMemory.c` (`create_vm:187`, `bind_vm:205`, `destroy_vm`).
- BO: `SweetX9RenderService.cpp:194-287` (Allocate/Import/Release),
  `X9PanVKMemory.c` (`allocate`, `free_bo`, `map_cpu`, `unmap_cpu`, `import_host`).
- Fence: `SweetX9RenderEngine.inc` (SYNC_SET64 at `+0x8000`, poll, `render_wait`),
  `X9PanVKMemory.c` (`wait_bo:165`), `X9PanVKSync.c` (fence DAG/timeline).
- Cache: `X9RenderMemory.cpp` (only `dc civac/cvac/ivac` emitter),
  `X9RenderCachePlan.inc` (span planning), `X9GPUCache.h` (`x9_gpu_clean` for tables).
`SOURCE CONFIRMED`.

**Q4. How are IOSurface Direct Host / Host Pitched / Shadow / Owned Buffer, and Render
Target / Sampled Texture separate?**
Six routes decided in metal2vulkan (`in_place.rs:245`, `linear_plan.rs:653`) and mapped in
`call-graph.md` §2:
- A Direct Host: no-copy buffer → host-pointer import (`x9_pan_kmod_import_host:384`) →
  in-place linear image, class `Pointer`.
- B Host Pitched: same import with explicit GPU pitch from `iosurface_gpu_layout:462`;
  falls to shadow on `Refusal::Pitch`.
- C Shadow: any `ShadowCause` (policy, RT, shader write, pending/failed check, refusal) →
  device-allocated image + copy.
- D Owned Buffer: normal `VkBuffer` allocation → `pan_kmod_bo_alloc` → `X9RenderAllocate`.
- E Render Target: usage contains `RenderTarget` → unconditional shadow
  (`ShadowCause::RenderTarget`), plus usage-shadow/attachment-stand-in for undeclared RT.
- F Sampled Texture: sample-only usage eligible for in place when the class verdict
  passed; otherwise shadow. The `DmaBuf` class is unreachable under X9 because fd import
  is `ENOTSUP` (`X9PanVKMemory.c:90`).
`SOURCE CONFIRMED`.

**Q5. What is the full current `render_submit` path?**
`render_submit` (`SweetX9RenderEngine.inc:269`) → take `metal_service_lock` → readiness →
build `RenderCachePlan` → per-resource pre-maintenance (WC/coherent/never-exposed skips,
plan spans or whole BO) → build ≤3 CSF packets (markers, CALL, WAIT, SYNC_SET64 fence) →
`x9_render_memory_publish` → `render_range_sync` → `flush_gpu` → `install_as(AS1)` →
user doorbell (`ml_io_map(0x48090000)`) → poll health/page/fence/guards →
`x9_csf_group_halt(SUSPEND)` → `flush_gpu` → `remove_as` → `x9_render_memory_detached` →
post-CIVAC → completion reply. On failure: disable client, mark resident uncertain, retain
refs, no reset. `SOURCE CONFIRMED`.

**Q6. Which operations hold `metal_service_lock`?**
- `render_submit` for its entire duration (`SweetX9RenderEngine.inc:269`; owner
  `SweetX9CSF.cpp:657`).
- CSF bootstrap (`sweet_x9_render_bootstrap:673,680`) and any engine entry using
  `RenderOwner`.
- The desktop GPU worker around `sweet_x9_gpu_desktop`
  (`SweetX9DesktopWorker.inc:45-49`).
- Display does **not** hold it: it uses `sweet_x9_render_display_try_enter`
  (`SweetX9CSF.cpp:664`).
`SOURCE CONFIRMED`.

**Q7. Why did cache maintenance expand to multiple CPU-visible BO mapping ranges?**
Because the current implementation chooses correctness over precision: the plan is built
from every live mapping of the context, so any BO reachable by the client is maintained,
including imported aliases and indirect GPU accesses. The code comment
(`SweetX9RenderEngine.inc:320-338`) states this is deliberate: it does not rely on
"opt-in dirty hints" or an incomplete draw-resource list, and imports must be pre-faulted.
Maintenance is already span-exact per mapping with holes excluded and skips WC/coherent/
never-exposed resources, so the plan document's "whole-range CIVAC of every CPU-exposed
BO" premise is **outdated** for current code; the remaining imprecision is the
reachability scope, not the range. `SOURCE CONFIRMED`.

**Q8. Why is the display tick delayed by GPU submission?**
It is not blocked: the display tick takes the GPU owner only with TryLock and otherwise
skips the flush (`SweetFramebuffer.cpp:1045-1056`). What it observes is (a) the owner
busy because `render_submit` (or the desktop worker) holds `metal_service_lock` for the
whole synchronous submit, and (b) staged GPU frames not yet generation-matched. So the
mechanism is tick suppression/observation delay, not lock waiting. `SOURCE CONFIRMED`
for mechanism; actual delay `NOT TESTED`.

**Q9. How does the IOMobileFramebuffer CPU copy path work?**
`AppleCLCD::swap` only records the pending frame and surface holds. At vsync,
`vsyncFired` takes the record and calls `present()`. `present()` first tries
`directPresent()` (needs `fVig0` and `scanoutPhys`; copies via `flipFrom` when a flip
buffer exists, else `scanFrom(pa, stride)`), then for X9 composes layers into a shadow/flip
buffer and queues the desktop frame; publication copies the chosen source rows into
`fPixels` and verifies with `memcmp`. The LK submit (`x9_lk_gui_present` →
`x9_lk_gui_step`) then pushes the buffer on a fresh TE. `SOURCE CONFIRMED`.

**Q10. Can the display controller directly scan a GPU render target?**
Not in the current implementation. `directPresent` scans a `SweetSurface` with a
`scanoutPhys` (M4U/IOMMU physical) or copies it; GPU render targets live in the native GPU
VA range and there is no registration path that hands a GPU image to the display
controller. Whether the hardware could scan such a target is `UNKNOWN` and was not tested.
`SOURCE CONFIRMED` (current absence), `UNKNOWN` (capability).

**Q11. Do GPU and Display share one IOVA domain?**
No. GPU uses its own 16 GiB VA range and kernel-built page table
(`X9RenderPageTable.cpp`; `X9PanVKMemory.c:52`). Display uses physical/M4U addresses from
`SweetSurface::scanoutPhys`. No shared allocator or shared page table exists.
`SOURCE CONFIRMED`. Whether a shared domain is possible is `UNKNOWN`.

**Q12. Which modules already have independent interfaces, and which share internal state?**
Independent boundaries: Metal↔metal2vulkan (`nmb_*`), metal2vulkan↔PanVK (Vulkan),
PanVK↔X9 backend (`pan_kmod_ops`), transport↔kernel (`X9RenderABI`), display↔panel
(IOMFB + LK functions), build↔plugin (manifest).
Shared internal state: `metal_service_lock` (render/CSF/desktop), resident session and
render context (kernel), desktop frame statics, `SweetSurface` registry and framebuffer
`fLock`, and the `pan_kmod` connection/VM. `SOURCE CONFIRMED`.

## Appendix B — Document map

| Question / deliverable | Location |
|---|---|
| Per-module inventory | `architecture-inventory.md` |
| Submission, texture, display call graphs | `call-graph.md` |
| Issues by category and tag | `refactor-issues.md` |
| Seven ABI layers | `abi-map.md` |
| 12 questions | Appendix A above |
| Minimal change order | §13 above |
