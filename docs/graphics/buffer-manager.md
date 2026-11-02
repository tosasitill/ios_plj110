# Buffer Manager — Task 5

- Date: 2026-11-02
- Branch / HEAD: `feat/G1Ultra` / `5b3caad` (working tree carries Task 3–5)
- Task: Task 5 (P3). Establish one metadata/lifecycle description for IOSurface,
  host-linear, shadow and GPU-owned buffers. Unify lifecycle and metadata first;
  do not change allocation strategy.
- Tags: `SOURCE CONFIRMED`, `BUILD VERIFIED`, `NOT TESTED`, `INFERENCE`,
  `NOT IMPLEMENTED`.

This document describes the metadata layer only. It allocates nothing, maps
nothing, maintains no cache and changes no rendering decision. `NOT TESTED` on a
device; only the userspace bundle build is `BUILD VERIFIED`.

---

## 1. Scope and placement

The Buffer Manager lives in the Metal runtime, because that is where buffers are
imported, wrapped and referenced by command buffers:

```
IOSurface / host pointer / Metal allocation
                  │
                  ▼
        X9BufferManager (metadata + lifecycle)
          │         │          │
      CPU view   GPU view   Display view (API only)
          │         │          │
          └─────────┴──────────┘
                  ▼
     kernel render backend (allocation, mapping, cache, fences)
```

Spec §5.5 ownership is preserved: the original surface is owned by its creator,
the Buffer Manager owns only post-import metadata/references, the GPU backend
owns use-while-rendering, and the Display backend will own scanout references in
Task 7. The manager is a registry, not an allocator.

Per the task instruction, allocation strategy is unchanged: every path still
calls the same `nmb_*` bridge and native ABI functions as before.

## 2. Data model

`x9/metal/General/X9BufferManager.h`:

| Field | Meaning | Source in this implementation |
|---|---|---|
| `kind` | IOSurface, host linear, shadow, GPU-owned, display | known at creation; display/shadow have no caller yet |
| `ownership` | client, imported, driver, display | IOSurface/host import → imported; allocations → driver |
| `usage` | sampled, render target, storage, vertex, index, indirect, display | Metal texture usage bits where present, else 0 |
| `format_domain`, `format` | Metal pixel format or IOSurface fourcc | kept distinct rather than conflated |
| `width`, `height`, `bytes` | logical extent and allocation size | from the Metal descriptor or IOSurface |
| `surface_id` | IOSurface identity when known | `IOSurfaceGetID` |
| `planes[]` | offset, stride, size per plane | buffer offset/row bytes; two planes for planar surfaces |
| `parent` | key of the buffer a view sits on | 0 for a root buffer |

This is the spec §5.2 model mapped onto facts this layer actually has; unknown
fields stay zero rather than guessed.

Lifecycle (`X9BufferLifecycle`) keeps GPU and display use separate, as §5.4
requires:

| Field | Meaning |
|---|---|
| `gpu_state` | idle → in use → complete |
| `display_state` | idle → in use → released |
| `gpu_refs`, `display_refs` | consumers currently holding the metadata |
| `gpu_uses`, `display_uses` | completed intervals |
| `retired` | unregistered while a consumer still held a reference |
| `created_ns` | monotonic creation time |

A GPU fence/unregister never implies the display stopped scanning, and vice
versa: the two counters and states never collapse into one flag.

## 3. Registry and API

Entries are keyed by the native handle pointer (`NmbBuffer*`/`NmbTexture*`),
which is unique while the object exists. A duplicate key is refused, not merged,
so handle reuse after release cannot silently inherit stale metadata.

| Function | Contract |
|---|---|
| `x9_buffer_register` | Add a root or view; refuses key 0, unknown kind, >3 planes, duplicate, OOM |
| `x9_buffer_unregister` | Remove now, or mark retired if refs are held |
| `x9_buffer_gpu_acquire` / `_release` | Track a GPU-use interval; release marks `complete` at zero |
| `x9_buffer_display_acquire` / `_release` | Track a scanout interval; release marks `released` at zero |
| `x9_buffer_describe` | Copy descriptor + lifecycle to the caller |
| `x9_buffer_live_count` | Number of non-retired entries |

All calls are advisory. The implementation (`X9BufferManager.c:42-142`) returns
`false` on a miss and never asserts, blocks on rendering, or touches resource
lifetime. A metadata failure cannot refuse a Metal resource.

Thread safety is one process-wide `pthread_mutex`; the registry is a dynamically
grown array (`grow`, `:33`) with swap-removal (`remove_at`, `:29`). Entries are
small and freed at unregister; there is no per-frame allocation.

## 4. Integration points

`x9/metal/General/NativeBridgeMetalFrontend.mm`:

