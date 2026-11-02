# Phase 3 Report — Buffer Manager (metadata and lifecycle)

- Date: 2026-11-02
- Branch / HEAD: `feat/G1Ultra` / `5b3caad` (working tree carries Task 3–5)
- Task: Task 5 (P3). Unify IOSurface, host-linear, shadow and GPU-owned buffer
  description; unify lifecycle and metadata first; do not change allocation.
- Tags: `SOURCE CONFIRMED`, `BUILD VERIFIED`, `NOT TESTED`, `INFERENCE`,
  `NOT IMPLEMENTED`.

Only the userspace bundle build is `BUILD VERIFIED`. Nothing is
`DEVICE VERIFIED`; no image was built, flashed or booted.

---

## 1. Scope

**In scope**
- A single internal descriptor for the buffer kinds the Metal runtime handles.
- A lifecycle record that keeps GPU use and display use separate.
- Registration at creation, GPU reference intervals at commit/completion,
  removal at deallocation.
- Build wiring and symbol verification.

**Out of scope (per the task)**
- Changing allocation, import or mapping strategy.
- Changing cache maintenance, fences, submission, CSF or display behaviour.
- Replacing the existing IOSurface import cache, Metal object graph or kernel
  resource registry.
- Any ABI or manifest change.
- Deployment or device testing.

**Method**
Source reading of the Metal frontend and bridge, one full bundle build, a symbol
audit, and the Task 4 boundary check. No runtime measurement.

## 2. Existing Implementation

Before this change there was no single description of a buffer:

- `NBBuffer` carried only `handle`, `options`, `wrapped`, heap origin.
- `NBTexture` carried a descriptor, optional `IOSurfaceRef`, buffer/offset/row.
- `surfaceBuffer` kept a weak import cache keyed by surface ID, base address and
  size; lifetime was held by `CFRetain(surface)` and the runtime's import.
- GPU lifetime was held by `resourceOwners` (strong) from encode to
  `finish`, and by the runtime's in-flight retention.
- The kernel owned `X9RenderResource` (bytes, flags, imported, prepared,
  exposed, write-combined); PanVK owned `x9_bo`.
- metal2vulkan chose direct/pitched/shadow internally and exposed only aggregate
  counters.

So the facts existed in three layers but were not described the same way, and
there was no place that could answer "which buffer is this, which kind, and is
the GPU or the display still using it". `SOURCE CONFIRMED`.

## 3. Changes

Four files; two new, two edited. No runtime allocation, cache, fence, CSF,
display or ABI source was changed.

| File | Change |
|---|---|
| `x9/metal/General/X9BufferManager.h` (new) | Descriptor, planes, kind/ownership/usage/format domains, lifecycle, registry API. |
| `x9/metal/General/X9BufferManager.c` (new) | Mutex-protected dynamic registry; register/retire/acquire/release/describe. Advisory only. |
| `x9/metal/General/NativeBridgeMetalFrontend.mm` (edited) | Register on every buffer/texture creation path; unregister on dealloc; acquire/release GPU intervals at commit/finish. |
| `x9/build/scripts/build-general-metal.py` (edited) | Compile `X9BufferManager.c` with `-Wall -Wextra -Werror` and link it into the bundle. |

The descriptor maps the spec §5.2 model onto facts the layer actually has; a
field it cannot know stays zero instead of being guessed.

## 4. Architecture

```
Metal API buffer / texture
        │  creation (kind, usage, planes, surface ID)
        ▼
  X9BufferManager  ── register ──►  entry {desc, gpu/display refs}
        ▲                                 │
        │  commit: gpu_acquire            │ describe / live_count
        │  finish: gpu_release            │
        └──────────── command buffer ─────┘
        │
        ▼
  nmb_* bridge → PanVK/native ABI → kernel backend
     (allocation, mapping, cache, fences — unchanged)
```

The manager sits beside, not inside, the resource path: no call in the manager
can fail a Metal operation, and no allocation, mapping or cache decision reads
it.

## 5. ABI Impact

**None.** No `X9RenderABI.h`, selector, `pan_kmod_ops`, manifest or allowlist
change. The descriptor and registry are process-local C structures, not an ABI.
`SOURCE CONFIRMED` by diff.

## 6. Memory Impact

- The registry allocates one small entry per tracked buffer and frees it at
  unregister (or at the last reference for a retired entry). It holds no pixel
  memory and pins nothing.
- Buffer/texture allocation, size, alignment and ownership are unchanged.
- Deallocation timing is unchanged: the manager does not retain Metal objects;
  the existing strong `resourceOwners` set and the runtime's in-flight retention
  still govern reclamation.
- Retirement (`retired=1`) removes metadata only after consumers release; it does
  not free the underlying buffer, because this layer never owned it.
- `INFERENCE`: for a frame with hundreds of resources the registry is tens of
  KiB at most; it was not measured.

## 7. Synchronization Impact

- One process-wide `pthread_mutex` protects the registry. It is taken at
  creation, commit (once per unique resource), completion/refusal and dealloc,
  not per draw.
- No fence, wait, timeout, lock order or submission ordering changed. The GPU
  reference interval is bookkeeping; it neither signals nor blocks.
