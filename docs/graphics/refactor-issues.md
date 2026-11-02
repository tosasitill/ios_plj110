# Refactor Issues

- Audit date: 2026-11-02, HEAD `5b3caad`
- Tags: `CONFIRMED` = visible in current source; `LIKELY` = code strongly implies but not
  measured; `UNKNOWN` = requires hardware/driver measurement; `NOT A PROBLEM` = coupling
  exists but is intentional or harmless.
- Each item names evidence and the smallest decoupling direction. No plan is applied here.

---

## API Coupling

### API-1 — The native ABI is synchronous submit-and-wait `CONFIRMED`
`render_submit()` polls the fence page until completion and returns a completion record
(`SweetX9RenderEngine.inc:269-591`). `x9_pan_kmod_submit()` rejects any reply whose
`completed_id != submission_id` and poisons the device (`X9PanVKMemory.c:318-340`).
Consequence: `vkQueueSubmit` has GPU latency; the Metal runtime cannot overlap CPU work
with GPU work through this ABI.
Evidence: `SweetX9RenderService.cpp:351`; `X9PanVKMemory.c:318`; `X9PanVKSync.c:256`.
Direction: separate `Submit` from `Wait`/completion observation (event source or timeline),
keep the synchronous mode as an option.

### API-2 — `Wait` is a history lookup, not a wait `CONFIRMED`
`render_wait()` searches a bounded history and returns `kIOReturnNotFound` otherwise
(`SweetX9RenderEngine.inc:593-597`). PanVK calls it through `x9_pan_kmod_group_check`
(`X9PanVKMemory.c:364`), so a miss becomes a driver-visible failure rather than a block.
Direction: make Wait take a completion object or fence value with a deadline.

### API-3 — 12-selector ABI has no resource-set input `CONFIRMED`
`X9RenderSubmission` carries streams only; cache maintenance must infer resources from the
context's mapping table (`X9RenderABI.h`; `X9RenderCachePlan.inc`).
Direction: add an optional resource/range list to the submit packet (additive, versioned).

### API-4 — One client context per process, one VM `CONFIRMED`
`create_vm()` refuses a second VM (`EBUSY`) and `x9_pan_kmod_create()` opens exactly one
connection (`X9PanVKMemory.c:187-203,261`).
Direction: allow N VMs per device with per-VM connection state.

### API-5 — Objective-C surface import bypasses native ABI `CONFIRMED`, intended
`surfaceBuffer()` imports IOSurfaces through host pointers into Vulkan, never through the
native ImportHost selector directly (it is reached via `VK_EXT_external_memory_host`).
This is `NOT A PROBLEM` for layering, but it means the native ABI never sees the
IOSurface identity and cannot apply surface-specific policy.

---

## Lifecycle Coupling

### LIFE-1 — VM lifetime tied to groups and connection `CONFIRMED`
`destroy_vm()` sets `d->failed = true` if any group remains and unmaps slots before
returning (`X9PanVKMemory.c:196-203`). Groups must die first; there is no detached group.
Direction: reference-count groups against VM, destroy in any order.

### LIFE-2 — Partial multi-op bind poisons the whole VM `CONFIRMED`
If any op in a `bind_vm()` batch fails, the loop marks `d->failed = true` and returns,
without rolling back earlier ops (`X9PanVKMemory.c:205-254`).
This is deliberate ("fail the VM instead of retrying"), but it makes a single malformed
op a device-wide outage.
Direction: validate every op before executing any (two-pass), so failure is side-effect free.

### LIFE-3 — Submit failure disables the client and retains hardware refs `CONFIRMED`
`render_submit` uncertainty path sets `c->disabled` and `resident->uncertain`, keeps
AS/MCU/DMA/power refs (`SweetX9RenderEngine.inc:589`). No reset or retry exists.
Direction: add an explicit quiesce/teardown path that can release refs after a fault, and
make the display path independent of that teardown.

### LIFE-4 — Desktop frame pool is a boot-scoped singleton `CONFIRMED`
`desktop_frame_pool`, `desktop_frame_state`, `desktop_frame_no_more` are file statics;
after 3 published frames or 8 attempts the path disables for the boot
(`SweetX9DesktopWorker.inc`).
Direction: pool lifetime should follow the display service, not a boot counter.