| Point | Line | Action |
|---|---|---|
| helpers | 181-217 | key, Metal-usage mapping, root/view registration, parent lookup |
| `NBBuffer dealloc` | 226 | unregister before `nmb_buffer_release` |
| `surfaceBuffer` | 267 | IOSurface import root, ownership imported, surface ID |
| `NBTexture dealloc` | 288 | unregister before `nmb_texture_release` |
| `view:` | 323 | texture view inherits the parent descriptor, `parent` set |
| `textureOnBuffer` | 359 | view over a buffer: offset/stride/height, inherited kind |
| command-buffer property | 676 | `bufferRefsHeld` marks an open GPU interval |
| `finish` | 1172-1177 | release every resource once, on completion or refusal |
| `commit` | 1211-1213 | acquire every resource before native submission |
| device buffers | 1388, 1571, 1579 | GPU-owned root |
| host no-copy buffer | 1588 | host-linear root, ownership imported |
| device/heap textures | 1401, 1596 | GPU-owned view, usage/format/extent |
| planar IOSurface texture | 1622-1634 | two-plane IOSurface view |

The acquire happens only when encoding succeeded; it runs once per unique
resource in `resourceOwners` (an `NSMutableSet`). `finish` is the single terminal
path (confirmed completion, local refusal, or failed submission), and clears the
flag before releasing so a second call cannot double-release. The manager itself
never retains a Metal object, so deallocation timing is unchanged; the existing
strong `resourceOwners` set and the runtime's in-flight retention remain the
authorities.

## 5. What is not registered (and why)

1. **Shadow images.** `metal2vulkan-runtime` creates shadow images internally
   (`in_place.rs`, `shadow_copy_in.rs`) and exposes only aggregate counters
   (`shadowed_textures`), not a per-texture placement query. The descriptor kind
   exists, but no registration call can name the fact without guessing.
   `NOT IMPLEMENTED`; needs a runtime placement query, which is a separate change.
2. **Internal runtime buffers/textures.** Resources metal2vulkan allocates for
   its own machinery never pass through the Metal API wrapper and are not in
   `resourceOwners`. They remain governed by the runtime.
3. **Display buffers.** The display path is kernel-side CPU composition today and
   does not expose per-buffer scanout state to this process. `display_*` API and
   state exist for Task 7; no caller until the display backend reports
   acquisition/release.
4. **Cross-process views.** The registry is per-process. IOSurface identity is
   recorded (`surface_id`) but not used as a key, so two processes never share
   metadata; coordination remains the kernel backend's.

These are explicit gaps, not silent assumptions.

## 6. What is unchanged

- **Allocation**: same `nmb_buffer_new`, no-copy import, heap and texture calls.
- **Mapping/IOVA**: nothing added; the kernel page tables remain authoritative.
- **Cache maintenance**: untouched; exact-range requests and the kernel span plan
  are the same as Task 4.
- **Fences/submission**: untouched; the GPU-ref interval is bookkeeping only and
  does not signal, wait or order anything.
- **Native ABI**: no `X9RenderABI.h` change, no new selector, no manifest change.
- **Display**: untouched; no scanout path is claimed.

## 7. Verification

`BUILD VERIFIED` for the userspace bundle only:

```
python3 x9/build/scripts/build-general-metal.py --revision 230
  native boundary clean (metal): 27 sources
  Generic Metal runtime and iOS16 bundle compiled/linked/signed; ...
```

- `X9BufferManager.c` compiles with `-std=c11 -Wall -Wextra -Werror`.
- `nm` on `X9GeneralMetal230.bundle/X9GeneralMetal` shows the eight
  `_x9_buffer_*` functions defined; the frontend object's only new undefined
  symbols are those five used functions.
- No new IOKit, display or XNU-VM reference; the boundary check passes with the
  two new sources included.
- The two `X9FrameCapture.inc:147,148` warnings are pre-existing.

`NOT TESTED`: no install, boot or device run. The lifecycle has not been observed
against real GPU completion or display scanout.

## 8. Limits and next steps

1. The shadow and display kinds are defined but unused (§5.1, §5.3).
2. `x9_buffer_live_count`/`describe` have no consumer yet; they exist so a later
   instrumentation or display-task caller can query without new plumbing.
3. The registry does not participate in memory reclamation; the rule "only
   recycle after every consumer releases" is still enforced by the existing
   object graph (strong `resourceOwners` + runtime in-flight retention) and, once
   display exists, must add a display reference.
4. Per-frame cost is one mutex acquisition per resource at commit and at
   completion; it is not measured (`INFERENCE`), and it is deliberately not on
   the per-draw path.
5. A per-BO fence and an exact submission resource set remain Task 6 work.