- `finish` clears `bufferRefsHeld` before releasing so a second terminal call
  cannot double-release. A failed submission is treated as terminal for
  metadata, matching the frontend's existing `finish` path; the runtime may still
  retain memory, which the manager does not own.
- GPU and display states are separate fields and counters. A GPU complete event
  does not mark display released, and vice versa.

## 8. Build Result

`BUILD VERIFIED` for the userspace bundle only:

```
python3 x9/build/scripts/build-general-metal.py --revision 230
  native boundary clean (metal): 27 sources
  Generic Metal runtime and iOS16 bundle compiled/linked/signed; ...
```

- `X9BufferManager.c` compiled with `-std=c11 -O2 -Wall -Wextra -Werror`.
- `nm` on `X9GeneralMetal230.bundle/X9GeneralMetal`: `_x9_buffer_register`,
  `_x9_buffer_unregister`, `_x9_buffer_gpu_acquire`, `_x9_buffer_gpu_release`,
  `_x9_buffer_display_acquire`, `_x9_buffer_display_release`,
  `_x9_buffer_describe`, `_x9_buffer_live_count` are defined; the frontend
  object references the five it uses.
- No new IOKit/display/XNU-VM dependency; the Task 4 boundary check passes with
  the two new sources (27 vs 25).
- Only the two pre-existing `X9FrameCapture.inc:147,148` warnings.
- **Not built:** kernel, boot image. **Not run:** nothing installed or booted.

## 9. Runtime Result

`NOT TESTED`. No device run, flash or reboot. Registration, GPU intervals and
tombstone retirement have not been observed against real completion or display
scanout.

## 10. Performance

No measured effect. The additions are one mutex acquisition per resource at
commit and at `finish`, plus one per creation/deallocation; nothing per draw or
per pixel. `INFERENCE`; not benchmarked. Task 6 remains the place for measured
submission work.

## 11. Regression

| Area | Before | After |
|---|---|---|
| Buffer/texture creation | `nmb_*` calls | same calls, plus advisory registration |
| Allocation, size, alignment | kernel/runtime | unchanged |
| Mapping/IOVA | kernel page tables | unchanged |
| Cache maintenance | exact ranges + kernel plan | unchanged |
| Fences/submission | synchronous validated completion | unchanged |
| Deallocation | `nmb_*_release` after last owner | same order, unregister before release |
| Resource lifetime | strong `resourceOwners` + runtime | unchanged |
| Native ABI | v1 | unchanged |

Acceptance conditions from spec §5:

| Condition | Status |
|---|---|
| One logical description covers IOSurface / host linear / shadow / GPU-owned | Holds for three kinds in code; shadow kind defined but no caller (§13). `SOURCE CONFIRMED` |
| Plane offset/stride/size recorded | Holds for buffer views and two-plane surfaces; `SOURCE CONFIRMED` |
| Format and modifier kept as real values | Format domain/value recorded; modifier not available in the Metal descriptor path and stays 0. `NOT IMPLEMENTED` |
| GPU and display lifetime separate | Holds in the record; display has no producer yet. `SOURCE CONFIRMED` + `NOT IMPLEMENTED` |
| Ownership rules preserved | Holds: surface owner, manager metadata, GPU backend use, display deferred. `SOURCE CONFIRMED` |
| Allocation strategy not changed | Holds. `SOURCE CONFIRMED` |
| No regression in existing call chain | Compile/link verified; runtime `NOT TESTED` |

## 12. Rollback

Revert the four Task 5 files and rebuild:

```
git checkout -- x9/metal/General/NativeBridgeMetalFrontend.mm x9/build/scripts/build-general-metal.py
rm x9/metal/General/X9BufferManager.h x9/metal/General/X9BufferManager.c
python3 x9/build/scripts/build-general-metal.py --revision <rev>
```

Compile-only rollback; no kernel, PanVK, ABI, manifest or device state involved.

## 13. Remaining Issues

1. **Control Center platter triangles remain unresolved** (missing, split,
   flickering). No rendering path was touched; this phase neither explains nor
   claims to fix them.
2. **Shadow buffers are not registered.** metal2vulkan exposes only aggregate
   shadow counters, not per-texture placement. Registering them needs a runtime
   query; `NOT IMPLEMENTED`.
3. **Display buffers are not registered.** The display path is kernel-side CPU
   composition with no per-buffer scanout event to this process; the API and
   state exist for Task 7. `NOT IMPLEMENTED`.
4. **Modifiers are zero.** The Metal descriptor path exposes pixel format but not
   the real layout modifier; recording a guess would be wrong.
5. **Metadata is advisory and not consumed by allocation.** That is deliberate
   for Task 5 ("do not rush to change allocation"); a later phase must decide
   which decisions read it.
6. **No device observation of lifecycle.** Golden-path and failure-path intervals
   are unverified on hardware.
7. **Task 2 design remains unimplemented**, and Task 6's per-BO fence/resource
   set work is still pending.
8. **Deployment remains pending**: PanVK 230 bundle, packaging, allowlist,
   kernel build, then a user-flashed candidate.
9. **Patch 00 remains path-locked** (Task 4 finding), so a fresh tree is not
   reproducible from the tracked patch.