### LIFE-5 — Mapped resources must be unregistered before release `LIKELY`
`Release` frees the resource slot; correctness depends on prior `Unmap`
(`SweetX9RenderService.cpp:277-287`; `X9PanVKMemory.c:342`). A client that releases early
leaves stale mappings.
Direction: either reject release with live mappings explicitly (if not already) or unmaps
automatically.

---

## Memory Coupling

### MEM-1 — Cache maintenance scope is the whole reachable mapping set `CONFIRMED`
`render_cache_plan_build()` spans every mapping in `context->mappings`, not the submission's
resources (`X9RenderCachePlan.inc`); pre-submit loop iterates all `context->resources`
(`SweetX9RenderEngine.inc:339`).
Cost grows with resident resources, not with work submitted.
Direction: pass the resource set in the submit packet; keep the reachability path as fallback.

### MEM-2 — Non-exposed BOs are cleaned whole for first use `CONFIRMED`
`else { bytes = r.bytes; ... }` then `x9_render_memory_cache(..., r.bytes)`
(`SweetX9RenderEngine.inc:355-359`).
Direction: clean only the mapped/used ranges once the range list is available.

### MEM-3 — Imported aliases need a kernel mapping and pre-fault each submit `CONFIRMED`
`X9RenderMemory.cpp:87-105` creates the mapping lazily and touches every page before
maintenance. The pre-fault runs on each cache call, not once per import.
Direction: pre-fault at import time and keep the mapping alive with the resource.

### MEM-4 — Allocation budget and reported caps disagree `CONFIRMED`
`client_budget()` defaults to 1024 MiB with a 512 MiB floor
(`SweetX9RenderService.cpp:129`), while `X9_RENDER_CLIENT_LIMIT` (reported in caps) is
512 MiB. `X9PanVKMemory.c:10` notes 4096 kernel mappings but 1024 reported.
Direction: report the effective values or remove the boot-arg override from the contract.

### MEM-5 — GPU and display do not share an IOVA domain `CONFIRMED`
The native page table lives in `X9RenderPageTable.cpp` over GPU VA
`0x40000000..0x400000000`; display uses `SweetSurface::scanoutPhys` from the M4U/IOMMU
path. No shared allocator exists.
Direction: only after hardware capability is confirmed (see `phase-0-report.md` Q11).

### MEM-6 — IOSurface identity is lost after import `CONFIRMED`
`surfaceBuffer()` caches weakly by surface, but once imported the GPU sees only a host
pointer range. Aliasing/overlap checks are range-based (`overlap_held`, `linear_plan.rs:493`).
This is `NOT A PROBLEM` for correctness but limits surface-aware policy.

### MEM-7 — No attachment provenance or damage region exists `CONFIRMED`
No layer tracks which submission last wrote a subresource, and no layer consumes a
damage/update region: the Metal WSI takes `VkPresentRegionKHR` and ignores it
(`wsi_common_metal.c:688-723`), the runtime's render area is always the full attachment
(`render.rs:5025-5029`), and PanVK's incremental rendering is tiler-OOM recovery only.
Consequence: the stack can neither prove nor skip a partial redraw, and a first frame's
inherited attachment contents cannot be attributed to a producer.
Direction: emit per-draw scissor and presented-image footprint before changing policy.
Evidence: `docs/graphics/damage-tracking-audit.md`.

---

## Synchronization Coupling

### SYNC-1 — `metal_service_lock` spans cache maintenance + submit + poll `CONFIRMED`
`RenderOwner` is taken at the top of `render_submit` and released at return
(`SweetX9RenderEngine.inc:269`), covering plan build, CIVAC loops, `flush_gpu`, `install_as`,
doorbell, and the poll loop. Display uses TryLock and defers (`SweetFramebuffer.cpp:1045`).
Consequence: a long submit directly increases display tick skips.
Direction: hold the global lock only for register/AS/doorbell programming; do cache
maintenance outside it under per-resource locks.

### SYNC-2 — Display lock order is `fLock → GPU owner`, TryLock only `CONFIRMED`
Comment at `SweetFramebuffer.cpp:1041-1046`; `sweet_x9_render_display_try_enter()`
(`SweetX9CSF.cpp:664`) refuses when uncertain or busy.
This is `NOT A PROBLEM` (it prevents deadlock) but it converts GPU latency into dropped
display ticks rather than back-pressure.

### SYNC-3 — Completion is polled, no interrupt path `CONFIRMED`
`render_submit` polls `x9_csf_group_health`, page sync and fence (`SweetX9RenderEngine.inc:400-455`);
`flush_gpu` waits on an IRQ bit only for the GPU-control flush (`SweetX9CSF.cpp:383`).
Direction: add a completion interrupt/event source; keep polling as fallback.

### SYNC-4 — Per-client context lock is held across the whole selector `CONFIRMED`
`externalMethod()` holds the context lock for any operation, including synchronous Submit
(`SweetX9RenderService.cpp:435`). Two threads cannot submit concurrently even when the
hardware could accept it.
Direction: per-group or per-submission locking once the ABI carries more than one group.

### SYNC-5 — One group at a time in practice `LIKELY`
`render_submit` uses a single `resident->caps` register window and the per-group shared
state; no code was found that runs two groups concurrently.
Direction: verify against hardware; only then introduce concurrent groups.

---

## Cache Coupling

### CACHE-1 — Pre- and post-CIVAC are inside the submit critical section `CONFIRMED`
Pre: `SweetX9RenderEngine.inc:339-362`; post: after `x9_render_memory_detached` (`:458+`).
Both run under `metal_service_lock`, so cache cost is on the display-visible critical path.
Direction: move maintenance to the resource layer, outside the global GPU lock, with a
handoff barrier before `install_as`.

### CACHE-2 — WC resources disable maintenance entirely `CONFIRMED`
`if(r.write_combined){...continue;}` (`:340`) and `X9RenderMemory.cpp:74` returns success.
If a WC BO is ever read by the CPU through a cached alias, correctness relies on the client
not doing that. `NOT A PROBLEM` under the documented contract; a latent hazard otherwise.

### CACHE-3 — Coherent path is disabled because GPU snooping is not programmed `CONFIRMED`
`gpu_side_coherency_programmed=false` in `SweetX9CSFResident.inc`; optimistic coherent
skips are only taken when `coherent && r.cpu_exposed` (`:341`), which cannot be true while
the flag is false.
Direction: enable only after the SMMU/MMU snoop configuration is verified on hardware.

### CACHE-4 — 256-byte atom enforced, but holes are only reported `CONFIRMED`
`render_cache_plan_build()` rejects unaligned `offset|bytes`, and computes `holes`/`overlap`
for logs. Holes are not used to skip work; spans are exact so holes are inherently skipped,
but the counters are the only visibility.
Direction: expose counters through the ABI for tuning.

### CACHE-5 — Page-table clean is a separate physical path `NOT A PROBLEM`
`x9_render_vm_publish()` cleans dirty table pages with `x9_gpu_clean(..., physical=true)`
(`X9RenderPageTable.cpp:117`), independent of the BO cache plan. This is correct and
small (≤18 MiB of tables).

---

## Display Coupling

### DISP-1 — Display refresh can be skipped by a busy GPU owner `CONFIRMED`
`vsyncFired` calls `sweet_x9_render_display_try_enter()`; failure increments
`fCadenceGpuBusy` and suppresses the flush (`SweetFramebuffer.cpp:1045-1056`). The opposite
order is never used.
Direction: publish a completed frame from the GPU worker without needing the owner lock in
the display tick.

### DISP-2 — Panel content is CPU-composed even when GPU result exists `CONFIRMED`
`present()` composes layers into a shadow buffer; `sweet_x9_desktop_frame_poll` copies the
GPU result into the panel and verifies with `memcmp`. There is no direct GPU scanout.
Direction: a real scanout path requires the display controller to consume the render target
(see Q10; hardware capability `UNKNOWN`).

### DISP-3 — `directPresent` is a copy path in the current configuration `CONFIRMED`
It requires `fVig0` and `scanoutPhys` and, when `fFlip[0]` exists, copies with `flipFrom`
(`SweetFramebuffer.cpp:1500-1522`) to avoid tearing. So "direct" means "no composition",
not "GPU target scanned out".
Direction: rename/split the direct-scanout concept from the no-composition copy.

### DISP-4 — LK GUI submit is timer-driven and best-effort `CONFIRMED`
`x9QueuePresent()` schedules `x9PresentFired` at 100 µs, bounded 50 ms, and submits only on
a fresh TE (`SweetFramebuffer.cpp:917-948`; `pe_serial.c:734`; `x9_lk_console.h:149`).
A missed TE drops the frame instead of retrying with stale TE.
This is `NOT A PROBLEM` (intentional panel protection).

### DISP-5 — Desktop frame pool couples display stride to a fixed geometry `CONFIRMED`
Pool is hard-coded to 1256×2760 with source stride 5120 and destination 5184
(`SweetX9DesktopWorker.inc`; `sweet_x9_desktop_frame_begin` validates `stride==5120`).
Direction: derive geometry from the framebuffer instance.

### DISP-6 — No layer guarantees initial content of the panel-sized target `CONFIRMED`
Captured panel-sized passes are all `Load`+`Store`; the `attachment_clear` pipeline
bound inside them has write mask 0 on colour 0 (`X9 GRAPH BLEND`, rev 211/212), so it
does not zero that attachment. The kernel composites the whole surface and never clears
it. A first frame therefore inherits whatever the target held, and only pixels the frame
covers are replaced.
This is a client-contract question, not by itself a driver defect, but no design document
assigns the responsibility. Direction: decide between a WSI acquire clear and a
client-side full-coverage guarantee; the acquire-clear A/B is experiment 2 in
`docs/graphics/first-frame-experiment-plan.md`.

---

## ABI Coupling

### ABI-1 — Native ABI v1 is a single flat version `CONFIRMED`
`X9_RENDER_ABI` is one integer checked in every struct (`X9RenderABI.h`;
`X9PanVKMemory.c`). No feature negotiation beyond caps.
Direction: keep v1, add optional capability bits rather than bumping every struct.

### ABI-2 — Kernel and userspace agree on struct layout only `CONFIRMED`
`X9RenderStream`, `X9RenderMapping`, `X9RenderCompletion` are shared by copy; no size
fields. A mismatch is detected only by the per-call `abi` word where present
(`X9RenderABI.h`; `SweetX9RenderService.cpp:197`).
Direction: add struct size/version to each request.

### ABI-3 — Build-time plugin allowlist is an implicit ABI `CONFIRMED`
The kernel imports `x9_render_external_manifest.h` with the plugin's SHA-256, size and path
(`x9/kernel/generated/`). A plugin rebuild without regenerating the manifest is rejected at
load.
Direction: generate the manifest from the build graph; fail the build on drift.

### ABI-4 — PanVK's kmod ops table is the real internal ABI `CONFIRMED`
`struct pan_kmod_ops` in `X9PanVKMemory.c:256` is the contract between Mesa and X9; adding
a feature means changing both. This is `NOT A PROBLEM` (upstream-shaped) but is the main
reason features cannot be added kernel-only.

---

## Build Coupling

### BUILD-1 — Three source trees must stay in lockstep `CONFIRMED`
Mesa (`mesa-render-source`), metal2vulkan (`metal2vulkan-pan205-source`), and kernel
sources are separate trees with pins in `x9/BUILD.md:180-300` and hashes in
`x9/build/local-source-lock.json`. `BUILD.md` lists metal2vulkan `219f0136…` while the
local tree is `720ef1ce…` plus `metal2vulkan-pan205-source`.
Direction: one lock file that the build verifies before compiling.

### BUILD-2 — Patch stack order is load-bearing `LIKELY`
`00-mesa-panvk-x9.patch`, `01-metal2vulkan-x9.patch`, then numbered patches 0023/0025 and
pan194–pan209 plus 02–10. There is no automatic conflict or ordering check described in
`x9/BUILD.md`.
Direction: record patch series hashes and apply with `git apply --3way` plus a post-apply
tree hash check.

### BUILD-3 — Root manifest path is machine-specific `CONFIRMED`
`x9/build/local.json` points at `.local/x9/x9/out/pan220-batched-cache-ranges/root-manifest.json`,
so the built artifact set depends on which output directory was last populated.
Direction: derive the output directory from the revision.

### BUILD-4 — Frozen XNU object tree `NOT A PROBLEM`
Revisions 227–229 only regenerate the kernel allowlist, so the XNU object tree is stable
and rebuilds are cheap. This is a deliberate workflow, not a coupling defect.
